/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file agent_chat_cache.h Cache policy of the agent chat list and of its transcripts. */

#ifndef AGENT_CHAT_CACHE_H
#define AGENT_CHAT_CACHE_H

#include <algorithm>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

/**
 * Check whether the answer of a request still belongs to the world and epoch it was sent in.
 *
 * A request is sent for one world and one epoch and its answer is handled later. A switch in
 * between makes the answer stale, and the chat id alone does not tell the two apart: the chat of
 * the new epoch can carry the same id as the chat the request was sent for, so an answer of the
 * world that was left would otherwise be appended to the transcript of the new one.
 * @param request_world World the request was sent in.
 * @param request_epoch Load epoch the request was sent in.
 * @param world World the UI is in now.
 * @param epoch Load epoch the UI is in now.
 * @return \c true when the answer belongs to the current world and epoch.
 */
inline bool AgentChatAnswerIsCurrent(const std::string &request_world, const std::string &request_epoch, const std::string &world, const std::string &epoch)
{
	return request_world == world && request_epoch == epoch;
}

/** What merging one answer into a cached transcript did. */
struct AgentChatMergeResult {
	bool changed = false;         ///< Whether the transcript gained a message.
	bool inserted_before = false; ///< Whether a message went in before messages that were cached already.
};

/**
 * Merge the messages of one answer into a cached transcript.
 *
 * The answers of the backend are not guaranteed to arrive in order: a read that was sent before
 * the chat list was refreshed, or a full history that is answered after a delta, can bring
 * messages that an earlier answer already passed. Placing every message at its sequence position
 * instead of appending whatever is newer than the last sequence seen keeps the transcript
 * complete and in order, and a message that was received before is not shown twice.
 *
 * Only messages the backend numbered can be placed, so a message without a positive sequence
 * number is dropped: an answer that repeats such a message cannot be told apart from one that
 * brings a new one, and showing it again on every poll would be a duplicate reply.
 * @tparam Message A message with an `int64_t seq` member.
 * @param messages The cached transcript, in sequence order.
 * @param incoming The messages of one answer, in the order the backend returned them.
 * @return What the merge changed, including whether the indices of the cached messages moved.
 */
template <typename Message>
AgentChatMergeResult AgentChatMergeMessages(std::vector<Message> &messages, std::vector<Message> &&incoming)
{
	AgentChatMergeResult result;

	std::vector<Message> sorted;
	sorted.reserve(incoming.size());
	for (Message &message : incoming) {
		if (message.seq <= 0) continue;
		sorted.push_back(std::move(message));
	}
	if (sorted.empty()) return result;

	/* An answer is expected in sequence order, but a merge must not depend on it; sorting the
	 * batch keeps the merge below a single pass over both lists. */
	std::stable_sort(sorted.begin(), sorted.end(), [](const Message &a, const Message &b) { return a.seq < b.seq; });

	std::vector<Message> merged;
	merged.reserve(messages.size() + sorted.size());

	size_t cached = 0;
	int64_t last_seq = 0;
	for (Message &message : sorted) {
		/* A sequence number that the answer repeats is the same message once. */
		if (message.seq == last_seq) continue;
		last_seq = message.seq;

		/* Everything of the cache that comes before this message keeps its place. */
		while (cached < messages.size() && messages[cached].seq < message.seq) {
			merged.push_back(std::move(messages[cached]));
			++cached;
		}

		if (cached < messages.size() && messages[cached].seq == message.seq) {
			/* Received before, so the cached message is the one that is shown. */
			merged.push_back(std::move(messages[cached]));
			++cached;
			continue;
		}

		/* A message that goes in before the rest of the cache moves the cached indices, so a
		 * selection made in the transcript no longer points at the text it was made in. An answer
		 * that only appends moves nothing. */
		if (cached < messages.size()) result.inserted_before = true;
		merged.push_back(std::move(message));
		result.changed = true;
	}

	while (cached < messages.size()) {
		merged.push_back(std::move(messages[cached]));
		++cached;
	}
	messages = std::move(merged);
	return result;
}

/**
 * Merge a refreshed chat list with the chats that are already cached.
 *
 * A refresh of the chat list of the same world and epoch describes the same conversations: the
 * same chat id is the same chat, so its cached transcript and its cursors are moved into the
 * refreshed entry, which only brings what the list says about it (the title and the task state).
 * Rebuilding the entries instead would drop the transcript and reset the cursors, and a read that
 * is still in flight would then land on the rebuilt chat, take the sequence numbers of the
 * messages it brings for the whole transcript, and hide every message that came before them.
 *
 * A chat the backend no longer lists is dropped with its cache, and a chat that appears in the
 * list for the first time starts with an empty one. The task state is the one the refresh
 * reports, never the cached one: it is the authoritative state of the chat right now.
 * @tparam Chat A chat with `id`, `messages`, `message_seq` and `event_seq` members, i.e. a chat of
 *              the UI cache.
 * @param chats The cached chats, replaced by the refreshed list in its order.
 * @param refreshed The chats of the refreshed list, without a cached transcript.
 */
template <typename Chat>
void AgentChatMergeChatList(std::vector<Chat> &chats, std::vector<Chat> &&refreshed)
{
	for (Chat &chat : refreshed) {
		for (Chat &cached : chats) {
			if (cached.id != chat.id) continue;
			/* Moved, not copied: the cache of the chat stays the cache of the chat. */
			chat.messages = std::move(cached.messages);
			chat.message_seq = cached.message_seq;
			chat.event_seq = cached.event_seq;
			break;
		}
	}
	chats = std::move(refreshed);
}

#endif /* AGENT_CHAT_CACHE_H */
