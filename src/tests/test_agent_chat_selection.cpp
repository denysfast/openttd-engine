/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/gpl-2.0>.
 */

/**
 * @file test_agent_chat_selection.cpp Test the selection of agent chat transcript text.
 *
 * The selection is a pair of byte offsets in the cached transcript, so these cases run without a
 * window, a font or a backend: they cover the promises the player is given, i.e. that a drag
 * selects the glyphs that are on screen, that copying returns the text exactly as it was rendered
 * (Cyrillic, emoji, newlines and all), that a reversed drag selects the same text, and that a
 * selection never survives a chat or world switch as a stale range of somebody else's text.
 *
 * The expected character boundaries are never derived from a fixed character width: they are
 * decoded from the text by #Utf8Boundaries, so a text that mixes one byte ASCII, two byte
 * Cyrillic and four byte emoji characters is checked against what its bytes really are.
 */

#include "../stdafx.h"

#include "../3rdparty/catch2/catch.hpp"

#include "../agent_chat_selection.h"
#include "../core/utf8.hpp"

#include <algorithm>
#include <string>
#include <string_view>
#include <vector>

#include "../safeguards.h"

/**
 * A stand-in for the font: every code point is ten pixels wide, whatever the number of bytes it
 * occupies. That is what a monospace font does, and it makes a byte count and a pixel count two
 * different things, which is exactly the confusion the hit testing has to survive.
 * @param text The text to measure.
 * @return The width in pixels.
 */
static int MeasureMonospace(std::string_view text)
{
	int width = 0;
	Utf8View view(text);
	for (auto it = view.begin(); it != view.end(); ++it) width += 10;
	return width;
}

/**
 * The UTF-8 character boundaries of a text, decoded from the text itself.
 *
 * This is the independent reference the selection helpers are checked against: the offset of
 * every code point the iterator finds, followed by the end of the text. Nothing here assumes a
 * character width, so ASCII, Cyrillic and emoji are all measured by decoding them.
 * @param text The text to decode.
 * @return The byte offsets that are character boundaries, in increasing order.
 */
static std::vector<size_t> Utf8Boundaries(std::string_view text)
{
	std::vector<size_t> boundaries;
	Utf8View view(text);
	for (auto it = view.begin(); it != view.end(); ++it) boundaries.push_back(it.GetByteOffset());
	boundaries.push_back(text.size());
	return boundaries;
}

/**
 * The greatest boundary at or before an offset.
 * @param boundaries The boundaries of the text, in increasing order.
 * @param offset The offset to look up.
 * @return The boundary.
 */
static size_t BoundaryAtOrBefore(const std::vector<size_t> &boundaries, size_t offset)
{
	size_t result = 0;
	for (const size_t boundary : boundaries) {
		if (boundary > offset) break;
		result = boundary;
	}
	return result;
}

/**
 * The smallest boundary after an offset.
 * @param boundaries The boundaries of the text, in increasing order.
 * @param offset The offset to look up.
 * @return The boundary, or the end of the text when there is none.
 */
static size_t BoundaryAfter(const std::vector<size_t> &boundaries, size_t offset)
{
	for (const size_t boundary : boundaries) {
		if (boundary > offset) return boundary;
	}
	return boundaries.back();
}

/**
 * The greatest boundary before an offset.
 * @param boundaries The boundaries of the text, in increasing order.
 * @param offset The offset to look up.
 * @return The boundary, or 0 when there is none.
 */
static size_t BoundaryBefore(const std::vector<size_t> &boundaries, size_t offset)
{
	size_t result = 0;
	for (const size_t boundary : boundaries) {
		if (boundary >= offset) break;
		result = boundary;
	}
	return result;
}

/**
 * Count the code points of a text by decoding it.
 * @param text The text.
 * @return The number of code points.
 */
static size_t CountCodePoints(std::string_view text)
{
	size_t count = 0;
	Utf8View view(text);
	for (auto it = view.begin(); it != view.end(); ++it) count++;
	return count;
}

/** A text of two byte characters only. */
static const std::string CYRILLIC = "Привет, мир";

