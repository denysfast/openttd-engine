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
#include "group_type.h"
#include "tile_type.h"
#include "vehicle_type.h"

#include <optional>
#include <string_view>

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

} // namespace InGameAgent

#endif /* AGENT_GAME_TOOLS_H */
