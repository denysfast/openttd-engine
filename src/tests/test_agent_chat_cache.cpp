/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/**
 * @file test_agent_chat_cache.cpp Test the cache policy of the agent chat list and its transcripts.
 *
 * The chat UI keeps the transcript of every chat it has seen, and a refresh of the chat list of
 * the same world and epoch must not throw that cache away: a read that is still in flight would
 * then land on the rebuilt chat and take the sequence numbers of the messages it brings for the
 * whole transcript. The cases below drive the policy the UI uses, i.e. that a refresh moves the
 * cached transcript and cursors of the same chat id into the refreshed entry, that an answer which
 * overtook another one still fills the transcript in sequence order without repeating a message,
 * that a chat the backend dropped is not resurrected, and that an answer of another world or epoch
 * is not applied at all.
 */

#include "../stdafx.h"

#include "../3rdparty/catch2/catch.hpp"

#include "../agent_chat_cache.h"

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

#include "../safeguards.h"

namespace {

/** A message of a test transcript: the policy only orders messages by their sequence number. */
struct TestMessage {
	int64_t seq = 0;   ///< Sequence number of the message.
	std::string text;  ///< Text that tells the messages apart.
};

/** A chat of the test cache, with the fields the cache policy keeps across a refresh. */
struct TestChat {
	std::string id;                     ///< Chat id.
	std::string title;                  ///< Title reported by the chat list.
	int64_t message_seq = 0;            ///< Highest message sequence number seen so far.
	int64_t event_seq = 0;              ///< Highest event sequence number seen so far.
	std::vector<TestMessage> messages;  ///< Cached transcript.
	bool task_active = false;           ///< Whether the chat has a running task.
	bool stop_pending = false;          ///< Whether a stop was requested and is not acknowledged yet.
};

/** A message of the given sequence number, with a text that names it. */
TestMessage Msg(int64_t seq)
{
	return TestMessage{seq, "m" + std::to_string(seq)};
}

/** A batch of messages with the given sequence numbers, in that order. */
std::vector<TestMessage> Batch(const std::vector<int64_t> &seqs)
{
	std::vector<TestMessage> batch;
	for (const int64_t seq : seqs) batch.push_back(Msg(seq));
	return batch;
}

/**
 * Merge one answer into a chat the way the message loader does: merge the messages, then move the
 * cursor forward to the highest sequence number of the answer.
 * @param chat The cached chat.
 * @param batch The messages of the answer.
 * @return What the merge changed.
 */
AgentChatMergeResult Answer(TestChat &chat, std::vector<TestMessage> &&batch)
{
	int64_t highest = chat.message_seq;
	for (const TestMessage &message : batch) highest = std::max(highest, message.seq);

	const AgentChatMergeResult merged = AgentChatMergeMessages(chat.messages, std::move(batch));
	chat.message_seq = highest;
	return merged;
}

/**
 * The sequence numbers of the transcript of a chat, in transcript order, as one text so that a
 * failing check shows the whole transcript instead of only the first message that differs.
 * @param chat The chat.
 * @return The sequence numbers, separated by single spaces.
 */
std::string Seqs(const TestChat &chat)
{
	std::string seqs;
	for (const TestMessage &message : chat.messages) {
		if (!seqs.empty()) seqs.push_back(' ');
		seqs += std::to_string(message.seq);
	}
	return seqs;
}

/** A chat with a cached transcript of the given sequence numbers and a cursor at the last one. */
TestChat CachedChat(const std::string &id, const std::vector<int64_t> &seqs)
{
	TestChat chat;
	chat.id = id;
	chat.title = id;
	AgentChatMergeMessages(chat.messages, Batch(seqs));
	chat.message_seq = seqs.empty() ? 0 : seqs.back();
	return chat;
}

/** A chat of a refreshed chat list: no transcript, the fields the list carries. */
TestChat RefreshedChat(const std::string &id, const std::string &title)
{
	TestChat chat;
	chat.id = id;
	chat.title = title;
	return chat;
}

} // namespace

TEST_CASE("agent_chat_cache: an answer that overtook another one still fills the transcript")
{
	/* The chat was read up to sequence 5 before the list was refreshed. */
	TestChat chat = CachedChat("chat-a", {1, 2, 3, 4, 5});
	REQUIRE(Seqs(chat) == "1 2 3 4 5");

	/* A delta of 6 and 7 lands first: it only appends, so nothing that was cached moves. */
	const AgentChatMergeResult delta = Answer(chat, Batch({6, 7}));
	CHECK(delta.changed);
	CHECK_FALSE(delta.inserted_before);
	CHECK(Seqs(chat) == "1 2 3 4 5 6 7");
	CHECK(chat.message_seq == 7);

	/* The full history then arrives with the prefix that was already cached: it fills nothing new,
	 * and it must not append the messages a second time. */
	const AgentChatMergeResult history = Answer(chat, Batch({1, 2, 3, 4, 5, 6, 7}));
	CHECK_FALSE(history.changed);
	CHECK_FALSE(history.inserted_before);
	CHECK(Seqs(chat) == "1 2 3 4 5 6 7");
	CHECK(chat.message_seq == 7);
}