/** Texts whose characters are not all the same number of bytes. */
static const std::vector<std::string> MIXED_TEXTS = {
	"hello world",                       // one byte characters only
	"Привет, мир",                       // two byte Cyrillic next to one byte ", "
	"Привет, world",                     // the two kinds in one line
	"ok \xF0\x9F\x99\x82 done",          // a four byte emoji between ASCII
	"строка\nвторая \xF0\x9F\x98\x80",   // a newline and an emoji in Cyrillic text
	"",
};

TEST_CASE("agent_chat_selection: an offset never cuts a character")
{
	for (const std::string &sample : MIXED_TEXTS) {
		const std::string_view text = sample;
		const std::vector<size_t> boundaries = Utf8Boundaries(text);

		REQUIRE(boundaries.front() == 0);
		REQUIRE(boundaries.back() == text.size());

		/* The reference list itself is strictly increasing and holds real boundaries. */
		for (size_t i = 0; i < boundaries.size(); i++) {
			CHECK(boundaries[i] <= text.size());
			CHECK((boundaries[i] == text.size() || !IsUtf8Part(text[boundaries[i]])));
			if (i > 0) CHECK(boundaries[i - 1] < boundaries[i]);
		}

		/* Every offset of the text is mapped onto the boundary list, whatever the byte lengths. */
		for (size_t offset = 0; offset <= text.size(); offset++) {
			CHECK(SnapUtf8Boundary(text, offset) == BoundaryAtOrBefore(boundaries, offset));
			CHECK(NextUtf8Boundary(text, offset) == BoundaryAfter(boundaries, offset));
			CHECK(PrevUtf8Boundary(text, offset) == BoundaryBefore(boundaries, offset));
		}

		/* An offset beyond the end is clamped to the end. */
		CHECK(SnapUtf8Boundary(text, text.size() + 100) == text.size());
		CHECK(NextUtf8Boundary(text, text.size() + 100) == text.size());
		/* The boundary before the end of the text, i.e. the start of its last character. */
		CHECK(PrevUtf8Boundary(text, text.size() + 100) == BoundaryBefore(boundaries, text.size()));
	}

	/* The first characters of the Cyrillic text are two bytes each, so these offsets are the
	 * ones a drag produces on the rendered glyphs. */
	const std::string_view text = CYRILLIC;
	CHECK(SnapUtf8Boundary(text, 0) == 0);
	CHECK(SnapUtf8Boundary(text, 1) == 0);
	CHECK(SnapUtf8Boundary(text, 3) == 2);
	CHECK(SnapUtf8Boundary(text, text.size()) == text.size());
	CHECK(NextUtf8Boundary(text, 0) == 2);
	CHECK(NextUtf8Boundary(text, 1) == 2);
	CHECK(PrevUtf8Boundary(text, 2) == 0);
	CHECK(PrevUtf8Boundary(text, 3) == 2);
	CHECK(PrevUtf8Boundary(text, 0) == 0);

	/* The ASCII characters of the same text are one byte each. */
	const std::vector<size_t> boundaries = Utf8Boundaries(text);
	const size_t comma = text.find(',');
	REQUIRE(comma != std::string_view::npos);
	CHECK(BoundaryAtOrBefore(boundaries, comma) == comma);
	CHECK(SnapUtf8Boundary(text, comma) == comma);
	CHECK(SnapUtf8Boundary(text, comma + 1) == comma + 1);
	CHECK(NextUtf8Boundary(text, comma) == comma + 1);

	/* A plain ASCII text has a boundary on every byte. */
	CHECK(NextUtf8Boundary("abc", 0) == 1);
	CHECK(PrevUtf8Boundary("abc", 3) == 2);
	CHECK(SnapUtf8Boundary("abc", 2) == 2);
	CHECK(SnapUtf8Boundary("", 5) == 0);
}

