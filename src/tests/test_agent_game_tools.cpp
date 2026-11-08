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

#include <filesystem>

#include "../agent_game_tools.h"
#include "../aircraft.h"
#include "../airport.h"
#include "../cargotype.h"
#include "../clear_map.h"
#include "../company_base.h"
#include "../engine_base.h"
#include "../language.h"
#include "../map_func.h"
#include "../order_base.h"
#include "../road.h"
#include "../road_map.h"
#include "../road_type.h"
#include "../roadveh.h"
#include "../settings_type.h"
#include "../ship.h"
#include "../station_base.h"
#include "../station_map.h"
#include "../station_type.h"
#include "../table/strings.h"
#include "../tile_cmd.h"
#include "../track_func.h"
#include "../train.h"
#include "../vehicle_base.h"
#include "../void_map.h"

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
	CHECK(Contains(vehicles["implemented"], "refit"));
	CHECK(Contains(vehicles["implemented"], "clone"));
	CHECK(vehicles["gaps"].empty());

	const Json orders = FindCapability(capabilities, "world.orders");
	REQUIRE(!orders.empty());
	CHECK(Contains(orders["implemented"], "set"));
	CHECK(Contains(orders["implemented"], "append"));
	CHECK(Contains(orders["implemented"], "timetable"));
	CHECK(orders["gaps"].empty());

	const Json query = FindCapability(capabilities, "world.query");
	REQUIRE(!query.empty());
	CHECK(query["authority"] == "read");
	CHECK(Contains(query["implemented"], "vehicles"));
	CHECK(Contains(query["implemented"], "engines"));
	CHECK(Contains(query["implemented"], "orders"));
	CHECK(Contains(query["implemented"], "groups"));
	CHECK(Contains(query["implemented"], "cargos"));
	CHECK_FALSE(Contains(query["gaps"], "groups"));
	CHECK_FALSE(Contains(query["gaps"], "cargos"));
}

TEST_CASE("agent_game_tools: a method outside the game tools is not claimed")
{
	CHECK_FALSE(DispatchGameTool("world.unknown", Json::object(), CompanyID::Invalid()).has_value());
	CHECK_FALSE(DispatchGameTool("", Json::object(), CompanyID::Invalid()).has_value());
}

TEST_CASE("agent_game_tools: a method another module owns is not claimed")
{
	/* The bridge kernel, transport and admin own these; claiming them would shadow their work. */
	CHECK_FALSE(DispatchGameTool("world.info", Json::object(), CompanyID::Invalid()).has_value());
	CHECK_FALSE(DispatchGameTool("world.pause", Json::object(), CompanyID::Invalid()).has_value());
	CHECK_FALSE(DispatchGameTool("world.capture", Json::object(), CompanyID::Invalid()).has_value());
	CHECK_FALSE(DispatchGameTool("task.barrier", Json::object(), CompanyID::Invalid()).has_value());
}

TEST_CASE("agent_game_tools: params must be an object")
{
	const std::optional<Json> result = DispatchGameTool("world.query", Json::array(), CompanyID::Invalid());
	REQUIRE(result.has_value());
	CHECK((*result)["ok"] == false);
	CHECK((*result)["error"] == "invalid_arguments");
}

TEST_CASE("agent_game_tools: world.query needs a kind and leaves other kinds to their module")
{
	const std::optional<Json> missing = DispatchGameTool("world.query", Json::object(), CompanyID::Invalid());
	REQUIRE(missing.has_value());
	CHECK((*missing)["error"] == "invalid_arguments");

	CHECK_FALSE(DispatchGameTool("world.query", {{"kind", "towns"}}, CompanyID::Invalid()).has_value());
	CHECK_FALSE(DispatchGameTool("world.query", {{"kind", "stations"}}, CompanyID::Invalid()).has_value());
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

	const std::optional<Json> orders = DispatchGameTool("world.query", {{"kind", "orders"}}, CompanyID::Invalid());
	REQUIRE(orders.has_value());
	CHECK((*orders)["ok"] == true);
	CHECK((*orders)["kind"] == "orders");
	CHECK((*orders)["rows"].is_array());
	CHECK((*orders)["total"] == 0);
}

TEST_CASE("agent_game_tools: query filters are validated instead of silently ignored")
{
	const std::optional<Json> unknown_filter = DispatchGameTool("world.query", {{"kind", "vehicles"}, {"filters", {{"colour", "red"}}}}, CompanyID::Invalid());
	REQUIRE(unknown_filter.has_value());
	CHECK((*unknown_filter)["ok"] == false);
	CHECK((*unknown_filter)["error"] == "invalid_arguments");
	CHECK((*unknown_filter)["detail"]["filter"] == "colour");
	CHECK((*unknown_filter)["detail"]["kind"] == "vehicles");

	/* A filter that only another kind understands is an error, not a dropped filter. */
	const std::optional<Json> wrong_kind = DispatchGameTool("world.query", {{"kind", "engines"}, {"filters", {{"in_depot", true}}}}, CompanyID::Invalid());
	REQUIRE(wrong_kind.has_value());
	CHECK((*wrong_kind)["error"] == "invalid_arguments");
	CHECK((*wrong_kind)["detail"]["filter"] == "in_depot");

	const std::optional<Json> bad_type = DispatchGameTool("world.query", {{"kind", "vehicles"}, {"filters", {{"vehicle_type", "monorail"}}}}, CompanyID::Invalid());
	REQUIRE(bad_type.has_value());
	CHECK((*bad_type)["error"] == "invalid_arguments");

	const std::optional<Json> bad_cargo = DispatchGameTool("world.query", {{"kind", "vehicles"}, {"filters", {{"cargo_id", 99}}}}, CompanyID::Invalid());
	REQUIRE(bad_cargo.has_value());
	CHECK((*bad_cargo)["error"] == "invalid_arguments");
	CHECK((*bad_cargo)["detail"]["cargo_id"] == 99);

	const std::optional<Json> bad_bool = DispatchGameTool("world.query", {{"kind", "vehicles"}, {"filters", {{"stopped", "yes"}}}}, CompanyID::Invalid());
	REQUIRE(bad_bool.has_value());
	CHECK((*bad_bool)["error"] == "invalid_arguments");

	const std::optional<Json> bad_limit = DispatchGameTool("world.query", {{"kind", "vehicles"}, {"limit", "many"}}, CompanyID::Invalid());
	REQUIRE(bad_limit.has_value());
	CHECK((*bad_limit)["error"] == "invalid_arguments");
}

TEST_CASE("agent_game_tools: ops of the write families that another module owns are not claimed")
{
	CHECK_FALSE(DispatchGameTool("world.construct", {{"op", "rail"}}, CompanyID::Invalid()).has_value());
	CHECK_FALSE(DispatchGameTool("world.construct", {{"op", "signal"}}, CompanyID::Invalid()).has_value());
	CHECK_FALSE(DispatchGameTool("world.vehicles", {{"op", "autoreplace"}}, CompanyID::Invalid()).has_value());
	CHECK_FALSE(DispatchGameTool("world.orders", {{"op", "delete"}}, CompanyID::Invalid()).has_value());
}

TEST_CASE("agent_game_tools: a parameter the op does not read is rejected, not silently dropped")
{
	const std::optional<Json> refit = DispatchGameTool("world.vehicles", {{"op", "refit"}, {"vehicle_id", 0}, {"cargo_id", 0}, {"colour", "red"}}, CompanyID(3));
	REQUIRE(refit.has_value());
	CHECK((*refit)["ok"] == false);
	CHECK((*refit)["error"] == "invalid_arguments");
	CHECK((*refit)["detail"]["method"] == "world.vehicles");
	CHECK((*refit)["detail"]["op"] == "refit");
	CHECK((*refit)["detail"]["unknown"] == Json::array({"colour"}));
	CHECK(Contains((*refit)["detail"]["supported"], "cargo_id"));

	const std::optional<Json> timetable = DispatchGameTool("world.orders", {{"op", "timetable"}, {"vehicle_id", 0}, {"timetable", Json::array()}, {"orders", Json::array()}}, CompanyID(3));
	REQUIRE(timetable.has_value());
	CHECK((*timetable)["error"] == "invalid_arguments");
	CHECK((*timetable)["detail"]["unknown"] == Json::array({"orders"}));

	/* `expected_refs` belongs to the parent bridge and is accepted everywhere. */
	const std::optional<Json> expected_refs = DispatchGameTool("world.vehicles", {{"op", "refit"}, {"vehicle_id", 0}, {"cargo_id", 0}, {"expected_refs", Json::array()}}, CompanyID(3));
	REQUIRE(expected_refs.has_value());
	CHECK((*expected_refs)["error"] == "invalid_company");
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
	CHECK((*cargo)["error"] == "invalid_params");
	CHECK((*cargo)["detail"]["cargo_id"] == 99);

	const std::optional<Json> name = DispatchGameTool("world.vehicles", {{"op", "build"}, {"engine_id", 0}, {"depot_id", 0}, {"name", std::string(129, 'x')}}, CompanyID(3));
	REQUIRE(name.has_value());
	CHECK((*name)["error"] == "invalid_arguments");
	CHECK((*name)["detail"]["max"] == 128);

	/* `share_orders_from` is read, so a wrong type is an error and not an ignored parameter. */
	const std::optional<Json> share = DispatchGameTool("world.vehicles", {{"op", "build"}, {"engine_id", 0}, {"depot_id", 0}, {"share_orders_from", "x"}}, CompanyID(3));
	REQUIRE(share.has_value());
	CHECK((*share)["error"] == "invalid_params");
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
	CHECK((*refit)["error"] == "invalid_params");

	const std::optional<Json> shared = DispatchGameTool("world.orders", {{"op", "shared"}, {"vehicle_id", 0}}, CompanyID(3));
	REQUIRE(shared.has_value());
	CHECK((*shared)["error"] == "invalid_arguments");

	const std::optional<Json> group = DispatchGameTool("world.orders", {{"op", "group"}, {"vehicle_id", 0}}, CompanyID(3));
	REQUIRE(group.has_value());
	CHECK((*group)["error"] == "invalid_arguments");
}

TEST_CASE("agent_game_tools: refit parameters are validated before anything is mutated")
{
	const std::optional<Json> no_vehicle = DispatchGameTool("world.vehicles", {{"op", "refit"}, {"cargo_id", 0}}, CompanyID(3));
	REQUIRE(no_vehicle.has_value());
	CHECK((*no_vehicle)["error"] == "invalid_arguments");

	const std::optional<Json> no_cargo = DispatchGameTool("world.vehicles", {{"op", "refit"}, {"vehicle_id", 0}}, CompanyID(3));
	REQUIRE(no_cargo.has_value());
	CHECK((*no_cargo)["error"] == "invalid_arguments");

	const std::optional<Json> bad_cargo = DispatchGameTool("world.vehicles", {{"op", "refit"}, {"vehicle_id", 0}, {"cargo_id", 99}}, CompanyID(3));
	REQUIRE(bad_cargo.has_value());
	CHECK((*bad_cargo)["error"] == "invalid_params");
	CHECK((*bad_cargo)["detail"]["cargo_id"] == 99);
	CHECK((*bad_cargo)["detail"]["max"] == 63);

	const std::optional<Json> bad_flag = DispatchGameTool("world.vehicles", {{"op", "refit"}, {"vehicle_id", 0}, {"cargo_id", 0}, {"only_this", "yes"}}, CompanyID(3));
	REQUIRE(bad_flag.has_value());
	CHECK((*bad_flag)["error"] == "invalid_arguments");

	/* A usable request reaches the company check, so refit is dispatched and not a gap. */
	const std::optional<Json> company = DispatchGameTool("world.vehicles", {{"op", "refit"}, {"vehicle_id", 0}, {"cargo_id", 0}}, CompanyID(3));
	REQUIRE(company.has_value());
	CHECK((*company)["error"] == "invalid_company");
}

TEST_CASE("agent_game_tools: clone parameters are validated before anything is created")
{
	const std::optional<Json> no_vehicle = DispatchGameTool("world.vehicles", {{"op", "clone"}}, CompanyID(3));
	REQUIRE(no_vehicle.has_value());
	CHECK((*no_vehicle)["error"] == "invalid_arguments");

	const std::optional<Json> count = DispatchGameTool("world.vehicles", {{"op", "clone"}, {"vehicle_id", 0}, {"count", 101}}, CompanyID(3));
	REQUIRE(count.has_value());
	CHECK((*count)["error"] == "invalid_arguments");
	CHECK((*count)["detail"]["max"] == 100);

	const std::optional<Json> zero_count = DispatchGameTool("world.vehicles", {{"op", "clone"}, {"vehicle_id", 0}, {"count", 0}}, CompanyID(3));
	REQUIRE(zero_count.has_value());
	CHECK((*zero_count)["error"] == "invalid_arguments");

	const std::optional<Json> share = DispatchGameTool("world.vehicles", {{"op", "clone"}, {"vehicle_id", 0}, {"share_orders", "yes"}}, CompanyID(3));
	REQUIRE(share.has_value());
	CHECK((*share)["error"] == "invalid_arguments");

	const std::optional<Json> company = DispatchGameTool("world.vehicles", {{"op", "clone"}, {"vehicle_id", 0}}, CompanyID(3));
	REQUIRE(company.has_value());
	CHECK((*company)["error"] == "invalid_company");
}

TEST_CASE("agent_game_tools: timetable entries are validated before the first command")
{
	const std::optional<Json> no_vehicle = DispatchGameTool("world.orders", {{"op", "timetable"}, {"timetable", Json::array({Json{{"index", 0}, {"wait_time", 10}}})}}, CompanyID(3));
	REQUIRE(no_vehicle.has_value());
	CHECK((*no_vehicle)["error"] == "invalid_arguments");

	const std::optional<Json> no_entries = DispatchGameTool("world.orders", {{"op", "timetable"}, {"vehicle_id", 0}}, CompanyID(3));
	REQUIRE(no_entries.has_value());
	CHECK((*no_entries)["error"] == "invalid_arguments");

	const std::optional<Json> empty = DispatchGameTool("world.orders", {{"op", "timetable"}, {"vehicle_id", 0}, {"timetable", Json::array()}}, CompanyID(3));
	REQUIRE(empty.has_value());
	CHECK((*empty)["error"] == "invalid_arguments");

	const std::optional<Json> not_object = DispatchGameTool("world.orders", {{"op", "timetable"}, {"vehicle_id", 0}, {"timetable", Json::array({1})}}, CompanyID(3));
	REQUIRE(not_object.has_value());
	CHECK((*not_object)["error"] == "invalid_arguments");

	const std::optional<Json> unknown_field = DispatchGameTool("world.orders", {{"op", "timetable"}, {"vehicle_id", 0}, {"timetable", Json::array({Json{{"index", 0}, {"wait_time", 10}, {"colour", "red"}}})}}, CompanyID(3));
	REQUIRE(unknown_field.has_value());
	CHECK((*unknown_field)["error"] == "invalid_arguments");
	CHECK((*unknown_field)["detail"]["unknown"] == Json::array({"colour"}));

	const std::optional<Json> no_field = DispatchGameTool("world.orders", {{"op", "timetable"}, {"vehicle_id", 0}, {"timetable", Json::array({Json{{"index", 0}}})}}, CompanyID(3));
	REQUIRE(no_field.has_value());
	CHECK((*no_field)["error"] == "invalid_arguments");

	const std::optional<Json> bad_value = DispatchGameTool("world.orders", {{"op", "timetable"}, {"vehicle_id", 0}, {"timetable", Json::array({Json{{"index", 0}, {"wait_time", 70000}}})}}, CompanyID(3));
	REQUIRE(bad_value.has_value());
	CHECK((*bad_value)["error"] == "invalid_arguments");
	CHECK((*bad_value)["detail"]["max"] == 65535);

	const std::optional<Json> company = DispatchGameTool("world.orders", {{"op", "timetable"}, {"vehicle_id", 0}, {"timetable", Json::array({Json{{"index", 0}, {"wait_time", 10}}})}}, CompanyID(3));
	REQUIRE(company.has_value());
	CHECK((*company)["error"] == "invalid_company");
}

