/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <http://www.gnu.org/licenses/>.
 */

/**
 * @file test_agent_bridge_refs.cpp Test the typed tile object identity and the reference
 * resolution of the agent bridge: tile categories, state fingerprints, stale validation, the
 * generic `tile` alias, a house with its town lifetime, a map object with its pool lifetime,
 * ownership, points, the road subtypes (a road depot with its pool lifetime, a tram depot, a level
 * crossing, a road stop and a road bridgehead), the error cases and the calendar conversion
 * `world.info` reports.
 *
 * The bridge reads only the requested tile, so a small allocated map is enough: no game loop, no
 * video driver and no command is needed.
 */

#include "../stdafx.h"

#include "../3rdparty/catch2/catch.hpp"

#include "../agent_bridge.h"
#include "../bridge_map.h"
#include "../depot_base.h"
#include "../map_func.h"
#include "../object_base.h"
#include "../object_map.h"
#include "../openttd.h"
#include "../rail_map.h"
#include "../road.h"
#include "../road_map.h"
#include "../station_map.h"
#include "../station_type.h"
#include "../tile_map.h"
#include "../timer/timer_game_calendar.h"
#include "../town.h"
#include "../town_map.h"
#include "../tree_map.h"
#include "../water_map.h"

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
	}

	~MapFixture()
	{
		_game_mode = previous_mode;
	}
};

/** A tile location in the canonical shape. */
Json TilePoint(int x, int y)
{
	return Json{{"x", x}, {"y", y}};
}

/** Find one typed tile object of a category, or an empty object when the tile has none. */
Json FindTileObject(TileIndex tile, std::string_view type)
{
	for (const Json &object : DescribeTileObjects(tile)) {
		if (object["type"] == std::string(type)) return object;
	}
	return Json::object();
}

/** Build the reference a caller would keep for a described tile object. */
Json TileRef(const Json &row)
{
	Json ref = {{"type", row["type"]}, {"tile", row["tile"]}};
	if (row.contains("fingerprint")) ref["fingerprint"] = row["fingerprint"];
	if (row.contains("town_generation") && !row["town_generation"].is_null()) ref["town_generation"] = row["town_generation"];
	return ref;
}

/**
 * One real road depot with a real road depot tile, removed again when the fixture goes out of scope.
 *
 * The tile carries the depot id and the owner, exactly as the engine's own road depot builder writes
 * them, so the fingerprint is computed on a tile the game itself can produce.
 */
class TestRoadDepot {
public:
	TestRoadDepot(TileIndex tile, Owner owner, DiagDirection dir, RoadType rt = ROADTYPE_ROAD)
	{
		REQUIRE(Depot::GetNumItems() == 0);
		REQUIRE(Depot::CanAllocateItem());
		this->depot = Depot::Create(tile);
		MakeRoadDepot(tile, owner, this->depot->index, dir, rt);
	}

	~TestRoadDepot() { _depot_pool.CleanPool(); }

	/**
	 * Demolish the depot and rebuild it on the same tile in the same state: the pool hands out a new
	 * lifetime, so the tile carries a depot the caller has never seen.
	 */
	Depot *Rebuild(TileIndex tile, Owner owner, DiagDirection dir, RoadType rt = ROADTYPE_ROAD)
	{
		delete this->depot;
		REQUIRE(Depot::CanAllocateItem());
		this->depot = Depot::Create(tile);
		MakeRoadDepot(tile, owner, this->depot->index, dir, rt);
		return this->depot;
	}

	Depot *Get() const { return this->depot; }

private:
	Depot *depot = nullptr; ///< The depot the fixture created.
};

/** A town that owns a house tile, created once for the cases that need a lifetime. */
Town *TestTown(TileIndex tile)
{
	for (const Town *existing : Town::Iterate()) return const_cast<Town *>(existing);
	REQUIRE(Town::CanAllocateItem());
	return Town::Create(tile);
}

} // namespace

