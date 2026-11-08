/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <http://www.gnu.org/licenses/>.
 */

/**
 * @file test_agent_bridge_guard.cpp Test the postcondition receipt of a guarded mutation.
 *
 * A guard names the state a command was planned for, and a command that really ran leaves the tile
 * it was guarded with in another state. Only the engine can say which, so the bridge reports the
 * before state it checked next to the state it reads on that tile immediately after the command
 * (`agent_bridge.cpp: GuardPostcondition`). These cases run the real command path on a small
 * allocated map and prove that the reported state is the guard the next command is admitted with,
 * while the guard the first command ran with is stale - and that nothing at all is reported for a
 * command that did not run, for a tile the command did not change, or for a read. The receipt is
 * only issued on the module's own statement of an actual execution, and only for the references a
 * request may carry.
 */

#include "../stdafx.h"

#include "../3rdparty/catch2/catch.hpp"

#include "../agent_bridge.h"
#include "../agent_game_tools.h"
#include "../company_base.h"
#include "../company_func.h"
#include "../map_func.h"
#include "../openttd.h"
#include "../road.h"
#include "../road_map.h"
#include "../road_type.h"
#include "../tile_map.h"
#include "../town_type.h"

#include "../safeguards.h"

using namespace InGameAgent;

namespace {

/** A 64x64 map with the bridge's world gate open; the previous mode is restored at the end. */
struct MapFixture {
	GameMode previous_mode = _game_mode; ///< Mode to restore.

	MapFixture()
	{
		Map::Allocate(64, 64);
		_game_mode = GameMode::Normal;
		/* Without a game the road type masks are empty, so `RoadTypeIsRoad()` answers for nothing and
		 * `MakeRoadNormal()` asserts on the road type it is given. `ResetRoadTypes()` installs the
		 * engine's own road and tram defaults, which is the state a running game has. */
		ResetRoadTypes();
	}

	~MapFixture() { _game_mode = previous_mode; }
};

/** A tile location in the canonical shape. */
Json TilePoint(int x, int y)
{
	return Json{{"x", x}, {"y", y}};
}

/**
 * A real company that can pay for a command, removed again when the fixture goes out of scope.
 *
 * It carries the two things `CmdLandscapeClear` needs before it posts: money to pay for the
 * demolition and a clearing quota (`landscape.cpp: CmdLandscapeClear` rejects a company whose
 * `clear_limit` has no frame left), which a company created outside a running game does not have.
 */
class TestCompany {
public:
	TestCompany()
	{
		REQUIRE(Company::CanAllocateItem());
		this->company = Company::Create();
		REQUIRE(this->company != nullptr);
		this->company->money = 100000;
		this->company->clear_limit = UINT32_MAX;
	}

	~TestCompany() { delete this->company; }

	CompanyID Id() const { return this->company->index; }

private:
	Company *company = nullptr; ///< The company the fixture created.
};

/**
 * The reference a caller keeps for the object the tile carries now.
 *
 * It is the read the agent does: `{type: "tile", tile}`, and the answer is the row the caller then
 * hands back as the guard of a write.
 */
Json ReadTileRef(TileIndex tile)
{
	const Json read = ResolveObjectRef(Json{{"type", "tile"}, {"tile", TilePoint(TileX(tile), TileY(tile))}}, false);
	REQUIRE(read["found"] == true);
	Json ref = {{"type", read["type"]}, {"tile", read["tile"]}, {"fingerprint", read["fingerprint"]}};
	if (read.contains("what") && read["what"].is_string()) ref["what"] = read["what"];
	return ref;
}

/** The request the backend sends for one `world.construct` call. */
Json ConstructRequest(std::string_view request_id, const Json &params, std::string_view task_id = "task-1")
{
	return {
		{"request_id", std::string(request_id)},
		{"task_id", std::string(task_id)},
		{"world_id", "test-world"},
		{"epoch", "test-epoch"},
		{"method", "world.construct"},
		{"params", params},
	};
}

/** The bridge shape of an executed tool module result. */
Json BridgeResult(const Json &module_result)
{
	return {{"ok", module_result.value("ok", false)}, {"value", module_result}};
}

/** The parameters of one `world.construct op=demolish` call on one tile. */
Json DemolishParams(int x, int y, const Json &guard)
{
	Json refs = Json::array();
	refs.push_back(guard);
	return {{"op", "demolish"}, {"tiles", Json::array({TilePoint(x, y)})}, {"expected_refs", refs}};
}

} // namespace

