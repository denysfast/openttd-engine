/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/>.
 */

/**
 * @file test_agent_blueprint_tools.cpp Test the dispatch, the argument validation, the bounded plan
 * store and the honest chunk accounting of the agent blueprint and trunk line tools.
 *
 * No game is running in this test binary, so the map is empty: every call that takes a tile is only
 * validated here, and the one command a case really posts fails the way the engine fails without a
 * map. That is exactly what the last case checks: a posted chunk that the engine did not build is
 * not reported as done.
 */

#include "../stdafx.h"

#include "../3rdparty/catch2/catch.hpp"

#include "../agent_blueprint_tools.h"
#include "../blueprint.h"
#include "../core/backup_type.hpp"
#include "../fileio_func.h"
#include "../map_func.h"

#include "../safeguards.h"

using namespace InGameAgent;

/**
 * Is a value present in a JSON array of strings?
 * @param array The array to search.
 * @param value The value to look for.
 * @return True when the array holds the value.
 */
static bool Contains(const Json &array, std::string_view value)
{
	for (const Json &item : array) {
		if (item.is_string() && item.get<std::string>() == value) return true;
	}
	return false;
}

/**
 * Find a capability entry by method name.
 * @param capabilities The capability document.
 * @param method The method to look for.
 * @return The entry, or an empty object when the method is not listed.
 */
static Json FindCapability(const Json &capabilities, std::string_view method)
{
	for (const Json &entry : capabilities["methods"]) {
		if (entry["method"].is_string() && entry["method"].get<std::string>() == method) return entry;
	}
	return Json::object();
}

/**
 * Dispatch a call of the module and require that the module owns the method.
 * @param method The method name.
 * @param params The parameters.
 * @return The result document.
 */
static Json Dispatch(std::string_view method, Json params)
{
	std::optional<Json> result = DispatchBlueprintTool(method, params, CompanyID::Invalid());
	REQUIRE(result.has_value());
	return *result;
}

/**
 * Build the binding fields of a call.
 * @param plan_id The plan id.
 * @param task_id The task id.
 * @param world_id The world id.
 * @param world_epoch The world epoch.
 * @return The parameter object of the call.
 */
static Json Binding(uint32_t plan_id, std::string_view task_id = "task-1", std::string_view world_id = "world-1", uint64_t world_epoch = 7)
{
	return Json{{"plan_id", plan_id}, {"task_id", task_id}, {"world_id", world_id}, {"world_epoch", world_epoch}};
}

/**
 * Build one synthetic chunk that builds a single straight track piece.
 * @return The chunk.
 */
static BlueprintChunkData TrackChunk()
{
	BlueprintChunkData chunk;
	/* Kind Track, x 0, y 0, rail type 0, TrackBits X. */
	chunk.data = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01};
	return chunk;
}

/**
 * Build a small plan with a number of synthetic chunks.
 * @param chunks The number of chunks.
 * @return The plan.
 */
static BlueprintToolPlan MakePlan(size_t chunks)
{
	BlueprintToolPlan plan;
	plan.route = false;
	plan.name = "synthetic";
	plan.origin = TileXY(0, 0);
	plan.company = static_cast<CompanyID>(0);
	plan.task_id = "task-1";
	plan.world_id = "world-1";
	plan.world_epoch = 7;
	plan.prepare_count = 0;
	for (size_t i = 0; i < chunks; i++) plan.chunks.push_back(TrackChunk());
	return plan;
}

