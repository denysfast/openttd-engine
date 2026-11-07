/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <http://www.gnu.org/licenses/>.
 */

/**
 * @file test_agent_bridge_query.cpp Test the kinds the kernel answers for `world.query` itself:
 * a depot row is the depot pool item plus the owner and kind the engine records for its tile, a
 * waypoint row is the station pool item with its own lifetime, and a station row carries the
 * engine's own facility flags instead of a family guessed from the station name.
 *
 * The cases use real pool items and a small allocated map, and they remove exactly what they created:
 * every fixture requires an empty pool first and removes its items through the pool's own `CleanPool`,
 * which is the engine's path for deleting items without the game-dependent teardown.
 */

#include "../stdafx.h"

#include "../3rdparty/catch2/catch.hpp"

#include "../agent_bridge.h"
#include "../company_type.h"
#include "../core/enum_type.hpp"
#include "../depot_base.h"
#include "../map_func.h"
#include "../openttd.h"
#include "../rail_map.h"
#include "../station_base.h"
#include "../station_type.h"
#include "../tile_map.h"
#include "../waypoint_base.h"

#include "../safeguards.h"

using namespace InGameAgent;

namespace {

/** A 64x64 map, with the previous game mode restored when the fixture goes out of scope. */
struct QueryMapFixture {
	GameMode previous_mode = _game_mode; ///< Mode to restore.

	QueryMapFixture()
	{
		Map::Allocate(64, 64);
		_game_mode = GameMode::Normal;
	}

	~QueryMapFixture()
	{
		_game_mode = previous_mode;
	}
};

/**
 * One real depot with a real rail depot tile, removed again when the fixture goes out of scope.
 *
 * The tile carries the depot id and the owner, exactly as the engine's own rail depot builder writes
 * them, so the row's owner and kind come from the map and not from the test.
 */
class QueryDepot {
public:
	QueryDepot(TileIndex tile, Owner owner)
	{
		REQUIRE(Depot::GetNumItems() == 0);
		REQUIRE(Depot::CanAllocateItem());
		this->depot = Depot::Create(tile);
		MakeRailDepot(tile, owner, this->depot->index, DiagDirection::NE, RAILTYPE_RAIL);
	}

	~QueryDepot() { _depot_pool.CleanPool(); }

	Depot *Get() const { return this->depot; }

private:
	Depot *depot = nullptr; ///< The depot the fixture created.
};

/** One real station in the engine's own station pool, removed again when the fixture goes out of scope. */
class QueryStation {
public:
	QueryStation(TileIndex tile, Owner owner)
	{
		REQUIRE(Station::GetNumItems() == 0);
		REQUIRE(Station::CanAllocateItem());
		this->station = Station::Create(tile);
		this->station->owner = owner;
	}

	~QueryStation() { _station_pool.CleanPool(); }

	Station *Get() const { return this->station; }

private:
	Station *station = nullptr; ///< The station the fixture created.
};

/** One real waypoint in the engine's own station pool, removed again when the fixture goes out of scope. */
class QueryWaypoint {
public:
	QueryWaypoint(TileIndex tile, Owner owner)
	{
		REQUIRE(Station::GetNumItems() == 0);
		REQUIRE(Station::CanAllocateItem());
		this->waypoint = Waypoint::Create(tile);
		this->waypoint->owner = owner;
	}

	~QueryWaypoint() { _station_pool.CleanPool(); }

	Waypoint *Get() const { return this->waypoint; }

private:
	Waypoint *waypoint = nullptr; ///< The waypoint the fixture created.
};

/**
 * The value of a successful method answer.
 *
 * A bridge method answers the envelope `{ok, value}` (agent_bridge.cpp: `Ok()`), so the query result
 * is the `value` member and not the envelope itself. The shape is required before it is indexed, so a
 * broken envelope is a clear failure instead of a missing-key exception.
 * @param answer The answer of the method.
 * @return The value of the answer.
 */
Json ValueOf(const Json &answer)
{
	REQUIRE(answer.is_object());
	REQUIRE(answer.contains("ok"));
	REQUIRE(answer["ok"] == true);
	REQUIRE(answer.contains("value"));
	return answer["value"];
}

} // namespace

TEST_CASE("Agent bridge query: a depot row is the pool item plus the owner and kind of its tile")
{
	QueryMapFixture map;
	const TileIndex tile = TileXY(10, 10);
	QueryDepot depot(tile, CompanyID{3});
	depot.Get()->name = "Depot A";

	const Json all = ValueOf(QueryWorldForTest({{"kind", "depots"}}));
	REQUIRE(all["total"] == 1);
	REQUIRE(all["rows"].is_array());
	REQUIRE(all["rows"].size() == 1);
	const Json &row = all["rows"][0];
	CHECK(row["id"] == depot.Get()->index.base());
	CHECK(row["generation"] == depot.Get()->generation);
	CHECK(row["name"] == "Depot A");
	CHECK(row["tile"]["x"] == 10);
	CHECK(row["tile"]["y"] == 10);
	/* The owner and the kind are the ones the engine records for the depot tile. */
	CHECK(row["owner"] == 3);
	CHECK(row["what"] == "rail");
	CHECK(row["town_id"] == -1);

	/* The singular alias reaches the same kind. */
	CHECK(ValueOf(QueryWorldForTest({{"kind", "depot"}}))["total"] == 1);

	/* The filters the other kinds have are the ones this kind applies. */
	CHECK(ValueOf(QueryWorldForTest({{"kind", "depots"}, {"filters", {{"company_id", 3}}}}))["total"] == 1);
	CHECK(ValueOf(QueryWorldForTest({{"kind", "depots"}, {"filters", {{"company_id", 4}}}}))["total"] == 0);
	CHECK(ValueOf(QueryWorldForTest({{"kind", "depots"}, {"filters", {{"id", depot.Get()->index.base()}}}}))["total"] == 1);
	CHECK(ValueOf(QueryWorldForTest({{"kind", "depots"}, {"filters", {{"id", 9}}}}))["total"] == 0);
	CHECK(ValueOf(QueryWorldForTest({{"kind", "depots"}, {"filters", {{"name_contains", "depot a"}}}}))["total"] == 1);
	CHECK(ValueOf(QueryWorldForTest({{"kind", "depots"}, {"filters", {{"name_contains", "no such depot"}}}}))["total"] == 0);
	CHECK(ValueOf(QueryWorldForTest({{"kind", "depots"}, {"filters", {{"x0", 10}, {"y0", 10}, {"x1", 20}, {"y1", 20}}}}))["total"] == 1);
	CHECK(ValueOf(QueryWorldForTest({{"kind", "depots"}, {"filters", {{"x0", 20}, {"y0", 20}, {"x1", 30}, {"y1", 30}}}}))["total"] == 0);

	/* Pagination is the existing one: the total counts the matches, not the returned rows. */
	const Json page = ValueOf(QueryWorldForTest({{"kind", "depots"}, {"offset", 1}}));
	CHECK(page["total"] == 1);
	CHECK(page["rows"].empty());
}

