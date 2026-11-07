/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file agent_chat_selection.cpp Selectable text of the agent chat transcript. */

#include "stdafx.h"

#include "agent_chat_selection.h"

#include <algorithm>

#include "safeguards.h"

void AgentChatSelection::Clear()
{
	this->anchor = {};
	this->cursor = {};
	this->has_anchor = false;
}

void AgentChatSelection::BindChat(const std::string &chat_id, const std::string &world_id, const std::string &epoch)
{
	if (chat_id == this->chat && world_id == this->world && epoch == this->epoch) return;
	this->Clear();
	this->chat = chat_id;
	this->world = world_id;
	this->epoch = epoch;
}

void AgentChatSelection::Start(const AgentChatSelectionPoint &point)
{
	this->anchor = point;
	this->cursor = point;
	this->has_anchor = true;
}

void AgentChatSelection::Extend(const AgentChatSelectionPoint &point)
{
	if (!this->has_anchor) {
		this->Start(point);
		return;
	}
	this->cursor = point;
}

std::pair<AgentChatSelectionPoint, AgentChatSelectionPoint> AgentChatSelection::Ordered() const
{
	return this->cursor.Before(this->anchor) ? std::make_pair(this->cursor, this->anchor) : std::make_pair(this->anchor, this->cursor);
}

std::string AgentChatSelection::GetText(const std::vector<std::string_view> &texts) const
{
	if (this->Empty()) return {};

	const auto [from, to] = this->Ordered();
	if (from.message >= texts.size()) return {};

	const size_t last = std::min(to.message, texts.size() - 1);
	std::string result;
	for (size_t message = from.message; message <= last; message++) {
		const std::string_view text = texts[message];
		const size_t begin = message == from.message ? std::min(from.offset, text.size()) : 0;
		/* The end of the selection only applies to the message it was made in. */
		const size_t end = message == to.message ? std::min(to.offset, text.size()) : text.size();

		if (message != from.message) result.push_back('\n');
		if (begin < end) result.append(text.substr(begin, end - begin));
	}
	return result;
}

std::optional<std::pair<size_t, size_t>> AgentChatSelection::RangeIn(size_t message, size_t length) const
{
	if (this->Empty()) return std::nullopt;

	const auto [from, to] = this->Ordered();
	if (message < from.message || message > to.message) return std::nullopt;

	const size_t begin = message == from.message ? std::min(from.offset, length) : 0;
	const size_t end = message == to.message ? std::min(to.offset, length) : length;
	if (begin >= end) return std::nullopt;
	return std::make_pair(begin, end);
}

void AgentChatSelection::Clamp(const std::vector<std::string_view> &texts)
{
	if (!this->has_anchor) return;

	if (this->anchor.message >= texts.size() || this->cursor.message >= texts.size()) {
		this->Clear();
		return;
	}

	/* A reloaded message may be shorter or may have been edited: keep the ends inside it and on a
	 * character boundary, so the next copy cannot cut a character. */
	this->anchor.offset = SnapUtf8Boundary(texts[this->anchor.message], this->anchor.offset);
	this->cursor.offset = SnapUtf8Boundary(texts[this->cursor.message], this->cursor.offset);
}

size_t NextUtf8Boundary(std::string_view text, size_t offset)
{
	if (offset >= text.size()) return text.size();
	offset++;
	while (offset < text.size() && IsUtf8Part(text[offset])) offset++;
	return offset;
}

size_t PrevUtf8Boundary(std::string_view text, size_t offset)
{
	offset = std::min(offset, text.size());
	if (offset == 0) return 0;
	offset--;
	while (offset > 0 && IsUtf8Part(text[offset])) offset--;
	return offset;
}

size_t SnapUtf8Boundary(std::string_view text, size_t offset)
{
	if (offset >= text.size()) return text.size();
	while (offset > 0 && IsUtf8Part(text[offset])) offset--;
	return offset;
}

size_t OffsetForX(std::string_view line, int x, const AgentChatTextMeasure &measure)
{
	if (x <= 0) return 0;

	size_t best = 0;
	int best_distance = x;
	for (size_t offset = 0; offset < line.size();) {
		offset = NextUtf8Boundary(line, offset);
		const int width = measure(line.substr(0, offset));
		const int distance = width > x ? width - x : x - width;
		if (distance < best_distance) {
			best_distance = distance;
			best = offset;
		}
	}
	return best;
}

int WidthForOffset(std::string_view line, size_t offset, const AgentChatTextMeasure &measure)
{
	return measure(line.substr(0, SnapUtf8Boundary(line, offset)));
}
