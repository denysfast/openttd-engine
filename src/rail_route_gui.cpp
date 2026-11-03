/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/**
 * @file rail_route_gui.cpp GUI of the rail trunk line tool.
 *
 * The trunk line window holds the options of the route (number of tracks, signals, what may be cleared) and the
 * placement tool: the first click on the map picks the start tile, the second one the end tile, which plans the
 * route and starts building it. While the end tile is being chosen, the route to the tile under the cursor is
 * planned again whenever that tile changes, but not more often than every #PREVIEW_MIN_INTERVAL (longer for routes
 * that take long to plan), and the viewport draws its track pieces from #GetRailRoutePreviewTile.
 *
 * The window changes the game only through #StartConstructionJob, which sends network commands.
 */

#include "stdafx.h"
#include "blueprint.h"
#include "company_base.h"
#include "company_func.h"
#include "error.h"
#include "gfx_func.h"
#include "gui.h"
#include "hotkeys.h"
#include "map_func.h"
#include "rail.h"
#include "rail_gui.h"
#include "rail_route.h"
#include "rail_route_gui.h"
#include "strings_func.h"
#include "tilehighlight_func.h"
#include "track_func.h"
#include "viewport_func.h"
#include "window_func.h"
#include "window_gui.h"

#include "widgets/rail_route_widget.h"

#include "table/strings.h"

#include "safeguards.h"

/** Shortest time between two plans of the preview. */
static constexpr std::chrono::milliseconds PREVIEW_MIN_INTERVAL{200};
/** Longest time between two plans of the preview, for routes that take long to plan. */
static constexpr std::chrono::milliseconds PREVIEW_MAX_INTERVAL{2000};
/** Largest distance between signals that can be chosen. */
static constexpr uint8_t MAX_SIGNAL_INTERVAL = 20;

/** Options of the trunk line window; the rail type and the signal variant are taken from the rail GUI when planning. Remembered while the game runs. */
static RailRouteOptions _rail_route_options;

/** The route preview shown while the end tile is chosen. */
struct RailRoutePreview {
	bool active = false; ///< The trunk line window is choosing the end tile.
	TileIndex start = INVALID_TILE; ///< Start tile of the route.
	std::vector<std::pair<TileIndex, TrackBits>> tracks{}; ///< Planned track per tile, sorted by tile.
	bool red = false; ///< The company cannot afford the planned route.
};

static RailRoutePreview _rail_route_preview; ///< The route preview.

/**
 * Is a trunk line being placed, so the viewport shows the route preview?
 * @return True when the viewport should draw the preview.
 */
bool IsRailRoutePreviewActive()
{
	return _rail_route_preview.active && _thd.window_class == WindowClass::RailRoute;
}

/**
 * Get what the route preview shows on a tile.
 * @param tile The tile.
 * @return The planned track of the tile and whether the tile is the start tile.
 */
RailRoutePreviewTile GetRailRoutePreviewTile(TileIndex tile)
{
	RailRoutePreviewTile result;
	result.start = tile == _rail_route_preview.start;
	result.red = _rail_route_preview.red;
	const auto &tracks = _rail_route_preview.tracks;
	auto it = std::ranges::lower_bound(tracks, tile, {}, &std::pair<TileIndex, TrackBits>::first);
	if (it != tracks.end() && it->first == tile) result.tracks = it->second;
	return result;
}

/**
 * Translate an error of the route planner.
 * @param error The error as returned in #RailRoutePlan::error.
 * @return The error in the language of the player.
 */
static std::string GetRailRouteErrorText(const std::string &error)
{
	static const std::pair<std::string_view, StringID> known[] = {
		{"invalid tile", STR_RAIL_ROUTE_ERROR_INVALID_TILE},
		{"invalid number of tracks", STR_RAIL_ROUTE_ERROR_INVALID_TRACKS},
		{"the route is too short", STR_RAIL_ROUTE_ERROR_TOO_SHORT},
		{"no route found", STR_RAIL_ROUTE_ERROR_NO_ROUTE},
		{"the corridor does not fit on the map", STR_RAIL_ROUTE_ERROR_DOES_NOT_FIT},
	};
	for (const auto &[text, string] : known) {
		if (error == text) return GetString(string);
	}
	return error;
}