TEST_CASE("agent_game_tools: world.query answers the canonical kinds and their singular aliases")
{
	const Json capabilities = GameToolCapabilities();
	REQUIRE(capabilities["query_aliases"].is_object());
	CHECK(capabilities["query_aliases"].size() == 10);
	CHECK(capabilities["query_aliases"]["vehicles"] == "vehicles");
	CHECK(capabilities["query_aliases"]["vehicle"] == "vehicles");
	CHECK(capabilities["query_aliases"]["engines"] == "engines");
	CHECK(capabilities["query_aliases"]["engine"] == "engines");
	CHECK(capabilities["query_aliases"]["orders"] == "orders");
	CHECK(capabilities["query_aliases"]["order"] == "orders");
	CHECK(capabilities["query_aliases"]["groups"] == "groups");
	CHECK(capabilities["query_aliases"]["group"] == "groups");
	CHECK(capabilities["query_aliases"]["cargos"] == "cargos");
	CHECK(capabilities["query_aliases"]["cargo"] == "cargos");

	for (const auto &[kind, canonical] : {std::pair{"vehicle", "vehicles"}, {"vehicles", "vehicles"},
			{"engine", "engines"}, {"engines", "engines"}, {"order", "orders"}, {"orders", "orders"},
			{"group", "groups"}, {"groups", "groups"}, {"cargo", "cargos"}, {"cargos", "cargos"}}) {
		CAPTURE(kind);
		const std::optional<Json> result = DispatchGameTool("world.query", {{"kind", kind}}, CompanyID::Invalid());
		REQUIRE(result.has_value());
		CHECK((*result)["ok"] == true);
		/* The result always names the canonical kind, whichever spelling the caller used. */
		CHECK((*result)["kind"] == canonical);
	}
}

TEST_CASE("agent_game_tools: a company id that does not fit is rejected instead of truncated")
{
	/* 300 truncated to a uint8_t would silently read company 44's rows. */
	const std::optional<Json> top_level = DispatchGameTool("world.query", {{"kind", "vehicles"}, {"company_id", 300}}, CompanyID::Invalid());
	REQUIRE(top_level.has_value());
	CHECK((*top_level)["ok"] == false);
	CHECK((*top_level)["error"] == "invalid_arguments");
	CHECK((*top_level)["detail"]["company_id"] == 300);
	CHECK((*top_level)["detail"]["max"] == 255);

	const std::optional<Json> filter = DispatchGameTool("world.query", {{"kind", "engines"}, {"filters", {{"company_id", 300}}}}, CompanyID::Invalid());
	REQUIRE(filter.has_value());
	CHECK((*filter)["error"] == "invalid_arguments");
	CHECK((*filter)["detail"]["company_id"] == 300);

	const std::optional<Json> negative = DispatchGameTool("world.query", {{"kind", "orders"}, {"filters", {{"company_id", -1}}}}, CompanyID::Invalid());
	REQUIRE(negative.has_value());
	CHECK((*negative)["error"] == "invalid_arguments");

	/* A company id that does fit is used as it is. */
	const std::optional<Json> valid = DispatchGameTool("world.query", {{"kind", "vehicles"}, {"company_id", 2}}, CompanyID::Invalid());
	REQUIRE(valid.has_value());
	CHECK((*valid)["ok"] == true);
	CHECK((*valid)["total"] == 0);
}

TEST_CASE("agent_game_tools: every filter a kind documents is read without an error")
{
	const std::optional<Json> vehicles = DispatchGameTool("world.query", {{"kind", "vehicles"}, {"filters", {
			{"company_id", 0}, {"vehicle_type", "road"}, {"vehicle_id", 0}, {"group_id", 0}, {"cargo_id", 0},
			{"name_contains", "bus"}, {"in_depot", true}, {"stopped", false}}}}, CompanyID::Invalid());
	REQUIRE(vehicles.has_value());
	CHECK((*vehicles)["ok"] == true);

	const std::optional<Json> engines = DispatchGameTool("world.query", {{"kind", "engines"}, {"filters", {
			{"company_id", 0}, {"vehicle_type", "train"}, {"engine_id", 0}, {"cargo_id", 0},
			{"name_contains", "loco"}, {"refittable", true}}}}, CompanyID::Invalid());
	REQUIRE(engines.has_value());
	CHECK((*engines)["ok"] == true);

	const std::optional<Json> orders = DispatchGameTool("world.query", {{"kind", "orders"}, {"filters", {
			{"company_id", 0}, {"vehicle_id", 0}}}}, CompanyID::Invalid());
	REQUIRE(orders.has_value());
	CHECK((*orders)["ok"] == true);
}

TEST_CASE("agent_game_tools: a world.query parameter the kind does not read is rejected")
{
	const std::optional<Json> unknown = DispatchGameTool("world.query", {{"kind", "vehicles"}, {"colour", "red"}}, CompanyID::Invalid());
	REQUIRE(unknown.has_value());
	CHECK((*unknown)["ok"] == false);
	CHECK((*unknown)["error"] == "invalid_arguments");
	CHECK((*unknown)["detail"]["method"] == "world.query");
	CHECK((*unknown)["detail"]["op"] == "vehicles");
	CHECK((*unknown)["detail"]["unknown"] == Json::array({"colour"}));

	/* `limit`, `offset`, `filters` and `company_id` are read, so they are accepted. */
	const std::optional<Json> known = DispatchGameTool("world.query", {{"kind", "orders"}, {"limit", 5}, {"offset", 0}, {"company_id", 1}, {"filters", {{"vehicle_id", 0}}}}, CompanyID::Invalid());
	REQUIRE(known.has_value());
	CHECK((*known)["ok"] == true);
}

TEST_CASE("agent_game_tools: the page of a query is bounded and the applied page is reported")
{
	/* A caller cannot ask for the whole world in one answer. */
	const std::optional<Json> clamped = DispatchGameTool("world.query", {{"kind", "vehicles"}, {"limit", 1000}, {"offset", -5}}, CompanyID::Invalid());
	REQUIRE(clamped.has_value());
	CHECK((*clamped)["ok"] == true);
	CHECK((*clamped)["limit"] == 500);
	CHECK((*clamped)["offset"] == 0);
	CHECK((*clamped)["total"] == 0);
	CHECK((*clamped)["has_more"] == false);

	const std::optional<Json> smallest = DispatchGameTool("world.query", {{"kind", "engines"}, {"limit", 0}}, CompanyID::Invalid());
	REQUIRE(smallest.has_value());
	CHECK((*smallest)["limit"] == 1);

	const std::optional<Json> defaulted = DispatchGameTool("world.query", {{"kind", "groups"}}, CompanyID::Invalid());
	REQUIRE(defaulted.has_value());
	CHECK((*defaulted)["limit"] == 100);
	CHECK((*defaulted)["offset"] == 0);
}

TEST_CASE("agent_game_tools: the ownership scope of a query is explicit")
{
	/* No company named and no company of the call: every owner, and the answer says so. */
	const std::optional<Json> all = DispatchGameTool("world.query", {{"kind", "vehicles"}}, CompanyID::Invalid());
	REQUIRE(all.has_value());
	CHECK((*all)["scope"]["company_id"].is_null());

	/* The company of the call scopes a company scoped kind, and the scope is reported. */
	const std::optional<Json> scoped = DispatchGameTool("world.query", {{"kind", "vehicles"}}, CompanyID(2));
	REQUIRE(scoped.has_value());
	CHECK((*scoped)["scope"]["company_id"] == 2);

	/* `company_id: null` asks for every owner even when the call names a company. */
	const std::optional<Json> unscoped = DispatchGameTool("world.query", {{"kind", "vehicles"}, {"company_id", nullptr}}, CompanyID(2));
	REQUIRE(unscoped.has_value());
	CHECK((*unscoped)["scope"]["company_id"].is_null());

	/* An explicit company wins over the company of the call, in the filter and at the top level. */
	const std::optional<Json> filtered = DispatchGameTool("world.query", {{"kind", "vehicles"}, {"filters", {{"company_id", 1}}}}, CompanyID(2));
	REQUIRE(filtered.has_value());
	CHECK((*filtered)["scope"]["company_id"] == 1);

	/* A global kind is never scoped to a company, even when the call names one. */
	const std::optional<Json> cargos = DispatchGameTool("world.query", {{"kind", "cargos"}}, CompanyID(2));
	REQUIRE(cargos.has_value());
	CHECK((*cargos)["scope"]["company_id"].is_null());
}

TEST_CASE("agent_game_tools: group and cargo filters are validated instead of ignored")
{
	const std::optional<Json> unknown = DispatchGameTool("world.query", {{"kind", "groups"}, {"filters", {{"in_depot", true}}}}, CompanyID::Invalid());
	REQUIRE(unknown.has_value());
	CHECK((*unknown)["error"] == "invalid_arguments");
	CHECK((*unknown)["detail"]["kind"] == "groups");
	CHECK((*unknown)["detail"]["filter"] == "in_depot");

	/* A group id outside the group pool is rejected instead of wrapping onto another group. */
	const std::optional<Json> bad_group = DispatchGameTool("world.query", {{"kind", "groups"}, {"filters", {{"group_id", 0xFF000}}}}, CompanyID::Invalid());
	REQUIRE(bad_group.has_value());
	CHECK((*bad_group)["error"] == "invalid_arguments");
	CHECK((*bad_group)["detail"]["max"] == 63999);

	const std::optional<Json> bad_protection = DispatchGameTool("world.query", {{"kind", "groups"}, {"filters", {{"replace_protection", "yes"}}}}, CompanyID::Invalid());
	REQUIRE(bad_protection.has_value());
	CHECK((*bad_protection)["error"] == "invalid_arguments");

	const std::optional<Json> bad_label = DispatchGameTool("world.query", {{"kind", "cargos"}, {"filters", {{"label", "PASSENGER"}}}}, CompanyID::Invalid());
	REQUIRE(bad_label.has_value());
	CHECK((*bad_label)["error"] == "invalid_arguments");

	const std::optional<Json> bad_freight = DispatchGameTool("world.query", {{"kind", "cargos"}, {"filters", {{"is_freight", 1}}}}, CompanyID::Invalid());
	REQUIRE(bad_freight.has_value());
	CHECK((*bad_freight)["error"] == "invalid_arguments");

	/* An unbounded name filter is rejected, so a query cannot scan for an arbitrary pattern. */
	const std::optional<Json> long_name = DispatchGameTool("world.query", {{"kind", "vehicles"}, {"filters", {{"name_contains", std::string(65, 'a')}}}}, CompanyID::Invalid());
	REQUIRE(long_name.has_value());
	CHECK((*long_name)["error"] == "invalid_arguments");
	CHECK((*long_name)["detail"]["max_length"] == 64);
}

TEST_CASE("agent_game_tools: vehicle ids are read at their real width")
{
	/* 70000 is a valid vehicle id; a uint16_t conversion would silently turn it into another id. */
	const std::optional<Json> wide = DispatchGameTool("world.query", {{"kind", "vehicles"}, {"filters", {{"vehicle_id", 70000}}}}, CompanyID::Invalid());
	REQUIRE(wide.has_value());
	CHECK((*wide)["ok"] == true);

	const std::optional<Json> outside = DispatchGameTool("world.query", {{"kind", "vehicles"}, {"filters", {{"vehicle_id", 0xFF000}}}}, CompanyID::Invalid());
	REQUIRE(outside.has_value());
	CHECK((*outside)["error"] == "invalid_arguments");
	CHECK((*outside)["detail"]["max"] == 0xFEFFF);
}

TEST_CASE("agent_game_tools: the cargo inventory reads the real cargo table when it is initialised")
{
	const std::optional<Json> result = DispatchGameTool("world.query", {{"kind", "cargos"}}, CompanyID::Invalid());
	REQUIRE(result.has_value());
	CHECK((*result)["ok"] == true);
	CHECK((*result)["kind"] == "cargos");
	REQUIRE((*result)["rows"].is_array());

	if (!CargoSpec::Get(0)->IsValid()) {
		/* No game initialised the cargo table in this binary; the rows can only be checked in a
		 * real game, because the cargo names come from the language pack the game loads. */
		WARN("the cargo table is not initialised in this test binary");
		CHECK((*result)["total"] == 0);
		return;
	}

	REQUIRE((*result)["total"] > 0);
	const Json &first = (*result)["rows"].front();
	REQUIRE(first["label"].is_string());
	CHECK(first["label"].get<std::string>().size() == 4);
	REQUIRE(first["name"].is_string());
	CHECK_FALSE(first["name"].get<std::string>().empty());
	CHECK(first["is_freight"].is_boolean());

	/* The label filter selects exactly the cargo the first row described. */
	const std::string label = first["label"].get<std::string>();
	const std::optional<Json> by_label = DispatchGameTool("world.query", {{"kind", "cargos"}, {"filters", {{"label", label}}}}, CompanyID::Invalid());
	REQUIRE(by_label.has_value());
	CHECK((*by_label)["ok"] == true);
	REQUIRE((*by_label)["total"] == 1);
	CHECK((*by_label)["rows"].front()["label"] == label);
	CHECK((*by_label)["rows"].front()["name"] == first["name"]);
}

/**
 * Dispatch a write op with one replaced parameter value and return the result.
 * @param method Engine method name.
 * @param op Op of the method.
 * @param base Parameters of a usable request.
 * @param key Parameter to replace.
 * @param value Value to use.
 * @return The tool result.
 */