TEST_CASE("Agent bridge refs: the typed tile categories are distinguished")
{
	MapFixture map;

	/* Plain landscape. */
	const Json terrain = FindTileObject(TileXY(1, 1), "terrain");
	REQUIRE(!terrain.empty());
	CHECK(terrain["identity"] == "tile");
	CHECK(terrain["subtype"].is_string());
	CHECK(terrain["owner"].is_null());
	CHECK(terrain["fingerprint"].is_number_unsigned());

	/* Water. */
	const TileIndex water = TileXY(2, 2);
	MakeWater(water, OWNER_WATER, WaterClass::Sea, 0);
	const Json water_row = FindTileObject(water, "water");
	REQUIRE(!water_row.empty());
	CHECK(water_row["subtype"] == "water");
	CHECK(water_row["fingerprint"] != terrain["fingerprint"]);

	/* Rail, with the track it carries as its subtype. */
	const TileIndex rail = TileXY(3, 3);
	MakeRailNormal(rail, OWNER_NONE, TrackBits{Track::X}, RAILTYPE_RAIL);
	const Json rail_row = FindTileObject(rail, "rail");
	REQUIRE(!rail_row.empty());
	CHECK(rail_row["subtype"] == "x");
	CHECK(rail_row["owner"].is_null());

	/* Road, and tram on a tile that carries both: two objects, not one. */
	const TileIndex road = TileXY(4, 4);
	MakeRoadNormal(road, ROAD_X, ROADTYPE_ROAD, ROADTYPE_TRAM, TownID::Invalid(), OWNER_NONE, OWNER_NONE);
	CHECK(!FindTileObject(road, "road").empty());
	CHECK(!FindTileObject(road, "tram").empty());

	/* A house is a tile object of its own, not the town it belongs to. */
	const TileIndex house = TileXY(5, 5);
	SetTileType(house, TileType::House);
	SetHouseType(house, 0);
	const Json house_row = FindTileObject(house, "house");
	REQUIRE(!house_row.empty());
	CHECK(house_row["identity"] == "tile");
	CHECK(house_row["fingerprint"].is_number_unsigned());
}

TEST_CASE("Agent bridge refs: a tile reference must name the state it was planned for")
{
	MapFixture map;

	const TileIndex tile = TileXY(10, 10);
	MakeRailNormal(tile, OWNER_NONE, TrackBits{Track::X}, RAILTYPE_RAIL);
	const Json row = FindTileObject(tile, "rail");
	REQUIRE(!row.empty());
	const Json ref = TileRef(row);

	/* A read resolves the object as it is and reports the fingerprint to keep. */
	const Json read = ResolveObjectRef(ref, false);
	CHECK(read["found"] == true);
	CHECK(read["type"] == "rail");
	CHECK(read["fingerprint"] == row["fingerprint"]);

	/* The same reference is current for a write as well. */
	CHECK(ResolveObjectRef(ref, true)["found"] == true);

	/* The tile changed after the read: the write is rejected, with the state it found now. */
	MakeRailNormal(tile, OWNER_NONE, TrackBits{Track::Y}, RAILTYPE_RAIL);
	const Json changed = ResolveObjectRef(ref, true);
	CHECK(changed["found"] == false);
	CHECK(changed["error"] == "stale_ref");
	CHECK(changed["reason"] == "tile_state_changed");
	CHECK(changed["fingerprint_now"].is_number_unsigned());
	CHECK(changed["fingerprint_now"] != row["fingerprint"]);

	/* A read of the old reference still resolves, so a caller can see what the tile carries now. */
	CHECK(ResolveObjectRef(ref, false)["found"] == true);

	/* A write has to name the fingerprint at all. */
	const Json without = ResolveObjectRef(Json{{"type", "rail"}, {"tile", TilePoint(10, 10)}}, true);
	CHECK(without["found"] == false);
	CHECK(without["reason"] == "fingerprint_required");
}