/** The window of the rail trunk line tool. */
struct RailRouteWindow : Window {
	/** What the window is doing with the mouse on the map. */
	enum class Mode : uint8_t {
		None, ///< Nothing.
		PickStart, ///< Waiting for the click on the start tile.
		PickEnd, ///< Waiting for the click on the end tile, showing the preview.
	};

	/** What is known about the last planned route, without its commands. */
	struct Summary {
		bool valid = false; ///< A route was planned.
		std::string error{}; ///< Error of the planner, untranslated; empty when the route can be built.
		RailRouteEstimate estimate{}; ///< Estimated costs.
		uint length = 0; ///< Length in steps.
		uint bends = 0; ///< Number of bends.
		size_t track_tiles = 0; ///< Tiles with track.
		int min_height = 0; ///< Lowest height of the track.
		int max_height = 0; ///< Highest height of the track.
	};

	Mode mode = Mode::None; ///< Current map tool.
	TileIndex start_tile = INVALID_TILE; ///< Start tile, when choosing the end tile.
	TileIndex planned_end = INVALID_TILE; ///< End tile of the plan of the preview.
	bool plan_stale = false; ///< The options changed since the preview was planned.
	RailType planned_railtype = INVALID_RAILTYPE; ///< Rail type the preview was planned with.
	Summary summary{}; ///< The last planned route (preview or built).
	std::chrono::steady_clock::time_point last_plan{}; ///< When the preview was planned last.
	std::chrono::milliseconds last_plan_duration{0}; ///< How long planning the preview took last.

	RailRouteWindow(WindowDesc &desc, WindowNumber window_number) : Window(desc)
	{
		this->InitNested(window_number);
		this->UpdateButtons();
	}

	void Close([[maybe_unused]] int data = 0) override
	{
		this->ClearPreview();
		this->Window::Close();
	}

	/**
	 * Get the options to plan with: the options of the window, the rail type selected in the rail toolbar and the
	 * signal variant of the signal GUI.
	 * @return The options.
	 */
	static RailRouteOptions GetOptions()
	{
		RailRouteOptions options = _rail_route_options;
		RailType rt = GetSelectedRailType();
		options.railtype = rt == INVALID_RAILTYPE ? RAILTYPE_RAIL : rt;
		options.signal_type = SignalType::PathOneWay;
		options.signal_variant = GetSelectedSignalVariant();
		return options;
	}

	/**
	 * Can the local company afford a route?
	 * @param estimate Estimated costs of the route.
	 * @return True when the company has at least the estimated money.
	 */
	static bool CanAfford(const RailRouteEstimate &estimate)
	{
		return Company::IsValidID(_local_company) && Company::Get(_local_company)->money >= estimate.Total();
	}

	/** Enable and disable the buttons for the current state. */
	void UpdateButtons()
	{
		const RailRouteOptions &o = _rail_route_options;
		this->SetWidgetDisabledState(WID_RR_BUILD, !Company::IsValidID(_local_company));
		this->SetWidgetLoweredState(WID_RR_BUILD, this->mode != Mode::None);
		this->SetWidgetDisabledState(WID_RR_TRACKS_DECREASE, o.tracks <= 1);
		this->SetWidgetDisabledState(WID_RR_TRACKS_INCREASE, o.tracks >= RailRouteOptions::MAX_TRACKS);
		this->SetWidgetLoweredState(WID_RR_SIGNALS, o.signals);
		this->SetWidgetDisabledState(WID_RR_INTERVAL_DECREASE, !o.signals || o.signal_interval <= 1);
		this->SetWidgetDisabledState(WID_RR_INTERVAL_INCREASE, !o.signals || o.signal_interval >= MAX_SIGNAL_INTERVAL);
		this->SetWidgetDisabledState(WID_RR_INTERVAL, !o.signals);
		this->SetWidgetLoweredState(WID_RR_WATER, o.remove_water);
		this->SetWidgetLoweredState(WID_RR_DEMOLISH, o.demolish);
		this->SetWidgetLoweredState(WID_RR_SEA, o.raise_sea_level);
		this->SetWidgetDisabledState(WID_RR_ABORT, !GetBlueprintPasteStatus().active);
	}

	/** The options changed: plan the preview again and redraw. */
	void OnOptionsChanged()
	{
		this->plan_stale = true;
		this->UpdateButtons();
		this->SetDirty();
	}

