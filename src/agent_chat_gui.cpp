/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file agent_chat_gui.cpp Native in-game agent chat window. */

#include "stdafx.h"

#include "agent_chat_gui.h"
#include "agent_bridge.h"

#include "company_func.h"
#include "core/backup_type.hpp"
#include "core/enum_type.hpp"
#include "core/geometry_func.hpp"
#include "core/string_consumer.hpp"
#include "core/utf8.hpp"
#include "gfx_func.h"
#include "map_func.h"
#include "string_func.h"
#include "strings_func.h"
#include "tilehighlight_func.h"
#include "timer/timer.h"
#include "timer/timer_window.h"
#include "viewport_func.h"
#include "window_func.h"
#include "window_gui.h"
#include "zoom_func.h"

#include "widgets/agent_chat_widget.h"

#include "3rdparty/fmt/format.h"

#include "table/sprites.h"
#include "table/strings.h"

#include "safeguards.h"

/** Provided by the OS specific backends, also declared locally in textbuf.cpp. */
std::optional<std::string> GetClipboardContents();

using Json = nlohmann::json;

/** Maximum number of bytes accepted in the message input. */
static const size_t AGENT_CHAT_MAX_INPUT = 8000;

/** Interval between two polls of the backend, in milliseconds. */
static const uint AGENT_CHAT_POLL_INTERVAL_MS = 1000;

/** Number of text lines shown in the message input. */
static const int AGENT_CHAT_INPUT_LINES = 3;

/** Number of text lines shown in the attachment tray. */
static const int AGENT_CHAT_TRAY_LINES = 2;

/** One typed game reference attached to a chat message. */
struct AgentChatAttachment {
	std::string type;               ///< Reference type, e.g. "point", "town" or "vehicle".
	int32_t id = -1;                ///< Object id of an object reference, -1 for points and tiles.
	TileIndex tile = INVALID_TILE;  ///< Tile of a point or tile reference, or the location of an object.
	std::string name;               ///< Human readable name, shown in the attachment tray.
	std::string what;               ///< Extra classification of a tile reference.
};

/** One message of a chat transcript. */
struct AgentChatMessage {
	int64_t seq = 0;                ///< Backend sequence number of the message.
	std::string role;               ///< "user", "assistant", "system" or "tool".
	std::string text;               ///< Message text, UTF-8 encoded.
	std::string status;             ///< Optional status of the message, e.g. "failed".
	std::vector<AgentChatAttachment> attachments; ///< Attachments of a user message.
};

/** One chat conversation with its locally cached transcript. */
struct AgentChatEntry {
	std::string id;                 ///< Backend chat id.
	std::string title;              ///< Chat title.
	int64_t message_seq = 0;        ///< Highest message sequence number seen so far.
	int64_t event_seq = 0;          ///< Highest event sequence number seen so far.
	std::vector<AgentChatMessage> messages; ///< Cached transcript.
	bool task_active = false;       ///< Whether a task is running for this chat.
	std::string task_status;        ///< Short status of the running task, shown to the player.
};

/** All state of the in-game agent chat UI. */
struct AgentChatUiState {
	std::vector<AgentChatEntry> chats;      ///< Known chats of the current world.
	int active = -1;                        ///< Index of the selected chat, or -1.
	std::vector<AgentChatAttachment> tray;  ///< Attachments of the message being composed.
	std::string input;                      ///< Text of the message being composed, UTF-8 encoded.
	size_t caret = 0;                       ///< Caret position in the input, in bytes.
	int scroll = 0;                         ///< Scroll offset of the transcript, in pixels.
	int content_height = 0;                 ///< Height of the transcript content, in pixels.
	bool picking_point = false;             ///< Waiting for a map click to add a point.
	bool picking_object = false;            ///< Waiting for a map click to add an object.
	Json candidates = Json::array();        ///< Object candidates of the last map click.
	bool candidates_open = false;           ///< Whether the object disambiguation list is shown.
	std::string error;                      ///< Last error shown to the player.
	int pending_requests = 0;               ///< Number of requests in flight.
	std::string world_id;                   ///< World id of the current game.
	std::string epoch;                      ///< Load epoch of the current game.
	std::string connection_id;              ///< Backend connection id of the current game.
	bool connected = false;                 ///< Whether the game is connected to the backend.
	bool chats_loaded = false;              ///< Whether the chat list was loaded for this world.
	std::chrono::steady_clock::time_point next_poll{}; ///< Time of the next poll.
};

static AgentChatUiState _agent_chat;

/* --- Small helpers --- */

/**
 * Read a string field of a JSON object, tolerating missing and non-string values.
 * @param j The object to read from.
 * @param key The field name.
 * @param fallback Value returned when the field is absent or not convertible.
 * @return The field value.
 */
static std::string JsonString(const Json &j, const char *key, std::string fallback = {})
{
	if (!j.is_object()) return fallback;
	const auto it = j.find(key);
	if (it == j.end() || it->is_null()) return fallback;
	if (it->is_string()) return it->get<std::string>();
	if (it->is_number_integer()) return fmt::format("{}", it->get<int64_t>());
	if (it->is_number_unsigned()) return fmt::format("{}", it->get<uint64_t>());
	return fallback;
}

/**
 * Read an integer field of a JSON object, tolerating missing and non-integer values.
 * @param j The object to read from.
 * @param key The field name.
 * @param fallback Value returned when the field is absent or not convertible.
 * @return The field value.
 */
static int64_t JsonInt(const Json &j, const char *key, int64_t fallback = 0)
{
	if (!j.is_object()) return fallback;
	const auto it = j.find(key);
	if (it == j.end() || it->is_null()) return fallback;
	if (it->is_number_integer()) return it->get<int64_t>();
	if (it->is_number_unsigned()) return static_cast<int64_t>(it->get<uint64_t>());
	if (it->is_number_float()) return static_cast<int64_t>(it->get<double>());
	if (it->is_string()) {
		const std::optional<int64_t> value = ParseInteger<int64_t>(it->get<std::string>());
		return value.has_value() ? *value : fallback;
	}
	return fallback;
}

/**
 * Read a boolean field of a JSON object, tolerating missing and non-boolean values.
 * @param j The object to read from.
 * @param key The field name.
 * @param fallback Value returned when the field is absent or not convertible.
 * @return The field value.
 */
static bool JsonBool(const Json &j, const char *key, bool fallback = false)
{
	if (!j.is_object()) return fallback;
	const auto it = j.find(key);
	if (it == j.end() || it->is_null()) return fallback;
	if (it->is_boolean()) return it->get<bool>();
	if (it->is_number_integer()) return it->get<int64_t>() != 0;
	return fallback;
}

/**
 * Get the array stored in a field of a JSON object, or an empty array.
 * @param j The object to read from.
 * @param key The field name.
 * @return The array.
 */
static const Json &JsonArray(const Json &j, const char *key)
{
	static const Json empty = Json::array();
	if (!j.is_object()) return empty;
	const auto it = j.find(key);
	if (it == j.end() || !it->is_array()) return empty;
	return *it;
}

/**
 * Get the human readable name of an attachment type.
 * @param type The attachment type.
 * @return The name of the type.
 */
static std::string GetAttachmentTypeName(const std::string &type)
{
	if (type == "point") return GetString(STR_AGENT_CHAT_TYPE_POINT);
	if (type == "tile") return GetString(STR_AGENT_CHAT_TYPE_TILE);
	if (type == "town") return GetString(STR_AGENT_CHAT_TYPE_TOWN);
	if (type == "station") return GetString(STR_AGENT_CHAT_TYPE_STATION);
	if (type == "industry") return GetString(STR_AGENT_CHAT_TYPE_INDUSTRY);
	if (type == "vehicle") return GetString(STR_AGENT_CHAT_TYPE_VEHICLE);
	if (type == "sign") return GetString(STR_AGENT_CHAT_TYPE_SIGN);
	if (type == "company") return GetString(STR_AGENT_CHAT_TYPE_COMPANY);
	return GetString(STR_AGENT_CHAT_TYPE_UNKNOWN);
}

/**
 * Get the label of an attachment, as shown in the attachment tray.
 * @param attachment The attachment.
 * @return The label.
 */
static std::string GetAttachmentLabel(const AgentChatAttachment &attachment)
{
	if (!attachment.name.empty()) return attachment.name;
	if (attachment.tile != INVALID_TILE) return GetString(STR_AGENT_CHAT_ATTACHMENT_POINT, TileX(attachment.tile), TileY(attachment.tile));
	if (attachment.id >= 0) return GetString(STR_AGENT_CHAT_ATTACHMENT_OBJECT, GetAttachmentTypeName(attachment.type), attachment.id);
	return GetAttachmentTypeName(attachment.type);
}

/**
 * Build a one line summary of the attachments of a message.
 * @param attachments The attachments.
 * @return The summary.
 */
static std::string GetAttachmentSummary(const std::vector<AgentChatAttachment> &attachments)
{
	std::string summary;
	for (const AgentChatAttachment &attachment : attachments) {
		if (!summary.empty()) summary += ", ";
		summary += GetAttachmentLabel(attachment);
	}
	return summary;
}

/**
 * Convert an attachment to the JSON representation used by the backend.
 * @param attachment The attachment.
 * @return The JSON object.
 */
