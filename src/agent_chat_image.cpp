/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file agent_chat_image.cpp Decode, cache, lay out and draw screenshot previews of the transcript. */

#include "stdafx.h"
#include "agent_chat_image.h"

#include "blitter/base.hpp"
#include "blitter/factory.hpp"
#include "gfx_func.h"
#include "palette_func.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <utility>

#include "safeguards.h"

/* --- SHA-256 --- */

namespace {

/** One round constant of SHA-256. */
constexpr uint32_t SHA256_K[64] = {
	0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
	0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
	0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
	0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
	0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
	0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
	0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
	0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
};

/**
 * Rotate a 32 bit value to the right.
 * @param value The value to rotate.
 * @param count Number of bits to rotate by.
 * @return The rotated value.
 */
inline uint32_t Rotr(uint32_t value, int count)
{
	return (value >> count) | (value << (32 - count));
}

/**
 * Mix one 64 byte block into the hash state.
 * @param block The block to mix.
 * @param state The eight words of the hash state, updated in place.
 */
void Sha256Block(const uint8_t *block, uint32_t *state)
{
	uint32_t w[64];
	for (int i = 0; i < 16; i++) {
		w[i] = (static_cast<uint32_t>(block[i * 4]) << 24) | (static_cast<uint32_t>(block[i * 4 + 1]) << 16) |
				(static_cast<uint32_t>(block[i * 4 + 2]) << 8) | static_cast<uint32_t>(block[i * 4 + 3]);
	}
	for (int i = 16; i < 64; i++) {
		const uint32_t s0 = Rotr(w[i - 15], 7) ^ Rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
		const uint32_t s1 = Rotr(w[i - 2], 17) ^ Rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
		w[i] = w[i - 16] + s0 + w[i - 7] + s1;
	}

	uint32_t a = state[0], b = state[1], c = state[2], d = state[3];
	uint32_t e = state[4], f = state[5], g = state[6], h = state[7];
	for (int i = 0; i < 64; i++) {
		const uint32_t s1 = Rotr(e, 6) ^ Rotr(e, 11) ^ Rotr(e, 25);
		const uint32_t ch = (e & f) ^ (~e & g);
		const uint32_t t1 = h + s1 + ch + SHA256_K[i] + w[i];
		const uint32_t s0 = Rotr(a, 2) ^ Rotr(a, 13) ^ Rotr(a, 22);
		const uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
		const uint32_t t2 = s0 + maj;
		h = g;
		g = f;
		f = e;
		e = d + t1;
		d = c;
		c = b;
		b = a;
		a = t1 + t2;
	}

	state[0] += a;
	state[1] += b;
	state[2] += c;
	state[3] += d;
	state[4] += e;
	state[5] += f;
	state[6] += g;
	state[7] += h;
}

/**
 * Compare two hexadecimal digests, ignoring the case of the letters.
 * @param a The first digest.
 * @param b The second digest.
 * @return \c true when both name the same bytes.
 */
bool SameHex(std::string_view a, std::string_view b)
{
	if (a.size() != b.size()) return false;
	for (size_t i = 0; i < a.size(); i++) {
		if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i]))) return false;
	}
	return true;
}

/**
 * Read a string member of a JSON object.
 * @param j The object.
 * @param key The member name.
 * @return The string, or an empty view when the member is absent or not a string.
 */
std::string_view JsonStringField(const nlohmann::json &j, const char *key)
{
	const auto it = j.find(key);
	if (it == j.end() || !it->is_string()) return {};
	return it->get_ref<const std::string &>();
}

/**
 * Read an integer member of a JSON object.
 * @param j The object.
 * @param key The member name.
 * @return The integer, or nothing when the member is absent or not an integer.
 */
std::optional<int64_t> JsonIntegerField(const nlohmann::json &j, const char *key)
{
	const auto it = j.find(key);
	if (it == j.end()) return std::nullopt;
	if (it->is_number_unsigned()) {
		const uint64_t value = it->get<uint64_t>();
		if (value > static_cast<uint64_t>(INT64_MAX)) return std::nullopt;
		return static_cast<int64_t>(value);
	}
	if (!it->is_number_integer()) return std::nullopt;
	return it->get<int64_t>();
}

/**
 * The value of one base64 character.
 * @param c The character.
 * @return The six bits it carries, or -1 when it is not a base64 character.
 */
