/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file agent_capture.cpp Capture of the live game frame for the in-game agent bridge. */

#include "stdafx.h"
#include "agent_capture.h"
#include "blitter/factory.hpp"
#include "core/format.hpp"
#include "core/math_func.hpp"
#include "gfx_func.h"
#include "map_func.h"
#include "openttd.h"
#include "palette_func.h"
#include "screenshot_gui.h"
#include "screenshot_type.h"
#include "settings_type.h"
#include "string_func.h"
#include "tile_type.h"
#include "timer/timer_game_tick.h"
#include "town.h"
#include "video/video_driver.hpp"
#include "viewport_func.h"
#include "window_func.h"
#include "window_gui.h"
#include "window_type.h"
#include "3rdparty/md5/md5.h"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <random>

#include "safeguards.h"

namespace InGameAgent {

/** Part of the screen frame that is captured, in screen pixels. */
struct CaptureRect {
	int left;
	int top;
	int width;
	int height;
};

/** Default and hard limits of a single capture, so one request stays bounded. */
static constexpr uint64_t DEFAULT_MAX_PIXELS = 4096ull * 4096ull;
static constexpr uint64_t HARD_MAX_PIXELS = 8192ull * 8192ull;

/**
 * The backend rejects a request body above 8 MiB (`MAX_BODY` in agent/backend/server.py), and a
 * result it refuses is retried by the bridge forever. The PNG travels base64 encoded (4 bytes for
 * every 3) inside the JSON result, so the PNG itself has to stay well below that limit.
 */
static constexpr size_t MAX_RESULT_BODY_BYTES = 8ull * 1024ull * 1024ull;
static constexpr size_t RESULT_BODY_RESERVE = 128ull * 1024ull; ///< Room for the JSON envelope around the base64 payload.
static constexpr size_t MAX_PNG_BYTES = (MAX_RESULT_BODY_BYTES - RESULT_BODY_RESERVE) / 4 * 3; ///< Largest PNG that still fits the body limit.
static constexpr size_t DEFAULT_MAX_BYTES = 5ull * 1024ull * 1024ull; ///< Default cap; a 1280x800 frame is about 1 MiB.

static std::atomic<uint64_t> _capture_serial{0}; ///< Serial number, to give every capture its own directory.
static std::string _last_capture_hash; ///< Hash of the previous capture, reported as the parent hash.

/**
 * Private temporary directory of one capture, removed again however the scope is left.
 *
 * The directory is created with an atomic mkdir, so two captures can never pick the same name and
 * an existing directory is never reused or overwritten. The PNG lives inside it, so the file name
 * itself does not have to be unique.
 */
class CaptureTempDir {
public:
	CaptureTempDir() = default;
	~CaptureTempDir() { this->Remove(); }

	CaptureTempDir(const CaptureTempDir &) = delete;
	CaptureTempDir &operator=(const CaptureTempDir &) = delete;

	/**
	 * Create a new unique directory in the system temporary directory.
	 * @return True iff the directory was created.
	 */
	bool Create()
	{
		std::error_code ec;
		const std::filesystem::path base = std::filesystem::temp_directory_path(ec);
		if (ec) return false;

		for (int attempt = 0; attempt < 16; attempt++) {
			const std::filesystem::path candidate = base / fmt::format("openttd-agent-capture-{}-{}-{}",
					_capture_serial.fetch_add(1), std::chrono::steady_clock::now().time_since_epoch().count(), std::random_device{}());

			std::error_code create_ec;
			if (std::filesystem::create_directory(candidate, create_ec)) {
				this->path = candidate;
				/* Keep the pixels private to this account; best effort, the name is already unique. */
				std::filesystem::permissions(candidate, std::filesystem::perms::owner_all, std::filesystem::perm_options::replace, ec);
				return true;
			}
			if (create_ec) return false; // not a name collision but a real error
		}
		return false;
	}

	/** The directory that was created. */
	const std::filesystem::path &Path() const { return this->path; }

private:
	void Remove()
	{
		if (this->path.empty()) return;
		std::error_code ec;
		std::filesystem::remove_all(this->path, ec);
		this->path.clear();
	}