static Json AttachmentToJson(const AgentChatAttachment &attachment)
{
	Json j = Json::object();
	j["type"] = attachment.type;
	if (attachment.id >= 0) j["id"] = attachment.id;
	if (attachment.tile != INVALID_TILE) j["tile"] = Json::array({TileX(attachment.tile), TileY(attachment.tile)});
	if (!attachment.name.empty()) j["name"] = attachment.name;
	if (!attachment.what.empty()) j["what"] = attachment.what;
	return j;
}

/**
 * Convert a JSON attachment to an attachment.
 * @param j The JSON object.
 * @return The attachment.
 */
static AgentChatAttachment ParseAttachment(const Json &j)
{
	AgentChatAttachment attachment;
	attachment.type = JsonString(j, "type", "point");
	attachment.id = static_cast<int32_t>(JsonInt(j, "id", -1));
	attachment.name = JsonString(j, "name");
	attachment.what = JsonString(j, "what");

	const Json &tile = j.is_object() && j.contains("tile") ? j["tile"] : Json();
	if (tile.is_array() && tile.size() == 2 && tile[0].is_number() && tile[1].is_number()) {
		const uint x = tile[0].get<uint>();
		const uint y = tile[1].get<uint>();
		if (x < Map::SizeX() && y < Map::SizeY()) attachment.tile = TileXY(x, y);
	}
	return attachment;
}

/**
 * Convert a JSON object to a screen rectangle.
 * @param r The rectangle.
 * @return The JSON object.
 */
static Json RectToJson(const Rect &r)
{
	Json j = Json::object();
	j["left"] = r.left;
	j["top"] = r.top;
	j["right"] = r.right;
	j["bottom"] = r.bottom;
	return j;
}

/**
 * Get the selected chat.
 * @return The selected chat, or \c nullptr when there is none.
 */
static AgentChatEntry *GetActiveChat()
{
	if (_agent_chat.active < 0 || static_cast<size_t>(_agent_chat.active) >= _agent_chat.chats.size()) return nullptr;
	return &_agent_chat.chats[_agent_chat.active];
}

/**
 * Find a chat by its backend id.
 * @param id The chat id.
 * @return The chat, or \c nullptr when it is unknown.
 */
static AgentChatEntry *FindChatById(const std::string &id)
{
	for (AgentChatEntry &chat : _agent_chat.chats) {
		if (chat.id == id) return &chat;
	}
	return nullptr;
}

/**
 * Find the chat with a running task.
 * @return The chat with a running task, or \c nullptr when no task is running.
 */
static AgentChatEntry *FindBusyChat()
{
	for (AgentChatEntry &chat : _agent_chat.chats) {
		if (chat.task_active) return &chat;
	}
	return nullptr;
}

/** Mark the agent chat windows dirty. */
static void InvalidateAgentChatWindows()
{
	SetWindowClassesDirty(WindowClass::AgentChat);
	SetWindowClassesDirty(WindowClass::AgentChatStatus);
}

/**
 * Get the status text of a chat.
 * @param chat The chat.
 * @return The status text.
 */
static std::string GetChatStatusText(const AgentChatEntry &chat)
{
	if (chat.task_active) return chat.task_status.empty() ? GetString(STR_AGENT_CHAT_STATUS_THINKING) : chat.task_status;
	return GetString(STR_AGENT_CHAT_STATUS_IDLE);
}

/* --- Transport --- */

static void AgentChatLoadMessages(const std::string &chat_id);
static void AgentChatPollEvents(const std::string &chat_id);

/**
 * Send a request to the agent backend.
 * @param method HTTP method, e.g. "GET" or "POST".
 * @param path Request path, including the query string.
 * @param body Request body, ignored for GET requests.
 * @param on_success Called on the main thread with the response of a successful request.
 */
static void AgentChatRequest(const std::string &method, const std::string &path, const Json &body, std::function<void(const Json &)> on_success)
{
	_agent_chat.pending_requests++;
	InGameAgent::Request(method, path, body, [on_success = std::move(on_success)](Json response, bool ok) {
		_agent_chat.pending_requests--;
		if (!ok) {
			std::string message = JsonString(response, "message");
			if (message.empty() && response.is_object() && response.contains("error")) message = JsonString(response["error"], "message");
			if (message.empty()) message = JsonString(response, "error");
			if (message.empty()) message = GetString(STR_AGENT_CHAT_ERROR_UNKNOWN);
			_agent_chat.error = GetString(STR_AGENT_CHAT_ERROR, message);
			InvalidateAgentChatWindows();
			return;
		}
		_agent_chat.error.clear();
		on_success(response);
		InvalidateAgentChatWindows();
	});
}

/** Read the world context of the transport, resetting the chat list when the world changed. */
static void AgentChatSyncContext()
{
	const Json context = InGameAgent::Context();
	const std::string world_id = JsonString(context, "world_id");
	const std::string epoch = JsonString(context, "epoch");
	const bool connected = JsonBool(context, "connected");

	if (world_id != _agent_chat.world_id || epoch != _agent_chat.epoch) {
		_agent_chat.chats.clear();
		_agent_chat.active = -1;
		_agent_chat.chats_loaded = false;
		_agent_chat.candidates = Json::array();
		_agent_chat.candidates_open = false;
		_agent_chat.error.clear();

		/* Typed attachments refer to objects of the old world, so they cannot be sent to the new one. */
		_agent_chat.tray.clear();
		if (_agent_chat.picking_point || _agent_chat.picking_object) {
			ResetObjectToPlace();
			_agent_chat.picking_point = false;
			_agent_chat.picking_object = false;
		}
		/* The text draft does not refer to the world, so it is kept on purpose. */
	}

	_agent_chat.world_id = world_id;
	_agent_chat.epoch = epoch;
	_agent_chat.connection_id = JsonString(context, "connection_id");
	_agent_chat.connected = connected;
}

/** Load the chat list of the current world. */
static void AgentChatLoadChats()
{
	if (!_agent_chat.connected || _agent_chat.world_id.empty()) return;

	const std::string world_id = _agent_chat.world_id;
	const std::string epoch = _agent_chat.epoch;
	const std::string path = "/v1/chats?world_id=" + world_id + "&epoch=" + epoch;

	AgentChatRequest("GET", path, Json(), [world_id, epoch](const Json &response) {
		/* Ignore the answer when the world changed while the request was in flight. */
		if (world_id != _agent_chat.world_id || epoch != _agent_chat.epoch) return;

		const std::string previous = GetActiveChat() != nullptr ? GetActiveChat()->id : std::string();
		_agent_chat.chats.clear();
		_agent_chat.active = -1;

		for (const Json &entry : JsonArray(response, "chats")) {
			AgentChatEntry chat;
			chat.id = JsonString(entry, "chat_id", JsonString(entry, "id"));
			if (chat.id.empty()) continue;
			chat.title = JsonString(entry, "title");
			chat.task_active = JsonBool(entry, "task_active");
			_agent_chat.chats.push_back(std::move(chat));
		}

		_agent_chat.chats_loaded = true;

		for (size_t i = 0; i < _agent_chat.chats.size(); i++) {
			if (_agent_chat.chats[i].id == previous) {
				_agent_chat.active = static_cast<int>(i);
				break;
			}
		}
		if (_agent_chat.active < 0 && !_agent_chat.chats.empty()) _agent_chat.active = 0;

		if (GetActiveChat() != nullptr) AgentChatLoadMessages(GetActiveChat()->id);
	});
}

/**
 * Create a new chat and select it.
 * @param then Called after the chat was created and selected.
 */
static void AgentChatCreateChat(std::function<void()> then = {})
{
	if (!_agent_chat.connected) {
		_agent_chat.error = GetString(STR_AGENT_CHAT_NOT_CONNECTED);
		InvalidateAgentChatWindows();
		return;
	}

	const std::string world_id = _agent_chat.world_id;
	const std::string epoch = _agent_chat.epoch;

	Json body = Json::object();
	body["world_id"] = world_id;
	body["epoch"] = epoch;
	if (!_agent_chat.connection_id.empty()) body["connection_id"] = _agent_chat.connection_id;
	body["title"] = GetString(STR_AGENT_CHAT_DEFAULT_TITLE, static_cast<int32_t>(_agent_chat.chats.size() + 1));
	body["company_id"] = static_cast<int32_t>(_local_company.base());

	AgentChatRequest("POST", "/v1/chats", body, [then = std::move(then), world_id, epoch](const Json &response) {
		/* Ignore the answer when the world changed while the request was in flight. */
		if (world_id != _agent_chat.world_id || epoch != _agent_chat.epoch) return;

		const Json &chat_json = response.is_object() && response.contains("chat") ? response["chat"] : response;

		AgentChatEntry chat;
		chat.id = JsonString(chat_json, "chat_id", JsonString(chat_json, "id"));
		if (chat.id.empty()) {
			_agent_chat.error = GetString(STR_AGENT_CHAT_ERROR_UNKNOWN);
			return;
		}
		chat.title = JsonString(chat_json, "title", GetString(STR_AGENT_CHAT_DEFAULT_TITLE, static_cast<int32_t>(_agent_chat.chats.size() + 1)));

		_agent_chat.chats.push_back(std::move(chat));
		_agent_chat.active = static_cast<int>(_agent_chat.chats.size() - 1);
		_agent_chat.chats_loaded = true;

		if (then) then();
	});
}