int Base64Value(char c)
{
	if (c >= 'A' && c <= 'Z') return c - 'A';
	if (c >= 'a' && c <= 'z') return c - 'a' + 26;
	if (c >= '0' && c <= '9') return c - '0' + 52;
	if (c == '+') return 62;
	if (c == '/') return 63;
	return -1;
}

/**
 * Decode a base64 payload strictly: no whitespace, no padding in the middle, and no more bytes
 * than the caller expects.
 * @param text The payload.
 * @param max_bytes The largest decoded size that is accepted.
 * @return The decoded bytes, or nothing when the payload is not canonical base64 of that size.
 */
std::optional<std::vector<uint8_t>> DecodeBase64(std::string_view text, size_t max_bytes)
{
	if (text.empty() || text.size() % 4 != 0) return std::nullopt;

	size_t padding = 0;
	if (text.size() >= 2 && text.substr(text.size() - 2) == "==") {
		padding = 2;
	} else if (text.back() == '=') {
		padding = 1;
	}
	const size_t decoded_size = text.size() / 4 * 3 - padding;
	if (decoded_size > max_bytes) return std::nullopt;

	std::vector<uint8_t> result;
	result.reserve(decoded_size);
	for (size_t i = 0; i < text.size(); i += 4) {
		int values[4] = {0, 0, 0, 0};
		for (int k = 0; k < 4; k++) {
			const char c = text[i + k];
			if (c != '=') {
				values[k] = Base64Value(c);
				if (values[k] < 0) return std::nullopt;
				continue;
			}
			/* Padding may only end the payload, as the last one or two characters. */
			if (i + 4 != text.size() || k < 2) return std::nullopt;
			if (k == 2 && text[i + 3] != '=') return std::nullopt;
		}

		result.push_back(static_cast<uint8_t>((values[0] << 2) | (values[1] >> 4)));
		if (text[i + 2] != '=') result.push_back(static_cast<uint8_t>(((values[1] & 0x0F) << 4) | (values[2] >> 2)));
		if (text[i + 3] != '=') result.push_back(static_cast<uint8_t>(((values[2] & 0x03) << 6) | values[3]));
	}

	if (result.size() != decoded_size) return std::nullopt;
	return result;
}

/**
 * The palette index closest to an RGB colour, for an 8bpp screen.
 * @param r Red channel.
 * @param g Green channel.
 * @param b Blue channel.
 * @return The index of the nearest colour of the current palette.
 */
uint8_t NearestPaletteIndex(uint8_t r, uint8_t g, uint8_t b)
{
	uint best = 0;
	uint best_distance = UINT32_MAX;
	for (uint i = 0; i < 256; i++) {
		const Colour &colour = _cur_palette.palette[i];
		const int dr = static_cast<int>(colour.r) - r;
		const int dg = static_cast<int>(colour.g) - g;
		const int db = static_cast<int>(colour.b) - b;
		const uint distance = static_cast<uint>(dr * dr + dg * dg + db * db);
		if (distance < best_distance) {
			best_distance = distance;
			best = i;
			if (distance == 0) break;
		}
	}
	return static_cast<uint8_t>(best);
}

} // namespace

/* --- Hashing and parsing --- */

std::string AgentChatSha256Hex(std::string_view data)
{
	uint32_t state[8] = {
		0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
		0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19,
	};

	size_t offset = 0;
	for (; offset + 64 <= data.size(); offset += 64) {
		Sha256Block(reinterpret_cast<const uint8_t *>(data.data()) + offset, state);
	}

	/* The tail is the rest of the message, a single one bit and the length in bits, in one or
	 * two blocks; the buffer is zeroed, so the padding itself is already there. */
	uint8_t tail[128] = {};
	const size_t remaining = data.size() - offset;
	if (remaining > 0) std::copy_n(reinterpret_cast<const uint8_t *>(data.data() + offset), remaining, tail);
	tail[remaining] = 0x80;
	const size_t tail_size = remaining + 1 + 8 <= 64 ? 64 : 128;
	const uint64_t bits = static_cast<uint64_t>(data.size()) * 8;
	for (int i = 0; i < 8; i++) tail[tail_size - 1 - i] = static_cast<uint8_t>(bits >> (8 * i));

	Sha256Block(tail, state);
	if (tail_size == 128) Sha256Block(tail + 64, state);

	static const char * const digits = "0123456789abcdef";
	std::string hex;
	hex.reserve(64);
	for (const uint32_t word : state) {
		for (int shift = 28; shift >= 0; shift -= 4) hex.push_back(digits[(word >> shift) & 0x0F]);
	}
	return hex;
}

