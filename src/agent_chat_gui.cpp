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
#include "agent_chat_cache.h"
#include "agent_chat_image.h"
#include "agent_chat_selection.h"

#include "company_base.h"
#include "company_func.h"
#include "console_func.h"
#include "core/backup_type.hpp"
#include "core/enum_type.hpp"
#include "core/geometry_func.hpp"
#include "core/string_consumer.hpp"
#include "core/utf8.hpp"
#include "gfx_func.h"
#include "gfx_layout.h"
#include "dropdown_func.h"
#include "map_func.h"
#include "querystring_gui.h"
#include "settings_internal.h"
#include "settings_table.h"
#include "settings_type.h"
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

/**
 * Put text on the system clipboard.
 *
 * The OS backends only had a clipboard reader, which the message box uses to paste; a
 * selectable transcript needs the writer as well. The text is handed over as UTF-8 and the
 * backend converts it, so Cyrillic and newlines reach the clipboard unchanged.
 * @param text The text to copy.
 * @return \c true when the clipboard took the text.
 */
bool SetClipboardContents(const std::string &text);

using Json = nlohmann::json;

/** Maximum number of bytes accepted in the message input. */
static const size_t AGENT_CHAT_MAX_INPUT = 8000;

/** Interval between two polls of the backend, in milliseconds. */
static const uint AGENT_CHAT_POLL_INTERVAL_MS = 1000;

/** Interval used while a task is stopping or settling, until the backend reports it terminal. */
static const uint AGENT_CHAT_FINISHING_POLL_INTERVAL_MS = 250;

/** Number of requests in flight above which a periodic read is skipped instead of queued. */
static const int AGENT_CHAT_MAX_PENDING_REQUESTS = 4;

/** Number of screenshot previews fetched at the same time. */
static const int AGENT_CHAT_MAX_PREVIEW_REQUESTS = 4;

/** Number of text lines shown in the message input. */
static const int AGENT_CHAT_INPUT_LINES = 3;

/** Number of text lines shown in the attachment tray. */
static const int AGENT_CHAT_TRAY_LINES = 2;

/** One typed game reference attached to a chat message. */
struct AgentChatAttachment {
	std::string type;               ///< Reference type, e.g. "point", "town" or "vehicle".
	int32_t id = -1;                ///< Object id of an object reference, -1 for points and tiles.
	int64_t generation = -1;        ///< Lifetime generation of the object, -1 when the backend did not report one.
	TileIndex tile = INVALID_TILE;  ///< Tile of a point or tile reference, or the location of an object.
	std::string name;               ///< Human readable name, shown in the attachment tray.
	std::string setting_name;       ///< Exact engine key of a setting reference, the identity of the reference.
	std::string scope;              ///< Scope of a setting reference: "game", "client" or "company".
	int32_t company_id = -1;        ///< Company that owns a company scoped setting, -1 when the reference has no owner.
	int64_t company_generation = -1; ///< Lifetime generation of that company, -1 when it is unknown.
	std::string what;               ///< Extra classification of a tile reference.
	std::string world_id;           ///< World the reference was created in.
	std::string epoch;              ///< Load epoch the reference was created in.
	std::string artifact_id;        ///< Artifact id of a screenshot attachment, empty for references.
	std::string sha256;             ///< SHA-256 of the stored source image of a screenshot.
	int width = 0;                  ///< Width of the source image of a screenshot, in pixels.
	int height = 0;                 ///< Height of the source image of a screenshot, in pixels.
	int64_t tick = 0;               ///< Game tick a screenshot was captured at.
	Json extra = Json::object();    ///< Reference fields this UI does not know, kept verbatim.
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
	bool task_finishing = false;    ///< Whether the turn of the task ended while its writes may still be settling.
	bool stop_pending = false;      ///< Whether a stop was requested and its barrier is not acknowledged yet.
	std::string task_status;        ///< Short status of the running task, shown to the player.
};