/**
 * Make sure a chat is selected, creating one when needed.
 * @param then Called after a chat is selected.
 */
static void AgentChatEnsureChat(std::function<void()> then)
{
	if (GetActiveChat() != nullptr) {
		then();
		return;
	}
	AgentChatCreateChat(std::move(then));
}

/**
 * Load the messages of a chat that were not seen yet.
 * @param chat_id The chat id.
 */
static void AgentChatLoadMessages(const std::string &chat_id)
{
	const AgentChatEntry *chat = FindChatById(chat_id);
	if (chat == nullptr) return;

	const std::string path = "/v1/chats/" + chat_id + "/messages?after=" + fmt::format("{}", chat->message_seq);

	AgentChatRequest("GET", path, Json(), [chat_id](const Json &response) {
		AgentChatEntry *chat = FindChatById(chat_id);
		if (chat == nullptr) return;

		bool any = false;
		for (const Json &entry : JsonArray(response, "messages")) {
			AgentChatMessage message;
			message.seq = JsonInt(entry, "seq", 0);
			message.role = JsonString(entry, "role", "assistant");
			message.text = JsonString(entry, "text");
			message.status = JsonString(entry, "status");
			for (const Json &attachment : JsonArray(entry, "attachments")) {
				message.attachments.push_back(ParseAttachment(attachment));
			}

			/* Skip messages that were already received. */
			if (message.seq != 0 && message.seq <= chat->message_seq) continue;

			if (message.seq > chat->message_seq) chat->message_seq = message.seq;
			chat->messages.push_back(std::move(message));
			any = true;
		}

		/* Fall back to the cursor of the backend when the messages carry no sequence number. */
		if (!any && response.is_object() && response.contains("next_seq")) {
			chat->message_seq = std::max(chat->message_seq, JsonInt(response, "next_seq", chat->message_seq));
		}

		/* Keep the transcript pinned to the bottom while new messages arrive. */
		if (any) _agent_chat.scroll = INT32_MAX;
	});
}

/**
 * Apply one event of a chat to the local task state.
 * @param chat The chat the event belongs to.
 * @param event The event.
 */
static void AgentChatApplyEvent(AgentChatEntry &chat, const Json &event)
{
	const std::string type = JsonString(event, "type");

	if (type == "turn_started" || type == "task_started") {
		chat.task_active = true;
		chat.task_status = GetString(STR_AGENT_CHAT_STATUS_THINKING);
	} else if (type == "tool_call") {
		chat.task_active = true;
		const std::string tool = JsonString(event, "tool", JsonString(event, "name"));
		chat.task_status = tool.empty() ? GetString(STR_AGENT_CHAT_STATUS_THINKING) : GetString(STR_AGENT_CHAT_STATUS_TOOL, tool);
	} else if (type == "tool_result") {
		chat.task_active = true;
		chat.task_status = GetString(STR_AGENT_CHAT_STATUS_THINKING);
	} else if (type == "turn_completed" || type == "task_completed") {
		chat.task_active = false;
		chat.task_status = GetString(STR_AGENT_CHAT_STATUS_DONE);
	} else if (type == "turn_failed" || type == "task_failed") {
		chat.task_active = false;
		chat.task_status = GetString(STR_AGENT_CHAT_STATUS_FAILED);
	} else if (type == "turn_stopped" || type == "task_stopped" || type == "stopped") {
		chat.task_active = false;
		chat.task_status = GetString(STR_AGENT_CHAT_STATUS_STOPPED);
	} else if (type == "sub_unavailable" || type == "backend_unavailable") {
		chat.task_active = false;
		chat.task_status = GetString(STR_AGENT_CHAT_STATUS_UNAVAILABLE);
	} else if (type == "epoch_changed" || type == "world_changed") {
		_agent_chat.chats_loaded = false;
	}
}

/**
 * Poll the events of a chat.
 * @param chat_id The chat id.
 */
static void AgentChatPollEvents(const std::string &chat_id)
{
	const AgentChatEntry *chat = FindChatById(chat_id);
	if (chat == nullptr) return;

	const std::string path = "/v1/chats/" + chat_id + "/events?after=" + fmt::format("{}", chat->event_seq);

	AgentChatRequest("GET", path, Json(), [chat_id](const Json &response) {
		AgentChatEntry *chat = FindChatById(chat_id);
		if (chat == nullptr) return;

		bool any = false;
		for (const Json &event : JsonArray(response, "events")) {
			const int64_t seq = JsonInt(event, "seq", 0);
			if (seq != 0 && seq <= chat->event_seq) continue;
			if (seq > chat->event_seq) chat->event_seq = seq;
			AgentChatApplyEvent(*chat, event);
			any = true;
		}

		if (!any && response.is_object() && response.contains("next_seq")) {
			chat->event_seq = std::max(chat->event_seq, JsonInt(response, "next_seq", chat->event_seq));
		}
	});
}

/** Send the composed message to the selected chat. */
static void AgentChatSend()
{
	if (_agent_chat.input.empty() && _agent_chat.tray.empty()) return;

	AgentChatEnsureChat([]() {
		AgentChatEntry *chat = GetActiveChat();
		if (chat == nullptr) return;

		const std::string chat_id = chat->id;
		const std::string world_id = _agent_chat.world_id;
		const std::string epoch = _agent_chat.epoch;
		const std::string text = _agent_chat.input;
		const std::vector<AgentChatAttachment> tray = _agent_chat.tray;

		Json body = Json::object();
		body["text"] = text;
		Json attachments = Json::array();
		for (const AgentChatAttachment &attachment : tray) attachments.push_back(AttachmentToJson(attachment));
		body["attachments"] = attachments;
		body["client_message_id"] = fmt::format("{}", std::chrono::steady_clock::now().time_since_epoch().count());

		AgentChatRequest("POST", "/v1/chats/" + chat_id + "/messages", body, [chat_id, world_id, epoch](const Json &) {
			/* Ignore the answer when the world changed while the request was in flight. */
			if (world_id != _agent_chat.world_id || epoch != _agent_chat.epoch) return;

			AgentChatEntry *chat = FindChatById(chat_id);
			if (chat != nullptr) {
				chat->task_active = true;
				chat->task_status = GetString(STR_AGENT_CHAT_STATUS_THINKING);
			}
			_agent_chat.input.clear();
			_agent_chat.caret = 0;
			_agent_chat.tray.clear();
			_agent_chat.scroll = INT32_MAX;
			AgentChatLoadMessages(chat_id);
			AgentChatPollEvents(chat_id);
		});
	});
}

/**
 * Stop the running task of a chat.
 * @param chat_id The chat id, or an empty string to stop the task of the selected chat.
 */
static void AgentChatStop(const std::string &chat_id)
{
	std::string id = chat_id;
	if (id.empty()) {
		const AgentChatEntry *busy = FindBusyChat();
		if (busy != nullptr) {
			id = busy->id;
		} else {
			const AgentChatEntry *chat = GetActiveChat();
			if (chat == nullptr) return;
			id = chat->id;
		}
	}

	AgentChatRequest("POST", "/v1/chats/" + id + "/stop", Json::object(), [id](const Json &) {
		AgentChatEntry *chat = FindChatById(id);
		if (chat != nullptr) {
			chat->task_active = false;
			chat->task_status = GetString(STR_AGENT_CHAT_STATUS_STOPPED);
		}
		AgentChatPollEvents(id);
	});
}

/* --- Layout helpers --- */

/**
 * Get the height of one row of the chat list.
 * @return The row height.
 */
static int GetChatRowHeight()
{
	return GetCharacterHeight(FontSize::Normal) + WidgetDimensions::scaled.vsep_normal;
}

/**
 * Get the height of one message of the transcript.
 * @param message The message.
 * @param width Available width.
 * @return The height.
 */
static int GetMessageHeight(const AgentChatMessage &message, int width)
{
	int height = GetCharacterHeight(FontSize::Normal);
	if (!message.text.empty()) height += GetStringHeight(message.text, width);
	if (!message.attachments.empty()) height += GetStringHeight(GetAttachmentSummary(message.attachments), width);
	return height + WidgetDimensions::scaled.vsep_normal;
}

/**
 * Get the total height of the transcript of a chat.
 * @param chat The chat, may be \c nullptr.
 * @param width Available width.
 * @return The height.
 */
static int GetTranscriptContentHeight(const AgentChatEntry *chat, int width)
{
	if (chat == nullptr) return 0;
	int height = 0;
	for (const AgentChatMessage &message : chat->messages) height += GetMessageHeight(message, width);
	return height;
}

/**
 * Lay out the messages of the transcript.
 * @param chat The chat, may be \c nullptr.
 * @param area The transcript area.
 * @param scroll The scroll offset in pixels.
 * @return The rectangle of every message, in screen coordinates.
 */