TEST_CASE("Agent bridge guard: a demolition reports the state it left, which is the next guard")
{
	MapFixture map;
	TestCompany company;

	const TileIndex tile = TileXY(10, 10);
	MakeRoadNormal(tile, ROAD_X, ROADTYPE_ROAD, INVALID_ROADTYPE, TownID::Invalid(), OWNER_NONE, OWNER_NONE);

	/* The read the task keeps, and the guard the demolition is planned with. */
	const Json guard = ReadTileRef(tile);
	REQUIRE(guard["type"] == "road");
	REQUIRE(ResolveObjectRef(guard, true)["found"] == true);

	/* The real command path of the module, exactly as `world.construct op=demolish` posts it. */
	const Json params = DemolishParams(10, 10, guard);
	/* The module switches the company that pays for the command and restores the company it found
	 * (`agent_game_tools.cpp: CompanyContext`), so the process keeps the company it had before. */
	const CompanyID previous_company = _current_company;
	const std::optional<Json> module_result = DispatchGameTool("world.construct", params, company.Id());
	REQUIRE(module_result.has_value());
	REQUIRE((*module_result)["ok"].get<bool>());
	CHECK(_current_company == previous_company);
	CHECK_FALSE(IsTileType(tile, TileType::Road));

	const Json postcondition = GuardPostcondition(ConstructRequest("request-1", params), BridgeResult(*module_result));

	/* The receipt is bound to the request it belongs to and says it is complete. */
	REQUIRE(postcondition.is_object());
	CHECK(postcondition["request_id"] == "request-1");
	CHECK(postcondition["task_id"] == "task-1");
	CHECK(postcondition["world_id"] == "test-world");
	CHECK(postcondition["epoch"] == "test-epoch");
	CHECK(postcondition["guarded"] == 1);
	CHECK(postcondition["complete"] == true);
	CHECK(postcondition["truncated"] == false);

	/* The one effect is the guarded tile: the state the guard was checked against, and the state
	 * the engine reads there after the command really ran. */
	REQUIRE(postcondition["tiles"].size() == 1);
	const Json &effect = postcondition["tiles"][0];
	CHECK(effect["changed"] == true);
	CHECK(effect["tile"] == TilePoint(10, 10));
	CHECK(effect["before"]["type"] == "road");
	CHECK(effect["before"]["fingerprint"] == guard["fingerprint"]);
	CHECK(effect["after"]["type"] == "terrain");
	CHECK(effect["after"]["fingerprint"].is_number_unsigned());
	CHECK(effect["after"]["fingerprint"] != guard["fingerprint"]);

	/*
	 * The composition the backend relies on: the reported after state is current for the next
	 * write, while the guard the first command ran with is stale. Nothing was inferred from the
	 * request - the after state is the engine's own read of the tile after the command.
	 */
	Json after = effect["after"];
	/* A reference is bound to the world and epoch it was read in, and the read happens in the world
	 * the bridge is bound to now - not in the world a request names. `Context()` is that binding. */
	const Json context = Context();
	REQUIRE(context["world_id"].is_string());
	REQUIRE_FALSE(context["world_id"].get<std::string>().empty());
	after["world_id"] = context["world_id"];
	after["epoch"] = context["epoch"];
	CHECK(ResolveObjectRef(after, true)["found"] == true);
	const Json stale = ResolveObjectRef(guard, true);
	CHECK(stale["found"] == false);
	CHECK(stale["error"] == "stale_ref");
}

TEST_CASE("Agent bridge guard: a guarded tile the command did not change is no effect")
{
	MapFixture map;
	TestCompany company;

	/* A clear tile stays clear: the demolition runs, costs nothing and changes nothing. */
	const TileIndex tile = TileXY(11, 11);
	REQUIRE(IsTileType(tile, TileType::Clear));
	const Json guard = ReadTileRef(tile);
	REQUIRE(guard["type"] == "terrain");

	const Json params = DemolishParams(11, 11, guard);
	const std::optional<Json> module_result = DispatchGameTool("world.construct", params, company.Id());
	REQUIRE(module_result.has_value());
	REQUIRE((*module_result)["ok"].get<bool>());

	const Json postcondition = GuardPostcondition(ConstructRequest("request-2", params), BridgeResult(*module_result));
	REQUIRE(postcondition.is_object());
	CHECK(postcondition["guarded"] == 1);
	CHECK(postcondition["complete"] == true);
	CHECK(postcondition["tiles"].empty());
	CHECK(ReadTileRef(tile)["fingerprint"] == guard["fingerprint"]);
}