TEST_CASE("agent_chat_selection: hit testing lands on the glyph under the pointer")
{
	/* Ten pixels per character: the boundaries of "Привет" sit at 0, 10, 20, ... pixels. */
	const std::string_view cyrillic = "Привет";
	CHECK(OffsetForX(cyrillic, -20, MeasureMonospace) == 0);
	CHECK(OffsetForX(cyrillic, 0, MeasureMonospace) == 0);
	CHECK(OffsetForX(cyrillic, 4, MeasureMonospace) == 0);
	/* 6 is nearer to the end of the first character than to its start. */
	CHECK(OffsetForX(cyrillic, 6, MeasureMonospace) == 2);
	/* A tie is resolved to the earlier boundary, as a text cursor does. */
	CHECK(OffsetForX(cyrillic, 5, MeasureMonospace) == 0);
	CHECK(OffsetForX(cyrillic, 21, MeasureMonospace) == 4);
	CHECK(OffsetForX(cyrillic, 1000, MeasureMonospace) == cyrillic.size());
	CHECK(OffsetForX("", 40, MeasureMonospace) == 0);

	/* Whatever the position, the result is a character boundary of the line. */
	for (int x = -10; x <= 200; x += 3) {
		const size_t offset = OffsetForX(cyrillic, x, MeasureMonospace);
		REQUIRE(offset <= cyrillic.size());
		CHECK((offset == cyrillic.size() || !IsUtf8Part(cyrillic[offset])));
	}

	/* The inverse direction, used to draw the highlight, measures the same prefixes. */
	CHECK(WidthForOffset(cyrillic, 0, MeasureMonospace) == 0);
	CHECK(WidthForOffset(cyrillic, 2, MeasureMonospace) == 10);
	CHECK(WidthForOffset(cyrillic, 3, MeasureMonospace) == 10);
	CHECK(WidthForOffset(cyrillic, cyrillic.size(), MeasureMonospace) == 60);

	/* The same holds for a line that mixes one byte and four byte characters. */
	const std::string_view mixed = "a \xF0\x9F\x99\x82 b";
	const std::vector<size_t> boundaries = Utf8Boundaries(mixed);
	for (int x = -10; x <= 100; x += 1) {
		const size_t offset = OffsetForX(mixed, x, MeasureMonospace);
		CHECK(std::find(boundaries.begin(), boundaries.end(), offset) != boundaries.end());
	}
}

TEST_CASE("agent_chat_selection: a single message keeps every byte")
{
	const std::string text = "line one\nline two";
	const std::vector<std::string_view> texts = {text};
	AgentChatSelection selection;
	selection.BindChat("chat-a", "world-1", "epoch-1");

	/* Nothing selected: nothing to draw and nothing to copy. */
	CHECK(selection.Empty());
	CHECK(selection.GetText(texts).empty());

	/* A selection that ends before the newline copies the first line, without the newline. */
	selection.Start({0, 0});
	selection.Extend({0, 8});
	CHECK_FALSE(selection.Empty());
	CHECK(selection.GetText(texts) == "line one");
	CHECK(selection.GetText(texts) == text.substr(0, 8));

	/* The newline itself is part of the text and is copied as it is. */
	selection.Extend({0, 9});
	CHECK(selection.GetText(texts) == "line one\n");

	/* The whole multiline text, byte for byte. */
	selection.Extend({0, text.size()});
	CHECK(selection.GetText(texts) == text);
	CHECK(selection.GetText(texts).find('\n') == 8);

	/* Both ends on the same place is an empty selection again. */
	selection.Extend({0, 0});
	CHECK(selection.Empty());
	CHECK(selection.GetText(texts).empty());
}

TEST_CASE("agent_chat_selection: a reversed drag selects the same text")
{
	const std::string text = "Привет, мир";
	const std::vector<std::string_view> texts = {text};

	AgentChatSelection forward;
	forward.Start({0, 2});
	forward.Extend({0, 8});

	AgentChatSelection backward;
	backward.Start({0, 8});
	backward.Extend({0, 2});

	CHECK(forward.GetText(texts) == "рив");
	CHECK(backward.GetText(texts) == forward.GetText(texts));

	/* The two ends are reported in transcript order in both cases. */
	const auto [forward_from, forward_to] = forward.Ordered();
	const auto [backward_from, backward_to] = backward.Ordered();
	CHECK(forward_from.Same(backward_from));
	CHECK(forward_to.Same(backward_to));
	CHECK(forward_from.offset == 2);
	CHECK(forward_to.offset == 8);
}