static std::vector<Rect> LayoutTranscript(const AgentChatEntry *chat, const Rect &area, int scroll)
{
	std::vector<Rect> result;
	if (chat == nullptr) return result;

	int y = area.top - scroll;
	for (const AgentChatMessage &message : chat->messages) {
		const int height = GetMessageHeight(message, area.Width());
		result.push_back(Rect{area.left, y, area.right, y + height - 1});
		y += height;
	}
	return result;
}

/**
 * Lay out the attachment chips of the tray.
 * @param area The tray area.
 * @return The rectangle of every chip, in screen coordinates.
 */
static std::vector<Rect> LayoutTrayChips(const Rect &area)
{
	std::vector<Rect> result;
	int x = area.left;
	int y = area.top;
	const int height = GetCharacterHeight(FontSize::Normal) + WidgetDimensions::scaled.vsep_normal;

	for (const AgentChatAttachment &attachment : _agent_chat.tray) {
		const int width = GetStringBoundingBox(GetAttachmentLabel(attachment)).width + WidgetDimensions::scaled.hsep_wide * 2 + GetCharacterHeight(FontSize::Normal);
		if (x + width > area.right && x > area.left) {
			x = area.left;
			y += height;
		}
		if (y + height - 1 > area.bottom) break;
		result.push_back(Rect{x, y, x + width - 1, y + height - 1});
		x += width + WidgetDimensions::scaled.hsep_normal;
	}
	return result;
}

/**
 * Lay out the rows of the object disambiguation list.
 * @param area The transcript area the list is drawn in.
 * @return The rectangle of every candidate, in screen coordinates.
 */
static std::vector<Rect> LayoutCandidates(const Rect &area)
{
	std::vector<Rect> result;
	if (!_agent_chat.candidates_open) return result;

	const int row_height = GetChatRowHeight();
	const int count = static_cast<int>(_agent_chat.candidates.size());
	const int height = (count + 1) * row_height + WidgetDimensions::scaled.framerect.Vertical();
	const int top = std::max(area.top, area.bottom - height + 1);

	int y = top + row_height + WidgetDimensions::scaled.framerect.top;
	for (int i = 0; i < count; i++) {
		result.push_back(Rect{area.left, y, area.right, y + row_height - 1});
		y += row_height;
	}
	return result;
}

/* --- Windows --- */

/** The native in-game agent chat window. */
struct AgentChatWindow : Window {
	Scrollbar *chat_scroll = nullptr;       ///< Scrollbar of the chat list.
	Scrollbar *transcript_scroll = nullptr; ///< Scrollbar of the transcript.

	AgentChatWindow(WindowDesc &desc) : Window(desc)
	{
		this->CreateNestedTree();
		this->chat_scroll = this->GetScrollbar(WID_AC_CHAT_SCROLL);
		this->transcript_scroll = this->GetScrollbar(WID_AC_TRANSCRIPT_SCROLL);
		this->FinishInitNested(0);
		this->UpdateWidgetStates();
		this->UpdateScrollbars();
	}

	/**
	 * Get the usable area of a panel widget.
	 * @param widget The widget.
	 * @return The area inside the frame of the widget.
	 */
	Rect GetInnerRect(WidgetID widget) const
	{
		return this->GetWidget<NWidgetBase>(widget)->GetCurrentRect().Shrink(WidgetDimensions::scaled.framerect);
	}

	/** Update the enabled and lowered state of the widgets. */
	void UpdateWidgetStates()
	{
		const AgentChatEntry *chat = GetActiveChat();
		const bool busy = chat != nullptr && chat->task_active;

		this->SetWidgetDisabledState(WID_AC_SEND, !_agent_chat.connected || (_agent_chat.input.empty() && _agent_chat.tray.empty()));
		this->SetWidgetDisabledState(WID_AC_STOP, !busy);
		this->SetWidgetDisabledState(WID_AC_NEW_CHAT, !_agent_chat.connected);
		this->SetWidgetLoweredState(WID_AC_ADD_POINT, _agent_chat.picking_point);
		this->SetWidgetLoweredState(WID_AC_ADD_OBJECT, _agent_chat.picking_object);
	}

	/** Update the range and position of the scrollbars. */
	void UpdateScrollbars()
	{
		const Rect transcript = this->GetInnerRect(WID_AC_TRANSCRIPT);
		const AgentChatEntry *chat = GetActiveChat();
		_agent_chat.content_height = GetTranscriptContentHeight(chat, transcript.Width());

		this->transcript_scroll->SetCapacity(std::max(1, transcript.Height()));
		this->transcript_scroll->SetCount(std::max<int>(transcript.Height(), _agent_chat.content_height));
		this->transcript_scroll->SetStepSize(std::max(1, GetCharacterHeight(FontSize::Normal) * 2));
		this->transcript_scroll->SetPosition(std::max(0, _agent_chat.scroll));
		_agent_chat.scroll = this->transcript_scroll->GetPosition();

		const Rect list = this->GetInnerRect(WID_AC_CHAT_LIST);
		const int rows = std::max(1, list.Height() / GetChatRowHeight());
		this->chat_scroll->SetCapacity(rows);
		this->chat_scroll->SetCount(std::max<int>(rows, _agent_chat.chats.size()));
		this->chat_scroll->SetStepSize(1);
	}

	void OnPaint() override
	{
		this->DrawWidgets();
	}

	void DrawWidget(const Rect &r, WidgetID widget) const override
	{
		switch (widget) {
			case WID_AC_CHAT_LIST: this->DrawChatList(r); break;
			case WID_AC_TRANSCRIPT: this->DrawTranscript(r); break;
			case WID_AC_TRAY: this->DrawTray(r); break;
			case WID_AC_INPUT: this->DrawInput(r); break;
			case WID_AC_STATUS: this->DrawStatus(r); break;
			default: break;
		}
	}

	/**
	 * Draw the list of chats.
	 * @param r The area of the chat list widget.
	 */
	void DrawChatList(const Rect &r) const
	{
		const Rect area = r.Shrink(WidgetDimensions::scaled.framerect);
		const int row_height = GetChatRowHeight();
		const int first = this->chat_scroll->GetPosition();
		const int last = std::min<int>(_agent_chat.chats.size(), first + this->chat_scroll->GetCapacity());

		if (_agent_chat.chats.empty()) {
			DrawStringMultiLine(area, GetString(STR_AGENT_CHAT_NO_CHATS), TextColour::Grey);
			return;
		}

		int y = area.top;
		for (int i = first; i < last; i++) {
			const AgentChatEntry &chat = _agent_chat.chats[i];
			const Rect row = Rect{area.left, y, area.right, y + row_height - 1};
			if (i == _agent_chat.active) GfxFillRect(row, PC_DARK_BLUE);
			if (chat.task_active) GfxFillRect(Rect{row.left, row.top, row.left + ScaleGUITrad(2), row.bottom}, PC_ORANGE);

			std::string title = chat.title.empty() ? chat.id : chat.title;
			DrawString(row.left + ScaleGUITrad(4), row.right, row.top, title, i == _agent_chat.active ? TextColour::White : TextColour::Black);
			y += row_height;
		}
	}

	/**
	 * Draw the transcript of the selected chat.
	 * @param r The area of the transcript widget.
	 */
	void DrawTranscript(const Rect &r) const
	{
		const Rect area = r.Shrink(WidgetDimensions::scaled.framerect);
		const AgentChatEntry *chat = GetActiveChat();

		DrawPixelInfo tmp_dpi;
		if (!FillDrawPixelInfo(&tmp_dpi, area)) return;
		tmp_dpi.left += area.left;
		tmp_dpi.top += area.top;
		AutoRestoreBackup dpi_backup(_cur_dpi, &tmp_dpi);

		if (chat == nullptr) {
			DrawStringMultiLine(area, GetString(STR_AGENT_CHAT_NO_CHATS), TextColour::Grey);
			return;
		}

		if (chat->messages.empty()) {
			DrawStringMultiLine(area, GetString(STR_AGENT_CHAT_EMPTY_TRANSCRIPT), TextColour::Grey);
		}

		const std::vector<Rect> layout = LayoutTranscript(chat, area, _agent_chat.scroll);
		for (size_t i = 0; i < layout.size(); i++) {
			const Rect &rect = layout[i];
			if (rect.bottom < area.top || rect.top > area.bottom) continue;

			const AgentChatMessage &message = chat->messages[i];
			const bool is_user = message.role == "user";
			const TextColour colour = is_user ? TextColour::White : (message.role == "system" ? TextColour::Grey : TextColour::Cream);

			std::string header = is_user ? GetString(STR_AGENT_CHAT_ROLE_USER) : (message.role == "system" ? GetString(STR_AGENT_CHAT_ROLE_SYSTEM) : (message.role == "tool" ? GetString(STR_AGENT_CHAT_ROLE_TOOL) : GetString(STR_AGENT_CHAT_ROLE_ASSISTANT)));
			if (!message.status.empty()) header += GetString(STR_AGENT_CHAT_MESSAGE_STATUS, message.status);
			DrawString(rect.left, rect.right, rect.top, header, is_user ? TextColour::LightBlue : TextColour::Gold);

			int y = rect.top + GetCharacterHeight(FontSize::Normal);
			if (!message.text.empty()) {
				y = DrawStringMultiLine(rect.left, rect.right, y, rect.bottom, message.text, colour);
			}
			if (!message.attachments.empty()) {
				DrawStringMultiLine(rect.left, rect.right, y, rect.bottom, GetAttachmentSummary(message.attachments), TextColour::Grey);
			}
		}

		this->DrawCandidates(area);
	}

