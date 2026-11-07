/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/>.
 */

/**
 * @file test_agent_game_tools.cpp Test the dispatch, the argument validation and the reported gaps
 * of the agent game tools.
 *
 * No game is running in this test binary, so no real command is ever executed here: these cases
 * cover dispatch, the validation that runs before a command is posted, and gap reporting. The
 * command path itself (Command::Post, the created ids and the actual costs) needs a running game.
 */

#include "../stdafx.h"

#include "../3rdparty/catch2/catch.hpp"

#include "../agent_game_tools.h"

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
 * Build a `{x, y}` tile object.
 * @param x X coordinate.
 * @param y Y coordinate.
 * @return The tile object.
 */
static Json Tile(int x, int y)
{
	return Json{{"x", x}, {"y", y}};
}

TEST_CASE("agent_game_tools: capabilities list the implemented methods and their gaps")
{
	const Json capabilities = GameToolCapabilities();
	CHECK(capabilities["module"] == "agent_game_tools");
	REQUIRE(capabilities["methods"].is_array());
	CHECK(capabilities["unimplemented_methods"].is_array());

	const Json construct = FindCapability(capabilities, "world.construct");
	REQUIRE(!construct.empty());
	CHECK(construct["authority"] == "write");
	CHECK(Contains(construct["implemented"], "road"));
	CHECK(Contains(construct["implemented"], "station"));
	CHECK(Contains(construct["implemented"], "depot"));
	CHECK(Contains(construct["gaps"], "rail"));

	const Json vehicles = FindCapability(capabilities, "world.vehicles");
	REQUIRE(!vehicles.empty());
	CHECK(Contains(vehicles["implemented"], "build"));
	CHECK(Contains(vehicles["implemented"], "start"));
	CHECK(Contains(vehicles["implemented"], "stop"));
	CHECK(Contains(vehicles["gaps"], "refit"));

	const Json orders = FindCapability(capabilities, "world.orders");
	REQUIRE(!orders.empty());
	CHECK(Contains(orders["implemented"], "set"));
	CHECK(Contains(orders["implemented"], "append"));
	CHECK(Contains(orders["gaps"], "timetable"));

	const Json query = FindCapability(capabilities, "world.query");
	REQUIRE(!query.empty());
	CHECK(query["authority"] == "read");
	CHECK(Contains(query["implemented"], "vehicles"));
	CHECK(Contains(query["implemented"], "engines"));
}

TEST_CASE("agent_game_tools: a method outside the game tools is not claimed")
{
	CHECK_FALSE(DispatchGameTool("world.unknown", Json::object(), CompanyID::Invalid()).has_value());
	CHECK_FALSE(DispatchGameTool("", Json::object(), CompanyID::Invalid()).has_value());
}

TEST_CASE("agent_game_tools: a known but unimplemented method is an explicit gap")
{
	const std::optional<Json> result = DispatchGameTool("world.info", Json::object(), CompanyID::Invalid());
	REQUIRE(result.has_value());
	CHECK((*result)["ok"] == false);
	CHECK((*result)["error"] == "unsupported_method");
	CHECK((*result)["detail"]["method"] == "world.info");
}

TEST_CASE("agent_game_tools: params must be an object")
{
	const std::optional<Json> result = DispatchGameTool("world.query", Json::array(), CompanyID::Invalid());
	REQUIRE(result.has_value());
	CHECK((*result)["ok"] == false);
	CHECK((*result)["error"] == "invalid_arguments");
}

TEST_CASE("agent_game_tools: world.query needs a kind and reports unknown kinds as gaps")
{
	const std::optional<Json> missing = DispatchGameTool("world.query", Json::object(), CompanyID::Invalid());
	REQUIRE(missing.has_value());
	CHECK((*missing)["error"] == "invalid_arguments");

	const std::optional<Json> unknown = DispatchGameTool("world.query", {{"kind", "towns"}}, CompanyID::Invalid());
	REQUIRE(unknown.has_value());
	CHECK((*unknown)["ok"] == false);
	CHECK((*unknown)["error"] == "unsupported_op");
	CHECK((*unknown)["detail"]["op"] == "towns");
}

