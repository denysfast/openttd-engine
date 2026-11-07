/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file agent_chat_gui.h Interface of the native in-game agent chat window. */

#ifndef AGENT_CHAT_GUI_H
#define AGENT_CHAT_GUI_H

#include "3rdparty/nlohmann/json.hpp"

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>

/**
 * State machine of one chat message submission: from the Send handler, through the HTTP
 * acknowledgement of the backend, to the first paint that draws the pending-send feedback.
 *
 * The player has to see that the click did something before the backend answers, so the
 * feedback is drawn from the next paint on and the answer only ends it. Every duration in here
 * is measured with std::chrono::steady_clock inside this process: the numbers are comparable
 * within one run of the game and are never mixed with a clock of the backend process.
 *
 * The machine is a plain value with no window, backend or game dependency, so the UI owns one
 * instance and a test can drive its own.
 */
struct AgentChatSendAck {
	/** One submitted message that is on its way to the backend. */
	struct Submission {
		int64_t send = 0;                                ///< Sequence number of this submission, 0 for none.
		std::string chat_id;                             ///< Chat the message was sent to, empty while it is created.
		std::string world_id;                            ///< World the message was sent in.
		std::string epoch;                               ///< Load epoch the message was sent in.
		std::string payload_id;                          ///< Canonical identity of the composed message, for a retry.
		std::string client_message_id;                   ///< Id the backend deduplicates the message on.
		std::chrono::steady_clock::time_point handler{}; ///< When the Send handler ran.
	};

	/** What the UI does with the composed message when an acknowledgement arrives. */
	enum class AckResult {
		Ignored,     ///< A late, foreign or already answered answer; nothing is applied.
		ClearDraft,  ///< The acknowledged draft is still in the editor, so it can be cleared.
		KeepDraft,   ///< The player composed something newer while the request was in flight.
	};

	/** Why a submission ended without an acknowledgement. */
	enum class FailReason {
		NotSent,    ///< The request never left, so the backend cannot have the message.
		Uncertain,  ///< The request left but its answer was lost, so the backend may have the message.
	};

	/**
	 * What the last submission did, bounded to one. The `outcome` field is one of "painted" (the
	 * feedback was on screen, no answer yet), "accepted" (the backend acknowledged the message),
	 * "not_sent" (the message never left) or "uncertain" (the request failed after it left, so the
	 * backend may have the message); it is empty until something happened.
	 */
	struct Timing {
		int64_t send = 0;                          ///< Sequence number of that submission, 0 for none.
		std::string chat_id;                       ///< Chat of that submission, empty when it has none yet.
		std::string world_id;                      ///< World that submission was made in.
		std::string epoch;                         ///< Load epoch that submission was made in.
		std::string outcome;                       ///< What the submission did, see above.
		std::string payload_id;                    ///< Canonical identity of the composed message; kept for a retry.
		std::string client_message_id;             ///< Id of the submission, so a retry can be matched.
		bool painted = false;                      ///< Whether its feedback reached a paint at all.
		std::optional<int64_t> handler_to_paint_us; ///< Send handler to the first paint of the feedback.
		std::optional<int64_t> ack_to_paint_us;     ///< HTTP acceptance to the first paint of the accepted state.
		std::optional<int64_t> handler_to_ack_us;   ///< Send handler to HTTP acceptance, i.e. the answer latency.
	};

	Submission pending{};                 ///< Submission whose acknowledgement is still on its way.
	Submission visible{};                 ///< Submission whose pending feedback is still drawn.
	std::chrono::steady_clock::time_point accepted{}; ///< When the visible submission was acknowledged.
	bool accepted_known = false;          ///< Whether #accepted belongs to #visible.
	bool visible_painted = false;         ///< Whether the feedback of #visible was painted already.
	Timing last{};                        ///< Last painted (or failed) submission, bounded to one.
	int64_t next_send = 1;                ///< Sequence number handed to the next submission.

	/**
	 * Start a submission, unless one is already waiting for its acknowledgement.
	 *
	 * This is called at the entry of the Send handler, before anything asynchronous: the chat may
	 * still have to be created, and the time the player waits for feedback starts at the click.
	 *
	 * The client message id is reused only for a retry of a message whose acceptance is unknown
	 * and whose payload identity, chat, world and epoch are exactly the ones that were sent, so a
	 * changed attachment or another chat is sent under a fresh id instead of being dropped by a
	 * backend that deduplicates.
	 * @param chat_id The chat the message goes to, empty while it is still being created.
	 * @param world_id The world the message is sent in.
	 * @param epoch The load epoch the message is sent in.
	 * @param payload_id The canonical identity of the composed message: text and ordered attachments.
	 * @param now The time the Send handler ran.
	 * @return The sequence number of the new submission, or 0 when a submission was pending already.
	 */
	int64_t Begin(const std::string &chat_id, const std::string &world_id, const std::string &epoch, const std::string &payload_id, std::chrono::steady_clock::time_point now);