TEST_CASE("Agent bridge refs: the generic tile spelling is an alias")
{
	MapFixture map;

	const TileIndex tile = TileXY(12, 12);
	MakeRailNormal(tile, OWNER_NONE, TrackBits{Track::Upper}, RAILTYPE_RAIL);
	const Json row = FindTileObject(tile, "rail");
	REQUIRE(!row.empty());

	/* `what` names the category of the older generic spelling. */
	const Json aliased = ResolveObjectRef(Json{{"type", "tile"}, {"what", "rail"}, {"tile", TilePoint(12, 12)}}, false);
	CHECK(aliased["found"] == true);
	CHECK(aliased["type"] == "rail");
	CHECK(aliased["subtype"] == "upper");

	/* The plural spelling the tile type used to have is accepted as well. */
	const TileIndex trees = TileXY(13, 13);
	MakeTree(trees, TreeType{0}, 1, TreeGrowthStage::Grown, TreeGround::Grass, 1);
	CHECK(ResolveObjectRef(Json{{"type", "tile"}, {"what", "trees"}, {"tile", TilePoint(13, 13)}}, false)["type"] == "tree");

	/* Without `what` the generic spelling names the most specific object of the tile. */
	const Json generic = ResolveObjectRef(Json{{"type", "tile"}, {"tile", TilePoint(12, 12)}}, false);
	CHECK(generic["found"] == true);
	CHECK(generic["type"] == "rail");

	/* An unknown `what` is an invalid argument, not a stale reference. */
	const Json unknown = ResolveObjectRef(Json{{"type", "tile"}, {"what", "planet"}, {"tile", TilePoint(12, 12)}}, false);
	CHECK(unknown["found"] == false);
	CHECK(unknown["error"] == "invalid_params");
	CHECK(unknown["reason"] == "unknown_what");
}

TEST_CASE("Agent bridge refs: a point names a location only")
{
	MapFixture map;

	/* Nothing is checked but the location, so a point is current even for a write. */
	const Json point = ResolveObjectRef(Json{{"type", "point"}, {"tile", TilePoint(7, 7)}}, true);
	CHECK(point["found"] == true);
	CHECK(point["identity"] == "point");
	CHECK(point["tile"] == TilePoint(7, 7));
	CHECK_FALSE(point.contains("fingerprint"));
}

TEST_CASE("Agent bridge refs: a house carries the lifetime of its town")
{
	MapFixture map;

	const TileIndex tile = TileXY(20, 20);
	Town *town = TestTown(tile);
	REQUIRE(town != nullptr);

	SetTileType(tile, TileType::House);
	SetHouseType(tile, 0);
	SetTownIndex(tile, town->index);

	const Json row = FindTileObject(tile, "house");
	REQUIRE(!row.empty());
	CHECK(row["town_id"] == town->index.base());
	CHECK(row["town_generation"] == town->generation);

	/* The tile identity plus the town lifetime is what a write has to name. */
	const Json ref = TileRef(row);
	CHECK(ResolveObjectRef(ref, true)["found"] == true);

	/* A house reference without the town lifetime is not strict enough for a write. */
	Json without = ref;
	without.erase("town_generation");
	const Json missing = ResolveObjectRef(without, true);
	CHECK(missing["found"] == false);
	CHECK(missing["reason"] == "town_generation_required");

	/* A town that was replaced is a different lifetime, even when the id is the same. */
	Json wrong = ref;
	wrong["town_generation"] = row["town_generation"].get<uint64_t>() + 1;
	const Json mismatch = ResolveObjectRef(wrong, true);
	CHECK(mismatch["found"] == false);
	CHECK(mismatch["reason"] == "town_lifetime_mismatch");

	/* The stored house id is part of the tile identity, not only its NewGRF translation: the
	 * translation of an unmapped id can still be the same HouseID, so the raw id has to be in the
	 * fingerprint for the change below to be observable at all. */
	REQUIRE(GetCleanHouseType(tile) == 0);
	SetHouseType(tile, 1);
	REQUIRE(GetCleanHouseType(tile) == 1);
	const Json changed = ResolveObjectRef(ref, true);
	REQUIRE_FALSE(changed["found"].get<bool>());
	CHECK(changed["reason"] == "tile_state_changed");
}