TEST_CASE("agent_chat_selection: a drag across messages copies both of them")
{
	const std::string first = "hello world";
	const std::string second = "second line";
	const std::string third = "third";
	const std::vector<std::string_view> texts = {first, second, third};

	AgentChatSelection selection;
	selection.BindChat("chat-a", "world-1", "epoch-1");
	selection.Start({0, 6});
	selection.Extend({2, 5});

	/* The messages in between are copied whole, and the messages themselves are separated by a
	 * newline because they are rendered one under the other. */
	CHECK(selection.GetText(texts) == "world\nsecond line\nthird");

	/* The part of each message the selection covers, which is what the highlight draws. */
	const auto range_first = selection.RangeIn(0, first.size());
	REQUIRE(range_first.has_value());
	CHECK(range_first->first == 6);
	CHECK(range_first->second == first.size());

	const auto range_second = selection.RangeIn(1, second.size());
	REQUIRE(range_second.has_value());
	CHECK(range_second->first == 0);
	CHECK(range_second->second == second.size());

	const auto range_third = selection.RangeIn(2, third.size());
	REQUIRE(range_third.has_value());
	CHECK(range_third->first == 0);
	CHECK(range_third->second == 5);

	/* A message the selection does not touch has nothing to highlight. */
	CHECK_FALSE(selection.RangeIn(3, 10).has_value());
	/* A transcript that lost the last message keeps the range that is still inside it. */
	CHECK(selection.GetText({first}) == "world");
}

TEST_CASE("agent_chat_selection: a selection belongs to its chat")
{
	const std::vector<std::string_view> texts_a = {"text of chat a"};
	const std::vector<std::string_view> texts_b = {"text of chat b"};

	AgentChatSelection selection;
	selection.BindChat("chat-a", "world-1", "epoch-1");
	selection.Start({0, 0});
	selection.Extend({0, 4});
	CHECK(selection.GetText(texts_a) == "text");
	CHECK(selection.ChatId() == "chat-a");

	/* Binding the same chat again keeps the selection, e.g. when the chat list is reloaded. */
	selection.BindChat("chat-a", "world-1", "epoch-1");
	CHECK_FALSE(selection.Empty());
	CHECK(selection.GetText(texts_a) == "text");

	/* Another chat has another transcript: the selection of the previous one is dropped, so its
	 * offsets can never be copied out of the text of the new chat. */
	selection.BindChat("chat-b", "world-1", "epoch-1");
	CHECK(selection.Empty());
	CHECK(selection.GetText(texts_b).empty());
	CHECK(selection.ChatId() == "chat-b");
}

TEST_CASE("agent_chat_selection: the same chat in another world or epoch is another transcript")
{
	const std::vector<std::string_view> texts = {"Привет"};

	AgentChatSelection selection;
	selection.BindChat("chat-a", "world-1", "epoch-1");
	selection.Start({0, 0});
	selection.Extend({0, 6});
	REQUIRE(selection.GetText(texts) == "При");

	/* The same world and epoch, e.g. a reload of the same chat: the selection is kept. */
	selection.BindChat("chat-a", "world-1", "epoch-1");
	CHECK_FALSE(selection.Empty());
	CHECK(selection.GetText(texts) == "При");

	/* The same chat id in another world is another transcript, so its offsets are dropped. */
	selection.BindChat("chat-a", "world-2", "epoch-1");
	CHECK(selection.Empty());
	CHECK(selection.GetText(texts).empty());

	/* The same chat id in another epoch of the same world as well. Offset 4 is the end of the
	 * second two byte character, so the selection covers "Пр". */
	selection.Start({0, 0});
	selection.Extend({0, 4});
	REQUIRE(selection.GetText(texts) == "Пр");
	selection.BindChat("chat-a", "world-2", "epoch-2");
	CHECK(selection.Empty());
	CHECK(selection.GetText(texts).empty());
}