TEST_CASE("Agent bridge query: a waypoint row is the station pool item with its own lifetime")
{
	QueryMapFixture map;
	const TileIndex tile = TileXY(5, 5);
	QueryWaypoint waypoint(tile, CompanyID{2});
	waypoint.Get()->name = "Waypoint A";

	const Json all = ValueOf(QueryWorldForTest({{"kind", "waypoints"}}));
	REQUIRE(all["total"] == 1);
	REQUIRE(all["rows"].is_array());
	REQUIRE(all["rows"].size() == 1);
	const Json &row = all["rows"][0];
	CHECK(row["id"] == waypoint.Get()->index.base());
	CHECK(row["generation"] == waypoint.Get()->generation);
	CHECK(row["name"] == "Waypoint A");
	/* The station pool rows carry the tile as `[x, y]`, the shape this kind already answers; the
	 * depot row below is the one that carries `{x, y}`. */
	REQUIRE(row["tile"].is_array());
	REQUIRE(row["tile"].size() == 2);
	CHECK(row["tile"][0] == 5);
	CHECK(row["tile"][1] == 5);
	CHECK(row["owner"] == 2);
	CHECK(row["town_id"] == -1);
	/* A waypoint carries the engine's own facility flag for it, so a station row cannot be confused
	 * with a waypoint row. */
	REQUIRE(row["facilities"].is_object());
	CHECK(row["facilities"]["waypoint"] == true);

	/* A waypoint is not a station: the station kind does not report it. */
	CHECK(ValueOf(QueryWorldForTest({{"kind", "stations"}}))["total"] == 0);
	CHECK(ValueOf(QueryWorldForTest({{"kind", "waypoint"}}))["total"] == 1);

	CHECK(ValueOf(QueryWorldForTest({{"kind", "waypoints"}, {"filters", {{"company_id", 2}}}}))["total"] == 1);
	CHECK(ValueOf(QueryWorldForTest({{"kind", "waypoints"}, {"filters", {{"company_id", 5}}}}))["total"] == 0);
	CHECK(ValueOf(QueryWorldForTest({{"kind", "waypoints"}, {"filters", {{"name_contains", "waypoint a"}}}}))["total"] == 1);
	CHECK(ValueOf(QueryWorldForTest({{"kind", "waypoints"}, {"filters", {{"name_contains", "station"}}}}))["total"] == 0);
	CHECK(ValueOf(QueryWorldForTest({{"kind", "waypoints"}, {"filters", {{"x0", 0}, {"y0", 0}, {"x1", 4}, {"y1", 4}}}}))["total"] == 0);
	CHECK(ValueOf(QueryWorldForTest({{"kind", "waypoints"}, {"filters", {{"x0", 0}, {"y0", 0}, {"x1", 9}, {"y1", 9}}}}))["total"] == 1);
}

TEST_CASE("Agent bridge query: a station row carries the engine's own facility flags")
{
	QueryMapFixture map;
	QueryStation station(TileXY(20, 20), CompanyID{1});
	Station *st = station.Get();
	st->name = "Mixed station";
	/* A station can have several facilities at once; the row reports the flags, not one family. */
	st->facilities = StationFacilities({StationFacility::Train, StationFacility::BusStop, StationFacility::Dock});

	const Json all = ValueOf(QueryWorldForTest({{"kind", "stations"}}));
	REQUIRE(all["total"] == 1);
	REQUIRE(all["rows"].is_array());
	REQUIRE(all["rows"].size() == 1);
	const Json &row = all["rows"][0];
	REQUIRE(row["facilities"].is_object());
	CHECK(row["facilities"]["rail"] == true);
	CHECK(row["facilities"]["bus_stop"] == true);
	CHECK(row["facilities"]["dock"] == true);
	CHECK(row["facilities"]["truck_stop"] == false);
	CHECK(row["facilities"]["airport"] == false);
	CHECK(row["facilities"]["waypoint"] == false);

	/* world.objects resolves the same station with the same flags, so a caller that keeps the ref
	 * does not have to guess the facilities again. The seam answers the resolved row itself, not the
	 * `{ok, value}` envelope. */
	const Json resolved = ResolveObjectRef({{"type", "station"}, {"id", st->index.base()}, {"generation", st->generation}}, false);
	REQUIRE(resolved["found"] == true);
	CHECK(resolved["id"] == st->index.base());
	CHECK(resolved["facilities"] == row["facilities"]);
}