	/**
	 * Remember what is shown about a plan.
	 * @param plan The plan.
	 */
	void Summarise(const RailRoutePlan &plan)
	{
		this->summary.valid = true;
		this->summary.error = plan.error;
		this->summary.estimate = plan.estimate;
		this->summary.length = plan.length;
		this->summary.bends = plan.bends;
		this->summary.track_tiles = plan.tracks.size();
		this->summary.min_height = plan.min_height;
		this->summary.max_height = plan.max_height;
	}

	/** Stop showing the route preview. */
	void ClearPreview()
	{
		if (!_rail_route_preview.active && _rail_route_preview.start == INVALID_TILE) return;
		_rail_route_preview = {};
		MarkWholeScreenDirty();
	}

	/**
	 * Plan the route from the start tile to a tile and show it as the preview.
	 * @param end The end tile.
	 */
	void PlanPreview(TileIndex end)
	{
		RailRouteOptions options = GetOptions();
		auto begin = std::chrono::steady_clock::now();
		RailRoutePlan plan = PlanRailRoute(this->start_tile, end, options);
		auto now = std::chrono::steady_clock::now();
		this->last_plan = now;
		this->last_plan_duration = std::chrono::duration_cast<std::chrono::milliseconds>(now - begin);
		this->planned_end = end;
		this->planned_railtype = options.railtype;
		this->plan_stale = false;

		this->Summarise(plan);
		_rail_route_preview.active = true;
		_rail_route_preview.start = this->start_tile;
		_rail_route_preview.red = !CanAfford(plan.estimate);
		_rail_route_preview.tracks = std::move(plan.tracks);
		/* The route can be anywhere on the screen. */
		MarkWholeScreenDirty();
		this->SetWidgetDirty(WID_RR_INFO);
	}

	/**
	 * Plan the route to the end tile and start building it.
	 * @param end The end tile.
	 */
	void BuildRoute(TileIndex end)
	{
		int x = TileX(end) * TILE_SIZE;
		int y = TileY(end) * TILE_SIZE;
		if (!Company::IsValidID(_local_company)) return;
		if (GetBlueprintPasteStatus().active) {
			ShowErrorMessage(GetEncodedString(STR_ERROR_RAIL_ROUTE_CAN_T_BUILD), GetEncodedString(STR_ERROR_RAIL_ROUTE_JOB_RUNNING), WarningLevel::Info, x, y);
			return;
		}

		RailRoutePlan plan = PlanRailRoute(this->start_tile, end, GetOptions());
		this->Summarise(plan);
		this->SetWidgetDirty(WID_RR_INFO);
		if (!plan.error.empty()) {
			ShowErrorMessage(GetEncodedString(STR_ERROR_RAIL_ROUTE_CAN_T_BUILD), GetEncodedString(STR_ERROR_RAIL_ROUTE_DETAIL, GetRailRouteErrorText(plan.error)), WarningLevel::Info, x, y);
			return;
		}
		Money needed = plan.estimate.Total();
		Money available = Company::Get(_local_company)->money;
		if (available < needed) {
			ShowErrorMessage(GetEncodedString(STR_ERROR_RAIL_ROUTE_CAN_T_BUILD), GetEncodedString(STR_ERROR_RAIL_ROUTE_NOT_ENOUGH_MONEY, needed, available), WarningLevel::Info, x, y);
			return;
		}
		std::string name = fmt::format("{},{} - {},{}", TileX(this->start_tile), TileY(this->start_tile), TileX(end), TileY(end));
		if (!StartConstructionJob(name, plan.origin, std::move(plan.prepare), std::move(plan.build), _local_company, true)) {
			ShowErrorMessage(GetEncodedString(STR_ERROR_RAIL_ROUTE_CAN_T_BUILD), GetEncodedString(STR_ERROR_RAIL_ROUTE_JOB_RUNNING), WarningLevel::Info, x, y);
		}
		this->UpdateButtons();
		this->SetDirty();
	}