	/**
	 * Draw the object disambiguation list on top of the transcript.
	 * @param area The transcript area.
	 */
	void DrawCandidates(const Rect &area) const
	{
		const std::vector<Rect> rows = LayoutCandidates(area);
		if (rows.empty()) return;

		const Rect panel = Rect{area.left, rows.front().top - GetChatRowHeight() - WidgetDimensions::scaled.framerect.top, area.right, rows.back().bottom + WidgetDimensions::scaled.framerect.bottom};
		GfxFillRect(panel, PC_DARK_BLUE);
		DrawString(panel.left + ScaleGUITrad(2), panel.right, panel.top, GetString(STR_AGENT_CHAT_CANDIDATES_CAPTION), TextColour::White);

		for (size_t i = 0; i < rows.size(); i++) {
			const Json &candidate = _agent_chat.candidates[i];
			std::string label = JsonString(candidate, "name");
			if (label.empty()) label = GetString(STR_AGENT_CHAT_ATTACHMENT_OBJECT, GetAttachmentTypeName(JsonString(candidate, "type", "object")), JsonInt(candidate, "id", -1));
			DrawString(rows[i].left + ScaleGUITrad(4), rows[i].right, rows[i].top, label, TextColour::Cream);
		}
	}

	/**
	 * Draw the attachment tray.
	 * @param r The area of the tray widget.
	 */
	void DrawTray(const Rect &r) const
	{
		const Rect area = r.Shrink(WidgetDimensions::scaled.framerect);
		if (_agent_chat.tray.empty()) {
			DrawString(area.left, area.right, area.top, GetString(STR_AGENT_CHAT_TRAY_EMPTY), TextColour::Grey);
			return;
		}

		const std::vector<Rect> chips = LayoutTrayChips(area);
		for (size_t i = 0; i < chips.size(); i++) {
			const Rect &chip = chips[i];
			DrawFrameRect(chip, Colours::Grey, FrameFlag::Lowered);
			const int remove_width = GetCharacterHeight(FontSize::Normal);
			DrawString(chip.left + WidgetDimensions::scaled.hsep_wide, chip.right - remove_width, chip.top + WidgetDimensions::scaled.vsep_normal / 2, GetAttachmentLabel(_agent_chat.tray[i]), TextColour::Black);
			DrawString(chip.right - remove_width, chip.right, chip.top + WidgetDimensions::scaled.vsep_normal / 2, GetString(STR_AGENT_CHAT_TRAY_REMOVE), TextColour::Red);
		}

		if (chips.size() < _agent_chat.tray.size()) {
			DrawString(area.left, area.right, area.bottom - GetCharacterHeight(FontSize::Normal), GetString(STR_AGENT_CHAT_MORE_ATTACHMENTS, static_cast<int32_t>(_agent_chat.tray.size() - chips.size())), TextColour::Grey, AlignmentH::End);
		}
	}

	/**
	 * Draw the multiline message input.
	 * @param r The area of the input widget.
	 */
	void DrawInput(const Rect &r) const
	{
		const Rect area = r.Shrink(WidgetDimensions::scaled.framerect);
		const int line_height = GetCharacterHeight(FontSize::Normal);
		const int visible_lines = std::max(1, area.Height() / line_height);

		/* Split the input into lines and find the line the caret is on. */
		std::vector<std::string> lines;
		size_t caret_line = 0;
		size_t line_start = 0;
		while (true) {
			const size_t end = _agent_chat.input.find('\n', line_start);
			if (end == std::string::npos) {
				lines.push_back(_agent_chat.input.substr(line_start));
				if (_agent_chat.caret >= line_start) caret_line = lines.size() - 1;
				break;
			}
			lines.push_back(_agent_chat.input.substr(line_start, end - line_start));
			if (_agent_chat.caret >= line_start && _agent_chat.caret <= end) caret_line = lines.size() - 1;
			line_start = end + 1;
		}

		const size_t first_line = caret_line + 1 > static_cast<size_t>(visible_lines) ? caret_line + 1 - visible_lines : 0;

		int y = area.top;
		for (size_t i = first_line; i < lines.size() && y + line_height - 1 <= area.bottom; i++) {
			DrawString(area.left, area.right, y, lines[i], TextColour::Black);
			y += line_height;
		}

		/* Draw the caret. */
		if (caret_line >= first_line && caret_line - first_line < static_cast<size_t>(visible_lines)) {
			const std::string &line = lines[caret_line];
			size_t line_begin = 0;
			for (size_t i = 0; i < caret_line; i++) line_begin += lines[i].size() + 1;
			const size_t offset = _agent_chat.caret > line_begin ? std::min(_agent_chat.caret - line_begin, line.size()) : 0;
			const int caret_x = area.left + GetStringBoundingBox(std::string_view(line).substr(0, offset)).width;
			const int caret_y = area.top + static_cast<int>(caret_line - first_line) * line_height;
			GfxFillRect(Rect{caret_x, caret_y, caret_x + 1, caret_y + line_height - 1}, PC_BLACK);
		}
	}

	/**
	 * Draw the status line.
	 * @param r The area of the status widget.
	 */
	void DrawStatus(const Rect &r) const
	{
		const Rect area = r.Shrink(WidgetDimensions::scaled.framerect);
		const AgentChatEntry *chat = GetActiveChat();

		std::string text;
		if (!_agent_chat.connected) {
			text = GetString(STR_AGENT_CHAT_NOT_CONNECTED);
		} else if (!_agent_chat.error.empty()) {
			text = _agent_chat.error;
		} else if (chat != nullptr) {
			text = GetChatStatusText(*chat);
		}

		if (!text.empty()) DrawString(area.left, area.right, area.top, text, _agent_chat.error.empty() ? TextColour::Grey : TextColour::Red);
	}

	void OnClick(Point pt, WidgetID widget, [[maybe_unused]] int click_count) override
	{
		switch (widget) {
			case WID_AC_NEW_CHAT:
				AgentChatCreateChat();
				break;

			case WID_AC_SEND:
				AgentChatSend();
				break;

			case WID_AC_STOP:
				AgentChatStop({});
				break;

			case WID_AC_ADD_POINT:
				this->StartPicking(true);
				break;

			case WID_AC_ADD_OBJECT:
				this->StartPicking(false);
				break;

			case WID_AC_CHAT_LIST:
				this->ClickChatList(pt);
				break;

			case WID_AC_TRAY:
				this->ClickTray(pt);
				break;

			case WID_AC_TRANSCRIPT:
				this->ClickTranscript(pt);
				break;

			case WID_AC_INPUT:
				this->ClickInput(pt);
				break;

			default:
				break;
		}
	}

	/**
	 * Handle a click in the chat list.
	 * @param pt Click position.
	 */
	void ClickChatList(Point pt)
	{
		const Rect area = this->GetInnerRect(WID_AC_CHAT_LIST);
		if (!area.Contains(pt)) return;

		const int row = (pt.y - area.top) / GetChatRowHeight() + this->chat_scroll->GetPosition();
		if (row < 0 || static_cast<size_t>(row) >= _agent_chat.chats.size()) return;
		if (row == _agent_chat.active) return;

		_agent_chat.active = row;
		_agent_chat.scroll = INT32_MAX;
		_agent_chat.candidates_open = false;
		AgentChatLoadMessages(_agent_chat.chats[row].id);
		AgentChatPollEvents(_agent_chat.chats[row].id);
		this->UpdateWidgetStates();
		this->UpdateScrollbars();
		this->SetDirty();
	}

	/**
	 * Handle a click in the attachment tray: the remove area drops the attachment,
	 * the rest of the chip centers the map on it.
	 * @param pt Click position.
	 */
	void ClickTray(Point pt)
	{
		const Rect area = this->GetInnerRect(WID_AC_TRAY);
		const std::vector<Rect> chips = LayoutTrayChips(area);
		for (size_t i = 0; i < chips.size(); i++) {
			if (!chips[i].Contains(pt)) continue;

			const int remove_width = GetCharacterHeight(FontSize::Normal);
			if (pt.x >= chips[i].right - remove_width) {
				_agent_chat.tray.erase(_agent_chat.tray.begin() + i);
			} else if (_agent_chat.tray[i].tile != INVALID_TILE) {
				ScrollMainWindowToTile(_agent_chat.tray[i].tile);
			}
			this->UpdateWidgetStates();
			this->SetDirty();
			return;
		}
	}

