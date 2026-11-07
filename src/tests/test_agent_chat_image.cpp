/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/**
 * @file test_agent_chat_image.cpp Test the screenshot previews of the agent chat transcript.
 *
 * The previews are pure data: a hash, a strict parser of the answer of the backend, a fit and a
 * bounded cache. These cases run without a window, a font, a blitter or a backend, and cover the
 * promises the player is given: that a preview is only shown when it is the exact artifact that
 * was asked for, that its pixels are the ones it hashes, that a capture keeps the world it was
 * taken in after a load, and that the cache never grows past its bounds or keeps a late answer.
 */

#include "../stdafx.h"

#include "../3rdparty/catch2/catch.hpp"

#include "../agent_chat_image.h"

#include "../3rdparty/fmt/format.h"

#include <cctype>
#include <string>
#include <string_view>
#include <vector>

#include "../safeguards.h"

using Json = nlohmann::json;

/**
 * Encode bytes as base64, so a test can build the answer the backend would send.
 * @param data The bytes to encode.
 * @return The base64 payload.
 */
static std::string EncodeBase64(std::string_view data)
{
	static const char * const digits = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
	std::string out;
	size_t i = 0;
	for (; i + 2 < data.size(); i += 3) {
		const uint32_t value = (static_cast<uint32_t>(static_cast<uint8_t>(data[i])) << 16) |
				(static_cast<uint32_t>(static_cast<uint8_t>(data[i + 1])) << 8) |
				static_cast<uint32_t>(static_cast<uint8_t>(data[i + 2]));
		out.push_back(digits[(value >> 18) & 63]);
		out.push_back(digits[(value >> 12) & 63]);
		out.push_back(digits[(value >> 6) & 63]);
		out.push_back(digits[value & 63]);
	}

	const size_t remaining = data.size() - i;
	if (remaining == 1) {
		const uint32_t value = static_cast<uint32_t>(static_cast<uint8_t>(data[i])) << 16;
		out.push_back(digits[(value >> 18) & 63]);
		out.push_back(digits[(value >> 12) & 63]);
		out += "==";
	} else if (remaining == 2) {
		const uint32_t value = (static_cast<uint32_t>(static_cast<uint8_t>(data[i])) << 16) |
				(static_cast<uint32_t>(static_cast<uint8_t>(data[i + 1])) << 8);
		out.push_back(digits[(value >> 18) & 63]);
		out.push_back(digits[(value >> 12) & 63]);
		out.push_back(digits[(value >> 6) & 63]);
		out += '=';
	}
	return out;
}

/**
 * Build a preview identity.
 * @param artifact The artifact id.
 * @param world The world the capture was taken in.
 * @param epoch The load epoch the capture was taken in.
 * @return The key.
 */
static AgentChatPreviewKey MakeKey(const std::string &artifact = "artifact-1", const std::string &world = "world-1", const std::string &epoch = "epoch-1")
{
	AgentChatPreviewKey key;
	key.world_id = world;
	key.epoch = epoch;
	key.chat_id = "chat-1";
	key.artifact_id = artifact;
	key.source_sha256 = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
	return key;
}

/**
 * Build pixels of a solid colour, so a hash of them can be computed.
 * @param width Width in pixels.
 * @param height Height in pixels.
 * @param value The byte written to every channel.
 * @return The RGBA8 pixels.
 */
static std::vector<uint8_t> MakePixels(int width, int height, uint8_t value)
{
	return std::vector<uint8_t>(static_cast<size_t>(width) * static_cast<size_t>(height) * 4, value);
}

/**
 * Build the answer the preview route sends for a key.
 * @param key The preview that was asked for.
 * @param pixels The pixels to report.
 * @return The JSON answer.
 */