static Json WriteWith(const char *method, const char *op, Json base, const char *key, Json value)
{
	base["op"] = op;
	base[key] = std::move(value);
	const std::optional<Json> result = DispatchGameTool(method, base, CompanyID(3));
	REQUIRE(result.has_value());
	return *result;
}

TEST_CASE("agent_game_tools: a write id is checked against its pool before the cast")
{
	/* The largest valid vehicle id is accepted and reaches the company check. */
	const Json max_vehicle = WriteWith("world.vehicles", "start", {{"vehicle_id", 0}}, "vehicle_id", 0xFEFFF);
	CHECK(max_vehicle["error"] == "invalid_company");

	/* One past the last vehicle id would wrap onto another vehicle. */
	const Json over_vehicle = WriteWith("world.vehicles", "start", {{"vehicle_id", 0}}, "vehicle_id", 0xFF000);
	CHECK(over_vehicle["error"] == "invalid_params");
	CHECK(over_vehicle["detail"]["vehicle_id"] == 0xFF000);
	CHECK(over_vehicle["detail"]["max"] == 0xFEFFF);

	/* 2^32 + a real id used to become that real id after a 32 bit cast. */
	const Json wide_vehicle = WriteWith("world.vehicles", "start", {{"vehicle_id", 0}}, "vehicle_id", int64_t{4294967296} + 3);
	CHECK(wide_vehicle["error"] == "invalid_params");
	CHECK(wide_vehicle["detail"]["vehicle_id"] == int64_t{4294967296} + 3);

	const Json negative_vehicle = WriteWith("world.vehicles", "start", {{"vehicle_id", 0}}, "vehicle_id", -1);
	CHECK(negative_vehicle["error"] == "invalid_params");
	CHECK(negative_vehicle["detail"]["max"] == 0xFEFFF);

	/* A float and a boolean are not ids, they must not become 1 or 0. */
	const Json float_vehicle = WriteWith("world.vehicles", "start", {{"vehicle_id", 0}}, "vehicle_id", 1.5);
	CHECK(float_vehicle["error"] == "invalid_params");

	const Json bool_vehicle = WriteWith("world.vehicles", "start", {{"vehicle_id", 0}}, "vehicle_id", true);
	CHECK(bool_vehicle["error"] == "invalid_params");

	/* The same rule for every other pool a write op names. */
	const Json max_engine = WriteWith("world.vehicles", "build", {{"engine_id", 0}, {"depot_id", 0}}, "engine_id", 63999);
	CHECK(max_engine["error"] == "invalid_company");

	const Json over_engine = WriteWith("world.vehicles", "build", {{"engine_id", 0}, {"depot_id", 0}}, "engine_id", 64000);
	CHECK(over_engine["error"] == "invalid_params");
	CHECK(over_engine["detail"]["max"] == 63999);

	const Json over_depot = WriteWith("world.vehicles", "build", {{"engine_id", 0}, {"depot_id", 0}}, "depot_id", 64000);
	CHECK(over_depot["error"] == "invalid_params");
	CHECK(over_depot["detail"]["max"] == 63999);

	const Json over_share = WriteWith("world.vehicles", "build", {{"engine_id", 0}, {"depot_id", 0}}, "share_orders_from", 0xFF000);
	CHECK(over_share["error"] == "invalid_params");

	const Json over_sell_depot = WriteWith("world.vehicles", "sell", {{"vehicle_id", 0}}, "depot_id", 64000);
	CHECK(over_sell_depot["error"] == "invalid_params");

	const Json over_clone_depot = WriteWith("world.vehicles", "clone", {{"vehicle_id", 0}}, "depot_id", 64000);
	CHECK(over_clone_depot["error"] == "invalid_params");

	const Json max_group = WriteWith("world.orders", "group", {{"vehicle_id", 0}}, "group_id", 63999);
	CHECK(max_group["error"] == "invalid_company");

	const Json over_group = WriteWith("world.orders", "group", {{"vehicle_id", 0}}, "group_id", 64000);
	CHECK(over_group["error"] == "invalid_params");
	CHECK(over_group["detail"]["max"] == 63999);

	const Json over_source = WriteWith("world.orders", "shared", {{"vehicle_id", 0}, {"share_orders_from", 0}}, "share_orders_from", 0xFF000);
	CHECK(over_source["error"] == "invalid_params");
	CHECK(over_source["detail"]["max"] == 0xFEFFF);
}

TEST_CASE("agent_game_tools: order ids and subtypes are checked against their pools before the cast")
{
	/* A station id and a depot id share the 16 bit station pool; 0xFA00 used to become station 0. */
	const Json over_station = WriteWith("world.orders", "set", {{"vehicle_id", 0}, {"orders", Json::array()}}, "orders",
			Json::array({Json{{"type", "station"}, {"id", 64000}}}));
	CHECK(over_station["error"] == "invalid_params");
	CHECK(over_station["detail"]["max"] == 63999);

	const Json over_waypoint = WriteWith("world.orders", "set", {{"vehicle_id", 0}, {"orders", Json::array()}}, "orders",
			Json::array({Json{{"type", "waypoint"}, {"id", 64000}}}));
	CHECK(over_waypoint["error"] == "invalid_params");

	const Json over_order_depot = WriteWith("world.orders", "set", {{"vehicle_id", 0}, {"orders", Json::array()}}, "orders",
			Json::array({Json{{"type", "depot"}, {"id", 64000}}}));
	CHECK(over_order_depot["error"] == "invalid_params");

	const Json negative_station = WriteWith("world.orders", "set", {{"vehicle_id", 0}, {"orders", Json::array()}}, "orders",
			Json::array({Json{{"type", "station"}, {"id", -1}}}));
	CHECK(negative_station["error"] == "invalid_params");

	const Json float_station = WriteWith("world.orders", "set", {{"vehicle_id", 0}, {"orders", Json::array()}}, "orders",
			Json::array({Json{{"type", "station"}, {"id", 1.5}}}));
	CHECK(float_station["error"] == "invalid_params");

	const Json bool_station = WriteWith("world.orders", "set", {{"vehicle_id", 0}, {"orders", Json::array()}}, "orders",
			Json::array({Json{{"type", "station"}, {"id", true}}}));
	CHECK(bool_station["error"] == "invalid_params");

	/* The last valid station id is accepted and reaches the company check. */
	const Json max_station = WriteWith("world.orders", "set", {{"vehicle_id", 0}, {"orders", Json::array()}}, "orders",
			Json::array({Json{{"type", "station"}, {"id", 63999}}}));
	CHECK(max_station["error"] == "invalid_company");

	/* The cargo subtype of an order is bounded by the cargo table, not by 8 bits. */
	const Json over_order_cargo = WriteWith("world.orders", "set", {{"vehicle_id", 0}, {"orders", Json::array()}}, "orders",
			Json::array({Json{{"type", "station"}, {"id", 0}, {"refit_cargo_id", 64}}}));
	CHECK(over_order_cargo["error"] == "invalid_params");
	CHECK(over_order_cargo["detail"]["max"] == 63);

	/* A timetable index is a VehicleOrderID: 0xFF is the invalid marker, so 0xFE is the last one. */
	const Json max_index = WriteWith("world.orders", "timetable", {{"vehicle_id", 0}, {"timetable", Json::array()}}, "timetable",
			Json::array({Json{{"index", 0xFE}, {"wait_time", 10}}}));
	CHECK(max_index["error"] == "invalid_company");

	const Json over_index = WriteWith("world.orders", "timetable", {{"vehicle_id", 0}, {"timetable", Json::array()}}, "timetable",
			Json::array({Json{{"index", 0xFF}, {"wait_time", 10}}}));
	CHECK(over_index["error"] == "invalid_params");
	CHECK(over_index["detail"]["max"] == 0xFE);

	/* A value that used to be truncated to 16 bits is rejected, not wrapped. */
	const Json over_wait = WriteWith("world.orders", "timetable", {{"vehicle_id", 0}, {"timetable", Json::array()}}, "timetable",
			Json::array({Json{{"index", 0}, {"wait_time", 65536}}}));
	CHECK(over_wait["error"] == "invalid_arguments");
	CHECK(over_wait["detail"]["max"] == 65535);
}

namespace {

/**
 * A real company in the engine's own company pool, removed again when the guard goes out of scope.
 *
 * The vehicles of the cases below belong to a company, so the owner they carry is a real one: an
 * invalid owner would make the engine's own teardown paths look up a company that does not exist.
 */
class TestCompanyGuard {
public:
	TestCompanyGuard()
	{
		/* The pool asserts that the allocation was checked, exactly as the blueprint cases do. */
		REQUIRE(Company::CanAllocateItem());
		this->company = Company::Create();
		REQUIRE(this->company != nullptr);
	}

	~TestCompanyGuard()
	{
		if (this->company != nullptr) delete this->company;
	}

	CompanyID Id() const { return this->company->index; }

private:
	Company *company = nullptr; ///< The company, or nullptr when it was not created.
};

/**
 * Three real vehicles in the engine's own vehicle pool, removed again when the fixture goes out of
 * scope.
 *
 * The vehicles are genuine pool items, so their index and lifetime generation are the engine's own
 * and the counters read from them are the fields the engine itself uses. The fixture requires an
 * empty vehicle pool first, so the cleanup only ever removes the three vehicles it created itself
 * and can never delete an item another case is responsible for. Removal uses the pool's own
 * `CleanPool`: that is the engine's path for deleting items without the per-vehicle teardown (the
 * company's unit number, the group statistics, the tile hash and the windows) that needs a running
 * game, and it resets the pool completely, so a case that runs afterwards still sees an empty pool.
 *
 * Every check that can fail runs before the first allocation: `PoolItem::Create` only fails with a
 * fatal error (`GetNew` on a full pool), so no statement after the first `Create` can throw and a
 * failed `REQUIRE` can never leave a vehicle behind in the pool.
 */
class TestVehicles {
public:
	TestVehicles()
	{
		/* The pool asserts that the allocation was checked, exactly as the other agent cases do.
		 * `CanAllocate` *assigns* the number it was asked for (`checked = n`, core/pool_type.hpp:193-196)
		 * and `GetNew` decrements it once per allocation (core/pool_func.hpp:119-120), so the three
		 * vehicles are reserved with one call: three separate `CanAllocateItem()` calls would leave
		 * `checked` at 1 and the second `Create` would trip the assertion. */
		REQUIRE(Vehicle::GetNumItems() == 0);
		REQUIRE(Vehicle::CanAllocateItem(3));

		this->train = Train::Create();
		this->aircraft = Aircraft::Create();
		this->road = RoadVehicle::Create();
	}

	~TestVehicles() { _vehicle_pool.CleanPool(); }

	Train *train = nullptr; ///< The train the fixture created.
	Aircraft *aircraft = nullptr; ///< The aircraft the fixture created.
	RoadVehicle *road = nullptr; ///< The road vehicle the fixture created.
};

/**
 * Load the language pack the build generated, so this binary resolves real strings.
 *
 * The test binary has no language pack of its own: `GetString()` aborts on the first string that is
 * not in the loaded table ("String 0x8820 is invalid"), and an engine name is resolved through
 * `STR_ENGINE_NAME` plus the engine's own name string. The pack is the real `english.lng` the build
 * writes to `${CMAKE_BINARY_DIR}/lang`; CTest passes its path in `OTTD_TEST_LANGUAGE_FILE`, and the
 * relative path is the fallback for a manual run from the build directory.
 *
 * The metadata is static because `ReadLanguagePack()` keeps the pointer it is given as the current
 * language, so a stack object would dangle after this function returns.
 */
void LoadTestLanguagePack()
{
	static LanguageMetadata english;
	if (_current_language == &english) return;

	std::vector<std::filesystem::path> candidates;
	if (const char *from_ctest = getenv("OTTD_TEST_LANGUAGE_FILE"); from_ctest != nullptr && *from_ctest != '\0') {
		candidates.emplace_back(from_ctest);
	}
	candidates.emplace_back(std::filesystem::path("lang") / "english.lng");

	for (const std::filesystem::path &candidate : candidates) {
		if (!std::filesystem::exists(candidate)) continue;
		english.file = candidate;
		REQUIRE(ReadLanguagePack(&english));
		return;
	}
	FAIL("no english.lng found: CTest passes OTTD_TEST_LANGUAGE_FILE, a manual run needs the build directory as its working directory");
}

/**
 * One real engine in the engine's own pool, removed again when the fixture goes out of scope.
 *
 * The engine is created through the pool's own `CreateAtIndex`, the path the engine itself uses when
 * it sets up the default engines, so its id and lifetime generation are the engine's own. Removal
 * uses the pool's `CleanPool`, which needs no running game. Every check that can fail runs before
 * the allocation, so a failed `REQUIRE` never leaves an engine behind.
 */
class TestEngines {
public:
	TestEngines()
	{
		REQUIRE(Engine::GetNumItems() == 0);
		REQUIRE(Engine::CanAllocateItem());
		this->engine = Engine::CreateAtIndex(EngineID(0), VehicleType::Road, 0);
		REQUIRE(this->engine != nullptr);
	}

	~TestEngines() { _engine_pool.CleanPool(); }

	Engine *engine = nullptr; ///< The engine the fixture created.
};

} // namespace

TEST_CASE("agent_game_tools: a vehicle row reports the engine's own raw wait counters")
{
	/* The company is declared first, so the vehicles are removed before it. */
	TestCompanyGuard company;
	TestVehicles vehicles;

	/* A train carries the signal/stuck/forced-proceeding counter and the common counters. */
	vehicles.train->owner = company.Id();
	vehicles.train->wait_counter = 37;
	vehicles.train->load_unload_ticks = 11;
	vehicles.train->current_order_time = 123;

	const Json train_wait = VehicleWaitJson(vehicles.train);
	CHECK(train_wait["wait_counter"] == 37);
	CHECK(train_wait["load_unload_ticks"] == 11);
	CHECK(train_wait["current_order_time"] == 123);
	/* A train has no turn throttle: the field is null, not a made up zero. */
	CHECK(train_wait["turn_counter"].is_null());

	/* An unrelated vehicle type keeps the common counters and carries its own counter instead. */
	vehicles.aircraft->owner = company.Id();
	vehicles.aircraft->turn_counter = 5;
	vehicles.aircraft->load_unload_ticks = 2;
	vehicles.aircraft->current_order_time = 456;

	const Json aircraft_wait = VehicleWaitJson(vehicles.aircraft);
	CHECK(aircraft_wait["turn_counter"] == 5);
	CHECK(aircraft_wait["load_unload_ticks"] == 2);
	CHECK(aircraft_wait["current_order_time"] == 456);
	/* The aircraft has no signal counter, so the train's counter can never leak into its row. */
	CHECK(aircraft_wait["wait_counter"].is_null());

	/* A road vehicle has neither of the two type specific counters, while its own counters are
	 * still numbers: a zero there is a value, not the null that means "no such counter". */
	vehicles.road->owner = company.Id();
	vehicles.road->load_unload_ticks = 3;

	const Json road_wait = VehicleWaitJson(vehicles.road);
	CHECK(road_wait["load_unload_ticks"] == 3);
	CHECK(road_wait["current_order_time"] == 0);
	CHECK(road_wait["wait_counter"].is_null());
	CHECK(road_wait["turn_counter"].is_null());

	/* `current_order_time` is signed and the engine marks "not known" with -1
	 * (`TimerGameTick::Ticks::INVALID_TICKS`): the row reports that value as it is, so it is never wrapped
	 * into a large unsigned number and a caller can tell "not known" from a real tick count. */
	vehicles.road->current_order_time = -1;
	const Json road_unknown = VehicleWaitJson(vehicles.road);
	CHECK(road_unknown["current_order_time"] == -1);
}