TEST_CASE("Agent bridge refs: the anchor tile of a town names the town as a pooled candidate")
{
	MapFixture map;

	/* A town whose anchor tile carries a road: the tile object alone answered "road", so the town
	 * itself was not selectable on the tile it is placed on. The custom name keeps the row off the
	 * town name generator, which needs the game's name data. */
	const TileIndex anchor = TileXY(30, 30);
	MakeRoadNormal(anchor, ROAD_X, ROADTYPE_ROAD, ROADTYPE_TRAM, TownID::Invalid(), OWNER_NONE, OWNER_NONE);
	REQUIRE(Town::CanAllocateItem());
	Town *town = Town::Create(anchor);
	REQUIRE(town != nullptr);
	town->name = "Testville";
	REQUIRE(town->xy == anchor);

	const Json candidates = PickObjects(anchor);

	/* Both candidates are offered, and the town exactly once. */
	Json town_row = Json::object();
	Json road_row = Json::object();
	int town_count = 0;
	for (const Json &candidate : candidates) {
		if (candidate["type"] == "town") {
			town_row = candidate;
			town_count++;
		}
		if (candidate["type"] == "road") road_row = candidate;
	}
	REQUIRE(!town_row.empty());
	REQUIRE(!road_row.empty());
	CHECK(town_count == 1);

	/* The town is a canonical pooled reference: the pool lifetime, the name, the anchor tile and the
	 * world it was read in. A town is not a tile object, so it carries no fingerprint. */
	CHECK(town_row["identity"] == "pool");
	CHECK(town_row["id"] == town->index.base());
	CHECK(town_row["generation"] == town->generation);
	CHECK(town_row["name"] == "Testville");
	CHECK(town_row["tile"] == TilePoint(30, 30));
	CHECK(town_row["world_id"].is_string());
	CHECK(town_row["epoch"].is_string());
	CHECK_FALSE(town_row.contains("fingerprint"));

	/* The reference resolves, and a stale lifetime of the same id does not. */
	const Json ref = {{"type", "town"}, {"id", town->index.base()}, {"generation", town->generation}};
	CHECK(ResolveObjectRef(ref, true)["found"] == true);
	Json stale = ref;
	stale["generation"] = town->generation + 1;
	CHECK(ResolveObjectRef(stale, true)["found"] == false);

	/* A house of the town carries the association in its own row, and does not repeat the town as a
	 * second candidate, because the house tile is not the town's anchor. */
	const TileIndex house = TileXY(31, 31);
	SetTileType(house, TileType::House);
	SetHouseType(house, 0);
	SetTownIndex(house, town->index);
	const Json house_row = FindTileObject(house, "house");
	REQUIRE(!house_row.empty());
	CHECK(house_row["town_id"] == town->index.base());
	CHECK(house_row["town_generation"] == town->generation);
	for (const Json &candidate : PickObjects(house)) CHECK(candidate["type"] != "town");

	/* Terrain that belongs to no town invents none. */
	const Json plain = PickObjects(TileXY(40, 40));
	REQUIRE(!plain.empty());
	for (const Json &candidate : plain) CHECK(candidate["type"] != "town");
}

TEST_CASE("Agent bridge refs: the error cases are explicit")
{
	MapFixture map;

	/* No location. */
	const Json no_tile = ResolveObjectRef(Json{{"type", "rail"}}, false);
	CHECK(no_tile["found"] == false);
	CHECK(no_tile["error"] == "invalid_params");
	CHECK(no_tile["reason"] == "invalid_tile");

	/* Outside the map. */
	const Json outside = ResolveObjectRef(Json{{"type", "rail"}, {"tile", TilePoint(1000, 1000)}}, false);
	CHECK(outside["found"] == false);
	CHECK(outside["reason"] == "invalid_tile");

	/* A type the kernel cannot resolve at all. */
	const Json unknown = ResolveObjectRef(Json{{"type", "planet"}, {"tile", TilePoint(1, 1)}}, false);
	CHECK(unknown["found"] == false);
	CHECK(unknown["error"] == "unsupported_ref_type");
	CHECK(unknown["reason"] == "unknown_type");

	/* The tile no longer carries that kind of object. */
	const TileIndex tile = TileXY(30, 30);
	MakeRailNormal(tile, OWNER_NONE, TrackBits{Track::X}, RAILTYPE_RAIL);
	const Json rail = TileRef(FindTileObject(tile, "rail"));
	MakeRoadNormal(tile, ROAD_X, ROADTYPE_ROAD, INVALID_ROADTYPE, TownID::Invalid(), OWNER_NONE, OWNER_NONE);
	const Json replaced = ResolveObjectRef(rail, true);
	CHECK(replaced["found"] == false);
	CHECK(replaced["reason"] == "tile_type_mismatch");

	/* A malformed fingerprint is an invalid argument, not a stale reference. */
	const Json bad = ResolveObjectRef(Json{{"type", "road"}, {"tile", TilePoint(30, 30)}, {"fingerprint", "abc"}}, false);
	CHECK(bad["found"] == false);
	CHECK(bad["error"] == "invalid_params");
	CHECK(bad["reason"] == "invalid_fingerprint");
}

