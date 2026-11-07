/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file agent_chat_image.h Screenshot previews inside the agent chat transcript. */

#ifndef AGENT_CHAT_IMAGE_H
#define AGENT_CHAT_IMAGE_H

#include "core/geometry_type.hpp"

#include "3rdparty/nlohmann/json.hpp"

#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

/**
 * The largest preview box the backend serves, and the box the UI asks for.
 *
 * The transcript reserves exactly the fitted size of this box, so the layout of a message is the
 * size of the pixels that are drawn in it, not an estimate of them.
 */
static const int AGENT_CHAT_PREVIEW_MAX_WIDTH = 256;
static const int AGENT_CHAT_PREVIEW_MAX_HEIGHT = 144;

/** The pixel size a screenshot preview is reserved at. */
struct AgentChatPreviewSize {
	int width = 0;  ///< Width in pixels, 0 when there is nothing to reserve.
	int height = 0; ///< Height in pixels, 0 when there is nothing to reserve.
};

/**
 * The identity of one transcript screenshot.
 *
 * A preview is only ever shown for the artifact, source hash, chat, world and epoch it was asked
 * for: the same artifact id in another world or epoch is another image, and a source hash that
 * does not match the attachment means the pixels are not the ones the message refers to.
 */
struct AgentChatPreviewKey {
	std::string world_id;      ///< World of the game the preview belongs to.
	std::string epoch;         ///< Load epoch of the game the preview belongs to.
	std::string chat_id;       ///< Id of the chat the artifact belongs to.
	std::string artifact_id;   ///< Id of the artifact inside that chat.
	std::string source_sha256; ///< SHA-256 of the stored source image, from the attachment.

	/**
	 * Whether two keys name the same preview.
	 * @param other The key to compare with.
	 * @return \c true when every part of the identity is equal.
	 */
	bool operator==(const AgentChatPreviewKey &other) const
	{
		return this->world_id == other.world_id && this->epoch == other.epoch &&
				this->chat_id == other.chat_id && this->artifact_id == other.artifact_id &&
				this->source_sha256 == other.source_sha256;
	}

	/**
	 * Order keys, so they can be used in a set or a map.
	 * @param other The key to compare with.
	 * @return \c true when this key sorts before \a other.
	 */
	bool operator<(const AgentChatPreviewKey &other) const
	{
		if (this->world_id != other.world_id) return this->world_id < other.world_id;
		if (this->epoch != other.epoch) return this->epoch < other.epoch;
		if (this->chat_id != other.chat_id) return this->chat_id < other.chat_id;
		if (this->artifact_id != other.artifact_id) return this->artifact_id < other.artifact_id;
		return this->source_sha256 < other.source_sha256;
	}
};

/** The decoded RGBA8 pixels of one preview, with the hashes that prove where they came from. */
struct AgentChatPreviewPixels {
	std::string source_sha256; ///< SHA-256 of the source image the pixels were derived from.
	std::string pixel_sha256;  ///< SHA-256 of #rgba, as the backend reported it.
	int width = 0;             ///< Width of the pixels in #rgba.
	int height = 0;            ///< Height of the pixels in #rgba.
	std::vector<uint8_t> rgba; ///< Width * height RGBA8 pixels, row by row, no padding.
};

/**
 * The SHA-256 of a byte range, as lower case hexadecimal.
 * @param data The bytes to hash.
 * @return The 64 character hex digest.
 */
std::string AgentChatSha256Hex(std::string_view data);

/**
 * Read the answer of the preview route into pixels, refusing anything that is not exactly the
 * artifact that was asked for.
 *
 * The answer is bounded and checked, never trusted: the encoding has to be RGBA8, the chat, world,
 * epoch and artifact have to be the requested ones, the source hash has to be the hash the
 * attachment recorded, the dimensions have to be positive and at most 256x144, the byte count has
 * to be width * height * 4, the base64 payload has to decode strictly to exactly that many bytes,
 * and the reported pixel hash has to be the hash of the decoded pixels.
 * @param response The JSON answer of the backend.
 * @param key The preview that was asked for.
 * @param error Optional out-parameter with the reason when the answer is refused.
 * @return The pixels, or nothing when the answer is not the requested preview.
 */