TEST_CASE("agent_game_tools: the vehicle and engine inventory is read-only and empty without a game")
{
	const std::optional<Json> vehicles = DispatchGameTool("world.query", {{"kind", "vehicles"}}, CompanyID::Invalid());
	REQUIRE(vehicles.has_value());
	CHECK((*vehicles)["ok"] == true);
	CHECK((*vehicles)["kind"] == "vehicles");
	CHECK((*vehicles)["rows"].is_array());
	CHECK((*vehicles)["total"] == 0);

	const std::optional<Json> engines = DispatchGameTool("world.query", {{"kind", "engines"}}, CompanyID::Invalid());
	REQUIRE(engines.has_value());
	CHECK((*engines)["ok"] == true);
	CHECK((*engines)["kind"] == "engines");
	CHECK((*engines)["rows"].is_array());
}

TEST_CASE("agent_game_tools: unsupported ops of the write families are gaps, not fake successes")
{
	const std::optional<Json> rail = DispatchGameTool("world.construct", {{"op", "rail"}}, CompanyID::Invalid());
	REQUIRE(rail.has_value());
	CHECK((*rail)["ok"] == false);
	CHECK((*rail)["error"] == "unsupported_op");
	CHECK((*rail)["detail"]["op"] == "rail");

	const std::optional<Json> refit = DispatchGameTool("world.vehicles", {{"op", "refit"}}, CompanyID::Invalid());
	REQUIRE(refit.has_value());
	CHECK((*refit)["error"] == "unsupported_op");
	CHECK((*refit)["detail"]["op"] == "refit");

	const std::optional<Json> timetable = DispatchGameTool("world.orders", {{"op", "timetable"}}, CompanyID::Invalid());
	REQUIRE(timetable.has_value());
	CHECK((*timetable)["error"] == "unsupported_op");
	CHECK((*timetable)["detail"]["op"] == "timetable");
}

TEST_CASE("agent_game_tools: a write method needs a valid company")
{
	const std::optional<Json> result = DispatchGameTool("world.construct", {{"op", "road"}, {"from", Tile(0, 0)}, {"to", Tile(0, 0)}}, CompanyID(3));
	REQUIRE(result.has_value());
	CHECK((*result)["ok"] == false);
	CHECK((*result)["error"] == "invalid_company");
	CHECK((*result)["detail"]["company_id"] == 3);

	/* A dry run still needs a company to estimate for. */
	const std::optional<Json> dry_run = DispatchGameTool("world.construct", {{"op", "road"}, {"from", Tile(0, 0)}, {"to", Tile(0, 0)}, {"dry_run", true}}, CompanyID(3));
	REQUIRE(dry_run.has_value());
	CHECK((*dry_run)["error"] == "invalid_company");
}

TEST_CASE("agent_game_tools: a station rectangle wider than 255 tiles is rejected, not truncated")
{
	const std::optional<Json> result = DispatchGameTool("world.construct", {{"op", "station"}, {"from", Tile(0, 0)}, {"to", Tile(300, 0)}}, CompanyID(3));
	REQUIRE(result.has_value());
	CHECK((*result)["ok"] == false);
	CHECK((*result)["error"] == "invalid_arguments");
	CHECK((*result)["detail"]["width"] == 301);
	CHECK((*result)["detail"]["max"] == 255);

	/* The span is checked before the company, so a bad rectangle never reaches a command. */
	const std::optional<Json> tall = DispatchGameTool("world.construct", {{"op", "station"}, {"from", Tile(0, 0)}, {"to", Tile(0, 256)}}, CompanyID(3));
	REQUIRE(tall.has_value());
	CHECK((*tall)["error"] == "invalid_arguments");
	CHECK((*tall)["detail"]["length"] == 257);
}

