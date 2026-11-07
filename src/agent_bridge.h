/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file agent_bridge.h Native bridge between the running game and the in-game agent backend. */

#ifndef AGENT_BRIDGE_H
#define AGENT_BRIDGE_H

#include "3rdparty/nlohmann/json.hpp"
#include "company_type.h"
#include "tile_type.h"

#include <chrono>
#include <functional>
#include <string>
#include <string_view>

/**
 * Native bridge between the running game and the in-game agent backend.
 *
 * The bridge connects outward to the backend over authenticated HTTP, polls for
 * requests and executes them on the main thread. It is shared by the engine
 * (which owns the polling) and by the native chat UI (which uses Request()).
 */
namespace InGameAgent {

using Json = nlohmann::json;

/**
 * Delay between two polls while the backend is healthy.
 *
 * A request waits at most one idle interval before the engine picks it up, so this interval is part
 * of the tool latency; the heartbeat the backend asks for is a separate, much slower connection
 * signal and is not affected by it. One poll is in flight at a time and the loop sleeps between
 * polls, so a short interval is still a bounded healthy poll and never a spin on the HTTP layer.
 *
 * It is 25 ms because the measured round trip is dominated by waiting for the next poll: E16
 * measured ui.inspect HTTP round trips of 256..408 ms against a 250 ms budget at a 150 ms cadence,
 * which is about one idle wait plus the HTTP and frame handling the current transport adds. A
 * faster cadence reduces that wait, but whether the actual 250 ms budget is met with a real backend
 * and a real frame rate is a live measurement, not a property of this constant.
 */
inline constexpr std::chrono::milliseconds POLL_IDLE{25};

/** Delay before retrying a poll that failed; a backend that is down is not hammered. */
inline constexpr std::chrono::milliseconds POLL_RETRY{2000};

/**
 * Run one step of the bridge. Must be called from the main thread once per
 * game loop iteration; it also runs while the game is paused.
 */
void Tick();

/**
 * Forget everything about the current world and start a fresh epoch. Called
 * whenever a new game is started or a savegame is loaded.
 */
void ResetWorld();

/**
 * Identity of the world the bridge is currently bound to.
 * @return {world_id, epoch, connection_id, connected}.
 */
Json Context();

/**
 * Describe the objects attached to a tile, as typed attachment candidates.
 *
 * Every entry carries the generation of the object and the world/epoch it was
 * read in, so a later request can name the exact object it was planned for: the
 * object the tile carries (station, waypoint or buoy, industry, the town of a
 * house, rail/road/ship depot), then every vehicle and every sign standing on it,
 * then the tile itself. The tile is named in the canonical `{"x": .., "y": ..}`
 * shape the protocol schema uses.
 * @param tile The tile to inspect.
 * @return Array of {type, id?, generation?, name?, what?, tile, world_id, epoch} entries, most specific first.
 */
Json PickObjects(TileIndex tile);

/**
 * Describe the typed objects the tile itself carries, most specific first.
 *
 * A tile object is identified by the tile and by a state fingerprint derived from the native tile
 * data, not by a pool index: `terrain`, `water`, `tree`, `rail`, `signal`, `road`, `tram`,
 * `bridge`, `tunnel`, `mapobject` and `house`. Every row carries `type`, `what`, `subtype`,
 * `identity` ("tile"), `tile`, `owner` and `fingerprint`. `owner` is the owner the engine records
 * for that category (rail, signal, bridge, tunnel, water and mapobject: the tile owner; road and
 * tram: the owner of their own transport type; terrain, tree and house: null). A house also
 * carries `town_id` and `town_generation` (the lifetime of its town is part of its identity, and a
 * house has no pool generation of its own), a map object carries `object_id` and
 * `object_generation` (the pooled object behind the tile). Only the requested tile is read, so a
 * call is bounded and never scans the map.
 *
 * The fingerprint is FNV-1a over the values the engine stores for the object (tile type and
 * height, the recorded owner, both halves of every 64 bit pool lifetime, plus the subtype specific
 * fields listed at the implementation), not over the bytes of the map array and not over an
 * address, so it is stable across runs for the same state. Signal states change with passing
 * traffic, so a signal reference can go stale without anyone building anything.
 * @param tile The tile to describe.
 * @return Array of typed tile object rows.
 */
Json DescribeTileObjects(TileIndex tile);

/**
 * Resolve one typed reference against the current world, as `world.objects` does.
 *
 * A reference is `{type, id?, tile?, generation?, fingerprint?, what?, town_generation?,
 * object_generation?, world_id?, epoch?}`. A read resolves the object as it is now and reports its
 * identity; a write (`require_identity`) additionally rejects a pooled reference without a
 * generation, a tile object without a fingerprint (or, for a house, without the generation of its
 * town, or for a map object without the generation of the pooled object behind it) and any
 * reference whose recorded state is no longer current. A `point` reference names a location only
 * and has no identity to check.
 * @param ref The reference.
 * @param require_identity True for a write's `expected_refs`.
 * @return The resolved object, or a stale_ref entry with `reason`.
 */
Json ResolveObjectRef(const Json &ref, bool require_identity);

/**
 * Send a request to the backend on behalf of the native UI. The UI token is
 * used for authentication, and the callback is always invoked on the main
 * thread.
 * @param method HTTP method, e.g. "GET" or "POST".
 * @param path Path below the backend base URL, e.g. "/v1/chats".
 * @param body Request body; ignored for GET.
 * @param callback Called with the parsed response and whether it succeeded.
 */
void Request(std::string method, std::string path, Json body, std::function<void(Json, bool)> callback);

/**
 * The backend's acknowledgement of one delivered result.
 *
 * The backend applies a result and answers `{ok, applied, request_id, status}`. HTTP 2xx alone is not
 * a confirmation that the result was applied, so a request that must only take effect after the
 * backend accepted it (a savegame load) waits for this acknowledgement.
 */
struct ResultAck {
	bool delivered = false; ///< The answer belongs to the delivered request and the backend took it.
	bool applied = false;   ///< The backend applied the result; a duplicate delivery counts as applied.
};

/**
 * Read the backend's acknowledgement of a result delivery.
 * @param body The parsed answer body.
 * @param request_id The request whose result was delivered.
 * @return The acknowledgement.
 */
ResultAck ParseResultAck(const Json &body, std::string_view request_id);

/**
 * Whether an armed savegame load still belongs to the current world.
 * @param load_world The world the load was armed in.
 * @param load_epoch The epoch the load was armed in.
 * @param load_company The company of the request that armed it, or CompanyID::Invalid().
 * @param world The current world id.
 * @param epoch The current epoch.
 * @param local_company The current local company.
 * @return True iff the load may still start.
 */
bool PendingLoadStillCurrent(std::string_view load_world, std::string_view load_epoch, CompanyID load_company,
		std::string_view world, std::string_view epoch, CompanyID local_company);

/**
 * Whether a task barrier of a task cancels the pending load of that task.
 * @param load_task The task that armed the load.
 * @param barrier_task The task whose barrier was received.
 * @return True iff the pending load is cancelled.
 */
bool PendingLoadCancelledByBarrier(std::string_view load_task, std::string_view barrier_task);

/**
 * Whether a second savegame load has to be refused because one is already waiting.
 * @param method The method of the request.
 * @param op The operation of the request.
 * @param pending_load_armed Whether a load is already waiting for its acknowledgement.
 * @return True iff the request must be refused before it is executed.
 */
bool SavegameLoadRefused(std::string_view method, std::string_view op, bool pending_load_armed);

/**
 * Whether a savegame load is waiting for the backend's acknowledgement of its result.
 * @return True iff a load is pending.
 */
bool HasPendingLoad();

/**
 * Arm a pending load, for the state tests.
 * @param request_id The request whose acknowledgement starts the load.
 * @param file The savegame file the load reads.
 * @param task_id The task of the request.
 * @param actor The company the request acts for.
 * @param player The local player at the time the load is armed.
 */
void ArmPendingLoadForTest(std::string request_id, std::string file, std::string task_id, CompanyID actor, CompanyID player);

/**
 * The local player captured when the pending load was armed.
 * @return The company the load is guarded against, or CompanyID::Invalid() when nothing is armed.
 */
CompanyID PendingLoadPlayer();

/*
 * Receipt cache seams.
 *
 * A stored receipt answers a replay of its request, and a request that waits for the draw tick holds
 * a receipt slot from the moment it is scheduled. The cache is bounded by MAX_RECEIPTS and only
 * acknowledged receipts may be forgotten, so the rules are only observable with a backend and a
 * video driver; these seams expose exactly the rules to the tests.
 */

/**
 * Store an unacknowledged receipt, as an executed request does.
 * @param request_id The request the receipt belongs to.
 * @param result The result the receipt holds.
 */
void StoreReceiptForTest(std::string request_id, const Json &result);

/**
 * Mark a receipt as acknowledged by the backend, so it may be forgotten.
 * @param request_id The request whose receipt is acknowledged.
 */
void AcknowledgeReceiptForTest(const std::string &request_id);

/**
 * Reserve the receipt slot of a request that waits for the draw tick, as ScheduleDeferred does.
 * @param request_id The request that reserves the slot.
 * @return True iff the slot was reserved; false means the caller would answer `busy`.
 */
bool ReserveDeferredForTest(std::string request_id);

/**
 * The number of receipts the cache holds.
 * @return The number of receipts.
 */
size_t ReceiptCountForTest();

/**
 * The number of requests that reserved a receipt slot for the draw tick.
 * @return The number of reservations.
 */
size_t DeferredReservationCountForTest();

/**
 * Whether a replay of the request would be answered from a stored receipt.
 * @param request_id The request to look up.
 * @return True iff a receipt is stored for it.
 */
bool HasReceiptForTest(const std::string &request_id);

/** Empty the receipt cache and the draw-tick reservations. */
void ClearReceiptCacheForTest();

/*
 * Query seam. world.query is answered inside the request execution, which needs a backend and a
 * poll; this seam answers one query directly, so the kinds the kernel iterates are testable.
 */

/**
 * Answer one `world.query` request, exactly as the bridge answers it for the backend.
 * @param params The parameters of the query.
 * @return The query result.
 */
Json QueryWorldForTest(const Json &params);

/**
 * Answer one `world.pause` request, exactly as the bridge answers it for the backend.
 * @param params The parameters of the request.
 * @return The result of the request.
 */
Json PauseForTest(const Json &params);

} // namespace InGameAgent

#endif /* AGENT_BRIDGE_H */