TEST_CASE("Agent bridge refs: the calendar comes from the engine conversion")
{
	/* The conversion world.info uses, on a date the game itself produces. The bridge does not do
	 * this arithmetic by hand, so the numbers and the engine's own ISO text agree by construction. */
	const TimerGameCalendar::Date date = TimerGameCalendar::ConvertYMDToDate(
			TimerGameCalendar::Year{1950}, TimerGameCalendar::Month{6}, TimerGameCalendar::Day{12});
	const TimerGameCalendar::YearMonthDay ymd = TimerGameCalendar::ConvertDateToYMD(date);
	CHECK(ymd.year.base() == 1950);
	CHECK(ymd.month == 6);
	CHECK(ymd.day == 12);

	/* The protocol reports the month as 1..12, i.e. the engine's 0-based month plus one, so a
	 * caller never has to know that January is 0. */
	CHECK(static_cast<int>(ymd.month) + 1 == 7);
}

TEST_CASE("Agent bridge refs: ownership is part of the tile identity")
{
	MapFixture map;

	/* A rail tile of company 0. */
	const TileIndex rail = TileXY(40, 40);
	MakeRailNormal(rail, Owner{0}, TrackBits{Track::X}, RAILTYPE_RAIL);
	const Json row = FindTileObject(rail, "rail");
	REQUIRE(!row.empty());
	CHECK(row["owner"] == 0);
	const Json ref = TileRef(row);
	CHECK(ResolveObjectRef(ref, true)["found"] == true);

	/* The owner changed although the track bits did not: the observed state is gone. */
	SetTileOwner(rail, Owner{1});
	const Json changed = ResolveObjectRef(ref, true);
	CHECK(changed["found"] == false);
	CHECK(changed["reason"] == "tile_state_changed");
	CHECK(FindTileObject(rail, "rail")["owner"] == 1);

	/* The road and the tram on one tile can belong to different companies, and each row reports
	 * the owner of its own transport type. */
	const TileIndex road = TileXY(41, 41);
	MakeRoadNormal(road, ROAD_X, ROADTYPE_ROAD, ROADTYPE_TRAM, TownID::Invalid(), Owner{0}, Owner{1});
	CHECK(FindTileObject(road, "road")["owner"] == 0);
	CHECK(FindTileObject(road, "tram")["owner"] == 1);

	/* A canal is owned by the company that dug it; open sea stores OWNER_WATER, which is not one. */
	const TileIndex canal = TileXY(42, 42);
	MakeWater(canal, Owner{2}, WaterClass::Canal, 0);
	CHECK(FindTileObject(canal, "water")["owner"] == 2);

	const TileIndex sea = TileXY(43, 43);
	MakeWater(sea, OWNER_WATER, WaterClass::Sea, 0);
	CHECK(FindTileObject(sea, "water")["owner"].is_null());
}

TEST_CASE("Agent bridge refs: a road depot is fingerprinted without the normal-road accessor")
{
	MapFixture map;

	/* The tile the E12 purchase test crashed on: a built road depot. A depot stores its geometry as
	 * the exit direction, which the normal-road accessor asserts against, so the fingerprint has to
	 * read it through the subtype-safe helper. */
	const TileIndex tile = TileXY(50, 50);
	TestRoadDepot depot(tile, Owner{0}, DiagDirection::SE);
	REQUIRE(depot.Get() != nullptr);

	const Json row = FindTileObject(tile, "road");
	REQUIRE(!row.empty());
	CHECK(row["subtype"] == "road");
	CHECK(row["owner"] == 0);
	CHECK(row["fingerprint"].is_number_unsigned());

	/* A read resolves the depot tile and reports the fingerprint to keep. */
	const Json ref = TileRef(row);
	CHECK(ResolveObjectRef(ref, true)["found"] == true);

	/* Another exit direction is another state of the depot. */
	SetRoadDepotExitDirection(tile, DiagDirection::NW);
	const Json turned = ResolveObjectRef(ref, true);
	CHECK(turned["found"] == false);
	CHECK(turned["reason"] == "tile_state_changed");
	SetRoadDepotExitDirection(tile, DiagDirection::SE);

	/* The same direction after a rebuild is a different depot: the pool lifetime is part of the
	 * identity, so a stale reference cannot act on the depot that replaced the one it saw. */
	REQUIRE(depot.Rebuild(tile, Owner{0}, DiagDirection::SE) != nullptr);
	const Json rebuilt = ResolveObjectRef(ref, true);
	CHECK(rebuilt["found"] == false);
	CHECK(rebuilt["reason"] == "tile_state_changed");
	CHECK(rebuilt["fingerprint_now"] != ref["fingerprint"]);

	/* An ownership change invalidates a reference as well. */
	const Json rebuilt_ref = TileRef(FindTileObject(tile, "road"));
	CHECK(ResolveObjectRef(rebuilt_ref, true)["found"] == true);
	SetTileOwner(tile, Owner{1});
	const Json sold = ResolveObjectRef(rebuilt_ref, true);
	CHECK(sold["found"] == false);
	CHECK(sold["reason"] == "tile_state_changed");
}