TEST_CASE("Agent blueprint tools: capabilities")
{
	const Json capabilities = BlueprintToolCapabilities();
	REQUIRE(capabilities["ok"] == true);

	const Json blueprint = FindCapability(capabilities, "world.blueprint");
	REQUIRE(!blueprint.empty());
	for (const char *op : {"list", "inspect", "capture", "plan", "status", "execute_chunk", "stop", "drop"}) {
		CHECK(Contains(blueprint["implemented"], op));
	}
	/* The operations that are really missing are still listed as gaps. */
	CHECK(Contains(blueprint["gaps"], "rename"));
	CHECK(Contains(blueprint["gaps"], "delete"));

	const Json route = FindCapability(capabilities, "world.rail_route");
	REQUIRE(!route.empty());
	for (const char *op : {"plan", "status", "execute_chunk", "stop", "drop"}) {
		CHECK(Contains(route["implemented"], op));
	}

	/* The query operations are listed apart from the write operations. */
	REQUIRE(capabilities["query_ops"].is_array());
	CHECK(Contains(capabilities["query_ops"], "world.blueprint op=list"));
	CHECK(Contains(capabilities["query_ops"], "world.blueprint op=inspect"));
	CHECK(Contains(capabilities["query_ops"], "world.blueprint op=status"));
	CHECK(Contains(capabilities["query_ops"], "world.rail_route op=status"));

	/* The bounds are published, so the bridge can plan around them. */
	const Json bounds = capabilities["bounds"];
	CHECK(bounds["capture_tiles"] == 8192);
	CHECK(bounds["plans"] == 8);
	CHECK(bounds["plan_chunks"] == 1024);
	CHECK(bounds["plan_bytes"] == 512 * 1024);
	CHECK(bounds["plan_store_bytes"] == 2 * 1024 * 1024);
	CHECK(bounds["list_limit"] == 256);
	CHECK(bounds["item_limit"] == 512);
	CHECK(bounds["name_bytes"] == 48);
	CHECK(!capabilities["notes"].empty());

	/* Methods of the other modules are not owned by this one. */
	CHECK_FALSE(DispatchBlueprintTool("world.construct", Json::object(), CompanyID::Invalid()).has_value());
	CHECK_FALSE(DispatchBlueprintTool("world.query", Json::object(), CompanyID::Invalid()).has_value());
}

TEST_CASE("Agent blueprint tools: mutation classification")
{
	/* The read-only operations do not change the world. */
	CHECK_FALSE(IsBlueprintMutation("world.blueprint", Json::object({{"op", "list"}})));
	CHECK_FALSE(IsBlueprintMutation("world.blueprint", Json::object({{"op", "inspect"}})));
	CHECK_FALSE(IsBlueprintMutation("world.blueprint", Json::object({{"op", "status"}})));
	CHECK_FALSE(IsBlueprintMutation("world.rail_route", Json::object({{"op", "status"}})));

	/* Capturing, planning and the plan store changes are reported as mutations. */
	CHECK(IsBlueprintMutation("world.blueprint", Json::object({{"op", "capture"}})));
	CHECK(IsBlueprintMutation("world.blueprint", Json::object({{"op", "plan"}})));
	CHECK(IsBlueprintMutation("world.blueprint", Json::object({{"op", "execute_chunk"}})));
	CHECK(IsBlueprintMutation("world.blueprint", Json::object({{"op", "stop"}})));
	CHECK(IsBlueprintMutation("world.blueprint", Json::object({{"op", "drop"}})));
	CHECK(IsBlueprintMutation("world.rail_route", Json::object({{"op", "plan"}})));
	CHECK(IsBlueprintMutation("world.rail_route", Json::object({{"op", "execute_chunk"}})));

	/* An unknown operation and a call without an operation are treated as mutations. */
	CHECK(IsBlueprintMutation("world.blueprint", Json::object({{"op", "no_such_op"}})));
	CHECK(IsBlueprintMutation("world.blueprint", Json::object()));
	CHECK(IsBlueprintMutation("world.rail_route", Json::object()));

	/* The methods of the other modules are not mutations of this one. */
	CHECK_FALSE(IsBlueprintMutation("world.construct", Json::object()));
	CHECK_FALSE(IsBlueprintMutation("world.towns", Json::object({{"op", "found"}})));
}