	std::filesystem::path path;
};

/**
 * Encode binary data as base64.
 * @param data The data to encode.
 * @return The base64 representation of the data.
 */
static std::string Base64Encode(std::string_view data)
{
	static constexpr char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

	std::string out;
	out.reserve(((data.size() + 2) / 3) * 4);

	size_t i = 0;
	for (; i + 2 < data.size(); i += 3) {
		const uint32_t v = (static_cast<uint8_t>(data[i]) << 16) | (static_cast<uint8_t>(data[i + 1]) << 8) | static_cast<uint8_t>(data[i + 2]);
		out.push_back(alphabet[(v >> 18) & 0x3F]);
		out.push_back(alphabet[(v >> 12) & 0x3F]);
		out.push_back(alphabet[(v >> 6) & 0x3F]);
		out.push_back(alphabet[v & 0x3F]);
	}

	if (i + 1 == data.size()) {
		const uint32_t v = static_cast<uint8_t>(data[i]) << 16;
		out.push_back(alphabet[(v >> 18) & 0x3F]);
		out.push_back(alphabet[(v >> 12) & 0x3F]);
		out.append("==");
	} else if (i + 2 == data.size()) {
		const uint32_t v = (static_cast<uint8_t>(data[i]) << 16) | (static_cast<uint8_t>(data[i + 1]) << 8);
		out.push_back(alphabet[(v >> 18) & 0x3F]);
		out.push_back(alphabet[(v >> 12) & 0x3F]);
		out.push_back(alphabet[(v >> 6) & 0x3F]);
		out.push_back('=');
	}

	return out;
}

/**
 * Copy a part of the current screen frame into the buffer of the screenshot provider.
 * @param rect  The part of the screen frame to copy.
 * @param buf   Destination buffer.
 * @param y     First line to copy.
 * @param pitch Number of pixels per line in the destination buffer.
 * @param n     Number of lines to copy.
 */
static void CaptureFrameCallback(const CaptureRect &rect, void *buf, uint y, uint pitch, uint n)
{
	Blitter *blitter = BlitterFactory::GetCurrentBlitter();
	void *src = blitter->MoveTo(_screen.dst_ptr, rect.left, rect.top + static_cast<int>(y));
	blitter->CopyImageToBuffer(src, buf, rect.width, n, pitch);
}

/**
 * Create the result of a failed capture.
 * @param error Machine readable reason of the failure.
 * @return The JSON result.
 */
static Json Failure(std::string_view error)
{
	Json result = Json::object();
	result["ok"] = false;
	result["error"] = error;
	result["tick"] = TimerGameTick::counter;
	return result;
}

Json CaptureGameFrame(const Json &params)
{
	/* Without a video driver, blitter or frame buffer there is nothing to capture; say so. */
	Blitter *blitter = BlitterFactory::GetCurrentBlitter();
	if (blitter == nullptr || VideoDriver::GetInstance() == nullptr || _screen.dst_ptr == nullptr || _screen.width <= 0 || _screen.height <= 0) {
		return Failure("headless_unavailable");
	}

	std::string kind = "viewport";
	CaptureRect rect{0, 0, _screen.width, _screen.height};
	uint64_t max_pixels = DEFAULT_MAX_PIXELS;
	size_t max_bytes = DEFAULT_MAX_BYTES;
	bool include_png = true;
	try {
		kind = params.value("kind", kind);
		max_pixels = std::min<uint64_t>(params.value("max_pixels", DEFAULT_MAX_PIXELS), HARD_MAX_PIXELS);
		/* Clamp to the largest PNG the backend can still receive, so an accepted result is deliverable. */
		max_bytes = std::min<size_t>(params.value("max_bytes", DEFAULT_MAX_BYTES), MAX_PNG_BYTES);
		include_png = params.value("include_png", include_png);

		if (kind == "region") {
			if (!params.contains("region") || !params["region"].is_object()) return Failure("invalid_params");
			const Json &region = params["region"];
			if (!region.contains("left") || !region.contains("top") || !region.contains("width") || !region.contains("height")) return Failure("invalid_params");

			rect.left = region.value("left", 0);
			rect.top = region.value("top", 0);
			rect.width = region.value("width", 0);
			rect.height = region.value("height", 0);

			/* Clamp the requested region to the frame that actually exists. */
			const int64_t right = std::min<int64_t>(static_cast<int64_t>(rect.left) + rect.width, _screen.width);
			const int64_t bottom = std::min<int64_t>(static_cast<int64_t>(rect.top) + rect.height, _screen.height);
			rect.left = std::max(rect.left, 0);
			rect.top = std::max(rect.top, 0);
			rect.width = static_cast<int>(right - rect.left);
			rect.height = static_cast<int>(bottom - rect.top);
			if (rect.width <= 0 || rect.height <= 0) return Failure("invalid_params");
		} else if (kind != "viewport") {
			return Failure("invalid_params");
		}
	} catch (const Json::exception &) {
		return Failure("invalid_params");
	}

	const uint64_t pixels = static_cast<uint64_t>(rect.width) * static_cast<uint64_t>(rect.height);
	if (pixels > max_pixels) {
		Json result = Failure("too_large");
		result["width"] = rect.width;
		result["height"] = rect.height;
		result["pixels"] = pixels;
		result["max_pixels"] = max_pixels;
		return result;
	}

	/* Use the regular PNG screenshot provider, so the encoding is the same as a normal screenshot. */
	const ScreenshotProvider *provider = nullptr;
	for (const ScreenshotProvider *candidate : ProviderManager<ScreenshotProvider>::GetProviders()) {
		if (candidate->GetName() == "png") {
			provider = candidate;
			break;
		}
	}
	if (provider == nullptr) return Failure("png_unavailable");

	/* Write to a private directory in the temporary directory, never to a file of the user. */
	CaptureTempDir temp_dir;
	if (!temp_dir.Create()) return Failure("temp_dir_unavailable");
	const std::filesystem::path path = temp_dir.Path() / "capture.png";

	/* The screen buffer can still show the frame from before a command that ran earlier in this
	 * same game loop iteration. Redraw the dirty blocks first, exactly like a viewport screenshot
	 * does, so the pixels belong to the state whose tick we report. */
	const uint64_t frame_tick = TimerGameTick::counter;
	bool written = false;
	{
		/* Lock the video buffer like the crashlog screenshot does, so the frame is stable while it is copied. */
		VideoDriver::VideoBufferLocker video_buffer_lock;

		SetScreenshotWindowVisibility(true);
		UndrawMouseCursor();
		DrawDirtyBlocks();
		SetScreenshotWindowVisibility(false);

		written = provider->MakeImage(path.string(), [&rect](void *buf, uint y, uint pitch, uint n) {
			CaptureFrameCallback(rect, buf, y, pitch, n);
		}, rect.width, rect.height, blitter->GetScreenDepth(), _cur_palette.palette);
	}

	/* If the game advanced while the frame was copied, the pixels are older than the state we would
	 * report; a stale frame is not visual verification. */
	const uint64_t tick_after_copy = TimerGameTick::counter;
	if (tick_after_copy != frame_tick) {
		Json result = Json::object();
		result["ok"] = false;
		result["error"] = "stale_frame";
		result["tick"] = frame_tick;
		result["tick_now"] = tick_after_copy;
		return result;
	}

	std::string png;
	if (written) {
		std::ifstream file(path, std::ios::binary);
		if (file) png.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
	}
	if (!written || png.empty()) return Failure("encode_failed");

	if (png.size() > max_bytes) {
		Json result = Failure("too_large");
		result["tick"] = frame_tick;
		result["png_bytes"] = png.size();
		result["max_bytes"] = max_bytes;
		result["max_body_bytes"] = MAX_RESULT_BODY_BYTES;
		return result;
	}

	Md5 md5;
	md5.Append(png.data(), png.size());
	MD5Hash hash;
	md5.Finish(hash);
	const std::string hash_hex = FormatArrayAsHex(hash);

	Json result = Json::object();
	result["ok"] = true;
	result["kind"] = kind;
	result["width"] = rect.width;
	result["height"] = rect.height;
	result["tick"] = frame_tick;
	result["camera"] = {
		{"left", rect.left},
		{"top", rect.top},
		{"width", rect.width},
		{"height", rect.height},
	};
	result["frame"] = {
		{"redrawn", true},
		{"tick", frame_tick},
	};
	result["hash"] = hash_hex;
	result["hash_algorithm"] = "md5";
	result["parent_hash"] = _last_capture_hash.empty() ? Json(nullptr) : Json(_last_capture_hash);
	result["png_bytes"] = png.size();
	if (include_png) result["png_base64"] = Base64Encode(png);

	_last_capture_hash = hash_hex;
	return result;
}

/**
 * Name of a zoom level, for the JSON results.
 * @param zoom The zoom level.
 * @return The name of the zoom level.
 */
static std::string_view ZoomLevelName(ZoomLevel zoom)
{
	switch (zoom) {
		case ZoomLevel::In4x: return "in4x";
		case ZoomLevel::In2x: return "in2x";
		case ZoomLevel::Normal: return "normal";
		case ZoomLevel::Out2x: return "out2x";
		case ZoomLevel::Out4x: return "out4x";
		case ZoomLevel::Out8x: return "out8x";
		case ZoomLevel::End: break;
	}
	return "unknown";
}

/**
 * Name of the game mode, for the JSON results.
 * @param mode The game mode.
 * @return The name of the game mode.
 */
static std::string_view GameModeName(GameMode mode)
{
	switch (mode) {
		case GameMode::Menu: return "menu";
		case GameMode::Normal: return "normal";
		case GameMode::Editor: return "editor";
		case GameMode::Bootstrap: return "bootstrap";
	}
	return "unknown";
}

/**
 * The main window, when it has a viewport.
 * @return The main window, or nullptr when the game shows no main viewport.
 */
static Window *GetMainViewportWindow()
{
	Window *w = FindWindowById(WindowClass::MainWindow, 0);
	return (w != nullptr && w->viewport != nullptr) ? w : nullptr;
}

/**
 * Describe the rectangle and the zoom of a viewport, in screen pixels.
 * @param vp The viewport.
 * @return The JSON description.
 */
static Json ViewportJson(const Viewport &vp)
{
	return Json{
		{"left", vp.left},
		{"top", vp.top},
		{"width", vp.width},
		{"height", vp.height},
		{"virtual_left", vp.virtual_left},
		{"virtual_top", vp.virtual_top},
		{"virtual_width", vp.virtual_width},
		{"virtual_height", vp.virtual_height},
		{"zoom", to_underlying(vp.zoom)},
		{"zoom_name", ZoomLevelName(vp.zoom)},
	};
}

/**
 * Describe where a viewport looks and how far it is zoomed in.
 * @param vp The viewport data of the main window.
 * @return The JSON description.
 */
static Json CameraJson(const ViewportData &vp)
{
	const int center_x = vp.virtual_left + vp.virtual_width / 2;
	const int center_y = vp.virtual_top + vp.virtual_height / 2;
	const bool center_on_map = center_x >= 0 && center_y >= 0 &&
			center_x < static_cast<int>(Map::SizeX()) * static_cast<int>(TILE_SIZE) &&
			center_y < static_cast<int>(Map::SizeY()) * static_cast<int>(TILE_SIZE);

	return Json{
		{"center_world", {{"x", center_x}, {"y", center_y}}},
		{"center_tile", center_on_map ? Json{{"x", center_x / static_cast<int>(TILE_SIZE)}, {"y", center_y / static_cast<int>(TILE_SIZE)}} : Json(nullptr)},
		{"scrollpos", {{"x", vp.scrollpos_x}, {"y", vp.scrollpos_y}}},
		{"dest_scrollpos", {{"x", vp.dest_scrollpos_x}, {"y", vp.dest_scrollpos_y}}},
		{"zoom", to_underlying(vp.zoom)},
		{"zoom_name", ZoomLevelName(vp.zoom)},
	};
}

/**
 * The window that covers a screen position, when it is not the main window.
 * @param x Screen x.
 * @param y Screen y.
 * @return The occluding window as JSON, or null when the main window is on top there.
 */
static Json OccludingWindowJson(int x, int y)
{
	const Window *w = FindWindowFromPt(x, y);
	if (w == nullptr || w->window_class == WindowClass::MainWindow) return Json(nullptr);
	return Json{
		{"class", to_underlying(w->window_class)},
		{"number", static_cast<int32_t>(w->window_number)},
	};
}

/**
 * Project a tile to the screen.
 * @param vp   The viewport to project into.
 * @param tile The tile to project.
 * @return The projection as JSON.
 */
static Json ProjectTile(const Viewport &vp, TileIndex tile)
{
	/* The centre of the tile at ground height, the same point ScrollMainWindowToTile() centres on. */
	const int x = TileX(tile) * TILE_SIZE + TILE_SIZE / 2;
	const int y = TileY(tile) * TILE_SIZE + TILE_SIZE / 2;
	const int z = GetSlopePixelZ(x, y);
	const Point screen = WorldToScreen(vp, x, y, z);
	const bool in_viewport = screen.x >= vp.left && screen.x < vp.left + vp.width && screen.y >= vp.top && screen.y < vp.top + vp.height;
	const Json occluding = OccludingWindowJson(screen.x, screen.y);

	return Json{
		{"coordinate_space", "screen_pixels"},
		{"tile", {{"index", tile.base()}, {"x", TileX(tile)}, {"y", TileY(tile)}}},
		{"world", {{"x", x}, {"y", y}, {"z", z}}},
		{"screen", {{"x", screen.x}, {"y", screen.y}}},
		{"in_viewport", in_viewport},
		{"visible", in_viewport && occluding.is_null()},
		{"occluding_window", occluding},
		{"zoom", to_underlying(vp.zoom)},
	};
}

/**
 * Project a screen pixel to the map.
 * @param vp           The viewport to project into.
 * @param x            Screen x.
 * @param y            Screen y.
 * @param clamp_to_map Clamp the result to the map.
 * @return The projection as JSON.
 */
static Json ProjectScreen(const Viewport &vp, int x, int y, bool clamp_to_map)
{
	const bool in_viewport = x >= vp.left && x < vp.left + vp.width && y >= vp.top && y < vp.top + vp.height;
	const Json occluding = OccludingWindowJson(x, y);

	/* TranslateXYToTileCoord() returns the world position under the pixel, not a tile index. */
	Json world = Json(nullptr);
	Json tile = Json(nullptr);
	Json tile_index = Json(nullptr);
	if (in_viewport) {
		const Point pt = TranslateXYToTileCoord(vp, x, y, clamp_to_map);
		world = {{"x", pt.x}, {"y", pt.y}};
		if (pt.x >= 0 && pt.y >= 0 &&
				pt.x < static_cast<int>(Map::SizeX()) * static_cast<int>(TILE_SIZE) &&
				pt.y < static_cast<int>(Map::SizeY()) * static_cast<int>(TILE_SIZE)) {
			tile = {{"x", pt.x / static_cast<int>(TILE_SIZE)}, {"y", pt.y / static_cast<int>(TILE_SIZE)}};
			tile_index = TileVirtXY(pt.x, pt.y).base();
		}
	}

	return Json{
		{"coordinate_space", "world"},
		{"screen", {{"x", x}, {"y", y}}},
		{"world", world},
		{"tile", tile},
		{"tile_index", tile_index},
		{"in_viewport", in_viewport},
		{"visible", in_viewport && occluding.is_null()},
		{"occluding_window", occluding},
		{"clamp_to_map", clamp_to_map},
		{"zoom", to_underlying(vp.zoom)},
	};
}

/**
 * Resolve the tile a request refers to.
 *
 * Accepts "tile" (a tile index), "tile_x"/"tile_y" (tile coordinates), "town" (a town id,
 * resolved to the town centre tile) and "center" (an object with tile coordinates "x"/"y").
 *
 * @param params The request parameters.
 * @param tile   Receives the resolved tile.
 * @param error  Receives the error to report when the tile cannot be resolved.
 * @return True iff a tile was requested and resolved.
 */
static bool ResolveTile(const Json &params, TileIndex &tile, std::string_view &error)
{
	if (params.contains("tile")) {
		const int64_t index = params.value("tile", int64_t{-1});
		if (index < 0 || index > UINT32_MAX) {
			error = "invalid_params";
			return false;
		}
		tile = static_cast<TileIndex>(index);
	} else if (params.contains("town")) {
		const int64_t town_id = params.value("town", int64_t{-1});
		if (town_id < 0 || town_id > UINT16_MAX || !Town::IsValidID(static_cast<TownID>(town_id))) {
			error = "out_of_bounds";
			return false;
		}
		tile = Town::Get(static_cast<TownID>(town_id))->xy;
	} else {
		int64_t x;
		int64_t y;
		if (params.contains("center")) {
			if (!params["center"].is_object()) {
				error = "invalid_params";
				return false;
			}
			x = params["center"].value("x", int64_t{-1});
			y = params["center"].value("y", int64_t{-1});
		} else {
			if (!params.contains("tile_x") || !params.contains("tile_y")) {
				error = "invalid_params";
				return false;
			}
			x = params.value("tile_x", int64_t{-1});
			y = params.value("tile_y", int64_t{-1});
		}
		if (x < 0 || y < 0 || x >= Map::SizeX() || y >= Map::SizeY()) {
			error = "out_of_bounds";
			return false;
		}
		tile = TileXY(static_cast<uint>(x), static_cast<uint>(y));
	}

	if (!IsValidTile(tile)) {
		error = "out_of_bounds";
		return false;
	}
	return true;
}

Json InspectGameViewport(const Json &params)
{
	/* Parse and validate the request before touching any game state. */
	const bool project_tile = params.contains("tile") || params.contains("tile_x") || params.contains("tile_y") || params.contains("town") || params.contains("center");
	const bool project_screen = params.contains("screen_x") || params.contains("screen_y");
	if (project_tile && project_screen) return Failure("invalid_params");
	if (project_screen && (!params.contains("screen_x") || !params.contains("screen_y"))) return Failure("invalid_params");

	TileIndex tile = INVALID_TILE;
	int screen_x = 0;
	int screen_y = 0;
	bool clamp_to_map = true;
	try {
		if (project_tile) {
			std::string_view error;
			if (!ResolveTile(params, tile, error)) return Failure(error);
		}
		if (project_screen) {
			screen_x = params.value("screen_x", 0);
			screen_y = params.value("screen_y", 0);
		}
		clamp_to_map = params.value("clamp_to_map", true);
	} catch (const Json::exception &) {
		return Failure("invalid_params");
	}

	Window *w = GetMainViewportWindow();
	if (w == nullptr) return Failure("viewport_unavailable");
	const ViewportData &vp = *w->viewport;

	Json result = Json::object();
	result["ok"] = true;
	result["tick"] = TimerGameTick::counter;
	result["coordinate_space"] = "screen_pixels";
	result["game_mode"] = GameModeName(_game_mode);
	result["in_game"] = _game_mode != GameMode::Menu;
	result["screen"] = {{"width", _screen.width}, {"height", _screen.height}};
	result["viewport"] = ViewportJson(vp);
	result["camera"] = CameraJson(vp);
	if (project_tile) result["projection"] = ProjectTile(vp, tile);
	if (project_screen) result["projection"] = ProjectScreen(vp, screen_x, screen_y, clamp_to_map);
	return result;
}

Json SetGameCamera(const Json &params)
{
	/* Parse and validate the request before touching any game state. */
	const bool has_tile = params.contains("tile") || params.contains("tile_x") || params.contains("tile_y") || params.contains("town") || params.contains("center");
	const bool has_scroll = params.contains("scroll");
	const bool has_zoom = params.contains("zoom");
	const bool has_zoom_delta = params.contains("zoom_delta");
	if (!has_tile && !has_scroll && !has_zoom && !has_zoom_delta) return Failure("invalid_params");
	if (has_zoom && has_zoom_delta) return Failure("invalid_params");
	if (has_tile && has_scroll) return Failure("invalid_params");

	bool instant = true;
	int zoom = 0;
	int zoom_delta = 0;
	int scroll_x = 0;
	int scroll_y = 0;
	TileIndex tile = INVALID_TILE;
	try {
		instant = params.value("instant", true);
		if (has_zoom) zoom = params.value("zoom", 0);
		if (has_zoom_delta) zoom_delta = params.value("zoom_delta", 0);
		if (has_scroll) {
			if (!params["scroll"].is_object()) return Failure("invalid_params");
			scroll_x = params["scroll"].value("dx", 0);
			scroll_y = params["scroll"].value("dy", 0);
		}
		if (has_tile) {
			std::string_view error;
			if (!ResolveTile(params, tile, error)) return Failure(error);
		}
	} catch (const Json::exception &) {
		return Failure("invalid_params");
	}

	Window *w = GetMainViewportWindow();
	if (w == nullptr) return Failure("viewport_unavailable");
	ViewportData &vp = *w->viewport;

	const int old_zoom = to_underlying(vp.zoom);
	const int old_scroll_x = vp.scrollpos_x;
	const int old_scroll_y = vp.scrollpos_y;

	/* Zoom through the normal UI zoom path, bounded by the user's zoom limits. */
	bool zoom_clamped = false;
	if (has_zoom || has_zoom_delta) {
		const int min_zoom = to_underlying(std::max(_settings_client.gui.zoom_min, ZoomLevel::Min));
		const int max_zoom = to_underlying(std::min(_settings_client.gui.zoom_max, ZoomLevel::Max));
		const int wanted = has_zoom ? zoom : old_zoom + zoom_delta;
		const int target = Clamp(wanted, min_zoom, max_zoom);
		zoom_clamped = target != wanted;
		while (to_underlying(vp.zoom) > target) {
			if (!DoZoomInOutWindow(ZOOM_IN, w)) break;
		}
		while (to_underlying(vp.zoom) < target) {
			if (!DoZoomInOutWindow(ZOOM_OUT, w)) break;
		}
	}

	/* Move through the normal UI scroll path; the requested centre is validated above and the
	 * viewport update clamps the destination to the map. */
	if (has_tile) {
		ScrollMainWindowToTile(tile, instant);
	} else if (has_scroll) {
		const int center_x = vp.virtual_left + vp.virtual_width / 2;
		const int center_y = vp.virtual_top + vp.virtual_height / 2;
		const int max_x = static_cast<int>(Map::SizeX()) * static_cast<int>(TILE_SIZE) - 1;
		const int max_y = static_cast<int>(Map::SizeY()) * static_cast<int>(TILE_SIZE) - 1;
		const int x = Clamp(center_x + ScaleByZoom(scroll_x, vp.zoom), 0, max_x);
		const int y = Clamp(center_y + ScaleByZoom(scroll_y, vp.zoom), 0, max_y);
		ScrollMainWindowTo(x, y, -1, instant);
	}

	/* An instant move only sets the destination; apply it now, so the reported state and the
	 * projection are the state the next frame is drawn from. */
	if (instant) UpdateViewportPosition(w, 0);

	Json result = Json::object();
	result["ok"] = true;
	result["tick"] = TimerGameTick::counter;
	result["changed"] = to_underlying(vp.zoom) != old_zoom || vp.scrollpos_x != old_scroll_x || vp.scrollpos_y != old_scroll_y;
	result["instant"] = instant;
	result["zoom_clamped"] = zoom_clamped;
	result["coordinate_space"] = "screen_pixels";
	result["screen"] = {{"width", _screen.width}, {"height", _screen.height}};
	result["viewport"] = ViewportJson(vp);
	result["camera"] = CameraJson(vp);
	return result;
}

} // namespace InGameAgent
