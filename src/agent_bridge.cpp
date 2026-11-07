/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file agent_bridge.cpp Native bridge between the running game and the in-game agent backend. */

#include "stdafx.h"
#include "agent_bridge.h"

#include "agent_admin_tools.h"
#include "agent_capture.h"
#include "agent_chat_gui.h"
#include "agent_game_tools.h"
#include "agent_transport_tools.h"
#include "command_func.h"
#include "company_base.h"
#include "company_cmd.h"
#include "company_func.h"
#include "core/backup_type.hpp"
#include "core/math_func.hpp"
#include "debug.h"
#include "genworld.h"
#include "industry.h"
#include "map_func.h"
#include "misc_cmd.h"
#include "network/core/http.h"
#include "network/network.h"
#include "openttd.h"
#include "progress.h"
#include "rev.h"
#include "signs_base.h"
#include "station_base.h"
#include "station_cmd.h"
#include "strings_func.h"
#include "table/strings.h"
#include "timer/timer_game_calendar.h"
#include "timer/timer_game_tick.h"
#include "tile_map.h"
#include "town.h"
#include "town_cmd.h"
#include "vehicle_base.h"
#include "video/video_driver.hpp"

#include "3rdparty/fmt/format.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <deque>
#include <fstream>
#include <map>
#include <memory>
#include <optional>
#include <random>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "safeguards.h"

namespace InGameAgent {

namespace {

using Clock = std::chrono::steady_clock;

/** Delay before retrying a failed hello. */
constexpr auto HELLO_RETRY = std::chrono::seconds(5);
/** Delay before retrying a failed poll. */
constexpr auto POLL_RETRY = std::chrono::seconds(2);
/** Delay before retrying a failed result delivery. */
constexpr auto RESULT_RETRY = std::chrono::seconds(1);
/** Maximum number of results kept while the backend is unreachable. */
constexpr size_t MAX_PENDING_RESULTS = 256;
/** Maximum number of request receipts kept for idempotent replay. */
constexpr size_t MAX_RECEIPTS = 512;
/** Default page size of world.query. */
constexpr int DEFAULT_QUERY_LIMIT = 50;
/** Maximum page size of world.query. */
constexpr int MAX_QUERY_LIMIT = 500;
/** Maximum number of tiles a single world.query may cover. */
constexpr int MAX_QUERY_TILES = 4096;
/** Maximum number of references a single world.objects or expected_refs may carry. */
constexpr size_t MAX_OBJECT_REFS = 64;

/**
 * Maximum number of requests that may wait for the next draw tick. A capture is
 * taken and the camera is read or moved on the draw thread, so a request that is
 * scheduled but not drawn yet holds a receipt and a result slot; the bound keeps
 * that reservation small.
 */
constexpr size_t MAX_PENDING_DEFERRED = 4;

/**
 * Generate a random (version 4) UUID.
 * @return The UUID as a lower case string.
 */
std::string GenerateUuid()
{
	static std::mt19937_64 rng{std::random_device{}()};
	uint64_t hi = rng();
	uint64_t lo = rng();

	hi = (hi & 0xFFFFFFFFFFFF0FFFULL) | 0x0000000000004000ULL;
	lo = (lo & 0x3FFFFFFFFFFFFFFFULL) | 0x8000000000000000ULL;

	return fmt::format("{:08x}-{:04x}-{:04x}-{:04x}-{:012x}",
			static_cast<uint32_t>(hi >> 32), static_cast<uint16_t>(hi >> 16), static_cast<uint16_t>(hi),
			static_cast<uint16_t>(lo >> 48), static_cast<uint64_t>(lo & 0xFFFFFFFFFFFFULL));
}

/**
 * Lower case an ASCII string.
 * @param text The string to lower case.
 * @return The lower cased string.
 */
std::string LowerAscii(std::string_view text)
{
	std::string result(text);
	for (char &c : result) {
		if (c >= 'A' && c <= 'Z') c = c - 'A' + 'a';
	}
	return result;
}

/**
 * Check whether a string contains another string, ignoring ASCII case.
 * @param haystack The string to search in.
 * @param needle The lower cased string to search for.
 * @return True iff \a haystack contains \a needle.
 */
bool ContainsAsciiCI(std::string_view haystack, std::string_view needle)
{
	if (needle.empty()) return true;
	const std::string lowered = LowerAscii(haystack);
	return lowered.find(needle) != std::string::npos;
}

/**
 * Read an integer from a JSON object, tolerating floats and missing keys.
 * @param object The object to read from.
 * @param key The key to read.
 * @param fallback The value to return when the key is missing or not a number.
 * @return The value of the key, or \a fallback.
 */
int JsonInt(const Json &object, const char *key, int fallback)
{
	if (!object.is_object() || !object.contains(key)) return fallback;
	const Json &value = object[key];
	if (value.is_number_integer()) return value.get<int>();
	if (value.is_number_float()) return static_cast<int>(value.get<double>());
	return fallback;
}

/**
 * Read a string from a JSON object, tolerating missing keys and other types.
 * @param object The object to read from.
 * @param key The key to read.
 * @param fallback The value to return when the key is missing or not a string.
 * @return The value of the key, or \a fallback.
 */
std::string JsonString(const Json &object, const char *key, std::string_view fallback = {})
{
	if (!object.is_object() || !object.contains(key)) return std::string(fallback);
	const Json &value = object[key];
	if (!value.is_string()) return std::string(fallback);
	return value.get<std::string>();
}

/** A finished HTTP request, waiting to be handled on the main thread. */
struct Completion {
	uint64_t id;      ///< Identifier of the request.
	bool ok;          ///< Whether the request completed without a transport error.
	std::string body; ///< The received body.
};

std::vector<Completion> _completions; ///< Completions waiting for the main thread.

/**
 * HTTP callback that collects the response body and hands it to the main thread.
 *
 * The callbacks are invoked from NetworkHTTPSocketHandler::HTTPReceive(), which
 * runs on the main thread, so no locking is needed here.
 */
class BridgeHTTPCallback : public HTTPCallback {
public:
	/**
	 * Create the callback.
	 * @param id Identifier of the request this callback belongs to.
	 */
	explicit BridgeHTTPCallback(uint64_t id) : id(id) {}

	void OnFailure() override
	{
		_completions.push_back({this->id, false, std::string{}});
	}

	void OnReceiveData(std::unique_ptr<char[]> data, size_t length) override
	{
		if (data == nullptr) {
			_completions.push_back({this->id, true, std::move(this->body)});
			this->body.clear();
			return;
		}
		this->body.append(data.get(), length);
	}

	bool IsCancelled() const override { return false; }

private:
	uint64_t id;      ///< Identifier of the request this callback belongs to.
	std::string body; ///< Body received so far.
};

/** The kind of request that is in flight. */
enum class RequestKind {
	Hello, ///< POST /v1/engine/hello
	Poll,  ///< POST /v1/engine/poll
	Result,///< POST /v1/engine/result
	Ui,    ///< A request made by the native UI.
};

/** Bookkeeping for a request that is in flight. */
struct PendingRequest {
	RequestKind kind;                                ///< The kind of request.
	std::string epoch;                               ///< The epoch the request was issued for.
	std::function<void(bool, const Json &)> callback;///< Callback for UI requests.
};

/** A stored result of an executed request, kept so that a replay is answered identically. */
struct Receipt {
	Json result;               ///< The result of the request.
	bool acknowledged = false; ///< Whether the backend acknowledged delivery of the result.
};

/** All state of the bridge. */
struct BridgeState {
	bool initialised = false; ///< Whether the runtime configuration has been read.
	bool enabled = false;     ///< Whether a backend is configured.
	std::string base_url;     ///< Base URL of the backend, without a trailing slash.
	std::string engine_token; ///< Token for the engine routes.
	std::string ui_token;     ///< Token for the chat routes.
	std::string engine_id;    ///< Stable identity of this engine.

	std::string world_id;      ///< Identity of the world, persisted with the savegame.
	std::string epoch;         ///< Identity of this load of the world.
	std::string connection_id; ///< Connection handed out by the backend.
	bool connected = false;    ///< Whether the backend accepted our hello.
	int heartbeat_seconds = 5; ///< Poll interval requested by the backend.
	int64_t cursor = 0;        ///< Highest request sequence we have received.

	bool hello_in_flight = false;  ///< Whether a hello is in flight.
	bool poll_in_flight = false;   ///< Whether a poll is in flight.
	bool result_in_flight = false; ///< Whether a result delivery is in flight.
	Clock::time_point next_hello{};  ///< Earliest time to send the next hello.
	Clock::time_point next_poll{};   ///< Earliest time to send the next poll.
	Clock::time_point next_result{}; ///< Earliest time to send the next result.