TEST_CASE("Agent blueprint tools: dispatch and name validation")
{
	/* An unknown operation of an owned method is a reported gap. */
	const Json gap = Dispatch("world.blueprint", Json::object({{"op", "window"}}));
	CHECK(gap["ok"] == false);
	CHECK(gap["error"] == "unsupported_op");
	CHECK(gap["detail"]["op"] == "window");

	CHECK(Dispatch("world.rail_route", Json::object({{"op", "preview_tiles"}}))["error"] == "unsupported_op");
	CHECK(Dispatch("world.blueprint", Json::object())["error"] == "invalid_arguments");
	CHECK(Dispatch("world.blueprint", Json::object({{"op", 7}}))["error"] == "invalid_arguments");

	/* A blueprint name is exactly one file name in the native blueprint directory. */
	CHECK(Dispatch("world.blueprint", Json::object({{"op", "inspect"}}))["error"] == "invalid_arguments");
	CHECK(Dispatch("world.blueprint", Json::object({{"op", "inspect"}, {"name", ""}}))["error"] == "invalid_arguments");
	CHECK(Dispatch("world.blueprint", Json::object({{"op", "inspect"}, {"name", "a/b"}}))["error"] == "invalid_arguments");
	CHECK(Dispatch("world.blueprint", Json::object({{"op", "inspect"}, {"name", "../escape"}}))["error"] == "invalid_arguments");
	CHECK(Dispatch("world.blueprint", Json::object({{"op", "inspect"}, {"name", "with:colon"}}))["error"] == "invalid_arguments");
	CHECK(Dispatch("world.blueprint", Json::object({{"op", "inspect"}, {"name", "trailing "}}))["error"] == "invalid_arguments");
	CHECK(Dispatch("world.blueprint", Json::object({{"op", "inspect"}, {"name", std::string(60, 'x')}}))["error"] == "invalid_arguments");
	CHECK(Dispatch("world.blueprint", Json::object({{"op", "inspect"}, {"name", 7}}))["error"] == "invalid_arguments");
	const Json missing = Dispatch("world.blueprint", Json::object({{"op", "inspect"}, {"name", "agent_tools_no_such_blueprint"}}));
	CHECK(missing["ok"] == false);
	CHECK(missing["error"] == "unknown_blueprint");
}

TEST_CASE("Agent blueprint tools: blueprint file round trip")
{
	/* Blueprints live in the blueprint directory of the personal directory; use a scratch one for this case. */
	std::error_code error_code;
	const std::string scratch = (std::filesystem::temp_directory_path(error_code) / "openttd_agent_blueprint_test").string() + "/";
	std::filesystem::create_directories(scratch, error_code);
	const AutoRestoreBackup<std::string> personal_dir(_personal_dir, scratch);

	/* Save a blueprint with the engine's own writer, then read it back through the module. */
	const std::string name = "agent_tools_test_blueprint";
	Blueprint bp;
	bp.name = name;
	bp.width = 3;
	bp.height = 1;
	bp.railtypes.push_back(RailTypeLabel{"RAIL"});
	bp.tracks.push_back({0, 0, 0, TrackBits{Track::X}});
	bp.tracks.push_back({1, 0, 0, TrackBits{Track::Y}});
	bp.signals.push_back({0, 0, Track::X, SignalType::Block, SignalVariant::Electric, 3});
	bp.depots.push_back({2, 0, 0, DiagDirection::NE});
	bp.tunnelbridges.push_back({0, 0, 1, 0, 0, true, 0});
	bp.stations.push_back({2, 0, 0, 0, Axis::X, false, UINT16_MAX});
	REQUIRE(SaveBlueprint(bp));

	/* The listing finds it. */
	const Json list = Dispatch("world.blueprint", Json::object({{"op", "list"}, {"limit", 256}}));
	REQUIRE(list["ok"] == true);
	REQUIRE(list["total"].is_number_integer());
	bool listed = false;
	for (const Json &entry : list["blueprints"]) {
		if (entry["name"] == name) listed = true;
	}
	CHECK(listed);

	/* Inspecting it reports the real content, bounded. */
	const Json info = Dispatch("world.blueprint", Json::object({{"op", "inspect"}, {"name", name}, {"limit", 2}}));
	REQUIRE(info["ok"] == true);
	CHECK(info["name"] == name);
	CHECK(info["width"] == 3);
	CHECK(info["height"] == 1);
	CHECK(info["empty"] == false);
	CHECK(info["items"].is_array());
	CHECK(info["counts"]["tracks"] == 2);
	CHECK(info["counts"]["signals"] == 1);
	CHECK(info["counts"]["depots"] == 1);
	CHECK(info["counts"]["tunnelbridges"] == 1);
	CHECK(info["counts"]["stations"] == 1);
	CHECK(info["railtypes"][0] == "RAIL");
	CHECK(info["items_total"] == 6);
	CHECK(info["items"].size() == 2);
	CHECK(info["items"][0]["kind"] == "track");
	CHECK(info["items"][0]["railtype"] == "RAIL");
	CHECK(info["limit"] == 2);

	/* The window is applied over the whole item stream, kind by kind. */
	const Json tail = Dispatch("world.blueprint", Json::object({{"op", "inspect"}, {"name", name}, {"offset", 5}, {"limit", 8}}));
	REQUIRE(tail["ok"] == true);
	REQUIRE(tail["items"].size() == 1);
	CHECK(tail["items"][0]["kind"] == "station");
	CHECK(tail["offset"] == 5);

	/* An out of range window is refused. */
	CHECK(Dispatch("world.blueprint", Json::object({{"op", "inspect"}, {"name", name}, {"limit", 0}}))["error"] == "invalid_arguments");
	CHECK(Dispatch("world.blueprint", Json::object({{"op", "inspect"}, {"name", name}, {"offset", -1}}))["error"] == "invalid_arguments");

	REQUIRE(DeleteBlueprint(name));
	CHECK(Dispatch("world.blueprint", Json::object({{"op", "inspect"}, {"name", name}}))["error"] == "unknown_blueprint");
}

