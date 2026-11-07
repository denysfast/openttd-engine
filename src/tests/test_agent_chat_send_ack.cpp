/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <http://www.gnu.org/licenses/>.
 */

/**
 * @file test_agent_chat_send_ack.cpp Test the state machine of a chat message submission: the
 * Send handler, the chat creation it may need, the HTTP acknowledgement and the paint that shows
 * the feedback.
 *
 * The machine is a plain value driven with explicit steady clock points, so the cases below run
 * without a window, a backend or a game: they are the ones the contract promises the player and
 * the environment, i.e. feedback from the click on, one submission at a time, a chat that is
 * bound to the submission it was created for, a draft that survives, and timings that are never
 * mixed across submissions, worlds or processes.
 */

#include "../stdafx.h"

#include "../3rdparty/catch2/catch.hpp"

#include "../agent_chat_gui.h"

#include <chrono>
#include <string>

#include "../safeguards.h"

using Clock = std::chrono::steady_clock;
using AckResult = AgentChatSendAck::AckResult;
using FailReason = AgentChatSendAck::FailReason;

/** The world and epoch every case starts in. */
static const std::string WORLD = "world-a";
static const std::string EPOCH = "epoch-1";
/** The payload identity the UI builds for the message every case sends. */
static const std::string PAYLOAD = "{\"attachments\":[],\"text\":\"hello agent\"}";
/** The same text, edited. */
static const std::string PAYLOAD_EDITED = "{\"attachments\":[],\"text\":\"hello agent!\"}";
/** The same text with one attachment, i.e. another payload. */
static const std::string PAYLOAD_TRAY = "{\"attachments\":[{\"id\":7,\"type\":\"town\"}],\"text\":\"hello agent\"}";
/** The same text with the same attachment in another position, i.e. another payload. */
static const std::string PAYLOAD_TRAY_REORDERED = "{\"attachments\":[{\"id\":8,\"type\":\"town\"},{\"id\":7,\"type\":\"town\"}],\"text\":\"hello agent\"}";

/**
 * A clock point a fixed number of microseconds after the start of the case.
 * @param micros The offset in microseconds.
 * @return The clock point.
 */
static Clock::time_point At(int64_t micros)
{
	return Clock::time_point{} + std::chrono::seconds(100) + std::chrono::microseconds(micros);
}

TEST_CASE("agent_chat_send_ack: one submission at a time")
{
	AgentChatSendAck ack;

	const int64_t send = ack.Begin("chat-1", WORLD, EPOCH, PAYLOAD, At(0));
	REQUIRE(send != 0);
	CHECK(ack.Pending());
	CHECK(ack.Visible() == send);

	/* A second Send while the first one waits for its answer is refused, and the feedback that is
	 * on screen still belongs to the first submission. */
	CHECK(ack.Begin("chat-1", WORLD, EPOCH, PAYLOAD, At(1000)) == 0);
	CHECK(ack.Pending());
	CHECK(ack.Visible() == send);

	/* Once the answer arrived, the next message is a new submission with its own number. */
	CHECK(ack.Acknowledge(send, WORLD, EPOCH, true, At(2000)) == AckResult::ClearDraft);
	CHECK_FALSE(ack.Pending());
	const int64_t next = ack.Begin("chat-1", WORLD, EPOCH, PAYLOAD, At(3000));
	CHECK(next == send + 1);
	CHECK(ack.Visible() == next);
}