/** All state of the in-game agent chat UI. */
struct AgentChatUiState {
	std::vector<AgentChatEntry> chats;      ///< Known chats of the current world.
	int active = -1;                        ///< Index of the selected chat, or -1.
	std::vector<AgentChatAttachment> tray;  ///< Attachments of the message being composed.
	std::string input;                      ///< Text of the message being composed, UTF-8 encoded.
	size_t caret = 0;                       ///< Caret position in the input, in bytes.
	size_t composed_length = 0;             ///< Bytes of IME text still being composed, replaced by the next update.
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
	AgentChatSendAck send_ack;              ///< Submission of the composed message and its acknowledgement.
	AgentChatSelection selection;           ///< Selected transcript text of the active chat.
	AgentChatPreviewCache previews;         ///< Screenshot previews of the transcript, bounded and per world.
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
 * Make text that came from the backend safe to draw.
 *
 * The string formatter interprets the private-use control code range, so a
 * message that happens to contain those code points could change the layout of
 * the window or read parameters that are not there. Invalid UTF-8 is replaced
 * as well; newlines are kept so multiline messages stay readable.
 * @param text The text to sanitise.
 * @return The sanitised text.
 */
static std::string SanitiseBackendText(std::string_view text)
{
	return StrMakeValid(text, {StringValidationSetting::ReplaceWithQuestionMark, StringValidationSetting::AllowNewline, StringValidationSetting::ReplaceTabCrNlWithSpace});
}

/**
 * Get the lifetime generation of a pooled object.
 *
 * Pool items track the identity of their lifetime so a reference to a recreated object can be
 * told apart from the original one. Not every pool in every build has that counter, so it is only
 * read when the type has it: a reference without a generation is reported as such, never invented.
 * @param object The pooled object.
 * @return The generation, or -1 when the pool of this build does not track one.
 */
template <typename T>
static int64_t GetPoolGeneration(const T &object)
{
	if constexpr (requires { object.generation; }) {
		return static_cast<int64_t>(object.generation);
	} else {
		return -1;
	}
}

/**
 * Get the human readable name of the kind of thing a reference points at.
 *
 * The bridge names the kind of a tile reference in `what` (and, for objects with a pool of their
 * own, the kind of that object), using the protocol names. A kind this UI does not know is
 * reported as an empty string, so the caller shows the plain reference instead of a wrong label.
 * @param what The kind, as reported by the bridge.
 * @return The name of the kind, or an empty string when it is unknown.
 */
static std::string GetAttachmentKindName(const std::string &what)
{
	if (what == "clear" || what == "terrain" || what == "landscape") return GetString(STR_AGENT_CHAT_KIND_TERRAIN);
	if (what == "rail") return GetString(STR_AGENT_CHAT_KIND_RAIL);
	if (what == "road") return GetString(STR_AGENT_CHAT_KIND_ROAD);
	if (what == "tram") return GetString(STR_AGENT_CHAT_KIND_TRAM);
	if (what == "signal") return GetString(STR_AGENT_CHAT_KIND_SIGNAL);
	if (what == "tree" || what == "trees") return GetString(STR_AGENT_CHAT_KIND_TREES);
	if (what == "water") return GetString(STR_AGENT_CHAT_KIND_WATER);
	if (what == "bridge" || what == "tunnelbridge") return GetString(STR_AGENT_CHAT_KIND_BRIDGE);
	if (what == "tunnel") return GetString(STR_AGENT_CHAT_KIND_TUNNEL);
	if (what == "station") return GetString(STR_AGENT_CHAT_TYPE_STATION);
	if (what == "industry") return GetString(STR_AGENT_CHAT_TYPE_INDUSTRY);
	if (what == "house") return GetString(STR_AGENT_CHAT_TYPE_HOUSE);
	if (what == "object" || what == "mapobject" || what == "map_object") return GetString(STR_AGENT_CHAT_TYPE_OBJECT);
	return {};
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
	if (type == "depot") return GetString(STR_AGENT_CHAT_TYPE_DEPOT);
	if (type == "waypoint") return GetString(STR_AGENT_CHAT_TYPE_WAYPOINT);
	if (type == "buoy") return GetString(STR_AGENT_CHAT_TYPE_BUOY);
	if (type == "house") return GetString(STR_AGENT_CHAT_TYPE_HOUSE);
	if (type == "group") return GetString(STR_AGENT_CHAT_TYPE_GROUP);
	if (type == "engine") return GetString(STR_AGENT_CHAT_TYPE_ENGINE);
	if (type == "cargo") return GetString(STR_AGENT_CHAT_TYPE_CARGO);
	if (type == "setting") return GetString(STR_AGENT_CHAT_TYPE_SETTING);
	if (type == "object" || type == "mapobject") return GetString(STR_AGENT_CHAT_TYPE_OBJECT);
	if (type == "image") return GetString(STR_AGENT_CHAT_TYPE_IMAGE);

	/* A bridge may name the kind of tile in the type itself instead of in "what". */
	const std::string kind = GetAttachmentKindName(type);
	if (!kind.empty()) return kind;
	return GetString(STR_AGENT_CHAT_TYPE_UNKNOWN);
}

/**
 * Get the readable name of the subtype of a typed tile object.
 *
 * The bridge describes the subtype of a typed tile row (ground, water kind, signal variant,
 * transport type of a bridge or tunnel, ...). Only words a player can read are named: track bits
 * and raw NewGRF numbers would say nothing, so they leave the label short.
 * @param type The type of the reference.
 * @param subtype The protocol subtype.
 * @return The name, or an empty string when the subtype adds nothing to the label.
 */
static std::string GetAttachmentSubtypeName(const std::string &type, const std::string &subtype)
{
	if (subtype.empty() || subtype == type) return {};
	if (subtype.starts_with("house_") || subtype.starts_with("type_")) return {};

	if (subtype == "grass") return GetString(STR_AGENT_CHAT_SUBTYPE_GRASS);
	if (subtype == "rough") return GetString(STR_AGENT_CHAT_SUBTYPE_ROUGH);
	if (subtype == "rocks") return GetString(STR_AGENT_CHAT_SUBTYPE_ROCKS);
	if (subtype == "fields") return GetString(STR_AGENT_CHAT_SUBTYPE_FIELDS);
	if (subtype == "desert") return GetString(STR_AGENT_CHAT_SUBTYPE_DESERT);
	if (subtype == "snow_or_desert") return GetString(STR_AGENT_CHAT_SUBTYPE_SNOW_OR_DESERT);
	if (subtype == "shore") return GetString(STR_AGENT_CHAT_SUBTYPE_SHORE);
	if (subtype == "rough_snow") return GetString(STR_AGENT_CHAT_SUBTYPE_ROUGH_SNOW);
	if (subtype == "coast") return GetString(STR_AGENT_CHAT_SUBTYPE_COAST);
	if (subtype == "canal") return GetString(STR_AGENT_CHAT_SUBTYPE_CANAL);
	if (subtype == "river") return GetString(STR_AGENT_CHAT_SUBTYPE_RIVER);
	if (subtype == "lock") return GetString(STR_AGENT_CHAT_SUBTYPE_LOCK);
	if (subtype == "water") return GetString(STR_AGENT_CHAT_SUBTYPE_WATER);
	if (subtype == "clear_rocks") return GetString(STR_AGENT_CHAT_SUBTYPE_CLEAR_ROCKS);
	if (subtype == "coast_rocks") return GetString(STR_AGENT_CHAT_SUBTYPE_COAST_ROCKS);
	if (subtype == "depot") return GetString(STR_AGENT_CHAT_SUBTYPE_DEPOT);
	if (subtype == "signals") return GetString(STR_AGENT_CHAT_SUBTYPE_SIGNALS);
	if (subtype == "semaphore") return GetString(STR_AGENT_CHAT_SUBTYPE_SEMAPHORE);
	if (subtype == "electric") return GetString(STR_AGENT_CHAT_SUBTYPE_ELECTRIC);
	if (subtype == "air") return GetString(STR_AGENT_CHAT_SUBTYPE_AIR);

	/* Track bits and protocol markers are words the player does not read as a word. */
	if (subtype == "x" || subtype == "y" || subtype == "upper" || subtype == "lower" || subtype == "left" ||
			subtype == "right" || subtype == "cross" || subtype == "horizontal" || subtype == "vertical" ||
			subtype == "all" || subtype == "mixed" || subtype == "clear" || subtype == "unknown" ||
			subtype == "invalid") {
		return {};
	}
	/* A word this build does not know is still more useful than hiding it. */
	return subtype;
}

/**
 * Read the subtype of a reference.
 *
 * The subtype of a typed tile object is not part of the identity the UI keeps in its own fields, so
 * it travels in #AgentChatAttachment::extra; reading it there keeps a reference that came back from
 * the backend labelled the same way as a freshly picked one.
 * @param attachment The attachment.
 * @return The subtype, or an empty string.
 */
static std::string GetAttachmentSubtype(const AgentChatAttachment &attachment)
{
	if (!attachment.extra.is_object()) return {};
	return JsonString(attachment.extra, "subtype");
}

/**
 * Get the label of a reference that is identified by its tile.
 * @param type The type of the reference.
 * @param what The classification the reference carries.
 * @param subtype The subtype of the object, if any.
 * @param tile The tile.
 * @return The label.
 */
static std::string GetTileAttachmentLabel(const std::string &type, const std::string &what, const std::string &subtype, TileIndex tile)
{
	const std::string kind = GetAttachmentKindName(what.empty() ? type : what);
	if (kind.empty()) return GetString(STR_AGENT_CHAT_ATTACHMENT_TILE, TileX(tile), TileY(tile));

	const std::string sub = GetAttachmentSubtypeName(type, subtype);
	if (sub.empty()) return GetString(STR_AGENT_CHAT_ATTACHMENT_TILE_WHAT, kind, TileX(tile), TileY(tile));
	return GetString(STR_AGENT_CHAT_ATTACHMENT_TILE_KIND_SUBTYPE, kind, sub, TileX(tile), TileY(tile));
}

/**
 * Check whether an attachment is a screenshot of the transcript.
 *
 * A screenshot carries the identity of its artifact, the hash of the stored source image and the
 * size of that source, so the transcript can reserve the right box and ask for exactly that
 * artifact. It has no tile, no file path and no pixels of its own: the pixels are fetched
 * separately and never travel inside a message.
 * @param attachment The attachment.
 * @return \c true when the attachment is a screenshot.
 */
static bool IsImageAttachment(const AgentChatAttachment &attachment)
{
	return attachment.type == "image" && !attachment.artifact_id.empty() &&
			!attachment.sha256.empty() && attachment.width > 0 && attachment.height > 0;
}

/**
 * Get the label of the company that owns a company scoped setting.
 * @param company The company.
 * @return The name of the company, or a plain id when that company is gone.
 */
static std::string SettingOwnerLabel(CompanyID company)
{
	const Company *c = Company::GetIfValid(company);
	if (c == nullptr) return GetString(STR_AGENT_CHAT_ATTACHMENT_COMPANY_ID, company.base());
	if (!c->name.empty()) return SanitiseBackendText(c->name);
	return GetString(STR_COMPANY_NAME, company.base());
}

/**
 * Get the label of an attachment, as shown in the attachment tray.
 * @param attachment The attachment.
 * @return The label.
 */
static std::string GetAttachmentLabel(const AgentChatAttachment &attachment)
{
	/* A setting has no id and no tile: its exact engine key is the name a player can check, and a
	 * company scoped setting also names the company that owns it, because the same key holds a
	 * different value for every company. This comes before the generic name, because the picker
	 * stores that same key as the label and the owner would otherwise never be shown. */
	if (!attachment.setting_name.empty()) {
		const std::string key = SanitiseBackendText(attachment.setting_name);
		if (attachment.scope != "company" || attachment.company_id < 0) return key;
		return GetString(STR_AGENT_CHAT_ATTACHMENT_SETTING_COMPANY, key, SettingOwnerLabel(CompanyID(attachment.company_id)));
	}
	if (!attachment.name.empty()) return SanitiseBackendText(attachment.name);
	/* A pooled object is named by its id; a typed tile object has no id of its own and is named
	 * by its category, subtype and location instead. */
	if (attachment.id >= 0) return GetString(STR_AGENT_CHAT_ATTACHMENT_OBJECT, GetAttachmentTypeName(attachment.type), attachment.id);
	if (attachment.tile != INVALID_TILE) {
		if (attachment.type == "point") return GetString(STR_AGENT_CHAT_ATTACHMENT_POINT, TileX(attachment.tile), TileY(attachment.tile));
		return GetTileAttachmentLabel(attachment.type, attachment.what, GetAttachmentSubtype(attachment), attachment.tile);
	}
	return GetAttachmentTypeName(attachment.type);
}

/**
 * Check whether an attachment refers to an object of another world or epoch.
 *
 * A setting reference also names its scope, so one without a scope cannot resolve at all, and a
 * company scoped setting is stale when the company that owns it is gone or was recreated: the id
 * alone is not an identity, so the reference cannot resolve to that company any more.
 * @param attachment The attachment.
 * @return \c true when the reference cannot be resolved in the current world.
 */
static bool IsAttachmentStale(const AgentChatAttachment &attachment)
{
	if (!attachment.world_id.empty() && attachment.world_id != _agent_chat.world_id) return true;
	if (!attachment.epoch.empty() && attachment.epoch != _agent_chat.epoch) return true;
	if (attachment.type == "setting" && attachment.scope.empty()) return true;
	if (attachment.type == "setting" && attachment.scope == "company" && attachment.company_id >= 0) {
		const Company *company = Company::GetIfValid(CompanyID(attachment.company_id));
		if (company == nullptr || attachment.company_generation < 0 || GetPoolGeneration(*company) != attachment.company_generation) return true;
	}
	return false;
}

/**
 * Check whether a reference type names an object of a pool whose ids are reused.
 *
 * Every pooled object in the game (ComponentBase) has a lifetime generation, so a reference to
 * one without a generation cannot be proven to be the object that was picked. Everything else the
 * bridge can return is addressed by something that has no lifetime of its own: a point or a typed
 * tile reference is a map coordinate, and cargo is a static specification.
 * @param type The reference type.
 * @return \c true when the type is a pooled object.
 */
static bool IsPooledAttachmentType(const std::string &type)
{
	return type == "town" || type == "station" || type == "industry" || type == "vehicle" ||
			type == "sign" || type == "depot" || type == "waypoint" || type == "company" ||
			type == "group" || type == "engine";
}

/**
 * Check whether an attachment refers to a pooled object without a lifetime generation.
 * Ids of pooled objects (towns, stations, industries, vehicles, signs, depots, waypoints) are
 * reused after the object is deleted, so without a generation the reference cannot be proven to
 * be the object that was picked.
 * @param attachment The attachment.
 * @return \c true when the reference is a pooled object whose generation is unknown.
 */
static bool IsAttachmentGenerationUnknown(const AgentChatAttachment &attachment)
{
	if (attachment.generation >= 0) return false;
	/* A company scoped setting is owned by one company lifetime, which is its generation. */
	if (attachment.type == "setting" && attachment.scope == "company") return attachment.company_generation < 0;
	return IsPooledAttachmentType(attachment.type);
}

/**
 * Get the label of an attachment as shown to the player, marking stale references.
 * @param attachment The attachment.
 * @return The label.
 */
static std::string GetAttachmentDisplayLabel(const AgentChatAttachment &attachment)
{
	const std::string label = GetAttachmentLabel(attachment);
	if (IsAttachmentStale(attachment)) return GetString(STR_AGENT_CHAT_ATTACHMENT_STALE, label);
	if (IsAttachmentGenerationUnknown(attachment)) return GetString(STR_AGENT_CHAT_ATTACHMENT_UNVERIFIED, label);
	return label;
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
		/* A screenshot is drawn as a picture, so its label would only repeat it. */
		if (IsImageAttachment(attachment)) continue;
		if (!summary.empty()) summary += ", ";
		summary += GetAttachmentDisplayLabel(attachment);
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
	Json j = attachment.extra.is_object() ? attachment.extra : Json::object();
	j["type"] = attachment.type;
	if (attachment.id >= 0) j["id"] = attachment.id;
	if (attachment.generation >= 0) j["generation"] = attachment.generation;
	/* Always write the canonical object shape; the array form is only read for compatibility. */
	if (attachment.tile != INVALID_TILE) j["tile"] = Json{{"x", TileX(attachment.tile)}, {"y", TileY(attachment.tile)}};
	if (!attachment.name.empty()) j["name"] = attachment.name;
	if (!attachment.setting_name.empty()) j["setting_name"] = attachment.setting_name;
	if (!attachment.scope.empty()) j["scope"] = attachment.scope;
	/* A company scoped setting is owned: the id alone is not an identity, the lifetime is. */
	if (attachment.company_id >= 0) j["company_id"] = attachment.company_id;
	if (attachment.company_generation >= 0) j["company_generation"] = attachment.company_generation;
	if (!attachment.what.empty()) j["what"] = attachment.what;
	if (!attachment.world_id.empty()) j["world_id"] = attachment.world_id;
	if (!attachment.epoch.empty()) j["epoch"] = attachment.epoch;
	if (!attachment.artifact_id.empty()) {
		j["artifact_id"] = attachment.artifact_id;
		j["sha256"] = attachment.sha256;
		j["width"] = attachment.width;
		j["height"] = attachment.height;
		j["tick"] = attachment.tick;
	}
	return j;
}

/**
 * Describe one reference the way `ui.inspect` reports it.
 *
 * The canonical reference fields come from #AttachmentToJson, so a fingerprint, a `what`, a town
 * generation or an object generation the backend reported is reported here as well instead of
 * being dropped. A reference that has no id or no generation (a point, a typed tile object) simply
 * has no such member; it is never given a made up -1. The readable labels and the two flags the
 * tray shows are added on top of the canonical fields.
 * @param attachment The attachment.
 * @return The reference description.
 */
static Json ReferenceToUiJson(const AgentChatAttachment &attachment)
{
	Json entry = AttachmentToJson(attachment);
	/* The plain label, and the label the tray shows, which marks stale and unverified references. */
	entry["label"] = GetAttachmentLabel(attachment);
	entry["name"] = GetAttachmentDisplayLabel(attachment);
	entry["stale"] = IsAttachmentStale(attachment);
	entry["generation_known"] = !IsAttachmentGenerationUnknown(attachment);
	return entry;
}

/**
 * Read the tile of a reference.
 *
 * The canonical shape names the tile as an object `{"x": x, "y": y}`; the legacy shape is the
 * array `[x, y]`. Both are accepted, so a reference written by an older bridge, or snapshotted
 * by the backend before the shape changed, keeps its position instead of silently losing it.
 * @param j The reference.
 * @return The tile, or #INVALID_TILE when the reference carries no usable tile.
 */
static TileIndex ParseTileLocation(const Json &j)
{
	if (!j.is_object() || !j.contains("tile")) return INVALID_TILE;
	const Json &tile = j["tile"];

	int64_t x = -1;
	int64_t y = -1;
	if (tile.is_object()) {
		x = JsonInt(tile, "x", -1);
		y = JsonInt(tile, "y", -1);
	} else if (tile.is_array() && tile.size() == 2 && tile[0].is_number() && tile[1].is_number()) {
		x = tile[0].get<int64_t>();
		y = tile[1].get<int64_t>();
	}

	if (x < 0 || y < 0 || x >= static_cast<int64_t>(Map::SizeX()) || y >= static_cast<int64_t>(Map::SizeY())) return INVALID_TILE;
	return TileXY(static_cast<uint>(x), static_cast<uint>(y));
}

/**
 * Convert a JSON attachment to an attachment.
 * @param j The JSON object.
 * @return The attachment.
 */
static AgentChatAttachment ParseAttachment(const Json &j)
{
	AgentChatAttachment attachment;
	/* A reference without a type is not a point by default; the caller decides what it is. */
	attachment.type = JsonString(j, "type");
	attachment.id = static_cast<int32_t>(JsonInt(j, "id", -1));
	attachment.generation = JsonInt(j, "generation", -1);
	attachment.name = JsonString(j, "name");
	attachment.setting_name = JsonString(j, "setting_name");
	attachment.scope = JsonString(j, "scope");
	attachment.company_id = static_cast<int32_t>(JsonInt(j, "company_id", -1));
	attachment.company_generation = JsonInt(j, "company_generation", -1);
	attachment.what = JsonString(j, "what");
	attachment.world_id = JsonString(j, "world_id");
	attachment.epoch = JsonString(j, "epoch");
	attachment.artifact_id = JsonString(j, "artifact_id");
	attachment.sha256 = JsonString(j, "sha256");
	attachment.width = static_cast<int>(JsonInt(j, "width", 0));
	attachment.height = static_cast<int>(JsonInt(j, "height", 0));
	attachment.tick = JsonInt(j, "tick", 0);

	/* Keep the reference fields this UI does not know about, so they survive a round trip. */
	if (j.is_object()) {
		for (auto it = j.begin(); it != j.end(); ++it) {
			const std::string &key = it.key();
			if (key == "type" || key == "id" || key == "generation" || key == "tile" || key == "name" || key == "setting_name" || key == "scope" || key == "company_id" || key == "company_generation" || key == "what" || key == "world_id" || key == "epoch" ||
					key == "artifact_id" || key == "sha256" || key == "width" || key == "height" || key == "tick") continue;
			attachment.extra[key] = it.value();
		}
	}

	attachment.tile = ParseTileLocation(j);
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
 * Convert a window-local rectangle to screen pixels.
 * Nested widget rectangles and #Window::GetInnerRect are relative to the window, so the window
 * position has to be added before the rectangle can be used for real input.
 * @param w The window the rectangle belongs to.
 * @param r The rectangle, relative to the window.
 * @return The rectangle in screen pixels.
 */
static Rect ToScreenRect(const Window &w, const Rect &r)
{
	return Rect{r.left + w.left, r.top + w.top, r.right + w.left, r.bottom + w.top};
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
 * Get the text of every message of a transcript, in transcript order.
 *
 * A selection is expressed in byte offsets of these texts, so the same list is used to hit
 * test, to draw the highlight and to copy, and a selection can never be copied out of a text
 * that was not the one it was made in.
 * @param chat The chat, may be \c nullptr.
 * @return The texts of the messages of that chat.
 */
static std::vector<std::string_view> TranscriptTexts(const AgentChatEntry *chat)
{
	std::vector<std::string_view> texts;
	if (chat == nullptr) return texts;
	texts.reserve(chat->messages.size());
	for (const AgentChatMessage &message : chat->messages) texts.push_back(message.text);
	return texts;
}

/**
 * Bind the selection to the transcript that is shown now.
 *
 * The binding carries the world and the epoch as well as the chat id, so a load that keeps the
 * same chat id in another world or epoch still drops the selection: the cached transcript it was
 * made in is gone, and its offsets must not be applied to the new text.
 * @param chat The chat that is shown, may be \c nullptr.
 */
static void BindTranscriptSelection(const AgentChatEntry *chat)
{
	_agent_chat.selection.BindChat(chat != nullptr ? chat->id : std::string(), _agent_chat.world_id, _agent_chat.epoch);
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

/**
 * Check whether any chat has a task that is stopping or settling.
 * @return \c true when at least one task still waits for its barrier acknowledgement.
 */
static bool HasFinishingChat()
{
	for (const AgentChatEntry &chat : _agent_chat.chats) {
		if (chat.task_active && (chat.task_finishing || chat.stop_pending)) return true;
	}
	return false;
}

/**
 * Check whether one more periodic read fits into the in-flight budget.
 *
 * Only the periodic reads are gated: an explicit Stop or Send is a user action and is queued
 * unconditionally, so a slow backend can never drop it.
 * @return \c true when the read may be queued.
 */
static bool AgentChatCanQueueRead()
{
	return _agent_chat.pending_requests < AGENT_CHAT_MAX_PENDING_REQUESTS;
}

/** Mark the agent chat windows dirty. */
static void InvalidateAgentChatWindows()
{
	/* The widget state (which buttons are enabled) and the scrollbars are only refreshed by the
	 * invalidation data of the window, so the chat window is invalidated, not only repainted. */
	InvalidateWindowClassesData(WindowClass::AgentChat, 0);
	SetWindowClassesDirty(WindowClass::AgentChat);
	SetWindowClassesDirty(WindowClass::AgentChatStatus);
	SetWindowClassesDirty(WindowClass::AgentChatSettings);
}

/**
 * Check whether a backend task status means the task still owns work.
 *
 * The backend keeps a task active while it is `running`, `stopping` or `settling`
 * (`ACTIVE_TASK_STATUSES` in the backend store), and a queued or dispatched task is active as
 * well. Stop has to stay available for all of them, because a settling task still holds the
 * world gate until its writes and its barrier settled.
 * @param status The task status reported by the backend.
 * @return \c true when the task is still active.
 */
static bool IsActiveTaskStatus(const std::string &status)
{
	return status == "queued" || status == "dispatched" || status == "running" || status == "started" ||
			status == "stopping" || status == "settling";
}

/**
 * Translate a backend task status to the text shown to the player.
 * @param status The task status reported by the backend.
 * @return The status text, or an empty string when the status is unknown.
 */
static std::string GetTaskStatusText(const std::string &status)
{
	if (status == "queued" || status == "dispatched" || status == "running" || status == "started") return GetString(STR_AGENT_CHAT_STATUS_THINKING);
	if (status == "stopping") return GetString(STR_AGENT_CHAT_STATUS_STOPPING);
	if (status == "settling") return GetString(STR_AGENT_CHAT_STATUS_SETTLING);
	if (status == "completed") return GetString(STR_AGENT_CHAT_STATUS_DONE);
	if (status == "failed") return GetString(STR_AGENT_CHAT_STATUS_FAILED);
	if (status == "stopped") return GetString(STR_AGENT_CHAT_STATUS_STOPPED);
	if (status == "stale") return GetString(STR_AGENT_CHAT_STATUS_UNAVAILABLE);
	return {};
}

/**
 * Get the status text of a chat.
 * @param chat The chat.
 * @return The status text.
 */
static std::string GetChatStatusText(const AgentChatEntry &chat)
{
	if (!chat.task_status.empty()) return chat.task_status;
	return GetString(STR_AGENT_CHAT_STATUS_IDLE);
}

/* --- Transport --- */

static void AgentChatLoadMessages(const std::string &chat_id);
static void AgentChatPollEvents(const std::string &chat_id);
static void AgentChatRefreshChat(const std::string &chat_id);

/**
 * Send a request to the agent backend.
 * @param method HTTP method, e.g. "GET" or "POST".
 * @param path Request path, including the query string.
 * @param body Request body, ignored for GET requests.
 * @param on_success Called on the main thread with the response of a successful request.
 * @param on_failure Called on the main thread when the request failed; optional.
 */
static void AgentChatRequest(const std::string &method, const std::string &path, const Json &body, std::function<void(const Json &)> on_success, std::function<void()> on_failure = {})
{
	_agent_chat.pending_requests++;
	InGameAgent::Request(method, path, body, [on_success = std::move(on_success), on_failure = std::move(on_failure)](Json response, bool ok) {
		_agent_chat.pending_requests--;
		if (!ok) {
			if (on_failure) on_failure();
			std::string message = JsonString(response, "message");
			if (message.empty() && response.is_object() && response.contains("error")) message = JsonString(response["error"], "message");
			if (message.empty()) message = JsonString(response, "error");
			if (message.empty()) message = GetString(STR_AGENT_CHAT_ERROR_UNKNOWN);
			_agent_chat.error = GetString(STR_AGENT_CHAT_ERROR, SanitiseBackendText(message));
			InvalidateAgentChatWindows();
			return;
		}
		_agent_chat.error.clear();
		on_success(response);
		InvalidateAgentChatWindows();
	});
}

/** Undo the shade the chat window took to uncover the map while a click was awaited. */
static void RestoreAgentChatPickingShade();

/** Open the settings picker of the chat window. */
static void ShowAgentChatSettingPicker();

/**
 * Close the settings picker of the chat window.
 *
 * The picker belongs to the chat it was opened in, like the candidate list: a chat that is created
 * or selected while it is open gets its own picker, so a choice made for one chat can never land in
 * another. The draft and the attachment tray are not chat scoped, so they are left alone.
 */
static void CloseSettingPicker()
{
	CloseWindowByClass(WindowClass::AgentChatSettings);
}

/** Read the world context of the transport, resetting the chat list when the world changed. */
static void AgentChatSyncContext()
{
	const Json context = InGameAgent::Context();
	const std::string world_id = JsonString(context, "world_id");
	const std::string epoch = JsonString(context, "epoch");
	const bool connected = JsonBool(context, "connected");

	/* Screenshot previews belong to the world and epoch they were fetched in; a switch drops them
	 * and makes every answer still in flight unusable. */
	_agent_chat.previews.BindWorld(world_id, epoch);

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
		/* A tile of the old world cannot be picked any more, so the window must not stay shaded. */
		RestoreAgentChatPickingShade();
		/* The settings picker shows values of the world that was left, so it is closed with it and
		 * cannot attach a reference of the old epoch. */
		CloseWindowByClass(WindowClass::AgentChatSettings);
		/* The text draft does not refer to the world, so it is kept on purpose. */
		/* A submission of the world that was left can no longer be answered, and a late answer of
		 * it must not end the feedback of the new world. The last painted timing keeps the world
		 * and epoch it was measured in. */
		_agent_chat.send_ack.Reset();
		/* The transcript of the world that was left is gone, so its selection cannot be copied. */
		_agent_chat.selection.Clear();
	}

	const bool changed = world_id != _agent_chat.world_id || epoch != _agent_chat.epoch || connected != _agent_chat.connected;

	_agent_chat.world_id = world_id;
	_agent_chat.epoch = epoch;
	_agent_chat.connection_id = JsonString(context, "connection_id");
	_agent_chat.connected = connected;

	/* The enabled state of the widgets follows the connection, and the window is created before the
	 * transport reports it. Refreshing the widget state here is what makes New chat usable on a
	 * cold open; it happens in the tick, never while the window is being painted. */
	if (changed) InvalidateAgentChatWindows();
}

/**
 * Read the task state of a chat from a chat DTO.
 *
 * The backend reports the running task as `task_active`/`task_status`; the
 * `latest_task` object is used as a fallback for payloads that only carry the
 * last task. A chat without any of these fields is treated as idle.
 *
 * `task_active` is authoritative: the backend derives it from its own active statuses
 * (`running`, `stopping`, `settling`), so a settling task keeps Stop available until its
 * writes and its barrier settled. The fallback list has to name the same statuses.
 * @param chat The chat to update.
 * @param dto The chat DTO.
 */
static void ApplyChatTaskState(AgentChatEntry &chat, const Json &dto)
{
	const bool has_active = dto.is_object() && dto.contains("task_active") && !dto["task_active"].is_null();
	chat.task_active = JsonBool(dto, "task_active", false);
	std::string status = JsonString(dto, "task_status");
	chat.task_status = GetTaskStatusText(status);

	const Json &latest = dto.is_object() && dto.contains("latest_task") ? dto["latest_task"] : Json();
	if (latest.is_object()) {
		const std::string latest_status = JsonString(latest, "status");
		if (!has_active) chat.task_active = JsonBool(latest, "active", false) || IsActiveTaskStatus(latest_status);
		if (chat.task_status.empty()) chat.task_status = GetTaskStatusText(latest_status);
		if (status.empty()) status = latest_status;
	}

	/* A settling or stopping task is still active, but its turn already ended; the chat DTO has
	 * to be re-read until the backend reports the task terminal. */
	chat.task_finishing = chat.task_active && (status == "settling" || status == "stopping");
	if (chat.task_active && chat.task_status.empty()) chat.task_status = GetString(STR_AGENT_CHAT_STATUS_THINKING);

	/* A requested stop stays pending until the backend reports the task terminal; a fresh `running`
	 * read alone is not a failure of the stop, it can race the stop request on its way. The stop
	 * request failing clears the flag, and so does the terminal DTO or event. */
	if (!chat.task_active) chat.stop_pending = false;
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
		if (!AgentChatAnswerIsCurrent(world_id, epoch, _agent_chat.world_id, _agent_chat.epoch)) return;

		const AgentChatEntry *previous_chat = GetActiveChat();
		const std::string previous = previous_chat != nullptr ? previous_chat->id : std::string();

		std::vector<AgentChatEntry> refreshed;
		for (const Json &entry : JsonArray(response, "chats")) {
			AgentChatEntry chat;
			chat.id = JsonString(entry, "chat_id", JsonString(entry, "id"));
			if (chat.id.empty()) continue;
			chat.title = SanitiseBackendText(JsonString(entry, "title"));
			ApplyChatTaskState(chat, entry);
			refreshed.push_back(std::move(chat));
		}

		/* The list describes the chats of this world and epoch, so the same chat id is the same
		 * conversation: its cached transcript and its cursors survive the refresh, and a read that
		 * is still in flight can only add to it instead of landing on a rebuilt cache and hiding
		 * the messages that came before the ones it brings. */
		AgentChatMergeChatList(_agent_chat.chats, std::move(refreshed));
		_agent_chat.active = -1;
		_agent_chat.chats_loaded = true;

		for (size_t i = 0; i < _agent_chat.chats.size(); i++) {
			if (_agent_chat.chats[i].id == previous) {
				_agent_chat.active = static_cast<int>(i);
				break;
			}
		}
		if (_agent_chat.active < 0 && !_agent_chat.chats.empty()) _agent_chat.active = 0;

		/* The selection and the settings picker belong to the chat they were made in: another chat
		 * drops them. */
		const AgentChatEntry *active = GetActiveChat();
		if ((active != nullptr ? active->id : std::string()) != previous) CloseSettingPicker();
		BindTranscriptSelection(active);

		if (active != nullptr) AgentChatLoadMessages(active->id);
	});
}

/**
 * Create a new chat and select it.
 * @param then Called with the id of the chat that was created and selected.
 * @param on_failure Called when no chat could be created.
 */
static void AgentChatCreateChat(std::function<void(const std::string &)> then = {}, std::function<void()> on_failure = {})
{
	if (!_agent_chat.connected) {
		_agent_chat.error = GetString(STR_AGENT_CHAT_NOT_CONNECTED);
		InvalidateAgentChatWindows();
		if (on_failure) on_failure();
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

	AgentChatRequest("POST", "/v1/chats", body, [then = std::move(then), on_failure, world_id, epoch](const Json &response) {
		/* The world changed while the request was in flight, so this chat belongs to the world
		 * that was left and nothing may be sent to it. */
		if (world_id != _agent_chat.world_id || epoch != _agent_chat.epoch) {
			if (on_failure) on_failure();
			return;
		}

		const Json &chat_json = response.is_object() && response.contains("chat") ? response["chat"] : response;

		AgentChatEntry chat;
		chat.id = JsonString(chat_json, "chat_id", JsonString(chat_json, "id"));
		if (chat.id.empty()) {
			_agent_chat.error = GetString(STR_AGENT_CHAT_ERROR_UNKNOWN);
			if (on_failure) on_failure();
			return;
		}
		chat.title = SanitiseBackendText(JsonString(chat_json, "title", GetString(STR_AGENT_CHAT_DEFAULT_TITLE, static_cast<int32_t>(_agent_chat.chats.size() + 1))));

		/* The id of the chat that was created, not whatever is selected when the continuation runs:
		 * the player can select another chat in between. */
		const std::string chat_id = chat.id;
		_agent_chat.chats.push_back(std::move(chat));
		_agent_chat.active = static_cast<int>(_agent_chat.chats.size() - 1);
		_agent_chat.chats_loaded = true;
		_agent_chat.selection.BindChat(chat_id, _agent_chat.world_id, _agent_chat.epoch);
		/* The created chat is the active one now, so a picker that was open for the chat the player
		 * was looking at is closed with it. The draft and the tray keep their existing policy. */
		CloseSettingPicker();

		if (then) then(chat_id);
	}, [on_failure]() {
		/* The chat was not created, so there is nothing to send the message to. */
		if (on_failure) on_failure();
	});
}

/**
 * Make sure a chat is selected, creating one when needed.
 * @param then Called with the id of the chat to use.
 * @param on_failure Called when no chat could be selected, so nothing can be sent.
 */
static void AgentChatEnsureChat(std::function<void(const std::string &)> then, std::function<void()> on_failure = {})
{
	/* The chat that is selected now is captured here, so a player who selects another chat while
	 * the message is prepared does not redirect the submission. */
	const AgentChatEntry *chat = GetActiveChat();
	if (chat != nullptr) {
		const std::string chat_id = chat->id;
		then(chat_id);
		return;
	}
	AgentChatCreateChat(std::move(then), std::move(on_failure));
}

/**
 * Load the messages of a chat that were not seen yet.
 * @param chat_id The chat id.
 */
static void AgentChatLoadMessages(const std::string &chat_id)
{
	const AgentChatEntry *chat = FindChatById(chat_id);
	if (chat == nullptr) return;

	const std::string world_id = _agent_chat.world_id;
	const std::string epoch = _agent_chat.epoch;
	const std::string path = "/v1/chats/" + chat_id + "/messages?after=" + fmt::format("{}", chat->message_seq);

	AgentChatRequest("GET", path, Json(), [chat_id, world_id, epoch](const Json &response) {
		/* An answer of the world or epoch that was left must not touch the transcript of a chat that
		 * carries the same id in this one. */
		if (!AgentChatAnswerIsCurrent(world_id, epoch, _agent_chat.world_id, _agent_chat.epoch)) return;

		AgentChatEntry *chat = FindChatById(chat_id);
		if (chat == nullptr) return;

		std::vector<AgentChatMessage> messages;
		int64_t highest = chat->message_seq;
		for (const Json &entry : JsonArray(response, "messages")) {
			AgentChatMessage message;
			message.seq = JsonInt(entry, "seq", 0);
			message.role = JsonString(entry, "role", "assistant");
			message.text = SanitiseBackendText(JsonString(entry, "text"));
			message.status = SanitiseBackendText(JsonString(entry, "status"));
			for (const Json &attachment : JsonArray(entry, "attachments")) {
				message.attachments.push_back(ParseAttachment(attachment));
			}

			highest = std::max(highest, message.seq);
			messages.push_back(std::move(message));
		}

		/* The answer is merged at the sequence positions of its messages instead of being appended
		 * behind the cursor: an answer that overtook another one still fills the transcript, and a
		 * message that was already received is not shown twice. The cursor only moves forward. */
		const AgentChatMergeResult merged = AgentChatMergeMessages(chat->messages, std::move(messages));
		chat->message_seq = highest;

		/* Fall back to the cursor of the backend when the answer carried no message that could be
		 * placed. */
		if (!merged.changed && response.is_object() && response.contains("next_seq")) {
			chat->message_seq = std::max(chat->message_seq, JsonInt(response, "next_seq", chat->message_seq));
		}

		/* Keep the transcript pinned to the bottom while new messages arrive. The offset is
		 * clamped by the scrollbar, so the window has to re-run its layout for it to take effect. */
		if (merged.changed) {
			_agent_chat.scroll = INT32_MAX;
			if (merged.inserted_before) {
				/* A message that went in before cached ones moves their transcript indices, so
				 * the selection would point at other text and is dropped. */
				_agent_chat.selection.Clear();
			} else {
				/* An ordinary answer only appends, so the selection stays on its own text; a
				 * reloaded message may be shorter than the one it was made in. */
				_agent_chat.selection.Clamp(TranscriptTexts(chat));
			}
			InvalidateWindowClassesData(WindowClass::AgentChat);
		}
	});
}

/**
 * Read a field of an event.
 *
 * The backend nests the fields of an event in `payload`; a payload that puts them at the top
 * level is accepted as well, so both shapes are read.
 * @param event The event.
 * @param key The field name.
 * @return The field value, or a null value when the event does not carry it.
 */
static Json EventField(const Json &event, const char *key)
{
	if (!event.is_object()) return Json();
	if (event.contains("payload") && event["payload"].is_object() && event["payload"].contains(key)) return event["payload"][key];
	const auto it = event.find(key);
	return it != event.end() ? *it : Json();
}

/**
 * Read a string field of an event.
 * @param event The event.
 * @param key The field name.
 * @return The field value, or an empty string when the event does not carry it.
 */
static std::string EventString(const Json &event, const char *key)
{
	const Json value = EventField(event, key);
	return value.is_string() ? value.get<std::string>() : std::string();
}

/**
 * Read a boolean field of an event.
 * @param event The event.
 * @param key The field name.
 * @param fallback Value returned when the event does not carry the field.
 * @return The field value.
 */
static bool EventBool(const Json &event, const char *key, bool fallback = false)
{
	const Json value = EventField(event, key);
	return value.is_boolean() ? value.get<bool>() : fallback;
}

/**
 * Apply one event of a chat to the local task state.
 *
 * A turn that ends does not end its task: while one of its mutations is still dispatched the
 * backend keeps the task `settling` (active, stoppable, holding the world gate) and only
 * `turn_settled` makes it terminal. Stop is acknowledged in two steps as well: the stop request
 * marks the task `stopping` and the barrier acknowledgement makes it `stopped`. So an event that
 * ends a turn keeps the task active and marks it as finishing; the chat DTO, which the tick
 * re-reads while a task is finishing, is the authority on when it really ended.
 * @param chat The chat the event belongs to.
 * @param event The event.
 */
static void AgentChatApplyEvent(AgentChatEntry &chat, const Json &event)
{
	const std::string type = JsonString(event, "type");

	if (type == "turn_started" || type == "task_started" || type == "turn_sent") {
		chat.task_active = true;
		chat.task_finishing = false;
		chat.task_status = GetString(STR_AGENT_CHAT_STATUS_THINKING);
	} else if (type == "tool_call") {
		chat.task_active = true;
		const std::string tool = SanitiseBackendText(EventString(event, "tool").empty() ? EventString(event, "name") : EventString(event, "tool"));
		chat.task_status = tool.empty() ? GetString(STR_AGENT_CHAT_STATUS_THINKING) : GetString(STR_AGENT_CHAT_STATUS_TOOL, tool);
	} else if (type == "tool_result") {
		chat.task_active = true;
		chat.task_status = GetString(STR_AGENT_CHAT_STATUS_THINKING);
	} else if (type == "turn_settling") {
		chat.task_active = true;
		chat.task_finishing = true;
		chat.task_status = GetString(STR_AGENT_CHAT_STATUS_SETTLING);
	} else if (type == "turn_completed" || type == "task_completed" || type == "turn_failed" || type == "task_failed" ||
			type == "turn_timeout" || type == "sub_unavailable" || type == "backend_unavailable") {
		/* The turn ended, but its writes may still be settling: keep the task active and let the
		 * chat DTO decide, so Stop stays available until the barrier settled. */
		chat.task_active = true;
		chat.task_finishing = true;
		chat.task_status = GetString(STR_AGENT_CHAT_STATUS_SETTLING);
	} else if (type == "turn_settled") {
		chat.task_active = false;
		chat.task_finishing = false;
		chat.task_status = EventString(event, "status") == "failed" ? GetString(STR_AGENT_CHAT_STATUS_FAILED) : GetString(STR_AGENT_CHAT_STATUS_DONE);
	} else if (type == "turn_cancelled") {
		/* The SUB turn was cancelled; the task is stopping until its barrier is acknowledged. */
		chat.task_active = true;
		chat.task_finishing = true;
		chat.task_status = GetString(STR_AGENT_CHAT_STATUS_STOPPING);
	} else if (type == "stopped" || type == "turn_stopped" || type == "task_stopped") {
		/* The stop request marks the task stopping; only the acknowledged barrier ends it. */
		const bool acknowledged = EventBool(event, "acknowledged", true);
		chat.task_active = !acknowledged;
		chat.task_finishing = !acknowledged;
		chat.task_status = acknowledged ? GetString(STR_AGENT_CHAT_STATUS_STOPPED) : GetString(STR_AGENT_CHAT_STATUS_STOPPING);
	} else if (type == "turn_degraded") {
		/* The turn was requeued after a SUB failure, so the task is still active. */
		chat.task_active = true;
		chat.task_finishing = false;
		chat.task_status = GetString(STR_AGENT_CHAT_STATUS_THINKING);
	} else if (type == "epoch_changed" || type == "world_changed") {
		_agent_chat.chats_loaded = false;
	}

	/* A stop request is only pending while its task exists; the acknowledged barrier ends the task. */
	if (!chat.task_active) chat.stop_pending = false;
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

/**
 * Re-read the task state of one chat from the backend.
 *
 * The event stream reports that a turn ended, but not whether its task is still settling or
 * stopping; the chat DTO carries the authoritative `task_active`/`task_status`, so it is read
 * while a task is finishing and Stop stays available until the backend reports it terminal.
 * @param chat_id The chat id.
 */
static void AgentChatRefreshChat(const std::string &chat_id)
{
	const std::string world_id = _agent_chat.world_id;
	const std::string epoch = _agent_chat.epoch;

	AgentChatRequest("GET", "/v1/chats/" + chat_id, Json(), [chat_id, world_id, epoch](const Json &response) {
		/* Ignore the answer when the world changed while the request was in flight. */
		if (world_id != _agent_chat.world_id || epoch != _agent_chat.epoch) return;

		AgentChatEntry *chat = FindChatById(chat_id);
		if (chat == nullptr) return;

		const Json &dto = response.is_object() && response.contains("chat") ? response["chat"] : response;
		ApplyChatTaskState(*chat, dto);
	});
}

/* --- Send acknowledgement --- */

/**
 * Measure the time between two steady clock points, in microseconds.
 * @param from The earlier point.
 * @param to The later point.
 * @return The duration in microseconds.
 */
static int64_t MicrosSince(std::chrono::steady_clock::time_point from, std::chrono::steady_clock::time_point to)
{
	return std::chrono::duration_cast<std::chrono::microseconds>(to - from).count();
}

/** Outcome of a submission whose feedback was drawn but whose answer has not arrived. */
static const char * const ACK_OUTCOME_PAINTED = "painted";
/** Outcome of a submission the backend acknowledged. */
static const char * const ACK_OUTCOME_ACCEPTED = "accepted";
/** Outcome of a submission that never left, so the backend cannot have the message. */
static const char * const ACK_OUTCOME_NOT_SENT = "not_sent";
/** Outcome of a submission whose request failed after it left, so the backend may have the message. */
static const char * const ACK_OUTCOME_UNCERTAIN = "uncertain";

int64_t AgentChatSendAck::Begin(const std::string &chat_id, const std::string &world_id, const std::string &epoch, const std::string &payload_id, std::chrono::steady_clock::time_point now)
{
	/* One submission at a time: the draft of a message that is on its way must not be sent twice,
	 * and a chat that is still being created for it must not be created a second time either. */
	if (this->pending.send != 0) return 0;

	/* A retry of a message whose acceptance is unknown keeps its client message id, so a backend
	 * that deduplicates on it recognises the second attempt instead of taking the message twice.
	 * Only exactly the same chat and the same payload may reuse it: another attachment, another
	 * order or another chat is a different message and must not be dropped as a duplicate. */
	const bool retry = this->last.outcome == ACK_OUTCOME_UNCERTAIN &&
			this->last.payload_id == payload_id &&
			this->last.chat_id == chat_id &&
			this->last.world_id == world_id && this->last.epoch == epoch;
	const std::string client_message_id = retry ? this->last.client_message_id : fmt::format("{}", now.time_since_epoch().count());

	this->pending = Submission{this->next_send++, chat_id, world_id, epoch, payload_id, client_message_id, now};
	/* The feedback belongs to the submission the player just made, so it replaces the feedback of
	 * an earlier submission that never reached a paint. */
	this->visible = this->pending;
	this->accepted_known = false;
	this->visible_painted = false;
	return this->pending.send;
}

bool AgentChatSendAck::BindChat(int64_t send, const std::string &chat_id)
{
	/* The submission can have been answered, failed or reset while the chat was being created. */
	if (send == 0 || send != this->pending.send || chat_id.empty()) return false;

	this->pending.chat_id = chat_id;
	if (this->visible.send == send) this->visible.chat_id = chat_id;
	return true;
}

AgentChatSendAck::AckResult AgentChatSendAck::Acknowledge(int64_t send, const std::string &world_id, const std::string &epoch, bool draft_unchanged, std::chrono::steady_clock::time_point now)
{
	/* A late answer of an earlier submission, or of one that was never submitted, changes nothing. */
	if (send == 0 || send != this->pending.send) return AckResult::Ignored;
	if (world_id != this->pending.world_id || epoch != this->pending.epoch) {
		/* The world of this submission is gone, so it is not pending any more either. */
		this->pending = Submission{};
		if (this->visible.send == send) this->visible = Submission{};
		return AckResult::Ignored;
	}

	if (this->visible.send == send) {
		this->accepted = now;
		this->accepted_known = true;
	}
	/* The accepted outcome belongs to the submission that was painted; an answer that arrives
	 * before its first paint is recorded by that paint instead. */
	if (this->last.send == send) this->last.outcome = ACK_OUTCOME_ACCEPTED;
	/* The answer ends the pending state, so Send is available again right away; the feedback
	 * stays on screen until the paint that shows the acknowledged state. */
	this->pending = Submission{};
	return draft_unchanged ? AckResult::ClearDraft : AckResult::KeepDraft;
}

void AgentChatSendAck::Fail(int64_t send, const std::string &world_id, const std::string &epoch, FailReason reason)
{
	if (send == 0 || send != this->pending.send) return;

	/* A timing that was already painted for this submission is true and stays; only a submission
	 * that never reached a paint is reported as unpainted. */
	if (this->last.send != send) {
		this->last = Timing{};
		this->last.send = send;
		this->last.chat_id = this->pending.chat_id;
		this->last.world_id = world_id;
		this->last.epoch = epoch;
		this->last.payload_id = this->pending.payload_id;
		this->last.client_message_id = this->pending.client_message_id;
	}
	this->last.outcome = reason == FailReason::Uncertain ? ACK_OUTCOME_UNCERTAIN : ACK_OUTCOME_NOT_SENT;
	this->pending = Submission{};
	if (this->visible.send == send) this->visible = Submission{};
}

bool AgentChatSendAck::Painted(int64_t send, std::chrono::steady_clock::time_point now)
{
	if (send == 0 || send != this->visible.send) return false;

	if (!this->visible_painted) {
		this->last = Timing{};
		this->last.send = send;
		this->last.chat_id = this->visible.chat_id;
		this->last.world_id = this->visible.world_id;
		this->last.epoch = this->visible.epoch;
		this->last.outcome = ACK_OUTCOME_PAINTED;
		this->last.payload_id = this->visible.payload_id;
		this->last.client_message_id = this->visible.client_message_id;
		this->last.painted = true;
		this->last.handler_to_paint_us = MicrosSince(this->visible.handler, now);
		this->visible_painted = true;
	}

	/* The paint that shows the acknowledged state is measured on its own, never mixed into the
	 * time the player waited for the first feedback. */
	if (this->accepted_known && this->last.send == send && !this->last.ack_to_paint_us.has_value()) {
		this->last.ack_to_paint_us = MicrosSince(this->accepted, now);
		this->last.handler_to_ack_us = MicrosSince(this->visible.handler, this->accepted);
		this->last.outcome = ACK_OUTCOME_ACCEPTED;
	}

	/* Once the acknowledged state was painted, the pending feedback has done its job. */
	if (this->accepted_known) this->visible = Submission{};
	return true;
}

void AgentChatSendAck::Reset()
{
	this->pending = Submission{};
	this->visible = Submission{};
	this->accepted_known = false;
	this->visible_painted = false;
}

Json AgentChatSendAck::ToJson() const
{
	const auto micros = [](const std::optional<int64_t> &value) -> Json {
		return value.has_value() ? Json(*value) : Json(nullptr);
	};

	return {
		{"unit", "microseconds"},
		{"clock", "steady_clock"},
		{"scope", "same process only: in-game chat Send handler -> first paint that draws the pending-send feedback, HTTP acceptance -> first paint of the accepted or task status, and Send handler -> HTTP acceptance; no clock of the backend process is mixed in"},
		{"target_us", 100000},
		{"pending", this->Pending()},
		{"last_send", this->last.send},
		{"chat_id", this->last.chat_id},
		{"world_id", this->last.world_id},
		{"epoch", this->last.epoch},
		{"outcome", this->last.outcome},
		{"client_message_id", this->last.client_message_id},
		{"painted", this->last.painted},
		{"handler_to_paint_us", micros(this->last.handler_to_paint_us)},
		{"ack_to_paint_us", micros(this->last.ack_to_paint_us)},
		{"handler_to_ack_us", micros(this->last.handler_to_ack_us)},
	};
}

Json AgentChatAckTimingJson()
{
	return _agent_chat.send_ack.ToJson();
}

/**
 * Build the message body of a composed message, without the client message id.
 *
 * The same function gives the identity of the payload: its canonical JSON is what decides whether
 * a retry sends the message that may already have arrived, and whether the acknowledgement may
 * clear the draft. The text and the attachments in their order are part of it, so an edited or
 * reordered tray is another message.
 * @param text The composed text.
 * @param tray The composed attachments, in order.
 * @return The body of the message.
 */
static Json ComposeMessageBody(const std::string &text, const std::vector<AgentChatAttachment> &tray)
{
	Json body = Json::object();
	body["text"] = text;
	Json attachments = Json::array();
	for (const AgentChatAttachment &attachment : tray) attachments.push_back(AttachmentToJson(attachment));
	body["attachments"] = std::move(attachments);
	return body;
}

/** Send the composed message to the selected chat. */
static void AgentChatSend()
{
	if (_agent_chat.input.empty() && _agent_chat.tray.empty()) return;
	/* A second Send while the first one waits for its answer would send the same draft twice, and
	 * a chat that is still being created for it must not be created twice either. */
	if (_agent_chat.send_ack.Pending()) return;

	/* The submission is captured at the click, before anything asynchronous: creating a chat takes
	 * a round trip, and the feedback the player waits for starts at the click, not at its answer. */
	const auto now = std::chrono::steady_clock::now();
	const std::string world_id = _agent_chat.world_id;
	const std::string epoch = _agent_chat.epoch;
	const std::string text = _agent_chat.input;
	const std::vector<AgentChatAttachment> tray = _agent_chat.tray;
	const AgentChatEntry *active = GetActiveChat();
	const std::string chat_id = active != nullptr ? active->id : std::string();

	/* The body of the message and its identity are the same thing, built once here: a retry of a
	 * message whose acceptance is unknown may only reuse its client message id for exactly this
	 * chat and exactly this payload, attachments in their order. */
	const Json message = ComposeMessageBody(text, tray);
	const std::string payload_id = message.dump();

	const int64_t send = _agent_chat.send_ack.Begin(chat_id, world_id, epoch, payload_id, now);
	if (send == 0) return;
	InvalidateAgentChatWindows();

	/* Nothing was sent, so the draft and the tray stay and the player can send them again. */
	const auto not_sent = [send, world_id, epoch]() {
		_agent_chat.send_ack.Fail(send, world_id, epoch, AgentChatSendAck::FailReason::NotSent);
		InvalidateAgentChatWindows();
	};

	AgentChatEnsureChat([send, world_id, epoch, message, payload_id](const std::string &send_chat_id) {
		/* The world can change while a chat is created; the submission then belongs to the world
		 * that was left and is not sent to the new one. */
		if (world_id != _agent_chat.world_id || epoch != _agent_chat.epoch) {
			_agent_chat.send_ack.Fail(send, world_id, epoch, AgentChatSendAck::FailReason::NotSent);
			InvalidateAgentChatWindows();
			return;
		}

		/* The chat this submission was created for, not whatever is selected now: the player can
		 * select another chat while the chat is created. A submission that is gone by now is not
		 * sent, and a chat that could not be created ends it as not sent. */
		if (!_agent_chat.send_ack.BindChat(send, send_chat_id)) {
			_agent_chat.send_ack.Fail(send, world_id, epoch, AgentChatSendAck::FailReason::NotSent);
			InvalidateAgentChatWindows();
			return;
		}

		Json body = message;
		/* The id of the submission: a retry of a message whose acceptance is unknown reuses it. */
		body["client_message_id"] = _agent_chat.send_ack.PendingClientMessageId();

		AgentChatRequest("POST", "/v1/chats/" + send_chat_id + "/messages", body, [send_chat_id, world_id, epoch, send, payload_id](const Json &response) {
			/* The draft the message was composed from; a newer one must survive the answer. The
			 * payload identity is compared, so an edited or reordered tray is kept as well. */
			const bool draft_unchanged = ComposeMessageBody(_agent_chat.input, _agent_chat.tray).dump() == payload_id;
			/* Both decide: the submission must still own the answer, and the world and epoch it was
			 * issued in must still be the current ones. */
			const bool same_context = world_id == _agent_chat.world_id && epoch == _agent_chat.epoch;
			const AgentChatSendAck::AckResult result = _agent_chat.send_ack.Acknowledge(send, world_id, epoch, draft_unchanged, std::chrono::steady_clock::now());
			/* Ignore the answer when it is late or belongs to a world that was left. */
			if (!same_context || result == AgentChatSendAck::AckResult::Ignored) return;

			AgentChatEntry *chat = FindChatById(send_chat_id);
			if (chat != nullptr) {
				/* The answer carries the task of the message; a queued task is active as well, so
				 * the player sees that the turn is waiting and Stop is available. */
				const Json &task = response.is_object() && response.contains("task") ? response["task"] : Json();
				chat->task_active = true;
				chat->task_finishing = false;
				chat->task_status = GetTaskStatusText(JsonString(task, "status"));
				if (chat->task_status.empty()) chat->task_status = GetString(STR_AGENT_CHAT_STATUS_THINKING);
			}
			if (result == AgentChatSendAck::AckResult::ClearDraft) {
				_agent_chat.input.clear();
				_agent_chat.caret = 0;
				_agent_chat.composed_length = 0;
				_agent_chat.tray.clear();
			}
			_agent_chat.scroll = INT32_MAX;
			InvalidateWindowClassesData(WindowClass::AgentChat);
			AgentChatLoadMessages(send_chat_id);
			AgentChatPollEvents(send_chat_id);
		}, [world_id, epoch, send]() {
			/* The request left but its answer was lost, so the backend may have the message: the
			 * draft and the tray stay, and the player is told that sending again may repeat it. */
			_agent_chat.send_ack.Fail(send, world_id, epoch, AgentChatSendAck::FailReason::Uncertain);
			InvalidateAgentChatWindows();
		});
	}, not_sent);
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

	/* The request is on its way, so the task is stopping from now on - but it is not stopped: only
	 * the acknowledged barrier ends it. Marking it locally lets the fast refresh below start now
	 * instead of on the next one-second poll. */
	AgentChatEntry *pending = FindChatById(id);
	if (pending != nullptr && pending->task_active) {
		pending->stop_pending = true;
		pending->task_finishing = true;
		pending->task_status = GetString(STR_AGENT_CHAT_STATUS_STOPPING);
		InvalidateAgentChatWindows();
	}
	_agent_chat.next_poll = std::chrono::steady_clock::now();

	AgentChatRequest("POST", "/v1/chats/" + id + "/stop", Json::object(), [id](const Json &response) {
		AgentChatEntry *chat = FindChatById(id);
		if (chat != nullptr) {
			/* The stop request only marks the task stopping; it stays active (and stoppable) until
			 * its barrier is acknowledged, so the answer decides whether Stop is still needed. The
			 * answer can still be the stale `stopping` of the request itself, so the chat DTO is
			 * re-read right away and the fast refresh keeps reading it until it is terminal. */
			const Json &task = response.is_object() && response.contains("task") ? response["task"] : Json();
			const std::string status = JsonString(task, "status");
			chat->task_active = IsActiveTaskStatus(status);
			chat->task_finishing = chat->task_active;
			chat->task_status = chat->task_active ? GetTaskStatusText(status) : GetString(STR_AGENT_CHAT_STATUS_STOPPED);
			if (chat->task_active && chat->task_status.empty()) chat->task_status = GetString(STR_AGENT_CHAT_STATUS_STOPPING);
			if (!chat->task_active) chat->stop_pending = false;
		}
		/* Read the authoritative state now, not at the next regular poll. */
		_agent_chat.next_poll = std::chrono::steady_clock::now();
		AgentChatPollEvents(id);
		AgentChatRefreshChat(id);
	}, [id]() {
		/* The stop request itself failed, so nothing is pending any more; the task state is left to
		 * the authoritative reads, which keep the task active until the backend says otherwise. */
		AgentChatEntry *chat = FindChatById(id);
		if (chat != nullptr) chat->stop_pending = false;
	});
}

/* --- Screenshot previews --- */

/**
 * Build the identity of the preview of a screenshot attachment.
 * @param chat The chat the message belongs to.
 * @param attachment The screenshot attachment.
 * @return The key of the preview that has to be shown for it.
 */
static AgentChatPreviewKey GetPreviewKey(const AgentChatEntry &chat, const AgentChatAttachment &attachment)
{
	AgentChatPreviewKey key;
	/* A capture belongs to the world and epoch it was taken in, not to the one that is shown now:
	 * after a load, an older screenshot keeps its own pixels instead of being retargeted at the
	 * current world, where its artifact id means nothing and its source hash would never match.
	 * Only a capture that names no world at all falls back to the current one. */
	key.world_id = !attachment.world_id.empty() ? attachment.world_id : _agent_chat.world_id;
	key.epoch = !attachment.epoch.empty() ? attachment.epoch : _agent_chat.epoch;
	key.chat_id = chat.id;
	key.artifact_id = attachment.artifact_id;
	key.source_sha256 = attachment.sha256;
	return key;
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
 * Get the height the screenshot previews of a message reserve.
 * @param message The message.
 * @param width Available width.
 * @return The height in pixels.
 */
static int GetMessageImagesHeight(const AgentChatMessage &message, int width)
{
	int height = 0;
	for (const AgentChatAttachment &attachment : message.attachments) {
		if (!IsImageAttachment(attachment)) continue;
		const AgentChatPreviewSize size = FitAgentChatPreview(attachment.width, attachment.height,
				std::min(width, AGENT_CHAT_PREVIEW_MAX_WIDTH), AGENT_CHAT_PREVIEW_MAX_HEIGHT);
		if (size.width <= 0) continue;
		if (height > 0) height += WidgetDimensions::scaled.vsep_normal;
		height += size.height;
	}
	return height;
}

/** One screenshot preview of a message: which artifact it shows and where it is drawn. */
struct AgentChatImageRow {
	const AgentChatAttachment *attachment = nullptr; ///< The screenshot attachment.
	Rect rect = {0, 0, -1, -1};                      ///< Screen rectangle reserved for it.
};

/**
 * Lay out the screenshot previews of a message.
 *
 * Every preview gets exactly the box its own aspect ratio fills inside the transcript width, so
 * the height the message reserves is the height of the pixels that are drawn in it.
 * @param message The message.
 * @param area The content rectangle of the message.
 * @param top The screen y the previews start at.
 * @return The previews, in message order.
 */
static std::vector<AgentChatImageRow> LayoutMessageImages(const AgentChatMessage &message, const Rect &area, int top)
{
	std::vector<AgentChatImageRow> result;
	int y = top;
	for (const AgentChatAttachment &attachment : message.attachments) {
		if (!IsImageAttachment(attachment)) continue;
		const AgentChatPreviewSize size = FitAgentChatPreview(attachment.width, attachment.height,
				std::min(area.Width(), AGENT_CHAT_PREVIEW_MAX_WIDTH), AGENT_CHAT_PREVIEW_MAX_HEIGHT);
		if (size.width <= 0) continue;
		if (!result.empty()) y += WidgetDimensions::scaled.vsep_normal;

		AgentChatImageRow row;
		row.attachment = &attachment;
		row.rect = Rect{area.left, y, area.left + size.width - 1, y + size.height - 1};
		result.push_back(row);
		y += size.height;
	}
	return result;
}

/**
 * Get the screen y the screenshot previews of a message start at.
 * @param message The message.
 * @param rect The screen rectangle of the message.
 * @return The y coordinate below the text of the message.
 */
static int GetMessageImagesTop(const AgentChatMessage &message, const Rect &rect)
{
	int top = rect.top + GetCharacterHeight(FontSize::Normal);
	if (!message.text.empty()) top += GetStringHeight(message.text, rect.Width());
	return top;
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
	height += GetMessageImagesHeight(message, width);
	const std::string summary = GetAttachmentSummary(message.attachments);
	if (!summary.empty()) height += GetStringHeight(summary, width);
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

/** Measure a prefix of a rendered transcript line with the font that draws it. */
static int MeasureTextPrefix(std::string_view text)
{
	return static_cast<int>(GetStringBoundingBox(text).width);
}

/**
 * Lay out the selectable text lines of the transcript.
 *
 * The lines come from the same layouter that draws the messages, so wrapping, line height and
 * the byte offsets of a line are the ones that were rendered, not an estimate of them. A hit
 * test therefore lands on the glyphs that are on screen.
 * @param chat The chat, may be \c nullptr.
 * @param area The transcript area.
 * @param scroll The scroll offset in pixels.
 * @return The rendered text lines, in transcript order.
 */
static std::vector<AgentChatTextLine> LayoutTranscriptText(const AgentChatEntry *chat, const Rect &area, int scroll)
{
	std::vector<AgentChatTextLine> result;
	if (chat == nullptr) return result;

	int y = area.top - scroll;
	for (size_t i = 0; i < chat->messages.size(); i++) {
		const AgentChatMessage &message = chat->messages[i];
		int top = y + GetCharacterHeight(FontSize::Normal);

		if (!message.text.empty()) {
			const Layouter layout(message.text, area.Width());
			size_t previous_end = 0;
			for (size_t index = 0; index < layout.size(); index++) {
				const ParagraphLayouter::Line &line = *layout[index];

				AgentChatTextLine text_line;
				text_line.message = i;
				text_line.top = top;
				text_line.leading = line.GetLeading();
				text_line.left = area.left;
				text_line.width = line.GetWidth();

				/* The byte offsets come from the glyphs of the line itself, so a line range never
				 * starts or ends inside a character. A line without a drawable glyph keeps the end
				 * of the previous line as its start. */
				const ptrdiff_t begin = layout.GetCharAtPosition(0, index);
				text_line.begin = begin >= 0 ? static_cast<size_t>(begin) : previous_end;
				const ptrdiff_t last = layout.GetCharAtPosition(std::max(0, line.GetWidth() - 1), index);
				text_line.end = last >= 0 ? std::max(text_line.begin, NextUtf8Boundary(message.text, static_cast<size_t>(last))) : text_line.begin;
				previous_end = text_line.end;

				result.push_back(text_line);
				top += text_line.leading;
			}
		}

		y += GetMessageHeight(message, area.Width());
	}
	return result;
}

/**
 * Lay out the highlighted part of every rendered line of the transcript.
 * @param selection The selection.
 * @param lines The rendered text lines of the transcript.
 * @param chat The chat, may be \c nullptr.
 * @return The rectangles to fill behind the text, in screen coordinates.
 */
static std::vector<Rect> LayoutSelection(const AgentChatSelection &selection, const std::vector<AgentChatTextLine> &lines, const AgentChatEntry *chat)
{
	std::vector<Rect> result;
	if (chat == nullptr || selection.Empty()) return result;

	for (const AgentChatTextLine &line : lines) {
		if (line.message >= chat->messages.size()) continue;
		const std::string_view text = chat->messages[line.message].text;
		const std::optional<std::pair<size_t, size_t>> range = selection.RangeIn(line.message, text.size());
		if (!range.has_value()) continue;

		const size_t from = std::max(range->first, line.begin);
		const size_t to = std::min(range->second, line.end);
		if (from >= to) continue;

		const std::string_view line_text = text.substr(line.begin, line.end - line.begin);
		const int left = line.left + WidthForOffset(line_text, from - line.begin, MeasureTextPrefix);
		const int right = line.left + WidthForOffset(line_text, to - line.begin, MeasureTextPrefix);
		result.push_back(Rect{left, line.top, std::max(left, right - 1), line.top + line.leading - 1});
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
		const int width = GetStringBoundingBox(GetAttachmentDisplayLabel(attachment)).width + WidgetDimensions::scaled.hsep_wide * 2 + GetCharacterHeight(FontSize::Normal);
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

/**
 * Request the screenshot previews the transcript shows but does not have yet.
 *
 * Only the previews that are on screen are fetched, so a long chat cannot churn the cache by
 * asking for every screenshot it ever showed. One request per artifact is in flight at a time,
 * the number of them is bounded, and a preview that is cached or whose failure is remembered is
 * not requested again. The transport budget is checked before a request is started, so an
 * exhausted budget is never mistaken for a preview that cannot be loaded. The answer is only
 * stored when the world and epoch it was requested in are still the current ones, so a late
 * answer is never shown for another world.
 */
static void AgentChatEnsurePreviews()
{
	Window *w = FindWindowById(WindowClass::AgentChat, 0);
	if (w == nullptr) return;

	const AgentChatEntry *chat = GetActiveChat();
	if (chat == nullptr) return;

	const Rect area = w->GetWidget<NWidgetBase>(WID_AC_TRANSCRIPT)->GetCurrentRect().Shrink(WidgetDimensions::scaled.framerect);
	const std::vector<Rect> layout = LayoutTranscript(chat, area, _agent_chat.scroll);
	if (layout.size() != chat->messages.size()) return;

	for (size_t i = 0; i < layout.size(); i++) {
		if (layout[i].bottom < area.top || layout[i].top > area.bottom) continue;

		const AgentChatMessage &message = chat->messages[i];
		const int top = GetMessageImagesTop(message, layout[i]);
		for (const AgentChatImageRow &row : LayoutMessageImages(message, layout[i], top)) {
			if (row.rect.bottom < area.top || row.rect.top > area.bottom) continue;
			if (_agent_chat.previews.InflightCount() >= AGENT_CHAT_MAX_PREVIEW_REQUESTS) return;

			const AgentChatPreviewKey key = GetPreviewKey(*chat, *row.attachment);
			if (_agent_chat.previews.Find(key) != nullptr) continue;
			if (_agent_chat.previews.Failure(key) != nullptr) continue;

			/* A request that is not started must not be remembered as a failed preview. */
			if (!AgentChatCanQueueRead()) return;
			if (!_agent_chat.previews.BeginLoad(key)) continue;

			const uint64_t generation = _agent_chat.previews.Generation();
			const std::string path = fmt::format("/v1/chats/{}/artifacts/{}/preview?width={}&height={}",
					chat->id, row.attachment->artifact_id, AGENT_CHAT_PREVIEW_MAX_WIDTH, AGENT_CHAT_PREVIEW_MAX_HEIGHT);
			AgentChatRequest("GET", path, Json(), [key, generation](const Json &response) {
				std::string error;
				std::optional<AgentChatPreviewPixels> pixels = ParseAgentChatPreview(response, key, &error);
				if (pixels.has_value()) {
					_agent_chat.previews.Store(generation, key, std::move(*pixels));
				} else {
					_agent_chat.previews.Fail(generation, key, std::move(error));
				}
				InvalidateAgentChatWindows();
			}, [key, generation]() {
				_agent_chat.previews.Fail(generation, key, "request failed");
				InvalidateAgentChatWindows();
			});
		}
	}
}

/* --- Windows --- */

/** The native in-game agent chat window. */
struct AgentChatWindow : Window {
	Scrollbar *chat_scroll = nullptr;       ///< Scrollbar of the chat list.
	Scrollbar *transcript_scroll = nullptr; ///< Scrollbar of the transcript.

	/* Transcript selection: the press that starts it, and whether it moved. */
	bool transcript_focused = false;          ///< The transcript owns the selection keys (Ctrl+A, Ctrl+C).
	bool selection_pressed = false;           ///< A press in the transcript waits for its release.
	bool selection_dragged = false;           ///< That press moved, so it selects text instead of clicking.
	Point selection_press_pt = {0, 0};        ///< Where that press landed, for the click it may still be.

	/* Temporary shade that uncovers the map while a click is awaited. */
	bool picking_shaded = false;              ///< This window was shaded by StartPicking().
	bool picking_was_shaded = false;          ///< Shade state before StartPicking() shaded it.
	bool picking_had_input_focus = false;     ///< The message input had the focus before picking.
	Rect picking_restore_rect = {0, 0, 0, 0}; ///< Window rectangle before StartPicking() shaded it.

	AgentChatWindow(WindowDesc &desc) : Window(desc)
	{
		this->CreateNestedTree();
		this->chat_scroll = this->GetScrollbar(WID_AC_CHAT_SCROLL);
		this->transcript_scroll = this->GetScrollbar(WID_AC_TRANSCRIPT_SCROLL);
		this->FinishInitNested(0);
		this->SetFocusedWidget(WID_AC_INPUT);
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

		/* A submission that waits for its answer keeps Send disabled: the same draft must not be
		 * sent twice, but the draft itself stays in the editor. */
		this->SetWidgetDisabledState(WID_AC_SEND, !_agent_chat.connected ||
				(_agent_chat.input.empty() && _agent_chat.tray.empty()) || _agent_chat.send_ack.Pending());
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

			const std::string title = SanitiseBackendText(chat.title.empty() ? chat.id : chat.title);
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

		/* The highlight goes behind the text, and is laid out from the selection offsets of the
		 * cached transcript, so it follows the text when the transcript is scrolled or repainted. */
		const std::vector<AgentChatTextLine> text_lines = LayoutTranscriptText(chat, area, _agent_chat.scroll);
		for (const Rect &highlight : LayoutSelection(_agent_chat.selection, text_lines, chat)) {
			GfxFillRect(highlight, PC_DARK_BLUE);
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

			/* The screenshots sit below the text, in the boxes the layout reserved for them. */
			const int images_top = GetMessageImagesTop(message, rect);
			for (const AgentChatImageRow &row : LayoutMessageImages(message, rect, images_top)) {
				this->DrawMessageImage(*chat, *row.attachment, row.rect);
			}
			const int images_height = GetMessageImagesHeight(message, rect.Width());
			if (images_height > 0) y = std::max(y, images_top + images_height);

			const std::string summary = GetAttachmentSummary(message.attachments);
			if (!summary.empty()) {
				DrawStringMultiLine(rect.left, rect.right, y, rect.bottom, summary, TextColour::Grey);
			}
		}

		this->DrawCandidates(area);
	}

	/**
	 * Draw one screenshot preview of the transcript.
	 *
	 * The pixels are the artifact's own. Until they arrive, and when they cannot arrive, the
	 * reserved box says what the player is waiting for instead of showing a made up picture.
	 * @param chat The chat the message belongs to.
	 * @param attachment The screenshot attachment.
	 * @param rect The screen rectangle that was reserved for it.
	 */
	void DrawMessageImage(const AgentChatEntry &chat, const AgentChatAttachment &attachment, const Rect &rect) const
	{
		GfxFillRect(rect, PC_BLACK);

		const AgentChatPreviewKey key = GetPreviewKey(chat, attachment);
		const AgentChatPreviewPixels *pixels = _agent_chat.previews.Find(key);
		if (pixels != nullptr) {
			DrawAgentChatPreviewPixels(rect, *pixels);
			return;
		}

		const StringID text = !IsAgentChatPreviewDrawable() ? STR_AGENT_CHAT_IMAGE_UNSUPPORTED :
				(_agent_chat.previews.Failure(key) != nullptr ? STR_AGENT_CHAT_IMAGE_UNAVAILABLE : STR_AGENT_CHAT_IMAGE_LOADING);
		DrawStringMultiLine(rect.Shrink(WidgetDimensions::scaled.framerect), GetString(text), TextColour::Grey);
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
			std::string label = SanitiseBackendText(JsonString(candidate, "name"));
			if (label.empty()) {
				AgentChatAttachment preview = ParseAttachment(candidate);
				if (preview.type.empty()) preview.type = "object";
				label = GetAttachmentLabel(preview);
			}
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
			DrawString(chip.left + WidgetDimensions::scaled.hsep_wide, chip.right - remove_width, chip.top + WidgetDimensions::scaled.vsep_normal / 2, GetAttachmentDisplayLabel(_agent_chat.tray[i]), TextColour::Black);
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
		/* The message box keeps its own buffer, so its edit box widget has no QueryString and draws
		 * nothing by itself: the frame and the background are drawn here, with the theme of a
		 * standard edit box. The background is filled on every paint, so a character that was
		 * deleted cannot stay on screen as a ghost of the previous text. */
		const NWidgetLeaf *leaf = this->GetWidget<NWidgetLeaf>(WID_AC_INPUT);
		DrawFrameRect(r, leaf->colour, {FrameFlag::Lowered, FrameFlag::Darkened});
		GfxFillRect(r.Shrink(WidgetDimensions::scaled.bevel), PC_BLACK);

		/* The text area is the one the hit test, the caret and the IME rects use. */
		const Rect area = r.Shrink(WidgetDimensions::scaled.framerect);
		const int line_height = GetCharacterHeight(FontSize::Normal);
		const InputLayout layout = this->LayoutInput(area);

		/* Clip the text and the caret to the box, like the standard edit box does. */
		DrawPixelInfo dpi;
		if (!FillDrawPixelInfo(&dpi, area)) return;
		dpi.left += area.left;
		dpi.top += area.top;
		AutoRestoreBackup dpi_backup(_cur_dpi, &dpi);

		int y = area.top;
		for (size_t i = layout.first_line; i < layout.lines.size() && y + line_height - 1 <= area.bottom; i++) {
			DrawString(area.left, area.right, y, layout.lines[i], TextColour::Yellow);
			y += line_height;
		}

		/* Draw the caret. */
		if (layout.caret_line >= layout.first_line && layout.caret_line - layout.first_line < static_cast<size_t>(layout.visible_lines)) {
			const Point caret = this->GetInputCaretPoint(area, layout);
			GfxFillRect(Rect{caret.x, caret.y, std::min(caret.x + 1, area.right), caret.y + line_height - 1}, PC_WHITE);
		}
	}

	/** The message input split into the lines that are shown, and where the caret is in them. */
	struct InputLayout {
		std::vector<std::string> lines; ///< The input split into lines.
		size_t caret_line = 0;          ///< Line the caret is on.
		size_t first_line = 0;          ///< First visible line.
		int visible_lines = 1;          ///< Number of visible lines.
	};

	/**
	 * Split the message input into lines and find the line the caret is on.
	 * @param area The area inside the frame of the input widget.
	 * @return The layout of the input.
	 */
	InputLayout LayoutInput(const Rect &area) const
	{
		InputLayout layout;
		layout.visible_lines = std::max(1, area.Height() / GetCharacterHeight(FontSize::Normal));

		size_t line_start = 0;
		while (true) {
			const size_t end = _agent_chat.input.find('\n', line_start);
			if (end == std::string::npos) {
				layout.lines.push_back(_agent_chat.input.substr(line_start));
				if (_agent_chat.caret >= line_start) layout.caret_line = layout.lines.size() - 1;
				break;
			}
			layout.lines.push_back(_agent_chat.input.substr(line_start, end - line_start));
			if (_agent_chat.caret >= line_start && _agent_chat.caret <= end) layout.caret_line = layout.lines.size() - 1;
			line_start = end + 1;
		}

		layout.first_line = layout.caret_line + 1 > static_cast<size_t>(layout.visible_lines) ? layout.caret_line + 1 - layout.visible_lines : 0;
		return layout;
	}

	/**
	 * Get the position of the caret in the message input.
	 * @param area The area inside the frame of the input widget.
	 * @param layout The layout of the input, computed from the same area.
	 * @return The caret position, relative to the window.
	 */
	Point GetInputCaretPoint(const Rect &area, const InputLayout &layout) const
	{
		const std::string &line = layout.lines[layout.caret_line];
		size_t line_begin = 0;
		for (size_t i = 0; i < layout.caret_line; i++) line_begin += layout.lines[i].size() + 1;
		const size_t offset = _agent_chat.caret > line_begin ? std::min(_agent_chat.caret - line_begin, line.size()) : 0;
		const int caret_x = std::min(area.right, area.left + static_cast<int>(GetStringBoundingBox(std::string_view(line).substr(0, offset)).width));
		const int caret_y = area.top + static_cast<int>(layout.caret_line - layout.first_line) * GetCharacterHeight(FontSize::Normal);
		return {caret_x, caret_y};
	}

	/**
	 * Get the position of the caret in the message input.
	 * @param area The area inside the frame of the input widget.
	 * @return The caret position, relative to the window.
	 */
	Point GetInputCaretPoint(const Rect &area) const
	{
		return this->GetInputCaretPoint(area, this->LayoutInput(area));
	}

	/**
	 * Draw the status line.
	 * @param r The area of the status widget.
	 */
	void DrawStatus(const Rect &r) const
	{
		const Rect area = r.Shrink(WidgetDimensions::scaled.framerect);
		const AgentChatEntry *chat = GetActiveChat();

		/* The feedback of a submission belongs to the chat it was sent to: a player who switched to
		 * another chat sees that chat's status, and this paint is not stamped for the submission. A
		 * submission whose chat is still being created has no chat to match yet. */
		const int64_t pending_send = _agent_chat.send_ack.Visible();
		const std::string &pending_chat = _agent_chat.send_ack.VisibleChatId();
		const bool feedback_here = pending_send != 0 && (pending_chat.empty() || (chat != nullptr && chat->id == pending_chat));

		/* Until the answer arrived the feedback is the pending text, drawn from the next paint after
		 * the Send handler, so the click shows before the backend answers. */
		if (feedback_here && !_agent_chat.send_ack.VisibleAcknowledged()) {
			DrawString(area.left, area.right, area.top, GetString(STR_AGENT_CHAT_STATUS_SENDING), TextColour::Grey);
			_agent_chat.send_ack.Painted(pending_send, std::chrono::steady_clock::now());
			return;
		}

		std::string text;
		if (!_agent_chat.connected) {
			text = GetString(STR_AGENT_CHAT_NOT_CONNECTED);
		} else if (pending_send == 0 && _agent_chat.send_ack.LastOutcome() == ACK_OUTCOME_UNCERTAIN &&
				_agent_chat.send_ack.LastWorldId() == _agent_chat.world_id && _agent_chat.send_ack.LastEpoch() == _agent_chat.epoch) {
			/* The answer of the last send was lost, so whether the backend took the message is
			 * unknown: say so instead of letting the player repeat it silently. It outranks the
			 * generic error text, which describes the request rather than what the player may do. */
			text = GetString(STR_AGENT_CHAT_SEND_UNCERTAIN);
		} else if (!_agent_chat.error.empty()) {
			text = _agent_chat.error;
		} else if (chat != nullptr) {
			text = GetChatStatusText(*chat);
		}

		if (!text.empty()) {
			DrawString(area.left, area.right, area.top, text, _agent_chat.error.empty() ? TextColour::Grey : TextColour::Red);
			/* The accepted or task status is on screen now, so this is the paint that may be
			 * recorded as the one after the acceptance; the pending text above never is. */
			if (feedback_here) _agent_chat.send_ack.Painted(pending_send, std::chrono::steady_clock::now());
		}
	}

	void OnClick(Point pt, WidgetID widget, [[maybe_unused]] int click_count) override
	{
		/* Only the transcript takes the selection keys; any other widget gives them back to the
		 * message box, so Ctrl+A in the input keeps its own meaning. */
		this->transcript_focused = widget == WID_AC_TRANSCRIPT;

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

			case WID_AC_ADD_COMPANY:
				this->ShowCompanyCandidates();
				break;
			case WID_AC_ADD_SETTING:
				ShowAgentChatSettingPicker();
				break;

			case WID_AC_CHAT_LIST:
				this->ClickChatList(pt);
				break;

			case WID_AC_TRAY:
				this->ClickTray(pt);
				break;

			case WID_AC_TRANSCRIPT:
				this->PressTranscript(pt);
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
		/* The picker was opened for the chat that is left, so it closes with the switch. */
		CloseSettingPicker();
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
				/* A screenshot has no tile; clicking it must not center the map on an invalid one. */
				if (IsImageAttachment(attachment)) continue;
				if (attachment.tile != INVALID_TILE) {
					ScrollMainWindowToTile(attachment.tile);
					return;
				}
			}
		}
	}

	/**
	 * Handle a press in the transcript: start a text selection, or act on the object list.
	 *
	 * The click itself, i.e. centering the map on the object of a message, is handled when the
	 * button is released and nothing was dragged, so selecting text never acts as a click.
	 * @param pt Press position.
	 */
	void PressTranscript(Point pt)
	{
		/* The object list is a click target of its own and keeps acting on the press. */
		if (_agent_chat.candidates_open) {
			this->ClickTranscript(pt);
			return;
		}

		this->selection_pressed = true;
		this->selection_dragged = false;
		this->selection_press_pt = pt;

		const AgentChatEntry *chat = GetActiveChat();
		BindTranscriptSelection(chat);

		const std::vector<AgentChatTextLine> lines = LayoutTranscriptText(chat, this->GetInnerRect(WID_AC_TRANSCRIPT), _agent_chat.scroll);
		const std::optional<AgentChatSelectionPoint> point = this->SelectionPointAt(lines, pt);
		if (point.has_value()) {
			/* A new press replaces the previous selection, like a text cursor does. */
			_agent_chat.selection.Start(*point);
		} else {
			_agent_chat.selection.Clear();
		}
		this->SetWidgetDirty(WID_AC_TRANSCRIPT);
	}

	/**
	 * Map a point in the transcript to the text under it.
	 *
	 * The point is looked up in the rendered lines of the transcript, so the offset is the byte
	 * of the glyph that is under the pointer; a point outside the transcript selects the nearest
	 * line, so a drag that leaves the panel keeps selecting.
	 * @param lines The rendered text lines of the transcript.
	 * @param pt The point, relative to the window.
	 * @return The selection point, or nothing when the transcript has no selectable text.
	 */
	std::optional<AgentChatSelectionPoint> SelectionPointAt(const std::vector<AgentChatTextLine> &lines, Point pt) const
	{
		const AgentChatEntry *chat = GetActiveChat();
		if (chat == nullptr || lines.empty()) return std::nullopt;

		size_t best = 0;
		int best_distance = INT32_MAX;
		for (size_t i = 0; i < lines.size(); i++) {
			const AgentChatTextLine &line = lines[i];
			const int distance = pt.y < line.top ? line.top - pt.y : (pt.y > line.top + line.leading - 1 ? pt.y - (line.top + line.leading - 1) : 0);
			if (distance >= best_distance) continue;
			best_distance = distance;
			best = i;
			if (distance == 0) break;
		}

		const AgentChatTextLine &line = lines[best];
		if (line.message >= chat->messages.size()) return std::nullopt;

		const std::string_view text = chat->messages[line.message].text;
		const std::string_view line_text = text.substr(line.begin, line.end - line.begin);
		const size_t offset = line.begin + OffsetForX(line_text, pt.x - line.left, MeasureTextPrefix);
		return AgentChatSelectionPoint{line.message, SnapUtf8Boundary(text, offset)};
	}

	/** Select the text of the whole transcript of the active chat. */
	void SelectAllTranscript()
	{
		const AgentChatEntry *chat = GetActiveChat();
		if (chat == nullptr) return;
		BindTranscriptSelection(chat);

		AgentChatSelectionPoint first{};
		AgentChatSelectionPoint last{};
		bool any = false;
		for (size_t i = 0; i < chat->messages.size(); i++) {
			const std::string &text = chat->messages[i].text;
			if (text.empty()) continue;
			if (!any) first = AgentChatSelectionPoint{i, 0};
			last = AgentChatSelectionPoint{i, text.size()};
			any = true;
		}

		if (!any) {
			_agent_chat.selection.Clear();
		} else {
			_agent_chat.selection.Start(first);
			_agent_chat.selection.Extend(last);
		}
		this->SetWidgetDirty(WID_AC_TRANSCRIPT);
	}

	/** Copy the selected transcript text to the system clipboard. */
	void CopyTranscriptSelection()
	{
		const std::string text = _agent_chat.selection.GetText(TranscriptTexts(GetActiveChat()));
		if (text.empty()) return;
		SetClipboardContents(text);
	}

	/**
	 * Follow a press in the transcript until the button is released.
	 *
	 * While the button is down the end of the selection follows the mouse; a press that is
	 * released without moving is the single click the transcript had before, so an objectlink
	 * still centers the map.
	 */
	void OnMouseLoop() override
	{
		if (!this->selection_pressed) return;

		if (_left_button_down) {
			const Point pt = {_cursor.pos.x - this->left, _cursor.pos.y - this->top};

			/* A pointer that left the press point is a drag, also when it is not over text: the
			 * attachment of a message without text must not center the map after a drag. */
			const int drag_distance = ScaleGUITrad(2);
			const int dx = pt.x - this->selection_press_pt.x;
			const int dy = pt.y - this->selection_press_pt.y;
			if (dx * dx + dy * dy > drag_distance * drag_distance) this->selection_dragged = true;

			const std::vector<AgentChatTextLine> lines = LayoutTranscriptText(GetActiveChat(), this->GetInnerRect(WID_AC_TRANSCRIPT), _agent_chat.scroll);
			const std::optional<AgentChatSelectionPoint> point = this->SelectionPointAt(lines, pt);
			if (!point.has_value()) return;

			if (!point->Same(_agent_chat.selection.anchor)) this->selection_dragged = true;
			if (this->selection_dragged) {
				_agent_chat.selection.Extend(*point);
				this->SetWidgetDirty(WID_AC_TRANSCRIPT);
			}
			return;
		}

		this->selection_pressed = false;
		if (!this->selection_dragged) this->ClickTranscript(this->selection_press_pt);
		this->selection_dragged = false;
		this->SetWidgetDirty(WID_AC_TRANSCRIPT);
	}

	/**
	 * Handle a click in the message input: move the caret to the clicked position.
	 * @param pt Click position.
	 */
	void ClickInput(Point pt)
	{
		const Rect area = this->GetInnerRect(WID_AC_INPUT);
		if (!area.Contains(pt)) return;

		/* The window has to own the edit box focus for the backend to deliver text input. */
		this->SetFocusedWidget(WID_AC_INPUT);

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
	 * Shade this window while a map click is awaited, so the chat does not cover the map.
	 *
	 * Only this window is touched. The shade state, the rectangle and the input focus are
	 * remembered so RestoreAfterPicking() can put them back exactly; a window that cannot shade
	 * keeps its state untouched.
	 */
	void ShadeForPicking()
	{
		if (this->picking_shaded || this->shade_select == nullptr) return;

		this->picking_was_shaded = this->IsShaded();
		this->picking_had_input_focus = this->nested_focus != nullptr && this->nested_focus->type == WWT_EDITBOX;
		this->picking_restore_rect = Rect{this->left, this->top, this->left + this->width - 1, this->top + this->height - 1};
		this->picking_shaded = true;
		this->SetShaded(true);
		this->SetDirty();
	}

	/** Undo #ShadeForPicking(): put the shade state, the rectangle and the input focus back. */
	void RestoreAfterPicking()
	{
		if (!this->picking_shaded) return;
		this->picking_shaded = false;

		/* A closing window keeps what it has; the engine does not restore anything for it. */
		if (this->z_position == WindowList::iterator() || *this->z_position != this) return;

		this->SetShaded(this->picking_was_shaded);
		if (!this->picking_was_shaded) {
			/* SetShaded() restores the size, but a window that was moved while it was shaded can
			 * come back moved and clamped at the screen edge, so the remembered rectangle wins. */
			const int restore_width = this->picking_restore_rect.right - this->picking_restore_rect.left + 1;
			const int restore_height = this->picking_restore_rect.bottom - this->picking_restore_rect.top + 1;
			if (this->width != restore_width || this->height != restore_height) {
				ResizeWindow(this, restore_width - this->width, restore_height - this->height, false, false);
			}
			this->left = this->picking_restore_rect.left;
			this->top = this->picking_restore_rect.top;
		}
		if (this->picking_had_input_focus && this->nested_focus == nullptr) this->SetFocusedWidget(WID_AC_INPUT);
		this->UpdateWidgetStates();
		this->SetDirty();
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
		/* The map has to be reachable: an open chat window covers the tiles around the camera. */
		this->ShadeForPicking();
		this->UpdateWidgetStates();
		this->SetDirty();
	}

	void OnPlaceObject([[maybe_unused]] Point pt, TileIndex tile) override
	{
		this->RestoreAfterPicking();
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
		this->RestoreAfterPicking();
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

		/* A point stays a point, also when the tile is occupied: the object picker is a separate
		 * button, so picking a point never turns into an object reference. */
		AgentChatAttachment attachment;
		attachment.type = "point";
		attachment.tile = tile;
		attachment.world_id = _agent_chat.world_id;
		attachment.epoch = _agent_chat.epoch;
		attachment.name = GetString(STR_AGENT_CHAT_ATTACHMENT_POINT, TileX(tile), TileY(tile));
		_agent_chat.tray.push_back(std::move(attachment));
		this->UpdateWidgetStates();
		this->SetDirty();
	}

	/**
	 * Offer every company of the game as an attachment, using the same candidate list as a map
	 * click with several objects. The lifetime generation of the company is sent along when the
	 * game tracks one, so a company that was recreated with the same id does not silently take the
	 * place of the picked one.
	 */
	void ShowCompanyCandidates()
	{
		Json candidates = Json::array();
		for (const Company *company : Company::Iterate()) {
			const std::string name = SanitiseBackendText(company->name.empty() ? GetString(STR_COMPANY_NAME, company->index) : company->name);
			/* Companies are pooled: the id of a company that was removed can be handed out again,
			 * so the lifetime generation has to travel with the reference. It is left out when the
			 * pool of this build does not track a generation, which the bridge reports as unverified
			 * rather than as a verified identity. */
			Json candidate = {{"type", "company"}, {"id", company->index.base()}, {"name", name},
					{"world_id", _agent_chat.world_id}, {"epoch", _agent_chat.epoch}};
			const int64_t generation = GetPoolGeneration(*company);
			if (generation >= 0) candidate["generation"] = generation;
			candidates.push_back(std::move(candidate));
		}
		if (candidates.empty()) return;

		_agent_chat.candidates = std::move(candidates);
		_agent_chat.candidates_open = true;
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
			attachment.world_id = _agent_chat.world_id;
			attachment.epoch = _agent_chat.epoch;
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
		/* A typed tile row has no pool id: label it by category, subtype and tile, never by -1. */
		if (attachment.name.empty()) attachment.name = GetAttachmentLabel(attachment);
		/* The candidate may already name its world and epoch; only fill in what is missing. */
		if (attachment.world_id.empty()) attachment.world_id = _agent_chat.world_id;
		if (attachment.epoch.empty()) attachment.epoch = _agent_chat.epoch;
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

		/* The console key is a global hotkey, offered only after every window had its turn; taking it
		 * here would type it into the message and the console would never open. It keeps the same
		 * priority as Escape, so it is never eaten as text even while this window is not focused. */
		if (base == WKC_BACKQUOTE) return EventState::NotHandled;

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

		/* The transcript owns the two selection keys while it has the focus. Taking them here, in
		 * the focused window, keeps them away from the global hotkeys and from the message box,
		 * so Ctrl+A selects the transcript text and nothing is sent or centered. */
		if (this->transcript_focused) {
			if (keycode == ('A' | WKC_CTRL)) {
				this->SelectAllTranscript();
				return EventState::Handled;
			}
			if (keycode == ('C' | WKC_CTRL)) {
				this->CopyTranscriptSelection();
				return EventState::Handled;
			}
		}

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
			this->ResetComposition();
			this->SetWidgetDirty(WID_AC_INPUT);
			return EventState::Handled;
		}

		if (base == WKC_END) {
			_agent_chat.caret = _agent_chat.input.size();
			this->ResetComposition();
			this->SetWidgetDirty(WID_AC_INPUT);
			return EventState::Handled;
		}

		if (key != 0 && IsPrintable(key)) {
			/* A character reaches the game either as text input (SDL and Cocoa: InsertTextString)
			 * or as a key press that carries the character (Windows: WM_CHAR, and the SDL backend
			 * when no edit box has the text focus). No backend sends both for the same character,
			 * so both paths insert, and neither duplicates the other. */
			auto [buf, len] = EncodeUtf8(key);
			this->InsertInput(std::string_view(buf, len));
			return EventState::Handled;
		}

		return EventState::NotHandled;
	}

	/**
	 * Insert text the backend delivered for the message box.
	 *
	 * Everything the keyboard layout produces arrives here: plain characters, characters that need
	 * a composition step (Cyrillic with a dead key, accented input) and the preedit of an IME. The
	 * message box is its own multiline buffer rather than a Textbuf, so the insertion happens here.
	 *
	 * The console key is passed on to the key handler instead of being inserted: a backend that
	 * delivers text input (SDL, Cocoa) does not send a key press for it while an edit box has the
	 * text focus, so the global console hotkey would never see it. The key handler offers it to
	 * every window, this one included, which answers NotHandled for it.
	 * @param wid The widget the text is for.
	 * @param str The text to insert, UTF-8 encoded.
	 * @param marked Whether the text is IME text that is still being composed.
	 */
	void InsertTextString(WidgetID wid, std::string_view str, bool marked, std::optional<size_t> caret, std::optional<size_t> insert_location, std::optional<size_t> replacement_end) override
	{
		if (wid != WID_AC_INPUT) {
			Window::InsertTextString(wid, str, marked, caret, insert_location, replacement_end);
			return;
		}

		/* The console key belongs to the global hotkey, not to the message. A composition is left
		 * alone, so an IME preedit is never dropped by it. */
		if (!marked && str == "`") {
			HandleKeypress(WKC_BACKQUOTE, U'`');
			return;
		}

		/* The next update of a composition replaces the text of the previous one. */
		if (_agent_chat.composed_length != 0) {
			const size_t begin = _agent_chat.caret >= _agent_chat.composed_length ? _agent_chat.caret - _agent_chat.composed_length : 0;
			_agent_chat.input.erase(begin, _agent_chat.caret - begin);
			_agent_chat.caret = begin;
			_agent_chat.composed_length = 0;
		}

		/* Only printable text: a backend may send control characters as text as well (backspace and
		 * delete are sent this way by some backends), and a newline is inserted by the key handler,
		 * not by typing. The bytes of a UTF-8 sequence are all >= 0x80 and are kept as they are. */
		std::string text;
		for (const char c : str) {
			const unsigned char byte = static_cast<unsigned char>(c);
			if (IsPrintable(byte) && byte != 0x7F) text.push_back(c);
		}

		if (!text.empty()) {
			this->InsertInput(text);
			if (marked) _agent_chat.composed_length = text.size();
		}
		this->SetWidgetDirty(WID_AC_INPUT);
	}

	/**
	 * The message box is not backed by a Textbuf, so there is no focused text buffer to report.
	 * @return Always \c nullptr.
	 */
	const struct Textbuf *GetFocusedTextbuf() const override { return nullptr; }

	/**
	 * Get the position of the caret, so a backend can put an IME composition window next to it.
	 * @return The caret position, relative to the window.
	 */
	Point GetCaretPosition() const override
	{
		if (this->GetWidget<NWidgetBase>(WID_AC_INPUT) == nullptr) return {0, 0};
		return this->GetInputCaretPoint(this->GetInnerRect(WID_AC_INPUT));
	}

	/**
	 * Get the bounding rectangle of a range of the text in the message box.
	 *
	 * The base implementation dereferences the QueryString of the focused edit box, and the
	 * message box has none (it keeps its own buffer), so the range is measured here instead.
	 * @param from First byte of the range.
	 * @param to Last byte of the range (exclusive).
	 * @return The rectangle of the range, empty when it is not on screen.
	 */
	Rect GetTextBoundingRect(size_t from, size_t to) const override
	{
		const Rect area = this->GetInnerRect(WID_AC_INPUT);
		const InputLayout layout = this->LayoutInput(area);
		const int line_height = GetCharacterHeight(FontSize::Normal);

		size_t line_begin = 0;
		for (size_t i = 0; i < layout.lines.size(); i++) {
			const std::string &line = layout.lines[i];
			if (from <= line_begin + line.size()) {
				if (i < layout.first_line || i - layout.first_line >= static_cast<size_t>(layout.visible_lines)) return {0, 0, 0, 0};

				const size_t begin = std::min(from - line_begin, line.size());
				const size_t finish = std::clamp(to > line_begin ? to - line_begin : 0, begin, line.size());
				const int y = area.top + static_cast<int>(i - layout.first_line) * line_height;
				const int left = area.left + static_cast<int>(GetStringBoundingBox(std::string_view(line).substr(0, begin)).width);
				const int right = area.left + static_cast<int>(GetStringBoundingBox(std::string_view(line).substr(0, finish)).width);
				return {left, y, right, y + line_height - 1};
			}
			line_begin += line.size() + 1;
		}
		return {0, 0, 0, 0};
	}

	/**
	 * Get the character of the message box at a position.
	 *
	 * The base implementation dereferences the QueryString of the focused edit box, and the
	 * message box has none (it keeps its own buffer), so the position is mapped here instead.
	 * @param pt The position, relative to the window.
	 * @return The byte offset of the nearest character boundary, or -1 when the position is outside.
	 */
	ptrdiff_t GetTextCharacterAtPosition(const Point &pt) const override
	{
		const Rect area = this->GetInnerRect(WID_AC_INPUT);
		if (!area.Contains(pt)) return -1;

		const InputLayout layout = this->LayoutInput(area);
		const size_t index = layout.first_line + static_cast<size_t>(std::max(0, (pt.y - area.top) / GetCharacterHeight(FontSize::Normal)));
		if (index >= layout.lines.size()) return -1;

		size_t line_begin = 0;
		for (size_t i = 0; i < index; i++) line_begin += layout.lines[i].size() + 1;

		const std::string &line = layout.lines[index];
		size_t best = 0;
		int best_distance = std::abs(pt.x - area.left);
		for (size_t pos = 0; pos < line.size(); pos++) {
			/* Only the last byte of a character is a boundary. */
			if (pos + 1 < line.size() && IsUtf8Part(line[pos + 1])) continue;

			const int width = area.left + static_cast<int>(GetStringBoundingBox(std::string_view(line).substr(0, pos + 1)).width);
			const int distance = std::abs(width - pt.x);
			if (distance < best_distance) {
				best_distance = distance;
				best = pos + 1;
			}
		}
		return static_cast<ptrdiff_t>(line_begin + best);
	}

	/**
	 * Forget the text of an IME composition that is no longer being composed, so the next text
	 * input update does not try to replace it.
	 */
	void ResetComposition()
	{
		_agent_chat.composed_length = 0;
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
		this->ResetComposition();
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
		this->ResetComposition();
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
		this->ResetComposition();
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
		this->ResetComposition();
		this->SetWidgetDirty(WID_AC_INPUT);
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

	void Close([[maybe_unused]] int data = 0) override
	{
		/* The settings picker attaches to the message of this window, so it is scoped to it and
		 * does not outlive it. */
		CloseWindowByClass(WindowClass::AgentChatSettings);
		this->Window::Close();
	}
};

/* --- Settings picker --- */

/**
 * One setting the settings picker offers.
 *
 * The key is the identity of the reference that is attached: the current value is only shown, so
 * the player can tell two similar keys apart, and it is never part of the reference.
 */
struct AgentSettingPickerRow {
	std::string name;       ///< Exact engine key of the setting.
	std::string scope;      ///< Scope of the setting, "game", "client" or "company".
	std::string value_text; ///< Current value, as the engine formats it, for the selected owner.
};

/**
 * One company the owner selector offered, with the lifetime the player was shown.
 *
 * The dropdown is a snapshot: a company that is removed or recreated between opening the dropdown
 * and choosing a row must not be silently replaced by whichever company holds the id then, so the
 * lifetime that was shown is kept and checked against the company that exists when it is chosen.
 */
struct AgentSettingPickerOwner {
	CompanyID company;  ///< The company the dropdown offered.
	int64_t generation; ///< Lifetime generation of that company when the dropdown was opened.
};

/**
 * Name of the scope of a setting, as the protocol names it.
 * @param type The setting type.
 * @return The scope name.
 */
static std::string_view SettingScopeOf(SettingType type)
{
	switch (type) {
		case ST_GAME: return "game";
		case ST_CLIENT: return "client";
		case ST_COMPANY: return "company";
		default: return "unknown";
	}
}

/**
 * The object a setting of the picker is read from.
 *
 * A per-company setting is read from the selected owner and from nothing else, exactly like the
 * bridge resolves a company scoped reference: the picker never shows the client's company defaults
 * or the value of whichever company the actor happens to be. Game and client settings keep the
 * object their scope names.
 * @param sd The setting.
 * @param owner The company the picker shows company settings for; it must be valid for them.
 * @return The object the setting lives in.
 */
static const void *SettingPickerObject(const SettingDesc &sd, CompanyID owner)
{
	switch (sd.GetType()) {
		case ST_GAME: return &GetGameSettings();
		case ST_CLIENT: return &_settings_client;
		case ST_COMPANY: return &Company::Get(owner)->settings;
		default: NOT_REACHED();
	}
}

/**
 * The company the picker shows company settings for when it opens.
 *
 * The owner of the chat is the local company. It is only used when that company really exists, so
 * an empty pool never turns into a guessed company 0; otherwise the first company of the game is
 * used, and a game without companies leaves the picker without an owner.
 * @return The company, or CompanyID::Invalid() when there is none.
 */
static CompanyID DefaultSettingPickerOwner()
{
	if (Company::IsValidID(_local_company)) return _local_company;
	for (const Company *company : Company::Iterate()) return company->index;
	return CompanyID::Invalid();
}

/**
 * Every setting the picker offers, sorted by key.
 *
 * The list is built from the engine's own descriptors, so the picker offers exactly the settings
 * this build has: a setting that is not valid in this savegame version is left out, and a setting
 * added to the tables later needs no change here. The private and secret tables are left out on
 * purpose - a secret setting's value is a credential, and the picker shows values. The key is also
 * what the filter matches, so the list stays searchable without a translated title.
 * @param owner The company the values of company settings are read from.
 * @return The rows of the picker, sorted by key.
 */
static std::vector<AgentSettingPickerRow> BuildSettingPickerRows(CompanyID owner)
{
	std::vector<AgentSettingPickerRow> rows;
	auto add = [&rows, owner](const SettingDesc &sd) {
		if (!SlIsObjectCurrentlyValid(sd.save.version_from, sd.save.version_to)) return;

		const SettingType type = sd.GetType();
		AgentSettingPickerRow row;
		row.name = sd.GetName();
		row.scope = std::string(SettingScopeOf(type));
		/* A company setting has no value without an owner, and the picker never shows another
		 * company's value or the client defaults instead. */
		if (type == ST_COMPANY && !Company::IsValidID(owner)) {
			rows.push_back(std::move(row));
			return;
		}
		row.value_text = sd.FormatValue(SettingPickerObject(sd, owner));
		rows.push_back(std::move(row));
	};

	for (const SettingDesc *sd : GetFilteredSettingCollection([](const SettingDesc &) { return true; })) add(*sd);
	for (const SettingVariant &desc : _company_settings) add(*GetSettingDesc(desc));

	std::sort(rows.begin(), rows.end(), [](const AgentSettingPickerRow &a, const AgentSettingPickerRow &b) { return a.name < b.name; });
	return rows;
}

/**
 * Check whether a setting row matches the filter text.
 * @param row The row.
 * @param filter The search text; empty matches every row.
 * @return \c true when the exact key contains the text.
 */
static bool SettingRowMatches(const AgentSettingPickerRow &row, std::string_view filter)
{
	return filter.empty() || StrContainsIgnoreCase(row.name, filter);
}

/** Height of one row of the settings picker, in pixels. */
static int GetSettingPickerRowHeight()
{
	return GetCharacterHeight(FontSize::Normal) + WidgetDimensions::scaled.vsep_normal;
}

/** The native settings picker of the agent chat window. */
struct AgentChatSettingsWindow : Window {
	QueryString search_editbox;              ///< Filter editbox; a QueryString, so typing and the on-screen keyboard work as in every other list filter.
	Scrollbar *vscroll = nullptr;            ///< Scrollbar of the setting list.
	std::string world_id;                    ///< World the picker was opened in.
	std::string epoch;                       ///< Load epoch the picker was opened in.
	CompanyID owner = CompanyID::Invalid();  ///< Company whose values the company settings show.
	int64_t owner_generation = -1;           ///< Lifetime of #owner, pinned when the picker opens or an owner is chosen.
	std::vector<AgentSettingPickerOwner> offered_owners; ///< Companies the open owner dropdown offered, with the lifetimes shown.
	std::vector<AgentSettingPickerRow> rows; ///< Every setting the picker offers, sorted by key.
	std::vector<size_t> visible;             ///< Indices of #rows that match the filter.
	size_t selected = 0;                     ///< Index into #visible of the highlighted row.

	AgentChatSettingsWindow(WindowDesc &desc) : Window(desc), search_editbox(64)
	{
		this->world_id = _agent_chat.world_id;
		this->epoch = _agent_chat.epoch;
		this->owner = DefaultSettingPickerOwner();
		this->PinOwner();
		this->rows = BuildSettingPickerRows(this->owner);

		this->CreateNestedTree();
		this->vscroll = this->GetScrollbar(WID_ASP_SCROLL);
		this->FinishInitNested(0);

		this->querystrings[WID_ASP_SEARCH] = &this->search_editbox;
		this->search_editbox.text.Assign("");
		/* The filter can be cleared with the usual editbox cancel, and Enter attaches the
		 * highlighted setting, so the picker is usable without the mouse. */
		this->search_editbox.cancel_button = QueryString::ACTION_CLEAR;
		this->search_editbox.ok_button = WID_ASP_SELECT;
		this->SetFocusedWidget(WID_ASP_SEARCH);

		this->Refilter();
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

	/** Pin the lifetime of the owner the picker opens with, so a recreated company is not mistaken for it. */
	void PinOwner()
	{
		const Company *company = Company::GetIfValid(this->owner);
		this->owner_generation = company != nullptr ? GetPoolGeneration(*company) : -1;
	}

	/**
	 * Show the companies of the game and select the owner the company settings are read from.
	 *
	 * The list is the real company pool, so the owner is always a company that exists; it is never
	 * an id that was guessed from the current actor.
	 */
	void ShowOwnerDropdown()
	{
		DropDownList list;
		this->offered_owners.clear();
		for (const Company *company : Company::Iterate()) {
			/* The lifetime the row is offered with is the one the player sees, and it is the one a
			 * choice of that row has to resolve to. */
			this->offered_owners.push_back({company->index, GetPoolGeneration(*company)});
			list.push_back(MakeDropDownListStringItem(SettingOwnerLabel(company->index), company->index.base()));
		}
		if (list.empty()) return;
		/* The dropdown marks the row whose result is `selected`, and the result of a row is the
		 * company id - not its position in the list, which company ids are not. */
		ShowDropDownList(this, std::move(list), this->owner == CompanyID::Invalid() ? -1 : this->owner.base(), WID_ASP_OWNER);
	}

	/**
	 * Switch the owner of the company settings and show their values for that company.
	 * @param company The new owner.
	 * @param generation The lifetime of that company the player chose; it is pinned as it was shown.
	 */
	void SelectOwner(CompanyID company, int64_t generation)
	{
		this->owner = company;
		this->owner_generation = generation;
		this->rows = BuildSettingPickerRows(this->owner);
		this->Refilter();
		this->SetDirty();
	}

	/** Recompute the visible rows from the filter text and show the list from the top again. */
	void Refilter()
	{
		const std::string filter(this->search_editbox.text.GetText());
		this->visible.clear();
		for (size_t i = 0; i < this->rows.size(); i++) {
			if (SettingRowMatches(this->rows[i], filter)) this->visible.push_back(i);
		}
		this->selected = 0;
		this->vscroll->SetPosition(0);
		this->UpdateScrollbar();
		this->SetWidgetDirty(WID_ASP_LIST);
	}

	/** Update the range and the position of the scrollbar to the filtered list. */
	void UpdateScrollbar()
	{
		const int capacity = std::max(1, this->GetInnerRect(WID_ASP_LIST).Height() / GetSettingPickerRowHeight());
		this->vscroll->SetCapacity(capacity);
		this->vscroll->SetCount(std::max<int>(capacity, this->visible.size()));
		this->vscroll->SetStepSize(1);
		this->ScrollSelectionIntoView();
	}

	/** Scroll the list so the highlighted row is on screen. */
	void ScrollSelectionIntoView()
	{
		if (this->visible.empty()) {
			this->vscroll->SetPosition(0);
			return;
		}

		const int capacity = std::max(1, this->vscroll->GetCapacity());
		const int index = static_cast<int>(this->selected);
		int position = this->vscroll->GetPosition();
		if (index < position) position = index;
		if (index >= position + capacity) position = index - capacity + 1;
		this->vscroll->SetPosition(std::max(0, position));
	}

	/**
	 * Highlight one row of the filtered list.
	 * @param index The index into #visible; it is clamped to the list.
	 */
	void SelectIndex(int index)
	{
		if (this->visible.empty()) return;
		this->selected = static_cast<size_t>(std::clamp(index, 0, static_cast<int>(this->visible.size()) - 1));
		this->ScrollSelectionIntoView();
		this->SetWidgetDirty(WID_ASP_LIST);
	}

	/**
	 * Attach the highlighted setting to the message and close the picker.
	 *
	 * The picker only reads: it never changes the setting, and the attachment is a reference the
	 * backend resolves itself. A picker that was opened in another world or epoch attaches nothing,
	 * so a load that happened while it was open cannot put a reference of the world that is gone
	 * into the tray of the new world. A company setting carries the owner it was shown for and the
	 * lifetime of that company, so a company that was removed or recreated in the meantime is not
	 * silently replaced by whichever company holds the id now.
	 */
	void AttachSelected()
	{
		if (this->visible.empty()) return;
		const AgentSettingPickerRow &row = this->rows[this->visible[this->selected]];

		if (this->world_id != _agent_chat.world_id || this->epoch != _agent_chat.epoch) {
			_agent_chat.error = GetString(STR_AGENT_CHAT_SETTING_PICKER_STALE);
			this->Close();
			InvalidateAgentChatWindows();
			return;
		}

		AgentChatAttachment attachment;
		attachment.type = "setting";
		attachment.setting_name = row.name;
		attachment.scope = row.scope;
		/* The key is the label: it is the identity the backend resolves. The current value belongs
		 * to the setting, not to the reference, so it is not frozen into the label. */
		attachment.name = row.name;
		attachment.world_id = _agent_chat.world_id;
		attachment.epoch = _agent_chat.epoch;

		if (row.scope == "company") {
			const Company *company = Company::GetIfValid(this->owner);
			/* The owner the player saw has to be the owner that is attached: a company that was
			 * removed or recreated while the picker was open must not silently hand the reference
			 * to whichever company holds the id now. */
			if (company == nullptr || this->owner_generation < 0 || GetPoolGeneration(*company) != this->owner_generation) {
				_agent_chat.error = GetString(STR_AGENT_CHAT_SETTING_PICKER_OWNER_GONE);
				this->Close();
				InvalidateAgentChatWindows();
				return;
			}
			attachment.company_id = this->owner.base();
			attachment.company_generation = this->owner_generation;
		}

		_agent_chat.tray.push_back(std::move(attachment));

		this->Close();
		InvalidateAgentChatWindows();
	}

	void OnPaint() override
	{
		/* Without a company there is no owner to choose, and no company setting to show. */
		this->SetWidgetDisabledState(WID_ASP_OWNER, Company::GetNumItems() == 0);
		this->DrawWidgets();
	}

	std::string GetWidgetString(WidgetID widget, StringID stringid) const override
	{
		if (widget != WID_ASP_OWNER) return this->Window::GetWidgetString(widget, stringid);
		/* The owner selector names the company the company settings are shown for. */
		return GetString(stringid, this->owner == CompanyID::Invalid() ? GetString(STR_AGENT_CHAT_SETTING_PICKER_OWNER_NONE) : SettingOwnerLabel(this->owner));
	}

	void DrawWidget(const Rect &r, WidgetID widget) const override
	{
		if (widget != WID_ASP_LIST) return;

		const Rect area = r.Shrink(WidgetDimensions::scaled.framerect);
		if (this->visible.empty()) {
			DrawStringMultiLine(area, GetString(STR_AGENT_CHAT_SETTING_PICKER_EMPTY), TextColour::Grey);
			return;
		}

		const int row_height = GetSettingPickerRowHeight();
		const int first = this->vscroll->GetPosition();
		const int last = std::min<int>(this->visible.size(), first + this->vscroll->GetCapacity());
		const int name_right = area.left + area.Width() * 3 / 5;
		int y = area.top;
		for (int i = first; i < last; i++) {
			const AgentSettingPickerRow &row = this->rows[this->visible[i]];
			const Rect line = Rect{area.left, y, area.right, y + row_height - 1};
			const bool selected = static_cast<size_t>(i) == this->selected;
			if (selected) GfxFillRect(line, PC_DARK_BLUE);

			const TextColour colour = selected ? TextColour::White : TextColour::Black;
			/* The key is what the attachment will name; the value is what the setting holds now. */
			DrawString(line.left, name_right, line.top, row.name, colour);
			DrawString(name_right, line.right, line.top, row.value_text, colour, AlignmentH::End);
			y += row_height;
		}
	}

	void OnClick(Point pt, WidgetID widget, [[maybe_unused]] int click_count) override
	{
		switch (widget) {
			case WID_ASP_OWNER:
				this->ShowOwnerDropdown();
				break;

			case WID_ASP_SELECT:
				this->AttachSelected();
				break;

			case WID_ASP_CLOSE:
				this->Close();
				break;

			case WID_ASP_LIST:
				this->ClickList(pt);
				break;

			default:
				break;
		}
	}

	void OnDropdownSelect(WidgetID widget, int index, [[maybe_unused]] int click_result) override
	{
		if (widget != WID_ASP_OWNER) return;

		/* The result of a row is the company id it offered. */
		const CompanyID chosen = static_cast<CompanyID>(index);
		int64_t offered_generation = -1;
		bool offered = false;
		for (const AgentSettingPickerOwner &entry : this->offered_owners) {
			if (entry.company != chosen) continue;
			offered_generation = entry.generation;
			offered = true;
			break;
		}
		if (!offered) return;

		/* The choice names the company and the lifetime the player was shown. A company that was
		 * removed or recreated while the dropdown was open is not silently replaced by whichever
		 * company holds the id now, and a refused choice leaves the owner that was shown before
		 * exactly as stale as it was: it is never re-pinned to a lifetime the player did not see. */
		const Company *company = Company::GetIfValid(chosen);
		if (company == nullptr || offered_generation < 0 || GetPoolGeneration(*company) != offered_generation) {
			_agent_chat.error = GetString(STR_AGENT_CHAT_SETTING_PICKER_OWNER_GONE);
			InvalidateAgentChatWindows();
			return;
		}

		this->SelectOwner(chosen, offered_generation);
	}

	/**
	 * Handle a click in the setting list: highlight the row and attach it.
	 * @param pt Click position.
	 */
	void ClickList(Point pt)
	{
		const Rect area = this->GetInnerRect(WID_ASP_LIST);
		if (!area.Contains(pt)) return;

		const int index = (pt.y - area.top) / GetSettingPickerRowHeight() + this->vscroll->GetPosition();
		if (index < 0 || static_cast<size_t>(index) >= this->visible.size()) return;
		this->SelectIndex(index);
		this->AttachSelected();
	}

	void OnEditboxChanged(WidgetID widget) override
	{
		if (widget != WID_ASP_SEARCH) return;
		this->Refilter();
	}

	void OnScrollbarScroll(WidgetID widget) override
	{
		if (widget == WID_ASP_SCROLL) this->SetWidgetDirty(WID_ASP_LIST);
	}

	void OnResize() override
	{
		this->UpdateScrollbar();
	}

	EventState OnKeyPress([[maybe_unused]] char32_t key, uint16_t keycode) override
	{
		if (keycode == WKC_ESC) {
			this->Close();
			return EventState::Handled;
		}

		/* The editbox keeps the text keys; the list keys are only reached when it did not take
		 * them, so typing in the filter never moves the highlight. */
		const int capacity = std::max(1, this->vscroll->GetCapacity());
		const int current = static_cast<int>(this->selected);
		switch (keycode) {
			case WKC_UP: this->SelectIndex(current - 1); return EventState::Handled;
			case WKC_DOWN: this->SelectIndex(current + 1); return EventState::Handled;
			case WKC_PAGEUP: this->SelectIndex(current - capacity); return EventState::Handled;
			case WKC_PAGEDOWN: this->SelectIndex(current + capacity); return EventState::Handled;
			case WKC_HOME: this->SelectIndex(0); return EventState::Handled;
			case WKC_END: this->SelectIndex(INT32_MAX); return EventState::Handled;
			default: return EventState::NotHandled;
		}
	}
};

/** Widget parts of the settings picker of the agent chat window. */
static constexpr std::initializer_list<NWidgetPart> _nested_agent_chat_settings_widgets = {
	NWidget(NWID_HORIZONTAL),
		NWidget(WWT_CLOSEBOX, Colours::Grey),
		NWidget(WWT_CAPTION, Colours::Grey, WID_ASP_CAPTION), SetResize(1, 0), SetStringTip(STR_AGENT_CHAT_SETTING_PICKER_CAPTION),
		NWidget(WWT_RESIZEBOX, Colours::Grey),
	EndContainer(),
	NWidget(NWID_VERTICAL),
		NWidget(WWT_EDITBOX, Colours::Grey, WID_ASP_SEARCH), SetMinimalSize(0, 12), SetFill(1, 0), SetResize(1, 0), SetStringTip(STR_AGENT_CHAT_SETTING_PICKER_SEARCH_OSKTITLE, STR_AGENT_CHAT_SETTING_PICKER_SEARCH_TOOLTIP),
		NWidget(NWID_HORIZONTAL),
			NWidget(WWT_DROPDOWN, Colours::Grey, WID_ASP_OWNER), SetMinimalSize(160, 12), SetFill(1, 0), SetResize(1, 0), SetStringTip(STR_AGENT_CHAT_SETTING_PICKER_OWNER, STR_AGENT_CHAT_SETTING_PICKER_OWNER_TOOLTIP),
		EndContainer(),
		NWidget(NWID_HORIZONTAL),
			NWidget(WWT_PANEL, Colours::Grey, WID_ASP_LIST), SetMinimalSize(360, 220), SetFill(1, 1), SetResize(1, 1), SetScrollbar(WID_ASP_SCROLL),
			EndContainer(),
			NWidget(NWID_VERTICAL),
				NWidget(NWID_VSCROLLBAR, Colours::Grey, WID_ASP_SCROLL),
			EndContainer(),
		EndContainer(),
		NWidget(NWID_HORIZONTAL),
			NWidget(WWT_PUSHTXTBTN, Colours::Grey, WID_ASP_SELECT), SetFill(1, 0), SetResize(1, 0), SetStringTip(STR_AGENT_CHAT_SETTING_PICKER_SELECT, STR_AGENT_CHAT_SETTING_PICKER_SELECT_TOOLTIP),
			NWidget(WWT_PUSHTXTBTN, Colours::Grey, WID_ASP_CLOSE), SetFill(1, 0), SetResize(1, 0), SetStringTip(STR_AGENT_CHAT_SETTING_PICKER_CLOSE, STR_AGENT_CHAT_SETTING_PICKER_CLOSE_TOOLTIP),
		EndContainer(),
	EndContainer(),
};

/**
 * Window description of the settings picker.
 *
 * The picker is a transient child of the chat window: it has no size or sticky box of its own, so it
 * carries no ini key and nothing about it is persisted between runs.
 */
static WindowDesc _agent_chat_settings_window_desc(
	WindowPosition::Automatic, {}, 440, 340,
	WindowClass::AgentChatSettings, WindowClass::AgentChat,
	{},
	_nested_agent_chat_settings_widgets
);

/** Open the settings picker, or bring it to the front when it is already open. */
static void ShowAgentChatSettingPicker()
{
	Window *w = BringWindowToFrontById(WindowClass::AgentChatSettings, 0);
	if (w != nullptr) {
		w->SetDirty();
		return;
	}
	new AgentChatSettingsWindow(_agent_chat_settings_window_desc);
}

/** Undo the shade the chat window took to uncover the map while a click was awaited. */
static void RestoreAgentChatPickingShade()
{
	AgentChatWindow *w = dynamic_cast<AgentChatWindow *>(FindWindowById(WindowClass::AgentChat, 0));
	if (w != nullptr) w->RestoreAfterPicking();
}

/** Widget parts of the agent chat window. */
static constexpr std::initializer_list<NWidgetPart> _nested_agent_chat_widgets = {
	NWidget(NWID_HORIZONTAL),
		NWidget(WWT_CLOSEBOX, Colours::Grey),
		/* The caption takes the extra width of the title bar; without a resize step of its own the
		 * title bar (and with it the whole window) could not grow horizontally. */
		NWidget(WWT_CAPTION, Colours::Grey, WID_AC_CAPTION), SetResize(1, 0), SetStringTip(STR_AGENT_CHAT_CAPTION),
		NWidget(WWT_SHADEBOX, Colours::Grey),
		NWidget(WWT_STICKYBOX, Colours::Grey),
		NWidget(WWT_RESIZEBOX, Colours::Grey),
	EndContainer(),
	NWidget(NWID_HORIZONTAL),
		NWidget(NWID_VERTICAL),
			/* Chat list and its scrollbar are one row, so the scrollbar stays a narrow vertical bar
			 * beside the list instead of being stretched over the width of the sidebar. */
			NWidget(NWID_HORIZONTAL),
				NWidget(WWT_PANEL, Colours::Grey, WID_AC_CHAT_LIST), SetMinimalSize(120, 120), SetFill(0, 1), SetResize(0, 1), SetScrollbar(WID_AC_CHAT_SCROLL), SetToolTip(STR_AGENT_CHAT_CHAT_LIST_TOOLTIP),
				EndContainer(),
				NWidget(NWID_VERTICAL),
					NWidget(NWID_VSCROLLBAR, Colours::Grey, WID_AC_CHAT_SCROLL),
				EndContainer(),
			EndContainer(),
			NWidget(WWT_PUSHTXTBTN, Colours::Grey, WID_AC_NEW_CHAT), SetFill(1, 0), SetStringTip(STR_AGENT_CHAT_NEW_CHAT, STR_AGENT_CHAT_NEW_CHAT_TOOLTIP),
		EndContainer(),
		NWidget(NWID_VERTICAL),
			NWidget(NWID_HORIZONTAL),
				/* The transcript carries the minimal size of the window and absorbs all extra space. */
				NWidget(WWT_PANEL, Colours::Grey, WID_AC_TRANSCRIPT), SetMinimalSize(480, 180), SetFill(1, 1), SetResize(1, 1), SetScrollbar(WID_AC_TRANSCRIPT_SCROLL), SetToolTip(STR_AGENT_CHAT_TRANSCRIPT_TOOLTIP),
				EndContainer(),
				NWidget(NWID_VERTICAL),
					NWidget(NWID_VSCROLLBAR, Colours::Grey, WID_AC_TRANSCRIPT_SCROLL),
				EndContainer(),
			EndContainer(),
			NWidget(WWT_PANEL, Colours::Grey, WID_AC_TRAY), SetMinimalSize(0, 24), SetFill(1, 0), SetResize(1, 0),
			EndContainer(),
			NWidget(NWID_HORIZONTAL),
				NWidget(WWT_PUSHTXTBTN, Colours::Grey, WID_AC_ADD_POINT), SetFill(1, 0), SetResize(1, 0), SetStringTip(STR_AGENT_CHAT_ADD_POINT, STR_AGENT_CHAT_ADD_POINT_TOOLTIP),
				NWidget(WWT_PUSHTXTBTN, Colours::Grey, WID_AC_ADD_OBJECT), SetFill(1, 0), SetResize(1, 0), SetStringTip(STR_AGENT_CHAT_ADD_OBJECT, STR_AGENT_CHAT_ADD_OBJECT_TOOLTIP),
				NWidget(WWT_PUSHTXTBTN, Colours::Grey, WID_AC_ADD_COMPANY), SetFill(1, 0), SetResize(1, 0), SetStringTip(STR_AGENT_CHAT_ADD_COMPANY, STR_AGENT_CHAT_ADD_COMPANY_TOOLTIP),
				NWidget(WWT_PUSHTXTBTN, Colours::Grey, WID_AC_ADD_SETTING), SetFill(1, 0), SetResize(1, 0), SetStringTip(STR_AGENT_CHAT_ADD_SETTING, STR_AGENT_CHAT_ADD_SETTING_TOOLTIP),
			EndContainer(),
			/* An edit box so the window can take the global edit box focus: the backends deliver
			 * typed text (and only typed text) through HandleTextInput() to a focused edit box.
			 * It is a leaf: the vertical container it lives in is closed further down. */
			NWidget(WWT_EDITBOX, Colours::Grey, WID_AC_INPUT), SetMinimalSize(0, 48), SetFill(1, 0), SetResize(1, 0), SetToolTip(STR_AGENT_CHAT_INPUT_TOOLTIP),
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
	WindowPosition::Automatic, "agent_chat", 760, 460,
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

		const std::string title = SanitiseBackendText(busy->title.empty() ? busy->id : busy->title);
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
	/* While a task is stopping or settling the authoritative state is read four times as often, so
	 * a stop shows up as soon as its barrier landed instead of on the next one-second poll. */
	const uint interval = HasFinishingChat() ? AGENT_CHAT_FINISHING_POLL_INTERVAL_MS : AGENT_CHAT_POLL_INTERVAL_MS;
	_agent_chat.next_poll = now + std::chrono::milliseconds(interval);

	if (!_agent_chat.chats_loaded) {
		if (AgentChatCanQueueRead()) AgentChatLoadChats();
		AgentChatUpdateStatusWindow();
		return;
	}

	/* A task whose turn ended, or whose stop is still pending, may still be settling; only the
	 * chat DTO knows when its writes and its barrier settled, so Stop stays available until then.
	 * That authoritative read comes first: on the fast interval the transcript and the event polls
	 * of a busy chat must never take the last slot and starve the state Stop depends on. */
	std::vector<std::string> finishing_ids;
	for (const AgentChatEntry &chat : _agent_chat.chats) {
		if (chat.task_active && (chat.task_finishing || chat.stop_pending)) finishing_ids.push_back(chat.id);
	}
	for (const std::string &id : finishing_ids) {
		if (!AgentChatCanQueueRead()) break;
		AgentChatRefreshChat(id);
	}

	const AgentChatEntry *active = GetActiveChat();
	const std::string active_id = active != nullptr ? active->id : std::string();
	if (active != nullptr) {
		if (AgentChatCanQueueRead()) AgentChatLoadMessages(active_id);
		if (AgentChatCanQueueRead()) AgentChatPollEvents(active_id);
	}

	/* The transcript shows screenshots of its messages; fetching them is bounded by the cache. */
	AgentChatEnsurePreviews();

	/* Keep the status of tasks in other chats up to date as well. The budget is checked for every
	 * single read, so a slow backend cannot turn the fast interval into a growing queue. */
	std::vector<std::string> busy_ids;
	for (const AgentChatEntry &chat : _agent_chat.chats) {
		if (chat.task_active && chat.id != active_id) busy_ids.push_back(chat.id);
	}
	for (const std::string &id : busy_ids) {
		if (!AgentChatCanQueueRead()) break;
		AgentChatPollEvents(id);
	}

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
	/* The real console mode, so a read-only check can see whether the console key worked. */
	out["console_open"] = _iconsole_mode != IConsoleMode::Closed;
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
		entry["stop_pending"] = chat.stop_pending;
		entry["status"] = GetChatStatusText(chat);
		chats.push_back(std::move(entry));
	}
	out["chats"] = std::move(chats);

	Json tray = Json::array();
	for (const AgentChatAttachment &attachment : _agent_chat.tray) {
		tray.push_back(ReferenceToUiJson(attachment));
	}
	out["tray"] = std::move(tray);

	Json candidates = Json::array();
	for (const Json &candidate : _agent_chat.candidates) {
		/* A candidate already is a canonical reference: the round trip through the attachment keeps
		 * the fields this UI does not model (a fingerprint, a `what`, a lifetime) and drops nothing. */
		AgentChatAttachment preview = ParseAttachment(candidate);
		/* The readable label needs a kind; a candidate without one is still an object reference. */
		if (preview.type.empty()) preview.type = "object";
		candidates.push_back(ReferenceToUiJson(preview));
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
			Json attachments = Json::array();
			for (const AgentChatAttachment &attachment : message.attachments) {
				attachments.push_back(ReferenceToUiJson(attachment));
			}
			entry["attachments"] = std::move(attachments);
			messages.push_back(std::move(entry));
		}
	}
	out["messages"] = std::move(messages);

	/* The selection is reported read-only: its text, the two ends in transcript order and, when
	 * the window is open, the rectangles that are highlighted. Nothing here changes the selection. */
	Json selection = Json::object();
	selection["chat"] = _agent_chat.selection.ChatId();
	selection["world_id"] = _agent_chat.selection.world;
	selection["epoch"] = _agent_chat.selection.epoch;
	selection["empty"] = _agent_chat.selection.Empty();
	selection["anchor"] = Json{{"message", _agent_chat.selection.anchor.message}, {"offset", _agent_chat.selection.anchor.offset}};
	selection["cursor"] = Json{{"message", _agent_chat.selection.cursor.message}, {"offset", _agent_chat.selection.cursor.offset}};
	const auto [selection_from, selection_to] = _agent_chat.selection.Ordered();
	selection["from"] = Json{{"message", selection_from.message}, {"offset", selection_from.offset}};
	selection["to"] = Json{{"message", selection_to.message}, {"offset", selection_to.offset}};
	selection["text"] = _agent_chat.selection.GetText(TranscriptTexts(active));
	out["selection"] = std::move(selection);

	/* Report the actual rectangles of the window and of the targets inside it. Nested widget
	 * rectangles and #Window::GetInnerRect are window-local, so they are offset by the window
	 * position; "coordinate_space" names the space of every rectangle in this object. */
	/* The same object world.info reports as timing.chat_ack, bound to the world and epoch it was
	 * measured in. */
	out["chat_ack"] = AgentChatAckTimingJson();

	out["coordinate_space"] = "screen";
	AgentChatWindow *w = dynamic_cast<AgentChatWindow *>(FindWindowById(WindowClass::AgentChat, 0));
	out["window_open"] = w != nullptr;

	/* The viewport is where a map click lands; a test picks a point inside it and outside the
	 * (shaded) chat window instead of guessing coordinates. */
	const Window *main = FindWindowById(WindowClass::MainWindow, 0);
	if (main != nullptr && main->viewport != nullptr) {
		out["viewport_rect"] = RectToJson(Rect{main->viewport->left, main->viewport->top,
				main->viewport->left + main->viewport->width - 1, main->viewport->top + main->viewport->height - 1});
	}

	if (w != nullptr) {
		out["window_rect"] = RectToJson(Rect{w->left, w->top, w->left + w->width - 1, w->top + w->height - 1});
		/* The actual shade state, not the intent: a window that cannot shade stays unshaded. */
		out["shaded"] = w->IsShaded();
		out["picking"] = Json::object({
			{"active", _agent_chat.picking_point || _agent_chat.picking_object},
			{"kind", _agent_chat.picking_point ? "point" : (_agent_chat.picking_object ? "object" : "")},
			{"shaded", w->picking_shaded},
			{"was_shaded", w->picking_was_shaded},
			{"restore_rect", RectToJson(w->picking_restore_rect)},
		});

		Json widgets = Json::object();
		const std::pair<const char *, WidgetID> widget_names[] = {
			/* The caption is a real widget with its own id, so a test can drag the window by it. */
			{"caption", WID_AC_CAPTION},
			{"chat_list", WID_AC_CHAT_LIST},
			{"chat_scroll", WID_AC_CHAT_SCROLL},
			{"new_chat", WID_AC_NEW_CHAT},
			{"transcript", WID_AC_TRANSCRIPT},
			{"transcript_scroll", WID_AC_TRANSCRIPT_SCROLL},
			{"tray", WID_AC_TRAY},
			{"add_point", WID_AC_ADD_POINT},
			{"add_object", WID_AC_ADD_OBJECT},
			{"add_company", WID_AC_ADD_COMPANY},
			{"add_setting", WID_AC_ADD_SETTING},
			{"input", WID_AC_INPUT},
			{"status", WID_AC_STATUS},
			{"send", WID_AC_SEND},
			{"stop", WID_AC_STOP},
		};
		for (const auto &[name, widget] : widget_names) {
			widgets[name] = RectToJson(ToScreenRect(*w, w->GetWidget<NWidgetBase>(widget)->GetCurrentRect()));
		}
		/* The close box has no widget id of its own, so look it up in the nested tree. */
		const NWidgetBase *closebox = w->nested_root->GetWidgetOfType(WWT_CLOSEBOX);
		if (closebox != nullptr) widgets["closebox"] = RectToJson(ToScreenRect(*w, closebox->GetCurrentRect()));
		out["widgets"] = std::move(widgets);

		/* The actual enabled state of the action widgets: a read-only check can see whether a
		 * click would be accepted instead of guessing it from the connection flag. */
		Json widget_states = Json::object();
		widget_states["new_chat"] = Json{{"disabled", w->IsWidgetDisabled(WID_AC_NEW_CHAT)}};
		widget_states["send"] = Json{{"disabled", w->IsWidgetDisabled(WID_AC_SEND)}};
		widget_states["stop"] = Json{{"disabled", w->IsWidgetDisabled(WID_AC_STOP)}};
		out["widget_states"] = std::move(widget_states);

		Json chat_rows = Json::array();
		const Rect list = w->GetInnerRect(WID_AC_CHAT_LIST);
		const int row_height = GetChatRowHeight();
		const int first = w->chat_scroll->GetPosition();
		const int last = std::min<int>(_agent_chat.chats.size(), first + w->chat_scroll->GetCapacity());
		for (int i = first; i < last; i++) {
			const int top = list.top + (i - first) * row_height;
			Json entry = Json::object();
			entry["id"] = _agent_chat.chats[i].id;
			entry["rect"] = RectToJson(ToScreenRect(*w, Rect{list.left, top, list.right, top + row_height - 1}));
			chat_rows.push_back(std::move(entry));
		}
		out["chat_rows"] = std::move(chat_rows);

		out["selection"]["focused"] = w->transcript_focused;

		Json message_rows = Json::array();
		if (active != nullptr) {
			const Rect area = w->GetInnerRect(WID_AC_TRANSCRIPT);
			const std::vector<Rect> layout = LayoutTranscript(active, area, _agent_chat.scroll);
			const std::vector<AgentChatTextLine> text_lines = LayoutTranscriptText(active, area, _agent_chat.scroll);
			for (size_t i = 0; i < layout.size(); i++) {
				Json entry = Json::object();
				entry["seq"] = active->messages[i].seq;
				entry["rect"] = RectToJson(ToScreenRect(*w, layout[i]));

				/* The rendered lines of the message text, so a real input test can drag from one
				 * of them to another instead of guessing a pixel. */
				Json lines = Json::array();
				for (const AgentChatTextLine &line : text_lines) {
					if (line.message != i) continue;
					Json line_entry = Json::object();
					line_entry["rect"] = RectToJson(ToScreenRect(*w, Rect{line.left, line.top, line.left + line.width - 1, line.top + line.leading - 1}));
					line_entry["text"] = active->messages[i].text.substr(line.begin, line.end - line.begin);
					lines.push_back(std::move(line_entry));
				}
				entry["lines"] = std::move(lines);
				message_rows.push_back(std::move(entry));
			}

			/* The highlight of the selection, exactly as it is painted, and its bounds. */
			Json rects = Json::array();
			Rect bounds = {0, 0, -1, -1};
			bool any = false;
			for (const Rect &highlight : LayoutSelection(_agent_chat.selection, text_lines, active)) {
				rects.push_back(RectToJson(ToScreenRect(*w, highlight)));
				bounds = any ? Rect{std::min(bounds.left, highlight.left), std::min(bounds.top, highlight.top),
						std::max(bounds.right, highlight.right), std::max(bounds.bottom, highlight.bottom)} : highlight;
				any = true;
			}
			out["selection"]["rects"] = std::move(rects);
			out["selection"]["bounds"] = any ? RectToJson(ToScreenRect(*w, bounds)) : Json();
		}
		out["message_rows"] = std::move(message_rows);

		/* The screenshot previews of the transcript: the box each one occupies and the identity of
		 * the pixels drawn in it. Read-only, and nothing about the host: no file path is reported. */
		Json image_rows = Json::array();
		if (active != nullptr) {
			const Rect area = w->GetInnerRect(WID_AC_TRANSCRIPT);
			const std::vector<Rect> layout = LayoutTranscript(active, area, _agent_chat.scroll);
			for (size_t i = 0; i < layout.size(); i++) {
				const AgentChatMessage &message = active->messages[i];
				const int top = GetMessageImagesTop(message, layout[i]);
				for (const AgentChatImageRow &row : LayoutMessageImages(message, layout[i], top)) {
					const AgentChatPreviewKey key = GetPreviewKey(*active, *row.attachment);
					const AgentChatPreviewPixels *pixels = _agent_chat.previews.Find(key);
					Json entry = Json::object();
					entry["rect"] = RectToJson(ToScreenRect(*w, row.rect));
					entry["artifact"] = row.attachment->artifact_id;
					entry["sourcehash"] = row.attachment->sha256;
					entry["pixelhash"] = pixels != nullptr ? pixels->pixel_sha256 : std::string();
					entry["loaded"] = pixels != nullptr;
					entry["width"] = row.rect.Width();
					entry["height"] = row.rect.Height();
					image_rows.push_back(std::move(entry));
				}
			}
		}
		out["image_rows"] = std::move(image_rows);

		Json tray_rows = Json::array();
		const Rect tray_area = w->GetInnerRect(WID_AC_TRAY);
		const std::vector<Rect> chips = LayoutTrayChips(tray_area);
		for (size_t i = 0; i < chips.size(); i++) {
			const int remove_width = GetCharacterHeight(FontSize::Normal);
			const AgentChatAttachment &attachment = _agent_chat.tray[i];
			Json entry = Json::object();
			entry["name"] = GetAttachmentDisplayLabel(attachment);
			/* The chip is a rectangle; the identity of the reference it shows is added, so a test can
			 * match a chip to the tray entry without guessing which chip belongs to which object. */
			entry["type"] = attachment.type;
			if (attachment.id >= 0) entry["id"] = attachment.id;
			if (attachment.generation >= 0) entry["generation"] = attachment.generation;
			if (attachment.tile != INVALID_TILE) entry["tile"] = Json{{"x", TileX(attachment.tile)}, {"y", TileY(attachment.tile)}};
			if (!attachment.setting_name.empty()) {
				/* A setting names its scope and, when it is owned, the company and its lifetime. */
				entry["setting_name"] = attachment.setting_name;
				entry["scope"] = attachment.scope;
				if (attachment.company_id >= 0) entry["company_id"] = attachment.company_id;
				if (attachment.company_generation >= 0) entry["company_generation"] = attachment.company_generation;
			}
			entry["stale"] = IsAttachmentStale(attachment);
			entry["rect"] = RectToJson(ToScreenRect(*w, chips[i]));
			entry["remove_rect"] = RectToJson(ToScreenRect(*w, Rect{chips[i].right - remove_width, chips[i].top, chips[i].right, chips[i].bottom}));
			tray_rows.push_back(std::move(entry));
		}
		out["tray_rows"] = std::move(tray_rows);

		Json candidate_rows = Json::array();
		const std::vector<Rect> rows = LayoutCandidates(w->GetInnerRect(WID_AC_TRANSCRIPT));
		for (size_t i = 0; i < rows.size(); i++) {
			Json entry = Json::object();
			entry["name"] = JsonString(_agent_chat.candidates[i], "name");
			entry["rect"] = RectToJson(ToScreenRect(*w, rows[i]));
			candidate_rows.push_back(std::move(entry));
		}
		out["candidate_rows"] = std::move(candidate_rows);
	}

	/* The settings picker: its real geometry, the filter text and the rows that are on screen, so a
	 * real input test can click a row instead of guessing a pixel. Nothing here changes the picker. */
	AgentChatSettingsWindow *picker = dynamic_cast<AgentChatSettingsWindow *>(FindWindowById(WindowClass::AgentChatSettings, 0));
	if (picker == nullptr) {
		out["settings_picker"] = Json{{"open", false}};
	} else {
		Json entry = Json::object();
		entry["open"] = true;
		entry["world_id"] = picker->world_id;
		entry["epoch"] = picker->epoch;
		entry["search"] = std::string(picker->search_editbox.text.GetText());
		entry["total"] = picker->rows.size();
		entry["matching"] = picker->visible.size();
		entry["selected"] = picker->selected;
		entry["scroll"] = Json{{"position", picker->vscroll->GetPosition()},
				{"capacity", picker->vscroll->GetCapacity()}, {"count", picker->vscroll->GetCount()}};
		/* The owner the company settings are shown for, and the companies the selector offers. The
		 * lifetime is the pinned one, so a recreated company is visible as a different owner. */
		const Company *owner = Company::GetIfValid(picker->owner);
		entry["owner"] = owner == nullptr ? Json() : Json{{"company_id", picker->owner.base()},
				{"company_generation", picker->owner_generation}, {"name", SettingOwnerLabel(picker->owner)}};
		Json companies = Json::array();
		for (const Company *company : Company::Iterate()) {
			companies.push_back({{"company_id", company->index.base()}, {"name", SettingOwnerLabel(company->index)},
					{"company_generation", GetPoolGeneration(*company)}, {"selected", company->index == picker->owner}});
		}
		entry["companies"] = std::move(companies);

		/* The owner dropdown, when it is open: the real native dropdown window of this picker and its
		 * owner widget, with the rows that are visible after filtering and scrolling and with the
		 * lifetime every row was offered with. A real input test can click a row from here instead of
		 * guessing a coordinate, and it can never pick a row that is not on screen. */
		const DropDownListGeometry owner_dropdown = GetOpenDropDownListGeometry(picker, WID_ASP_OWNER);
		Json dropdown_rows = Json::array();
		for (const DropDownRowGeometry &row : owner_dropdown.rows) {
			int64_t generation = -1;
			for (const AgentSettingPickerOwner &offered : picker->offered_owners) {
				if (offered.company.base() != row.result) continue;
				generation = offered.generation;
				break;
			}
			Json row_entry = {
				{"company_id", row.result},
				/* The lifetime the row was offered with, from the snapshot of the open dropdown and
				 * not from the current company pool: a company that was deleted and recreated while
				 * the dropdown is open keeps the lifetime the player was shown. */
				{"company_generation", generation >= 0 ? Json(generation) : Json()},
				{"rect", RectToJson(row.rect)},
				{"selected", row.selected},
			};
			if (row.disabled) row_entry["disabled"] = true;
			dropdown_rows.push_back(std::move(row_entry));
		}
		entry["owner_dropdown"] = Json{
			{"open", owner_dropdown.open},
			{"coordinate_space", "screen_pixels"},
			{"rows", std::move(dropdown_rows)},
		};
		entry["rect"] = RectToJson(Rect{picker->left, picker->top, picker->left + picker->width - 1, picker->top + picker->height - 1});

		Json widgets = Json::object();
		const std::pair<const char *, WidgetID> picker_widgets[] = {
			{"caption", WID_ASP_CAPTION},
			{"search", WID_ASP_SEARCH},
			{"owner", WID_ASP_OWNER},
			{"list", WID_ASP_LIST},
			{"scroll", WID_ASP_SCROLL},
			{"select", WID_ASP_SELECT},
			{"close", WID_ASP_CLOSE},
		};
		for (const auto &[name, widget] : picker_widgets) {
			widgets[name] = RectToJson(ToScreenRect(*picker, picker->GetWidget<NWidgetBase>(widget)->GetCurrentRect()));
		}
		const NWidgetBase *closebox = picker->nested_root->GetWidgetOfType(WWT_CLOSEBOX);
		if (closebox != nullptr) widgets["closebox"] = RectToJson(ToScreenRect(*picker, closebox->GetCurrentRect()));
		entry["widgets"] = std::move(widgets);

		Json rows = Json::array();
		const Rect area = picker->GetInnerRect(WID_ASP_LIST);
		const int row_height = GetSettingPickerRowHeight();
		const int first = picker->vscroll->GetPosition();
		const int last = std::min<int>(picker->visible.size(), first + picker->vscroll->GetCapacity());
		for (int i = first; i < last; i++) {
			const AgentSettingPickerRow &row = picker->rows[picker->visible[i]];
			const int top = area.top + (i - first) * row_height;
			Json row_entry = Json::object();
			row_entry["name"] = row.name;
			row_entry["scope"] = row.scope;
			row_entry["value"] = row.value_text;
			row_entry["selected"] = (static_cast<size_t>(i) == picker->selected);
			row_entry["rect"] = RectToJson(ToScreenRect(*picker, Rect{area.left, top, area.right, top + row_height - 1}));
			rows.push_back(std::move(row_entry));
		}
		entry["rows"] = std::move(rows);
		out["settings_picker"] = std::move(entry);
	}

	Window *status = FindWindowById(WindowClass::AgentChatStatus, 0);
	if (status != nullptr) {
		Json entry = Json::object();
		/* The status window has its own position, so its rectangles need its own offset. */
		entry["left"] = status->left;
		entry["top"] = status->top;
		entry["text"] = RectToJson(ToScreenRect(*status, status->GetWidget<NWidgetBase>(WID_ACS_TEXT)->GetCurrentRect()));
		entry["stop"] = RectToJson(ToScreenRect(*status, status->GetWidget<NWidgetBase>(WID_ACS_STOP)->GetCurrentRect()));
		out["status_window"] = std::move(entry);
	} else {
		out["status_window"] = Json();
	}

	return out;
}

std::string SettingAttachmentLabelForTest(const std::string &setting_name, const std::string &scope, int32_t company_id)
{
	/* The picker stores the exact engine key as the label of the attachment, so the test builds the
	 * same attachment AttachSelected() builds and runs the real label code on it. */
	AgentChatAttachment attachment;
	attachment.type = "setting";
	attachment.setting_name = setting_name;
	attachment.scope = scope;
	attachment.name = setting_name;
	attachment.company_id = company_id;
	return GetAttachmentLabel(attachment);
}

nlohmann::json SettingPickerRowsForTest(const std::string &filter)
{
	/* The same owner the picker opens with, so the values and the owner fields are the real ones. */
	const CompanyID owner = DefaultSettingPickerOwner();
	const Company *company = Company::GetIfValid(owner);
	Json rows = Json::array();
	for (const AgentSettingPickerRow &row : BuildSettingPickerRows(owner)) {
		if (!SettingRowMatches(row, filter)) continue;
		Json entry = {{"name", row.name}, {"scope", row.scope}, {"value", row.value_text}};
		if (row.scope == "company") {
			if (company == nullptr) {
				entry["company_id"] = nullptr;
				entry["company_generation"] = nullptr;
			} else {
				entry["company_id"] = owner.base();
				entry["company_generation"] = GetPoolGeneration(*company);
			}
		}
		rows.push_back(std::move(entry));
	}
	return rows;
}
