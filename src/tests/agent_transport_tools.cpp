/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/>.
 */

/** @file agent_transport_tools.cpp Test the construction tool dispatch of the agent bridge. */

#include "../stdafx.h"

#include "../3rdparty/catch2/catch.hpp"

#include "../agent_transport_tools.h"

#include "../safeguards.h"

namespace {

/**
 * A company id that is never valid, so a write op stops at the company check instead of touching
 * the world. The tests below therefore need no running game.
 */
const CompanyID INVALID_COMPANY{15};

/**
 * Check whether a capability list holds an entry.
 * @param list The capability list.
 * @param entry The entry to look for.
 * @return True when the entry is present.
 */
bool HasEntry(const InGameAgent::Json &list, std::string_view entry)
{
	for (const InGameAgent::Json &item : list) {
		if (item.is_string() && item.get<std::string>() == entry) return true;
	}
	return false;
}

} // namespace

TEST_CASE("agent transport tools: capabilities")
{
	const InGameAgent::Json caps = InGameAgent::TransportToolCapabilities();
	CHECK(caps["module"] == "agent_transport_tools");

	REQUIRE(caps["methods"].is_array());
	REQUIRE(caps["methods"].size() == 1);
	const InGameAgent::Json &construct = caps["methods"][0];
	CHECK(construct["method"] == "world.construct");
	CHECK(construct["authority"] == "write");

	/* Only implemented operations are advertised. */
	for (const std::string_view op : {"rail", "rail.demolish", "signal", "signal.demolish", "station.rail", "station.airport",
			"station.dock", "airport", "dock", "depot.rail", "depot.ship", "bridge", "tunnel", "canal", "terraform", "tram.demolish"}) {
		CHECK(HasEntry(construct["implemented"], op));
	}

	/* The families this module leaves to the road/vehicle module are named as gaps. */
	for (const std::string_view op : {"road", "tram", "demolish", "station.road", "depot.road", "depot.tram", "depot.air"}) {
		CHECK(HasEntry(construct["gaps"], op));
	}

	CHECK(!caps["unimplemented_methods"].empty());
	CHECK(!caps["notes"].empty());
}

TEST_CASE("agent transport tools: unowned operations fall through")
{
	/* Methods this module does not implement at all. */
	for (const std::string_view method : {"world.query", "world.vehicles", "world.orders", "world.info", "world.capture", "task.barrier"}) {
		CHECK_FALSE(InGameAgent::DispatchTransportTool(method, InGameAgent::Json::object(), INVALID_COMPANY).has_value());
	}

	/* Ops the road/vehicle module owns. */
	CHECK_FALSE(InGameAgent::DispatchTransportTool("world.construct", {{"op", "road"}}, INVALID_COMPANY).has_value());
	CHECK_FALSE(InGameAgent::DispatchTransportTool("world.construct", {{"op", "demolish"}}, INVALID_COMPANY).has_value());
	CHECK_FALSE(InGameAgent::DispatchTransportTool("world.construct", {{"op", "tram"}}, INVALID_COMPANY).has_value());
	CHECK_FALSE(InGameAgent::DispatchTransportTool("world.construct", {{"op", "station"}, {"stop_type", "bus"}}, INVALID_COMPANY).has_value());
	CHECK_FALSE(InGameAgent::DispatchTransportTool("world.construct", {{"op", "depot"}, {"road_type", "road"}}, INVALID_COMPANY).has_value());
	CHECK_FALSE(InGameAgent::DispatchTransportTool("world.construct", {{"op", "depot"}, {"depot_type", "tram"}}, INVALID_COMPANY).has_value());

	/* An unknown op is not this module's either. */
	CHECK_FALSE(InGameAgent::DispatchTransportTool("world.construct", {{"op", "waypoint"}}, INVALID_COMPANY).has_value());
}

TEST_CASE("agent transport tools: argument validation")
{
	/* params must be an object. */
	auto result = InGameAgent::DispatchTransportTool("world.construct", InGameAgent::Json::array(), INVALID_COMPANY);
	REQUIRE(result.has_value());
	CHECK((*result)["ok"] == false);
	CHECK((*result)["error"] == "invalid_arguments");

	/* op is required. */
	result = InGameAgent::DispatchTransportTool("world.construct", InGameAgent::Json::object(), INVALID_COMPANY);
	REQUIRE(result.has_value());
	CHECK((*result)["error"] == "invalid_arguments");

	/* demolish must be a boolean. */
	result = InGameAgent::DispatchTransportTool("world.construct", {{"op", "rail"}, {"demolish", "yes"}}, INVALID_COMPANY);
	REQUIRE(result.has_value());
	CHECK((*result)["error"] == "invalid_arguments");

	/* dry_run must be a boolean. */
	result = InGameAgent::DispatchTransportTool("world.construct", {{"op", "rail"}, {"dry_run", 1}}, INVALID_COMPANY);
	REQUIRE(result.has_value());
	CHECK((*result)["error"] == "invalid_arguments");
}

TEST_CASE("agent transport tools: owned ops need a real company")
{
	/* Every op this module owns stops at the company check before it touches the world. */
	for (const std::string_view op : {"rail", "signal", "station", "airport", "dock", "depot", "bridge", "tunnel", "canal", "terraform"}) {
		InGameAgent::Json params = InGameAgent::Json::object();
		params["op"] = op;
		if (op == "station") params["station_type"] = "rail";
		const std::optional<InGameAgent::Json> result = InGameAgent::DispatchTransportTool("world.construct", params, INVALID_COMPANY);
		REQUIRE(result.has_value());
		CHECK((*result)["ok"] == false);
		CHECK((*result)["error"] == "invalid_company");
	}

	/* Tram removal is the one tram case this module owns. */
	const std::optional<InGameAgent::Json> tram = InGameAgent::DispatchTransportTool("world.construct", {{"op", "tram"}, {"demolish", true}}, INVALID_COMPANY);
	REQUIRE(tram.has_value());
	CHECK((*tram)["error"] == "invalid_company");
}