	/**
	 * Get the lines of the info panel.
	 * @return The lines, each a complete string with colours.
	 */
	std::vector<std::string> GetInfoLines() const
	{
		std::vector<std::string> lines;
		RailRouteOptions options = GetOptions();
		lines.push_back(GetString(STR_RAIL_ROUTE_INFO_RAILTYPE, GetRailTypeInfo(options.railtype)->strings.name,
			options.signal_variant == SignalVariant::Semaphore ? STR_RAIL_ROUTE_INFO_SEMAPHORE : STR_RAIL_ROUTE_INFO_ELECTRIC));

		switch (this->mode) {
			case Mode::None: lines.push_back(GetString(STR_RAIL_ROUTE_INFO_IDLE)); break;
			case Mode::PickStart: lines.push_back(GetString(STR_RAIL_ROUTE_INFO_PICK_START)); break;
			case Mode::PickEnd: lines.push_back(GetString(STR_RAIL_ROUTE_INFO_PICK_END, TileX(this->start_tile), TileY(this->start_tile))); break;
		}

		const Summary &s = this->summary;
		if (!s.valid) return lines;
		if (!s.error.empty()) {
			lines.push_back(GetString(STR_RAIL_ROUTE_INFO_ERROR, GetRailRouteErrorText(s.error)));
			return lines;
		}
		const RailRouteEstimate &e = s.estimate;
		lines.push_back(GetString(STR_RAIL_ROUTE_INFO_LENGTH, s.length, s.bends, s.track_tiles, s.min_height, s.max_height));
		lines.push_back(GetString(STR_RAIL_ROUTE_INFO_COST, e.Total()));
		lines.push_back(GetString(STR_RAIL_ROUTE_INFO_COST_PARTS, e.clearing, e.terraform, e.track));
		lines.push_back(GetString(STR_RAIL_ROUTE_INFO_DEMOLISH, e.houses, e.industries));
		lines.push_back(GetString(STR_RAIL_ROUTE_INFO_WATER_BLOCKED, e.water, e.blocked));
		if (Company::IsValidID(_local_company) && !CanAfford(e)) {
			lines.push_back(GetString(STR_RAIL_ROUTE_INFO_NOT_ENOUGH_MONEY, e.Total(), Company::Get(_local_company)->money));
		}
		return lines;
	}

	/**
	 * Get the lines of the construction job panel.
	 * @return The lines, each a complete string with colours.
	 */
	static std::vector<std::string> GetStatusLines()
	{
		std::vector<std::string> lines;
		const BlueprintPasteStatus &s = GetBlueprintPasteStatus();
		if (s.active) {
			lines.push_back(GetString(STR_RAIL_ROUTE_STATUS_RUNNING, s.name, s.chunks_done, s.chunks_total, s.built, s.failed, s.cost));
		} else if (s.chunks_total != 0) {
			lines.push_back(GetString(STR_RAIL_ROUTE_STATUS_DONE, s.name, s.built, s.failed, s.cost));
		} else {
			lines.push_back(GetString(STR_RAIL_ROUTE_STATUS_NONE));
		}
		if (s.out_of_money) lines.push_back(GetString(STR_BLUEPRINT_STATUS_OUT_OF_MONEY));
		if (s.aborted) lines.push_back(GetString(STR_BLUEPRINT_STATUS_ABORTED));
		if (s.failed != 0 && s.last_error != INVALID_STRING_ID) lines.push_back(GetString(STR_BLUEPRINT_STATUS_LAST_ERROR, s.last_error));
		return lines;
	}

	/** Number of lines reserved for the info panel. */
	static constexpr uint INFO_LINES = 9;
	/** Number of lines reserved for the construction job panel. */
	static constexpr uint STATUS_LINES = 5;

	std::string GetWidgetString(WidgetID widget, StringID stringid) const override
	{
		switch (widget) {
			case WID_RR_TRACKS: return GetString(STR_JUST_INT, _rail_route_options.tracks);
			case WID_RR_INTERVAL: return GetString(STR_JUST_INT, _rail_route_options.signal_interval);
			default: return this->Window::GetWidgetString(widget, stringid);
		}
	}

	void UpdateWidgetSize(WidgetID widget, Dimension &size, [[maybe_unused]] const Dimension &padding, [[maybe_unused]] Dimension &fill, [[maybe_unused]] Dimension &resize) override
	{
		switch (widget) {
			case WID_RR_TRACKS:
			case WID_RR_INTERVAL:
				size.width = std::max(size.width, GetStringBoundingBox(GetString(STR_JUST_INT, 99)).width + padding.width);
				break;

			case WID_RR_INFO:
				size.height = std::max(size.height, padding.height + INFO_LINES * GetCharacterHeight(FontSize::Normal));
				break;

			case WID_RR_STATUS:
				size.height = std::max(size.height, padding.height + STATUS_LINES * GetCharacterHeight(FontSize::Normal));
				break;
		}
	}