TEST_CASE("Agent bridge guard: a dry run reports nothing")
{
	MapFixture map;
	TestCompany company;

	const TileIndex tile = TileXY(12, 12);
	MakeRoadNormal(tile, ROAD_X, ROADTYPE_ROAD, INVALID_ROADTYPE, TownID::Invalid(), OWNER_NONE, OWNER_NONE);
	const Json guard = ReadTileRef(tile);

	Json params = DemolishParams(12, 12, guard);
	params["dry_run"] = true;
	const std::optional<Json> module_result = DispatchGameTool("world.construct", params, company.Id());
	REQUIRE(module_result.has_value());
	REQUIRE((*module_result)["ok"].get<bool>());
	REQUIRE((*module_result)["dry_run"].get<bool>());
	CHECK(IsTileType(tile, TileType::Road));

	/* An estimate proves no after state, so it may not advance the guard of anything. */
	CHECK(GuardPostcondition(ConstructRequest("request-3", params), BridgeResult(*module_result)).empty());
}

TEST_CASE("Agent bridge guard: a command that did not run reports nothing")
{
	MapFixture map;

	const TileIndex tile = TileXY(13, 13);
	const Json guard = ReadTileRef(tile);
	const Json params = DemolishParams(13, 13, guard);

	/* The module answered with its own failure: the bridge keeps it at the top level, no value. */
	const Json not_executed = {{"ok", false}, {"error", "command_not_executed"}, {"executed", false}};
	CHECK(GuardPostcondition(ConstructRequest("request-4", params), not_executed).empty());

	/* A value that says the command never ran is no execution either. */
	const Json never_ran = {{"ok", true}, {"value", {{"ok", true}, {"executed", false}, {"dry_run", false}}}};
	CHECK(GuardPostcondition(ConstructRequest("request-5", params), never_ran).empty());

	/* An estimate is flagged by `estimated` as well. */
	const Json estimated = {{"ok", true}, {"value", {{"ok", true}, {"estimated", true}, {"dry_run", false}}}};
	CHECK(GuardPostcondition(ConstructRequest("request-6", params), estimated).empty());
}

TEST_CASE("Agent bridge guard: a read and an unguarded write report nothing")
{
	MapFixture map;

	const TileIndex tile = TileXY(14, 14);
	const Json guard = ReadTileRef(tile);

	/* A read is not a mutation, whatever it answers. */
	const Json read = {{"ok", true}, {"value", {{"rows", Json::array()}}}};
	const Json read_request = {{"request_id", "request-7"}, {"task_id", "task-1"},
			{"world_id", "test-world"}, {"epoch", "test-epoch"},
			{"method", "world.query"}, {"params", {{"kind", "vehicles"}}}};
	CHECK(GuardPostcondition(read_request, read).empty());

	/* A mutation without guarded tiles has no tile state to report. */
	const Json unguarded = {{"op", "demolish"}, {"tiles", Json::array({TilePoint(14, 14)})}};
	const Json unguarded_result = {{"ok", true}, {"value", {{"ok", true}, {"dry_run", false}}}};
	CHECK(GuardPostcondition(ConstructRequest("request-8", unguarded), unguarded_result).empty());

	/* A point names a location without an identity, so it guards no tile state. */
	Json points = Json::array();
	points.push_back({{"type", "point"}, {"tile", TilePoint(14, 14)}});
	const Json point_params = {{"op", "demolish"}, {"tiles", Json::array({TilePoint(14, 14)})},
			{"expected_refs", points}};
	CHECK(GuardPostcondition(ConstructRequest("request-9", point_params), unguarded_result).empty());
	CHECK(guard["fingerprint"].is_number_unsigned());
}