	std::deque<Json> results;              ///< Results waiting to be delivered.
	Json inflight_result;                  ///< Result currently being delivered.
	std::map<std::string, Receipt> receipts; ///< Results of executed requests, by request id.
	std::deque<std::string> receipt_order; ///< Order in which receipts were stored.
	std::set<std::string> barrier_tasks;   ///< Tasks whose barrier has been acknowledged.
};

BridgeState _state;                                        ///< The bridge state.
uint64_t _next_request_id = 0;                             ///< Identifier of the next request.
std::map<uint64_t, PendingRequest> _pending;               ///< Requests that are in flight.
std::map<uint64_t, std::unique_ptr<BridgeHTTPCallback>> _callbacks; ///< Callbacks of the requests in flight.

/** A request that waits for the next draw tick to be executed. */
struct DeferredRequest {
	std::string method;   ///< The method the request calls.
	std::string world_id; ///< The world the request was issued for.
	std::string epoch;    ///< The epoch the request was issued for.
	std::string task_id;  ///< The task the request belongs to, if any.
	Json params;          ///< The parameters of the request.
};

std::map<std::string, DeferredRequest> _deferred; ///< Requests waiting for the draw tick, by request id.

/**
 * Build a successful method result.
 * @param value The value of the result.
 * @return The result.
 */
Json Ok(Json value)
{
	return {{"ok", true}, {"value", std::move(value)}};
}

/**
 * Build a failed method result.
 * @param code The machine readable error code.
 * @param message Optional human readable message.
 * @return The result.
 */
Json Error(std::string_view code, std::string_view message = {})
{
	Json result = {{"ok", false}, {"error", std::string(code)}};
	if (!message.empty()) result["message"] = std::string(message);
	return result;
}

/**
 * Map a tool module result onto the bridge result shape.
 *
 * The tool modules report `{ok, ...}` with their own diagnostics: the actual cost, the created
 * ids, the engine's message id, partial receipts and the gap detail. A success becomes
 * `{ok: true, value: <module result>}`, which is what the backend unwraps, so nothing is lost. A
 * failure keeps the module's error code and diagnostics at the top level, which is the bridge's
 * error shape, so a gap or a rejected command is never reported as a success.
 * @param result The result of the tool module.
 * @return The bridge result.
 */
Json FromModule(Json result)
{
	if (!result.is_object()) return Ok(std::move(result));

	const auto ok = result.find("ok");
	if (ok != result.end() && ok->is_boolean() && ok->get<bool>()) return Ok(std::move(result));

	if (!result.contains("error") || !result["error"].is_string()) result["error"] = "tool_failed";
	if (!result.contains("message") || !result["message"].is_string()) result["message"] = "the tool module reported a failure";
	return result;
}

/**
 * Check whether the world may be read or changed right now.
 *
 * While a world is generated or a savegame is loaded, the map and the pools are
 * owned by the progress thread and are not consistent yet. The bridge must not
 * query or mutate them then, and must not advertise a world that is not usable.
 * @return True iff a loaded world exists and no modal progress is running.
 */
bool IsWorldReady()
{
	if (_game_mode != GameMode::Normal && _game_mode != GameMode::Editor) return false;
	if (_generating_world) return false;
	if (HasModalProgress()) return false;
	return true;
}

/**
 * Resolve the identity of the current world, if it is not known yet.
 *
 * The savegame id is persisted with the savegame, so it is a stable identity
 * for the world across loads.
 */
void ResolveWorldId()
{
	if (!_state.world_id.empty()) return;
	_state.world_id = _game_session_stats.savegame_id;
	if (_state.world_id.empty()) _state.world_id = GenerateUuid();
}

/**
 * Read a credential from an environment variable, or from the file named by
 * the matching `*_FILE` variable.
 *
 * The file variant keeps the secret out of the process environment, which is
 * world readable on most systems. The value is never logged.
 * @param env_name Name of the environment variable holding the credential.
 * @param file_env_name Name of the environment variable holding a file name.
 * @param credential Receives the credential.
 * @return True iff a non-empty credential was found.
 */
bool ReadCredential(const char *env_name, const char *file_env_name, std::string &credential)
{
	const char *value = std::getenv(env_name);
	if (value != nullptr && *value != '\0') {
		credential = value;
		return true;
	}

	const char *path = std::getenv(file_env_name);
	if (path == nullptr || *path == '\0') return false;

	std::ifstream file(path, std::ios::binary);
	if (!file) {
		Debug(Facility::Net, Severity::Warning, "In-game agent bridge: cannot read the file named by {}", file_env_name);
		return false;
	}

	std::string contents(4096, '\0');
	file.read(contents.data(), static_cast<std::streamsize>(contents.size()));
	contents.resize(static_cast<size_t>(file.gcount()));

	/* A credential file usually ends with a newline; strip the surrounding whitespace. */
	const size_t begin = contents.find_first_not_of(" \t\r\n");
	if (begin == std::string::npos) return false;
	const size_t end = contents.find_last_not_of(" \t\r\n");
	credential = contents.substr(begin, end - begin + 1);
	return !credential.empty();
}

/**
 * Read the runtime configuration, once.
 * @return True iff a backend is configured.
 */
bool EnsureInitialised()
{
	if (_state.initialised) return _state.enabled;
	_state.initialised = true;

	const char *url = std::getenv("AGENT_BACKEND_URL");
	if (url == nullptr || *url == '\0') {
		Debug(Facility::Net, Severity::Info, "In-game agent bridge: AGENT_BACKEND_URL is not set; bridge disabled");
		return false;
	}

	_state.base_url = url;
	while (!_state.base_url.empty() && _state.base_url.back() == '/') _state.base_url.pop_back();

	/* A configured backend is only talked to with credentials; never send an unauthenticated request. */
	if (!ReadCredential("AGENT_ENGINE_TOKEN", "AGENT_ENGINE_TOKEN_FILE", _state.engine_token) ||
			!ReadCredential("AGENT_UI_TOKEN", "AGENT_UI_TOKEN_FILE", _state.ui_token)) {
		Debug(Facility::Net, Severity::Warning, "In-game agent bridge: AGENT_ENGINE_TOKEN(_FILE) and AGENT_UI_TOKEN(_FILE) are required; bridge disabled");
		return false;
	}

	const char *engine_id = std::getenv("AGENT_ENGINE_ID");
	_state.engine_id = (engine_id != nullptr && *engine_id != '\0') ? engine_id : GenerateUuid();

	if (_state.epoch.empty()) _state.epoch = GenerateUuid();

	_state.enabled = true;
	_state.next_hello = Clock::now();
	Debug(Facility::Net, Severity::Info, "In-game agent bridge: backend {} configured", _state.base_url);
	return true;
}

/**
 * Send a request to the backend.
 * @param kind The kind of request.
 * @param path The path below the backend base URL.
 * @param payload The body of the request; empty for a GET.
 * @param token The token to authenticate with.
 * @param epoch The epoch the request is issued for.
 * @param callback Optional callback for UI requests.
 */
void Send(RequestKind kind, const std::string &path, std::string payload, const std::string &token, const std::string &epoch, std::function<void(bool, const Json &)> callback)
{
	const uint64_t id = ++_next_request_id;
	_callbacks[id] = std::make_unique<BridgeHTTPCallback>(id);
	_pending[id] = PendingRequest{kind, epoch, std::move(callback)};

	HTTPHeaders headers;
	headers.emplace_back("Authorization", "Bearer " + token);

	NetworkHTTPSocketHandler::Connect(_state.base_url + path, _callbacks[id].get(), std::move(payload), std::move(headers));
}

/**
 * Check whether another receipt can be stored.
 *
 * A receipt may only be forgotten once the backend acknowledged the delivery of
 * its result; otherwise a replay of that request would execute it a second time.
 * A request that waits for the draw tick already holds a receipt slot, so the
 * pending requests are counted here: an ordinary request must not take the slot
 * a deferred request was promised, or its result could never be stored.
 * @return True iff there is room for another receipt.
 */
bool HasReceiptCapacity()
{
	if (_state.receipts.size() + _deferred.size() < MAX_RECEIPTS) return true;
	for (const std::string &request_id : _state.receipt_order) {
		const auto receipt = _state.receipts.find(request_id);
		if (receipt != _state.receipts.end() && receipt->second.acknowledged) return true;
	}
	return false;
}

/**
 * Forget the oldest acknowledged receipt, to make room for a new one.
 *
 * Only acknowledged receipts are evicted: an unacknowledged one is the only
 * answer a replay of its request can get, so dropping it would execute the
 * request a second time. A pending request holds a receipt slot as well, so the
 * room is made for both.
 */
void MakeReceiptRoom()
{
	while (_state.receipts.size() + _deferred.size() >= MAX_RECEIPTS) {
		const auto oldest = std::find_if(_state.receipt_order.begin(), _state.receipt_order.end(), [](const std::string &request_id) {
			const auto receipt = _state.receipts.find(request_id);
			return receipt != _state.receipts.end() && receipt->second.acknowledged;
		});
		if (oldest == _state.receipt_order.end()) return;
		_state.receipts.erase(*oldest);
		_state.receipt_order.erase(oldest);
	}
}

/**
 * Store the result of an executed request, so a replay is answered identically.
 * @param request_id The request the result belongs to.
 * @param result The result of the request.
 */
void StoreReceipt(const std::string &request_id, const Json &result)
{
	MakeReceiptRoom();
	_state.receipts[request_id] = Receipt{result, false};
	_state.receipt_order.push_back(request_id);
}

/**
 * Mark the receipt of a delivered result as acknowledged by the backend.
 * @param request_id The request whose result was delivered.
 */
void AcknowledgeReceipt(const std::string &request_id)
{
	const auto receipt = _state.receipts.find(request_id);
	if (receipt != _state.receipts.end()) receipt->second.acknowledged = true;
}

/**
 * Queue a result for delivery to the backend.
 * @param request_id The request the result belongs to.
 * @param result The result of the request.
 * @return False iff the queue is full; the caller has to retry later.
 */
bool QueueResult(const std::string &request_id, const Json &result)
{
	/*
	 * A request that waits for the draw tick already holds a result slot. Counting
	 * the pending requests here keeps that slot free: an ordinary result may not
	 * consume it, because the deferred result is produced later and cannot be
	 * replayed from the request.
	 */
	if (_state.results.size() + _deferred.size() >= MAX_PENDING_RESULTS) {
		Debug(Facility::Net, Severity::Warning, "In-game agent bridge: result queue is full; the backend has to catch up");
		return false;
	}
	_state.results.push_back({{"request_id", request_id}, {"result", result}});
	return true;
}

/**
 * Get the name of a tile type, as used in the protocol.
 * @param type The tile type.
 * @return The name of the tile type.
 */
std::string_view TileTypeName(TileType type)
{
	switch (type) {
		case TileType::Clear: return "clear";
		case TileType::Railway: return "rail";
		case TileType::Road: return "road";
		case TileType::House: return "house";
		case TileType::Trees: return "trees";
		case TileType::Station: return "station";
		case TileType::Water: return "water";
		case TileType::Void: return "void";
		case TileType::Industry: return "industry";
		case TileType::TunnelBridge: return "tunnelbridge";
		case TileType::Object: return "object";
		default: return "unknown";
	}
}

/**
 * Describe a single tile.
 * @param tile The tile to describe.
 * @return The description of the tile.
 */
Json TileRow(TileIndex tile)
{
	const TileType type = GetTileType(tile);
	Json row = {
		{"x", TileX(tile)},
		{"y", TileY(tile)},
		{"type", std::string(TileTypeName(type))},
	};

	if (IsValidTile(tile)) {
		row["height"] = TileHeight(tile);
		if (type != TileType::House && type != TileType::Industry) {
			const Owner owner = GetTileOwner(tile);
			if (owner != OWNER_NONE && owner != INVALID_OWNER) row["owner"] = owner.base();
		}
	}

	return row;
}

/**
 * Does this world.project call ask for a screen projection instead of an administrative op?
 *
 * The administrative operations of world.project (`power`, `power_fuel`, `station_town`,
 * `district`) are named by `op` and stay with the admin module. A call without an op that
 * names a tile, a town or a screen pixel is a projection of the live viewport, which is a
 * read of the camera and not an administrative operation.
 * @param params The method parameters.
 * @return True iff the call is a projection request.
 */
bool IsProjectionRequest(const Json &params)
{
	if (!params.is_object() || params.contains("op")) return false;
	for (const char *key : {"tile", "tile_x", "tile_y", "town", "center", "screen_x", "screen_y"}) {
		if (params.contains(key)) return true;
	}
	return false;
}

/**
 * The methods that change the world, and therefore need the write gate.
 *
 * The administrative module knows which of its own operations are read-only
 * (`world.company op=info`, `world.towns op=needs`, `world.project op=power/district`),
 * so it decides for its families; the rest is fixed by the method name. Moving the camera
 * is a UI operation, but it is a write of the viewport state, so it takes the same gate.
 * @param method The method name.
 * @param params The method parameters.
 * @return True when the call is expected to change the world.
 */
bool IsMutation(std::string_view method, const Json &params)
{
	if (method == "world.pause" || method == "world.rename" || method == "world.settings.set" ||
			method == "world.construct" || method == "world.vehicles" || method == "world.orders" ||
			method == "world.command" || method == "world.camera.set") {
		return true;
	}
	/* A projection through world.project is a read of the camera; the administrative ops keep their own answer. */
	if (method == "world.project" && IsProjectionRequest(params)) return false;
	return IsAdminMutation(method, params);
}

/**
 * Implement world.info.
 * @return The result of the method.
 */
Json WorldInfo()
{
	Json companies = Json::array();
	for (const Company *c : Company::Iterate()) {
		companies.push_back({
			{"id", c->index.base()},
			{"name", c->name.empty() ? GetString(STR_COMPANY_NAME, c->index) : c->name},
			{"is_ai", c->is_ai},
			{"money", static_cast<int64_t>(c->money)},
		});
	}

	return Ok({
		{"date", TimerGameCalendar::date.base()},
		{"ticks", TimerGameTick::counter},
		{"paused", _pause_mode.Any()},
		{"map", {{"width", Map::SizeX()}, {"height", Map::SizeY()}}},
		{"companies", companies},
		{"build", std::string(_openttd_revision)},
	});
}

/**
 * Map a query kind onto its canonical plural name, accepting the legacy
 * singular aliases of the first kernel version.
 * @param kind The kind as requested.
 * @return The canonical kind, or an empty view when the kind is not implemented.
 */
std::string_view CanonicalQueryKind(std::string_view kind)
{
	if (kind == "towns" || kind == "town") return "towns";
	if (kind == "companies" || kind == "company") return "companies";
	if (kind == "stations" || kind == "station") return "stations";
	if (kind == "industries" || kind == "industry") return "industries";
	if (kind == "vehicles" || kind == "vehicle") return "vehicles";
	if (kind == "signs" || kind == "sign") return "signs";
	if (kind == "tiles" || kind == "tile") return "tiles";
	return {};
}

/** A rectangular tile filter, as used by world.query. */
struct TileRange {
	bool active = false; ///< Whether the filter was given at all.
	int x0 = 0;          ///< First column.
	int y0 = 0;          ///< First row.
	int x1 = 0;          ///< Last column.
	int y1 = 0;          ///< Last row.

