/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file agent_chat_selection.h Selectable text of the agent chat transcript. */

#ifndef AGENT_CHAT_SELECTION_H
#define AGENT_CHAT_SELECTION_H

#include "core/utf8.hpp"

#include <cstddef>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

/** One rendered line of the transcript text of the agent chat. */
struct AgentChatTextLine {
	size_t message = 0; ///< Index of the message in the transcript.
	size_t begin = 0;   ///< Byte offset of the first byte of the line in the text of that message.
	size_t end = 0;     ///< Byte offset one past the last byte of the line, always a character boundary.
	int top = 0;        ///< Screen y of the top of the line.
	int leading = 0;    ///< Height of the line in pixels, as the layouter that draws it reports.
	int left = 0;       ///< Screen x where the text of the line starts.
	int width = 0;      ///< Rendered width of the line in pixels.
};

/** One end of a transcript selection: a byte offset in the text of one message. */
struct AgentChatSelectionPoint {
	size_t message = 0; ///< Index of the message in the transcript.
	size_t offset = 0;  ///< Byte offset in the text of that message, always a character boundary.

	/**
	 * Whether this point comes before another one in the transcript.
	 * @param other The point to compare with.
	 * @return \c true when this point is earlier in the transcript.
	 */
	bool Before(const AgentChatSelectionPoint &other) const
	{
		return this->message < other.message || (this->message == other.message && this->offset < other.offset);
	}

	/**
	 * Whether two points are the same place in the transcript.
	 * @param other The point to compare with.
	 * @return \c true when both points name the same byte of the same message.
	 */
	bool Same(const AgentChatSelectionPoint &other) const
	{
		return this->message == other.message && this->offset == other.offset;
	}
};

/**
 * The selected text of the agent chat transcript.
 *
 * A selection is a range between two byte offsets of the cached transcript, not a set of pixels:
 * a paint or a scroll only changes where the text is drawn, never what is selected. Every offset
 * is a UTF-8 character boundary of the message it belongs to, so a selection never cuts a
 * character, and the text of a message is copied out of the cache verbatim, which keeps
 * newlines, Cyrillic and every other byte exactly as it was rendered.
 *
 * The selection is bound to the transcript it was made in, i.e. to the chat in a world and an
 * epoch. Another chat, world or epoch has another transcript, so the selection is dropped when
 * the player switches and a stale range can never be copied out of a text it was not made in.
 */
struct AgentChatSelection {
	AgentChatSelectionPoint anchor{}; ///< Where the selection was started.
	AgentChatSelectionPoint cursor{}; ///< Where the selection currently ends.
	bool has_anchor = false;          ///< Whether #anchor and #cursor belong to a selection.
	std::string chat;                 ///< Id of the chat the selection was made in.
	std::string world;                ///< World the selection was made in.
	std::string epoch;                ///< Load epoch the selection was made in.

	/** Forget the selected range; the chat it belongs to is kept. */
	void Clear();

	/**
	 * Bind the selection to the transcript that is shown, dropping it when another one is shown.
	 *
	 * The chat id alone is not enough: a load can keep the same chat id in another world or
	 * epoch, where the cached transcript is a different one, so a change of either drops the
	 * selection as well.
	 * @param chat_id The id of the chat that is shown now.
	 * @param world_id The world of the game.
	 * @param epoch The load epoch of the game.
	 */
	void BindChat(const std::string &chat_id, const std::string &world_id, const std::string &epoch);

	/**
	 * The chat the selection was made in.
	 * @return The chat id, empty when no chat was selected yet.
	 */
	const std::string &ChatId() const { return this->chat; }

	/**
	 * Whether nothing is selected, i.e. whether there is no range to draw or to copy.
	 * @return \c true when the selection is empty.
	 */
	bool Empty() const { return !this->has_anchor || this->anchor.Same(this->cursor); }

	/**
	 * Start a selection at a point, replacing the previous one.
	 * @param point The point the drag starts at.
	 */
	void Start(const AgentChatSelectionPoint &point);

	/**
	 * Move the end of the selection to a point.
	 * @param point The point the drag currently ends at.
	 */
	void Extend(const AgentChatSelectionPoint &point);

	/**
	 * The two ends of the selection in transcript order, so a drag in either direction selects
	 * the same text.
	 * @return The earlier and the later end of the selection.
	 */
	std::pair<AgentChatSelectionPoint, AgentChatSelectionPoint> Ordered() const;

	/**
	 * The text of the selection, as it has to be copied.
	 *
	 * A message is copied from its cached text between the two offsets, so a partial message is
	 * copied verbatim; the messages between the two ends are copied whole. A newline separates
	 * the messages, because the messages themselves are rendered one under the other.
	 * @param texts The text of every message of the transcript, in transcript order.
	 * @return The selected text, empty when nothing is selected.
	 */
	std::string GetText(const std::vector<std::string_view> &texts) const;

	/**
	 * The byte range of the selection inside one message.
	 * @param message Index of the message in the transcript.
	 * @param length Length of the text of that message in bytes.
	 * @return The begin and end offset of the selected part of that message, or nothing when the
	 *         selection does not touch it.
	 */
	std::optional<std::pair<size_t, size_t>> RangeIn(size_t message, size_t length) const;

	/**
	 * Keep the selection inside a transcript, e.g. after its messages were reloaded.
	 * @param texts The text of every message of the transcript, in transcript order.
	 */
	void Clamp(const std::vector<std::string_view> &texts);
};

/** Returns the rendered width of a prefix of a line of text, in pixels. */
using AgentChatTextMeasure = std::function<int(std::string_view)>;

/**
 * Get the next UTF-8 character boundary after an offset.
 * @param text The text.
 * @param offset The offset to start at.
 * @return The next boundary, or the size of the text when there is none.
 */
size_t NextUtf8Boundary(std::string_view text, size_t offset);

/**
 * Get the UTF-8 character boundary before an offset.
 * @param text The text.
 * @param offset The offset to start at.
 * @return The previous boundary, or 0 when there is none.
 */
size_t PrevUtf8Boundary(std::string_view text, size_t offset);

/**
 * Snap an offset to the character boundary at or before it, so a byte in the middle of a
 * character never becomes the end of a selection.
 * @param text The text.
 * @param offset The offset to snap.
 * @return The boundary, clamped to the size of the text.
 */
size_t SnapUtf8Boundary(std::string_view text, size_t offset);

/**
 * Find the character boundary of a line of text closest to a horizontal position.
 *
 * The line is measured with the font that draws it, so the boundary is the one under the pointer
 * instead of an estimate from a fixed character width. A tie is resolved to the earlier boundary,
 * which is what a text cursor does.
 * @param line The rendered text of the line.
 * @param x The position, in pixels, relative to the start of the line.
 * @param measure Returns the rendered width of a prefix of the line.
 * @return The byte offset of the nearest boundary, clamped to the line.
 */
size_t OffsetForX(std::string_view line, int x, const AgentChatTextMeasure &measure);

/**
 * Get the rendered width of the prefix of a line up to an offset.
 * @param line The rendered text of the line.
 * @param offset The byte offset in the line; it is snapped to a character boundary.
 * @param measure Returns the rendered width of a prefix of the line.
 * @return The width in pixels, from the start of the line.
 */
int WidthForOffset(std::string_view line, size_t offset, const AgentChatTextMeasure &measure);

#endif /* AGENT_CHAT_SELECTION_H */