std::optional<AgentChatPreviewPixels> ParseAgentChatPreview(const nlohmann::json &response, const AgentChatPreviewKey &key, std::string *error = nullptr);

/**
 * The size a preview of a source image is drawn at: the source aspect ratio inside the box,
 * rounded to whole pixels and never upscaled above the source itself.
 * @param source_width Width of the source image in pixels.
 * @param source_height Height of the source image in pixels.
 * @param max_width Width of the box in pixels.
 * @param max_height Height of the box in pixels.
 * @return The fitted size, 0x0 when there is nothing to fit.
 */
AgentChatPreviewSize FitAgentChatPreview(int source_width, int source_height, int max_width, int max_height);

/**
 * Lay out one preview inside a message.
 * @param content The content rectangle of the message.
 * @param top The screen y the preview starts at.
 * @param source_width Width of the source image in pixels.
 * @param source_height Height of the source image in pixels.
 * @return The rectangle of the preview, or an empty rectangle when it cannot be shown.
 */
Rect LayoutAgentChatPreview(const Rect &content, int top, int source_width, int source_height);

/**
 * The source pixel a display pixel of a preview samples.
 *
 * The preview is scaled with nearest neighbour, so a display column or row maps to the source
 * column or row it covers. The result is clamped to the pixels, so every sample is inside them
 * even when the rounded display size and the size of the pixels disagree by a pixel: a 5x288
 * capture arrives as 2x144 pixels, the transcript reserves 3x144 for it, and its last display
 * column must still sample source column 1 instead of reading the next row.
 * @param offset Offset in display pixels from the start of the image.
 * @param display_extent Size of the image on screen, in pixels.
 * @param source_extent Size of the pixels, in pixels.
 * @return The source coordinate, always in [0, source_extent).
 */
int AgentChatPreviewSourceCoordinate(int offset, int display_extent, int source_extent);

/**
 * Draw the pixels of a preview, clipped to the current draw region.
 *
 * The pixels are handed to the blitter in its own format: a 32bpp blitter receives the true
 * colours of the capture, an 8bpp blitter receives the nearest palette index of every pixel.
 * A driver whose screen is neither 8 nor 32 bits per pixel cannot show a screenshot; the caller
 * draws an explicit "unsupported" note for it instead of a picture.
 * @param rect The screen rectangle to fill; the image is scaled to it.
 * @param pixels The pixels to draw.
 */
void DrawAgentChatPreviewPixels(const Rect &rect, const AgentChatPreviewPixels &pixels);

/**
 * Whether the current video driver can show screenshot previews at all.
 * @return \c true when the screen is 8 or 32 bits per pixel.
 */
bool IsAgentChatPreviewDrawable();

/**
 * A bounded cache of decoded previews, with one request in flight per artifact.
 *
 * The cache belongs to the world and epoch it was bound to: switching either clears it and
 * invalidates every callback still in flight, so a late answer of the previous world is never
 * stored as if it were the current one. A failure is remembered per artifact, so a broken
 * preview is not retried on every paint.
 */
class AgentChatPreviewCache {
public:
	static const size_t DEFAULT_MAX_ITEMS = 32;               ///< Number of previews kept.
	static const size_t DEFAULT_MAX_BYTES = 8 * 1024 * 1024;  ///< Bytes of pixels kept.

	/**
	 * Create a cache.
	 * @param max_items Number of previews to keep.
	 * @param max_bytes Number of pixel bytes to keep.
	 */
	explicit AgentChatPreviewCache(size_t max_items = DEFAULT_MAX_ITEMS, size_t max_bytes = DEFAULT_MAX_BYTES);