	/**
	 * Test whether a tile is inside the range.
	 * @param tile The tile to test.
	 * @return True iff the tile is inside the range, or the filter is inactive.
	 */
	bool Contains(TileIndex tile) const
	{
		if (!this->active) return true;
		const int x = TileX(tile);
		const int y = TileY(tile);
		return x >= this->x0 && x <= this->x1 && y >= this->y0 && y <= this->y1;
	}
};

/**
 * Read the spatial filter of a query.
 * @param filters The filters of the query.
 * @return The tile range; inactive when no bound was given.
 */
TileRange ReadTileRange(const Json &filters)
{
	TileRange range;
	if (!filters.contains("x0") && !filters.contains("y0") && !filters.contains("x1") && !filters.contains("y1")) return range;

	range.active = true;
	range.x0 = Clamp(JsonInt(filters, "x0", 0), 0, static_cast<int>(Map::MaxX()));
	range.y0 = Clamp(JsonInt(filters, "y0", 0), 0, static_cast<int>(Map::MaxY()));
	range.x1 = Clamp(JsonInt(filters, "x1", static_cast<int>(Map::MaxX())), range.x0, static_cast<int>(Map::MaxX()));
	range.y1 = Clamp(JsonInt(filters, "y1", static_cast<int>(Map::MaxY())), range.y0, static_cast<int>(Map::MaxY()));
	return range;
}

/**
 * Read the name filter of a query, accepting the canonical `name_contains` and
 * the legacy `name` alias.
 * @param filters The filters of the query.
 * @return The lower cased needle; empty when no name filter was given.
 */
std::string ReadNameFilter(const Json &filters)
{
	std::string name = JsonString(filters, "name_contains");
	if (name.empty()) name = JsonString(filters, "name");
	return LowerAscii(name);
}

/**
 * The world and epoch the bridge is bound to, as included in read results.
 * @return {world_id, epoch}.
 */
Json WorldBinding()
{
	ResolveWorldId();
	return {{"world_id", _state.world_id}, {"epoch", _state.epoch}};
}

/**
 * Implement world.query for towns, companies, stations, industries, vehicles,
 * signs and tiles.
 *
 * The canonical kind names are plural, as advertised by the backend registry;
 * the singular names of the first kernel version are accepted as aliases. The
 * filters are applied before pagination, so `total` is the number of matching
 * objects and not the number of returned rows.
 * @param params The parameters of the method.
 * @return The result of the method.
 */
Json WorldQuery(const Json &params)
{
	const std::string requested = JsonString(params, "kind");
	const std::string_view kind = CanonicalQueryKind(requested);
	if (kind.empty()) return Error("unsupported_query_kind", requested);

	const Json filters = params.contains("filters") && params["filters"].is_object() ? params["filters"] : Json::object();
	const int limit = Clamp(JsonInt(params, "limit", DEFAULT_QUERY_LIMIT), 1, MAX_QUERY_LIMIT);
	const int offset = std::max(0, JsonInt(params, "offset", 0));

	const std::string name = ReadNameFilter(filters);
	const int id = JsonInt(filters, "id", -1);
	/* The registry advertises company_id both as a filter and as a top level parameter. */
	const int company_id = filters.contains("company_id") ? JsonInt(filters, "company_id", -1) : JsonInt(params, "company_id", -1);
	const int town_id = JsonInt(filters, "town_id", -1);
	const TileRange range = ReadTileRange(filters);

	Json rows = Json::array();
	int total = 0;

	auto add_row = [&](Json row) {
		if (total >= offset && static_cast<int>(rows.size()) < limit) rows.push_back(std::move(row));
		total++;
	};

	if (kind == "tiles") {
		const int x = JsonInt(filters, "x", -1);
		const int y = JsonInt(filters, "y", -1);
		if (x >= 0 && y >= 0) {
			if (x > static_cast<int>(Map::MaxX()) || y > static_cast<int>(Map::MaxY())) return Error("invalid_params", "tile is outside the map");
			add_row(TileRow(TileXY(static_cast<uint>(x), static_cast<uint>(y))));
		} else {
			const int x0 = Clamp(JsonInt(filters, "x0", 0), 0, static_cast<int>(Map::MaxX()));
			const int y0 = Clamp(JsonInt(filters, "y0", 0), 0, static_cast<int>(Map::MaxY()));
			const int x1 = Clamp(JsonInt(filters, "x1", x0), x0, static_cast<int>(Map::MaxX()));
			const int y1 = Clamp(JsonInt(filters, "y1", y0), y0, static_cast<int>(Map::MaxY()));

			const int64_t count = static_cast<int64_t>(x1 - x0 + 1) * static_cast<int64_t>(y1 - y0 + 1);
			if (count > MAX_QUERY_TILES) return Error("range_too_large", "the tile range is too large");

			total = static_cast<int>(count);
			int index = 0;
			for (int ty = y0; ty <= y1 && static_cast<int>(rows.size()) < limit; ty++) {
				for (int tx = x0; tx <= x1 && static_cast<int>(rows.size()) < limit; tx++, index++) {
					if (index < offset) continue;
					rows.push_back(TileRow(TileXY(static_cast<uint>(tx), static_cast<uint>(ty))));
				}
			}
		}
	} else if (kind == "towns") {
		for (const Town *t : Town::Iterate()) {
			if (id >= 0 && static_cast<int>(t->index.base()) != id) continue;
			if (!ContainsAsciiCI(t->GetCachedName(), name)) continue;
			if (!range.Contains(t->xy)) continue;
			add_row({
				{"id", t->index.base()},
				{"generation", t->generation},
				{"name", t->GetCachedName()},
				{"tile", {TileX(t->xy), TileY(t->xy)}},
				{"population", t->cache.population},
			});
		}
	} else if (kind == "companies") {
		for (const Company *c : Company::Iterate()) {
			if (id >= 0 && static_cast<int>(c->index.base()) != id) continue;
			if (company_id >= 0 && static_cast<int>(c->index.base()) != company_id) continue;
			const std::string company_name = c->name.empty() ? GetString(STR_COMPANY_NAME, c->index) : c->name;
			if (!ContainsAsciiCI(company_name, name)) continue;
			add_row({
				{"id", c->index.base()},
				{"generation", c->generation},
				{"name", company_name},
				{"is_ai", c->is_ai},
				{"money", static_cast<int64_t>(c->money)},
			});
		}
	} else if (kind == "stations") {
		for (const Station *st : Station::Iterate()) {
			if (id >= 0 && static_cast<int>(st->index.base()) != id) continue;
			if (company_id >= 0 && static_cast<int>(st->owner.base()) != company_id) continue;
			if (town_id >= 0 && (st->town == nullptr || static_cast<int>(st->town->index.base()) != town_id)) continue;
			if (!ContainsAsciiCI(st->GetCachedName(), name)) continue;
			if (!range.Contains(st->xy)) continue;
			add_row({
				{"id", st->index.base()},
				{"generation", st->generation},
				{"name", st->GetCachedName()},
				{"tile", {TileX(st->xy), TileY(st->xy)}},
				{"owner", st->owner.base()},
				{"town_id", st->town != nullptr ? st->town->index.base() : -1},
			});
		}
	} else if (kind == "industries") {
		for (const Industry *ind : Industry::Iterate()) {
			if (id >= 0 && static_cast<int>(ind->index.base()) != id) continue;
			if (town_id >= 0 && (ind->town == nullptr || static_cast<int>(ind->town->index.base()) != town_id)) continue;
			if (!ContainsAsciiCI(ind->GetCachedName(), name)) continue;
			if (!range.Contains(ind->location.tile)) continue;
			add_row({
				{"id", ind->index.base()},
				{"generation", ind->generation},
				{"name", ind->GetCachedName()},
				{"tile", {TileX(ind->location.tile), TileY(ind->location.tile)}},
				{"town_id", ind->town != nullptr ? ind->town->index.base() : -1},
			});
		}
	} else if (kind == "vehicles") {
		for (const Vehicle *v : Vehicle::Iterate()) {
			if (id >= 0 && static_cast<int>(v->index.base()) != id) continue;
			if (company_id >= 0 && static_cast<int>(v->owner.base()) != company_id) continue;
			if (!ContainsAsciiCI(v->name, name)) continue;
			if (!range.Contains(v->tile)) continue;
			add_row({
				{"id", v->index.base()},
				{"generation", v->generation},
				{"name", v->name},
				{"owner", v->owner.base()},
				{"tile", {TileX(v->tile), TileY(v->tile)}},
			});
		}
	} else if (kind == "signs") {
		for (const Sign *sign : Sign::Iterate()) {
			if (id >= 0 && static_cast<int>(sign->index.base()) != id) continue;
			if (company_id >= 0 && static_cast<int>(sign->owner.base()) != company_id) continue;
			if (!ContainsAsciiCI(sign->name, name)) continue;
			const TileIndex tile = TileVirtXY(static_cast<uint>(std::max(0, sign->x)), static_cast<uint>(std::max(0, sign->y)));
			if (!range.Contains(tile)) continue;
			add_row({
				{"id", sign->index.base()},
				{"generation", sign->generation},
				{"name", sign->name},
				{"owner", sign->owner.base()},
				{"tile", {TileX(tile), TileY(tile)}},
			});
		}
	}

	Json value = {{"rows", rows}, {"total", total}};
	value.update(WorldBinding());
	return Ok(std::move(value));
}

/**
 * Describe a resolved pool item, or report why the reference is stale.
 * @tparam T The pool item type.
 * @tparam F Type of the function that adds the type specific fields.
 * @param type The protocol name of the type.
 * @param item The item, or nullptr when no such object exists.
 * @param generation The generation the reference was made for, or 0 when it carried none.
 * @param fill Adds the type specific fields of \a item to the row.
 * @return The resolution of the reference.
 */
template <typename T, typename F>
Json ResolvePoolItem(std::string_view type, const T *item, uint64_t generation, F fill)
{
	Json row = {{"type", std::string(type)}};
	if (item == nullptr) {
		row["found"] = false;
		row["error"] = "stale_ref";
		row["reason"] = "deleted";
		return row;
	}

	row["found"] = true;
	row["id"] = item->index.base();
	row["generation"] = item->generation;
	fill(row, *item);

	/* A reference that names a generation is only current while it is the same lifetime of the object. */
	if (generation != 0 && generation != item->generation) {
		row["found"] = false;
		row["error"] = "stale_ref";
		row["reason"] = "generation_mismatch";
	}
	return row;
}

/**
 * Resolve one typed object reference against the current world.
 *
 * A reference is {type, id?, generation?, tile?}. When it carries a
 * generation, an object that was deleted and recreated at the same id is
 * reported as stale instead of being silently resolved to the new object.
 * @param ref The reference to resolve.
 * @return The resolved object, or a stale_ref / unsupported_ref_type entry.
 */
Json ResolveRef(const Json &ref)
{
	const std::string type = JsonString(ref, "type");
	const int id = JsonInt(ref, "id", -1);
	const bool has_generation = ref.contains("generation") && (ref["generation"].is_number_unsigned() || ref["generation"].is_number_integer());
	const uint64_t generation = has_generation ? ref["generation"].get<uint64_t>() : 0;

	if (type == "town") {
		return ResolvePoolItem("town", id >= 0 ? Town::GetIfValid(static_cast<TownID>(id)) : nullptr, generation, [](Json &row, const Town &t) {
			row["name"] = t.GetCachedName();
			row["tile"] = {TileX(t.xy), TileY(t.xy)};
			row["population"] = t.cache.population;
		});
	}
	if (type == "company") {
		return ResolvePoolItem("company", id >= 0 ? Company::GetIfValid(static_cast<CompanyID>(id)) : nullptr, generation, [](Json &row, const Company &c) {
			row["name"] = c.name.empty() ? GetString(STR_COMPANY_NAME, c.index) : c.name;
			row["is_ai"] = c.is_ai;
			row["money"] = static_cast<int64_t>(c.money);
		});
	}
	if (type == "station") {
		return ResolvePoolItem("station", id >= 0 ? Station::GetIfValid(static_cast<StationID>(id)) : nullptr, generation, [](Json &row, const Station &st) {
			row["name"] = st.GetCachedName();
			row["tile"] = {TileX(st.xy), TileY(st.xy)};
			row["owner"] = st.owner.base();
			row["town_id"] = st.town != nullptr ? st.town->index.base() : -1;
		});
	}
	if (type == "industry") {
		return ResolvePoolItem("industry", id >= 0 ? Industry::GetIfValid(static_cast<IndustryID>(id)) : nullptr, generation, [](Json &row, const Industry &ind) {
			row["name"] = ind.GetCachedName();
			row["tile"] = {TileX(ind.location.tile), TileY(ind.location.tile)};
			row["town_id"] = ind.town != nullptr ? ind.town->index.base() : -1;
		});
	}
	if (type == "vehicle") {
		return ResolvePoolItem("vehicle", id >= 0 ? Vehicle::GetIfValid(static_cast<VehicleID>(id)) : nullptr, generation, [](Json &row, const Vehicle &v) {
			row["name"] = v.name;
			row["owner"] = v.owner.base();
			row["tile"] = {TileX(v.tile), TileY(v.tile)};
		});
	}
	if (type == "sign") {
		return ResolvePoolItem("sign", id >= 0 ? Sign::GetIfValid(static_cast<SignID>(id)) : nullptr, generation, [](Json &row, const Sign &sign) {
			row["name"] = sign.name;
			row["owner"] = sign.owner.base();
			const TileIndex tile = TileVirtXY(static_cast<uint>(std::max(0, sign.x)), static_cast<uint>(std::max(0, sign.y)));
			row["tile"] = {TileX(tile), TileY(tile)};
		});
	}
	if (type == "tile") {
		const Json location = ref.contains("tile") && ref["tile"].is_object() ? ref["tile"] : Json::object();
		const int x = JsonInt(location, "x", -1);
		const int y = JsonInt(location, "y", -1);
		if (x < 0 || y < 0 || x > static_cast<int>(Map::MaxX()) || y > static_cast<int>(Map::MaxY())) {
			return {{"type", "tile"}, {"found", false}, {"error", "stale_ref"}, {"reason", "deleted"}};
		}
		Json row = TileRow(TileXY(static_cast<uint>(x), static_cast<uint>(y)));
		/* TileRow names the tile type in `type`; a reference is typed `tile`, so the tile type moves to `what`. */
		row["what"] = row["type"];
		row["type"] = "tile";
		row["found"] = true;
		return row;
	}

	/* Types the kernel does not resolve yet are an explicit gap, never a silent success. */
	return {{"type", type}, {"found", false}, {"error", "unsupported_ref_type"}};
}

/**
 * Implement world.objects: resolve typed references to current state.
 * @param params The parameters of the method.
 * @return The result of the method.
 */
Json WorldObjects(const Json &params)
{
	if (!params.contains("refs") || !params["refs"].is_array()) return Error("invalid_params", "refs must be an array");
	const Json &refs = params["refs"];
	if (refs.empty() || refs.size() > MAX_OBJECT_REFS) return Error("invalid_params", "refs must contain 1 to 64 references");

	Json objects = Json::array();
	for (const Json &ref : refs) {
		if (!ref.is_object()) return Error("invalid_params", "every ref must be an object");
		objects.push_back(ResolveRef(ref));
	}

	Json value = {{"objects", objects}};
	value.update(WorldBinding());
	return Ok(std::move(value));
}

/**
 * Implement world.pause.
 * @param params The parameters of the method.
 * @return The result of the method.
 */
Json WorldPause(const Json &params)
{
	if (!params.contains("paused") || !params["paused"].is_boolean()) return Error("invalid_params", "paused must be a boolean");

	const bool paused = params["paused"].get<bool>();
	if (!Command<Commands::Pause>::Post(PauseMode::Normal, paused)) return Error("command_failed", "pause");

	return Ok({{"paused", _pause_mode.Any()}});
}

/**
 * Implement world.rename for towns, companies and stations.
 * @param params The parameters of the method.
 * @param request_company The company the request was made for, if any.
 * @return The result of the method.
 */
Json WorldRename(const Json &params, CompanyID request_company)
{
	const std::string kind = JsonString(params, "kind");
	const int id = JsonInt(params, "id", -1);
	const std::string name = JsonString(params, "name");
	if (name.empty()) return Error("invalid_params", "name must not be empty");

	if (kind == "town") {
		const TownID town_id = static_cast<TownID>(id);
		if (id < 0 || !Town::IsValidID(town_id)) return Error("unknown_object", "no such town");
		if (!Command<Commands::RenameTown>::Post(STR_ERROR_CAN_T_RENAME_TOWN, town_id, name)) return Error("command_failed", "rename town");
	} else if (kind == "company") {
		const CompanyID company_id = id >= 0 ? static_cast<CompanyID>(id) : request_company;
		if (!Company::IsValidID(company_id)) return Error("unknown_object", "no such company");

		/* RenameCompany operates on the current company, so scope it to the requested one. */
		Backup<CompanyID> backup(_current_company, company_id);
		const bool posted = Command<Commands::RenameCompany>::Post(STR_ERROR_CAN_T_CHANGE_COMPANY_NAME, name);
		backup.Restore();
		if (!posted) return Error("command_failed", "rename company");

		/* Verify the object that was actually renamed, not just that the command was accepted. */
		const Company *company = Company::GetIfValid(company_id);
		if (company == nullptr || company->name != name) return Error("command_failed", "the company name was not changed");
	} else if (kind == "station") {
		const StationID station_id = static_cast<StationID>(id);
		if (id < 0 || !Station::IsValidID(station_id)) return Error("unknown_object", "no such station");
		if (!Command<Commands::RenameStation>::Post(STR_ERROR_CAN_T_RENAME_STATION, station_id, name)) return Error("command_failed", "rename station");
	} else {
		return Error("unsupported_rename_kind", kind);
	}

	return Ok({{"ok", true}, {"name", name}});
}

/**
 * Validate the references a mutation expects to still be current.
 *
 * The check is atomic: every reference is resolved before anything is applied,
 * so a mutation never runs against a world in which one of its objects was
 * deleted and its id reused.
 * @param params The parameters of the request.
 * @return An error result when a reference is stale, nothing when all are current.
 */
std::optional<Json> CheckExpectedRefs(const Json &params)
{
	if (!params.contains("expected_refs")) return std::nullopt;
	const Json &refs = params["expected_refs"];
	if (!refs.is_array()) return Error("invalid_params", "expected_refs must be an array");
	if (refs.size() > MAX_OBJECT_REFS) return Error("invalid_params", "expected_refs must contain at most 64 references");

	for (const Json &ref : refs) {
		if (!ref.is_object()) return Error("invalid_params", "every expected ref must be an object");
		Json resolved = ResolveRef(ref);
		if (!resolved.value("found", false)) {
			Json error = Error("stale_ref", "an expected object is no longer the one the request was made for");
			error["detail"] = std::move(resolved);
			return error;
		}
	}
	return std::nullopt;
}

/**
 * Check that a request belongs to the world and epoch the bridge is bound to.
 * @param request The request, as received from the backend.
 * @return An error result when the request is stale, nothing when it may be applied.
 */
std::optional<Json> CheckRequestWorld(const Json &request)
{
	const std::string world_id = JsonString(request, "world_id");
	const std::string epoch = JsonString(request, "epoch");
	if (world_id.empty() || epoch.empty()) return Error("invalid_request", "world_id and epoch are required");
	if (world_id != _state.world_id) return Error("stale_world", "the request belongs to another world");
	if (epoch != _state.epoch) return Error("stale_epoch", "the request belongs to another epoch");
	return std::nullopt;
}

/**
 * Read the company a request is made for.
 * @param request The request, as received from the backend.
 * @return The company id, or CompanyID::Invalid() when the request does not name one.
 */
CompanyID RequestCompanyId(const Json &request)
{
	const Json params = request.contains("params") && request["params"].is_object() ? request["params"] : Json::object();
	const int id = request.contains("company_id") ? JsonInt(request, "company_id", -1) : JsonInt(params, "company_id", -1);
	if (id < 0 || !Company::IsValidID(static_cast<CompanyID>(id))) return CompanyID::Invalid();
	return static_cast<CompanyID>(id);
}

/**
 * Turn a failed capture into a bridge error, keeping the diagnostics of the capture.
 * @param capture The failed capture result.
 * @return The error result.
 */
Json CaptureFailure(const Json &capture)
{
	Json result = Error(JsonString(capture, "error", "capture_failed"), "the frame could not be captured");
	for (const char *key : {"tick", "tick_now", "width", "height", "pixels", "max_pixels", "png_bytes", "max_bytes", "max_body_bytes"}) {
		if (capture.contains(key)) result[key] = capture[key];
	}
	return result;
}

/**
 * Turn a camera or projection result into a bridge result.
 *
 * Same shape as the other deferred method: a success is `{ok: true, value: <camera result>}`, a
 * failure keeps the camera's own error code and its diagnostics at the top level, so a missing
 * viewport is never reported as a success. A read is bound to the world and epoch it was taken in,
 * like every other read of the bridge.
 * @param camera The result of InspectGameViewport() or SetGameCamera().
 * @param bind_world Whether to add world_id/epoch to the value (reads only).
 * @return The bridge result.
 */
Json CameraResult(Json camera, bool bind_world)
{
	if (!camera.is_object()) return Ok(std::move(camera));

	if (!camera.value("ok", false)) {
		Json result = Error(JsonString(camera, "error", "camera_failed"), JsonString(camera, "message", "the camera request failed"));
		if (camera.contains("tick")) result["tick"] = camera["tick"];
		return result;
	}

	if (bind_world) camera.update(WorldBinding());
	return Ok(std::move(camera));
}

/**
 * Run one deferred request on the draw tick.
 *
 * Runs with the game state lock and the video buffer lock held, so neither the world nor the frame
 * can change underneath it. The world, the epoch and the task barrier of the request are checked
 * again, and a write is checked against `expected_refs` a second time: a load, a Stop or a changed
 * object between scheduling and drawing must not turn into a write of another world or object.
 * @param deferred The request to run.
 * @return The bridge result.
 */
Json RunDeferred(const DeferredRequest &deferred)
{
	if (!IsWorldReady()) return Error("world_not_ready", "the world is being generated or loaded");
	if (!deferred.task_id.empty() && _state.barrier_tasks.find(deferred.task_id) != _state.barrier_tasks.end()) {
		return Error("task_stopped", "the task has been stopped");
	}

	const bool mutation = IsMutation(deferred.method, deferred.params);
	if (mutation) {
		if (const std::optional<Json> stale = CheckExpectedRefs(deferred.params); stale.has_value()) return *stale;
	}

	if (deferred.method == "world.capture") {
		const Json capture = CaptureGameFrame(deferred.params);
		return capture.value("ok", false) ? Ok(capture) : CaptureFailure(capture);
	}

	/* world.camera.get and a world.project projection read the camera; world.camera.set moves it. */
	const Json camera = mutation ? SetGameCamera(deferred.params) : InspectGameViewport(deferred.params);
	return CameraResult(std::move(camera), !mutation);
}

/**
 * Finish a request that was deferred to the draw tick.
 *
 * The receipt and the result are stored only here, so a placeholder is never delivered as if the
 * request had been applied.
 *
 * The entry is removed first, which releases the receipt and result slot the request reserved. That
 * is safe because this runs under the game state lock, so no other capacity check can observe the
 * released slot before the answer of this request is stored and queued below.
 * @param request_id The request that was deferred.
 */
void FinishDeferredRequest(const std::string &request_id)
{
	const auto entry = _deferred.find(request_id);
	if (entry == _deferred.end()) return;
	const DeferredRequest deferred = std::move(entry->second);
	_deferred.erase(entry);

	/*
	 * The backend invalidates the requests of an epoch it no longer serves, so a request of a world
	 * that is gone is dropped instead of answered with a result the backend would reject as stale
	 * and the bridge would retry forever.
	 */
	if (deferred.world_id != _state.world_id || deferred.epoch != _state.epoch) {
		Debug(Facility::Net, Severity::Info, "In-game agent bridge: dropping deferred {} {} of a world that is gone", deferred.method, request_id);
		return;
	}

	const Json result = RunDeferred(deferred);

	StoreReceipt(request_id, result);
	/*
	 * The reservation taken by ScheduleDeferred guarantees this slot: every other capacity check
	 * counts the pending requests, and this request has just released its own. The fallback keeps
	 * the answer deliverable even if that invariant is ever broken, because a deferred result
	 * cannot be replayed from the request.
	 */
	if (!QueueResult(request_id, result)) {
		Debug(Facility::Net, Severity::Warning, "In-game agent bridge: result queue is full; queueing the deferred result anyway");
		_state.results.push_back({{"request_id", request_id}, {"result", result}});
	}
}

/**
 * Schedule a request for the next draw tick.
 *
 * The request must not run inside the game loop: the draw thread takes the video buffer lock before
 * the game state lock, so touching the frame or the viewport from the game thread would invert that
 * order. VideoDriver::QueueOnMainThread runs the callback on the next draw tick with both locks
 * held, which is exactly the context CaptureGameFrame(), InspectGameViewport() and SetGameCamera()
 * document.
 * @param method The method the request calls.
 * @param request The request, as received from the backend.
 * @param request_id The id of the request.
 * @param params The parameters of the request.
 * @param deferred Set to true iff the request was scheduled and answers later.
 * @return The result of the request; a placeholder when the request was scheduled.
 */
Json ScheduleDeferred(std::string_view method, const Json &request, const std::string &request_id, const Json &params, bool &deferred)
{
	/* A replay of a request that is already waiting must not schedule a second execution. */
	if (_deferred.find(request_id) != _deferred.end()) {
		deferred = true;
		return Ok({{"deferred", true}});
	}

	/* Without a GUI there is no draw tick to run the callback on, so the queue would never drain. */
	VideoDriver *driver = VideoDriver::GetInstance();
	if (driver == nullptr || !driver->HasGUI()) {
		return Error("headless_unavailable", "no video driver with a GUI; the request cannot run on the draw tick");
	}

	/* Reserve the receipt and the result of the request before it is scheduled, so the answer can
	 * always be stored and delivered when the draw tick runs. */
	if (_deferred.size() >= MAX_PENDING_DEFERRED ||
			_state.results.size() + _deferred.size() >= MAX_PENDING_RESULTS ||
			_state.receipts.size() + _deferred.size() >= MAX_RECEIPTS) {
		return Error("busy", "too many requests are waiting for the next draw tick");
	}

	_deferred[request_id] = DeferredRequest{std::string(method), _state.world_id, _state.epoch, JsonString(request, "task_id"), params};
	driver->QueueOnMainThread([request_id]() { FinishDeferredRequest(request_id); });
	deferred = true;
	return Ok({{"deferred", true}});
}

/**
 * Execute one request from the backend on the main thread.
 * @param request The request, as received from the backend.
 * @param deferred Set to true iff the request answers later, from the draw tick.
 * @return The result of the request; a placeholder when the request was deferred.
 */
Json ExecuteRequest(const Json &request, bool &deferred)
{
	deferred = false;
	const std::string method = JsonString(request, "method");
	const Json params = request.contains("params") && request["params"].is_object() ? request["params"] : Json::object();
	const std::string task_id = JsonString(request, "task_id");

	/* A request may only be applied to the world and epoch it was issued for; check before anything else. */
	if (const std::optional<Json> stale = CheckRequestWorld(request); stale.has_value()) return *stale;

	/*
	 * The world may have started generating or loading between the poll and this
	 * execution; the map and the pools are not consistent then, so fail closed
	 * instead of reading or mutating a half built world.
	 */
	if (!IsWorldReady()) return Error("world_not_ready", "the world is being generated or loaded");

	/* The company the call is made for; every tool module scopes its commands to it. */
	const CompanyID company = RequestCompanyId(request);
	const bool mutation = IsMutation(method, params);

	/* Mutations are only supported in single player: in multiplayer Post() merely enqueues the command. */
	if (mutation && _networking) return Error("multiplayer_unsupported", "mutations are only supported in single player");

	/* The barrier is the last request of a task; nothing of that task may mutate the world afterwards. */
	if (method == "task.barrier") {
		if (!task_id.empty()) _state.barrier_tasks.insert(task_id);
		return Ok({{"barrier", true}});
	}

	if (mutation && !task_id.empty() && _state.barrier_tasks.find(task_id) != _state.barrier_tasks.end()) {
		return Error("task_stopped", "the task has been stopped");
	}

	/* A mutation may only run when the objects it was planned for are still the same objects. */
	if (mutation) {
		if (const std::optional<Json> stale = CheckExpectedRefs(params); stale.has_value()) return *stale;
	}

	if (method == "world.info") return WorldInfo();
	if (method == "world.query") {
		/*
		 * The game tool module owns the vehicle and engine inventory, which is richer than the
		 * core rows; the core query keeps the other kinds and stays the fallback when the module
		 * does not claim the kind, so a kind is never shadowed by a thinner answer.
		 */
		const std::string_view kind = CanonicalQueryKind(JsonString(params, "kind"));
		if (kind == "vehicles" || kind == "engines") {
			if (std::optional<Json> result = DispatchGameTool(method, params, company); result.has_value()) {
				Json mapped = FromModule(std::move(*result));
				/* Every read is bound to the world and epoch it was taken in, whoever produced it. */
				if (mapped["ok"].is_boolean() && mapped["ok"].get<bool>() && mapped.contains("value") && mapped["value"].is_object()) {
					mapped["value"].update(WorldBinding());
				}
				return mapped;
			}
		}
		return WorldQuery(params);
	}
	if (method == "world.objects") return WorldObjects(params);
	if (method == "world.pause") return WorldPause(params);
	if (method == "world.rename") return WorldRename(params, company);

	/* Read-only description of the native chat UI, so a GUI test can locate real targets. */
	if (method == "ui.inspect") return Ok(::DescribeAgentChatUi());

	/*
	 * The frame is copied and the camera is read or moved on the draw thread, so these answer from
	 * the next draw tick. A world.project call that asks for a screen projection is a camera read;
	 * a world.project call that names an administrative op stays with the admin module below, so
	 * neither shadows the other.
	 */
	if (method == "world.capture" || method == "world.camera.get" || method == "world.camera.set" ||
			(method == "world.project" && IsProjectionRequest(params))) {
		return ScheduleDeferred(method, request, JsonString(request, "request_id"), params, deferred);
	}

	/*
	 * Construction is split between two modules. The transport module owns the rail, water and
	 * air ops and returns nothing for the road ops it leaves to the game tool module, which is
	 * therefore asked second; only an op a module explicitly owns is executed by it.
	 */
	if (method == "world.construct") {
		if (std::optional<Json> result = DispatchTransportTool(method, params, company); result.has_value()) {
			return FromModule(std::move(*result));
		}
		if (std::optional<Json> result = DispatchGameTool(method, params, company); result.has_value()) {
			return FromModule(std::move(*result));
		}
		return Error("unsupported_method", method);
	}

	/* Vehicle and order commands belong to the game tool module. */
	if (method == "world.vehicles" || method == "world.orders") {
		if (std::optional<Json> result = DispatchGameTool(method, params, company); result.has_value()) {
			return FromModule(std::move(*result));
		}
		return Error("unsupported_method", method);
	}

	/* Administrative families: settings, companies, towns and projects. */
	if (std::optional<Json> result = DispatchAdminTool(method, params, company); result.has_value()) {
		return FromModule(std::move(*result));
	}

	return Error("unsupported_method", method);
}

/**
 * Handle the response to a hello.
 * @param ok Whether the request succeeded.
 * @param body The parsed response.
 */
void HandleHelloResponse(bool ok, const Json &body)
{
	if (!ok) {
		_state.connected = false;
		_state.next_hello = Clock::now() + HELLO_RETRY;
		Debug(Facility::Net, Severity::Warning, "In-game agent bridge: hello failed; retrying");
		return;
	}

	_state.connection_id = JsonString(body, "connection_id");
	_state.connected = !_state.connection_id.empty();
	_state.heartbeat_seconds = Clamp(static_cast<int>(body.value("heartbeat_seconds", 5.0)), 1, 60);
	_state.next_poll = Clock::now();

	Debug(Facility::Net, Severity::Info, "In-game agent bridge: connected to the backend (heartbeat {}s)", _state.heartbeat_seconds);
}

/**
 * Handle the response to a poll: execute the requests, in order, on the main thread.
 * @param ok Whether the request succeeded.
 * @param body The parsed response.
 */
void HandlePollResponse(bool ok, const Json &body)
{
	if (!ok) {
		_state.next_poll = Clock::now() + POLL_RETRY;
		return;
	}

	bool complete = true;
	if (body.contains("requests") && body["requests"].is_array()) {
		for (const Json &request : body["requests"]) {
			if (!request.is_object()) continue;
			const std::string request_id = JsonString(request, "request_id");
			if (request_id.empty()) continue;

			/* A repeated request is the same request: answer with the stored receipt instead of executing it twice. */
			const auto receipt = _state.receipts.find(request_id);
			Json result;
			if (receipt != _state.receipts.end()) {
				result = receipt->second.result;
			} else {
				/* Fail closed when no receipt may be forgotten: a replay must never execute a request twice. */
				if (!HasReceiptCapacity()) {
					complete = false;
					break;
				}
				bool deferred = false;
				result = ExecuteRequest(request, deferred);
				if (deferred) {
					/* A deferred request answers from the draw tick: no receipt and no result yet. */
					continue;
				}
				StoreReceipt(request_id, result);
			}

			/* Never drop a result: an applied mutation has to reach the backend. */
			if (!QueueResult(request_id, result)) {
				complete = false;
				break;
			}
		}
	}

	/* Only advance the cursor when the whole batch was handled; otherwise the backend replays it. */
	if (complete && body.contains("cursor") && body["cursor"].is_number_integer()) {
		_state.cursor = body["cursor"].get<int64_t>();
	}

	_state.next_poll = Clock::now() + std::chrono::seconds(complete ? _state.heartbeat_seconds : 1);
}

/**
 * Handle the response to a result delivery.
 * @param ok Whether the request succeeded.
 */
void HandleResultResponse(bool ok)
{
	if (!ok) {
		_state.next_result = Clock::now() + RESULT_RETRY;
		if (!_state.inflight_result.is_null()) _state.results.push_front(_state.inflight_result);
	} else if (!_state.inflight_result.is_null()) {
		/* The backend accepted the result, so its receipt may be forgotten later on. */
		AcknowledgeReceipt(JsonString(_state.inflight_result, "request_id"));
	}
	_state.inflight_result = Json();
}

/**
 * Handle everything the HTTP layer finished since the last call.
 */
void ProcessCompletions()
{
	if (_completions.empty()) return;

	std::vector<Completion> completions;
	completions.swap(_completions);

	for (const Completion &completion : completions) {
		const auto pending = _pending.find(completion.id);
		if (pending == _pending.end()) continue;
		const PendingRequest request = std::move(pending->second);
		_pending.erase(pending);
		_callbacks.erase(completion.id);

		Json body = Json::object();
		bool parsed = completion.body.empty();
		if (completion.ok && !completion.body.empty()) {
			body = Json::parse(completion.body, nullptr, false);
			parsed = !body.is_discarded();
			if (!parsed) body = Json::object();
		}
		const bool ok = completion.ok && parsed;

		/* A completion of an older epoch must not touch the state of the current one. */
		const bool current = request.epoch == _state.epoch;

		switch (request.kind) {
			case RequestKind::Hello:
				if (current) {
					_state.hello_in_flight = false;
					HandleHelloResponse(ok, body);
				}
				break;

			case RequestKind::Poll:
				if (current) {
					_state.poll_in_flight = false;
					HandlePollResponse(ok, body);
				}
				break;

			case RequestKind::Result:
				if (current) {
					_state.result_in_flight = false;
					HandleResultResponse(ok);
				}
				break;

			case RequestKind::Ui:
				/* A late answer of an older epoch must not look like current data: the UI would act on a world that is gone. */
				if (request.callback) {
					if (current) {
						request.callback(ok, body);
					} else {
						request.callback(false, Error("stale_epoch", "the world changed while the request was in flight"));
					}
				}
				break;
		}
	}
}

/**
 * The camera methods of this file, described the way the tool modules describe theirs.
 *
 * The parameters are the ones InspectGameViewport() and SetGameCamera() document in
 * `src/agent_capture.h`; `parameters_header` names that file so an operator can read the exact
 * meaning and the returned fields without guessing.
 * @return Capability document of the camera methods.
 */
Json CameraCapabilities()
{
	const Json read_parameters = Json::array({"tile", "tile_x", "tile_y", "town", "center", "screen_x", "screen_y", "clamp_to_map"});
	const Json write_parameters = Json::array({"tile", "tile_x", "tile_y", "town", "center", "scroll", "zoom", "zoom_delta", "instant"});

	Json methods = Json::array();
	methods.push_back({
		{"method", "world.camera.get"},
		{"authority", "read"},
		{"deferred", true},
		{"implemented", Json::array({"camera", "tile_to_screen", "screen_to_tile"})},
		{"parameters", read_parameters},
	});
	methods.push_back({
		{"method", "world.camera.set"},
		{"authority", "write"},
		{"deferred", true},
		{"implemented", Json::array({"center_tile", "center_town", "scroll", "zoom", "zoom_delta"})},
		{"parameters", write_parameters},
	});
	methods.push_back({
		{"method", "world.project"},
		{"authority", "read"},
		{"deferred", true},
		{"implemented", Json::array({"projection"})},
		{"parameters", read_parameters},
	});

	Json result = Json::object();
	result["module"] = "agent_bridge_camera";
	result["parameters_header"] = "src/agent_capture.h";
	result["methods"] = std::move(methods);
	result["notes"] = Json::array({
		"InspectGameViewport() and SetGameCamera() run on the draw tick, deferred like world.capture, so the reported camera is the frame the game draws next.",
		"world.camera.get and a world.project projection are reads and run while the game is paused; they report the real viewport, zoom and scroll position, and project a tile to the screen or a screen pixel to a tile.",
		"world.project is a projection only when it carries no `op` and names a tile, a town or a screen pixel; `op` stays with the administrative module (power, power_fuel, station_town, district).",
		"world.camera.set is a write of the viewport state: it takes the single-player gate, the task barrier and expected_refs, checked when it is scheduled and again on the draw tick.",
	});
	return result;
}

/**
 * The methods the engine can actually serve right now.
 *
 * Composed from the core methods of this file and from the capability documents of the tool
 * modules: a method is only advertised when a module really implements at least one of its
 * operations, so a gap is never advertised as a capability. The per-module documents are sent
 * along as detail, so the backend and the operator can see what is missing.
 * @return {methods, modules}: the advertised method names and the module documents.
 */
Json Capabilities()
{
	std::vector<std::string> methods = {"world.info", "world.query", "world.objects", "world.pause",
			"world.rename", "world.capture", "world.camera.get", "world.camera.set", "task.barrier", "ui.inspect"};
	Json modules = Json::array();

	for (const Json &module : {GameToolCapabilities(), TransportToolCapabilities(), AdminToolCapabilities(), CameraCapabilities()}) {
		modules.push_back(module);
		const auto entries = module.find("methods");
		if (entries == module.end() || !entries->is_array()) continue;
		for (const Json &entry : *entries) {
			const auto implemented = entry.find("implemented");
			if (implemented == entry.end() || !implemented->is_array() || implemented->empty()) continue;
			const std::string name = JsonString(entry, "method");
			if (name.empty()) continue;
			if (std::find(methods.begin(), methods.end(), name) == methods.end()) methods.push_back(name);
		}
	}

	Json advertised = Json::array();
	for (const std::string &method : methods) advertised.push_back(method);
	return {{"methods", std::move(advertised)}, {"modules", std::move(modules)}};
}

/** Send a hello for the current world and epoch. */
void SendHello()
{
	ResolveWorldId();
	_state.hello_in_flight = true;

	const Json capabilities = Capabilities();
	const Json body = {
		{"engine_id", _state.engine_id},
		{"world_id", _state.world_id},
		{"epoch", _state.epoch},
		{"build", std::string(_openttd_revision)},
		{"capabilities", capabilities["methods"]},
		{"capability_detail", capabilities["modules"]},
	};

	Send(RequestKind::Hello, "/v1/engine/hello", body.dump(), _state.engine_token, _state.epoch, nullptr);
}

/** Send a poll for the current world and epoch. */
void SendPoll()
{
	ResolveWorldId();
	_state.poll_in_flight = true;

	const Json body = {
		{"connection_id", _state.connection_id},
		{"world_id", _state.world_id},
		{"epoch", _state.epoch},
		{"results", Json::array()},
		{"cursor", _state.cursor},
	};

	Send(RequestKind::Poll, "/v1/engine/poll", body.dump(), _state.engine_token, _state.epoch, nullptr);
}

/** Deliver the next queued result, if any. */
void SendNextResult()
{
	if (_state.results.empty() || _state.result_in_flight) return;
	if (Clock::now() < _state.next_result) return;

	_state.inflight_result = _state.results.front();
	_state.results.pop_front();
	_state.result_in_flight = true;

	const Json body = {
		{"connection_id", _state.connection_id},
		{"request_id", _state.inflight_result["request_id"]},
		{"world_id", _state.world_id},
		{"epoch", _state.epoch},
		{"result", _state.inflight_result["result"]},
	};

	Send(RequestKind::Result, "/v1/engine/result", body.dump(), _state.engine_token, _state.epoch, nullptr);
}

} // namespace

/**
 * Run one step of the bridge. Must be called from the main thread once per
 * game loop iteration; it also runs while the game is paused.
 */
void Tick()
{
	if (!EnsureInitialised()) return;
	if (!_network_available) return;

	/* Make sure responses are handled even when the network background loop is not running. */
	NetworkHTTPSocketHandler::HTTPReceive();
	ProcessCompletions();

	/*
	 * The bridge only talks about a world that is actually loaded and not being
	 * generated or loaded: no hello, poll or result may describe a half built
	 * world. Completions above are still drained, so nothing is lost meanwhile.
	 */
	if (!IsWorldReady()) return;

	const Clock::time_point now = Clock::now();

	if (!_state.connected) {
		if (!_state.hello_in_flight && now >= _state.next_hello) SendHello();
		return;
	}

	if (!_state.poll_in_flight && now >= _state.next_poll) SendPoll();
	SendNextResult();
}

/**
 * Forget everything about the current world and start a fresh epoch. Called
 * whenever a new game is started or a savegame is loaded.
 */
void ResetWorld()
{
	_state.world_id.clear();
	_state.epoch = GenerateUuid();
	_state.connection_id.clear();
	_state.connected = false;
	_state.cursor = 0;
	_state.heartbeat_seconds = 5;

	_state.hello_in_flight = false;
	_state.poll_in_flight = false;
	_state.result_in_flight = false;
	_state.next_hello = Clock::now();
	_state.next_poll = Clock::now();
	_state.next_result = Clock::now();

	_state.results.clear();
	_state.inflight_result = Json();
	_state.receipts.clear();
	_state.receipt_order.clear();
	_state.barrier_tasks.clear();
	/* Requests waiting for the draw tick belong to the world that is gone; their callbacks find nothing and drop them. */
	_deferred.clear();
	/* Requests in flight are kept: their completions are ignored by epoch, and UI callbacks still get their answer. */
}

/**
 * Identity of the world the bridge is currently bound to.
 * @return {world_id, epoch, connection_id, connected}.
 */
Json Context()
{
	EnsureInitialised();
	ResolveWorldId();

	return {
		{"world_id", _state.world_id},
		{"epoch", _state.epoch},
		{"connection_id", _state.connection_id},
		{"connected", _state.connected},
	};
}

/**
 * Describe the objects attached to a tile, as typed attachment candidates.
 *
 * Every entry carries the generation of the object and the world/epoch it was
 * read in, so a later request can name the exact object it was planned for and
 * a reused id is detected instead of silently resolved.
 * @param tile The tile to inspect.
 * @return Array of {type, id?, generation?, name?, tile?, world_id, epoch} entries, most specific first.
 */
Json PickObjects(TileIndex tile)
{
	Json objects = Json::array();
	if (!IsWorldReady()) return objects;
	if (!IsValidTile(tile)) return objects;

	const TileType type = GetTileType(tile);
	const Json location = {TileX(tile), TileY(tile)};
	const Json binding = WorldBinding();

	auto add = [&](Json entry) {
		entry.update(binding);
		objects.push_back(std::move(entry));
	};

	if (type == TileType::Station) {
		const Station *st = Station::GetByTile(tile);
		if (st != nullptr) {
			add({{"type", "station"}, {"id", st->index.base()}, {"generation", st->generation}, {"name", st->GetCachedName()}, {"tile", location}});
		}
	} else if (type == TileType::Industry) {
		const Industry *ind = Industry::GetByTile(tile);
		if (ind != nullptr) {
			add({{"type", "industry"}, {"id", ind->index.base()}, {"generation", ind->generation}, {"tile", location}});
		}
	} else if (type == TileType::House) {
		const Town *town = Town::GetByTile(tile);
		if (town != nullptr) {
			add({{"type", "town"}, {"id", town->index.base()}, {"generation", town->generation}, {"name", town->GetCachedName()}, {"tile", location}});
		}
	}

	add({{"type", "tile"}, {"tile", location}, {"what", std::string(TileTypeName(type))}});
	return objects;
}

/**
 * Send a request to the backend on behalf of the native UI. The UI token is
 * used for authentication, and the callback is always invoked on the main
 * thread.
 * @param method HTTP method, e.g. "GET" or "POST".
 * @param path Path below the backend base URL, e.g. "/v1/chats".
 * @param body Request body; ignored for GET.
 * @param callback Called with the parsed response and whether it succeeded.
 */
void Request(std::string method, std::string path, Json body, std::function<void(Json, bool)> callback)
{
	if (!EnsureInitialised() || !_state.enabled) {
		if (callback) callback(Json::object(), false);
		return;
	}

	std::string payload;
	if (method != "GET" && !body.is_null()) payload = body.dump();

	Send(RequestKind::Ui, path, std::move(payload), _state.ui_token, _state.epoch,
			[cb = std::move(callback)](bool ok, const Json &response) { if (cb) cb(response, ok); });
}

} // namespace InGameAgent
