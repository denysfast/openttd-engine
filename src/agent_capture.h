/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file agent_capture.h Capture of the live game frame for the in-game agent bridge. */

#ifndef AGENT_CAPTURE_H
#define AGENT_CAPTURE_H

#include "3rdparty/nlohmann/json.hpp"

/** Bridge between the running game and an external agent. */
namespace InGameAgent {

using Json = nlohmann::json;

/**
 * Capture the frame that the game is currently showing and return it as a PNG.
 *
 * The pixels come from the live screen buffer of the running game, encoded with the
 * regular PNG screenshot provider; nothing is synthesised. Before the copy the dirty
 * blocks are redrawn, exactly like a viewport screenshot does, so a capture requested
 * after a command shows the state that command produced instead of the frame from
 * before it. The image is written to a private temporary directory, read back, and the
 * directory is removed again, so no user file is ever touched.
 *
 * Must be called from the main thread: the thread that runs the video driver's draw
 * tick and owns the video buffer (the one that executes VideoDriver::QueueOnMainThread
 * callbacks), while that buffer is locked. A caller on the game thread has to wrap the
 * call in VideoDriver::GetInstance()->QueueOnMainThread(...). The project exposes no
 * public game-thread identity, so this is a caller contract and not a runtime check.
 *
 * Parameters (all optional):
 * - "kind":        "viewport" (default) for the whole screen frame, or "region" for a part of it.
 * - "region":      object with "left", "top", "width", "height" in screen pixels; required
 *                  for "region" and clamped to the frame that actually exists.
 * - "max_pixels":  upper bound on the number of captured pixels (default 16M, hard cap 64M).
 * - "max_bytes":   upper bound on the size of the encoded PNG (default 5MiB, clamped to the
 *                  largest PNG that still fits the backend's 8MiB request body after base64
 *                  and JSON overhead, about 5.9MiB).
 * - "include_png": whether to include the base64 PNG in the result (default true).
 *
 * On success the result contains:
 * - "ok": true, "kind", "width", "height", "tick" (the game tick the pixels were rendered
 *   from), "camera" (left/top/width/height of the captured screen rectangle), "frame"
 *   (redrawn/tick), "hash" (MD5 of the PNG bytes), "hash_algorithm", "parent_hash" (hash of
 *   the previous capture in this process, or null), "png_bytes" and, unless disabled,
 *   "png_base64". The backend derives its own SHA-256 from the decoded bytes.
 *
 * On failure the result contains "ok": false, "error" and "tick". Possible errors are
 * "headless_unavailable" (no video driver, blitter or frame buffer), "invalid_params",
 * "too_large" (with "png_bytes"/"max_bytes" or "pixels"/"max_pixels"), "stale_frame" (the
 * game advanced while the frame was copied, so the pixels are not the state of the reported
 * tick), "png_unavailable", "temp_dir_unavailable" and "encode_failed".
 *
 * @param params The request parameters.
 * @return The JSON result describing the capture.
 */
Json CaptureGameFrame(const Json &params);

} // namespace InGameAgent

#endif /* AGENT_CAPTURE_H */