TEST_CASE("agent_chat_send_ack: the submission starts before its chat exists")
{
	AgentChatSendAck ack;

	/* The Send handler starts the submission at the click, while the chat still has to be created:
	 * the feedback has a chat to match yet, and no second creation may be started. */
	const int64_t send = ack.Begin("", WORLD, EPOCH, PAYLOAD, At(0));
	REQUIRE(send != 0);
	CHECK(ack.Pending());
	CHECK(ack.VisibleChatId().empty());
	CHECK(ack.Begin("", WORLD, EPOCH, PAYLOAD, At(1000)) == 0);

	/* The created chat is bound to the submission it was created for. */
	CHECK(ack.BindChat(send, "chat-7"));
	CHECK(ack.VisibleChatId() == "chat-7");
	CHECK(ack.Pending());

	/* A stale submission cannot take the chat of another one. */
	CHECK_FALSE(ack.BindChat(send + 1, "chat-8"));
	CHECK_FALSE(ack.BindChat(send, ""));
	CHECK(ack.VisibleChatId() == "chat-7");
}

TEST_CASE("agent_chat_send_ack: a chat that could not be created ends the submission")
{
	AgentChatSendAck ack;

	const int64_t send = ack.Begin("", WORLD, EPOCH, PAYLOAD, At(0));
	REQUIRE(send != 0);
	/* The creation failed, so nothing was sent: the submission ends instead of waiting forever and
	 * the player can send the retained draft again. */
	ack.Fail(send, WORLD, EPOCH, FailReason::NotSent);

	CHECK_FALSE(ack.Pending());
	CHECK(ack.Visible() == 0);
	CHECK(ack.LastOutcome() == "not_sent");
	CHECK_FALSE(ack.ToJson()["painted"].get<bool>());
	CHECK(ack.Begin("", WORLD, EPOCH, PAYLOAD, At(1000)) != 0);
}

TEST_CASE("agent_chat_send_ack: an answer of an old epoch is ignored")
{
	AgentChatSendAck ack;

	const int64_t send = ack.Begin("chat-1", WORLD, "epoch-2", PAYLOAD, At(0));
	REQUIRE(send != 0);

	/* The world was left while the request was in flight: the answer must not clear the draft of
	 * the new one, and the submission it belongs to is gone. */
	CHECK(ack.Acknowledge(send, WORLD, "epoch-1", true, At(2000)) == AckResult::Ignored);
	CHECK_FALSE(ack.Pending());
	CHECK(ack.Visible() == 0);

	/* Nothing was painted for it, so nothing is claimed about it. */
	const nlohmann::json timing = ack.ToJson();
	CHECK(timing["last_send"] == 0);
	CHECK(timing["painted"] == false);
	CHECK(timing["handler_to_paint_us"].is_null());

	/* The same answer cannot be applied a second time either. */
	CHECK(ack.Acknowledge(send, WORLD, "epoch-2", true, At(3000)) == AckResult::Ignored);
}

TEST_CASE("agent_chat_send_ack: an answer of another world is ignored")
{
	AgentChatSendAck ack;

	const int64_t send = ack.Begin("chat-1", WORLD, EPOCH, PAYLOAD, At(0));
	REQUIRE(send != 0);
	CHECK(ack.Acknowledge(send, "world-b", EPOCH, true, At(1000)) == AckResult::Ignored);
	CHECK_FALSE(ack.Pending());
}

TEST_CASE("agent_chat_send_ack: a late answer of an earlier submission is ignored")
{
	AgentChatSendAck ack;

	const int64_t first = ack.Begin("chat-1", WORLD, EPOCH, PAYLOAD, At(0));
	REQUIRE(first != 0);
	CHECK(ack.Acknowledge(first, WORLD, EPOCH, true, At(1000)) == AckResult::ClearDraft);
	CHECK(ack.Painted(first, At(2000)));
	CHECK(ack.Visible() == 0);

	const int64_t second = ack.Begin("chat-1", WORLD, EPOCH, PAYLOAD, At(3000));
	REQUIRE(second != 0);
	/* A duplicate answer of the first submission does not end the second one. */
	CHECK(ack.Acknowledge(first, WORLD, EPOCH, true, At(4000)) == AckResult::Ignored);
	CHECK(ack.Pending());
	CHECK(ack.Visible() == second);
	CHECK(ack.ToJson()["last_send"] == first);
}