	/**
	 * Handle a click in the transcript: pick an object candidate, or center the map
	 * on the attachment of a message.
	 * @param pt Click position.
	 */
	void ClickTranscript(Point pt)
	{
		const Rect area = this->GetInnerRect(WID_AC_TRANSCRIPT);

		if (_agent_chat.candidates_open) {
			const std::vector<Rect> rows = LayoutCandidates(area);
			for (size_t i = 0; i < rows.size(); i++) {
				if (!rows[i].Contains(pt)) continue;
				AddCandidateAttachment(_agent_chat.candidates[i]);
				_agent_chat.candidates_open = false;
				_agent_chat.candidates = Json::array();
				this->UpdateWidgetStates();
				this->SetDirty();
				return;
			}
			_agent_chat.candidates_open = false;
			_agent_chat.candidates = Json::array();
			this->SetDirty();
			return;
		}

		const AgentChatEntry *chat = GetActiveChat();
		if (chat == nullptr) return;

		const std::vector<Rect> layout = LayoutTranscript(chat, area, _agent_chat.scroll);
		for (size_t i = 0; i < layout.size(); i++) {
			if (!layout[i].Contains(pt)) continue;
			for (const AgentChatAttachment &attachment : chat->messages[i].attachments) {
				if (attachment.tile != INVALID_TILE) {
					ScrollMainWindowToTile(attachment.tile);
					return;
				}
			}
		}
	}

	/**
	 * Handle a click in the message input: move the caret to the clicked position.
	 * @param pt Click position.
	 */
	void ClickInput(Point pt)
	{
		const Rect area = this->GetInnerRect(WID_AC_INPUT);
		if (!area.Contains(pt)) return;

		const int line_height = GetCharacterHeight(FontSize::Normal);
		const int visible_lines = std::max(1, area.Height() / line_height);

		std::vector<std::string> lines;
		size_t line_start = 0;
		while (true) {
			const size_t end = _agent_chat.input.find('\n', line_start);
			if (end == std::string::npos) {
				lines.push_back(_agent_chat.input.substr(line_start));
				break;
			}
			lines.push_back(_agent_chat.input.substr(line_start, end - line_start));
			line_start = end + 1;
		}

		size_t caret_line = 0;
		line_start = 0;
		for (size_t i = 0; i < lines.size(); i++) {
			if (_agent_chat.caret >= line_start && _agent_chat.caret <= line_start + lines[i].size()) {
				caret_line = i;
				break;
			}
			line_start += lines[i].size() + 1;
		}
		const size_t first_line = caret_line + 1 > static_cast<size_t>(visible_lines) ? caret_line + 1 - visible_lines : 0;

		const int clicked_line = std::max(0, (pt.y - area.top) / line_height);
		const size_t line = std::min(lines.size() - 1, first_line + clicked_line);

		size_t begin = 0;
		for (size_t i = 0; i < line; i++) begin += lines[i].size() + 1;

		/* Find the character boundary closest to the click. */
		size_t best = 0;
		int best_distance = INT32_MAX;
		const std::string &text = lines[line];
		for (size_t offset = 0; offset <= text.size(); offset++) {
			if (offset > 0 && IsUtf8Part(text[offset])) continue;
			const int width = GetStringBoundingBox(std::string_view(text).substr(0, offset)).width;
			const int distance = std::abs(area.left + width - pt.x);
			if (distance < best_distance) {
				best_distance = distance;
				best = offset;
			}
		}

		_agent_chat.caret = begin + best;
		this->SetWidgetDirty(WID_AC_INPUT);
	}

	/**
	 * Start picking a tile on the map.
	 * @param point Whether a point (rather than an object) is picked.
	 */
	void StartPicking(bool point)
	{
		SetObjectToPlace(SPR_CURSOR_QUERY, PAL_NONE, HT_RECT, this->window_class, this->window_number);
		_agent_chat.picking_point = point;
		_agent_chat.picking_object = !point;
		_agent_chat.candidates_open = false;
		_agent_chat.candidates = Json::array();
		this->UpdateWidgetStates();
		this->SetDirty();
	}

	void OnPlaceObject([[maybe_unused]] Point pt, TileIndex tile) override
	{
		if (_agent_chat.picking_point) {
			AddPointAttachment(tile);
		} else if (_agent_chat.picking_object) {
			AddObjectAttachment(tile);
		}
		ResetObjectToPlace();
	}

	void OnPlaceObjectAbort() override
	{
		_agent_chat.picking_point = false;
		_agent_chat.picking_object = false;
		this->UpdateWidgetStates();
		this->SetDirty();
	}

	/**
	 * Add a point attachment for a tile.
	 * @param tile The tile.
	 */
	void AddPointAttachment(TileIndex tile)
	{
		if (!IsValidTile(tile)) return;

		AgentChatAttachment attachment;
		attachment.type = "point";
		attachment.tile = tile;
		attachment.name = GetString(STR_AGENT_CHAT_ATTACHMENT_POINT, TileX(tile), TileY(tile));
		_agent_chat.tray.push_back(std::move(attachment));
		this->UpdateWidgetStates();
		this->SetDirty();
	}

	/**
	 * Add an object attachment for a tile, asking the player to choose when the tile
	 * holds more than one object.
	 * @param tile The tile.
	 */
	void AddObjectAttachment(TileIndex tile)
	{
		if (!IsValidTile(tile)) return;

		const Json picked = InGameAgent::PickObjects(tile);
		const Json &candidates = picked.is_array() ? picked : JsonArray(picked, "objects");

		if (candidates.empty()) {
			/* Nothing to disambiguate: attach the tile itself. */
			AgentChatAttachment attachment;
			attachment.type = "tile";
			attachment.tile = tile;
			attachment.what = "landscape";
			attachment.name = GetString(STR_AGENT_CHAT_ATTACHMENT_TILE, TileX(tile), TileY(tile));
			_agent_chat.tray.push_back(std::move(attachment));
		} else if (candidates.size() == 1) {
			AddCandidateAttachment(candidates.front());
		} else {
			_agent_chat.candidates = candidates;
			_agent_chat.candidates_open = true;
		}

		this->UpdateWidgetStates();
		this->SetDirty();
	}

	/**
	 * Add an object candidate as an attachment.
	 * @param candidate The candidate as returned by the transport.
	 */
	void AddCandidateAttachment(const Json &candidate)
	{
		AgentChatAttachment attachment = ParseAttachment(candidate);
		if (attachment.type.empty()) attachment.type = "object";
		if (attachment.name.empty()) attachment.name = GetString(STR_AGENT_CHAT_ATTACHMENT_OBJECT, GetAttachmentTypeName(attachment.type), attachment.id);
		_agent_chat.tray.push_back(std::move(attachment));
		this->UpdateWidgetStates();
		this->SetDirty();
	}

	EventState OnKeyPress(char32_t key, uint16_t keycode) override
	{
		/* Keys are offered to every window from front to back, so only take text input
		 * while this window is the focused one. */
		const bool focused = _focused_window == this;
		const uint16_t base = keycode & ~WKC_SPECIAL_KEYS;

		if (base == WKC_ESC) {
			if (_agent_chat.candidates_open) {
				_agent_chat.candidates_open = false;
				_agent_chat.candidates = Json::array();
				this->SetDirty();
				return EventState::Handled;
			}
			if (_agent_chat.picking_point || _agent_chat.picking_object) {
				ResetObjectToPlace();
				return EventState::Handled;
			}
			return EventState::NotHandled;
		}

		if (!focused) return EventState::NotHandled;

		if (keycode == ('V' | WKC_CTRL)) {
			const std::optional<std::string> contents = GetClipboardContents();
			if (contents.has_value()) this->InsertInput(*contents);
			return EventState::Handled;
		}

		if (base == WKC_RETURN) {
			if ((keycode & WKC_SHIFT) != 0) {
				this->InsertInput("\n");
			} else {
				AgentChatSend();
			}
			return EventState::Handled;
		}

		if (base == WKC_BACKSPACE) {
			this->DeleteInputBeforeCaret();
			return EventState::Handled;
		}

		if (base == WKC_DELETE) {
			this->DeleteInputAfterCaret();
			return EventState::Handled;
		}

		if (base == WKC_LEFT) {
			this->MoveCaret(-1);
			return EventState::Handled;
		}

		if (base == WKC_RIGHT) {
			this->MoveCaret(1);
			return EventState::Handled;
		}

		if (base == WKC_HOME) {
			_agent_chat.caret = 0;
			this->SetWidgetDirty(WID_AC_INPUT);
			return EventState::Handled;
		}

		if (base == WKC_END) {
			_agent_chat.caret = _agent_chat.input.size();
			this->SetWidgetDirty(WID_AC_INPUT);
			return EventState::Handled;
		}

		if (key != 0 && IsPrintable(key)) {
			auto [buf, len] = EncodeUtf8(key);
			this->InsertInput(std::string_view(buf, len));
			return EventState::Handled;
		}

		return EventState::NotHandled;
	}

	/**
	 * Insert text at the caret.
	 * @param text The text to insert.
	 */
	void InsertInput(std::string_view text)
	{
		if (_agent_chat.input.size() + text.size() > AGENT_CHAT_MAX_INPUT) return;
		_agent_chat.input.insert(_agent_chat.caret, text);
		_agent_chat.caret += text.size();
		this->UpdateWidgetStates();
		this->SetWidgetDirty(WID_AC_INPUT);
	}

	/** Delete the character before the caret. */
	void DeleteInputBeforeCaret()
	{
		if (_agent_chat.caret == 0) return;
		size_t begin = _agent_chat.caret - 1;
		while (begin > 0 && IsUtf8Part(_agent_chat.input[begin])) begin--;
		_agent_chat.input.erase(begin, _agent_chat.caret - begin);
		_agent_chat.caret = begin;
		this->UpdateWidgetStates();
		this->SetWidgetDirty(WID_AC_INPUT);
	}