	/**
	 * Bind the chat a submission is sent to, once it exists.
	 * @param send The sequence number of the submission.
	 * @param chat_id The id of the chat that was created for it.
	 * @return \c true when the submission still owns this chat and can be sent, \c false when it
	 *         was answered, failed or reset in the meantime.
	 */
	bool BindChat(int64_t send, const std::string &chat_id);

	/**
	 * The client message id of the submission that is on its way.
	 * @return The id, or an empty string when no submission is pending.
	 */
	const std::string &PendingClientMessageId() const { return this->pending.client_message_id; }

	/**
	 * The chat of the submission whose pending feedback is drawn.
	 * @return The chat id, empty while the chat of that submission is still being created.
	 */
	const std::string &VisibleChatId() const { return this->visible.chat_id; }

	/**
	 * Whether the drawn feedback belongs to a submission the backend has already acknowledged.
	 * @return \c true when the acknowledged state has to be drawn next.
	 */
	bool VisibleAcknowledged() const { return this->visible.send != 0 && this->accepted_known; }

	/**
	 * The outcome of the last submission, see #Timing::outcome.
	 * @return The outcome, empty when nothing happened yet.
	 */
	const std::string &LastOutcome() const { return this->last.outcome; }

	/**
	 * The world the last submission was made in.
	 * @return The world id, empty when nothing was submitted yet.
	 */
	const std::string &LastWorldId() const { return this->last.world_id; }

	/**
	 * The load epoch the last submission was made in.
	 * @return The epoch, empty when nothing was submitted yet.
	 */
	const std::string &LastEpoch() const { return this->last.epoch; }

	/**
	 * Whether a submitted message still waits for its acknowledgement, i.e. whether a second Send
	 * would send the same draft twice.
	 * @return \c true when a submission is pending.
	 */
	bool Pending() const { return this->pending.send != 0; }

	/**
	 * Sequence number of the submission whose pending feedback is still on screen.
	 * @return The sequence number, or 0 when no pending feedback is drawn.
	 */
	int64_t Visible() const { return this->visible.send; }

	/**
	 * Apply the HTTP acknowledgement of a submission.
	 * @param send The sequence number the answer belongs to.
	 * @param world_id The world the request was issued in.
	 * @param epoch The load epoch the request was issued in.
	 * @param draft_unchanged Whether the composed message is still the one that was sent.
	 * @param now The time the answer arrived.
	 * @return What the caller does with the composed message.
	 */
	AckResult Acknowledge(int64_t send, const std::string &world_id, const std::string &epoch, bool draft_unchanged, std::chrono::steady_clock::time_point now);

	/**
	 * Apply the failure of a submission: the feedback ends and the draft stays for another try.
	 *
	 * A timing that was already painted for this submission is true and is kept; only the outcome
	 * is changed, so a lost answer never hides the feedback the player did get.
	 * @param send The sequence number the failure belongs to.
	 * @param world_id The world the request was issued in.
	 * @param epoch The load epoch the request was issued in.
	 * @param reason Whether the message can have reached the backend.
	 */
	void Fail(int64_t send, const std::string &world_id, const std::string &epoch, FailReason reason);

	/**
	 * Record that the pending feedback of a submission was actually drawn.
	 * @param send The sequence number of the feedback that was drawn.
	 * @param now The time of the paint.
	 * @return \c true when the feedback of that submission was on screen, \c false for anything else.
	 */
	bool Painted(int64_t send, std::chrono::steady_clock::time_point now);

	/** Forget the submissions of the world that was left; the last painted timing keeps its own world. */
	void Reset();

	/**
	 * Describe the last acknowledgement for the environment.
	 * @return The bounded timing object, the same one world.info reports as `timing.chat_ack`.
	 */
	nlohmann::json ToJson() const;
};

/**
 * Timing of the last chat message acknowledgement of the in-game chat window.
 *
 * The object is published at `world.info` under `timing.chat_ack` and, with the same binding, as
 * `chat_ack` in the description of the chat UI (`ui.inspect`). Its `unit` is "microseconds" and
 * its `clock` is the game's own steady clock: `handler_to_paint_us` is the time from the Send
 * handler to the first paint that drew the pending feedback (the 100 ms the player waits for
 * feedback), `ack_to_paint_us` from the HTTP acceptance to the first paint of the accepted or
 * task status (never the pending text), and `handler_to_ack_us` the answer latency itself. A
 * duration is null while it was not measured, and all of them are measured in this process only.
 * `outcome` says whether the last submission was painted, accepted, not_sent or uncertain.
 *
 * @return The timing object, bound to the world and epoch it was measured in.
 */
nlohmann::json AgentChatAckTimingJson();

/**
 * Open the in-game agent chat window, or bring it to the front when it is already open.
 * The window is also reachable with the "agent_chat" hotkey (Ctrl+Shift+A by default) and
 * from the newspaper menu of the main toolbar.
 */
void ShowAgentChatWindow();

/**
 * Drive the agent chat: refresh the chat list, poll messages and events of the chats
 * with running tasks, and keep the status indicator in sync. The bridge itself is
 * ticked by the game loop, so this only polls the UI state.
 * Called from a realtime timer, so it runs whether or not the chat window is open.
 */