TEST_CASE("agent_chat_send_ack: a newer draft survives the acknowledgement")
{
	AgentChatSendAck ack;

	const int64_t send = ack.Begin("chat-1", WORLD, EPOCH, PAYLOAD, At(0));
	REQUIRE(send != 0);
	/* The player typed or attached something else while the request was in flight, so the answer
	 * must not clear what is in the editor now. */
	CHECK(ack.Acknowledge(send, WORLD, EPOCH, false, At(1000)) == AckResult::KeepDraft);
	CHECK_FALSE(ack.Pending());
}

TEST_CASE("agent_chat_send_ack: handler to paint is measured apart from the answer")
{
	AgentChatSendAck ack;

	const int64_t send = ack.Begin("chat-1", WORLD, EPOCH, PAYLOAD, At(0));
	REQUIRE(send != 0);

	/* The paint of the pending feedback: this is the feedback the player waits for, and it is
	 * stamped before the backend answered. */
	CHECK_FALSE(ack.VisibleAcknowledged());
	CHECK(ack.Painted(send, At(7000)));
	CHECK(ack.ToJson()["last_send"] == send);
	CHECK(ack.ToJson()["painted"] == true);
	CHECK(ack.LastOutcome() == "painted");
	REQUIRE(ack.ToJson()["handler_to_paint_us"].is_number_integer());
	CHECK(ack.ToJson()["handler_to_paint_us"] == 7000);
	CHECK(ack.ToJson()["ack_to_paint_us"].is_null());
	CHECK(ack.ToJson()["handler_to_ack_us"].is_null());
	/* The feedback stays on screen until the accepted state can be drawn. */
	CHECK(ack.Visible() == send);

	/* The answer arrives, and a paint of another submission is not stamped. */
	CHECK(ack.Acknowledge(send, WORLD, EPOCH, true, At(20000)) == AckResult::ClearDraft);
	CHECK(ack.VisibleAcknowledged());
	CHECK_FALSE(ack.Painted(send + 1, At(25000)));
	CHECK(ack.ToJson()["ack_to_paint_us"].is_null());

	/* The paint that shows the accepted or task status is measured on its own. */
	CHECK(ack.Painted(send, At(32000)));
	CHECK(ack.ToJson()["ack_to_paint_us"] == 12000);
	CHECK(ack.ToJson()["handler_to_ack_us"] == 20000);
	CHECK(ack.LastOutcome() == "accepted");
	/* The first feedback timing is not rewritten by the later paint. */
	CHECK(ack.ToJson()["handler_to_paint_us"] == 7000);
	CHECK(ack.Visible() == 0);
}

TEST_CASE("agent_chat_send_ack: an answer before the first paint still reports the feedback")
{
	AgentChatSendAck ack;

	const int64_t send = ack.Begin("chat-1", WORLD, EPOCH, PAYLOAD, At(0));
	REQUIRE(send != 0);
	CHECK(ack.Acknowledge(send, WORLD, EPOCH, true, At(4000)) == AckResult::ClearDraft);
	CHECK(ack.Visible() == send);
	CHECK(ack.VisibleAcknowledged());

	CHECK(ack.Painted(send, At(9000)));
	CHECK(ack.ToJson()["handler_to_paint_us"] == 9000);
	CHECK(ack.ToJson()["ack_to_paint_us"] == 5000);
	CHECK(ack.ToJson()["handler_to_ack_us"] == 4000);
	CHECK(ack.Visible() == 0);
}

