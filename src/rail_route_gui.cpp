/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/**
 * @file rail_route_gui.cpp GUI of the rail trunk line tool.
 *
 * The trunk line window holds the options of the route (orientation of the start, number of tracks, signals, what
 * may be cleared) and the placement tool. Before the first click the start pieces of all tracks (#GetRailRouteStartPieces)
 * follow the cursor; the first click fixes the start, which never moves afterwards (the orientation is always fixed,
 * so the planner keeps the tracks at the start tile whatever the end). While the end tile is being chosen, the route
 * to the tile under the cursor is planned again whenever that tile changes, but not more often than every
 * #PREVIEW_MIN_INTERVAL (longer for routes that take long to plan); an end out of reach of the start is moved into
 * reach by the planner and marked. The second click builds the route. The viewport draws the preview from
 * #GetRailRoutePreviewTile.
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

/**
 * Get the default options of the trunk line window: those of the planner with a straight start along the x axis.
 * @return The options.
 */
static RailRouteOptions GetDefaultRailRouteOptions()
{
	RailRouteOptions options;
	options.start = RailRouteStart::X;
	return options;
}

/**
 * Options of the trunk line window; the rail type and the signal variant are taken from the rail GUI when planning.
 * The start orientation is never Auto here. Remembered while the game runs.
 */
static RailRouteOptions _rail_route_options = GetDefaultRailRouteOptions();

/** The start orientation of each orientation button, in the order of the widgets (as in the rail toolbar). */
static constexpr RailRouteStart RAIL_ROUTE_START_BUTTONS[] = {RailRouteStart::NS, RailRouteStart::X, RailRouteStart::EW, RailRouteStart::Y};