TEST_CASE("agent_game_tools: a vehicle row reports the real road type, not the vehicle type")
{
	/* Without a game the road type masks are empty, so RoadTypeIsTram() would answer for nothing. The
	 * engine's own defaults install the real road and tram bits; no other case in this binary reads
	 * the masks, and they are the state a running game has. */
	ResetRoadTypes();

	/* The company is declared first, so the vehicles are removed before it. */
	TestCompanyGuard company;
	TestVehicles vehicles;

	/* One real road vehicle is read as a bus, then as a tram, then without a road type at all. */
	vehicles.road->owner = company.Id();
	vehicles.road->type = VehicleType::Road;
	vehicles.road->roadtype = ROADTYPE_ROAD;

	const Json bus_fields = VehicleRoadTypeFieldsForTest(vehicles.road);
	CHECK(bus_fields["road_type_id"] == 0);
	CHECK(bus_fields["road_type"] == "road");
	CHECK(bus_fields["is_tram"] == false);

	vehicles.road->roadtype = ROADTYPE_TRAM;
	const Json tram_fields = VehicleRoadTypeFieldsForTest(vehicles.road);
	CHECK(tram_fields["road_type_id"] == 1);
	CHECK(tram_fields["road_type"] == "tram");
	CHECK(tram_fields["is_tram"] == true);

	/* A road vehicle without a road type has no family: the fields are null, not false. */
	vehicles.road->roadtype = INVALID_ROADTYPE;
	const Json unknown = VehicleRoadTypeFieldsForTest(vehicles.road);
	CHECK(unknown["road_type_id"].is_null());
	CHECK(unknown["road_type"].is_null());
	CHECK(unknown["is_tram"].is_null());

	/* A train has no road type at all, so its row says nothing about one. */
	const Json train_fields = VehicleRoadTypeFieldsForTest(vehicles.train);
	CHECK(train_fields["road_type_id"].is_null());
	CHECK(train_fields["road_type"].is_null());
	CHECK(train_fields["is_tram"].is_null());
}

TEST_CASE("agent_game_tools: a station order the tool builds is one CmdInsertOrder accepts")
{
	/* Order::MakeGoToStation() leaves the stop location at the near end, which CmdInsertOrder
	 * refuses for everything but a train; a bus, tram, ship or aircraft order built from it failed
	 * with a bare command failure in game. The tool has to ask for the far end, the value the order
	 * window uses for every non-train. */
	Order order;
	const std::optional<Json> error = ReadOrderForTest(Json{{"type", "station"}, {"id", 0}, {"load", "full"}, {"unload", "transfer"}}, order);
	REQUIRE_FALSE(error.has_value());

	CHECK(order.GetType() == OT_GOTO_STATION);
	CHECK(order.GetStopLocation() == OrderStopLocation::FarEnd);
	CHECK(order.GetDestination().ToStationID().base() == 0);

	/* The rest of the order stays at the defaults CmdInsertOrder insists on, and the load/unload
	 * behaviour the caller asked for survives. */
	CHECK(order.GetRefitCargo() == CARGO_NO_REFIT);
	CHECK(order.GetWaitTime() == 0);
	CHECK(order.GetTravelTime() == 0);
	CHECK(order.GetMaxSpeed() == UINT16_MAX);
	CHECK(order.GetLoadType() == OrderLoadType::FullLoad);
	CHECK(order.GetUnloadType() == OrderUnloadType::Transfer);

	/* The far end is the only stop location CmdInsertOrder accepts for road, tram, ship and air, and
	 * it accepts it for trains too. The bare MakeGoToStation() order the tool used to post carried the
	 * near end, which the engine refuses for every non-train. */
	Order bare;
	bare.MakeGoToStation(StationID(0));
	CHECK(bare.GetStopLocation() == OrderStopLocation::NearEnd);
	CHECK(order.GetStopLocation() != bare.GetStopLocation());
}

TEST_CASE("agent_game_tools: a station order with unusable parameters is still refused")
{
	Order order;

	/* The fix only adds the stop location; every argument error the tool already reported stays. */
	CHECK(ReadOrderForTest(Json{{"type", "station"}}, order).has_value());
	CHECK(ReadOrderForTest(Json{{"type", "station"}, {"id", -1}}, order).has_value());
	CHECK(ReadOrderForTest(Json{{"type", "station"}, {"id", "zero"}}, order).has_value());
	CHECK(ReadOrderForTest(Json{{"type", "station"}, {"id", 0}, {"load", "bogus"}}, order).has_value());
	CHECK(ReadOrderForTest(Json{{"type", "station"}, {"id", 0}, {"unload", "bogus"}}, order).has_value());
	CHECK(ReadOrderForTest(Json{{"type", "station"}, {"id", 0}, {"refit_cargo_id", static_cast<int64_t>(NUM_CARGO)}}, order).has_value());
	CHECK(ReadOrderForTest(Json{{"type", "platform"}, {"id", 0}}, order).has_value());
}

TEST_CASE("agent_game_tools: an engine row reports the name the game shows")
{
	LoadTestLanguagePack();

	TestEngines engines;
	Engine *e = engines.engine;
	/* A real default engine name string: the English pack has it, so the row can be checked against
	 * the text a player sees instead of against the helper that produced it. */
	e->info.string_id = STR_VEHICLE_NAME_ROAD_VEHICLE_MPS_REGAL_BUS;
	/* The engine's own rule: the custom name is shown only while the engine is enabled. */
	e->info.climates.Set(_settings_game.game_creation.landscape);
	e->name = "Renamed Bus";
	REQUIRE(e->IsEnabled());

	/* A renamed engine: the row reports the name the game shows, and the override on its own. */
	const std::optional<Json> renamed = DispatchGameTool("world.query", {{"kind", "engines"}}, CompanyID::Invalid());
	REQUIRE(renamed.has_value());
	REQUIRE((*renamed)["ok"] == true);
	REQUIRE((*renamed)["total"] == 1);
	const Json &row = (*renamed)["rows"].front();
	CHECK(row["name"] == "Renamed Bus");
	CHECK(row["custom_name"] == "Renamed Bus");

	/* The other fields of the row are untouched. */
	CHECK(row["id"] == 0);
	CHECK(row["type"] == "road");
	CHECK(row["refittable"] == false);
	CHECK(row["enabled"] == true);
	CHECK_FALSE(row.contains("available"));

	/* The text filter reads the same name the row reports. */
	const std::optional<Json> filtered = DispatchGameTool("world.query", {{"kind", "engines"}, {"filters", {{"name_contains", "Renamed"}}}}, CompanyID::Invalid());
	REQUIRE(filtered.has_value());
	CHECK((*filtered)["total"] == 1);
	const std::optional<Json> missing = DispatchGameTool("world.query", {{"kind", "engines"}, {"filters", {{"name_contains", "No Such Engine"}}}}, CompanyID::Invalid());
	REQUIRE(missing.has_value());
	CHECK((*missing)["total"] == 0);

	/* Without an override the row reports the engine's own default name, resolved from the language
	 * pack, and there is no custom name. */
	e->name.clear();
	const std::optional<Json> plain = DispatchGameTool("world.query", {{"kind", "engines"}}, CompanyID::Invalid());
	REQUIRE(plain.has_value());
	REQUIRE((*plain)["total"] == 1);
	CHECK((*plain)["rows"].front()["custom_name"].is_null());
	CHECK((*plain)["rows"].front()["name"] == "MPS Regal Bus");

	/* A disabled engine does not show its override: the row follows the engine's resolution instead of
	 * reporting the raw field, which is what the tool used to do. */
	e->name = "Renamed Bus";
	e->info.climates = LandscapeTypes{};
	REQUIRE_FALSE(e->IsEnabled());
	const std::optional<Json> disabled = DispatchGameTool("world.query", {{"kind", "engines"}}, CompanyID::Invalid());
	REQUIRE(disabled.has_value());
	REQUIRE((*disabled)["total"] == 1);
	const Json &disabled_row = (*disabled)["rows"].front();
	CHECK(disabled_row["custom_name"] == "Renamed Bus");
	CHECK(disabled_row["name"] == "MPS Regal Bus");

	/* The filter follows the display name too: a disabled engine is found by its default name, not by
	 * the override it does not show. */
	const std::optional<Json> by_default = DispatchGameTool("world.query", {{"kind", "engines"}, {"filters", {{"name_contains", "Regal"}}}}, CompanyID::Invalid());
	REQUIRE(by_default.has_value());
	CHECK((*by_default)["total"] == 1);
	const std::optional<Json> by_override = DispatchGameTool("world.query", {{"kind", "engines"}, {"filters", {{"name_contains", "Renamed"}}}}, CompanyID::Invalid());
	REQUIRE(by_override.has_value());
	CHECK((*by_override)["total"] == 0);
}

TEST_CASE("agent_game_tools: a vehicle row reports the engine's display name")
{
	LoadTestLanguagePack();

	TestEngines engines;
	Engine *e = engines.engine;
	e->info.string_id = STR_VEHICLE_NAME_ROAD_VEHICLE_MPS_REGAL_BUS;
	e->info.climates.Set(_settings_game.game_creation.landscape);
	e->name = "Renamed Bus";
	REQUIRE(e->IsEnabled());

	/* The company is declared first, so the vehicles are removed before it. */
	TestCompanyGuard company;
	TestVehicles vehicles;
	vehicles.road->owner = company.Id();
	vehicles.road->engine_type = e->index;

	const Json named = VehicleEngineNameFieldsForTest(vehicles.road);
	CHECK(named["engine_name"] == "Renamed Bus");
	CHECK(named["engine_custom_name"] == "Renamed Bus");

	/* Without an override the field carries the engine's default name, not an empty string. */
	e->name.clear();
	const Json plain = VehicleEngineNameFieldsForTest(vehicles.road);
	CHECK(plain["engine_name"] == "MPS Regal Bus");
	CHECK(plain["engine_custom_name"].is_null());
}

TEST_CASE("agent_game_tools: an order's refit is a separate OrderRefit, not part of the insert")
{
	/* Commands::InsertOrder only accepts an order whose refit is the default, so the tool inserts the
	 * order without one and applies the refit with Commands::OrderRefit for the inserted index. */
	const Json plan = OrdersPlanForTest(Json::array({
		Json{{"type", "station"}, {"id", 0}, {"refit_cargo_id", 1}},
		Json{{"type", "station"}, {"id", 1}},
		Json{{"type", "depot"}, {"id", 0}, {"refit_cargo_id", 2}},
	}));
	REQUIRE(plan["ok"] == true);
	REQUIRE(plan["orders"].size() == 3);

	/* The inserted order carries no refit at all; the refit cargo waits for the second command. */
	CHECK(plan["orders"][0]["type"] == "station");
	CHECK(plan["orders"][0]["insert_refit_cargo_id"].is_null());
	CHECK(plan["orders"][0]["order_refit_cargo_id"] == 1);

	/* An order without a refit needs no second command. */
	CHECK(plan["orders"][1]["insert_refit_cargo_id"].is_null());
	CHECK(plan["orders"][1]["order_refit_cargo_id"].is_null());

	/* A depot order can carry a refit too. */
	CHECK(plan["orders"][2]["type"] == "depot");
	CHECK(plan["orders"][2]["insert_refit_cargo_id"].is_null());
	CHECK(plan["orders"][2]["order_refit_cargo_id"] == 2);
}

TEST_CASE("agent_game_tools: an unsupported refit is refused before the order list is touched")
{
	/* Commands::OrderRefit only refits station and depot orders, so a waypoint order that asks for a
	 * refit is rejected up front: `set` must not delete the old list for a request it cannot finish. */
	const Json waypoint = OrdersPlanForTest(Json::array({Json{{"type", "waypoint"}, {"id", 0}, {"refit_cargo_id", 1}}}));
	CHECK(waypoint["ok"] == false);
	CHECK(waypoint["order_index"] == 0);
	CHECK(waypoint["error"]["error"] == "invalid_arguments");
	CHECK(waypoint["error"]["detail"]["order_index"] == 0);
	CHECK(waypoint["error"]["detail"]["reason"] == "only station and depot orders can carry a refit");

	/* An order that does not load cannot be refitted either, exactly as Commands::OrderRefit says. */
	const Json no_load = OrdersPlanForTest(Json::array({Json{{"type", "station"}, {"id", 0}, {"refit_cargo_id", 1}, {"load", "none"}}}));
	CHECK(no_load["ok"] == false);
	CHECK(no_load["error"]["error"] == "invalid_arguments");
	CHECK(no_load["error"]["detail"]["reason"] == "an order that does not load cannot be refitted");

	/* The order that is fine on its own still passes, so the check rejects the refit and not the order. */
	const Json loading = OrdersPlanForTest(Json::array({Json{{"type", "station"}, {"id", 0}, {"refit_cargo_id", 1}, {"load", "full"}}}));
	REQUIRE(loading["ok"] == true);
	CHECK(loading["orders"][0]["order_refit_cargo_id"] == 1);

	/* A parameter error keeps its own error, before any refit check. */
	const Json no_id = OrdersPlanForTest(Json::array({Json{{"type", "station"}, {"refit_cargo_id", 1}}}));
	CHECK(no_id["ok"] == false);
	CHECK(no_id["error"]["error"] == "invalid_arguments");
}

namespace {

/**
 * A flat map of real tiles for the depot entrance case.
 *
 * The engine's own map is allocated and every tile is set to flat grass, so the road bits and the
 * depot below are real map state that the engine's accessors read back. No running game is needed
 * for that, exactly as the rail route cases show; the map is not freed because the next case that
 * needs one allocates over it.
 */
class TestRoadMap {
public:
	TestRoadMap()
	{
		Map::Allocate(128, 128);
		/* `TileIndex`, not `Tile`: this file has its own `Tile(x, y)` helper, which hides the type. */
		for (TileIndex t : Map::Iterate()) {
			MakeClear(t, ClearGround::Grass, 3);
			SetTileHeight(t, 3);
			if (TileX(t) == Map::MaxX() || TileY(t) == Map::MaxY()) MakeVoid(t);
		}
		/* The engine's own road type tables: without a game the road/tram masks are empty. */
		ResetRoadTypes();
	}
};

} // namespace

