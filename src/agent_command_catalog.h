/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/gpl-2.0>.
 */

/** @file agent_command_catalog.h Typed catalog of every engine command and a generic normal-command dispatcher. */

#ifndef AGENT_COMMAND_CATALOG_H
#define AGENT_COMMAND_CATALOG_H

#include "3rdparty/nlohmann/json.hpp"
#include "command_type.h"
#include "company_type.h"
#include "tile_type.h"

#include <optional>
#include <string_view>

/**
 * Catalog of every engine command (`Commands`), plus a generic dispatcher that turns ordered typed
 * JSON arguments into a real `Command<Tcmd>::Post` of the engine's normal player command path.
 *
 * The catalog is generated from the engine itself: it walks `Commands` from 0 to `Commands::End`
 * and reads `CommandTraits<Tcmd>` for each, so the number of entries can never drift from the
 * engine's own command table. Nothing here is a hand written list of commands.
 *
 * ## Methods
 *
 * `world.commands` (read, no company needed):
 *   `{"op": "list"}` - one compact entry per engine command.
 *   `{"op": "describe"}` - every command with its full argument schema.
 *   `{"op": "describe", "command": <name|id>}` - one command with its full argument schema.
 *   `{"op": "validate", "command": <name|id>, "args": [...]}` - decode one command's arguments
 *     without running it; the result reports the arguments as they were decoded. A gap command
 *     answers `unsupported_argument_types`, bad arguments `invalid_arguments`.
 *
 * A command entry looks like:
 * @code{.json}
 * {
 *   "id": 12,                          // numeric Commands value, 0 .. Commands::End - 1
 *   "name": "CmdBuildRoad",            // CommandTraits<Tcmd>::name, i.e. the proc name
 *   "alias": "BuildRoad",              // the same name without the leading "Cmd"
 *   "type": "LandscapeConstruction",   // CommandType
 *   "flags": ["Auto", "NoWater", "Deity"], // CommandFlag names
 *   "location_argument": false,        // CommandFlag::Location: Post needs an explicit tile
 *   "pause": {"allowed_when_paused": true},
 *   "company": {"server_only": false, "spectator_allowed": false, "deity_allowed": true,
 *               "offline_only": false, "needs_client_id": false},
 *   "support": {"supported": true, "status": "dispatchable"},
 *   "arguments": [                     // only for op=describe
 *     {"index": 0, "name": null, "type": "TileIndex", "kind": "strong_id", "supported": true}
 *   ],
 *   "returns": ["Money"]               // only for op=describe; extra typed returns of the proc
 * }
 * @endcode
 *
 * `support.status` is `dispatchable` when the generic decoder can build every argument of the
 * command, and `gap` otherwise; a gap entry carries `support.reason` naming the arguments whose
 * types have no JSON decoding yet.
 *
 * `world.command` (write, needs a valid company):
 *   `{"command": <name|id>, "args": [...], "dry_run": false, "location": <tile>}`
 *   - `command`: proc name (`CmdBuildRoad`), alias (`BuildRoad`) or numeric id, case insensitive.
 *   - `args`: ordered positional arguments; the order and types are the ones `op=describe` reports.
 *   - `dry_run`: estimate only (`DoCommandFlag::QueryCost`, no execution).
 *   - `location`: tile for commands whose `location_argument` is true; defaults to 0 (no location).
 *   Result: `{ok, command, id, dry_run, cost, expense_type, arguments}`; on rejection by the engine
 *   also `error: "command_failed"`, `message` and `message_id` (the engine's own error StringID).
 *   Errors before the command runs: `unknown_command`, `unsupported_argument_types` (a catalog gap),
 *   `invalid_arguments` (wrong arity, wrong type, overflow, bits outside the enum),
 *   `command_not_run` (Post refused before the proc ran, e.g. the game is paused for it),
 *   `invalid_company`, `multiplayer_unsupported`, `invalid_params`.
 *
 * ## Registration
 *
 * The module is registered by the agent bridge owner:
 *   - `src/agent_command_catalog.cpp` in the `src/CMakeLists.txt` source list,
 *   - `src/tests/test_agent_command_catalog.cpp` in `src/tests/CMakeLists.txt`,
 *   - `InGameAgent::AgentCommandCatalogCallback` in the `_callback_tuple` of
 *     `src/network/network_command.cpp` (`Command<Tcmd>::Post` asserts that its callback is
 *     network registered),
 *   - `DispatchCommandCatalog` in the bridge dispatch, `CommandCatalogCapabilities` in the
 *     bridge capability document.
 *
 * `world.command` is already classified as a write by the bridge (`IsMutation`), the catalog reads
 * run in parallel with the other read methods.
 */
namespace InGameAgent {

/** JSON value type used by the agent tool protocol. */
using Json = nlohmann::json;

/**
 * Describe the command catalog and the gaps that are still open.
 *
 * The counts and the list of undecodable argument types are computed from the engine's own command
 * table, so they cannot claim more than `DispatchCommandCatalog` actually accepts.
 *
 * @return Capability document: methods, command counts, supported argument kinds and known gaps.
 */
Json CommandCatalogCapabilities();

/**
 * Dispatch one catalog method.
 *
 * The caller owns authentication, task admission, the world epoch and the task's expected refs;
 * this function only runs the engine command and reports what the engine actually did. Every
 * company context change is scoped to the call and restored afterwards.
 *
 * Writes go through `Command<Tcmd>::Post`, i.e. the normal player command path, so the engine's own
 * pause, finance and eligibility checks apply; that path only exists in single player, so a write
 * in a network game is rejected instead of being executed behind the server's back. A `dry_run`
 * only asks for the cost (`DoCommandFlag::QueryCost`) and never executes.
 *
 * @param method Engine method name: `world.commands` or `world.command`.
 * @param params Method parameters as sent by the backend.
 * @param company Company that pays and owns the result; must be a valid company for `world.command`.
 * @return The method result, or `std::nullopt` when the method is not a catalog method at all.
 */
std::optional<Json> DispatchCommandCatalog(std::string_view method, const Json &params, CompanyID company);

/*
 * Command callback. `Command<Tcmd>::Post` only accepts callbacks that are registered for the
 * network command dispatch, so this one is declared here and listed in network_command.cpp. It has
 * the engine's common callback shape, which the engine also calls for commands whose proc returns
 * more than a `CommandCost`; the created ids of those typed returns are not reported (see the
 * `special_return_values` gap), only the actual `CommandCost` is. In single player the callback runs
 * synchronously, before `Post` returns.
 */

/**
 * Capture the actual cost of the command that was just posted.
 * @param cmd The command that ran.
 * @param result The actual cost, or the failure of the command.
 * @param tile Location the command was posted for.
 */
void AgentCommandCatalogCallback(Commands cmd, const CommandCost &result, TileIndex tile);

} // namespace InGameAgent

#endif /* AGENT_COMMAND_CATALOG_H */