TEST_CASE("Agent blueprint tools: plan validation")
{
	/* The blueprint arguments are validated before the map is touched. */
	CHECK(Dispatch("world.blueprint", Json::object({{"op", "plan"}}))["error"] == "invalid_arguments");
	const Json bad_rotation = Dispatch("world.blueprint", Json::object({{"op", "plan"}, {"name", "agent_tools_test_blueprint"},
			{"rotation", 5}, {"task_id", "task-1"}, {"world_id", "world-1"}, {"world_epoch", 7}}));
	CHECK(bad_rotation["ok"] == false);
	CHECK(bad_rotation["error"] == "invalid_arguments");
	CHECK(Dispatch("world.blueprint", Json::object({{"op", "plan"}, {"name", "agent_tools_test_blueprint"},
			{"mirror", "yes"}, {"task_id", "task-1"}, {"world_id", "world-1"}, {"world_epoch", 7}}))["error"] == "invalid_arguments");
	CHECK(Dispatch("world.blueprint", Json::object({{"op", "plan"}, {"name", "agent_tools_test_blueprint"},
			{"railtype", "ZZZZ"}, {"task_id", "task-1"}, {"world_id", "world-1"}, {"world_epoch", 7}}))["error"] == "unknown_railtype");
	/* The binding is required: a plan belongs to one task on one world epoch. */
	CHECK(Dispatch("world.blueprint", Json::object({{"op", "plan"}, {"name", "agent_tools_test_blueprint"}}))["error"] == "invalid_arguments");
	CHECK(Dispatch("world.blueprint", Json::object({{"op", "plan"}, {"name", "agent_tools_test_blueprint"},
			{"task_id", "task-1"}, {"world_id", "world-1"}, {"world_epoch", -1}}))["error"] == "invalid_arguments");
	/* Without a map there is no tile the blueprint could be pasted on. */
	CHECK(Dispatch("world.blueprint", Json::object({{"op", "plan"}, {"name", "agent_tools_test_blueprint"},
			{"origin", Json{{"x", 0}, {"y", 0}}}, {"task_id", "task-1"}, {"world_id", "world-1"}, {"world_epoch", 7}}))["error"] == "invalid_arguments");

	/* The trunk line arguments are validated before the map is touched too. */
	CHECK(Dispatch("world.rail_route", Json::object({{"op", "plan"}}))["error"] == "invalid_arguments");
	CHECK(Dispatch("world.rail_route", Json::object({{"op", "plan"}, {"tracks", 0}}))["error"] == "invalid_arguments");
	CHECK(Dispatch("world.rail_route", Json::object({{"op", "plan"}, {"tracks", 31}}))["error"] == "invalid_arguments");
	CHECK(Dispatch("world.rail_route", Json::object({{"op", "plan"}, {"tracks", 2}, {"start", "diagonal"}}))["error"] == "invalid_arguments");
	CHECK(Dispatch("world.rail_route", Json::object({{"op", "plan"}, {"tracks", 2}, {"signal_type", 9}}))["error"] == "invalid_arguments");
	CHECK(Dispatch("world.rail_route", Json::object({{"op", "plan"}, {"tracks", 2}, {"signal_interval", 0}}))["error"] == "invalid_arguments");
	const Json no_binding = Dispatch("world.rail_route", Json::object({{"op", "plan"}, {"tracks", 2}, {"signal_interval", 6}}));
	CHECK(no_binding["ok"] == false);
	CHECK(no_binding["error"] == "invalid_arguments");
}