	/**
	 * Draw lines of text into a panel, wrapping long lines, as many as fit.
	 * @param r The panel.
	 * @param lines The lines.
	 */
	static void DrawLines(const Rect &r, const std::vector<std::string> &lines)
	{
		Rect tr = r.Shrink(WidgetDimensions::scaled.framerect);
		for (const std::string &text : lines) {
			if (tr.top + GetCharacterHeight(FontSize::Normal) > tr.bottom + 1) break;
			tr.top = DrawStringMultiLine(tr, text);
		}
	}

	void DrawWidget(const Rect &r, WidgetID widget) const override
	{
		switch (widget) {
			case WID_RR_INFO: DrawLines(r, this->GetInfoLines()); break;
			case WID_RR_STATUS: DrawLines(r, GetStatusLines()); break;
		}
	}

	void OnClick([[maybe_unused]] Point pt, WidgetID widget, [[maybe_unused]] int click_count) override
	{
		RailRouteOptions &o = _rail_route_options;
		switch (widget) {
			case WID_RR_TRACKS_DECREASE:
				if (o.tracks > 1) o.tracks--;
				this->OnOptionsChanged();
				break;

			case WID_RR_TRACKS_INCREASE:
				if (o.tracks < RailRouteOptions::MAX_TRACKS) o.tracks++;
				this->OnOptionsChanged();
				break;

			case WID_RR_SIGNALS:
				o.signals = !o.signals;
				this->OnOptionsChanged();
				break;

			case WID_RR_INTERVAL_DECREASE:
				if (o.signal_interval > 1) o.signal_interval--;
				this->OnOptionsChanged();
				break;

			case WID_RR_INTERVAL_INCREASE:
				if (o.signal_interval < MAX_SIGNAL_INTERVAL) o.signal_interval++;
				this->OnOptionsChanged();
				break;

			case WID_RR_WATER:
				o.remove_water = !o.remove_water;
				this->OnOptionsChanged();
				break;

			case WID_RR_DEMOLISH:
				o.demolish = !o.demolish;
				this->OnOptionsChanged();
				break;

			case WID_RR_SEA:
				o.raise_sea_level = !o.raise_sea_level;
				this->OnOptionsChanged();
				break;

			case WID_RR_BUILD: {
				RailType rt = GetSelectedRailType();
				if (rt == INVALID_RAILTYPE) rt = RAILTYPE_RAIL;
				if (HandlePlacePushButton(this, WID_RR_BUILD, GetRailTypeInfo(rt)->cursor.autorail, HT_RECT)) {
					this->mode = Mode::PickStart;
					this->summary = {};
				}
				this->UpdateButtons();
				this->SetDirty();
				break;
			}

			case WID_RR_ABORT:
				AbortBlueprintPaste();
				this->UpdateButtons();
				this->SetDirty();
				break;
		}
	}

	void OnPlaceObject([[maybe_unused]] Point pt, TileIndex tile) override
	{
		switch (this->mode) {
			case Mode::PickStart:
				/* Nothing to plan yet; the preview starts when the cursor leaves the start tile. */
				this->start_tile = tile;
				this->planned_end = tile;
				this->plan_stale = false;
				this->summary = {};
				this->mode = Mode::PickEnd;
				_rail_route_preview = {};
				_rail_route_preview.active = true;
				_rail_route_preview.start = tile;
				MarkTileDirtyByTile(tile);
				break;

			case Mode::PickEnd:
				this->ClearPreview();
				this->mode = Mode::PickStart;
				this->BuildRoute(tile);
				this->start_tile = INVALID_TILE;
				break;

			case Mode::None:
				break;
		}
		this->SetDirty();
	}

	void OnPlaceObjectAbort() override
	{
		this->mode = Mode::None;
		this->start_tile = INVALID_TILE;
		this->ClearPreview();
		this->UpdateButtons();
		this->SetDirty();
	}