static Json MakeResponse(const AgentChatPreviewKey &key, const std::vector<uint8_t> &pixels, int width, int height)
{
	const std::string_view bytes(reinterpret_cast<const char *>(pixels.data()), pixels.size());
	Json response = Json::object();
	response["artifact_id"] = key.artifact_id;
	response["chat_id"] = key.chat_id;
	response["world_id"] = key.world_id;
	response["epoch"] = key.epoch;
	response["source_sha256"] = key.source_sha256;
	response["source_width"] = 1920;
	response["source_height"] = 1080;
	response["width"] = width;
	response["height"] = height;
	response["encoding"] = "rgba8_base64";
	response["bytes"] = pixels.size();
	response["pixels_sha256"] = AgentChatSha256Hex(bytes);
	response["rgba8_base64"] = EncodeBase64(bytes);
	return response;
}

TEST_CASE("agent_chat_image: SHA-256 matches the known vectors")
{
	CHECK(AgentChatSha256Hex("") == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
	CHECK(AgentChatSha256Hex("abc") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
	CHECK(AgentChatSha256Hex("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq") ==
			"248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");

	/* More than one block, so the message schedule and the length padding are exercised. */
	const std::string million_a(1000000, 'a');
	CHECK(AgentChatSha256Hex(million_a) == "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
}

TEST_CASE("agent_chat_image: a preview is only accepted for the artifact it was asked for")
{
	const AgentChatPreviewKey key = MakeKey();
	const std::vector<uint8_t> pixels = MakePixels(2, 2, 0x40);
	const Json response = MakeResponse(key, pixels, 2, 2);

	std::string error;
	const std::optional<AgentChatPreviewPixels> parsed = ParseAgentChatPreview(response, key, &error);
	REQUIRE(parsed.has_value());
	CHECK(error.empty());
	CHECK(parsed->width == 2);
	CHECK(parsed->height == 2);
	CHECK(parsed->rgba == pixels);
	CHECK(parsed->source_sha256 == key.source_sha256);
	CHECK(parsed->pixel_sha256 == AgentChatSha256Hex(std::string_view(reinterpret_cast<const char *>(pixels.data()), pixels.size())));

	SECTION("another chat, world, epoch or artifact is refused") {
		Json other = response;
		other["chat_id"] = "chat-2";
		CHECK_FALSE(ParseAgentChatPreview(other, key, &error).has_value());

		other = response;
		other["world_id"] = "world-2";
		CHECK_FALSE(ParseAgentChatPreview(other, key, &error).has_value());

		other = response;
		other["epoch"] = "epoch-2";
		CHECK_FALSE(ParseAgentChatPreview(other, key, &error).has_value());

		other = response;
		other["artifact_id"] = "artifact-2";
		CHECK_FALSE(ParseAgentChatPreview(other, key, &error).has_value());
	}

	SECTION("another source image is refused, whatever the case of the hash") {
		Json other = response;
		other["source_sha256"] = std::string(64, 'f');
		CHECK_FALSE(ParseAgentChatPreview(other, key, &error).has_value());

		other["source_sha256"] = key.source_sha256;
		for (char &c : other["source_sha256"].get_ref<std::string &>()) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
		CHECK(ParseAgentChatPreview(other, key, &error).has_value());
	}

	SECTION("only bounded RGBA8 pixels are accepted") {
		Json other = response;
		other["encoding"] = "png_base64";
		CHECK_FALSE(ParseAgentChatPreview(other, key, &error).has_value());

		other = response;
		other["width"] = 0;
		CHECK_FALSE(ParseAgentChatPreview(other, key, &error).has_value());

		other = response;
		other["width"] = AGENT_CHAT_PREVIEW_MAX_WIDTH + 1;
		CHECK_FALSE(ParseAgentChatPreview(other, key, &error).has_value());

		other = response;
		other["height"] = AGENT_CHAT_PREVIEW_MAX_HEIGHT + 1;
		CHECK_FALSE(ParseAgentChatPreview(other, key, &error).has_value());

		other = response;
		other["width"] = 2.5;
		CHECK_FALSE(ParseAgentChatPreview(other, key, &error).has_value());

		other = response;
		other["bytes"] = pixels.size() - 4;
		CHECK_FALSE(ParseAgentChatPreview(other, key, &error).has_value());

		other = response;
		other["pixels_sha256"] = std::string(64, '0');
		CHECK_FALSE(ParseAgentChatPreview(other, key, &error).has_value());

		CHECK_FALSE(ParseAgentChatPreview(Json::array(), key, &error).has_value());
	}

	SECTION("the payload is decoded strictly") {
		Json other = response;
		other["rgba8_base64"] = " " + response["rgba8_base64"].get<std::string>();
		CHECK_FALSE(ParseAgentChatPreview(other, key, &error).has_value());

		other = response;
		std::string payload = response["rgba8_base64"].get<std::string>();
		payload.pop_back();
		other["rgba8_base64"] = payload;
		CHECK_FALSE(ParseAgentChatPreview(other, key, &error).has_value());

		other = response;
		other["rgba8_base64"] = "!!!!";
		CHECK_FALSE(ParseAgentChatPreview(other, key, &error).has_value());

		other = response;
		other["rgba8_base64"] = std::string();
		CHECK_FALSE(ParseAgentChatPreview(other, key, &error).has_value());
	}
}

TEST_CASE("agent_chat_image: the reserved box keeps the aspect ratio and never upscales")
{
	AgentChatPreviewSize size = FitAgentChatPreview(1920, 1080, AGENT_CHAT_PREVIEW_MAX_WIDTH, AGENT_CHAT_PREVIEW_MAX_HEIGHT);
	CHECK(size.width == 256);
	CHECK(size.height == 144);

	size = FitAgentChatPreview(1920, 1080, 128, AGENT_CHAT_PREVIEW_MAX_HEIGHT);
	CHECK(size.width == 128);
	CHECK(size.height == 72);

	size = FitAgentChatPreview(800, 600, AGENT_CHAT_PREVIEW_MAX_WIDTH, AGENT_CHAT_PREVIEW_MAX_HEIGHT);
	CHECK(size.width == 192);
	CHECK(size.height == 144);

	/* A capture smaller than the box is shown at its own size. */
	size = FitAgentChatPreview(64, 36, AGENT_CHAT_PREVIEW_MAX_WIDTH, AGENT_CHAT_PREVIEW_MAX_HEIGHT);
	CHECK(size.width == 64);
	CHECK(size.height == 36);

	size = FitAgentChatPreview(0, 36, 256, 144);
	CHECK(size.width == 0);
	CHECK(size.height == 0);
	size = FitAgentChatPreview(64, 36, 0, 144);
	CHECK(size.width == 0);

	/* The layout reserves exactly that box, at the left edge of the message content. */
	Rect rect = LayoutAgentChatPreview(Rect{0, 100, 299, 499}, 110, 1920, 1080);
	CHECK(rect.left == 0);
	CHECK(rect.top == 110);
	CHECK(rect.right == 255);
	CHECK(rect.bottom == 253);

	/* A narrow transcript clips the box instead of overflowing it. */
	rect = LayoutAgentChatPreview(Rect{10, 0, 109, 499}, 20, 1920, 1080);
	CHECK(rect.left == 10);
	CHECK(rect.top == 20);
	CHECK(rect.right == 109);
	CHECK(rect.bottom == 75);
}

TEST_CASE("agent_chat_image: the cache is bounded and never keeps a late answer")
{
	const AgentChatPreviewKey key = MakeKey();
	AgentChatPreviewPixels pixels;
	pixels.width = 2;
	pixels.height = 2;
	pixels.rgba = MakePixels(2, 2, 0x20);
	pixels.source_sha256 = key.source_sha256;
	pixels.pixel_sha256 = AgentChatSha256Hex(std::string_view(reinterpret_cast<const char *>(pixels.rgba.data()), pixels.rgba.size()));

	SECTION("one request per artifact at a time") {
		AgentChatPreviewCache cache;
		cache.BindWorld("world-1", "epoch-1");

		CHECK(cache.BeginLoad(key));
		CHECK(cache.InflightCount() == 1);
		CHECK_FALSE(cache.BeginLoad(key));
		CHECK(cache.Inflight(key));

		cache.EndLoad(key);
		CHECK(cache.InflightCount() == 0);
		CHECK(cache.BeginLoad(key));

		REQUIRE(cache.Store(cache.Generation(), key, pixels));
		CHECK(cache.Size() == 1);
		CHECK(cache.Bytes() == pixels.rgba.size());
		CHECK(cache.InflightCount() == 0);
		CHECK(cache.Find(key) != nullptr);
		CHECK_FALSE(cache.BeginLoad(key));
	}

	SECTION("an answer of another generation is dropped") {
		AgentChatPreviewCache cache;
		cache.BindWorld("world-1", "epoch-1");
		const uint64_t generation = cache.Generation();

		cache.BindWorld("world-2", "epoch-2");
		CHECK(cache.Generation() != generation);
		CHECK_FALSE(cache.Store(generation, key, pixels));
		CHECK(cache.Find(key) == nullptr);

		CHECK(cache.BeginLoad(key));
		CHECK_FALSE(cache.Fail(generation, key, "too late"));
		CHECK(cache.Failure(key) == nullptr);

		/* The same artifact id in the new world is another preview, so it can be requested. */
		REQUIRE(cache.Store(cache.Generation(), key, pixels));
		CHECK(cache.Find(key) != nullptr);
	}

	SECTION("a capture of an older world stays viewable after a load") {
		AgentChatPreviewCache cache;
		cache.BindWorld("world-1", "epoch-1");
		const uint64_t old_generation = cache.Generation();

		/* The game loads: the cache now belongs to the new world and drops what it had. */
		cache.BindWorld("world-2", "epoch-2");
		CHECK(cache.Find(key) == nullptr);
		CHECK_FALSE(cache.Store(old_generation, key, pixels));

		/* The historical capture keeps its own identity, so a fresh request in the new epoch
		 * stores it under that identity and the history stays viewable. */
		CHECK(cache.BeginLoad(key));
		REQUIRE(cache.Store(cache.Generation(), key, pixels));
		CHECK(cache.Find(key) != nullptr);
		CHECK(cache.Find(MakeKey("artifact-1", "world-2", "epoch-2")) == nullptr);
	}

	SECTION("eviction keeps the bounds and the most recently used previews") {
		AgentChatPreviewCache cache(2, 1024 * 1024);
		cache.BindWorld("world-1", "epoch-1");

		const AgentChatPreviewKey first = MakeKey("artifact-1");
		const AgentChatPreviewKey second = MakeKey("artifact-2");
		const AgentChatPreviewKey third = MakeKey("artifact-3");
		REQUIRE(cache.Store(cache.Generation(), first, pixels));
		REQUIRE(cache.Store(cache.Generation(), second, pixels));
		CHECK(cache.Size() == 2);

		/* Using the first one makes the second the least recently used. */
		REQUIRE(cache.Find(first) != nullptr);
		REQUIRE(cache.Store(cache.Generation(), third, pixels));
		CHECK(cache.Size() == 2);
		CHECK(cache.Find(first) != nullptr);
		CHECK(cache.Find(second) == nullptr);
		CHECK(cache.Find(third) != nullptr);
	}

	SECTION("the byte bound evicts as well") {
		/* Four 16 byte previews fit; the fifth pushes the oldest out. */
		AgentChatPreviewCache cache(8, 64);
		cache.BindWorld("world-1", "epoch-1");
		for (int i = 0; i < 5; i++) {
			REQUIRE(cache.Store(cache.Generation(), MakeKey(fmt::format("artifact-{}", i)), pixels));
		}
		CHECK(cache.Size() == 4);
		CHECK(cache.Bytes() <= 64);
		CHECK(cache.Find(MakeKey("artifact-0")) == nullptr);
	}

	SECTION("a cache that cannot hold the preview at all stores nothing") {
		AgentChatPreviewCache empty(0, 1024 * 1024);
		empty.BindWorld("world-1", "epoch-1");
		CHECK_FALSE(empty.Store(empty.Generation(), key, pixels));
		CHECK(empty.Size() == 0);

		AgentChatPreviewCache small(4, 8);
		small.BindWorld("world-1", "epoch-1");
		CHECK_FALSE(small.Store(small.Generation(), key, pixels));
		CHECK(small.Size() == 0);
		CHECK(small.Bytes() == 0);
	}

	SECTION("a failure is remembered and dropped with the world") {
		AgentChatPreviewCache cache;
		cache.BindWorld("world-1", "epoch-1");

		CHECK(cache.BeginLoad(key));
		REQUIRE(cache.Fail(cache.Generation(), key, "request failed"));
		CHECK(cache.Failure(key) != nullptr);
		CHECK(cache.InflightCount() == 0);
		CHECK(cache.Size() == 0);

		cache.BindWorld("world-2", "epoch-1");
		CHECK(cache.Failure(key) == nullptr);
	}
}

TEST_CASE("agent_chat_image: every sample is inside the pixels it scales from")
{
	/* The backend rounds a 5x288 capture to 2x144 pixels, while the transcript reserves 3x144 for
	 * it. The last display column must sample source column 1, not read the next row or past the
	 * end of the pixels. */
	CHECK(AgentChatPreviewSourceCoordinate(0, 3, 2) == 0);
	CHECK(AgentChatPreviewSourceCoordinate(1, 3, 2) == 0);
	CHECK(AgentChatPreviewSourceCoordinate(2, 3, 2) == 1);
	for (int y = 0; y < 144; y++) CHECK(AgentChatPreviewSourceCoordinate(y, 144, 144) == y);

	/* A 5 pixel wide source shown in 3 columns keeps every sample inside and the right edge on
	 * the last source column. */
	for (int x = 0; x < 3; x++) {
		const int source_x = AgentChatPreviewSourceCoordinate(x, 3, 5);
		CHECK(source_x >= 0);
		CHECK(source_x < 5);
	}
	CHECK(AgentChatPreviewSourceCoordinate(2, 3, 5) == 3);

	/* A clipped left edge only changes the offset; the bounds hold for every offset of the box. */
	for (int offset = 0; offset < 10; offset++) {
		const int source_x = AgentChatPreviewSourceCoordinate(offset, 10, 3);
		CHECK(source_x >= 0);
		CHECK(source_x < 3);
	}
	CHECK(AgentChatPreviewSourceCoordinate(9, 10, 3) == 2);

	/* Every sample of the whole 3x144 box lands inside a 2x144 buffer. */
	const int display_width = 3;
	const int display_height = 144;
	const int source_width = 2;
	const int source_height = 144;
	const size_t pixel_count = static_cast<size_t>(source_width) * source_height;
	for (int y = 0; y < display_height; y++) {
		for (int x = 0; x < display_width; x++) {
			const int source_x = AgentChatPreviewSourceCoordinate(x, display_width, source_width);
			const int source_y = AgentChatPreviewSourceCoordinate(y, display_height, source_height);
			const size_t index = static_cast<size_t>(source_y) * source_width + source_x;
			REQUIRE(index < pixel_count);
		}
	}

	/* Degenerate extents cannot produce an out of range sample either. */
	CHECK(AgentChatPreviewSourceCoordinate(0, 0, 4) == 0);
	CHECK(AgentChatPreviewSourceCoordinate(0, 4, 0) == 0);
	CHECK(AgentChatPreviewSourceCoordinate(100, 3, 2) == 1);
}