TEST_CASE("Agent blueprint tools: bounded plan store")
{
	/* The store keeps a bounded number of plans and evicts the oldest one. */
	std::vector<uint32_t> ids;
	for (size_t i = 0; i < 9; i++) {
		const std::optional<uint32_t> id = StoreBlueprintToolPlan(MakePlan(1));
		REQUIRE(id.has_value());
		ids.push_back(*id);
	}
	CHECK(ids[0] != ids[8]);
	CHECK(Dispatch("world.blueprint", Json::object({{"op", "status"}, {"plan_id", ids[0]}}))["error"] == "unknown_plan");
	const Json newest = Dispatch("world.blueprint", Json::object({{"op", "status"}, {"plan_id", ids[8]}}));
	REQUIRE(newest["ok"] == true);
	CHECK(newest["kind"] == "blueprint");
	CHECK(newest["chunks"]["total"] == 1);
	CHECK(newest["next_chunk_index"] == 0);
	CHECK(newest["done"] == false);
	CHECK(newest["binding"]["task_id"] == "task-1");
	CHECK(newest["binding"]["world_epoch"] == 7);

	/* An empty plan and a plan of too many chunks are refused instead of truncated. */
	CHECK_FALSE(StoreBlueprintToolPlan(MakePlan(0)).has_value());
	BlueprintToolPlan big = MakePlan(1025);
	CHECK_FALSE(StoreBlueprintToolPlan(std::move(big)).has_value());

	/* Clean up: drop every plan this case made. */
	for (const uint32_t id : ids) {
		Dispatch("world.blueprint", Json::object({{"op", "drop"}, {"plan_id", id}, {"task_id", "task-1"}, {"world_id", "world-1"}, {"world_epoch", 7}}));
	}
}