TEST_CASE("agent_chat_selection: a world reset drops the selection")
{
	const std::vector<std::string_view> texts = {"Привет"};

	AgentChatSelection selection;
	selection.BindChat("chat-a", "world-1", "epoch-1");
	selection.Start({0, 0});
	selection.Extend({0, 6});
	REQUIRE(selection.GetText(texts) == "При");

	/* Leaving the world clears the selection, as the chat window does; the same chat of the new
	 * world cannot resurrect the old range. */
	selection.Clear();
	CHECK(selection.Empty());
	CHECK(selection.GetText(texts).empty());

	selection.BindChat("chat-a", "world-1", "epoch-1");
	CHECK(selection.Empty());
	CHECK(selection.GetText(texts).empty());

	/* Binding no chat at all also drops a selection of a chat that is gone. */
	selection.Start({0, 0});
	selection.Extend({0, 2});
	selection.BindChat("", "world-1", "epoch-1");
	CHECK(selection.Empty());
	CHECK(selection.GetText(texts).empty());
}

TEST_CASE("agent_chat_selection: a reloaded transcript keeps the ends inside it")
{
	AgentChatSelection selection;
	selection.BindChat("chat-a", "world-1", "epoch-1");
	selection.Start({0, 6});
	selection.Extend({0, 9});

	/* The message was reloaded unchanged: nothing moves, and the offset that was in the middle of
	 * a character is snapped onto a boundary, so a copy cannot cut it. */
	const std::vector<std::string_view> unchanged = {"Привет мир"};
	selection.Clamp(unchanged);
	CHECK(selection.anchor.offset == 6);
	CHECK(selection.cursor.offset == 8);
	CHECK(selection.GetText(unchanged) == "в");

	/* The message became shorter: both ends stay inside the new text. */
	const std::vector<std::string_view> shorter = {"При"};
	selection.Clamp(shorter);
	CHECK(selection.anchor.offset <= shorter[0].size());
	CHECK(selection.cursor.offset <= shorter[0].size());
	CHECK(selection.GetText(shorter).empty());

	/* The transcript lost the message the selection was made in. */
	selection.Clamp({});
	CHECK(selection.Empty());
}

TEST_CASE("agent_chat_selection: every selected range is valid UTF-8")
{
	for (const std::string &sample : MIXED_TEXTS) {
		const std::string_view text = sample;
		const std::vector<size_t> boundaries = Utf8Boundaries(text);

		for (const size_t begin : boundaries) {
			for (const size_t end : boundaries) {
				if (end < begin) continue;

				AgentChatSelection selection;
				selection.BindChat("chat-a", "world-1", "epoch-1");
				selection.Start({0, begin});
				selection.Extend({0, end});

				const std::string copied = selection.GetText({text});
				const std::string_view expected = text.substr(begin, end - begin);

				/* Byte for byte the range of the message, so no character is cut. */
				CHECK(copied == expected);
				/* The same number of code points as that range of the source. */
				CHECK(CountCodePoints(copied) == CountCodePoints(expected));
				/* And nothing was replaced by the invalid encoding placeholder. */
				CHECK(copied.find("\xEF\xBF\xBD") == std::string::npos);
				/* Both ends of the copy are boundaries of the source text. */
				CHECK(std::find(boundaries.begin(), boundaries.end(), begin) != boundaries.end());
				CHECK(std::find(boundaries.begin(), boundaries.end(), end) != boundaries.end());
			}
		}
	}

	/* A selection across two messages of different character widths: the parts are copied
	 * verbatim and joined with one newline. */
	const std::string first = "Привет, мир";
	const std::string second = "ok \xF0\x9F\x99\x82";
	const std::vector<std::string_view> texts = {first, second};

	AgentChatSelection selection;
	selection.BindChat("chat-a", "world-1", "epoch-1");
	selection.Start({0, 6});
	selection.Extend({1, second.size()});
	const std::string copied = selection.GetText(texts);
	CHECK(copied == first.substr(6) + "\n" + second);
	CHECK(CountCodePoints(copied) == CountCodePoints(first.substr(6)) + 1 + CountCodePoints(second));
	CHECK(copied.find("\xEF\xBF\xBD") == std::string::npos);
}