	void OnRealtimeTick([[maybe_unused]] uint delta_ms) override
	{
		if (this->mode != Mode::PickEnd || _thd.GetCallbackWnd() != this) return;

		/* The money changes all the time; keep the colour of the preview up to date. */
		bool red = !CanAfford(this->summary.estimate);
		if (red != _rail_route_preview.red) {
			_rail_route_preview.red = red;
			MarkWholeScreenDirty();
			this->SetWidgetDirty(WID_RR_INFO);
		}

		if ((_thd.drawstyle & HT_DRAG_MASK) == HT_NONE) return;
		TileIndex tile = TileVirtXY(_thd.pos.x, _thd.pos.y);
		if (tile >= Map::Size()) return;
		if (GetOptions().railtype != this->planned_railtype) this->plan_stale = true;
		if (tile == this->planned_end && !this->plan_stale) return;

		/* Plan at most every PREVIEW_MIN_INTERVAL, and leave the game at least twice the planning time in between. */
		auto interval = std::clamp(2 * this->last_plan_duration, PREVIEW_MIN_INTERVAL, PREVIEW_MAX_INTERVAL);
		if (std::chrono::steady_clock::now() - this->last_plan < interval) return;
		this->PlanPreview(tile);
	}

	/**
	 * Some data on this window has become invalid.
	 * @param data Unused.
	 * @param gui_scope Whether the call is done from GUI scope.
	 */
	void OnInvalidateData([[maybe_unused]] int data = 0, [[maybe_unused]] bool gui_scope = true) override
	{
		if (!gui_scope) return;
		this->UpdateButtons();
		this->SetWidgetDirty(WID_RR_STATUS);
		this->SetWidgetDirty(WID_RR_ABORT);
		this->SetWidgetDirty(WID_RR_BUILD);
	}

	static inline HotkeyList hotkeys{"trunk_line", {
		Hotkey('K', "build", WID_RR_BUILD),
		Hotkey(WKC_COMMA, "fewer_tracks", WID_RR_TRACKS_DECREASE),
		Hotkey(WKC_PERIOD, "more_tracks", WID_RR_TRACKS_INCREASE),
	}};
};