std::optional<AgentChatPreviewPixels> ParseAgentChatPreview(const nlohmann::json &response, const AgentChatPreviewKey &key, std::string *error)
{
	const auto refuse = [error](std::string_view why) -> std::optional<AgentChatPreviewPixels> {
		if (error != nullptr) *error = std::string(why);
		return std::nullopt;
	};

	if (!response.is_object()) return refuse("the preview answer is not an object");
	if (JsonStringField(response, "encoding") != "rgba8_base64") return refuse("the preview is not RGBA8");
	if (JsonStringField(response, "chat_id") != key.chat_id) return refuse("the preview is of another chat");
	if (JsonStringField(response, "world_id") != key.world_id) return refuse("the preview is of another world");
	if (JsonStringField(response, "epoch") != key.epoch) return refuse("the preview is of another epoch");
	if (JsonStringField(response, "artifact_id") != key.artifact_id) return refuse("the preview is of another artifact");
	if (!SameHex(JsonStringField(response, "source_sha256"), key.source_sha256)) return refuse("the preview is of another source image");

	const std::optional<int64_t> width = JsonIntegerField(response, "width");
	const std::optional<int64_t> height = JsonIntegerField(response, "height");
	if (!width.has_value() || !height.has_value()) return refuse("the preview has no pixel size");
	if (*width < 1 || *width > AGENT_CHAT_PREVIEW_MAX_WIDTH) return refuse("the preview is wider than the transcript allows");
	if (*height < 1 || *height > AGENT_CHAT_PREVIEW_MAX_HEIGHT) return refuse("the preview is higher than the transcript allows");

	const size_t expected_bytes = static_cast<size_t>(*width) * static_cast<size_t>(*height) * 4;
	const std::optional<int64_t> reported_bytes = JsonIntegerField(response, "bytes");
	if (!reported_bytes.has_value() || *reported_bytes < 0 || static_cast<size_t>(*reported_bytes) != expected_bytes) {
		return refuse("the preview byte count does not match its size");
	}

	const std::optional<std::vector<uint8_t>> rgba = DecodeBase64(JsonStringField(response, "rgba8_base64"), expected_bytes);
	if (!rgba.has_value() || rgba->size() != expected_bytes) return refuse("the preview pixels are not valid base64 of its size");

	const std::string pixel_sha256 = AgentChatSha256Hex(std::string_view(reinterpret_cast<const char *>(rgba->data()), rgba->size()));
	if (!SameHex(JsonStringField(response, "pixels_sha256"), pixel_sha256)) return refuse("the preview pixels are not the ones it hashes");

	AgentChatPreviewPixels pixels;
	pixels.source_sha256 = std::string(JsonStringField(response, "source_sha256"));
	pixels.pixel_sha256 = pixel_sha256;
	pixels.width = static_cast<int>(*width);
	pixels.height = static_cast<int>(*height);
	pixels.rgba = *rgba;
	return pixels;
}

/* --- Layout --- */

AgentChatPreviewSize FitAgentChatPreview(int source_width, int source_height, int max_width, int max_height)
{
	if (source_width <= 0 || source_height <= 0 || max_width <= 0 || max_height <= 0) return {};

	const double scale = std::min({static_cast<double>(max_width) / source_width,
			static_cast<double>(max_height) / source_height, 1.0});
	AgentChatPreviewSize size;
	size.width = std::max(1, static_cast<int>(std::lround(source_width * scale)));
	size.height = std::max(1, static_cast<int>(std::lround(source_height * scale)));
	return size;
}

Rect LayoutAgentChatPreview(const Rect &content, int top, int source_width, int source_height)
{
	const AgentChatPreviewSize size = FitAgentChatPreview(source_width, source_height,
			std::min(content.Width(), AGENT_CHAT_PREVIEW_MAX_WIDTH), AGENT_CHAT_PREVIEW_MAX_HEIGHT);
	if (size.width <= 0) return Rect{content.left, top, content.left - 1, top - 1};
	return Rect{content.left, top, content.left + size.width - 1, top + size.height - 1};
}

