/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/>.
 */

/** @file agent_admin_tools.h Administrative and game-settings tool methods the agent bridge dispatches onto real commands. */

#ifndef AGENT_ADMIN_TOOLS_H
#define AGENT_ADMIN_TOOLS_H

#include "3rdparty/nlohmann/json.hpp"
#include "command_type.h"
#include "company_type.h"
#include "tile_type.h"

#include <optional>
#include <string_view>

/** Administrative and game-settings tool methods the agent bridge dispatches onto real OpenTTD commands. */
namespace InGameAgent {

/** JSON value type used by the agent tool protocol. */
using Json = nlohmann::json;

/**
 * Describe the administrative tool methods this module implements, and the parts it does not.
 *
 * The bridge uses this to advertise capabilities and to answer a call for an operation that has no
 * implementation yet with an explicit gap instead of a fake success.
 *
 * @return Capability document: implemented methods/ops plus the known gaps.
 */
Json AdminToolCapabilities();

/**
 * Dispatch one administrative tool call onto the engine.
 *
 * The bridge validates the world, the epoch, the task and the expected references before it calls
 * this; the module validates the ids, the enums and the bounds of the call itself before it posts
 * anything. Every mutation goes through Command<Commands::...>::Post, so the engine's own
 * eligibility, pause and finance checks apply and the reported cost is the real one.
 *
 * @param method The method name, e.g. "world.settings.set".
 * @param params The method parameters.
 * @param company The company the call is made for; used as the default company of the call.
 * @return The result document, or std::nullopt when the method does not belong to this module.
 */
std::optional<Json> DispatchAdminTool(std::string_view method, const Json &params, CompanyID company);

/**
 * Does this call change the world?
 *
 * The bridge needs this to decide whether a call needs a task and expected references. Read-only
 * operations of the administrative methods (world.company op=info, world.towns op=needs,
 * world.project op=power/district) are not mutations.
 *
 * @param method The method name.
 * @param params The method parameters.
 * @return True when the call is expected to change the world.
 */
bool IsAdminMutation(std::string_view method, const Json &params);

/**
 * Callback of every command this module posts.
 *
 * The command path is synchronous in single player, so the result is available right after Post
 * returns. The callback is registered in the network command callback table.
 *
 * @param cmd The command that ran.
 * @param result The result the engine reported.
 * @param tile The tile the command ran on.
 */
void AgentAdminCommandCallback(Commands cmd, const CommandCost &result, TileIndex tile);

} // namespace InGameAgent

#endif /* AGENT_ADMIN_TOOLS_H */