TEST_CASE("agent_chat_cache: a prefix that arrives after a delta fills the transcript")
{
	/* The same race without a preserved cache: the delta of 6 and 7 landed on an empty transcript
	 * and the cursor already says 7. */
	TestChat chat;
	Answer(chat, Batch({6, 7}));
	REQUIRE(Seqs(chat) == "6 7");
	REQUIRE(chat.message_seq == 7);

	/* The full history brings the messages before them, and they are placed in front of the cache
	 * instead of being skipped as older than the cursor. */
	const AgentChatMergeResult merged = Answer(chat, Batch({1, 2, 3, 4, 5, 6, 7}));
	CHECK(merged.changed);
	CHECK(merged.inserted_before);
	CHECK(Seqs(chat) == "1 2 3 4 5 6 7");
	CHECK(chat.message_seq == 7);
}

TEST_CASE("agent_chat_cache: a message is shown once")
{
	TestChat chat = CachedChat("chat-a", {1, 2, 3});

	/* The same answer twice changes nothing the second time. */
	CHECK(Answer(chat, Batch({4, 5})).changed);
	CHECK_FALSE(Answer(chat, Batch({4, 5})).changed);
	CHECK(Seqs(chat) == "1 2 3 4 5");

	/* A sequence number the answer repeats is one message, and the cached one is the one shown. */
	CHECK(Answer(chat, Batch({6, 6, 6})).changed);
	CHECK(Seqs(chat) == "1 2 3 4 5 6");
	CHECK(chat.messages.back().text == "m6");

	/* A message that was cached before keeps its text when an answer repeats its sequence. */
	std::vector<TestMessage> reworded = Batch({4});
	reworded[0].text = "rewritten";
	CHECK_FALSE(Answer(chat, std::move(reworded)).changed);
	CHECK(chat.messages[3].text == "m4");
}

TEST_CASE("agent_chat_cache: a message without a sequence number is not shown")
{
	TestChat chat = CachedChat("chat-a", {1, 2});

	/* Neither zero nor a negative sequence number can be placed, so an answer that carries only
	 * such messages changes nothing, however often it is repeated. */
	CHECK_FALSE(Answer(chat, Batch({0})).changed);
	CHECK_FALSE(Answer(chat, Batch({-3})).changed);
	CHECK_FALSE(Answer(chat, Batch({0, 0, -1})).changed);
	CHECK(Seqs(chat) == "1 2");
	CHECK(chat.message_seq == 2);

	/* A placed message of the same answer is kept. */
	CHECK(Answer(chat, Batch({0, 3})).changed);
	CHECK(Seqs(chat) == "1 2 3");
}

TEST_CASE("agent_chat_cache: an answer in another order is placed in sequence order")
{
	TestChat chat = CachedChat("chat-a", {1, 4});

	/* The answer is not required to be sorted, and a merge in the middle moves the cached
	 * indices of the messages behind it. */
	const AgentChatMergeResult merged = Answer(chat, Batch({5, 2, 3}));
	CHECK(merged.changed);
	CHECK(merged.inserted_before);
	CHECK(Seqs(chat) == "1 2 3 4 5");
	CHECK(chat.message_seq == 5);
}

TEST_CASE("agent_chat_cache: a chat list refresh keeps the cached transcripts")
{
	std::vector<TestChat> chats;
	chats.push_back(CachedChat("chat-a", {1, 2, 3}));
	chats.back().event_seq = 11;
	chats.push_back(CachedChat("chat-b", {1, 2}));
	chats.back().event_seq = 4;

	/* The refresh of the same world and epoch reports the same chats in another order, with the
	 * title and the task state the backend has now. */
	std::vector<TestChat> refreshed;
	refreshed.push_back(RefreshedChat("chat-b", "chat-b renamed"));
	refreshed.push_back(RefreshedChat("chat-a", "chat-a renamed"));
	refreshed.back().task_active = true;
	AgentChatMergeChatList(chats, std::move(refreshed));

	REQUIRE(chats.size() == 2);
	CHECK(chats[0].id == "chat-b");
	CHECK(chats[1].id == "chat-a");

	/* The transcripts and cursors of both chats, the inactive one included, are the cached ones. */
	CHECK(Seqs(chats[0]) == "1 2");
	CHECK(chats[0].message_seq == 2);
	CHECK(chats[0].event_seq == 4);
	CHECK(Seqs(chats[1]) == "1 2 3");
	CHECK(chats[1].message_seq == 3);
	CHECK(chats[1].event_seq == 11);

	/* What the list says about the chats is the current state, not the cached one. */
	CHECK(chats[0].title == "chat-b renamed");
	CHECK(chats[1].title == "chat-a renamed");
	CHECK(chats[1].task_active);
}