/** The route preview shown while the end tile is chosen. */
struct RailRoutePreview {
	bool active = false; ///< The trunk line window is placing a route.
	TileIndex start = INVALID_TILE; ///< Start tile of the route (under the cursor until the start is fixed).
	TileIndex end = INVALID_TILE; ///< Tile where the planned route ends, or INVALID_TILE.
	std::vector<std::pair<TileIndex, TrackBits>> tracks{}; ///< Planned track (or only the start pieces) per tile, sorted by tile.
	bool red = false; ///< The route cannot be built towards the cursor, or the company cannot afford it.
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
	result.end = tile == _rail_route_preview.end;
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
		{"the end must lie in the direction of the start", STR_RAIL_ROUTE_ERROR_WRONG_DIRECTION},
		{"the route does not fit the start", STR_RAIL_ROUTE_ERROR_START_MISMATCH},
		{"invalid start orientation", STR_RAIL_ROUTE_ERROR_INVALID_START},
		{"invalid rail type", STR_RAIL_ROUTE_ERROR_INVALID_RAILTYPE},
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
		PickStart, ///< Waiting for the click on the start tile; the start pieces follow the cursor.
		PickEnd, ///< The start is fixed; waiting for the click on the end tile, showing the route to the cursor.
	};

	/** What is known about the last planned route, without its commands. */
	struct Summary {
		bool valid = false; ///< A route was planned.
		TileIndex requested_end = INVALID_TILE; ///< End tile the route was planned to.
		TileIndex end = INVALID_TILE; ///< End tile of the plan; differs from the requested one when that was out of reach.
		std::string error{}; ///< Error of the planner, untranslated; empty when the route can be built.
		RailRouteEstimate estimate{}; ///< Estimated costs.
		uint length = 0; ///< Length in steps.
		uint bends = 0; ///< Number of bends.
		size_t track_tiles = 0; ///< Tiles with track.
		int min_height = 0; ///< Lowest height of the track.
		int max_height = 0; ///< Highest height of the track.
	};

	Mode mode = Mode::None; ///< Current map tool.
	TileIndex start_tile = INVALID_TILE; ///< The fixed start tile, when choosing the end tile.
	TileIndex planned_end = INVALID_TILE; ///< Tile under the cursor the preview was made for (the start tile before the start is fixed).
	bool plan_stale = false; ///< The options changed since the preview was made.
	RailType planned_railtype = INVALID_RAILTYPE; ///< Rail type the preview was planned with.
	Summary summary{}; ///< The last planned route (preview or built).
	std::chrono::steady_clock::time_point last_plan{}; ///< When the preview was planned last.
	std::chrono::milliseconds last_plan_duration{0}; ///< How long planning the preview took last.
	RailType sprites_railtype = INVALID_RAILTYPE; ///< Rail type of the sprites on the orientation buttons.

	RailRouteWindow(WindowDesc &desc, WindowNumber window_number) : Window(desc)
	{
		this->InitNested(window_number);
		this->UpdateOrientationSprites();
		this->UpdateButtons();
	}

	/** Show the track sprites of the rail type selected in the rail toolbar on the orientation buttons. */
	void UpdateOrientationSprites()
	{
		RailType rt = GetSelectedRailType();
		const RailTypeInfo *rti = GetRailTypeInfo(rt == INVALID_RAILTYPE ? RAILTYPE_RAIL : rt);
		this->GetWidget<NWidgetCore>(WID_RR_START_NS)->SetSprite(rti->gui_sprites.build_ns_rail);
		this->GetWidget<NWidgetCore>(WID_RR_START_X)->SetSprite(rti->gui_sprites.build_x_rail);
		this->GetWidget<NWidgetCore>(WID_RR_START_EW)->SetSprite(rti->gui_sprites.build_ew_rail);
		this->GetWidget<NWidgetCore>(WID_RR_START_Y)->SetSprite(rti->gui_sprites.build_y_rail);
		this->sprites_railtype = rt;
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
		for (WidgetID widget = WID_RR_START_NS; widget <= WID_RR_START_Y; widget++) {
			this->SetWidgetLoweredState(widget, RAIL_ROUTE_START_BUTTONS[widget - WID_RR_START_NS] == o.start);
		}
		this->SetWidgetDisabledState(WID_RR_TRACKS_DECREASE, o.tracks <= 1);
		this->SetWidgetDisabledState(WID_RR_TRACKS_INCREASE, o.tracks >= RailRouteOptions::MAX_TRACKS);
		this->SetWidgetLoweredState(WID_RR_SIGNALS, o.signals);
		this->SetWidgetDisabledState(WID_RR_INTERVAL_DECREASE, !o.signals || o.signal_interval <= 1);
		this->SetWidgetDisabledState(WID_RR_INTERVAL_INCREASE, !o.signals || o.signal_interval >= MAX_SIGNAL_INTERVAL);
		this->SetWidgetDisabledState(WID_RR_INTERVAL, !o.signals);
		this->SetWidgetLoweredState(WID_RR_TERRAFORM, o.terraform);
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
	void Summarise(const RailRoutePlan &plan, TileIndex requested_end)
	{
		this->summary.valid = true;
		this->summary.requested_end = requested_end;
		this->summary.end = plan.end;
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
	 * Show only the start pieces of all tracks at a start tile as the preview.
	 * @param start The start tile.
	 * @param red Draw them in red: the route cannot be built towards the cursor.
	 */
	void ShowStartPreview(TileIndex start, bool red)
	{
		_rail_route_preview.active = true;
		_rail_route_preview.start = start;
		_rail_route_preview.end = INVALID_TILE;
		_rail_route_preview.red = red;
		RailRouteStart orientation = _rail_route_options.start;
		if (this->mode == Mode::PickStart) {
			RailRouteStart detected = GetRailRouteStartOrientation(start, GetOptions().railtype);
			if (detected != RailRouteStart::Auto) orientation = detected;
		}
		_rail_route_preview.tracks = GetRailRouteStartPieces(start, orientation, _rail_route_options.tracks);
	}

	/**
	 * Before the start is fixed: show the start pieces at the tile under the cursor.
	 * @param tile The tile under the cursor.
	 */
	void PreviewStart(TileIndex tile)
	{
		this->planned_end = tile;
		this->plan_stale = false;
		this->ShowStartPreview(tile, false);
		/* The start of many tracks covers many tiles; it is cheap to redraw. */
		MarkWholeScreenDirty();
	}

	/**
	 * After the start is fixed: plan the route from the start tile to a tile and show it as the preview.
	 * @param end The end tile (under the cursor).
	 */
	void PlanPreview(TileIndex end)
	{
		RailRouteOptions options = GetOptions();
		this->planned_end = end;
		this->planned_railtype = options.railtype;
		this->plan_stale = false;

		if (end == this->start_tile) {
			/* Nothing to plan; show the fixed start. */
			this->summary = {};
			this->ShowStartPreview(this->start_tile, false);
		} else {
			auto begin = std::chrono::steady_clock::now();
			RailRoutePlan plan = PlanRailRoute(this->start_tile, end, options);
			auto now = std::chrono::steady_clock::now();
			this->last_plan = now;
			this->last_plan_duration = std::chrono::duration_cast<std::chrono::milliseconds>(now - begin);

			this->Summarise(plan, end);
			if (!plan.error.empty() || plan.tracks.empty()) {
				/* Wrong direction or no route: only the fixed start, in red. */
				this->ShowStartPreview(this->start_tile, true);
			} else {
				_rail_route_preview.active = true;
				_rail_route_preview.start = this->start_tile;
				_rail_route_preview.end = plan.end;
				_rail_route_preview.red = !CanAfford(plan.estimate);
				_rail_route_preview.tracks = std::move(plan.tracks);
			}
		}
		/* The route can be anywhere on the screen. */
		MarkWholeScreenDirty();
		this->SetWidgetDirty(WID_RR_INFO);
	}

	/** Release the fixed start: the start pieces follow the cursor again. */
	void ReleaseStart()
	{
		this->mode = Mode::PickStart;
		this->start_tile = INVALID_TILE;
		this->summary = {};
		this->plan_stale = true;
	}

	/**
	 * Plan the route to the end tile and start building it.
	 * @param end The end tile.
	 */
	bool BuildRoute(TileIndex end)
	{
		int x = TileX(end) * TILE_SIZE;
		int y = TileY(end) * TILE_SIZE;
		if (!Company::IsValidID(_local_company)) return false;
		if (GetBlueprintPasteStatus().active) {
			ShowErrorMessage(GetEncodedString(STR_ERROR_RAIL_ROUTE_CAN_T_BUILD), GetEncodedString(STR_ERROR_RAIL_ROUTE_JOB_RUNNING), WarningLevel::Info, x, y);
			return false;
		}

		RailRoutePlan plan = PlanRailRoute(this->start_tile, end, GetOptions());
		this->Summarise(plan, end);
		this->SetWidgetDirty(WID_RR_INFO);
		if (!plan.error.empty()) {
			LogRailRouteFailure(this->start_tile, end, GetOptions(), plan);
			ShowErrorMessage(GetEncodedString(STR_ERROR_RAIL_ROUTE_CAN_T_BUILD), GetEncodedString(STR_ERROR_RAIL_ROUTE_DETAIL, GetRailRouteErrorText(plan.error)), WarningLevel::Info, x, y);
			return false;
		}
		if (_shift_pressed) {
			ShowEstimatedCostOrIncome(plan.estimate.Total(), x, y);
			return false;
		}
		Money needed = plan.estimate.Total();
		Money available = Company::Get(_local_company)->money;
		if (available < needed) {
			ShowErrorMessage(GetEncodedString(STR_ERROR_RAIL_ROUTE_CAN_T_BUILD), GetEncodedString(STR_ERROR_RAIL_ROUTE_NOT_ENOUGH_MONEY, needed, available), WarningLevel::Info, x, y);
			return false;
		}
		std::string name = fmt::format("{},{} - {},{}", TileX(this->start_tile), TileY(this->start_tile), TileX(plan.end), TileY(plan.end));
		if (plan.prepare.empty() && plan.build.empty()) return true;
		if (!StartConstructionJob(name, plan.origin, std::move(plan.prepare), std::move(plan.build), _local_company, true)) {
			ShowErrorMessage(GetEncodedString(STR_ERROR_RAIL_ROUTE_CAN_T_BUILD), GetEncodedString(STR_ERROR_RAIL_ROUTE_JOB_RUNNING), WarningLevel::Info, x, y);
			return false;
		}
		this->UpdateButtons();
		this->SetDirty();
		return true;
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
		if (s.end != s.requested_end && s.end != INVALID_TILE) lines.push_back(GetString(STR_RAIL_ROUTE_INFO_END_MOVED, TileX(s.end), TileY(s.end)));
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
	static constexpr uint INFO_LINES = 10;
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
			case WID_RR_START_NS:
			case WID_RR_START_X:
			case WID_RR_START_EW:
			case WID_RR_START_Y: {
				RailRouteStart start = RAIL_ROUTE_START_BUTTONS[widget - WID_RR_START_NS];
				if (start == o.start) break;
				o.start = start;
				/* The fixed start was placed for the old orientation: place it again. */
				if (this->mode == Mode::PickEnd) this->ReleaseStart();
				this->OnOptionsChanged();
				break;
			}

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

			case WID_RR_TERRAFORM:
				o.terraform = !o.terraform;
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
				/* The start is fixed now; the route is planned when the cursor leaves the start tile. */
				this->start_tile = tile;
				if (RailRouteStart detected = GetRailRouteStartOrientation(tile, GetOptions().railtype); detected != RailRouteStart::Auto) {
					_rail_route_options.start = detected;
					this->UpdateButtons();
				}
				this->summary = {};
				this->mode = Mode::PickEnd;
				this->PlanPreview(tile);
				break;

			case Mode::PickEnd:
				if (!this->BuildRoute(tile)) break;
				/* Wait for the next start; the start pieces follow the cursor again. */
				this->ClearPreview();
				this->mode = Mode::PickStart;
				this->start_tile = INVALID_TILE;
				this->plan_stale = true;
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
		RailType rt = GetSelectedRailType();
		if (rt != this->sprites_railtype) {
			this->UpdateOrientationSprites();
			this->SetDirty();
		}

		if (this->mode == Mode::None || _thd.GetCallbackWnd() != this) return;
		if ((_thd.drawstyle & HT_DRAG_MASK) == HT_NONE) return;
		TileIndex tile = TileVirtXY(_thd.pos.x, _thd.pos.y);
		if (tile >= Map::Size()) return;

		if (this->mode == Mode::PickStart) {
			/* Cheap: no planning, so follow the cursor at once. */
			if (tile != this->planned_end || this->plan_stale || !_rail_route_preview.active) this->PreviewStart(tile);
			return;
		}

		/* The money changes all the time; keep the colour of a buildable preview up to date. */
		if (this->summary.valid && this->summary.error.empty()) {
			bool red = !CanAfford(this->summary.estimate);
			if (red != _rail_route_preview.red) {
				_rail_route_preview.red = red;
				MarkWholeScreenDirty();
				this->SetWidgetDirty(WID_RR_INFO);
			}
		}

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

	EventState OnHotkey(int hotkey) override
	{
		/* The number keys choose the orientation only while placing; otherwise they belong to the rail toolbar. */
		if (hotkey >= WID_RR_START_NS && hotkey <= WID_RR_START_Y && (this->mode == Mode::None || _thd.GetCallbackWnd() != this)) return EventState::NotHandled;
		return this->Window::OnHotkey(hotkey);
	}

	static inline HotkeyList hotkeys{"trunk_line", {
		Hotkey('K', "build", WID_RR_BUILD),
		Hotkey('1', "start_ns", WID_RR_START_NS),
		Hotkey('2', "start_x", WID_RR_START_X),
		Hotkey('3', "start_ew", WID_RR_START_EW),
		Hotkey('4', "start_y", WID_RR_START_Y),
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
				NWidget(WWT_TEXT, Colours::Invalid), SetFill(1, 0), SetResize(1, 0), SetStringTip(STR_RAIL_ROUTE_START, STR_RAIL_ROUTE_START_TOOLTIP),
				NWidget(NWID_HORIZONTAL),
					NWidget(WWT_IMGBTN, Colours::DarkGreen, WID_RR_START_NS), SetToolbarMinimalSize(1), SetSpriteTip(SPR_IMG_RAIL_NS, STR_RAIL_ROUTE_START_NS_TOOLTIP),
					NWidget(WWT_IMGBTN, Colours::DarkGreen, WID_RR_START_X), SetToolbarMinimalSize(1), SetSpriteTip(SPR_IMG_RAIL_NE, STR_RAIL_ROUTE_START_X_TOOLTIP),
					NWidget(WWT_IMGBTN, Colours::DarkGreen, WID_RR_START_EW), SetToolbarMinimalSize(1), SetSpriteTip(SPR_IMG_RAIL_EW, STR_RAIL_ROUTE_START_EW_TOOLTIP),
					NWidget(WWT_IMGBTN, Colours::DarkGreen, WID_RR_START_Y), SetToolbarMinimalSize(1), SetSpriteTip(SPR_IMG_RAIL_NW, STR_RAIL_ROUTE_START_Y_TOOLTIP),
				EndContainer(),
			EndContainer(),
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
				NWidget(WWT_BOOLBTN, Colours::DarkGreen, WID_RR_TERRAFORM), SetAlternateColourTip(Colours::Yellow, STR_RAIL_ROUTE_TERRAFORM_TOOLTIP),
				NWidget(WWT_TEXT, Colours::Invalid), SetFill(1, 0), SetResize(1, 0), SetStringTip(STR_RAIL_ROUTE_TERRAFORM, STR_RAIL_ROUTE_TERRAFORM_TOOLTIP),
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
