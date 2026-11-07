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
#include "gfx_func.h"
#include "palette_func.h"
#include "screenshot_gui.h"
#include "screenshot_type.h"
#include "string_func.h"
#include "timer/timer_game_tick.h"
#include "video/video_driver.hpp"
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

} // namespace InGameAgent