TEST_CASE("agent_chat_cache: a refresh takes the task state of the list")
{
	std::vector<TestChat> chats;
	chats.push_back(CachedChat("chat-a", {1, 2}));
	chats.back().task_active = true;
	chats.back().stop_pending = true;

	/* The list no longer reports a running task, so the chat is idle again and a stop that was
	 * requested for the task that ended is not pending. */
	std::vector<TestChat> refreshed;
	refreshed.push_back(RefreshedChat("chat-a", "chat-a"));
	AgentChatMergeChatList(chats, std::move(refreshed));
	REQUIRE(chats.size() == 1);
	CHECK_FALSE(chats[0].task_active);
	CHECK_FALSE(chats[0].stop_pending);

	/* A task that is running again is the state of the list: a stop that was pending for the task
	 * before it is not carried over, because the cached chat does not know which task it was. */
	chats[0].stop_pending = true;
	std::vector<TestChat> rerun;
	rerun.push_back(RefreshedChat("chat-a", "chat-a"));
	rerun.back().task_active = true;
	AgentChatMergeChatList(chats, std::move(rerun));
	REQUIRE(chats.size() == 1);
	CHECK(chats[0].task_active);
	CHECK_FALSE(chats[0].stop_pending);
}

TEST_CASE("agent_chat_cache: a chat the backend dropped is not resurrected")
{
	std::vector<TestChat> chats;
	chats.push_back(CachedChat("chat-a", {1, 2, 3}));
	chats.push_back(CachedChat("chat-b", {1}));

	/* The backend no longer lists chat-a, so its cache goes with it; chat-c is new. */
	std::vector<TestChat> refreshed;
	refreshed.push_back(RefreshedChat("chat-b", "chat-b"));
	refreshed.push_back(RefreshedChat("chat-c", "chat-c"));
	AgentChatMergeChatList(chats, std::move(refreshed));

	REQUIRE(chats.size() == 2);
	CHECK(chats[0].id == "chat-b");
	CHECK(Seqs(chats[0]) == "1");
	CHECK(chats[1].id == "chat-c");
	CHECK(chats[1].messages.empty());
	CHECK(chats[1].message_seq == 0);
	CHECK(chats[1].event_seq == 0);
}

TEST_CASE("agent_chat_cache: an answer of another world or epoch is not applied")
{
	/* The answer belongs to the world and epoch the request was sent in. */
	CHECK(AgentChatAnswerIsCurrent("world-1", "epoch-1", "world-1", "epoch-1"));

	/* The same chat id in another epoch is another transcript, so the answer is stale. */
	CHECK_FALSE(AgentChatAnswerIsCurrent("world-1", "epoch-1", "world-1", "epoch-2"));
	CHECK_FALSE(AgentChatAnswerIsCurrent("world-1", "epoch-1", "world-2", "epoch-1"));
	CHECK_FALSE(AgentChatAnswerIsCurrent("world-1", "epoch-1", "world-2", "epoch-2"));
}

TEST_CASE("agent_chat_cache: only a prefix insertion moves the transcript indices")
{
	TestChat chat = CachedChat("chat-a", {1, 2, 3});

	/* An ordinary answer appends behind the cache, so a selection stays on its own text. */
	CHECK_FALSE(Answer(chat, Batch({4})).inserted_before);
	CHECK_FALSE(Answer(chat, Batch({5, 6})).inserted_before);

	/* An answer that repeats cached messages moves nothing either. */
	CHECK_FALSE(Answer(chat, Batch({1, 2, 3})).changed);

	/* A message that goes in before a cached one moves the indices of the transcript. */
	TestChat gap = CachedChat("chat-b", {1, 2, 4});
	const AgentChatMergeResult merged = Answer(gap, Batch({3}));
	CHECK(merged.changed);
	CHECK(merged.inserted_before);
	CHECK(Seqs(gap) == "1 2 3 4");
}