int AgentChatPreviewSourceCoordinate(int offset, int display_extent, int source_extent)
{
	if (source_extent <= 0 || display_extent <= 0) return 0;
	const int64_t scaled = static_cast<int64_t>(offset) * source_extent / display_extent;
	return static_cast<int>(std::clamp<int64_t>(scaled, 0, source_extent - 1));
}

/* --- Drawing --- */

bool IsAgentChatPreviewDrawable()
{
	const uint8_t depth = BlitterFactory::GetCurrentBlitter()->GetScreenDepth();
	return depth == 8 || depth == 32;
}

void DrawAgentChatPreviewPixels(const Rect &rect, const AgentChatPreviewPixels &pixels)
{
	if (pixels.width <= 0 || pixels.height <= 0) return;
	if (pixels.rgba.size() != static_cast<size_t>(pixels.width) * static_cast<size_t>(pixels.height) * 4) return;

	Blitter *blitter = BlitterFactory::GetCurrentBlitter();
	if (blitter == nullptr) return;
	const uint8_t depth = blitter->GetScreenDepth();
	if (depth != 8 && depth != 32) return;

	const DrawPixelInfo *dpi = _cur_dpi;
	if (dpi == nullptr || dpi->zoom != ZoomLevel::Min) return;

	/* Clip the preview to the region that is being drawn, exactly like GfxFillRect does. */
	const int left = std::max(rect.left, dpi->left);
	const int top = std::max(rect.top, dpi->top);
	const int right = std::min(rect.right, dpi->left + dpi->width - 1);
	const int bottom = std::min(rect.bottom, dpi->top + dpi->height - 1);
	if (left > right || top > bottom) return;

	const int width = right - left + 1;

	/* The image is scaled from its own pixels to the box it occupies: the display size the layout
	 * reserved can differ from the size of the pixels by a pixel, so every sample is derived from
	 * the display offset and clamped inside the pixels instead of being read at the same offset. */
	const int display_width = rect.Width();
	const int display_height = rect.Height();
	std::vector<int> sample_x(static_cast<size_t>(width));
	for (int x = 0; x < width; x++) {
		sample_x[x] = AgentChatPreviewSourceCoordinate(left - rect.left + x, display_width, pixels.width);
	}

	/* The animated blitters only accept the buffer of CopyFromBuffer on the main screen with the
	 * animation buffer enabled; that is exactly the layout it asserts. Anywhere else (an offscreen
	 * buffer, or a draw with the animation buffer switched off) the pixels are written directly,
	 * which is the same video format without an animation index to keep in sync. */
	const bool main_screen = dpi->dst_ptr == _screen.dst_ptr && !_screen_disable_anim;
	const bool anim_8bit = depth == 32 && main_screen && blitter->NeedsAnimationBuffer();
	const bool anim_16bit = depth == 32 && main_screen && !anim_8bit && blitter->UsePaletteAnimation() == Blitter::PaletteAnimation::Blitter;
	const size_t anim_bytes = anim_8bit ? 1 : (anim_16bit ? 2 : 0);
	const size_t pixel_bytes = depth == 8 ? 1 : 4;

	std::vector<uint8_t> row(static_cast<size_t>(width) * pixel_bytes + static_cast<size_t>(width) * anim_bytes, 0);
	for (int y = top; y <= bottom; y++) {
		const int source_y = AgentChatPreviewSourceCoordinate(y - rect.top, display_height, pixels.height);
		const uint8_t *source_row = pixels.rgba.data() + static_cast<size_t>(source_y) * static_cast<size_t>(pixels.width) * 4;
		void *dst = blitter->MoveTo(dpi->dst_ptr, left - dpi->left, y - dpi->top);

		if (depth == 8) {
			for (int x = 0; x < width; x++) {
				const uint8_t *source = source_row + static_cast<size_t>(sample_x[x]) * 4;
				row[x] = NearestPaletteIndex(source[0], source[1], source[2]);
			}
			blitter->CopyFromBuffer(dst, row.data(), width, 1);
			continue;
		}

		/* The capture is opaque; its own colours are the ones that are shown. */
		if (main_screen) {
			uint32_t *colours = reinterpret_cast<uint32_t *>(row.data());
			for (int x = 0; x < width; x++) {
				const uint8_t *source = source_row + static_cast<size_t>(sample_x[x]) * 4;
				colours[x] = Colour(source[0], source[1], source[2]).data;
			}
			blitter->CopyFromBuffer(dst, row.data(), width, 1);
		} else {
			Colour *colours = static_cast<Colour *>(dst);
			for (int x = 0; x < width; x++) {
				const uint8_t *source = source_row + static_cast<size_t>(sample_x[x]) * 4;
				colours[x] = Colour(source[0], source[1], source[2]);
			}
		}
	}
}