TEST_CASE("Agent blueprint tools: chunk execution accounting")
{
	const std::optional<uint32_t> stored = StoreBlueprintToolPlan(MakePlan(2));
	REQUIRE(stored.has_value());
	const uint32_t plan_id = *stored;

	/* A plan can only be executed by the task, world and epoch it was made for. */
	CHECK(Dispatch("world.blueprint", Json::object({{"op", "execute_chunk"}, {"plan_id", plan_id}, {"chunk_index", 0}}))["error"] == "invalid_arguments");
	const Json other_task = Dispatch("world.blueprint", Json::object({{"op", "execute_chunk"}, {"plan_id", plan_id},
			{"chunk_index", 0}, {"task_id", "task-2"}, {"world_id", "world-1"}, {"world_epoch", 7}}));
	CHECK(other_task["ok"] == false);
	CHECK(other_task["error"] == "plan_binding_mismatch");
	const Json other_epoch = Dispatch("world.blueprint", Json::object({{"op", "execute_chunk"}, {"plan_id", plan_id},
			{"chunk_index", 0}, {"task_id", "task-1"}, {"world_id", "world-1"}, {"world_epoch", 8}}));
	CHECK(other_epoch["error"] == "plan_binding_mismatch");
	CHECK(Dispatch("world.blueprint", Json::object({{"op", "execute_chunk"}, {"plan_id", 999999},
			{"chunk_index", 0}, {"task_id", "task-1"}, {"world_id", "world-1"}, {"world_epoch", 7}}))["error"] == "unknown_plan");

	/* Chunks are executed in order and inside the plan. */
	Json params = Binding(plan_id);
	params["op"] = "execute_chunk";
	params["chunk_index"] = 1;
	const Json out_of_order = Dispatch("world.blueprint", params);
	CHECK(out_of_order["ok"] == false);
	CHECK(out_of_order["error"] == "out_of_order");
	CHECK(out_of_order["detail"]["next_chunk_index"] == 0);
	params["chunk_index"] = 5;
	CHECK(Dispatch("world.blueprint", params)["error"] == "invalid_chunk_index");

	/* One chunk per call: the engine really runs it, and without a map it builds nothing. */
	params["chunk_index"] = 0;
	const Json chunk = Dispatch("world.blueprint", params);
	REQUIRE(chunk["ok"] == true);
	CHECK(chunk["plan_id"] == plan_id);
	CHECK(chunk["chunk_index"] == 0);
	CHECK(chunk["chunk_kind"] == "build");
	CHECK(chunk["chunk_bytes"] == 7);
	/* Nothing was built, so the chunk is not counted as done: submitted work is never called done. */
	CHECK(chunk["executed"] == false);
	CHECK(chunk["submitted"] == false);
	CHECK(chunk["command_failed"] == true);
	CHECK(chunk["next_chunk_index"] == 0);
	CHECK(chunk["chunks_done"] == 0);
	CHECK(chunk["done"] == false);
	CHECK(chunk["cost"] == 0);
	CHECK(chunk["built"] == 0);

	/* The status reports the same state. */
	const Json status = Dispatch("world.blueprint", Json::object({{"op", "status"}, {"plan_id", plan_id}}));
	REQUIRE(status["ok"] == true);
	CHECK(status["next_chunk_index"] == 0);
	CHECK(status["done"] == false);
	CHECK(status["stopped"] == false);

	/* Stopping drops the remaining chunks and no chunk can run afterwards. */
	const Json stopped = Dispatch("world.blueprint", Json::object({{"op", "stop"}, {"plan_id", plan_id},
			{"task_id", "task-1"}, {"world_id", "world-1"}, {"world_epoch", 7}}));
	REQUIRE(stopped["ok"] == true);
	CHECK(stopped["stopped"] == true);
	CHECK(stopped["done"] == true);
	CHECK(stopped["dropped_chunks"] == 2);
	CHECK(stopped["chunks"]["total"] == 0);
	CHECK(Dispatch("world.blueprint", params)["error"] == "plan_stopped");

	/* Dropping removes the plan; a stopped plan can still be dropped. */
	const Json dropped = Dispatch("world.blueprint", Json::object({{"op", "drop"}, {"plan_id", plan_id},
			{"task_id", "task-1"}, {"world_id", "world-1"}, {"world_epoch", 7}}));
	REQUIRE(dropped["ok"] == true);
	CHECK(dropped["dropped"] == true);
	CHECK(Dispatch("world.blueprint", Json::object({{"op", "status"}, {"plan_id", plan_id}}))["error"] == "unknown_plan");

	/* Stopping and dropping belong to the owning task. */
	const std::optional<uint32_t> owned = StoreBlueprintToolPlan(MakePlan(1));
	REQUIRE(owned.has_value());
	CHECK(Dispatch("world.blueprint", Json::object({{"op", "stop"}, {"plan_id", *owned},
			{"task_id", "task-2"}, {"world_id", "world-1"}, {"world_epoch", 7}}))["error"] == "plan_binding_mismatch");
	CHECK(Dispatch("world.blueprint", Json::object({{"op", "drop"}, {"plan_id", *owned},
			{"task_id", "task-1"}, {"world_id", "world-1"}, {"world_epoch", 7}}))["ok"] == true);
}

TEST_CASE("Agent blueprint tools: capture validation")
{
	/* A capture needs both corners inside the map. */
	CHECK(Dispatch("world.blueprint", Json::object({{"op", "capture"}}))["error"] == "invalid_arguments");
	CHECK(Dispatch("world.blueprint", Json::object({{"op", "capture"}, {"corner1", Json{{"x", 0}, {"y", 0}}}}))["error"] == "invalid_arguments");
	/* The test binary has no map, so no tile is inside it. */
	const Json no_map = Dispatch("world.blueprint", Json::object({{"op", "capture"},
			{"corner1", Json{{"x", 0}, {"y", 0}}}, {"corner2", Json{{"x", 1}, {"y", 1}}}}));
	CHECK(no_map["ok"] == false);
	CHECK(no_map["error"] == "invalid_arguments");
}