TEST_CASE("agent_chat_send_ack: a lost answer keeps the painted feedback timing")
{
	AgentChatSendAck ack;

	const int64_t send = ack.Begin("chat-1", WORLD, EPOCH, PAYLOAD, At(0));
	REQUIRE(send != 0);
	CHECK(ack.Painted(send, At(7000)));
	CHECK(ack.ToJson()["handler_to_paint_us"] == 7000);

	/* The request left but its answer was lost: the backend may have the message, so the true
	 * first-paint timing stays and only the outcome says that the acceptance is unknown. */
	ack.Fail(send, WORLD, EPOCH, FailReason::Uncertain);
	CHECK(ack.LastOutcome() == "uncertain");
	CHECK(ack.ToJson()["painted"] == true);
	CHECK(ack.ToJson()["handler_to_paint_us"] == 7000);
	CHECK_FALSE(ack.Pending());
	CHECK(ack.Visible() == 0);
	/* The answer latency of an uncertain send is unknown, not zero. */
	CHECK(ack.ToJson()["handler_to_ack_us"].is_null());
}

TEST_CASE("agent_chat_send_ack: a retry of an uncertain send keeps its client message id")
{
	AgentChatSendAck ack;

	const int64_t first = ack.Begin("chat-1", WORLD, EPOCH, PAYLOAD, At(0));
	REQUIRE(first != 0);
	const std::string id = ack.PendingClientMessageId();
	REQUIRE_FALSE(id.empty());
	ack.Fail(first, WORLD, EPOCH, FailReason::Uncertain);

	/* The same chat and the same payload again is a retry of a message the backend may already
	 * have, so it is sent under the same id and a backend that deduplicates does not take it twice. */
	const int64_t retry = ack.Begin("chat-1", WORLD, EPOCH, PAYLOAD, At(1000));
	REQUIRE(retry != 0);
	CHECK(ack.PendingClientMessageId() == id);

	/* A message that never left cannot have arrived, so a new one is a new message. */
	ack.Fail(retry, WORLD, EPOCH, FailReason::NotSent);
	const int64_t after = ack.Begin("chat-1", WORLD, EPOCH, PAYLOAD, At(2000));
	REQUIRE(after != 0);
	CHECK(ack.PendingClientMessageId() != id);

	/* Edited text is a different message even after an uncertain send. */
	ack.Fail(after, WORLD, EPOCH, FailReason::Uncertain);
	const int64_t edited = ack.Begin("chat-1", WORLD, EPOCH, PAYLOAD_EDITED, At(3000));
	REQUIRE(edited != 0);
	CHECK(ack.PendingClientMessageId() != id);
}

TEST_CASE("agent_chat_send_ack: the same text with another tray is another message")
{
	AgentChatSendAck ack;

	const int64_t first = ack.Begin("chat-1", WORLD, EPOCH, PAYLOAD, At(0));
	REQUIRE(first != 0);
	const std::string id = ack.PendingClientMessageId();
	ack.Fail(first, WORLD, EPOCH, FailReason::Uncertain);

	/* Same text, one attachment more: the payload changed, so a fresh id is used and a backend that
	 * deduplicates cannot drop the message the player actually composed. */
	const int64_t with_tray = ack.Begin("chat-1", WORLD, EPOCH, PAYLOAD_TRAY, At(1000));
	REQUIRE(with_tray != 0);
	CHECK(ack.PendingClientMessageId() != id);
	const std::string tray_id = ack.PendingClientMessageId();
	ack.Fail(with_tray, WORLD, EPOCH, FailReason::Uncertain);

	/* The same attachments in another order are another payload as well. */
	const int64_t reordered = ack.Begin("chat-1", WORLD, EPOCH, PAYLOAD_TRAY_REORDERED, At(2000));
	REQUIRE(reordered != 0);
	CHECK(ack.PendingClientMessageId() != tray_id);
	const std::string reordered_id = ack.PendingClientMessageId();
	ack.Fail(reordered, WORLD, EPOCH, FailReason::Uncertain);

	/* That payload is the one that may have arrived now, so a retry of it keeps its own id. */
	const int64_t again = ack.Begin("chat-1", WORLD, EPOCH, PAYLOAD_TRAY_REORDERED, At(3000));
	REQUIRE(again != 0);
	CHECK(ack.PendingClientMessageId() == reordered_id);
}