TEST_CASE("agent_game_tools: a multi-tile station or depot request is rejected instead of using tiles[0]")
{
	const Json tiles = Json::array({Tile(0, 0), Tile(1, 0)});

	const std::optional<Json> station = DispatchGameTool("world.construct", {{"op", "station"}, {"tiles", tiles}}, CompanyID(3));
	REQUIRE(station.has_value());
	CHECK((*station)["ok"] == false);
	CHECK((*station)["error"] == "unsupported_multi_item");
	CHECK((*station)["detail"]["count"] == 2);
	CHECK((*station)["detail"]["supported"] == 1);

	const std::optional<Json> depot = DispatchGameTool("world.construct", {{"op", "depot"}, {"tiles", tiles}}, CompanyID(3));
	REQUIRE(depot.has_value());
	CHECK((*depot)["error"] == "unsupported_multi_item");
	CHECK((*depot)["detail"]["count"] == 2);
}

TEST_CASE("agent_game_tools: road needs a usable tile list or a straight run")
{
	const std::optional<Json> nothing = DispatchGameTool("world.construct", {{"op", "road"}}, CompanyID(3));
	REQUIRE(nothing.has_value());
	CHECK((*nothing)["error"] == "invalid_arguments");

	const std::optional<Json> half_run = DispatchGameTool("world.construct", {{"op", "road"}, {"from", Tile(0, 0)}}, CompanyID(3));
	REQUIRE(half_run.has_value());
	CHECK((*half_run)["error"] == "invalid_arguments");

	const std::optional<Json> diagonal = DispatchGameTool("world.construct", {{"op", "road"}, {"from", Tile(0, 0)}, {"to", Tile(1, 1)}}, CompanyID(3));
	REQUIRE(diagonal.has_value());
	CHECK((*diagonal)["error"] == "invalid_arguments");

	const std::optional<Json> bad_tile = DispatchGameTool("world.construct", {{"op", "road"}, {"tiles", Json::array({"nope"})}}, CompanyID(3));
	REQUIRE(bad_tile.has_value());
	CHECK((*bad_tile)["error"] == "invalid_arguments");
}

TEST_CASE("agent_game_tools: optional parameters are validated instead of silently ignored")
{
	const Json tiles = Json::array({Tile(0, 0)});

	const std::optional<Json> dry_run = DispatchGameTool("world.construct", {{"op", "road"}, {"tiles", tiles}, {"dry_run", "yes"}}, CompanyID(3));
	REQUIRE(dry_run.has_value());
	CHECK((*dry_run)["error"] == "invalid_arguments");

	const std::optional<Json> road_type = DispatchGameTool("world.construct", {{"op", "road"}, {"tiles", tiles}, {"road_type", "monorail"}}, CompanyID(3));
	REQUIRE(road_type.has_value());
	CHECK((*road_type)["error"] == "invalid_arguments");

	const std::optional<Json> direction = DispatchGameTool("world.construct", {{"op", "depot"}, {"tiles", tiles}, {"direction", "up"}}, CompanyID(3));
	REQUIRE(direction.has_value());
	CHECK((*direction)["error"] == "invalid_arguments");

	const std::optional<Json> stop_type = DispatchGameTool("world.construct", {{"op", "station"}, {"tiles", tiles}, {"stop_type", "air"}}, CompanyID(3));
	REQUIRE(stop_type.has_value());
	CHECK((*stop_type)["error"] == "invalid_arguments");

	/* `demolish` is only honoured by op=demolish; elsewhere it is a gap, not a silent no-op. */
	const std::optional<Json> demolish = DispatchGameTool("world.construct", {{"op", "station"}, {"tiles", tiles}, {"demolish", true}}, CompanyID(3));
	REQUIRE(demolish.has_value());
	CHECK((*demolish)["error"] == "unsupported_op");
}