static constexpr std::initializer_list<NWidgetPart> _nested_rail_route_widgets = {
	NWidget(NWID_HORIZONTAL),
		NWidget(WWT_CLOSEBOX, Colours::DarkGreen),
		NWidget(WWT_CAPTION, Colours::DarkGreen, WID_RR_CAPTION), SetStringTip(STR_RAIL_ROUTE_CAPTION, STR_TOOLTIP_WINDOW_TITLE_DRAG_THIS),
		NWidget(WWT_SHADEBOX, Colours::DarkGreen),
		NWidget(WWT_STICKYBOX, Colours::DarkGreen),
	EndContainer(),
	NWidget(WWT_PANEL, Colours::DarkGreen),
		NWidget(NWID_VERTICAL), SetPadding(WidgetDimensions::unscaled.picker), SetPIP(0, WidgetDimensions::unscaled.vsep_normal, 0),
			NWidget(NWID_HORIZONTAL), SetPIP(0, WidgetDimensions::unscaled.hsep_wide, 0),
				NWidget(WWT_TEXT, Colours::Invalid), SetFill(1, 0), SetResize(1, 0), SetStringTip(STR_RAIL_ROUTE_TRACKS, STR_RAIL_ROUTE_TRACKS_TOOLTIP),
				NWidget(NWID_HORIZONTAL),
					NWidget(WWT_PUSHARROWBTN, Colours::DarkGreen, WID_RR_TRACKS_DECREASE), SetArrowWidgetTypeTip(ArrowWidgetType::Decrease, STR_RAIL_ROUTE_TRACKS_TOOLTIP),
					NWidget(WWT_LABEL, Colours::Invalid, WID_RR_TRACKS), SetToolTip(STR_RAIL_ROUTE_TRACKS_TOOLTIP),
					NWidget(WWT_PUSHARROWBTN, Colours::DarkGreen, WID_RR_TRACKS_INCREASE), SetArrowWidgetTypeTip(ArrowWidgetType::Increase, STR_RAIL_ROUTE_TRACKS_TOOLTIP),
				EndContainer(),
			EndContainer(),
			NWidget(NWID_HORIZONTAL), SetPIP(0, WidgetDimensions::unscaled.hsep_wide, 0),
				NWidget(WWT_BOOLBTN, Colours::DarkGreen, WID_RR_SIGNALS), SetAlternateColourTip(Colours::Yellow, STR_RAIL_ROUTE_SIGNALS_TOOLTIP),
				NWidget(WWT_TEXT, Colours::Invalid), SetFill(1, 0), SetResize(1, 0), SetStringTip(STR_RAIL_ROUTE_SIGNALS, STR_RAIL_ROUTE_SIGNALS_TOOLTIP),
				NWidget(NWID_HORIZONTAL),
					NWidget(WWT_PUSHARROWBTN, Colours::DarkGreen, WID_RR_INTERVAL_DECREASE), SetArrowWidgetTypeTip(ArrowWidgetType::Decrease, STR_RAIL_ROUTE_INTERVAL_TOOLTIP),
					NWidget(WWT_LABEL, Colours::Invalid, WID_RR_INTERVAL), SetToolTip(STR_RAIL_ROUTE_INTERVAL_TOOLTIP),
					NWidget(WWT_PUSHARROWBTN, Colours::DarkGreen, WID_RR_INTERVAL_INCREASE), SetArrowWidgetTypeTip(ArrowWidgetType::Increase, STR_RAIL_ROUTE_INTERVAL_TOOLTIP),
				EndContainer(),
			EndContainer(),
			NWidget(NWID_HORIZONTAL), SetPIP(0, WidgetDimensions::unscaled.hsep_wide, 0),
				NWidget(WWT_BOOLBTN, Colours::DarkGreen, WID_RR_WATER), SetAlternateColourTip(Colours::Yellow, STR_RAIL_ROUTE_WATER_TOOLTIP),
				NWidget(WWT_TEXT, Colours::Invalid), SetFill(1, 0), SetResize(1, 0), SetStringTip(STR_RAIL_ROUTE_WATER, STR_RAIL_ROUTE_WATER_TOOLTIP),
			EndContainer(),
			NWidget(NWID_HORIZONTAL), SetPIP(0, WidgetDimensions::unscaled.hsep_wide, 0),
				NWidget(WWT_BOOLBTN, Colours::DarkGreen, WID_RR_DEMOLISH), SetAlternateColourTip(Colours::Yellow, STR_RAIL_ROUTE_DEMOLISH_TOOLTIP),
				NWidget(WWT_TEXT, Colours::Invalid), SetFill(1, 0), SetResize(1, 0), SetStringTip(STR_RAIL_ROUTE_DEMOLISH, STR_RAIL_ROUTE_DEMOLISH_TOOLTIP),
			EndContainer(),
			NWidget(NWID_HORIZONTAL), SetPIP(0, WidgetDimensions::unscaled.hsep_wide, 0),
				NWidget(WWT_BOOLBTN, Colours::DarkGreen, WID_RR_SEA), SetAlternateColourTip(Colours::Yellow, STR_RAIL_ROUTE_SEA_TOOLTIP),
				NWidget(WWT_TEXT, Colours::Invalid), SetFill(1, 0), SetResize(1, 0), SetStringTip(STR_RAIL_ROUTE_SEA, STR_RAIL_ROUTE_SEA_TOOLTIP),
			EndContainer(),
		EndContainer(),
	EndContainer(),
	NWidget(WWT_TEXTBTN, Colours::DarkGreen, WID_RR_BUILD), SetResize(1, 0), SetFill(1, 0), SetStringTip(STR_RAIL_ROUTE_BUILD, STR_RAIL_ROUTE_BUILD_TOOLTIP),
	NWidget(WWT_PANEL, Colours::DarkGreen, WID_RR_INFO), SetMinimalSize(340, 0), SetResize(1, 0), SetFill(1, 0), EndContainer(),
	NWidget(WWT_PANEL, Colours::DarkGreen, WID_RR_STATUS), SetResize(1, 0), SetFill(1, 0), EndContainer(),
	NWidget(NWID_HORIZONTAL),
		NWidget(WWT_PUSHTXTBTN, Colours::DarkGreen, WID_RR_ABORT), SetResize(1, 0), SetFill(1, 0), SetStringTip(STR_RAIL_ROUTE_ABORT, STR_RAIL_ROUTE_ABORT_TOOLTIP),
		NWidget(WWT_RESIZEBOX, Colours::DarkGreen),
	EndContainer(),
};

/** Window definition of the trunk line window. */
static WindowDesc _rail_route_desc(
	WindowPosition::Automatic, "trunk_line", 0, 0,
	WindowClass::RailRoute, WindowClass::None,
	WindowDefaultFlag::Construction,
	_nested_rail_route_widgets,
	&RailRouteWindow::hotkeys
);

/**
 * Open the rail trunk line window, or bring it to the front.
 * @return The window.
 */
Window *ShowRailRouteWindow()
{
	return AllocateWindowDescFront<RailRouteWindow>(_rail_route_desc, 0);
}