/* --- Cache --- */

AgentChatPreviewCache::AgentChatPreviewCache(size_t max_items, size_t max_bytes) :
		max_items(max_items), max_bytes(max_bytes)
{
}

void AgentChatPreviewCache::BindWorld(const std::string &world_id, const std::string &epoch)
{
	if (world_id == this->world_id && epoch == this->epoch) return;
	this->world_id = world_id;
	this->epoch = epoch;
	this->Clear();
}

void AgentChatPreviewCache::Clear()
{
	this->entries.clear();
	this->failures.clear();
	this->inflight.clear();
	this->bytes = 0;
	this->generation++;
}

const AgentChatPreviewPixels *AgentChatPreviewCache::Find(const AgentChatPreviewKey &key)
{
	for (Entry &entry : this->entries) {
		if (!(entry.key == key)) continue;
		entry.used = ++this->clock;
		return &entry.pixels;
	}
	return nullptr;
}

const std::string *AgentChatPreviewCache::Failure(const AgentChatPreviewKey &key) const
{
	const auto it = this->failures.find(key);
	return it != this->failures.end() ? &it->second : nullptr;
}

bool AgentChatPreviewCache::BeginLoad(const AgentChatPreviewKey &key)
{
	if (this->Find(key) != nullptr) return false;
	if (!this->inflight.insert(key).second) return false;
	return true;
}

void AgentChatPreviewCache::EndLoad(const AgentChatPreviewKey &key)
{
	this->inflight.erase(key);
}

bool AgentChatPreviewCache::Store(uint64_t generation, const AgentChatPreviewKey &key, AgentChatPreviewPixels pixels)
{
	if (generation != this->generation) return false;
	if (pixels.width <= 0 || pixels.height <= 0 ||
			pixels.rgba.size() != static_cast<size_t>(pixels.width) * static_cast<size_t>(pixels.height) * 4) {
		return false;
	}

	this->inflight.erase(key);
	this->failures.erase(key);

	/* A cache that cannot hold even one preview would only churn: refuse the entry instead of
	 * storing it and evicting it on the same call. */
	if (this->max_items == 0 || pixels.rgba.size() > this->max_bytes) return false;

	/* A preview of the same artifact replaces the one that is cached, e.g. after a retry. */
	for (Entry &entry : this->entries) {
		if (!(entry.key == key)) continue;
		this->bytes -= entry.pixels.rgba.size();
		this->bytes += pixels.rgba.size();
		entry.pixels = std::move(pixels);
		entry.used = ++this->clock;
		this->Evict();
		return true;
	}

	Entry entry;
	entry.key = key;
	entry.used = ++this->clock;
	this->bytes += pixels.rgba.size();
	entry.pixels = std::move(pixels);
	this->entries.push_back(std::move(entry));
	this->Evict();
	return true;
}

bool AgentChatPreviewCache::Fail(uint64_t generation, const AgentChatPreviewKey &key, std::string message)
{
	if (generation != this->generation) return false;
	this->inflight.erase(key);
	this->failures[key] = std::move(message);
	while (this->failures.size() > std::max<size_t>(this->max_items, 1)) this->failures.erase(this->failures.begin());
	return true;
}

void AgentChatPreviewCache::Evict()
{
	while (!this->entries.empty() &&
			(this->entries.size() > this->max_items || this->bytes > this->max_bytes)) {
		size_t oldest = 0;
		for (size_t i = 1; i < this->entries.size(); i++) {
			if (this->entries[i].used < this->entries[oldest].used) oldest = i;
		}
		this->bytes -= this->entries[oldest].pixels.rgba.size();
		this->entries.erase(this->entries.begin() + oldest);
	}
}