TEST_CASE("Agent bridge refs: the other road subtypes are read by subtype")
{
	MapFixture map;

	/* Which road type is a road type and which is a tram type is decided by the masks the engine
	 * fills in with `ResetRoadTypes()` at game start, and they are the state a running game has.
	 * Without them a bare test process classifies ROADTYPE_TRAM as a road type, so the tram depot
	 * below would be built with a road road type and carry no tram row at all. */
	ResetRoadTypes();
	REQUIRE(GetRoadTramType(ROADTYPE_TRAM) == RoadTramType::Tram);

	/* A tram depot is the same kind of tile with a tram road type: the tram row reads it the same way,
	 * and the tile carries no road row because it has no road type. */
	const TileIndex depot_tile = TileXY(51, 51);
	TestRoadDepot tram_depot(depot_tile, Owner{0}, DiagDirection::SW, ROADTYPE_TRAM);
	REQUIRE(tram_depot.Get() != nullptr);
	const Json tram_row = FindTileObject(depot_tile, "tram");
	REQUIRE(!tram_row.empty());
	CHECK(tram_row["fingerprint"].is_number_unsigned());
	CHECK(FindTileObject(depot_tile, "road").empty());
	CHECK(ResolveObjectRef(TileRef(tram_row), true)["found"] == true);

	/* A normal road and a level crossing can carry the same road bits and the same owner: only the
	 * subtype tells them apart, so it is part of the fingerprint. */
	const TileIndex road = TileXY(52, 52);
	MakeRoadNormal(road, ROAD_X, ROADTYPE_ROAD, INVALID_ROADTYPE, TownID::Invalid(), Owner{0}, OWNER_NONE);
	const TileIndex crossing = TileXY(53, 53);
	MakeRoadCrossing(crossing, Owner{0}, OWNER_NONE, Owner{0}, Axis::X, RAILTYPE_RAIL, ROADTYPE_ROAD, INVALID_ROADTYPE, TownID::Invalid());
	const Json road_row = FindTileObject(road, "road");
	const Json crossing_row = FindTileObject(crossing, "road");
	REQUIRE(!road_row.empty());
	REQUIRE(!crossing_row.empty());
	CHECK(road_row["fingerprint"] != crossing_row["fingerprint"]);

	/* The axis of the crossing is part of its state. */
	MakeRoadCrossing(crossing, Owner{0}, OWNER_NONE, Owner{0}, Axis::Y, RAILTYPE_RAIL, ROADTYPE_ROAD, INVALID_ROADTYPE, TownID::Invalid());
	CHECK(FindTileObject(crossing, "road")["fingerprint"] != crossing_row["fingerprint"]);

	/* A road bridgehead reports the transport type and the direction the road bits are stored as. */
	const TileIndex bridge = TileXY(54, 54);
	MakeRoadBridgeRamp(bridge, Owner{0}, Owner{0}, OWNER_NONE, BridgeType{0}, DiagDirection::NE, ROADTYPE_ROAD, INVALID_ROADTYPE);
	const Json bridge_row = FindTileObject(bridge, "bridge");
	REQUIRE(!bridge_row.empty());
	CHECK(bridge_row["fingerprint"].is_number_unsigned());
	MakeRoadBridgeRamp(bridge, Owner{0}, Owner{0}, OWNER_NONE, BridgeType{0}, DiagDirection::SW, ROADTYPE_ROAD, INVALID_ROADTYPE);
	CHECK(FindTileObject(bridge, "bridge")["fingerprint"] != bridge_row["fingerprint"]);

	/* A road stop tile is a station tile: it carries no typed landscape object of its own, so no road
	 * fingerprint is asked for it, and describing it must not read it as a normal road either. */
	const TileIndex stop = TileXY(55, 55);
	MakeDriveThroughRoadStop(stop, Owner{0}, Owner{0}, OWNER_NONE, StationID{0}, StationType::Bus, ROADTYPE_ROAD, INVALID_ROADTYPE, Axis::X);
	CHECK(DescribeTileObjects(stop).empty());
}

