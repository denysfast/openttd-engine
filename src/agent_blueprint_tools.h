/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/>.
 */

/**
 * @file agent_blueprint_tools.h Rail blueprint and trunk line tool methods the agent bridge dispatches onto the
 * engine's own blueprint and rail route planners and onto the normal PasteBlueprint and PrepareRailRoute commands.
 */

#ifndef AGENT_BLUEPRINT_TOOLS_H
#define AGENT_BLUEPRINT_TOOLS_H

#include "3rdparty/nlohmann/json.hpp"
#include "blueprint.h"
#include "command_type.h"
#include "company_type.h"
#include "economy_type.h"
#include "tile_type.h"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

/** Rail blueprint and trunk line tool methods of the in-game agent. */
namespace InGameAgent {

/** JSON value type used by the agent tool protocol. */
using Json = nlohmann::json;

/**
 * One engine-owned plan of blueprint or trunk line chunks.
 *
 * A plan is the bridge's handle on a piece of work that is built one chunk per call. The module owns
 * the record: it hands out the id, keeps the chunks, and counts the results of the commands it posts.
 * The plan is bound to the task, the world and the world epoch of the call that made it, so a chunk
 * can only be executed by the same task on the same world epoch; the bridge checks the epoch and the
 * task barrier before every call, and this module refuses a plan whose binding does not match.
 *
 * A plan never runs on its own: there is no timer and no background job, so nothing keeps building
 * after the bridge stops calling, and the player's own blueprint paste job is not touched.
 */
struct BlueprintToolPlan {
	uint32_t plan_id = 0; ///< Engine-owned id, assigned by #StoreBlueprintToolPlan and never reused.
	bool route = false; ///< True for a rail trunk line (the first chunks are Commands::PrepareRailRoute).
	std::string name{}; ///< Blueprint name or trunk line label, for messages.
	TileIndex origin = INVALID_TILE; ///< Tile the offsets of every chunk are relative to.
	CompanyID company = CompanyID::Invalid(); ///< Company the chunks are posted for.
	std::string task_id{}; ///< Task the plan is bound to.
	std::string world_id{}; ///< World the plan is bound to.
	uint64_t world_epoch = 0; ///< World epoch the plan is bound to.
	size_t prepare_count = 0; ///< The first this-many chunks are Commands::PrepareRailRoute chunks.
	std::vector<BlueprintChunkData> chunks{}; ///< The commands, in execution order.

	/* Execution state, maintained by the module. */
	size_t next_chunk = 0; ///< Index of the next chunk to execute; also the number of executed chunks.
	Money cost = 0; ///< Money the executed chunks really cost.
	uint32_t built = 0; ///< Items the engine reported as built.
	uint32_t failed = 0; ///< Items the engine reported as failed.
	StringID last_error = INVALID_STRING_ID; ///< Last error the engine reported for an item.
	bool out_of_money = false; ///< A chunk stopped because the company ran out of money.
	bool pending = false; ///< A chunk was posted and its result has not come back yet.
	bool stopped = false; ///< The plan was stopped; its remaining chunks were dropped.
};

/**
 * Describe the rail blueprint and trunk line methods this module implements, and the parts it does not.
 * @return Capability document: implemented methods/ops, the query operations and the known gaps.
 */
Json BlueprintToolCapabilities();

/**
 * Dispatch one rail blueprint or trunk line tool call onto the engine.
 *
 * The bridge validates the world epoch, the task barrier and the expected references before it calls
 * this; the module validates the ids, the enums, the names and the bounds of the call itself before it
 * reads or posts anything. Every world change goes through Command<Commands::PasteBlueprint>::Post or
 * Command<Commands::PrepareRailRoute>::Post, so the engine's own eligibility, pause and finance checks
 * apply, and the reported cost is the one the engine returned.
 *
 * @param method The method name, e.g. "world.blueprint".
 * @param params The method parameters.
 * @param company The company the call is made for; used as the default company of the call.
 * @return The result document, or std::nullopt when the method does not belong to this module.
 */
std::optional<Json> DispatchBlueprintTool(std::string_view method, const Json &params, CompanyID company);

/**
 * Does this call change the world?
 *
 * The bridge needs this to decide whether a call needs a task and expected references. The read-only
 * operations are world.blueprint op=list|inspect|status and world.rail_route op=status. Planning and
 * capturing do not change the world either, but they do change this module's plan store, so they are
 * reported as mutations to be safe; only an unknown operation of an owned method is reported as a
 * mutation for the same reason.
 *
 * @param method The method name.
 * @param params The method parameters.
 * @return True when the call is expected to change the world or this module's plan store.
 */
bool IsBlueprintMutation(std::string_view method, const Json &params);

/**
 * Store a plan and give it its engine-owned id.
 *
 * The store is bounded: at most a fixed number of plans, of chunks and of bytes are kept. A plan that
 * is too large, or a store that is full, is refused instead of silently truncated.
 *
 * @param plan The plan; its id is assigned here.
 * @return The plan id, or std::nullopt when the plan does not fit in the store.
 */
std::optional<uint32_t> StoreBlueprintToolPlan(BlueprintToolPlan plan);

/**
 * Callback of Commands::PasteBlueprint and Commands::PrepareRailRoute.
 *
 * Both commands return the cost, the money that was missing, the number of items built and failed, and
 * the last error, so one callback serves both. The parent registers it in the network command callback
 * table next to the engine's own callbacks; the module cannot register it itself without touching the
 * network command file.
 *
 * @param cmd The command that ran.
 * @param result The result the engine reported.
 * @param extra_money Money that was missing to build the next item.
 * @param built Items built by the chunk.
 * @param failed Items the chunk could not build.
 * @param last_error Error of the last failed item.
 */
void AgentBlueprintChunkCallback(Commands cmd, const CommandCost &result, Money extra_money, uint32_t built, uint32_t failed, StringID last_error);

} // namespace InGameAgent

#endif /* AGENT_BLUEPRINT_TOOLS_H */