TEST_CASE("agent_game_tools: vehicle build parameters are validated before anything is created")
{
	const std::optional<Json> no_engine = DispatchGameTool("world.vehicles", {{"op", "build"}}, CompanyID(3));
	REQUIRE(no_engine.has_value());
	CHECK((*no_engine)["error"] == "invalid_arguments");

	const std::optional<Json> count = DispatchGameTool("world.vehicles", {{"op", "build"}, {"engine_id", 0}, {"depot_id", 0}, {"count", 101}}, CompanyID(3));
	REQUIRE(count.has_value());
	CHECK((*count)["error"] == "invalid_arguments");
	CHECK((*count)["detail"]["max"] == 100);

	const std::optional<Json> zero_count = DispatchGameTool("world.vehicles", {{"op", "build"}, {"engine_id", 0}, {"depot_id", 0}, {"count", 0}}, CompanyID(3));
	REQUIRE(zero_count.has_value());
	CHECK((*zero_count)["error"] == "invalid_arguments");

	const std::optional<Json> cargo = DispatchGameTool("world.vehicles", {{"op", "build"}, {"engine_id", 0}, {"depot_id", 0}, {"cargo_id", 99}}, CompanyID(3));
	REQUIRE(cargo.has_value());
	CHECK((*cargo)["error"] == "invalid_arguments");
	CHECK((*cargo)["detail"]["cargo_id"] == 99);

	const std::optional<Json> name = DispatchGameTool("world.vehicles", {{"op", "build"}, {"engine_id", 0}, {"depot_id", 0}, {"name", std::string(129, 'x')}}, CompanyID(3));
	REQUIRE(name.has_value());
	CHECK((*name)["error"] == "invalid_arguments");
	CHECK((*name)["detail"]["max"] == 128);

	/* `share_orders_from` is read, so a wrong type is an error and not an ignored parameter. */
	const std::optional<Json> share = DispatchGameTool("world.vehicles", {{"op", "build"}, {"engine_id", 0}, {"depot_id", 0}, {"share_orders_from", "x"}}, CompanyID(3));
	REQUIRE(share.has_value());
	CHECK((*share)["error"] == "invalid_arguments");
}

TEST_CASE("agent_game_tools: order lists are validated before the vehicle is touched")
{
	const std::optional<Json> missing = DispatchGameTool("world.orders", {{"op", "set"}, {"vehicle_id", 0}}, CompanyID(3));
	REQUIRE(missing.has_value());
	CHECK((*missing)["error"] == "invalid_arguments");

	const std::optional<Json> empty = DispatchGameTool("world.orders", {{"op", "set"}, {"vehicle_id", 0}, {"orders", Json::array()}}, CompanyID(3));
	REQUIRE(empty.has_value());
	CHECK((*empty)["error"] == "invalid_arguments");

	const std::optional<Json> type = DispatchGameTool("world.orders", {{"op", "set"}, {"vehicle_id", 0}, {"orders", Json::array({Json{{"type", "bogus"}, {"id", 1}}})}}, CompanyID(3));
	REQUIRE(type.has_value());
	CHECK((*type)["error"] == "invalid_arguments");

	const std::optional<Json> no_id = DispatchGameTool("world.orders", {{"op", "append"}, {"vehicle_id", 0}, {"orders", Json::array({Json{{"type", "station"}}})}}, CompanyID(3));
	REQUIRE(no_id.has_value());
	CHECK((*no_id)["error"] == "invalid_arguments");

	const std::optional<Json> refit = DispatchGameTool("world.orders", {{"op", "append"}, {"vehicle_id", 0}, {"orders", Json::array({Json{{"type", "station"}, {"id", 1}, {"refit_cargo_id", 99}}})}}, CompanyID(3));
	REQUIRE(refit.has_value());
	CHECK((*refit)["error"] == "invalid_arguments");

	const std::optional<Json> shared = DispatchGameTool("world.orders", {{"op", "shared"}, {"vehicle_id", 0}}, CompanyID(3));
	REQUIRE(shared.has_value());
	CHECK((*shared)["error"] == "invalid_arguments");

	const std::optional<Json> group = DispatchGameTool("world.orders", {{"op", "group"}, {"vehicle_id", 0}}, CompanyID(3));
	REQUIRE(group.has_value());
	CHECK((*group)["error"] == "invalid_arguments");
}