TEST_CASE("Agent bridge refs: a map object carries the lifetime of the pooled object")
{
	MapFixture map;

	const TileIndex tile = TileXY(44, 44);
	REQUIRE(Object::CanAllocateItem());
	Object *first = Object::Create(ObjectType{0}, nullptr, TileArea(tile, 1, 1), TimerGameCalendar::Date{0}, 0);
	REQUIRE(first != nullptr);
	MakeObject(tile, Owner{0}, first->index, WaterClass::Invalid, 0);

	const Json row = FindTileObject(tile, "mapobject");
	REQUIRE(!row.empty());
	CHECK(row["object_id"] == first->index.base());
	CHECK(row["object_generation"] == first->generation);
	CHECK(row["owner"] == 0);

	Json ref = TileRef(row);
	ref["object_id"] = row["object_id"];
	ref["object_generation"] = row["object_generation"];
	CHECK(ResolveObjectRef(ref, true)["found"] == true);

	/* A write has to name the pooled lifetime at all. */
	Json without = ref;
	without.erase("object_generation");
	const Json missing = ResolveObjectRef(without, true);
	CHECK(missing["found"] == false);
	CHECK(missing["reason"] == "object_generation_required");

	/* The object is replaced by one of the same type: the pool hands out a new lifetime, so the
	 * reference is no longer the object the caller planned for. */
	const uint64_t first_generation = first->generation;
	const ObjectType first_type = first->type;
	delete first;
	REQUIRE(Object::CanAllocateItem());
	Object *second = Object::Create(ObjectType{first_type}, nullptr, TileArea(tile, 1, 1), TimerGameCalendar::Date{0}, 0);
	REQUIRE(second != nullptr);
	MakeObject(tile, Owner{0}, second->index, WaterClass::Invalid, 0);
	REQUIRE(second->generation != first_generation);

	const Json replaced = ResolveObjectRef(ref, true);
	CHECK(replaced["found"] == false);
	CHECK(replaced["reason"] == "object_lifetime_mismatch");
	CHECK(replaced["generation_now"] == second->generation);

	/* A read of the old reference still resolves, so a caller can see what the tile carries now. */
	CHECK(ResolveObjectRef(ref, false)["found"] == true);
}