TEST_CASE("Agent bridge guard: only a receipt that states an actual execution reports a postcondition")
{
	MapFixture map;
	TestCompany company;

	/* One real demolition, so the receipts below are the module's own answers to a command that ran. */
	const TileIndex tile = TileXY(15, 15);
	MakeRoadNormal(tile, ROAD_X, ROADTYPE_ROAD, INVALID_ROADTYPE, TownID::Invalid(), OWNER_NONE, OWNER_NONE);
	const Json guard = ReadTileRef(tile);
	const Json params = DemolishParams(15, 15, guard);
	const std::optional<Json> module_result = DispatchGameTool("world.construct", params, company.Id());
	REQUIRE(module_result.has_value());
	REQUIRE((*module_result)["ok"].get<bool>());
	/* The module's own receipt of the actual execution: both mode flags say `false`. */
	REQUIRE((*module_result)["dry_run"].get<bool>() == false);
	REQUIRE((*module_result)["estimated"].get<bool>() == false);

	const Json request = ConstructRequest("request-10", params);
	CHECK_FALSE(GuardPostcondition(request, BridgeResult(*module_result)).empty());

	/*
	 * The mode of the call has to be stated by the module, as a boolean, and as an actual execution.
	 * A receipt without the flag, or with a value of another type, states no mode and is not read as
	 * an execution: the bridge would otherwise advance a guard no command proved.
	 */
	Json missing_dry_run = *module_result;
	missing_dry_run.erase("dry_run");
	CHECK(GuardPostcondition(request, BridgeResult(missing_dry_run)).empty());

	Json no_mode = *module_result;
	no_mode.erase("dry_run");
	no_mode.erase("estimated");
	CHECK(GuardPostcondition(request, BridgeResult(no_mode)).empty());

	Json string_dry_run = *module_result;
	string_dry_run["dry_run"] = "false";
	CHECK(GuardPostcondition(request, BridgeResult(string_dry_run)).empty());

	Json null_dry_run = *module_result;
	null_dry_run["dry_run"] = nullptr;
	CHECK(GuardPostcondition(request, BridgeResult(null_dry_run)).empty());

	Json estimate = *module_result;
	estimate["dry_run"] = true;
	estimate["estimated"] = true;
	CHECK(GuardPostcondition(request, BridgeResult(estimate)).empty());

	/* An optional mode flag that is malformed or contradicts the mode proves nothing either. */
	Json string_estimated = *module_result;
	string_estimated["estimated"] = "false";
	CHECK(GuardPostcondition(request, BridgeResult(string_estimated)).empty());

	Json estimated_while_executed = *module_result;
	estimated_while_executed["estimated"] = true;
	CHECK(GuardPostcondition(request, BridgeResult(estimated_while_executed)).empty());

	Json executed_false = *module_result;
	executed_false["executed"] = false;
	CHECK(GuardPostcondition(request, BridgeResult(executed_false)).empty());

	Json string_executed = *module_result;
	string_executed["executed"] = "true";
	CHECK(GuardPostcondition(request, BridgeResult(string_executed)).empty());

	/* The module's own word for an execution may be there; then it agrees with the mode flags. */
	Json executed_true = *module_result;
	executed_true["executed"] = true;
	CHECK_FALSE(GuardPostcondition(request, BridgeResult(executed_true)).empty());

	/* A command the module refused to run is no execution: the bridge keeps that failure itself. */
	const Json refusal = {{"ok", false}, {"error", "command_not_executed"},
			{"message", "the engine did not execute the command (rejected before dispatch, or only estimated)"},
			{"executed", false}};
	CHECK(GuardPostcondition(request, refusal).empty());
}