TEST_CASE("agent_game_tools: a depot entrance reports the road that is really there")
{
	/* The entrance of a depot is the tile the vehicles drive out into. The engine lets them leave
	 * only onto a road piece on the side of that tile which faces the depot, and only if that tile
	 * offers a track the vehicle can drive on; a depot built against the side of a straight road is
	 * accepted and then traps every vehicle it releases. Every case below is real map state: real
	 * road pieces and real depot tiles read back through the engine's own accessors. */
	TestRoadMap map;

	/* A straight road along the x axis: its pieces are the south-west and north-east ones. */
	for (uint x = 8; x <= 11; x++) {
		MakeRoadNormal(TileXY(x, 10), ROAD_X, ROADTYPE_ROAD, INVALID_ROADTYPE, TownID::Invalid(), OWNER_NONE, OWNER_NONE);
	}

	/* In line with the road: the exit points along the road, so the piece that faces the depot is
	 * there and a vehicle can leave. */
	MakeRoadDepot(TileXY(7, 10), OWNER_NONE, DepotID(0), DiagDirection::SW, ROADTYPE_ROAD);
	REQUIRE(IsRoadDepotTile(TileXY(7, 10)));

	const Json aligned = RoadDepotEntranceJson(TileXY(7, 10));
	CHECK(aligned["direction"] == "sw");
	CHECK(aligned["tile"]["x"] == 8);
	CHECK(aligned["tile"]["y"] == 10);
	CHECK(aligned["required_road_bit"] == "ne");
	CHECK(Contains(aligned["road_bits"], "ne"));
	CHECK(Contains(aligned["road_bits"], "sw"));
	CHECK(aligned["has_required_bit"] == true);
	CHECK(aligned["drivable"] == true);
	CHECK(aligned["connected"] == true);

	/* Beside the road: the exit points at the side of the same straight road, whose pieces are the
	 * other pair, so the neighbour has nothing on the side the vehicle enters it from. This is the
	 * layout the end-to-end case built, and the bus never left the depot tile. */
	MakeRoadDepot(TileXY(10, 11), OWNER_NONE, DepotID(1), DiagDirection::NW, ROADTYPE_ROAD);
	REQUIRE(IsRoadDepotTile(TileXY(10, 11)));

	const Json wrong_axis = RoadDepotEntranceJson(TileXY(10, 11));
	CHECK(wrong_axis["direction"] == "nw");
	CHECK(wrong_axis["tile"]["x"] == 10);
	CHECK(wrong_axis["tile"]["y"] == 10);
	CHECK(wrong_axis["required_road_bit"] == "se");
	CHECK_FALSE(Contains(wrong_axis["road_bits"], "se"));
	CHECK(wrong_axis["has_required_bit"] == false);
	CHECK(wrong_axis["connected"] == false);

	/* The engine's own rule agrees: the road pathfinder keeps only the tracks reachable from the
	 * direction the vehicle enters the neighbour tile with, and that set is empty here and not for
	 * the aligned depot. */
	const TrackStatus aligned_tracks = GetTileTrackStatus(TileXY(8, 10), TransportType::Road, RoadTramType::Road);
	CHECK((aligned_tracks.trackdirs & DiagdirReachesTrackdirs(DiagDirection::SW)).Any());
	const TrackStatus wrong_axis_tracks = GetTileTrackStatus(TileXY(10, 10), TransportType::Road, RoadTramType::Road);
	CHECK((wrong_axis_tracks.trackdirs & DiagdirReachesTrackdirs(DiagDirection::NW)).None());

	/* An entrance tile without any road at all is the same trap, and it says so. */
	MakeRoadDepot(TileXY(10, 13), OWNER_NONE, DepotID(2), DiagDirection::NW, ROADTYPE_ROAD);

	const Json empty = RoadDepotEntranceJson(TileXY(10, 13));
	CHECK(empty["tile"]["x"] == 10);
	CHECK(empty["tile"]["y"] == 12);
	CHECK(empty["road_bits"].empty());
	CHECK(empty["connected"] == false);

	/* The read is the map as it is now and not a remembered build: laying the road under the door
	 * changes the answer for the same depot. */
	MakeRoadNormal(TileXY(10, 12), ROAD_Y, ROADTYPE_ROAD, INVALID_ROADTYPE, TownID::Invalid(), OWNER_NONE, OWNER_NONE);
	const Json refreshed = RoadDepotEntranceJson(TileXY(10, 13));
	CHECK(Contains(refreshed["road_bits"], "se"));
	CHECK(refreshed["has_required_bit"] == true);
	CHECK(refreshed["drivable"] == true);
	CHECK(refreshed["connected"] == true);

	/* A one-piece stub: the piece that faces the depot is there, but a single piece has no drivable
	 * track in the engine, so it must not read as a usable exit. */
	MakeRoadNormal(TileXY(20, 20), RoadBits{RoadBit::SE}, ROADTYPE_ROAD, INVALID_ROADTYPE, TownID::Invalid(), OWNER_NONE, OWNER_NONE);
	MakeRoadDepot(TileXY(20, 21), OWNER_NONE, DepotID(3), DiagDirection::NW, ROADTYPE_ROAD);

	const Json stub = RoadDepotEntranceJson(TileXY(20, 21));
	CHECK(stub["required_road_bit"] == "se");
	CHECK(stub["has_required_bit"] == true);
	CHECK(stub["drivable"] == false);
	CHECK(stub["connected"] == false);

	/* A junction is not a straight road: the engine keeps the trackdirs reachable when moving in the
	 * direction of travel, so a vehicle leaving a depot can turn. This neighbour is an L curve to the
	 * east, and the engine does let the vehicle turn onto it. Reading the reachability with the
	 * opposite direction would call this depot a trap. */
	MakeRoadNormal(TileXY(10, 10), ROAD_E, ROADTYPE_ROAD, INVALID_ROADTYPE, TownID::Invalid(), OWNER_NONE, OWNER_NONE);
	const Json turning = RoadDepotEntranceJson(TileXY(10, 11));
	CHECK(turning["required_road_bit"] == "se");
	CHECK(turning["has_required_bit"] == true);
	CHECK(turning["drivable"] == true);
	CHECK(turning["connected"] == true);

	/* A one-way road is read as the engine has it: the entrance tile then only offers the trackdir
	 * its arrow allows (`GetTileTrackStatus_Road` keeps one of the two), so the same depot is a trap
	 * while the arrow points the other way. */
	MakeRoadNormal(TileXY(12, 10), ROAD_X, ROADTYPE_ROAD, INVALID_ROADTYPE, TownID::Invalid(), OWNER_NONE, OWNER_NONE);
	MakeRoadDepot(TileXY(11, 10), OWNER_NONE, DepotID(4), DiagDirection::SW, ROADTYPE_ROAD);
	CHECK(RoadDepotEntranceJson(TileXY(11, 10))["connected"] == true);

	SetDisallowedRoadDirections(TileXY(12, 10), DisallowedRoadDirections{DisallowedRoadDirection::Northbound});
	const Json one_way_exit = RoadDepotEntranceJson(TileXY(11, 10));
	CHECK(one_way_exit["has_required_bit"] == true);
	CHECK(one_way_exit["drivable"] == false);
	CHECK(one_way_exit["connected"] == false);

	SetDisallowedRoadDirections(TileXY(12, 10), DisallowedRoadDirections{});
	CHECK(RoadDepotEntranceJson(TileXY(11, 10))["connected"] == true);

	/* A tile that is not a road depot has no road entrance at all. */
	CHECK(RoadDepotEntranceJson(TileXY(8, 10)).is_null());
}

TEST_CASE("agent_game_tools: a road stop access reports the entries of its own tile")
{
	TestRoadMap map;

	/* A bay stop is entered from the one side the engine stored for it. */
	MakeRoadStop(TileXY(30, 10), OWNER_NONE, StationID(0), RoadStopType::Bus, ROADTYPE_ROAD, INVALID_ROADTYPE, DiagDirection::SE);
	MakeRoadNormal(TileXY(30, 11), ROAD_Y, ROADTYPE_ROAD, INVALID_ROADTYPE, TownID::Invalid(), OWNER_NONE, OWNER_NONE);

	const Json bay = RoadStopAccessJson(TileXY(30, 10));
	CHECK(bay["type"] == "bus");
	CHECK(bay["kind"] == "bay");
	CHECK(bay["tile"]["x"] == 30);
	CHECK(bay["tile"]["y"] == 10);
	CHECK(bay["entries"].size() == 1);
	CHECK(bay["entries"][0]["direction"] == "se");
	CHECK(bay["entries"][0]["tile"]["x"] == 30);
	CHECK(bay["entries"][0]["tile"]["y"] == 11);
	CHECK(bay["entries"][0]["required_road_bit"] == "nw");
	CHECK(bay["entries"][0]["connected"] == true);

	/* The same stop with nothing on its entrance side, then with the road laid afterwards: the
	 * answer follows the map, so a stop is never taken for usable from an old build. */
	MakeRoadStop(TileXY(31, 10), OWNER_NONE, StationID(0), RoadStopType::Truck, ROADTYPE_ROAD, INVALID_ROADTYPE, DiagDirection::SE);
	const Json no_road = RoadStopAccessJson(TileXY(31, 10));
	CHECK(no_road["type"] == "truck");
	CHECK(no_road["entries"].size() == 1);
	CHECK(no_road["entries"][0]["road_bits"].empty());
	CHECK(no_road["entries"][0]["connected"] == false);

	MakeRoadNormal(TileXY(31, 11), ROAD_Y, ROADTYPE_ROAD, INVALID_ROADTYPE, TownID::Invalid(), OWNER_NONE, OWNER_NONE);
	CHECK(RoadStopAccessJson(TileXY(31, 10))["entries"][0]["connected"] == true);

	/* A drive-through stop is entered from either end of the axis the engine stored. */
	MakeDriveThroughRoadStop(TileXY(40, 10), OWNER_NONE, OWNER_NONE, OWNER_NONE, StationID(1), StationType::Bus, ROADTYPE_ROAD, INVALID_ROADTYPE, Axis::X);
	MakeRoadNormal(TileXY(39, 10), ROAD_X, ROADTYPE_ROAD, INVALID_ROADTYPE, TownID::Invalid(), OWNER_NONE, OWNER_NONE);

	const Json through = RoadStopAccessJson(TileXY(40, 10));
	CHECK(through["kind"] == "drive_through");
	CHECK(through["axis"] == "x");
	REQUIRE(through["entries"].size() == 2);
	/* The two ends are the two axis neighbours, in the engine's direction order. */
	CHECK(through["entries"][0]["direction"] == "ne");
	CHECK(through["entries"][0]["tile"]["x"] == 39);
	CHECK(through["entries"][0]["tile"]["y"] == 10);
	CHECK(through["entries"][0]["connected"] == true);
	CHECK(through["entries"][1]["direction"] == "sw");
	CHECK(through["entries"][1]["tile"]["x"] == 41);
	CHECK(through["entries"][1]["tile"]["y"] == 10);
	CHECK(through["entries"][1]["connected"] == false);

	/* The other end, once the road reaches it. */
	MakeRoadNormal(TileXY(41, 10), ROAD_X, ROADTYPE_ROAD, INVALID_ROADTYPE, TownID::Invalid(), OWNER_NONE, OWNER_NONE);
	const Json both = RoadStopAccessJson(TileXY(40, 10));
	CHECK(both["entries"][0]["connected"] == true);
	CHECK(both["entries"][1]["connected"] == true);

	/* A tile that is not a bus or truck stop has no stop access at all. */
	CHECK(RoadStopAccessJson(TileXY(30, 11)).is_null());
}

TEST_CASE("agent_game_tools: a mixed road and tram stop reads each family on its own")
{
	/* A bus stop tile can carry a road and a tram road type at once, and the two are separate track.
	 * Each family is therefore read on its own: a neighbour with road only must not answer for tram,
	 * and a one-way road must not block the tram track beside it. The stop is a drive-through one,
	 * because the game lets trams on drive-through stops only: `CmdBuildRoadStop` refuses a bay stop
	 * whose road type is a tram. */
	TestRoadMap map;

	MakeDriveThroughRoadStop(TileXY(30, 10), OWNER_NONE, OWNER_NONE, OWNER_NONE, StationID(0), StationType::Bus, ROADTYPE_ROAD, ROADTYPE_TRAM, Axis::Y);

	/* The platform runs along the y axis, so it is entered from either end: the neighbour to the
	 * south-east (30, 11) and the one to the north-west (30, 9). The order of the entries is the
	 * engine's, so an entry is picked by its own direction and family, never by an index. */
	const auto entry = [](const Json &entries, const char *direction, const char *family) -> Json {
		for (const Json &candidate : entries) {
			if (candidate["direction"] == direction && candidate["family"] == family) return candidate;
		}
		FAIL("no entry for that direction and family");
		return Json(nullptr);
	};

	/* A road-only neighbour at the south-east end: the road family reaches the stop, the tram family
	 * cannot, because that tile carries no tram track at all. */
	MakeRoadNormal(TileXY(30, 11), ROAD_Y, ROADTYPE_ROAD, INVALID_ROADTYPE, TownID::Invalid(), OWNER_NONE, OWNER_NONE);

	const Json road_only = RoadStopAccessJson(TileXY(30, 10));
	CHECK(road_only["type"] == "bus");
	CHECK(road_only["kind"] == "drive_through");
	CHECK(road_only["axis"] == "y");
	REQUIRE(road_only.contains("road"));
	REQUIRE(road_only.contains("tram"));
	REQUIRE(road_only["road"]["entries"].size() == 2);
	REQUIRE(road_only["tram"]["entries"].size() == 2);
	CHECK(road_only["road"]["road_type_id"] == static_cast<int>(ROADTYPE_ROAD));
	CHECK(road_only["tram"]["road_type_id"] == static_cast<int>(ROADTYPE_TRAM));

	const Json se_road = entry(road_only["road"]["entries"], "se", "road");
	CHECK(se_road["tile"]["x"] == 30);
	CHECK(se_road["tile"]["y"] == 11);
	CHECK(se_road["connected"] == true);
	CHECK(Contains(se_road["road_bits"], "nw"));

	const Json se_tram = entry(road_only["tram"]["entries"], "se", "tram");
	CHECK(se_tram["connected"] == false);
	CHECK(se_tram["has_required_bit"] == false);
	CHECK(se_tram["road_bits"].empty());

	/* Nothing lies at the other end, so neither family reaches the stop from there. */
	CHECK(entry(road_only["road"]["entries"], "nw", "road")["connected"] == false);
	CHECK(entry(road_only["tram"]["entries"], "nw", "tram")["connected"] == false);

	/* The combined list carries both families, one entry per family and end, each labelled with its
	 * own `family`. That label is all the combined list gives: a mixed stop has a road entry and a
	 * tram entry for the same end, so a reader that wants one family has to select on `family` (or
	 * take that family's subobject) and cannot read an entry of the other family as its answer. */
	REQUIRE(road_only["entries"].size() == 4);
	CHECK(entry(road_only["entries"], "se", "road")["connected"] == true);
	CHECK(entry(road_only["entries"], "se", "tram")["connected"] == false);
	CHECK(entry(road_only["entries"], "nw", "road")["connected"] == false);
	CHECK(entry(road_only["entries"], "nw", "tram")["connected"] == false);

	/* Tram track on the neighbour makes the tram family reachable as well. */
	MakeRoadNormal(TileXY(30, 11), ROAD_Y, ROADTYPE_ROAD, ROADTYPE_TRAM, TownID::Invalid(), OWNER_NONE, OWNER_NONE);
	const Json both = RoadStopAccessJson(TileXY(30, 10));
	CHECK(entry(both["road"]["entries"], "se", "road")["connected"] == true);
	CHECK(entry(both["tram"]["entries"], "se", "tram")["connected"] == true);

	/* A one-way restriction belongs to the road family only: the engine leaves the tram trackdirs
	 * unscaled, so the tram entry stays usable while the road entry is blocked. `Northbound` on a
	 * ROAD_Y tile leaves only the southbound trackdir, which exits that neighbour towards (30, 12) and
	 * not towards the stop, so a bus standing there drives away from the stop instead of into it. */
	SetDisallowedRoadDirections(TileXY(30, 11), DisallowedRoadDirections{DisallowedRoadDirection::Northbound});
	const Json one_way = RoadStopAccessJson(TileXY(30, 10));
	const Json one_way_road = entry(one_way["road"]["entries"], "se", "road");
	CHECK(one_way_road["has_required_bit"] == true);
	CHECK(one_way_road["drivable"] == false);
	CHECK(one_way_road["connected"] == false);
	CHECK(entry(one_way["tram"]["entries"], "se", "tram")["connected"] == true);

	/* A stop with a road type only has no tram family at all, so nothing can be read as tram. */
	MakeRoadStop(TileXY(40, 10), OWNER_NONE, StationID(1), RoadStopType::Truck, ROADTYPE_ROAD, INVALID_ROADTYPE, DiagDirection::SE);
	const Json road_family_only = RoadStopAccessJson(TileXY(40, 10));
	CHECK(road_family_only.contains("road"));
	CHECK_FALSE(road_family_only.contains("tram"));
	REQUIRE(road_family_only["entries"].size() == 1);
	CHECK(road_family_only["entries"][0]["family"] == "road");
}

