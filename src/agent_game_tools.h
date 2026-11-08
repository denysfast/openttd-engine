/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/>.
 */

/** @file agent_game_tools.h Game tool methods the agent bridge dispatches onto real commands. */

#ifndef AGENT_GAME_TOOLS_H
#define AGENT_GAME_TOOLS_H

#include "3rdparty/nlohmann/json.hpp"
#include "cargo_type.h"
#include "command_type.h"
#include "company_type.h"
#include "direction_type.h"
#include "group_type.h"
#include "road_type.h"
#include "tile_type.h"
#include "vehicle_type.h"

#include <optional>
#include <string_view>

class Engine;
struct Order;
struct Vehicle;

/** Game tool methods the agent bridge dispatches onto real OpenTTD commands. */
namespace InGameAgent {

/** JSON value type used by the agent tool protocol. */
using Json = nlohmann::json;

/**
 * Describe the game tool methods this module implements, and the families it does not.
 *
 * The bridge uses this to advertise capabilities and to answer a call for a family that has no
 * implementation yet with an explicit gap instead of a fake success.
 *
 * @return Capability document: implemented methods/ops plus the known gaps.
 */
Json GameToolCapabilities();

/**
 * Dispatch one engine method onto the real OpenTTD commands.
 *
 * The caller owns authentication, task admission and the world epoch; this function only runs
 * the command and reports what the engine actually did. Every company context change is scoped
 * to the call and restored afterwards.
 *
 * Actual mutations go through `Command<Tcmd>::Post`, i.e. the normal player command path, so the
 * engine's own eligibility, pause and finance checks apply. That path only exists in single
 * player; a call in a network game is rejected instead of being executed behind the server's back.
 *
 * @param method Engine method name, e.g. `world.construct`.
 * @param params Method parameters as sent by the backend.
 * @param company Company that pays and owns the result; must be a valid company for write methods.
 * @return The tool result, or `std::nullopt` when the method is not a game tool method at all.
 */
std::optional<Json> DispatchGameTool(std::string_view method, const Json &params, CompanyID company);

/**
 * Serialise the wait state of one vehicle row.
 *
 * The counters are the engine's own raw tick counters, never a duration: the engine ticks a vehicle
 * 74 times per game day (`Ticks::DAY_TICKS`), so a value is only comparable with another counter or
 * with a timetable that is in the same unit.
 *  - `current_order_time`: ticks that passed since the current order started
 *    (`BaseConsist::current_order_time`). The field is signed and the engine's own "not known"
 *    marker for it is -1 (`TimerGameTick::Ticks::INVALID_TICKS`), which is reported as it is, so a
 *    negative value means "not known", not "one tick before the order started".
 *  - `load_unload_ticks`: ticks the vehicle still waits before the next step of its
 *    loading/unloading cycle (`Vehicle::load_unload_ticks`).
 *  - `wait_counter`: trains only. One engine field for three situations — ticks waiting in front of
 *    a signal, ticks being stuck, and the counter for forced proceeding through signals
 *    (`Train::wait_counter`) — so it is the raw value and not the time until the train moves again.
 *  - `turn_counter`: aircraft only, the engine's countdown until the next turn is allowed
 *    (`Aircraft::turn_counter`), which throttles turns to at most 45 degrees at once.
 * A counter that belongs to another vehicle type is `null`, never a made up 0, so "this vehicle has
 * no such counter" and "the counter is zero" stay distinguishable.
 *
 * @param v The vehicle (the forward declaration comes from `vehicle_type.h`).
 * @return The wait fields of a vehicle row.
 */
Json VehicleWaitJson(const Vehicle *v);

/*
 * Command callbacks. `Command<Tcmd>::Post` asserts that the callback is registered for the
 * network command dispatch, so these are declared here and must be listed in the
 * `_callback_tuple` of `src/network/network_command.cpp`. That file is a shared anchor owned by
 * the parent bridge integration, so this module does not edit it: the parent has to register
 * every callback declared below. They capture the actual result of the command that was just
 * posted; in single player the callback runs synchronously, before `Post` returns.
 *
 * PARENT REGISTRATION REQUIRED: `AgentRefitVehicleCallback` and `AgentCloneVehicleCallback` are
 * new and are not in the shared tuple yet. Until they are added, `world.vehicles op=refit` and
 * `op=clone` trip the `IsNetworkRegisteredCallback` assertion in debug builds.
 */

/**
 * Capture the cost of a posted command.
 * @param cmd The command that ran.
 * @param result The actual cost, or the failure of the command.
 * @param tile Location the command was posted for.
 */
void AgentCommandCallback(Commands cmd, const CommandCost &result, TileIndex tile);

/**
 * Capture the cost and the created vehicle of a posted `Commands::BuildVehicle`.
 * @param cmd The command that ran.
 * @param result The actual cost, or the failure of the command.
 * @param veh_id The vehicle that was created.
 * @param unit_number Unit number of the created vehicle.
 * @param max_speed Maximum speed of the created vehicle.
 * @param refitted_capacity Capacities of the created vehicle per cargo.
 */
void AgentBuildVehicleCallback(Commands cmd, const CommandCost &result, VehicleID veh_id, uint unit_number, uint16_t max_speed, CargoArray refitted_capacity);

/**
 * Capture the cost and the resulting group of a posted `Commands::AddVehicleToGroup`.
 * @param cmd The command that ran.
 * @param result The actual cost, or the failure of the command.
 * @param group_id The group the vehicle ended up in.
 */
void AgentAddVehicleToGroupCallback(Commands cmd, const CommandCost &result, GroupID group_id);

/**
 * Capture the cost and the resulting capacities of a posted `Commands::RefitVehicle`.
 * @param cmd The command that ran.
 * @param result The actual cost, or the failure of the command.
 * @param refit_capacity Capacity of the refitted consist for the new cargo.
 * @param mail_capacity Secondary (mail) capacity of a refitted aircraft.
 * @param cargo_capacities Capacities of the refitted consist per cargo.
 */
void AgentRefitVehicleCallback(Commands cmd, const CommandCost &result, uint refit_capacity, uint16_t mail_capacity, CargoArray cargo_capacities);

/**
 * Capture the cost and the created vehicle of a posted `Commands::CloneVehicle`.
 * @param cmd The command that ran.
 * @param result The actual cost, or the failure of the command.
 * @param veh_id The vehicle that was created.
 */
void AgentCloneVehicleCallback(Commands cmd, const CommandCost &result, VehicleID veh_id);

/*
 * Road reads.
 *
 * These helpers answer one question the engine answers silently: does a road actually connect where
 * a depot door or a stop entrance is? `CmdBuildRoadDepot` and `CmdBuildRoadStop` accept a tile
 * whose entrance side has no road, and the road pathfinder then turns every vehicle that leaves
 * around on the tile it started from. All facts below come from the engine's own map accessors
 * (`GetRoadDepotDirection`, `GetBayRoadStopDir`, `GetDriveThroughStopAxis`, `GetAnyRoadBits`,
 * `GetTileTrackStatus`, `DiagdirReachesTrackdirs`, `TrackdirToExitdir`) and never from an offset or
 * a remembered build.
 *
 * One connection (a depot exit or a stop entry) is reported as an object with these fields:
 *   `family`             `road` or `tram`: the road or tram family this connection was read for. The
 *                        two are separate track, so they are always read separately.
 *   `direction`          the direction from the reported tile to the neighbour tile, as `ne`/`se`/
 *                        `sw`/`nw` (the engine's `DiagDirection`).
 *   `tile`               the neighbour tile, from the engine's own `TileAddByDiagDir`, or null when
 *                        it is outside the map.
 *   `required_road_bit`  the road or tram piece that neighbour tile must have on the side facing
 *                        the reported tile (`nw`/`sw`/`se`/`ne`), or null when the neighbour is
 *                        outside the map.
 *   `road_bits`          the pieces that neighbour tile really has now, in the engine's piece order.
 *   `has_required_bit`   the raw piece test: is `required_road_bit` among `road_bits`? A one-piece
 *                        stub passes this and still cannot be driven on.
 *   `drivable`           the engine's track test: the tile the vehicle comes from carries its piece
 *                        towards the other one and the tile it drives into offers a track reachable
 *                        when moving in the direction of travel (`GetTileTrackStatus &
 *                        DiagdirReachesTrackdirs`, which `RoadFindPathToDest` applies to the entered
 *                        tile with the direction of motion). For a depot exit the vehicle drives out
 *                        of the depot tile, for a stop entry it drives from the neighbour into the
 *                        stop tile. The entered tile's own one-way state is part of its track status,
 *                        so a one-way tile only lets a vehicle in along its arrow, and for a stop
 *                        entry the neighbour's own one-way state counts as well: an arrow that points
 *                        away from the stop leaves no trackdir of that neighbour exiting towards it.
 *   `connected`          `has_required_bit && drivable`. This is the field to check: a vehicle can
 *                        only get through when it is true. The other fields say why it is not.
 */

/**
 * The entrance of a road or tram depot that is on the map, read from the depot tile itself.
 *
 * The direction is the engine's `GetRoadDepotDirection` and the road/tram family is the one the tile
 * carries, so the answer follows the tile when it is rebuilt or when the road next to it changes.
 * @param depot_tile Tile of the depot.
 * @return The connection object described above, or null when the tile is not a road or tram depot.
 */
Json RoadDepotEntranceJson(TileIndex depot_tile);

/**
 * What a planned depot exit would meet, for a depot that is not on the map yet.
 *
 * A dry run builds nothing, so the depot tile carries no exit piece and no `connected` can be read.
 * This reports only what the neighbour tile offers now, and marks itself `planned` so it is never
 * taken for the current state of a depot: `RoadDepotEntranceJson` is the current answer once the
 * depot is there.
 * @param depot_tile Tile the depot is planned for.
 * @param dir The planned exit direction.
 * @param rtt The planned road or tram family.
 * @return `{family, direction, tile, required_road_bit, road_bits, has_required_bit, planned,
 *         neighbour_drivable}`. `neighbour_drivable` is the engine's track test on the neighbour
 *         tile alone, for a vehicle entering it from the depot side, so a one-piece stub is caught
 *         before the depot exists. A built depot always carries its own exit piece, so the depot is
 *         usable exactly when `has_required_bit` and `neighbour_drivable` are both true.
 */
Json RoadDepotExitProposalJson(TileIndex depot_tile, DiagDirection dir, RoadTramType rtt);

/**
 * The access of one bus or truck stop tile, read from the stop tile itself.
 *
 * A bay stop has exactly one entry, on the side the engine stored with `GetBayRoadStopDir`. A
 * drive-through stop is entered from either end of the axis the engine stored with
 * `GetDriveThroughStopAxis`, so it has two. A multi-tile drive-through platform is reported one
 * tile at a time, exactly as the engine stores it: the entries of the reported tile are its own
 * neighbours.
 *
 * A stop tile can carry a road and a tram road type at once, and those are separate track. Every
 * family the tile carries is read on its own, so a road-only neighbour never answers for tram and the
 * road-only one-way rule never blocks tram track.
 * @param stop_tile Tile of the road stop.
 * @return `{tile, station_id, type, kind, axis?, road?, tram?, entries}` where `type` is `bus` or
 *         `truck`, `kind` is `bay` or `drive_through`, `axis` is `x` or `y` for a drive-through stop
 *         only. `road` and `tram` are present for each family the tile carries, as
 *         `{road_type_id, entries}`, and their `entries` hold the connection objects of that family
 *         alone (one for a bay, two for a drive-through). `entries` is the combined list kept for
 *         existing readers: the road entries first, then the tram entries, each carrying its own
 *         `family`. That label is all the combined list gives: a mixed stop has a road entry and a
 *         tram entry for the same end, so one entry of `entries` is not the answer for the other
 *         family, and a reader that needs the access of one family must take that family's subobject
 *         or select the entries whose `family` it wants. Null when the tile is not a bus or truck
 *         stop.
 */
Json RoadStopAccessJson(TileIndex stop_tile);

/**
 * The road or tram state of one tile, as the engine has it now.
 * @param tile The tile.
 * @param rtt The road or tram family to read.
 * @return `{road_bits, road_type_id, road_type, one_way, disallowed_directions, drivable_exits}`:
 *         the pieces of that family on the tile, the engine's road type id for it and its own
 *         classification of that id (`road`/`tram`, both null when the tile carries none), whether
 *         the tile is a one-way road, the engine's `DisallowedRoadDirection` values that are set
 *         (`northbound`/`southbound`, the engine's own names, not screen directions), and the
 *         diagonal directions a vehicle can actually drive off this tile in. The exits are the
 *         tile's track status mapped through the engine's own `TrackdirToExitdir`, so a curve
 *         reports the two sides it joins and a one-way tile reports only the direction it allows.
 *         The one-way state of the neighbouring tiles is a per-pair test and is not folded in.
 */
Json RoadTileJson(TileIndex tile, RoadTramType rtt);

/**
 * The road type fields of a vehicle row, for the tests.
 *
 * The fields are exactly the ones VehicleRow adds: `road_type_id`, `road_type` and `is_tram`, null
 * when the vehicle has no road type.
 *
 * @param v The vehicle.
 * @return The three fields.
 */
Json VehicleRoadTypeFieldsForTest(const Vehicle *v);

/**
 * The name the game itself shows for an engine.
 *
 * The engine's own custom name only replaces the display name while the engine is enabled, so the
 * purchase list, the vehicle details and the autoreplace window all resolve the name through
 * `STR_ENGINE_NAME`. A tool that reported `Engine::name` reported an empty name for every engine
 * nobody renamed.
 *
 * @param e The engine.
 * @return The display name.
 */
std::string EngineDisplayName(const Engine *e);

/**
 * The engine name fields of a vehicle row, for the tests.
 *
 * The fields are exactly the ones VehicleRow adds: `engine_name` (the engine's display name) and
 * `engine_custom_name` (the player's override, null when there is none).
 *
 * @param v The vehicle.
 * @return The two fields.
 */
Json VehicleEngineNameFieldsForTest(const Vehicle *v);

/**
 * Build one order from the tool's order object, for the tests.
 *
 * The seam exists so a test can check the order the tool actually builds - and the values
 * CmdInsertOrder validates it against - without a running game.
 *
 * @param entry The order object.
 * @param order Receives the order.
 * @return Error result, or an empty optional when the order was built.
 */
std::optional<Json> ReadOrderForTest(const Json &entry, Order &order);

/**
 * The order-list plan of `world.orders` for the tests.
 *
 * For every order of the request it reports the refit that would be encoded on the inserted order
 * (always null, because Commands::InsertOrder only takes the default) and the refit cargo that a
 * following Commands::OrderRefit would apply, or the error that rejects the request before the order
 * list is touched.
 *
 * @param orders The `orders` array of a `world.orders` call.
 * @return `{"ok": true, "orders": [{index, type, load, insert_refit_cargo_id, order_refit_cargo_id}]}`,
 *         or `{"ok": false, "order_index": N, "error": {...}}` when the request is rejected.
 */
Json OrdersPlanForTest(const Json &orders);

} // namespace InGameAgent

#endif /* AGENT_GAME_TOOLS_H */