TEST_CASE("Agent bridge guard: a guarded tile the command left as it was is no effect, whatever its spelling")
{
	MapFixture map;
	TestCompany company;

	/*
	 * The guarded tile keeps its road; the demolition runs on another tile. The command therefore
	 * really changed the world, and the only question this case answers is whether the receipt
	 * mistakes an unchanged guarded tile for an effect because the guard is spelled differently from
	 * the fresh read of it.
	 */
	const TileIndex kept = TileXY(20, 20);
	MakeRoadNormal(kept, ROAD_X, ROADTYPE_ROAD, INVALID_ROADTYPE, TownID::Invalid(), OWNER_NONE, OWNER_NONE);
	const TileIndex cleared = TileXY(21, 21);

	const Json typed = ReadTileRef(kept);
	REQUIRE(typed["type"] == "road");
	REQUIRE(typed["what"] == "road");

	/* The same state in the generic spelling: `tile` with the category in `what`, and the bare
	 * generic spelling that names whatever the tile carries. */
	const Json generic = {{"type", "tile"}, {"tile", typed["tile"]}, {"what", "road"}, {"fingerprint", typed["fingerprint"]}};
	const Json bare = {{"type", "tile"}, {"tile", typed["tile"]}, {"fingerprint", typed["fingerprint"]}};

	for (const Json &guard : {typed, generic, bare}) {
		MakeRoadNormal(cleared, ROAD_X, ROADTYPE_ROAD, INVALID_ROADTYPE, TownID::Invalid(), OWNER_NONE, OWNER_NONE);
		Json refs = Json::array();
		refs.push_back(guard);
		const Json params = {{"op", "demolish"}, {"tiles", Json::array({TilePoint(21, 21)})}, {"expected_refs", refs}};

		const std::optional<Json> module_result = DispatchGameTool("world.construct", params, company.Id());
		REQUIRE(module_result.has_value());
		REQUIRE((*module_result)["ok"].get<bool>());
		REQUIRE_FALSE(IsTileType(cleared, TileType::Road));

		const Json postcondition = GuardPostcondition(ConstructRequest("request-11", params), BridgeResult(*module_result));
		REQUIRE(postcondition.is_object());
		CHECK(postcondition["guarded"] == 1);
		CHECK(postcondition["complete"] == true);
		CHECK(postcondition["truncated"] == false);
		/* The guarded tile was examined and is unchanged: no effect, so no state to advance to. */
		CHECK(postcondition["tiles"].empty());
		CHECK(IsTileType(kept, TileType::Road));
	}
}

TEST_CASE("Agent bridge guard: the receipt is bounded to the 64 references a request may carry")
{
	MapFixture map;
	TestCompany company;

	/* A request may carry 64 guards; `CheckExpectedRefs` refuses 65 before anything is executed. */
	Json refs = Json::array();
	for (int i = 0; i < 64; i++) {
		const TileIndex tile = TileXY(30 + i % 16, 30 + i / 16);
		MakeRoadNormal(tile, ROAD_X, ROADTYPE_ROAD, INVALID_ROADTYPE, TownID::Invalid(), OWNER_NONE, OWNER_NONE);
		refs.push_back(ReadTileRef(tile));
	}
	REQUIRE(refs.size() == 64);

	/* The demolition runs on a tile no guard names, so every guarded tile stays unchanged. */
	const TileIndex cleared = TileXY(50, 50);
	MakeRoadNormal(cleared, ROAD_X, ROADTYPE_ROAD, INVALID_ROADTYPE, TownID::Invalid(), OWNER_NONE, OWNER_NONE);
	const Json params = {{"op", "demolish"}, {"tiles", Json::array({TilePoint(50, 50)})}, {"expected_refs", refs}};

	const std::optional<Json> module_result = DispatchGameTool("world.construct", params, company.Id());
	REQUIRE(module_result.has_value());
	REQUIRE((*module_result)["ok"].get<bool>());
	REQUIRE_FALSE(IsTileType(cleared, TileType::Road));

	const Json postcondition = GuardPostcondition(ConstructRequest("request-12", params), BridgeResult(*module_result));
	REQUIRE(postcondition.is_object());
	CHECK(postcondition["guarded"] == 64);
	CHECK(postcondition["complete"] == true);
	CHECK(postcondition["truncated"] == false);
	CHECK(postcondition["tiles"].empty());

	/*
	 * One reference more than a request may carry. The bridge validator rejects such a request
	 * before it executes anything, so this is the defensive bound of the collector itself, not a
	 * request the engine accepted: the report stops at 64 and says that it was cut short.
	 */
	const TileIndex extra = TileXY(52, 52);
	MakeRoadNormal(extra, ROAD_X, ROADTYPE_ROAD, INVALID_ROADTYPE, TownID::Invalid(), OWNER_NONE, OWNER_NONE);
	Json too_many = refs;
	too_many.push_back(ReadTileRef(extra));
	REQUIRE(too_many.size() == 65);
	const Json wide_params = {{"op", "demolish"}, {"tiles", Json::array({TilePoint(50, 50)})}, {"expected_refs", too_many}};

	const Json truncated = GuardPostcondition(ConstructRequest("request-13", wide_params), BridgeResult(*module_result));
	REQUIRE(truncated.is_object());
	CHECK(truncated["guarded"] == 64);
	CHECK(truncated["truncated"] == true);
	CHECK(truncated["complete"] == false);
	CHECK(truncated["tiles"].empty());
}