TEST_CASE("agent_chat_send_ack: the same payload in another chat is another message")
{
	AgentChatSendAck ack;

	const int64_t first = ack.Begin("chat-1", WORLD, EPOCH, PAYLOAD, At(0));
	REQUIRE(first != 0);
	const std::string id = ack.PendingClientMessageId();
	ack.Fail(first, WORLD, EPOCH, FailReason::Uncertain);

	/* The player selected another chat and sends the same text there: that message cannot be the
	 * one the backend may already have. */
	const int64_t other = ack.Begin("chat-2", WORLD, EPOCH, PAYLOAD, At(1000));
	REQUIRE(other != 0);
	CHECK(ack.PendingClientMessageId() != id);
}

TEST_CASE("agent_chat_send_ack: a submission keeps the chat it was created for")
{
	AgentChatSendAck ack;

	/* The chat is created while the message is prepared, so the submission has no chat yet. */
	const int64_t send = ack.Begin("", WORLD, EPOCH, PAYLOAD, At(0));
	REQUIRE(send != 0);
	CHECK(ack.VisibleChatId().empty());

	/* The continuation binds the id of the chat that was created, not the chat that is selected
	 * now: a player who selects another chat does not redirect the submission. */
	CHECK(ack.BindChat(send, "chat-created"));
	CHECK(ack.VisibleChatId() == "chat-created");

	/* The answer and the paint belong to that chat. */
	CHECK(ack.Acknowledge(send, WORLD, EPOCH, true, At(1000)) == AckResult::ClearDraft);
	CHECK(ack.Painted(send, At(2000)));
	CHECK(ack.ToJson()["chat_id"] == "chat-created");
	CHECK(ack.ToJson()["handler_to_paint_us"] == 2000);
}

TEST_CASE("agent_chat_send_ack: a world switch drops the submission of the old world")
{
	AgentChatSendAck ack;

	const int64_t send = ack.Begin("chat-1", WORLD, EPOCH, PAYLOAD, At(0));
	REQUIRE(send != 0);
	CHECK(ack.Painted(send, At(1000)));
	ack.Reset();

	CHECK_FALSE(ack.Pending());
	CHECK(ack.Visible() == 0);
	CHECK(ack.Acknowledge(send, WORLD, EPOCH, true, At(2000)) == AckResult::Ignored);
	/* The timing that was painted keeps the world it was measured in. */
	CHECK(ack.LastWorldId() == WORLD);
	CHECK(ack.LastEpoch() == EPOCH);
}

TEST_CASE("agent_chat_send_ack: the reported object names its clock, unit and binding")
{
	AgentChatSendAck ack;

	const int64_t send = ack.Begin("chat-1", WORLD, EPOCH, PAYLOAD, At(0));
	REQUIRE(send != 0);
	CHECK(ack.Painted(send, At(7000)));

	const nlohmann::json timing = ack.ToJson();
	CHECK(timing["unit"] == "microseconds");
	CHECK(timing["clock"] == "steady_clock");
	CHECK(timing["scope"].get<std::string>().find("same process only") != std::string::npos);
	CHECK(timing["target_us"] == 100000);
	CHECK(timing["pending"] == true);
	CHECK(timing["last_send"] == send);
	CHECK(timing["chat_id"] == "chat-1");
	CHECK(timing["world_id"] == WORLD);
	CHECK(timing["epoch"] == EPOCH);
	CHECK(timing["outcome"] == "painted");
	CHECK(timing["client_message_id"] == ack.PendingClientMessageId());
	CHECK(timing["painted"] == true);
	CHECK(timing["handler_to_paint_us"] == 7000);
	/* The answer latency is not known yet, and is reported as null instead of a made up number. */
	CHECK(timing["ack_to_paint_us"].is_null());
	CHECK(timing["handler_to_ack_us"].is_null());
}