TEST_CASE("Agent bridge refs: the backend acknowledgement of a result")
{
	/* An applied result is delivered and applied. */
	const Json applied = Json::object({{"ok", true}, {"applied", true}, {"request_id", "r1"}, {"status", "applied"}});
	CHECK(ParseResultAck(applied, "r1").delivered);
	CHECK(ParseResultAck(applied, "r1").applied);

	/* A duplicate delivery is answered `applied: false, status: "applied"`: it was applied before. */
	const Json duplicate = Json::object({{"ok", true}, {"applied", false}, {"request_id", "r1"}, {"status", "applied"}});
	CHECK(ParseResultAck(duplicate, "r1").delivered);
	CHECK(ParseResultAck(duplicate, "r1").applied);

	/* A failed result arrived but was not applied, so it may never start a load. */
	const Json failed = Json::object({{"ok", true}, {"applied", false}, {"request_id", "r1"}, {"status", "failed"}});
	CHECK(ParseResultAck(failed, "r1").delivered);
	CHECK_FALSE(ParseResultAck(failed, "r1").applied);

	/* `applied: true` with `status: "failed"` contradicts itself and never starts a load. */
	const Json contradicting = Json::object({{"ok", true}, {"applied", true}, {"request_id", "r1"}, {"status", "failed"}});
	CHECK(ParseResultAck(contradicting, "r1").delivered);
	CHECK_FALSE(ParseResultAck(contradicting, "r1").applied);

	/* An answer of another request, a refused answer or a body that is not an object is no delivery. */
	CHECK_FALSE(ParseResultAck(applied, "r2").delivered);
	CHECK_FALSE(ParseResultAck(Json::object({{"ok", false}, {"applied", true}, {"request_id", "r1"}, {"status", "applied"}}), "r1").delivered);
	CHECK_FALSE(ParseResultAck(Json::object({{"ok", true}, {"applied", true}}), "r1").delivered);
	CHECK_FALSE(ParseResultAck(Json::array(), "r1").delivered);

	/* A missing or non-boolean `applied`, and a status that is neither applied nor failed, are no delivery:
	 * the result is retried, so a waiting load keeps waiting. */
	CHECK_FALSE(ParseResultAck(Json::object({{"ok", true}, {"request_id", "r1"}, {"status", "applied"}}), "r1").delivered);
	CHECK_FALSE(ParseResultAck(Json::object({{"ok", true}, {"applied", "true"}, {"request_id", "r1"}, {"status", "applied"}}), "r1").delivered);
	CHECK_FALSE(ParseResultAck(Json::object({{"ok", true}, {"applied", true}, {"request_id", "r1"}, {"status", "unknown"}}), "r1").delivered);
	CHECK_FALSE(ParseResultAck(Json::object({{"ok", true}, {"applied", true}, {"request_id", "r1"}}), "r1").delivered);

	/* An empty or legacy body is no acknowledgement at all, so it cannot discard a waiting load. */
	CHECK_FALSE(ParseResultAck(Json::object(), "r1").delivered);
	CHECK_FALSE(ParseResultAck(Json::object({{"ok", true}}), "r1").delivered);
	CHECK_FALSE(ParseResultAck(Json::object({{"ok", true}}), "r1").applied);
}

TEST_CASE("Agent bridge refs: a pending load belongs to one world, epoch and player")
{
	CHECK(PendingLoadStillCurrent("w1", "e1", static_cast<CompanyID>(0), "w1", "e1", static_cast<CompanyID>(0)));
	CHECK_FALSE(PendingLoadStillCurrent("w1", "e1", static_cast<CompanyID>(0), "w2", "e1", static_cast<CompanyID>(0)));
	CHECK_FALSE(PendingLoadStillCurrent("w1", "e1", static_cast<CompanyID>(0), "w1", "e2", static_cast<CompanyID>(0)));
	CHECK_FALSE(PendingLoadStillCurrent("w1", "e1", static_cast<CompanyID>(0), "w1", "e1", static_cast<CompanyID>(1)));
	/* A request that named no company is not tied to one. */
	CHECK(PendingLoadStillCurrent("w1", "e1", CompanyID::Invalid(), "w1", "e1", static_cast<CompanyID>(3)));

	/* A barrier of the task that asked for the load cancels it, a barrier of another task does not. */
	CHECK(PendingLoadCancelledByBarrier("t1", "t1"));
	CHECK_FALSE(PendingLoadCancelledByBarrier("t1", "t2"));
	CHECK_FALSE(PendingLoadCancelledByBarrier("", ""));

	/* The player guard uses the real local player, not the company a full-control request acted for. */
	ArmPendingLoadForTest("r1", "/tmp/agent-bridge-pending-load.sav", "t1", static_cast<CompanyID>(2), static_cast<CompanyID>(0));
	REQUIRE(HasPendingLoad());
	CHECK(PendingLoadPlayer() == static_cast<CompanyID>(0));
	CHECK(PendingLoadStillCurrent("", "", PendingLoadPlayer(), "", "", static_cast<CompanyID>(0)));
	CHECK_FALSE(PendingLoadStillCurrent("", "", PendingLoadPlayer(), "", "", static_cast<CompanyID>(1)));

	/* A second load is refused while one waits, and only a load is refused. */
	CHECK(SavegameLoadRefused("world.project", "load", true));
	CHECK_FALSE(SavegameLoadRefused("world.project", "load", false));
	CHECK_FALSE(SavegameLoadRefused("world.project", "save", true));
	CHECK_FALSE(SavegameLoadRefused("world.project", "list_saves", true));
	CHECK_FALSE(SavegameLoadRefused("world.towns", "load", true));

	/* A world reset drops the armed load, so a late acknowledgement cannot apply it. */
	ResetWorld();
	CHECK_FALSE(HasPendingLoad());
}