TEST_CASE("agent_game_tools: a planned depot exit reports only the neighbour tile")
{
	/* A dry run builds nothing, so the depot tile carries no exit piece. The proposal therefore
	 * reports the neighbour tile alone and marks itself `planned`, so it is never read as the
	 * current state of a depot that exists. */
	TestRoadMap map;

	const Json empty = RoadDepotExitProposalJson(TileXY(60, 60), DiagDirection::SW, RoadTramType::Road);
	CHECK(empty["planned"] == true);
	CHECK(empty["direction"] == "sw");
	CHECK(empty["tile"]["x"] == 61);
	CHECK(empty["tile"]["y"] == 60);
	CHECK(empty["required_road_bit"] == "ne");
	CHECK(empty["road_bits"].empty());
	CHECK(empty["has_required_bit"] == false);
	CHECK(empty["neighbour_drivable"] == false);
	/* No current connectivity is claimed for a depot that is not on the map. */
	CHECK_FALSE(empty.contains("connected"));
	CHECK_FALSE(empty.contains("drivable"));

	/* A road laid out for the planned exit is reported as such. */
	MakeRoadNormal(TileXY(61, 60), ROAD_X, ROADTYPE_ROAD, INVALID_ROADTYPE, TownID::Invalid(), OWNER_NONE, OWNER_NONE);
	const Json ready = RoadDepotExitProposalJson(TileXY(60, 60), DiagDirection::SW, RoadTramType::Road);
	CHECK(ready["planned"] == true);
	CHECK(Contains(ready["road_bits"], "ne"));
	CHECK(ready["has_required_bit"] == true);
	CHECK(ready["neighbour_drivable"] == true);
	CHECK_FALSE(ready.contains("connected"));

	/* A one-piece stub carries the piece the planned depot needs and still cannot be driven on, so
	 * the proposal catches it before the depot is built. */
	MakeRoadNormal(TileXY(70, 70), RoadBits{RoadBit::NE}, ROADTYPE_ROAD, INVALID_ROADTYPE, TownID::Invalid(), OWNER_NONE, OWNER_NONE);
	const Json stub = RoadDepotExitProposalJson(TileXY(70, 71), DiagDirection::NW, RoadTramType::Road);
	CHECK(stub["tile"]["x"] == 70);
	CHECK(stub["tile"]["y"] == 70);
	CHECK(stub["required_road_bit"] == "se");
	CHECK(stub["has_required_bit"] == false);
	CHECK(stub["neighbour_drivable"] == false);

	MakeRoadNormal(TileXY(72, 70), RoadBits{RoadBit::SE}, ROADTYPE_ROAD, INVALID_ROADTYPE, TownID::Invalid(), OWNER_NONE, OWNER_NONE);
	const Json stub_facing = RoadDepotExitProposalJson(TileXY(72, 71), DiagDirection::NW, RoadTramType::Road);
	CHECK(stub_facing["required_road_bit"] == "se");
	CHECK(stub_facing["has_required_bit"] == true);
	CHECK(stub_facing["neighbour_drivable"] == false);
}

TEST_CASE("agent_game_tools: a road tile read reports its pieces and one-way state")
{
	TestRoadMap map;

	MakeRoadNormal(TileXY(8, 10), ROAD_X, ROADTYPE_ROAD, INVALID_ROADTYPE, TownID::Invalid(), OWNER_NONE, OWNER_NONE);

	/* A straight road offers the two directions along its own axis and no others, so two adjacent
	 * road tiles can be told apart from connected track. */
	const Json straight = RoadTileJson(TileXY(8, 10), RoadTramType::Road);
	CHECK(Contains(straight["road_bits"], "ne"));
	CHECK(Contains(straight["road_bits"], "sw"));
	CHECK(Contains(straight["drivable_exits"], "ne"));
	CHECK(Contains(straight["drivable_exits"], "sw"));
	CHECK_FALSE(Contains(straight["drivable_exits"], "se"));
	CHECK_FALSE(Contains(straight["drivable_exits"], "nw"));
	CHECK(straight["road_type_id"] == static_cast<int>(ROADTYPE_ROAD));
	CHECK(straight["road_type"] == "road");
	CHECK(straight["one_way"] == false);
	CHECK(straight["disallowed_directions"].empty());

	/* A one-way road says which travel direction the engine disallows, and the engine's own track
	 * status of the tile then keeps only the other one. `northbound`/`southbound` are the engine's
	 * own labels for the two one-way states, not screen directions. */
	MakeRoadNormal(TileXY(9, 10), ROAD_X, ROADTYPE_ROAD, INVALID_ROADTYPE, TownID::Invalid(), OWNER_NONE, OWNER_NONE);
	SetDisallowedRoadDirections(TileXY(9, 10), DisallowedRoadDirections{DisallowedRoadDirection::Northbound});

	const Json one_way = RoadTileJson(TileXY(9, 10), RoadTramType::Road);
	CHECK(one_way["one_way"] == true);
	CHECK(Contains(one_way["disallowed_directions"], "northbound"));
	CHECK_FALSE(Contains(one_way["disallowed_directions"], "southbound"));
	/* The engine's track status keeps only the trackdir the arrow allows, so only that exit is
	 * reported; the pieces themselves are unchanged. */
	CHECK(Contains(one_way["road_bits"], "ne"));
	CHECK(Contains(one_way["road_bits"], "sw"));
	CHECK(Contains(one_way["drivable_exits"], "ne"));
	CHECK_FALSE(Contains(one_way["drivable_exits"], "sw"));

	/* A curve joins two sides of the tile and offers no exit on the others, which the engine's own
	 * trackdir to exit direction map reports and a "reachable on entering" test does not. */
	MakeRoadNormal(TileXY(11, 10), ROAD_N, ROADTYPE_ROAD, INVALID_ROADTYPE, TownID::Invalid(), OWNER_NONE, OWNER_NONE);
	const Json curve = RoadTileJson(TileXY(11, 10), RoadTramType::Road);
	CHECK(Contains(curve["road_bits"], "ne"));
	CHECK(Contains(curve["road_bits"], "nw"));
	CHECK(Contains(curve["drivable_exits"], "ne"));
	CHECK(Contains(curve["drivable_exits"], "nw"));
	CHECK_FALSE(Contains(curve["drivable_exits"], "se"));
	CHECK_FALSE(Contains(curve["drivable_exits"], "sw"));

	/* A one-piece stub carries a piece and no drivable track at all. */
	MakeRoadNormal(TileXY(20, 20), RoadBits{RoadBit::SE}, ROADTYPE_ROAD, INVALID_ROADTYPE, TownID::Invalid(), OWNER_NONE, OWNER_NONE);
	const Json stub = RoadTileJson(TileXY(20, 20), RoadTramType::Road);
	CHECK(Contains(stub["road_bits"], "se"));
	CHECK(stub["drivable_exits"].empty());
	CHECK(stub["one_way"] == false);

	/* A tile without a road of that family says so instead of pretending to be empty road. */
	const Json clear_tile = RoadTileJson(TileXY(50, 50), RoadTramType::Road);
	CHECK(clear_tile["road_bits"].empty());
	CHECK(clear_tile["road_type_id"].is_null());
	CHECK(clear_tile["road_type"].is_null());
	CHECK(clear_tile["drivable_exits"].empty());
}

namespace {

/**
 * Real vehicles in the engine's own vehicle pool, removed again when the fixture goes out of scope.
 *
 * The vehicles are genuine pool items, so their index and their lifetime generation are the engine's
 * own. `Create` reserves the allocation with the pool's own `CanAllocateItem` first, because `GetNew`
 * asserts that the allocation was checked and decrements that reservation once per allocation.
 * Removal uses the pool's own `CleanPool`, the path the other cases of this file use: it resets the
 * pool completely, so a case that runs afterwards still sees an empty pool.
 */
class TestConsistVehicles {
public:
	TestConsistVehicles() { REQUIRE(Vehicle::GetNumItems() == 0); }

	~TestConsistVehicles()
	{
		_vehicle_pool.CleanPool();
		/* A vehicle only drops its packet list when the pool is cleaned, so the packets of these
		 * cases are returned to their own pool as well. */
		_cargopacket_pool.CleanPool();
	}

	/**
	 * Create one vehicle in the engine's own pool.
	 * @tparam Tvehicle The vehicle type to create.
	 * @return The created vehicle.
	 */
	template <typename Tvehicle>
	Tvehicle *Create()
	{
		REQUIRE(Vehicle::CanAllocateItem());
		Tvehicle *v = Tvehicle::Create();
		REQUIRE(v != nullptr);
		return v;
	}
};

/**
 * The real engines the consist cases put on their vehicles, removed again when the fixture goes out
 * of scope.
 *
 * A vehicle row asks the engine of the vehicle for its name and its running cost, so a vehicle
 * without a real engine could not be read at all. The engines are the engine pool's own default
 * entries, created through the pool's `CreateAtIndex`, so their names, capacities and running costs
 * are the engine's data and not a stub of the test.
 */
class TestConsistEngines {
public:
	TestConsistEngines()
	{
		REQUIRE(Engine::GetNumItems() == 0);
		REQUIRE(Engine::CanAllocateItem(5));
		this->loco = Engine::CreateAtIndex(EngineID(0), VehicleType::Train, 0);
		this->wagon = Engine::CreateAtIndex(EngineID(1), VehicleType::Train, 29);
		this->aircraft = Engine::CreateAtIndex(EngineID(2), VehicleType::Aircraft, 0);
		this->ship = Engine::CreateAtIndex(EngineID(3), VehicleType::Ship, 0);
		this->road = Engine::CreateAtIndex(EngineID(4), VehicleType::Road, 0);
	}

	~TestConsistEngines() { _engine_pool.CleanPool(); }

	Engine *loco = nullptr;     ///< The engine of a locomotive.
	Engine *wagon = nullptr;    ///< The engine of a coal wagon.
	Engine *aircraft = nullptr; ///< The engine of an aircraft.
	Engine *ship = nullptr;     ///< The engine of a ship.
	Engine *road = nullptr;     ///< The engine of a road vehicle.
};

/** Cargo type of the default passengers, as the engine's own cargo table numbers it. */
const CargoType TEST_CARGO_PASSENGERS{0};
/** Cargo type of the default coal. */
const CargoType TEST_CARGO_COAL{1};
/** Cargo type of the default mail. */
const CargoType TEST_CARGO_MAIL{2};
/** Cargo type of the default wood. */
const CargoType TEST_CARGO_WOOD{7};

/**
 * Set the labels of the cargo types these cases read.
 *
 * A running game fills the cargo table; this binary has none, so the cases set the labels of the four
 * entries they use to the real labels of the default cargoes. Only the label is set, and only the
 * rows read it through `CargoSpec`, so the rest of an entry stays as it is.
 */
void InitTestCargoLabels()
{
	CargoSpec::Get(TEST_CARGO_PASSENGERS)->label = CT_PASSENGERS;
	CargoSpec::Get(TEST_CARGO_COAL)->label = CT_COAL;
	CargoSpec::Get(TEST_CARGO_MAIL)->label = CT_MAIL;
	CargoSpec::Get(TEST_CARGO_WOOD)->label = CT_WOOD;
}

/**
 * Give a train part the owner, the engine and the tile its row reads, and the front-engine bit the
 * engine's own `IsPrimaryVehicle()` reads for a train.
 * @param v The train.
 * @param owner Owner of the vehicle.
 * @param engine Engine of the vehicle.
 * @param tile Tile the vehicle stands on.
 * @param front Whether the part is the front engine of its chain.
 */
void SetupTrainPart(Train *v, CompanyID owner, EngineID engine, TileIndex tile, bool front)
{
	v->owner = owner;
	v->engine_type = engine;
	v->tile = tile;
	if (front) {
		v->SetFrontEngine();
	} else {
		v->ClearFrontEngine();
	}
}

/**
 * Put cargo on a vehicle through the engine's own cargo list, with the capacity it has for it.
 * @param v The vehicle.
 * @param cargo Cargo type the vehicle carries.
 * @param capacity Capacity of the vehicle for that cargo.
 * @param count Units of cargo on board.
 */
void LoadCargo(Vehicle *v, CargoType cargo, uint16_t capacity, uint16_t count)
{
	v->cargo_type = cargo;
	v->cargo_cap = capacity;
	if (count == 0) return;
	REQUIRE(CargoPacket::CanAllocateItem());
	v->cargo.Append(CargoPacket::Create(StationID::Invalid(), count, Source{Source::Invalid, SourceType::Industry}));
}

/**
 * Read the row of one vehicle out of the vehicle inventory.
 *
 * The query is filtered by id, so the row is built for that vehicle only. That is what these cases
 * need: `VehicleRow` reads the vehicle through the engine's own `IsStoppedInDepot()`, which asserts
 * that it is asked for the head of a chain, so a case reads the rows of heads and of vehicles that
 * stand alone, and reads the parts of a chain through the consist of their head.
 * @param v The vehicle.
 * @return The vehicle row the inventory reported.
 */
Json VehicleRowOf(const Vehicle *v)
{
	const Json params = {{"kind", "vehicles"}, {"filters", {{"vehicle_id", static_cast<int64_t>(v->index.base())}}}};
	const std::optional<Json> result = DispatchGameTool("world.query", params, CompanyID::Invalid());
	REQUIRE(result.has_value());
	REQUIRE((*result)["ok"] == true);
	REQUIRE((*result)["total"] == 1);
	return (*result)["rows"].front();
}

} // namespace