	/** Delete the character after the caret. */
	void DeleteInputAfterCaret()
	{
		if (_agent_chat.caret >= _agent_chat.input.size()) return;
		size_t end = _agent_chat.caret + 1;
		while (end < _agent_chat.input.size() && IsUtf8Part(_agent_chat.input[end])) end++;
		_agent_chat.input.erase(_agent_chat.caret, end - _agent_chat.caret);
		this->UpdateWidgetStates();
		this->SetWidgetDirty(WID_AC_INPUT);
	}

	/**
	 * Move the caret by one character.
	 * @param direction -1 for left, 1 for right.
	 */
	void MoveCaret(int direction)
	{
		if (direction < 0) {
			if (_agent_chat.caret == 0) return;
			size_t pos = _agent_chat.caret - 1;
			while (pos > 0 && IsUtf8Part(_agent_chat.input[pos])) pos--;
			_agent_chat.caret = pos;
		} else {
			if (_agent_chat.caret >= _agent_chat.input.size()) return;
			size_t pos = _agent_chat.caret + 1;
			while (pos < _agent_chat.input.size() && IsUtf8Part(_agent_chat.input[pos])) pos++;
			_agent_chat.caret = pos;
		}
		this->SetWidgetDirty(WID_AC_INPUT);
	}

	void OnMouseWheel(int wheel, WidgetID widget) override
	{
		if (widget != WID_AC_TRANSCRIPT && widget != WID_AC_TRANSCRIPT_SCROLL) return;
		this->transcript_scroll->UpdatePosition(wheel);
		_agent_chat.scroll = this->transcript_scroll->GetPosition();
		this->SetWidgetDirty(WID_AC_TRANSCRIPT);
	}

	void OnScrollbarScroll(WidgetID widget) override
	{
		if (widget == WID_AC_TRANSCRIPT_SCROLL) {
			_agent_chat.scroll = this->transcript_scroll->GetPosition();
			this->SetWidgetDirty(WID_AC_TRANSCRIPT);
		} else if (widget == WID_AC_CHAT_SCROLL) {
			this->SetWidgetDirty(WID_AC_CHAT_LIST);
		}
	}

	void OnResize() override
	{
		this->UpdateScrollbars();
	}

	void OnInvalidateData([[maybe_unused]] int data = 0, [[maybe_unused]] bool gui_scope = true) override
	{
		this->UpdateWidgetStates();
		this->UpdateScrollbars();
		this->SetDirty();
	}
};

/** Widget parts of the agent chat window. */
static constexpr std::initializer_list<NWidgetPart> _nested_agent_chat_widgets = {
	NWidget(NWID_HORIZONTAL),
		NWidget(WWT_CLOSEBOX, Colours::Grey),
		NWidget(WWT_CAPTION, Colours::Grey, WID_AC_CAPTION), SetStringTip(STR_AGENT_CHAT_CAPTION),
		NWidget(WWT_SHADEBOX, Colours::Grey),
		NWidget(WWT_STICKYBOX, Colours::Grey),
		NWidget(WWT_RESIZEBOX, Colours::Grey),
	EndContainer(),
	NWidget(NWID_HORIZONTAL),
		NWidget(NWID_VERTICAL),
			NWidget(WWT_PANEL, Colours::Grey, WID_AC_CHAT_LIST), SetMinimalSize(120, 120), SetResize(0, 1), SetScrollbar(WID_AC_CHAT_SCROLL), SetToolTip(STR_AGENT_CHAT_CHAT_LIST_TOOLTIP),
			EndContainer(),
			NWidget(NWID_VERTICAL),
				NWidget(NWID_VSCROLLBAR, Colours::Grey, WID_AC_CHAT_SCROLL),
			EndContainer(),
			NWidget(WWT_PUSHTXTBTN, Colours::Grey, WID_AC_NEW_CHAT), SetFill(1, 0), SetStringTip(STR_AGENT_CHAT_NEW_CHAT, STR_AGENT_CHAT_NEW_CHAT_TOOLTIP),
		EndContainer(),
		NWidget(NWID_VERTICAL),
			NWidget(NWID_HORIZONTAL),
				NWidget(WWT_PANEL, Colours::Grey, WID_AC_TRANSCRIPT), SetFill(1, 1), SetResize(1, 1), SetScrollbar(WID_AC_TRANSCRIPT_SCROLL), SetToolTip(STR_AGENT_CHAT_TRANSCRIPT_TOOLTIP),
				EndContainer(),
				NWidget(NWID_VERTICAL),
					NWidget(NWID_VSCROLLBAR, Colours::Grey, WID_AC_TRANSCRIPT_SCROLL),
				EndContainer(),
			EndContainer(),
			NWidget(WWT_PANEL, Colours::Grey, WID_AC_TRAY), SetMinimalSize(0, 24), SetFill(1, 0), SetResize(1, 0),
			EndContainer(),
			NWidget(NWID_HORIZONTAL),
				NWidget(WWT_PUSHTXTBTN, Colours::Grey, WID_AC_ADD_POINT), SetFill(1, 0), SetStringTip(STR_AGENT_CHAT_ADD_POINT, STR_AGENT_CHAT_ADD_POINT_TOOLTIP),
				NWidget(WWT_PUSHTXTBTN, Colours::Grey, WID_AC_ADD_OBJECT), SetFill(1, 0), SetStringTip(STR_AGENT_CHAT_ADD_OBJECT, STR_AGENT_CHAT_ADD_OBJECT_TOOLTIP),
			EndContainer(),
			NWidget(WWT_PANEL, Colours::Grey, WID_AC_INPUT), SetMinimalSize(0, 48), SetFill(1, 0), SetResize(1, 0), SetToolTip(STR_AGENT_CHAT_INPUT_TOOLTIP),
			EndContainer(),
			NWidget(NWID_HORIZONTAL),
				NWidget(WWT_PANEL, Colours::Grey, WID_AC_STATUS), SetMinimalSize(0, 14), SetFill(1, 0), SetResize(1, 0),
				EndContainer(),
				NWidget(WWT_PUSHTXTBTN, Colours::Grey, WID_AC_SEND), SetStringTip(STR_AGENT_CHAT_SEND, STR_AGENT_CHAT_SEND_TOOLTIP),
				NWidget(WWT_PUSHTXTBTN, Colours::Grey, WID_AC_STOP), SetStringTip(STR_AGENT_CHAT_STOP, STR_AGENT_CHAT_STOP_TOOLTIP),
			EndContainer(),
		EndContainer(),
	EndContainer(),
};

/** Window description of the agent chat window. */
static WindowDesc _agent_chat_window_desc(
	WindowPosition::Automatic, "agent_chat", 620, 400,
	WindowClass::AgentChat, WindowClass::None,
	{},
	_nested_agent_chat_widgets
);

/** Status indicator of a running agent task, shown while the chat window is closed. */
struct AgentChatStatusWindow : Window {
	AgentChatStatusWindow(WindowDesc &desc) : Window(desc)
	{
		this->CreateNestedTree();
		this->FinishInitNested(0);
	}

	Point OnInitialPosition([[maybe_unused]] int16_t sm_width, [[maybe_unused]] int16_t sm_height, [[maybe_unused]] int window_number) override
	{
		return Point(0, GetMainViewTop());
	}

	void OnPaint() override
	{
		this->DrawWidgets();
	}

	std::string GetWidgetString(WidgetID widget, StringID stringid) const override
	{
		if (widget != WID_ACS_TEXT) return this->Window::GetWidgetString(widget, stringid);

		const AgentChatEntry *busy = FindBusyChat();
		if (busy == nullptr) return GetString(STR_AGENT_CHAT_STATUS_IDLE);

		const std::string title = busy->title.empty() ? busy->id : busy->title;
		return GetString(STR_AGENT_CHAT_STATUS_WINDOW, GetChatStatusText(*busy), title);
	}

	void OnClick([[maybe_unused]] Point pt, WidgetID widget, [[maybe_unused]] int click_count) override
	{
		switch (widget) {
			case WID_ACS_TEXT:
				ShowAgentChatWindow();
				break;

			case WID_ACS_STOP:
				AgentChatStop({});
				break;

			default:
				break;
		}
	}
};

/** Widget parts of the agent chat status window. */
static constexpr std::initializer_list<NWidgetPart> _nested_agent_chat_status_widgets = {
	NWidget(NWID_HORIZONTAL),
		NWidget(WWT_PUSHTXTBTN, Colours::Grey, WID_ACS_TEXT), SetFill(1, 0), SetResize(1, 0), SetStringTip(STR_AGENT_CHAT_STATUS_IDLE, STR_AGENT_CHAT_STATUS_TOOLTIP),
		NWidget(WWT_PUSHTXTBTN, Colours::Grey, WID_ACS_STOP), SetStringTip(STR_AGENT_CHAT_STOP, STR_AGENT_CHAT_STOP_TOOLTIP),
	EndContainer(),
};