	/**
	 * Bind the cache to a world and an epoch, dropping everything when either changed.
	 * @param world_id The world of the game.
	 * @param epoch The load epoch of the game.
	 */
	void BindWorld(const std::string &world_id, const std::string &epoch);

	/**
	 * The generation of the cache, bumped whenever it is cleared.
	 * A request captures it and hands it back when it completes, so a late answer is dropped.
	 * @return The current generation.
	 */
	uint64_t Generation() const { return this->generation; }

	/**
	 * Find the pixels of a preview and mark them as recently used.
	 * @param key The preview to look for.
	 * @return The pixels, or \c nullptr when they are not cached.
	 */
	const AgentChatPreviewPixels *Find(const AgentChatPreviewKey &key);

	/**
	 * The remembered failure of a preview, if it failed before.
	 * @param key The preview to look for.
	 * @return The failure, or \c nullptr when it did not fail.
	 */
	const std::string *Failure(const AgentChatPreviewKey &key) const;

	/**
	 * Mark a preview as being requested, unless it is cached or already in flight.
	 * @param key The preview to request.
	 * @return \c true when the caller now owns the request and must end it.
	 */
	bool BeginLoad(const AgentChatPreviewKey &key);

	/**
	 * Release the request of a preview.
	 * @param key The preview whose request ended.
	 */
	void EndLoad(const AgentChatPreviewKey &key);

	/**
	 * Whether a preview is being requested right now.
	 * @param key The preview to look for.
	 * @return \c true when a request is in flight.
	 */
	bool Inflight(const AgentChatPreviewKey &key) const { return this->inflight.find(key) != this->inflight.end(); }

	/**
	 * Store the pixels of a preview.
	 * @param generation The generation the request was started in.
	 * @param key The preview that was requested.
	 * @param pixels The decoded pixels.
	 * @return \c true when the pixels were stored, \c false when the answer was too late or the
	 *         entry does not fit the bounds of the cache at all.
	 */
	bool Store(uint64_t generation, const AgentChatPreviewKey &key, AgentChatPreviewPixels pixels);

	/**
	 * Remember that the preview of a key could not be loaded.
	 * @param generation The generation the request was started in.
	 * @param key The preview that failed.
	 * @param message A short reason, kept for diagnostics only.
	 * @return \c true when the failure was recorded, \c false when the answer was too late.
	 */
	bool Fail(uint64_t generation, const AgentChatPreviewKey &key, std::string message);

	/** Drop every cached preview, failure and request, and invalidate answers in flight. */
	void Clear();

	/** @return The number of cached previews. */
	size_t Size() const { return this->entries.size(); }
	/** @return The number of pixel bytes cached. */
	size_t Bytes() const { return this->bytes; }
	/** @return The number of requests in flight. */
	size_t InflightCount() const { return this->inflight.size(); }

private:
	/** One cached preview and when it was last used. */
	struct Entry {
		AgentChatPreviewKey key;      ///< The preview this entry holds.
		AgentChatPreviewPixels pixels; ///< The pixels.
		uint64_t used = 0;            ///< Use counter, the smallest is evicted first.
	};

	std::string world_id;                       ///< World the cache is bound to.
	std::string epoch;                          ///< Epoch the cache is bound to.
	uint64_t generation = 0;                    ///< Bumped on every clear.
	uint64_t clock = 0;                         ///< Use counter of the cache.
	std::vector<Entry> entries;                 ///< Cached previews, in insertion order.
	std::map<AgentChatPreviewKey, std::string> failures; ///< Previews that could not be loaded.
	std::set<AgentChatPreviewKey> inflight;     ///< Previews that are being requested.
	size_t max_items;                           ///< Number of previews to keep.
	size_t max_bytes;                           ///< Pixel bytes to keep.
	size_t bytes = 0;                           ///< Pixel bytes cached now.

	/** Evict the least recently used previews until the cache is within its bounds. */
	void Evict();
};

#endif /* AGENT_CHAT_IMAGE_H */