TEST_CASE("agent_game_tools: a locomotive row carries the consist it hauls")
{
	LoadTestLanguagePack();
	InitTestCargoLabels();

	/* The company is declared first, so the vehicles are removed before it. */
	TestCompanyGuard company;
	TestRoadMap map;
	TestConsistEngines engines;
	TestConsistVehicles vehicles;

	const TileIndex tile = TileXY(10, 10);
	Train *loco = vehicles.Create<Train>();
	Train *first_wagon = vehicles.Create<Train>();
	Train *second_wagon = vehicles.Create<Train>();
	SetupTrainPart(loco, company.Id(), engines.loco->index, tile, true);
	SetupTrainPart(first_wagon, company.Id(), engines.wagon->index, tile, false);
	SetupTrainPart(second_wagon, company.Id(), engines.wagon->index, tile, false);
	/* A refitted train carries the same cargo in every part; the locomotive has no capacity for it. */
	LoadCargo(loco, TEST_CARGO_COAL, 0, 0);
	LoadCargo(first_wagon, TEST_CARGO_COAL, 50, 30);
	LoadCargo(second_wagon, TEST_CARGO_COAL, 50, 40);
	loco->SetNext(first_wagon);
	first_wagon->SetNext(second_wagon);

	/* The chain the engine built, before anything is read: head, order and last part. */
	REQUIRE(loco->First() == loco);
	REQUIRE(loco->Next() == first_wagon);
	REQUIRE(first_wagon->Next() == second_wagon);
	REQUIRE(second_wagon->Previous() == first_wagon);
	REQUIRE(loco->Last() == second_wagon);

	const Json row = VehicleRowOf(loco);
	CHECK(row["id"] == static_cast<int64_t>(loco->index.base()));
	CHECK(row["is_primary"] == true);
	/* A primary vehicle is its own head, and the reference is a full generation reference. */
	CHECK(row["primary_ref"] == row["ref"]);
	CHECK(row["primary_ref"]["type"] == "vehicle");
	CHECK(row["primary_ref"]["generation"] == loco->generation);

	REQUIRE(row.contains("consist"));
	const Json &consist = row["consist"];
	CHECK(consist["total_parts"] == 3);
	CHECK(consist["truncated"] == false);
	REQUIRE(consist["parts"].size() == 3);
	/* The parts are the engine's own chain, in chain order, each with its own lifetime. */
	CHECK(consist["parts"][0]["ref"] == row["ref"]);
	CHECK(consist["parts"][1]["ref"]["id"] == static_cast<int64_t>(first_wagon->index.base()));
	CHECK(consist["parts"][1]["ref"]["generation"] == first_wagon->generation);
	CHECK(consist["parts"][2]["ref"]["id"] == static_cast<int64_t>(second_wagon->index.base()));
	CHECK(consist["parts"][2]["ref"]["generation"] == second_wagon->generation);
	/* Every part carries its own cargo: the locomotive none, the wagons what was loaded on them. */
	CHECK(consist["parts"][0]["cargo"]["load"] == 0);
	CHECK(consist["parts"][0]["cargo"]["capacity"] == 0);
	CHECK(consist["parts"][1]["cargo"]["type"] == static_cast<int>(TEST_CARGO_COAL));
	CHECK(consist["parts"][1]["cargo"]["label"] == "COAL");
	CHECK(consist["parts"][1]["cargo"]["load"] == 30);
	CHECK(consist["parts"][1]["cargo"]["capacity"] == 50);
	CHECK(consist["parts"][2]["cargo"]["load"] == 40);
	CHECK(consist["parts"][2]["cargo"]["capacity"] == 50);

	/* The chain total is the engine's own sum over every part, with the label of the cargo. */
	REQUIRE(consist["cargo"].size() == 1);
	CHECK(consist["cargo"][0]["type"] == static_cast<int>(TEST_CARGO_COAL));
	CHECK(consist["cargo"][0]["label"] == "COAL");
	CHECK(consist["cargo"][0]["load"] == 70);
	CHECK(consist["cargo"][0]["capacity"] == 100);

	/* The row's own cargo field still describes the one vehicle, not the chain. */
	CHECK(row["cargo"]["load"] == 0);
	CHECK(row["cargo"]["capacity"] == 0);
}

TEST_CASE("agent_game_tools: a consist totals each cargo over the whole chain")
{
	LoadTestLanguagePack();
	InitTestCargoLabels();

	/* The company is declared first, so the vehicles are removed before it. */
	TestCompanyGuard company;
	TestRoadMap map;
	TestConsistEngines engines;
	TestConsistVehicles vehicles;

	const TileIndex tile = TileXY(12, 12);
	Train *loco = vehicles.Create<Train>();
	Train *coal_wagon = vehicles.Create<Train>();
	Train *wood_wagon = vehicles.Create<Train>();
	Train *second_coal_wagon = vehicles.Create<Train>();
	SetupTrainPart(loco, company.Id(), engines.loco->index, tile, true);
	SetupTrainPart(coal_wagon, company.Id(), engines.wagon->index, tile, false);
	SetupTrainPart(wood_wagon, company.Id(), engines.wagon->index, tile, false);
	SetupTrainPart(second_coal_wagon, company.Id(), engines.wagon->index, tile, false);
	LoadCargo(loco, TEST_CARGO_COAL, 0, 0);
	LoadCargo(coal_wagon, TEST_CARGO_COAL, 50, 30);
	LoadCargo(wood_wagon, TEST_CARGO_WOOD, 40, 20);
	LoadCargo(second_coal_wagon, TEST_CARGO_COAL, 50, 10);
	loco->SetNext(coal_wagon);
	coal_wagon->SetNext(wood_wagon);
	wood_wagon->SetNext(second_coal_wagon);
	REQUIRE(loco->Last() == second_coal_wagon);

	const Json row = VehicleRowOf(loco);
	const Json &consist = row["consist"];
	CHECK(consist["total_parts"] == 4);
	CHECK(consist["truncated"] == false);
	REQUIRE(consist["parts"].size() == 4);

	/* One entry per cargo type, in the order of the engine's cargo type ids, summed over the whole
	 * chain: the coal of the first and the last wagon is one total, the wood of the middle one its
	 * own. */
	REQUIRE(consist["cargo"].size() == 2);
	CHECK(consist["cargo"][0]["type"] == static_cast<int>(TEST_CARGO_COAL));
	CHECK(consist["cargo"][0]["label"] == "COAL");
	CHECK(consist["cargo"][0]["load"] == 40);
	CHECK(consist["cargo"][0]["capacity"] == 100);
	CHECK(consist["cargo"][1]["type"] == static_cast<int>(TEST_CARGO_WOOD));
	CHECK(consist["cargo"][1]["label"] == "WOOD");
	CHECK(consist["cargo"][1]["load"] == 20);
	CHECK(consist["cargo"][1]["capacity"] == 40);

	/* The parts say where the totals come from: the mixed wagon is listed with its own cargo. */
	CHECK(consist["parts"][2]["ref"]["id"] == static_cast<int64_t>(wood_wagon->index.base()));
	CHECK(consist["parts"][2]["cargo"]["type"] == static_cast<int>(TEST_CARGO_WOOD));
	CHECK(consist["parts"][2]["cargo"]["load"] == 20);
	CHECK(consist["parts"][2]["cargo"]["capacity"] == 40);
}

TEST_CASE("agent_game_tools: a free wagon has no head and no consist")
{
	LoadTestLanguagePack();
	InitTestCargoLabels();

	/* The company is declared first, so the vehicles are removed before it. */
	TestCompanyGuard company;
	TestRoadMap map;
	TestConsistEngines engines;
	TestConsistVehicles vehicles;

	const TileIndex tile = TileXY(14, 14);
	Train *loco = vehicles.Create<Train>();
	Train *coupled_wagon = vehicles.Create<Train>();
	Train *free_wagon = vehicles.Create<Train>();
	SetupTrainPart(loco, company.Id(), engines.loco->index, tile, true);
	SetupTrainPart(coupled_wagon, company.Id(), engines.wagon->index, tile, false);
	SetupTrainPart(free_wagon, company.Id(), engines.wagon->index, tile, false);
	LoadCargo(loco, TEST_CARGO_COAL, 0, 0);
	LoadCargo(coupled_wagon, TEST_CARGO_COAL, 50, 20);
	LoadCargo(free_wagon, TEST_CARGO_COAL, 50, 5);
	loco->SetNext(coupled_wagon);

	/* A wagon that stands alone in the pool is its own head, and that head is no primary vehicle. */
	REQUIRE(free_wagon->First() == free_wagon);
	REQUIRE_FALSE(free_wagon->IsPrimaryVehicle());

	const Json free_row = VehicleRowOf(free_wagon);
	CHECK(free_row["is_primary"] == false);
	/* No fabricated head: the wagon itself is not a primary vehicle, so the field is null. */
	CHECK(free_row["primary_ref"].is_null());
	/* Only a primary row carries a consist, and a wagon's own cargo is still its own cargo. */
	CHECK_FALSE(free_row.contains("consist"));
	CHECK(free_row["cargo"]["load"] == 5);
	CHECK(free_row["cargo"]["capacity"] == 50);

	/* The coupled wagon is in the chain of the locomotive and in no other chain. */
	const Json loco_row = VehicleRowOf(loco);
	CHECK(loco_row["is_primary"] == true);
	CHECK(loco_row["consist"]["total_parts"] == 2);
	REQUIRE(loco_row["consist"]["parts"].size() == 2);
	CHECK(loco_row["consist"]["parts"][1]["ref"]["id"] == static_cast<int64_t>(coupled_wagon->index.base()));
	CHECK(loco_row["consist"]["cargo"][0]["load"] == 20);
}

TEST_CASE("agent_game_tools: a consist reference names a lifetime, not a pool id")
{
	LoadTestLanguagePack();
	InitTestCargoLabels();

	/* The company is declared first, so the vehicles are removed before it. */
	TestCompanyGuard company;
	TestRoadMap map;
	TestConsistEngines engines;
	TestConsistVehicles vehicles;

	const TileIndex tile = TileXY(16, 16);
	Train *loco = vehicles.Create<Train>();
	Train *wagon = vehicles.Create<Train>();
	SetupTrainPart(loco, company.Id(), engines.loco->index, tile, true);
	SetupTrainPart(wagon, company.Id(), engines.wagon->index, tile, false);
	LoadCargo(loco, TEST_CARGO_COAL, 0, 0);
	LoadCargo(wagon, TEST_CARGO_COAL, 50, 10);
	loco->SetNext(wagon);

	const Json before = VehicleRowOf(loco);
	REQUIRE(before["consist"]["parts"].size() == 2);
	const int64_t wagon_id = before["consist"]["parts"][1]["ref"]["id"].get<int64_t>();
	const uint64_t wagon_generation = before["consist"]["parts"][1]["ref"]["generation"].get<uint64_t>();
	CHECK(wagon_id == static_cast<int64_t>(wagon->index.base()));
	CHECK(wagon_generation == wagon->generation);
	CHECK(before["consist"]["cargo"][0]["load"] == 10);

	/* The pool is reset the way the fixtures do it and a new consist is built in it: the engine keeps
	 * the lifetime counter, so the rebuilt wagon gets the id back but not the lifetime. */
	_vehicle_pool.CleanPool();
	REQUIRE(Vehicle::CanAllocateItem(2));
	Train *new_loco = Train::Create();
	Train *new_wagon = Train::Create();
	REQUIRE(new_loco != nullptr);
	REQUIRE(new_wagon != nullptr);
	REQUIRE(new_wagon->index.base() == static_cast<uint32_t>(wagon_id));
	SetupTrainPart(new_loco, company.Id(), engines.loco->index, tile, true);
	SetupTrainPart(new_wagon, company.Id(), engines.wagon->index, tile, false);
	LoadCargo(new_loco, TEST_CARGO_COAL, 0, 0);
	LoadCargo(new_wagon, TEST_CARGO_COAL, 50, 15);
	new_loco->SetNext(new_wagon);

	const Json after = VehicleRowOf(new_loco);
	REQUIRE(after["consist"]["parts"].size() == 2);
	/* Same id, different lifetime: a reference to the old wagon can never be read as the new one. */
	CHECK(after["consist"]["parts"][1]["ref"]["id"] == wagon_id);
	CHECK(after["consist"]["parts"][1]["ref"]["generation"] != wagon_generation);
	CHECK(after["consist"]["parts"][1]["ref"]["generation"] == new_wagon->generation);
	/* And the new consist carries the new cargo, so the row is not a remembered answer. */
	CHECK(after["consist"]["cargo"][0]["load"] == 15);
}