/** Window description of the agent chat status window. */
static WindowDesc _agent_chat_status_window_desc(
	WindowPosition::Manual, {}, 320, 24,
	WindowClass::AgentChatStatus, WindowClass::None,
	{WindowDefaultFlag::NoFocus},
	_nested_agent_chat_status_widgets
);

/** Show or hide the status indicator, depending on whether a task is running. */
static void AgentChatUpdateStatusWindow()
{
	const bool busy = FindBusyChat() != nullptr;
	const bool chat_open = FindWindowById(WindowClass::AgentChat, 0) != nullptr;
	const bool status_open = FindWindowById(WindowClass::AgentChatStatus, 0) != nullptr;

	if (busy && !chat_open) {
		if (!status_open) new AgentChatStatusWindow(_agent_chat_status_window_desc);
	} else if (status_open) {
		CloseWindowById(WindowClass::AgentChatStatus, 0);
	}
}

/** Realtime timer that polls the agent chat, so it also runs while the window is closed. */
static const IntervalTimer<TimerWindow> _agent_chat_interval(std::chrono::milliseconds(250), [](auto) {
	AgentChatGuiTick();
});

void ShowAgentChatWindow()
{
	Window *w = BringWindowToFrontById(WindowClass::AgentChat, 0);
	if (w != nullptr) {
		w->SetDirty();
		return;
	}
	new AgentChatWindow(_agent_chat_window_desc);
}

void AgentChatGuiTick()
{
	AgentChatSyncContext();

	if (!_agent_chat.connected) {
		AgentChatUpdateStatusWindow();
		return;
	}

	const auto now = std::chrono::steady_clock::now();
	if (now < _agent_chat.next_poll) {
		AgentChatUpdateStatusWindow();
		return;
	}
	_agent_chat.next_poll = now + std::chrono::milliseconds(AGENT_CHAT_POLL_INTERVAL_MS);

	if (!_agent_chat.chats_loaded) {
		AgentChatLoadChats();
		AgentChatUpdateStatusWindow();
		return;
	}

	const AgentChatEntry *active = GetActiveChat();
	const std::string active_id = active != nullptr ? active->id : std::string();
	if (active != nullptr) {
		AgentChatLoadMessages(active_id);
		AgentChatPollEvents(active_id);
	}

	/* Keep the status of tasks in other chats up to date as well. */
	std::vector<std::string> busy_ids;
	for (const AgentChatEntry &chat : _agent_chat.chats) {
		if (chat.task_active && chat.id != active_id) busy_ids.push_back(chat.id);
	}
	for (const std::string &id : busy_ids) AgentChatPollEvents(id);

	AgentChatUpdateStatusWindow();
}

nlohmann::json DescribeAgentChatUi()
{
	Json out = Json::object();
	out["hotkey"] = "Ctrl+Shift+A";
	out["connected"] = _agent_chat.connected;
	out["world_id"] = _agent_chat.world_id;
	out["epoch"] = _agent_chat.epoch;
	out["connection_id"] = _agent_chat.connection_id;
	out["input"] = _agent_chat.input;
	out["error"] = _agent_chat.error;
	out["candidates_open"] = _agent_chat.candidates_open;
	out["picking_point"] = _agent_chat.picking_point;
	out["picking_object"] = _agent_chat.picking_object;

	const AgentChatEntry *active = GetActiveChat();
	out["active_chat"] = active != nullptr ? active->id : std::string();

	Json chats = Json::array();
	for (const AgentChatEntry &chat : _agent_chat.chats) {
		Json entry = Json::object();
		entry["id"] = chat.id;
		entry["title"] = chat.title;
		entry["selected"] = active != nullptr && active->id == chat.id;
		entry["task_active"] = chat.task_active;
		entry["status"] = GetChatStatusText(chat);
		chats.push_back(std::move(entry));
	}
	out["chats"] = std::move(chats);

	Json tray = Json::array();
	for (const AgentChatAttachment &attachment : _agent_chat.tray) {
		Json entry = Json::object();
		entry["type"] = attachment.type;
		entry["id"] = attachment.id;
		entry["name"] = GetAttachmentLabel(attachment);
		entry["tile"] = attachment.tile == INVALID_TILE ? Json() : Json::array({TileX(attachment.tile), TileY(attachment.tile)});
		tray.push_back(std::move(entry));
	}
	out["tray"] = std::move(tray);

	Json candidates = Json::array();
	for (const Json &candidate : _agent_chat.candidates) {
		Json entry = Json::object();
		entry["type"] = JsonString(candidate, "type");
		entry["id"] = JsonInt(candidate, "id", -1);
		entry["name"] = JsonString(candidate, "name");
		candidates.push_back(std::move(entry));
	}
	out["candidates"] = std::move(candidates);

	Json messages = Json::array();
	if (active != nullptr) {
		for (const AgentChatMessage &message : active->messages) {
			Json entry = Json::object();
			entry["seq"] = message.seq;
			entry["role"] = message.role;
			entry["text"] = message.text;
			entry["status"] = message.status;
			messages.push_back(std::move(entry));
		}
	}
	out["messages"] = std::move(messages);

	/* Report the actual rectangles of the window and of the targets inside it. */
	AgentChatWindow *w = dynamic_cast<AgentChatWindow *>(FindWindowById(WindowClass::AgentChat, 0));
	out["window_open"] = w != nullptr;
	if (w != nullptr) {
		Json widgets = Json::object();
		const std::pair<const char *, WidgetID> widget_names[] = {
			{"chat_list", WID_AC_CHAT_LIST},
			{"chat_scroll", WID_AC_CHAT_SCROLL},
			{"new_chat", WID_AC_NEW_CHAT},
			{"transcript", WID_AC_TRANSCRIPT},
			{"transcript_scroll", WID_AC_TRANSCRIPT_SCROLL},
			{"tray", WID_AC_TRAY},
			{"add_point", WID_AC_ADD_POINT},
			{"add_object", WID_AC_ADD_OBJECT},
			{"input", WID_AC_INPUT},
			{"status", WID_AC_STATUS},
			{"send", WID_AC_SEND},
			{"stop", WID_AC_STOP},
		};
		for (const auto &[name, widget] : widget_names) {
			widgets[name] = RectToJson(w->GetWidget<NWidgetBase>(widget)->GetCurrentRect());
		}
		out["widgets"] = std::move(widgets);

		Json chat_rows = Json::array();
		const Rect list = w->GetInnerRect(WID_AC_CHAT_LIST);
		const int row_height = GetChatRowHeight();
		const int first = w->chat_scroll->GetPosition();
		const int last = std::min<int>(_agent_chat.chats.size(), first + w->chat_scroll->GetCapacity());
		for (int i = first; i < last; i++) {
			const int top = list.top + (i - first) * row_height;
			Json entry = Json::object();
			entry["id"] = _agent_chat.chats[i].id;
			entry["rect"] = RectToJson(Rect{list.left, top, list.right, top + row_height - 1});
			chat_rows.push_back(std::move(entry));
		}
		out["chat_rows"] = std::move(chat_rows);

		Json message_rows = Json::array();
		if (active != nullptr) {
			const Rect area = w->GetInnerRect(WID_AC_TRANSCRIPT);
			const std::vector<Rect> layout = LayoutTranscript(active, area, _agent_chat.scroll);
			for (size_t i = 0; i < layout.size(); i++) {
				Json entry = Json::object();
				entry["seq"] = active->messages[i].seq;
				entry["rect"] = RectToJson(layout[i]);
				message_rows.push_back(std::move(entry));
			}
		}
		out["message_rows"] = std::move(message_rows);

		Json tray_rows = Json::array();
		const Rect tray_area = w->GetInnerRect(WID_AC_TRAY);
		const std::vector<Rect> chips = LayoutTrayChips(tray_area);
		for (size_t i = 0; i < chips.size(); i++) {
			const int remove_width = GetCharacterHeight(FontSize::Normal);
			Json entry = Json::object();
			entry["name"] = GetAttachmentLabel(_agent_chat.tray[i]);
			entry["rect"] = RectToJson(chips[i]);
			entry["remove_rect"] = RectToJson(Rect{chips[i].right - remove_width, chips[i].top, chips[i].right, chips[i].bottom});
			tray_rows.push_back(std::move(entry));
		}
		out["tray_rows"] = std::move(tray_rows);

		Json candidate_rows = Json::array();
		const std::vector<Rect> rows = LayoutCandidates(w->GetInnerRect(WID_AC_TRANSCRIPT));
		for (size_t i = 0; i < rows.size(); i++) {
			Json entry = Json::object();
			entry["name"] = JsonString(_agent_chat.candidates[i], "name");
			entry["rect"] = RectToJson(rows[i]);
			candidate_rows.push_back(std::move(entry));
		}
		out["candidate_rows"] = std::move(candidate_rows);
	}

	Window *status = FindWindowById(WindowClass::AgentChatStatus, 0);
	if (status != nullptr) {
		Json entry = Json::object();
		entry["text"] = RectToJson(status->GetWidget<NWidgetBase>(WID_ACS_TEXT)->GetCurrentRect());
		entry["stop"] = RectToJson(status->GetWidget<NWidgetBase>(WID_ACS_STOP)->GetCurrentRect());
		out["status_window"] = std::move(entry);
	} else {
		out["status_window"] = Json();
	}

	return out;
}