void AgentChatGuiTick();

/**
 * Read-only description of the agent chat UI, meant for GUI tests.
 *
 * The returned object contains the actual screen rectangles of the window widgets and of
 * the individual targets inside them (chat rows, attachment chips and their remove areas,
 * object disambiguation rows), so a test can locate a target and then inject real input.
 * Nothing in here executes an action or changes any state.
 *
 * Layout of the returned object:
 * - "coordinate_space": string, always "screen"; every rectangle in this object is in screen
 *   pixels, i.e. window-local widget rectangles already have the window position added.
 * - "hotkey": string, the hotkey that opens the window.
 * - "window_open": bool, whether the chat window is open.
 * - "window_rect": rectangle of the chat window itself, only present when it is open.
 * - "connected", "world_id", "epoch", "connection_id": transport state.
 * - "active_chat": string id of the selected chat, empty when there is none.
 * - "input": current text of the message input.
 * - "widgets": object mapping widget names to rectangles, only present when the window is open.
 *   Names are caption, chat_list, chat_scroll, new_chat, transcript, transcript_scroll, tray,
 *   add_point, add_object, add_company, input, status, send, stop and closebox (the close box has no widget
 *   id of its own, so it is looked up in the nested tree; Esc closes the window as well). The
 *   caption is the title bar, i.e. the area a real input test can drag the window by.
 * - "chats": array of {id, title, selected, task_active, status}. "task_active" stays true while
 *   the task of the chat is queued, running, stopping or settling, i.e. exactly while Stop is
 *   available; "status" is the translated status text of that task.
 * - "messages": array of {seq, role, text, status, attachments}.
 * - "message_rows": array of {seq, rect, lines} with the screen rectangle of every visible
 *   message and the rendered lines of its text as {rect, text}. The wrapping, the line height and
 *   the text of a line are the ones that were drawn, so a real input test can drag from one line
 *   to another instead of guessing a pixel.
 * - "selection": object {chat, world_id, epoch, empty, anchor, cursor, from, to, text, focused,
 *   rects, bounds} that
 *   describes the selectable transcript text. "anchor" and "cursor" are {message, offset}, i.e.
 *   byte offsets into the cached transcript (always on a UTF-8 character boundary), "from" and
 *   "to" are the same two ends in transcript order, and "text" is exactly what Ctrl+C puts on the
 *   clipboard. "focused" tells whether the transcript owns the selection keys (Ctrl+A selects the
 *   transcript, Ctrl+C copies it); "rects" and "bounds" are the highlighted screen rectangles, and
 *   are empty respectively null when nothing is selected. The selection belongs to the transcript
 *   named by "chat" in the world named by "world_id" and "epoch", and is dropped when another
 *   chat, world or epoch is shown, so it can never copy the text of a transcript it was not made
 *   in. The text of the system clipboard itself is not reported, only the selected text.
 * - "image_rows": array of {rect, artifact, sourcehash, pixelhash, loaded, width, height} with the
 *   screen rectangle of every screenshot preview of the transcript, the artifact and source hash
 *   it shows, the hash of the pixels actually drawn in it (empty until they are loaded) and the
 *   size of the box. Nothing about the host is reported: no file path, no clipboard.
 * - "tray": array of {type, id, generation, tile, name, world_id, epoch, stale, generation_known,
 *   rect, remove_rect}.
 * - "candidates": array of {type, id, generation, name, world_id, epoch, rect}.
 *
 * A screenshot attachment is {type: "image", artifact_id, sha256, width, height, tick, world_id,
 * epoch, stale, generation_known}; it has no tile, no file path and no pixels of its own, so the
 * transcript reserves the box of its source aspect ratio and fetches the pixels separately.
 *
 * An attachment is {type, id, generation, name, tile, world_id, epoch, stale, generation_known}:
 * the identity of a typed reference is kept as the backend reported it. "tile" is the array
 * [x, y] in this description; the payload sent to the backend names the tile as the canonical
 * object {"x": x, "y": y}, and both shapes are accepted when a reference is read. "stale" marks a reference
 * that belongs to another world or epoch than the current one, and "generation_known" is false
 * for a pooled object (town, station, industry, vehicle, sign) whose generation the backend did
 * not report, because such an id may already belong to another object. Both cases are labelled
 * in the UI ("(stale)" / "(unverified)") and are still sent, the backend resolves them.
 * - "status_window": object {left, top, text, stop} with the position of the status window and
 *   the screen rectangles of its widgets, or null when it is not open.
 * - "chat_ack": the timing of the last chat Send acknowledgement, the same object that world.info
 *   reports as `timing.chat_ack` (see AgentChatSendAck::ToJson()). It is bound to the world and
 *   epoch it was measured in and is never mixed with a clock of the backend process.
 *
 * A rectangle is an object {left, top, right, bottom} in screen pixels.
 *
 * @return The description of the agent chat UI.
 */
nlohmann::json DescribeAgentChatUi();

#endif /* AGENT_CHAT_GUI_H */