TEST_CASE("agent_game_tools: an aircraft, a ship and a road vehicle read the same consist fields")
{
	LoadTestLanguagePack();
	InitTestCargoLabels();

	/* The company is declared first, so the vehicles are removed before it. */
	TestCompanyGuard company;
	TestRoadMap map;
	TestConsistEngines engines;
	TestConsistVehicles vehicles;

	const TileIndex tile = TileXY(18, 18);

	/* An aircraft is a chain too: the front aircraft carries the passengers and the shadow behind it
	 * carries the mail, so the chain total is what the aircraft really hauls. */
	Aircraft *plane = vehicles.Create<Aircraft>();
	Aircraft *shadow = vehicles.Create<Aircraft>();
	plane->owner = company.Id();
	plane->engine_type = engines.aircraft->index;
	plane->tile = tile;
	plane->subtype = AIR_AIRCRAFT;
	shadow->owner = company.Id();
	shadow->engine_type = engines.aircraft->index;
	shadow->tile = tile;
	shadow->subtype = AIR_SHADOW;
	LoadCargo(plane, TEST_CARGO_PASSENGERS, 100, 40);
	LoadCargo(shadow, TEST_CARGO_MAIL, 20, 5);
	plane->SetNext(shadow);
	REQUIRE(plane->First() == plane);
	REQUIRE(plane->Next() == shadow);

	const Json plane_row = VehicleRowOf(plane);
	CHECK(plane_row["is_primary"] == true);
	CHECK(plane_row["primary_ref"] == plane_row["ref"]);
	const Json &plane_consist = plane_row["consist"];
	CHECK(plane_consist["total_parts"] == 2);
	REQUIRE(plane_consist["parts"].size() == 2);
	CHECK(plane_consist["parts"][1]["ref"]["id"] == static_cast<int64_t>(shadow->index.base()));
	CHECK(plane_consist["parts"][1]["ref"]["generation"] == shadow->generation);
	REQUIRE(plane_consist["cargo"].size() == 2);
	CHECK(plane_consist["cargo"][0]["type"] == static_cast<int>(TEST_CARGO_PASSENGERS));
	CHECK(plane_consist["cargo"][0]["load"] == 40);
	CHECK(plane_consist["cargo"][0]["capacity"] == 100);
	CHECK(plane_consist["cargo"][1]["type"] == static_cast<int>(TEST_CARGO_MAIL));
	CHECK(plane_consist["cargo"][1]["label"] == "MAIL");
	CHECK(plane_consist["cargo"][1]["load"] == 5);
	CHECK(plane_consist["cargo"][1]["capacity"] == 20);

	/* A ship is one vehicle: it is its own head and its own consist. */
	Ship *ship = vehicles.Create<Ship>();
	ship->owner = company.Id();
	ship->engine_type = engines.ship->index;
	ship->tile = tile;
	LoadCargo(ship, TEST_CARGO_COAL, 200, 90);

	const Json ship_row = VehicleRowOf(ship);
	CHECK(ship_row["is_primary"] == true);
	CHECK(ship_row["primary_ref"] == ship_row["ref"]);
	const Json &ship_consist = ship_row["consist"];
	CHECK(ship_consist["total_parts"] == 1);
	CHECK(ship_consist["truncated"] == false);
	REQUIRE(ship_consist["parts"].size() == 1);
	CHECK(ship_consist["parts"][0]["ref"] == ship_row["ref"]);
	REQUIRE(ship_consist["cargo"].size() == 1);
	CHECK(ship_consist["cargo"][0]["type"] == static_cast<int>(TEST_CARGO_COAL));
	CHECK(ship_consist["cargo"][0]["load"] == 90);
	CHECK(ship_consist["cargo"][0]["capacity"] == 200);

	/* A road vehicle can be articulated as well; the rear part carries its own cargo and is part of
	 * the same chain, so the two parts add up to what the bus carries. */
	RoadVehicle *bus = vehicles.Create<RoadVehicle>();
	RoadVehicle *bus_rear = vehicles.Create<RoadVehicle>();
	bus->owner = company.Id();
	bus->engine_type = engines.road->index;
	bus->tile = tile;
	bus->roadtype = ROADTYPE_ROAD;
	bus->SetFrontEngine();
	bus_rear->owner = company.Id();
	bus_rear->engine_type = engines.road->index;
	bus_rear->tile = tile;
	bus_rear->roadtype = ROADTYPE_ROAD;
	bus_rear->SetArticulatedPart();
	LoadCargo(bus, TEST_CARGO_PASSENGERS, 40, 20);
	LoadCargo(bus_rear, TEST_CARGO_PASSENGERS, 40, 15);
	bus->SetNext(bus_rear);
	REQUIRE(bus->Next() == bus_rear);
	REQUIRE(bus_rear->IsArticulatedPart());

	const Json bus_row = VehicleRowOf(bus);
	CHECK(bus_row["is_primary"] == true);
	CHECK(bus_row["primary_ref"] == bus_row["ref"]);
	CHECK(bus_row["road_type"] == "road");
	const Json &bus_consist = bus_row["consist"];
	CHECK(bus_consist["total_parts"] == 2);
	REQUIRE(bus_consist["parts"].size() == 2);
	CHECK(bus_consist["parts"][1]["ref"]["id"] == static_cast<int64_t>(bus_rear->index.base()));
	REQUIRE(bus_consist["cargo"].size() == 1);
	CHECK(bus_consist["cargo"][0]["type"] == static_cast<int>(TEST_CARGO_PASSENGERS));
	CHECK(bus_consist["cargo"][0]["load"] == 35);
	CHECK(bus_consist["cargo"][0]["capacity"] == 80);
}

TEST_CASE("agent_game_tools: a consist list is bounded and says it was truncated")
{
	LoadTestLanguagePack();
	InitTestCargoLabels();

	/* The company is declared first, so the vehicles are removed before it. */
	TestCompanyGuard company;
	TestRoadMap map;
	TestConsistEngines engines;
	TestConsistVehicles vehicles;

	/* One locomotive and 129 wagons: two more parts than a row may list. */
	const TileIndex tile = TileXY(20, 20);
	Train *loco = vehicles.Create<Train>();
	SetupTrainPart(loco, company.Id(), engines.loco->index, tile, true);
	LoadCargo(loco, TEST_CARGO_COAL, 0, 0);

	Train *last = loco;
	for (int i = 0; i < 129; i++) {
		Train *wagon = vehicles.Create<Train>();
		SetupTrainPart(wagon, company.Id(), engines.wagon->index, tile, false);
		LoadCargo(wagon, TEST_CARGO_COAL, 2, 1);
		last->SetNext(wagon);
		last = wagon;
	}
	REQUIRE(loco->Last() == last);

	const Json row = VehicleRowOf(loco);
	const Json &consist = row["consist"];
	CHECK(consist["total_parts"] == 130);
	CHECK(consist["truncated"] == true);
	REQUIRE(consist["parts"].size() == 128);
	/* The list stops at the 128th part, while the totals are still over the whole chain. */
	CHECK(consist["parts"][127]["ref"]["id"] == static_cast<int64_t>(loco->Move(127)->index.base()));
	CHECK(consist["parts"][127]["ref"]["generation"] == loco->Move(127)->generation);
	REQUIRE(consist["cargo"].size() == 1);
	CHECK(consist["cargo"][0]["type"] == static_cast<int>(TEST_CARGO_COAL));
	CHECK(consist["cargo"][0]["load"] == 129);
	CHECK(consist["cargo"][0]["capacity"] == 258);
}

namespace {

/**
 * One real airport station with the hangar tile of its small airport, removed again when the fixture
 * goes out of scope.
 *
 * A running game builds the airport through the build command; these cases only need the airport data
 * the engine reads to recognise a hangar, so the small airport is placed on the station directly.
 */
class TestConsistHangar {
public:
	TestConsistHangar(CompanyID owner, TileIndex tile)
	{
		REQUIRE(Station::GetNumItems() == 0);
		REQUIRE(Station::CanAllocateItem());
		this->station = Station::Create(tile);
		REQUIRE(this->station != nullptr);
		this->station->owner = owner;
		this->station->airport.type = AT_SMALL;
		this->station->airport.rotation = Direction::N;
		this->station->airport.tile = tile;
		this->hangar = this->station->airport.GetHangarTile(0);
		MakeAirport(this->hangar, owner, this->station->index, 0, WaterClass::Invalid);
	}

	~TestConsistHangar() { _station_pool.CleanPool(); }

	/** @return The hangar tile of the airport. */
	TileIndex Hangar() const { return this->hangar; }

private:
	Station *station = nullptr;      ///< The airport station.
	TileIndex hangar = INVALID_TILE; ///< The hangar tile of the airport.
};

} // namespace

TEST_CASE("agent_game_tools: a part of a chain is a row of its own")
{
	LoadTestLanguagePack();
	InitTestCargoLabels();

	/* The company is declared first, so the vehicles and the station are removed before it. */
	TestCompanyGuard company;
	TestRoadMap map;
	TestConsistEngines engines;
	TestConsistVehicles vehicles;
	TestConsistHangar hangar(company.Id(), TileXY(30, 30));

	/* A wagon behind the locomotive: its row is the row of that wagon, and the depot fields of the
	 * engine answer for the part and for the chain it is in. */
	const TileIndex tile = TileXY(10, 10);
	Train *loco = vehicles.Create<Train>();
	Train *wagon = vehicles.Create<Train>();
	SetupTrainPart(loco, company.Id(), engines.loco->index, tile, true);
	SetupTrainPart(wagon, company.Id(), engines.wagon->index, tile, false);
	LoadCargo(loco, TEST_CARGO_COAL, 0, 0);
	LoadCargo(wagon, TEST_CARGO_COAL, 50, 12);
	loco->SetNext(wagon);
	REQUIRE(wagon->First() == loco);
	REQUIRE(wagon->Previous() == loco);
	/* The part is on a depot track bit of its own while the chain is not stopped in a depot, so the
	 * per-part state below has to be the state of the part and not the state of the chain. */
	wagon->track = Track::Depot;

	const Json wagon_row = VehicleRowOf(wagon);
	CHECK(wagon_row["is_primary"] == false);
	CHECK(wagon_row["primary_ref"]["id"] == static_cast<int64_t>(loco->index.base()));
	CHECK(wagon_row["primary_ref"]["generation"] == loco->generation);
	CHECK_FALSE(wagon_row.contains("consist"));
	CHECK(wagon_row["cargo"]["type"] == static_cast<int>(TEST_CARGO_COAL));
	CHECK(wagon_row["cargo"]["load"] == 12);
	CHECK(wagon_row["cargo"]["capacity"] == 50);
	CHECK(wagon_row["in_depot_state"] == true);
	/* The chain of the wagon is not stopped in a depot, so the chain state is false for the part. */
	CHECK(wagon_row["in_depot"] == false);
	CHECK(wagon_row["halted_in_depot"] == false);
	CHECK(wagon_row["depot_ref"].is_null());
	CHECK(wagon_row["status"] == "travelling");

	/* The same part with the flag the engine sets on a vehicle it halted: the state name is the state
	 * of the chain, which is not a depot, so the part reads as stopped and not as in a depot. */
	wagon->vehstatus.Set(VehState::Stopped);
	const Json halted_row = VehicleRowOf(wagon);
	CHECK(halted_row["stopped"] == true);
	CHECK(halted_row["status"] == "stopped");
	CHECK(halted_row["halted_in_depot"] == false);
	wagon->vehstatus.Reset(VehState::Stopped);

	/* The shadow of an aircraft that stands in its hangar: a part, read through the aircraft where the
	 * engine only answers for a primary vehicle. */
	REQUIRE(IsHangarTile(hangar.Hangar()));
	Aircraft *plane = vehicles.Create<Aircraft>();
	Aircraft *shadow = vehicles.Create<Aircraft>();
	plane->owner = company.Id();
	plane->engine_type = engines.aircraft->index;
	plane->tile = hangar.Hangar();
	plane->subtype = AIR_AIRCRAFT;
	plane->vehstatus = {VehState::Hidden, VehState::Stopped, VehState::DefaultPalette};
	shadow->owner = company.Id();
	shadow->engine_type = engines.aircraft->index;
	shadow->tile = hangar.Hangar();
	shadow->subtype = AIR_SHADOW;
	shadow->vehstatus = {VehState::Hidden, VehState::Stopped, VehState::DefaultPalette};
	LoadCargo(plane, TEST_CARGO_PASSENGERS, 100, 40);
	LoadCargo(shadow, TEST_CARGO_MAIL, 20, 5);
	plane->SetNext(shadow);
	REQUIRE(shadow->First() == plane);
	REQUIRE_FALSE(shadow->IsPrimaryVehicle());

	const Json shadow_row = VehicleRowOf(shadow);
	CHECK(shadow_row["is_primary"] == false);
	CHECK(shadow_row["primary_ref"]["id"] == static_cast<int64_t>(plane->index.base()));
	CHECK_FALSE(shadow_row.contains("consist"));
	CHECK(shadow_row["cargo"]["type"] == static_cast<int>(TEST_CARGO_MAIL));
	CHECK(shadow_row["cargo"]["load"] == 5);
	/* The aircraft and its shadow stand in the hangar: the chain state and the depot state of the
	 * shadow are both true, and the shadow is read through the aircraft for the depot state. */
	CHECK(shadow_row["in_depot"] == true);
	CHECK(shadow_row["in_depot_state"] == true);
	CHECK(shadow_row["status"] == "in_depot");
	/* A hangar stores no depot, so an aircraft in its hangar has no depot reference. */
	CHECK(shadow_row["depot_ref"].is_null());

	const Json plane_row = VehicleRowOf(plane);
	CHECK(plane_row["in_depot"] == true);
	CHECK(plane_row["in_depot_state"] == true);
	CHECK(plane_row["status"] == "in_depot");
	CHECK(plane_row["depot_ref"].is_null());
	CHECK(plane_row["consist"]["total_parts"] == 2);

	/* The articulated part of a road vehicle: the depot state of the part is the state of the part,
	 * which is what the front part of the same chain does not have. */
	RoadVehicle *bus = vehicles.Create<RoadVehicle>();
	RoadVehicle *bus_rear = vehicles.Create<RoadVehicle>();
	const TileIndex road_tile = TileXY(14, 14);
	bus->owner = company.Id();
	bus->engine_type = engines.road->index;
	bus->tile = road_tile;
	bus->roadtype = ROADTYPE_ROAD;
	bus->SetFrontEngine();
	bus->state = RVSB_IN_DEPOT;
	bus_rear->owner = company.Id();
	bus_rear->engine_type = engines.road->index;
	bus_rear->tile = road_tile;
	bus_rear->roadtype = ROADTYPE_ROAD;
	bus_rear->SetArticulatedPart();
	/* State 0 is a track direction, which is not `RVSB_IN_DEPOT`. */
	bus_rear->state = 0;
	LoadCargo(bus, TEST_CARGO_PASSENGERS, 40, 20);
	LoadCargo(bus_rear, TEST_CARGO_PASSENGERS, 40, 15);
	bus->SetNext(bus_rear);
	REQUIRE(bus_rear->First() == bus);
	REQUIRE(bus_rear->IsArticulatedPart());
	REQUIRE(bus->IsInDepot());
	REQUIRE_FALSE(bus_rear->IsInDepot());

	const Json bus_rear_row = VehicleRowOf(bus_rear);
	CHECK(bus_rear_row["is_primary"] == false);
	CHECK(bus_rear_row["primary_ref"]["id"] == static_cast<int64_t>(bus->index.base()));
	CHECK_FALSE(bus_rear_row.contains("consist"));
	CHECK(bus_rear_row["cargo"]["load"] == 15);
	CHECK(bus_rear_row["in_depot_state"] == false);
	CHECK(bus_rear_row["in_depot"] == false);
	CHECK(bus_rear_row["depot_ref"].is_null());
}
