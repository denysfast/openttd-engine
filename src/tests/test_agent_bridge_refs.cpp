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
 * crossing, a road stop and a road bridgehead), the pooled structures a station or an industry tile
 * stands on (read, guarded removal through the real command path, rebuilt lifetime), the error
 * cases and the calendar conversion `world.info` reports.
 *
 * The bridge reads only the requested tile, so a small allocated map is enough: no game loop and no
 * video driver. The guarded removal case posts the engine's own demolition command, which updates the
 * station sign, so that case loads the language pack the build generated and bootstraps the engine's
 * own font caches for the sign measurement.
 */

#include "../stdafx.h"

#include "../3rdparty/catch2/catch.hpp"

#include <filesystem>

#include "../agent_bridge.h"
#include "../agent_command_catalog.h"
#include "../bridge_map.h"
#include "../company_func.h"
#include "../core/backup_type.hpp"
#include "../depot_base.h"
#include "../fontcache.h"
#include "../industry.h"
#include "../industry_map.h"
#include "../language.h"
#include "../map_func.h"
#include "../object_base.h"
#include "../object_map.h"
#include "../openttd.h"
#include "../rail_map.h"
#include "../road.h"
#include "../roadstop_base.h"
#include "../company_base.h"
#include "../settings_internal.h"
#include "../settings_type.h"
#include "../signal_func.h"
#include "../road_map.h"
#include "../roadveh.h"
#include "../station_base.h"
#include "../station_map.h"
#include "../station_type.h"
#include "../tile_map.h"
#include "../timer/timer_game_calendar.h"
#include "../town.h"
#include "../town_map.h"
#include "../tree_map.h"
#include "../tunnel_map.h"
#include "../vehicle_base.h"
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

/**
 * Load the language pack the build generated, so this binary formats real strings.
 *
 * The test binary has no language pack of its own: `GetString()` aborts on the first string that is
 * not in the loaded table ("String 0x8826 is invalid"), and a real station removal updates the
 * station sign, which formats `STR_VIEWPORT_STATION` and `STR_STATION_NAME`. The pack is the real
 * `english.lng` the build writes to `${CMAKE_BINARY_DIR}/lang`; CTest passes its path in
 * `OTTD_TEST_LANGUAGE_FILE`, and the relative path is the fallback for a manual run from the build
 * directory.
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
 * Give the engine the font caches its own string layout asks for, and take them away again.
 *
 * The test binary never loads a base set, so no font cache exists, and the string layout a real
 * removal runs asks `FontCache::Get()` for the font of the line and dereferences the answer: the
 * sign measurement of the station name segfaults on the null cache
 * (`ICUParagraphLayoutFactory::GetParagraphLayout`). `FontCache::InitializeFontCaches()` is the
 * engine's own bootstrap: it registers the built-in sprite font cache for every size, which needs
 * no base set, no font file and no renderer. Its glyph width is zero while the glyph is not loaded
 * (`SpriteFontCache::GetGlyphWidth`), and a zero width is all a read-only sign measurement needs.
 *
 * Only a binary without caches is bootstrapped and only that bootstrap is undone, so a cache a
 * later case set up (the mock environment of the widget tests) is left alone.
 */
struct TestFontCaches {
	bool initialized = false; ///< Whether this fixture registered the caches.

	TestFontCaches()
	{
		this->initialized = FontCache::Get(FontSize::Normal) == nullptr;
		if (this->initialized) FontCache::InitializeFontCaches();
	}

	~TestFontCaches()
	{
		if (this->initialized) FontCache::UninitializeFontCaches();
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

/**
 * A real company for the structure cases, removed again when the guard goes out of scope.
 *
 * It carries the two things `CmdLandscapeClear` needs before it posts: money to pay for the
 * demolition and a clearing quota (`landscape.cpp: CmdLandscapeClear` rejects a company whose
 * `clear_limit` has no frame left), which a company created outside a running game does not have.
 */
class StructureTestCompany {
public:
	StructureTestCompany()
	{
		REQUIRE(Company::CanAllocateItem());
		this->company = Company::Create();
		REQUIRE(this->company != nullptr);
		this->company->money = 100000;
		this->company->clear_limit = UINT32_MAX;
	}

	~StructureTestCompany() { delete this->company; }

	CompanyID Id() const { return this->company->index; }

private:
	Company *company = nullptr; ///< The company the fixture created.
};

/**
 * One real bus stop: a `Station` and a `RoadStop` in the engine's own pools, with the bus stop tile
 * the engine's own builder writes, removed again when the fixture goes out of scope.
 *
 * The state is what `CmdBuildRoadStop` writes for a one tile bay stop: the station owns the stop, the
 * tile carries the station id and the owner, the station lists the tile in its bus area and its
 * spread, and the company counts one station tile. The removal path of the engine reads exactly
 * that, so the fixture is the state a real build leaves behind.
 */
class TestBusStop {
public:
	TestBusStop(TileIndex tile, Owner owner)
	{
		REQUIRE(Station::GetNumItems() == 0);
		REQUIRE(RoadStop::GetNumItems() == 0);
		Create(tile, owner);
	}

	~TestBusStop()
	{
		_station_pool.CleanPool();
		_roadstop_pool.CleanPool();
	}

	/**
	 * Remove the stop and build it again on the same tile: the pool hands out the same id with a new
	 * lifetime, so the tile carries a station the caller has never seen.
	 */
	Station *Rebuild(TileIndex tile, Owner owner)
	{
		_station_pool.CleanPool();
		_roadstop_pool.CleanPool();
		return Create(tile, owner);
	}

	Station *Get() const { return this->station; }

private:
	Station *station = nullptr; ///< The station the fixture created.

	Station *Create(TileIndex tile, Owner owner)
	{
		REQUIRE(Station::CanAllocateItem());
		REQUIRE(RoadStop::CanAllocateItem());
		this->station = Station::Create(tile);
		REQUIRE(this->station != nullptr);
		/* The removal updates the station sign, and the sign asks for the station name: a name of its
		 * own keeps that off the town name generator (`Station::UpdateVirtCoord` ->
		 * `BaseStation::GetCachedName` -> `Station::FillCachedName`, which otherwise needs
		 * `st->town`). A real station built by the engine has a town for that. */
		this->station->name = "Test bus stop";

		MakeRoadStop(tile, owner, this->station->index, RoadStopType::Bus, ROADTYPE_ROAD, INVALID_ROADTYPE, DiagDirection::NE);

		RoadStop *stop = RoadStop::Create(tile);
		REQUIRE(stop != nullptr);
		this->station->bus_stops = stop;
		this->station->bus_station.Add(tile);
		this->station->spread.Add(tile);
		this->station->AddFacility(StationFacility::BusStop, tile);
		/* `AddFacility` takes the owner from the current company, which is what `CmdBuildRoadStop`
		 * relies on; the fixture builds outside a command, so it names the owner itself. */
		this->station->owner = owner;
		Company::Get(owner)->infrastructure.station++;
		return this->station;
	}
};

/** One real industry tile in the engine's own industry pool, removed again with the fixture. */
class TestIndustry {
public:
	TestIndustry(TileIndex tile) { Create(tile); }

	~TestIndustry() { _industry_pool.CleanPool(); }

	/**
	 * Remove the industry and build it again on the same tile: the pool hands out the same id with a
	 * new lifetime, so the tile carries an industry the caller has never seen.
	 */
	Industry *Rebuild(TileIndex tile)
	{
		_industry_pool.CleanPool();
		return Create(tile);
	}

	Industry *Get() const { return this->industry; }

private:
	Industry *industry = nullptr; ///< The industry the fixture created.

	Industry *Create(TileIndex tile)
	{
		REQUIRE(Industry::CanAllocateItem());
		this->industry = Industry::Create(tile);
		REQUIRE(this->industry != nullptr);
		MakeIndustry(tile, this->industry->index, IndustryGfx{0}, 0, WaterClass::Invalid);
		return this->industry;
	}
};

/**
 * The generic tile reference a caller keeps for the structure that stands on `tile`.
 *
 * It is the read the agent does: `{type: "tile", tile}`, and the answer is the row the caller then
 * hands back as the guard of a write.
 */
Json ReadStructureRef(TileIndex tile)
{
	const Json read = ResolveObjectRef(Json{{"type", "tile"}, {"tile", TilePoint(TileX(tile), TileY(tile))}}, false);
	REQUIRE(read["found"] == true);
	Json ref = {{"type", "tile"}, {"tile", read["tile"]}, {"what", read["what"]}, {"fingerprint", read["fingerprint"]}};
	return ref;
}

/**
 * Run one engine command through the catalog's own write path, the one `world.command` runs.
 *
 * The catalog checks `expected_targets` itself; `expected_refs` are the bridge's guard and are not
 * part of this call, so a case that needs them checks `ResolveObjectRef(ref, true)` itself.
 */
Json CallCommandAs(const Json &params, CompanyID company)
{
	const std::optional<Json> result = DispatchCommandCatalog("world.command", params, company);
	REQUIRE(result.has_value());
	return *result;
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

TEST_CASE("Agent bridge refs: the slope of the tile is part of every tile object")
{
	MapFixture map;
	/* Which road type is a tram type is decided by the masks the engine fills in at game start. */
	ResetRoadTypes();

	/*
	 * `TileHeight()` is the north corner of the tile only; the other three corners are the north
	 * corners of the neighbouring tiles. A landscape change there leaves the height of the tile
	 * alone and moves the slope the object is drawn on, so every category has to mix the slope as
	 * well. The fixtures are two tiles apart, so the corner changed for one of them is never a
	 * corner of another one, and the corner is put back after every check.
	 */
	const TileIndex terrain = TileXY(10, 10);
	const TileIndex water = TileXY(10, 12);
	const TileIndex tree = TileXY(10, 14);
	const TileIndex rail = TileXY(10, 16);
	const TileIndex signal = TileXY(10, 18);
	const TileIndex road = TileXY(10, 20);
	const TileIndex bridge = TileXY(10, 22);
	const TileIndex tunnel = TileXY(10, 24);
	const TileIndex house = TileXY(10, 26);
	const TileIndex mapobject = TileXY(10, 28);

	MakeWater(water, OWNER_WATER, WaterClass::Sea, 0);
	MakeTree(tree, TreeType{0}, 0, TreeGrowthStage::Grown, TreeGround::Grass, 3);
	MakeRailNormal(rail, Owner{0}, TrackBits{Track::X}, RAILTYPE_RAIL);
	/* A signal is a rail tile that carries signals; it is the more specific row of that tile. */
	MakeRailNormal(signal, Owner{0}, TrackBits{Track::X}, RAILTYPE_RAIL);
	SetHasSignals(signal, true);
	SetPresentSignals(signal, SignalOnTrack(Track::X));
	/* A road tile that carries both families: two rows, both of the same shared cell. */
	MakeRoadNormal(road, ROAD_X, ROADTYPE_ROAD, ROADTYPE_TRAM, TownID::Invalid(), Owner{0}, Owner{1});
	MakeRailBridgeRamp(bridge, Owner{0}, BridgeType{0}, DiagDirection::NE, RAILTYPE_RAIL);
	MakeRailTunnel(tunnel, Owner{0}, DiagDirection::NE, RAILTYPE_RAIL);
	SetTileType(house, TileType::House);
	SetHouseType(house, 0);
	REQUIRE(Object::CanAllocateItem());
	Object *object = Object::Create(ObjectType{0}, nullptr, TileArea(mapobject, 1, 1), TimerGameCalendar::Date{0}, 0);
	REQUIRE(object != nullptr);
	MakeObject(mapobject, Owner{0}, object->index, WaterClass::Invalid, 0);

	/* One check per category: the north corner height stays, the slope and the fingerprint move. */
	const auto slope_is_mixed = [](TileIndex tile, std::string_view type) {
		const Json before = FindTileObject(tile, type);
		REQUIRE(!before.empty());
		const uint height = TileHeight(tile);
		const auto slope = GetTileSlope(tile);
		const TileIndex corner = TileXY(TileX(tile) + 1, TileY(tile) + 1);
		REQUIRE(TileHeight(corner) < MAX_TILE_HEIGHT);
		SetTileHeight(corner, TileHeight(corner) + 1);
		CHECK(TileHeight(tile) == height);
		CHECK(GetTileSlope(tile) != slope);
		CHECK(FindTileObject(tile, type)["fingerprint"] != before["fingerprint"]);
		SetTileHeight(corner, TileHeight(corner) - 1);
	};

	slope_is_mixed(terrain, "terrain");
	slope_is_mixed(water, "water");
	slope_is_mixed(tree, "tree");
	slope_is_mixed(rail, "rail");
	slope_is_mixed(signal, "signal");
	slope_is_mixed(road, "road");
	slope_is_mixed(road, "tram");
	slope_is_mixed(bridge, "bridge");
	slope_is_mixed(tunnel, "tunnel");
	slope_is_mixed(house, "house");
	slope_is_mixed(mapobject, "mapobject");

	delete object;

	/* The slope is read through the engine's own accessor, which clamps at the map edge: a tile on
	 * the last row and the corner tile of the map are fingerprinted without reading outside it. */
	const TileIndex edge = TileXY(63, 30);
	MakeRoadNormal(edge, ROAD_X, ROADTYPE_ROAD, INVALID_ROADTYPE, TownID::Invalid(), Owner{0}, OWNER_NONE);
	CHECK(FindTileObject(edge, "road")["fingerprint"].is_number_unsigned());
	CHECK(FindTileObject(TileXY(63, 63), "terrain")["fingerprint"].is_number_unsigned());
}

TEST_CASE("Agent bridge refs: a road and a tram on one tile guard the whole cell")
{
	MapFixture map;
	/* Which road type is a tram type is decided by the masks the engine fills in at game start. */
	ResetRoadTypes();
	REQUIRE(GetRoadTramType(ROADTYPE_TRAM) == RoadTramType::Tram);

	const TileIndex tile = TileXY(20, 20);
	MakeRoadNormal(tile, ROAD_X, ROADTYPE_ROAD, INVALID_ROADTYPE, TownID::Invalid(), Owner{0}, OWNER_NONE);
	const Json road_row = FindTileObject(tile, "road");
	REQUIRE(!road_row.empty());
	const Json road_ref = TileRef(road_row);
	CHECK(ResolveObjectRef(road_ref, true)["found"] == true);

	/*
	 * A tram is built on the same tile. The road itself is untouched: the same subtype, the same
	 * road type, the same pieces, the same owner and the same one-way state. Only the cell the two
	 * families share changed, so only the fingerprint can say that the tile is not the one the
	 * caller read, and a whole-cell write (a demolition, a rebuild) would take the new tram with it.
	 */
	MakeRoadNormal(tile, ROAD_X, ROADTYPE_ROAD, ROADTYPE_TRAM, TownID::Invalid(), Owner{0}, Owner{1});
	const Json with_tram = FindTileObject(tile, "road");
	REQUIRE(!with_tram.empty());
	CHECK(with_tram["subtype"] == road_row["subtype"]);
	CHECK(with_tram["owner"] == road_row["owner"]);
	CHECK(with_tram["road_bits"] == road_row["road_bits"]);
	CHECK(with_tram["road_type_id"] == road_row["road_type_id"]);
	CHECK(with_tram["one_way"] == road_row["one_way"]);
	CHECK(with_tram["fingerprint"] != road_row["fingerprint"]);

	/* So the guard the caller kept for the road is stale before the demolition can run. */
	const Json stale = ResolveObjectRef(road_ref, true);
	CHECK(stale["found"] == false);
	CHECK(stale["error"] == "stale_ref");
	CHECK(stale["reason"] == "tile_state_changed");

	/* The tram row reports the owner of its own family, and its guard covers the road family too. */
	const Json tram_row = FindTileObject(tile, "tram");
	REQUIRE(!tram_row.empty());
	CHECK(tram_row["owner"] == 1);
	const Json tram_ref = TileRef(tram_row);
	CHECK(ResolveObjectRef(tram_ref, true)["found"] == true);
	SetRoadOwner(tile, RoadTramType::Road, Owner{2});
	const Json opposite_owner = ResolveObjectRef(tram_ref, true);
	CHECK(opposite_owner["found"] == false);
	CHECK(opposite_owner["reason"] == "tile_state_changed");

	/* The other direction as well: the tram owner is part of the road guard's cell. */
	const Json road_after = TileRef(FindTileObject(tile, "road"));
	CHECK(ResolveObjectRef(road_after, true)["found"] == true);
	SetRoadOwner(tile, RoadTramType::Tram, Owner{3});
	CHECK(ResolveObjectRef(road_after, true)["found"] == false);

	/* A one-way state is stored in the bits of a normal road, so it is part of the cell: changing it
	 * leaves the road type, the pieces and the owners alone. Both guards have to see it. */
	const Json one_way_ref = TileRef(FindTileObject(tile, "road"));
	CHECK(ResolveObjectRef(one_way_ref, true)["found"] == true);
	SetDisallowedRoadDirections(tile, DisallowedRoadDirections{DisallowedRoadDirection::Northbound});
	REQUIRE(IsOneWayRoadTile(tile));
	const Json one_way = ResolveObjectRef(one_way_ref, true);
	CHECK(one_way["found"] == false);
	CHECK(one_way["reason"] == "tile_state_changed");
	SetDisallowedRoadDirections(tile, DisallowedRoadDirections{});
	const Json tram_before = TileRef(FindTileObject(tile, "tram"));
	SetDisallowedRoadDirections(tile, DisallowedRoadDirections{DisallowedRoadDirection::Southbound});
	CHECK(ResolveObjectRef(tram_before, true)["found"] == false);

	/*
	 * A level crossing carries rail in the same cell, so another rail type is another cell: a
	 * demolition of the crossing removes the rail as well. The one-way accessor is only read on a
	 * normal road; a crossing and a depot store other values in those bits, which the depot case
	 * above covers.
	 */
	const TileIndex crossing = TileXY(21, 21);
	MakeRoadCrossing(crossing, Owner{0}, OWNER_NONE, Owner{0}, Axis::X, RAILTYPE_RAIL, ROADTYPE_ROAD, INVALID_ROADTYPE, TownID::Invalid());
	const Json crossing_row = FindTileObject(crossing, "road");
	REQUIRE(!crossing_row.empty());
	const Json crossing_ref = TileRef(crossing_row);
	CHECK(ResolveObjectRef(crossing_ref, true)["found"] == true);
	SetRailType(crossing, RAILTYPE_ELECTRIC);
	const Json rail_type_changed = ResolveObjectRef(crossing_ref, true);
	CHECK(rail_type_changed["found"] == false);
	CHECK(rail_type_changed["reason"] == "tile_state_changed");
}

TEST_CASE("Agent bridge refs: another tree on the tile is a new state")
{
	MapFixture map;

	const TileIndex tile = TileXY(30, 30);
	MakeTree(tile, TreeType{0}, 0, TreeGrowthStage::Grown, TreeGround::Grass, 3);
	REQUIRE(GetTreeCount(tile) == 1);
	const Json row = FindTileObject(tile, "tree");
	REQUIRE(!row.empty());
	const Json ref = TileRef(row);
	CHECK(ResolveObjectRef(ref, true)["found"] == true);

	/*
	 * The engine's own planting path for a tile that already carries trees: the type, the ground and
	 * the density stay and only the count moves (tree_cmd.cpp: PlaceTreeGroupAroundTile). The
	 * density is not a count, so the row alone cannot tell the two states apart.
	 */
	AddTreeCount(tile, 1);
	REQUIRE(GetTreeCount(tile) == 2);
	CHECK(FindTileObject(tile, "tree")["subtype"] == row["subtype"]);
	const Json changed = ResolveObjectRef(ref, true);
	CHECK(changed["found"] == false);
	CHECK(changed["error"] == "stale_ref");
	CHECK(changed["reason"] == "tile_state_changed");

	/* The growth stage belongs to the growth counter and advances without anyone touching the tile,
	 * so it is not part of the observed state: a tree that only grew keeps the reference current. */
	const Json grown_ref = TileRef(FindTileObject(tile, "tree"));
	SetTreeGrowth(tile, TreeGrowthStage::Growing1);
	REQUIRE(GetTreeGrowth(tile) != TreeGrowthStage::Grown);
	CHECK(ResolveObjectRef(grown_ref, true)["found"] == true);
}

TEST_CASE("Agent bridge refs: an unchanged tile keeps its fingerprint")
{
	MapFixture map;

	/* The fingerprint is the state of the tile and not the place of it, so two tiles in the same
	 * state carry the same value and reading one of them twice reports it unchanged. */
	MakeRoadNormal(TileXY(40, 40), ROAD_X, ROADTYPE_ROAD, INVALID_ROADTYPE, TownID::Invalid(), Owner{0}, OWNER_NONE);
	MakeRoadNormal(TileXY(40, 41), ROAD_X, ROADTYPE_ROAD, INVALID_ROADTYPE, TownID::Invalid(), Owner{0}, OWNER_NONE);
	const Json first = FindTileObject(TileXY(40, 40), "road");
	const Json second = FindTileObject(TileXY(40, 41), "road");
	REQUIRE(!first.empty());
	REQUIRE(!second.empty());
	CHECK(first["fingerprint"] == second["fingerprint"]);
	CHECK(FindTileObject(TileXY(40, 40), "road")["fingerprint"] == first["fingerprint"]);

	/* A vehicle standing on the tile is a pooled object of its own and not part of the tile state, so
	 * it does not move the fingerprint: the vehicle's own reference carries its lifetime instead. */
	REQUIRE(Vehicle::GetNumItems() == 0);
	REQUIRE(Vehicle::CanAllocateItem());
	RoadVehicle *vehicle = RoadVehicle::Create();
	REQUIRE(vehicle != nullptr);
	vehicle->tile = TileXY(40, 40);
	CHECK(FindTileObject(TileXY(40, 40), "road")["fingerprint"] == first["fingerprint"]);
	bool listed = false;
	for (const Json &candidate : PickObjects(TileXY(40, 40))) {
		if (candidate["type"] == "vehicle") listed = true;
	}
	CHECK(listed);
	_vehicle_pool.CleanPool();
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

/**
 * The exact engine key of a setting, or the canonical name the engine resolved it to.
 * @param name The key the reference named.
 * @return The canonical name, or an empty string when this build has no such setting.
 */
static std::string CanonicalSettingName(const std::string &name)
{
	const SettingDesc *sd = GetSettingFromName(name);
	return sd != nullptr ? sd->GetName() : std::string();
}

/**
 * One real company in the company pool, removed again when the fixture goes out of scope.
 *
 * A company setting is read from the settings of a company, so the test needs a company that
 * really exists in the pool: the id and the lifetime generation are the ones the engine hands out,
 * which is what a company scoped reference has to name.
 */
class TestCompany {
public:
	explicit TestCompany(CompanyID id)
	{
		REQUIRE_FALSE(Company::IsValidID(id));
		this->company = Company::CreateAtIndex(id);
		REQUIRE(this->company != nullptr);
	}

	~TestCompany() { delete this->company; }

	/** The lifetime generation of the company, as a reference names it. */
	int64_t Generation() const { return static_cast<int64_t>(this->company->generation); }

	Company *company = nullptr; ///< The company the fixture created.
};

/**
 * Build a company scoped reference to a company setting.
 * @param name The exact engine key.
 * @param company The owner.
 * @param generation The lifetime generation of the owner.
 * @return The reference.
 */
static Json CompanySettingRef(const std::string &name, CompanyID company, int64_t generation)
{
	return Json{{"type", "setting"}, {"setting_name", name}, {"scope", "company"},
			{"company_id", company.base()}, {"company_generation", generation}};
}

TEST_CASE("Agent bridge refs: a setting is identified by its exact engine key and scope")
{
	/* A setting is not a map object, so no map is needed; the game settings object is the one a
	 * running game reads, so the mode is set to a running game for the duration. */
	const GameMode previous_mode = _game_mode;
	_game_mode = GameMode::Normal;

	REQUIRE(CanonicalSettingName("economy.town_growth_rate") == "economy.town_growth_rate");
	const uint8_t growth_before = _settings_game.economy.town_growth_rate;

	const Json ref = {{"type", "setting"}, {"setting_name", "economy.town_growth_rate"}, {"scope", "game"}};
	const Json resolved = ResolveObjectRef(ref, false);
	REQUIRE(resolved["found"] == true);
	CHECK(resolved["type"] == "setting");
	CHECK(resolved["identity"] == "setting");
	CHECK(resolved["setting_name"] == "economy.town_growth_rate");
	CHECK(resolved["scope"] == "game");
	/* A game setting has no owner, no id, no generation, no tile and no fingerprint. */
	CHECK_FALSE(resolved.contains("company_id"));
	CHECK_FALSE(resolved.contains("company_generation"));
	CHECK_FALSE(resolved.contains("id"));
	CHECK_FALSE(resolved.contains("generation"));
	CHECK_FALSE(resolved.contains("tile"));
	CHECK_FALSE(resolved.contains("fingerprint"));
	/* The value is the engine's own typed value of that setting, not a formatted or guessed one. */
	REQUIRE(resolved["value"].is_number_integer());
	CHECK(resolved["value"].get<int32_t>() == static_cast<int32_t>(_settings_game.economy.town_growth_rate));
	REQUIRE(resolved["value_text"].is_string());
	CHECK_FALSE(resolved["value_text"].get<std::string>().empty());

	/* The value is not part of the identity: changing it keeps the reference current, and a write
	 * that names the key stays valid. */
	_settings_game.economy.town_growth_rate = growth_before == 4 ? 3 : static_cast<uint8_t>(growth_before + 1);
	const Json after_value_change = ResolveObjectRef(ref, true);
	REQUIRE(after_value_change["found"] == true);
	CHECK(after_value_change["value"].get<int32_t>() == static_cast<int32_t>(_settings_game.economy.town_growth_rate));
	CHECK(after_value_change["value"] != resolved["value"]);
	_settings_game.economy.town_growth_rate = growth_before;

	/* A boolean setting has a boolean value, and a client setting names its own scope. */
	const Json boolean = ResolveObjectRef(Json{{"type", "setting"}, {"setting_name", "economy.inflation"}, {"scope", "game"}}, false);
	REQUIRE(boolean["found"] == true);
	CHECK(boolean["scope"] == "game");
	REQUIRE(boolean["value"].is_boolean());
	CHECK(boolean["value"].get<bool>() == _settings_game.economy.inflation);

	const Json client = ResolveObjectRef(Json{{"type", "setting"}, {"setting_name", "gui.auto_remove_signals"}, {"scope", "client"}}, false);
	REQUIRE(client["found"] == true);
	CHECK(client["scope"] == "client");
	REQUIRE(client["value"].is_boolean());
	CHECK(client["value"].get<bool>() == _settings_client.gui.auto_remove_signals);
	CHECK_FALSE(client.contains("company_id"));

	_game_mode = previous_mode;
}

TEST_CASE("Agent bridge refs: a company setting is owned by one company lifetime")
{
	/* The key of a per-company setting is the bare key of the company settings table. */
	REQUIRE(CanonicalSettingName("engine_renew") == "engine_renew");

	const CompanyID owner_id(0);
	TestCompany owner(owner_id);
	const int64_t generation = owner.Generation();
	const bool renew_before = owner.company->settings.engine_renew;

	const Json ref = CompanySettingRef("engine_renew", owner_id, generation);
	const Json resolved = ResolveObjectRef(ref, false);
	REQUIRE(resolved["found"] == true);
	CHECK(resolved["setting_name"] == "engine_renew");
	CHECK(resolved["scope"] == "company");
	CHECK(resolved["company_id"] == owner_id.base());
	CHECK(resolved["company_generation"] == generation);
	/* The value is the value of that company, read through the descriptor of the setting. */
	REQUIRE(resolved["value"].is_boolean());
	CHECK(resolved["value"].get<bool>() == renew_before);

	/* Another company's value is never substituted, and the value is not part of the identity:
	 * changing it keeps the same owner reference current. */
	owner.company->settings.engine_renew = !renew_before;
	const Json after_value_change = ResolveObjectRef(ref, true);
	REQUIRE(after_value_change["found"] == true);
	CHECK(after_value_change["value"].get<bool>() == !renew_before);
	CHECK(after_value_change["company_generation"] == generation);
	owner.company->settings.engine_renew = renew_before;

	/* A company scoped reference has to name its owner; the current actor is never used instead. */
	const Json no_id = ResolveObjectRef(Json{{"type", "setting"}, {"setting_name", "engine_renew"}, {"scope", "company"}, {"company_generation", generation}}, false);
	CHECK(no_id["found"] == false);
	CHECK(no_id["error"] == "invalid_params");
	CHECK(no_id["reason"] == "missing_company_id");

	const Json no_generation = ResolveObjectRef(Json{{"type", "setting"}, {"setting_name", "engine_renew"}, {"scope", "company"}, {"company_id", owner_id.base()}}, false);
	CHECK(no_generation["found"] == false);
	CHECK(no_generation["error"] == "invalid_params");
	CHECK(no_generation["reason"] == "missing_company_generation");

	/* A company that is gone fails closed, and so does the id that was handed to another company. */
	const Json deleted = ResolveObjectRef(CompanySettingRef("engine_renew", CompanyID(1), generation), false);
	CHECK(deleted["found"] == false);
	CHECK(deleted["error"] == "stale_ref");
	CHECK(deleted["reason"] == "company_deleted");

	const Json reused = ResolveObjectRef(CompanySettingRef("engine_renew", owner_id, generation + 1), false);
	CHECK(reused["found"] == false);
	CHECK(reused["error"] == "stale_ref");
	CHECK(reused["reason"] == "company_lifetime_mismatch");
	CHECK(reused["company_generation_now"] == generation);

	/* A recreated company holds the same id but is another company. */
	delete owner.company;
	owner.company = Company::CreateAtIndex(owner_id);
	REQUIRE(owner.company != nullptr);
	REQUIRE(owner.Generation() != generation);
	const Json after_reuse = ResolveObjectRef(ref, false);
	CHECK(after_reuse["found"] == false);
	CHECK(after_reuse["error"] == "stale_ref");
	CHECK(after_reuse["reason"] == "company_lifetime_mismatch");
	CHECK(after_reuse["company_generation_now"] == owner.Generation());
}

TEST_CASE("Agent bridge refs: the scope of a setting reference must be its own")
{
	const CompanyID owner_id(0);
	TestCompany owner(owner_id);

	/* A game setting is not owned by a company, and a company setting is not a game setting. */
	const Json game_as_company = ResolveObjectRef(Json{{"type", "setting"}, {"setting_name", "economy.town_growth_rate"}, {"scope", "company"},
			{"company_id", owner_id.base()}, {"company_generation", owner.Generation()}}, false);
	CHECK(game_as_company["found"] == false);
	CHECK(game_as_company["error"] == "stale_ref");
	CHECK(game_as_company["reason"] == "scope_mismatch");
	CHECK(game_as_company["scope"] == "game");

	const Json company_as_game = ResolveObjectRef(Json{{"type", "setting"}, {"setting_name", "engine_renew"}, {"scope", "game"}}, false);
	CHECK(company_as_game["found"] == false);
	CHECK(company_as_game["reason"] == "scope_mismatch");
	CHECK(company_as_game["scope"] == "company");

	const Json client_as_game = ResolveObjectRef(Json{{"type", "setting"}, {"setting_name", "gui.auto_remove_signals"}, {"scope", "game"}}, false);
	CHECK(client_as_game["found"] == false);
	CHECK(client_as_game["reason"] == "scope_mismatch");
	CHECK(client_as_game["scope"] == "client");

	/* The same key with the scope of its own descriptor resolves, so the rejection above is the
	 * scope and not the key. */
	const Json game_as_game = ResolveObjectRef(Json{{"type", "setting"}, {"setting_name", "economy.town_growth_rate"}, {"scope", "game"}}, false);
	CHECK(game_as_game["found"] == true);
	const Json company_as_company = ResolveObjectRef(CompanySettingRef("engine_renew", owner_id, owner.Generation()), false);
	CHECK(company_as_company["found"] == true);
}

TEST_CASE("Agent bridge refs: a setting reference must name its scope")
{
	const CompanyID owner_id(0);
	TestCompany owner(owner_id);

	/* The scope is part of the identity of every setting, game and client settings included, and it
	 * is never inferred from the key: a reference without it is incomplete, not a shortcut. */
	const Json game = ResolveObjectRef(Json{{"type", "setting"}, {"setting_name", "economy.town_growth_rate"}}, false);
	CHECK(game["found"] == false);
	CHECK(game["error"] == "invalid_params");
	CHECK(game["reason"] == "missing_scope");
	CHECK(game["setting_name"] == "economy.town_growth_rate");
	/* The answer says which scope the key really has, so the caller can complete its reference. */
	CHECK(game["scope"] == "game");

	const Json client = ResolveObjectRef(Json{{"type", "setting"}, {"setting_name", "gui.auto_remove_signals"}}, false);
	CHECK(client["found"] == false);
	CHECK(client["error"] == "invalid_params");
	CHECK(client["reason"] == "missing_scope");
	CHECK(client["scope"] == "client");

	/* A company setting without a scope fails the same way, even when the owner is named. */
	const Json company = ResolveObjectRef(Json{{"type", "setting"}, {"setting_name", "engine_renew"},
			{"company_id", owner_id.base()}, {"company_generation", owner.Generation()}}, false);
	CHECK(company["found"] == false);
	CHECK(company["error"] == "invalid_params");
	CHECK(company["reason"] == "missing_scope");
	CHECK(company["scope"] == "company");

	/* An empty or a non-string scope names no scope either. */
	const Json empty_scope = ResolveObjectRef(Json{{"type", "setting"}, {"setting_name", "economy.town_growth_rate"}, {"scope", ""}}, false);
	CHECK(empty_scope["found"] == false);
	CHECK(empty_scope["reason"] == "missing_scope");

	const Json scope_not_a_string = ResolveObjectRef(Json{{"type", "setting"}, {"setting_name", "economy.town_growth_rate"}, {"scope", 3}}, false);
	CHECK(scope_not_a_string["found"] == false);
	CHECK(scope_not_a_string["error"] == "invalid_params");
	CHECK(scope_not_a_string["reason"] == "missing_scope");

	/* Naming the scope the descriptor has resolves the same key, so the scope is the only
	 * difference between the failing and the resolving reference. */
	const Json with_scope = ResolveObjectRef(Json{{"type", "setting"}, {"setting_name", "economy.town_growth_rate"}, {"scope", "game"}}, false);
	REQUIRE(with_scope["found"] == true);
	CHECK(with_scope["scope"] == "game");
}

TEST_CASE("Agent bridge refs: a setting key this build does not have is rejected")
{
	/* An unknown key is not a stale reference: there is no such setting to be current or not. The
	 * references here leave the scope out on purpose - the key is resolved first, so a key error is
	 * reported as such and not as a missing scope. */
	const Json unknown = ResolveObjectRef(Json{{"type", "setting"}, {"setting_name", "economy.no_such_setting"}}, false);
	CHECK(unknown["found"] == false);
	CHECK(unknown["error"] == "unknown_setting");
	CHECK(unknown["reason"] == "setting_not_found");
	CHECK(unknown["setting_name"] == "economy.no_such_setting");
	CHECK_FALSE(unknown.contains("canonical_name"));

	/* A shortcut or an alias the engine would also accept is not the exact key the caller named,
	 * so it is reported together with the canonical name instead of resolving to another setting. */
	const Json alias = ResolveObjectRef(Json{{"type", "setting"}, {"setting_name", "company.engine_renew"}}, false);
	CHECK(alias["found"] == false);
	CHECK(alias["error"] == "unknown_setting");
	CHECK(alias["canonical_name"] == "engine_renew");

	const Json shortcut = ResolveObjectRef(Json{{"type", "setting"}, {"setting_name", "town_growth_rate"}}, false);
	CHECK(shortcut["found"] == false);
	CHECK(shortcut["canonical_name"] == "economy.town_growth_rate");

	/* The private and the secrets configuration tables are not public settings: the engine knows
	 * those keys, a setting reference does not resolve them, so the identity and the credentials of
	 * the host and of the service are not reachable through the protocol. */
	REQUIRE(CanonicalSettingName("network.client_name") == "network.client_name");
	const Json private_key = ResolveObjectRef(Json{{"type", "setting"}, {"setting_name", "network.client_name"}}, false);
	CHECK(private_key["found"] == false);
	CHECK(private_key["error"] == "unknown_setting");
	CHECK(private_key["reason"] == "setting_not_found");
	CHECK_FALSE(private_key.contains("canonical_name"));

	REQUIRE(CanonicalSettingName("network.server_password") == "network.server_password");
	const Json secret_key = ResolveObjectRef(Json{{"type", "setting"}, {"setting_name", "network.server_password"}}, false);
	CHECK(secret_key["found"] == false);
	CHECK(secret_key["error"] == "unknown_setting");
	CHECK(secret_key["reason"] == "setting_not_found");
	CHECK_FALSE(secret_key.contains("canonical_name"));

	/* A public client setting still resolves, so the exclusion is the table and not the scope. */
	const Json public_client = ResolveObjectRef(Json{{"type", "setting"}, {"setting_name", "gui.auto_remove_signals"}, {"scope", "client"}}, false);
	CHECK(public_client["found"] == true);

	/* A reference without a key is an invalid argument, not an unknown setting. */
	const Json missing = ResolveObjectRef(Json{{"type", "setting"}}, false);
	CHECK(missing["found"] == false);
	CHECK(missing["error"] == "invalid_params");
	CHECK(missing["reason"] == "missing_setting_name");

	const Json not_a_string = ResolveObjectRef(Json{{"type", "setting"}, {"setting_name", 12}}, false);
	CHECK(not_a_string["found"] == false);
	CHECK(not_a_string["error"] == "invalid_params");
	CHECK(not_a_string["reason"] == "missing_setting_name");
}

TEST_CASE("Agent bridge refs: a setting reference belongs to one world and epoch")
{
	const GameMode previous_mode = _game_mode;
	_game_mode = GameMode::Normal;

	const Json context = Context();
	const std::string world_id = context["world_id"].get<std::string>();
	const std::string epoch = context["epoch"].get<std::string>();
	REQUIRE_FALSE(world_id.empty());

	Json ref = {{"type", "setting"}, {"setting_name", "economy.town_growth_rate"}, {"scope", "game"}, {"world_id", world_id}};
	if (!epoch.empty()) ref["epoch"] = epoch;
	CHECK(ResolveObjectRef(ref, false)["found"] == true);

	/* A reference of another world or epoch can never resolve, whatever the key is. */
	const Json other_world = ResolveObjectRef(Json{{"type", "setting"}, {"setting_name", "economy.town_growth_rate"}, {"scope", "game"}, {"world_id", "world-of-another-savegame"}}, false);
	CHECK(other_world["found"] == false);
	CHECK(other_world["error"] == "stale_ref");
	CHECK(other_world["reason"] == "world_mismatch");

	const Json other_epoch = ResolveObjectRef(Json{{"type", "setting"}, {"setting_name", "economy.town_growth_rate"}, {"scope", "game"}, {"epoch", "epoch-of-another-load"}}, false);
	CHECK(other_epoch["found"] == false);
	CHECK(other_epoch["error"] == "stale_ref");
	CHECK(other_epoch["reason"] == "epoch_mismatch");

	/* A load starts a new epoch and a new world, so the reference of the world that was left is
	 * stale afterwards - the key still exists, the world it named does not. */
	ResetWorld();
	const Json stale = ResolveObjectRef(ref, false);
	CHECK(stale["found"] == false);
	CHECK(stale["error"] == "stale_ref");
	CHECK(stale["reason"] == "world_mismatch");

	_game_mode = previous_mode;
}

TEST_CASE("Agent bridge refs: a station structure tile is a generic tile object")
{
	MapFixture map;
	StructureTestCompany company;

	/* A real bus stop: the engine's own station pool item behind a bus stop tile. */
	const TileIndex tile = TileXY(60, 60);
	TestBusStop stop(tile, company.Id());
	REQUIRE(stop.Get() != nullptr);

	/* The generic tile spelling resolves the structure the tile stands on, which is the read the
	 * agent needs: the tile carries no typed landscape object of its own. */
	const Json row = ResolveObjectRef(Json{{"type", "tile"}, {"tile", TilePoint(60, 60)}}, false);
	REQUIRE(row["found"] == true);
	CHECK(row["type"] == "tile");
	CHECK(row["what"] == "station");
	CHECK(row["subtype"] == "bus_stop");
	CHECK(row["identity"] == "tile");
	CHECK(row["tile"] == TilePoint(60, 60));
	CHECK(row["owner"] == company.Id().base());
	CHECK(row["fingerprint"].is_number_unsigned());

	/* The structure is a tile object, not a pooled one: no pool id and no lifetime is claimed as
	 * the identity of the tile. */
	CHECK_FALSE(row.contains("id"));
	CHECK_FALSE(row.contains("generation"));

	/* The `what` the row reports roundtrips, and it is the state a write is guarded with. */
	const Json ref = ReadStructureRef(tile);
	CHECK(ref["what"] == "station");
	CHECK(ResolveObjectRef(ref, true)["found"] == true);

	/* Another structure kind on the same tile does not silently retarget: the answer names what
	 * really stands there. */
	const Json industry_what = ResolveObjectRef(Json{{"type", "tile"}, {"what", "industry"}, {"tile", TilePoint(60, 60)}}, false);
	CHECK(industry_what["found"] == false);
	CHECK(industry_what["reason"] == "tile_type_mismatch");
	REQUIRE(industry_what["what"].is_array());
	REQUIRE(industry_what["what"].size() == 1);
	CHECK(industry_what["what"][0] == "station");

	/* A typed landscape category is still not what a station tile carries, and it is not invented. */
	const Json typed_what = ResolveObjectRef(Json{{"type", "tile"}, {"what", "road"}, {"tile", TilePoint(60, 60)}}, false);
	CHECK(typed_what["found"] == false);
	CHECK(typed_what["reason"] == "tile_type_mismatch");

	/* A `what` that names neither a typed category nor a structure stays an invalid argument. */
	const Json unknown_what = ResolveObjectRef(Json{{"type", "tile"}, {"what", "planet"}, {"tile", TilePoint(60, 60)}}, false);
	CHECK(unknown_what["found"] == false);
	CHECK(unknown_what["error"] == "invalid_params");
	CHECK(unknown_what["reason"] == "unknown_what");

	/* The typed rows are unchanged: a station tile carries no typed landscape object. */
	CHECK(DescribeTileObjects(tile).empty());

	/*
	 * The void at the map edge and a tile off the map stay honest. The engine's own `IsValidTile`
	 * excludes `TileType::Void` (tile_map.h), so the border void is no tile at all: it is an invalid
	 * argument, not a structure that does not match.
	 */
	const TileIndex void_tile = TileXY(63, 63);
	SetTileType(void_tile, TileType::Void);
	const Json void_row = ResolveObjectRef(Json{{"type", "tile"}, {"tile", TilePoint(63, 63)}}, false);
	CHECK(void_row["found"] == false);
	CHECK(void_row["error"] == "invalid_params");
	CHECK(void_row["reason"] == "invalid_tile");
	CHECK(ResolveObjectRef(Json{{"type", "tile"}, {"tile", TilePoint(1000, 1000)}}, false)["reason"] == "invalid_tile");
}

TEST_CASE("Agent bridge refs: the read guard of a bus stop is current when the demolition runs")
{
	MapFixture map;
	/* The removal updates the station sign, which formats the engine's own station strings, and the
	 * sign measurement lays them out with the engine's own font caches. */
	LoadTestLanguagePack();
	TestFontCaches font_caches;
	REQUIRE(FontCache::Get(FontSize::Normal) != nullptr);
	StructureTestCompany company;

	const TileIndex tile = TileXY(61, 61);
	TestBusStop stop(tile, company.Id());
	REQUIRE(stop.Get() != nullptr);

	/*
	 * The read the task keeps, and the guard a write is planned with. `ResolveObjectRef(ref, true)`
	 * is the check the bridge's own `CheckExpectedRefs` runs per reference before it delegates a
	 * write (the wrapper adds the array and size checks and stops at the first stale reference), so
	 * this is the engine's verdict on the reference, taken immediately before the command.
	 */
	const Json ref = ReadStructureRef(tile);
	CHECK(ResolveObjectRef(ref, true)["found"] == true);

	/*
	 * `CmdLandscapeClear` is the engine's own supported path for a demolition: it is the command
	 * `world.construct op=demolish` posts, and on a road stop tile it removes the stop
	 * (`landscape.cpp: CmdLandscapeClear` -> `station_cmd.cpp: ClearTile_Station` ->
	 * `RemoveRoadStop`).
	 *
	 * The dispatch below is the catalog's real write path, the one `world.command` runs: it checks
	 * `expected_targets` before it posts (`guard: "checked"` in the answer). It does not check
	 * `expected_refs` - the bridge owns that guard and runs it before it delegates, which is the
	 * `ResolveObjectRef(ref, true)` check above. So this case proves the composition: a current read
	 * guard, then a real removal of the very tile the read named, then the same read being stale.
	 */
	const Json actor_guard = {{"entity", "company"}, {"id", company.Id().base()}};
	Json targets = Json::array();
	targets.push_back(actor_guard);
	Json args = Json::array();
	args.push_back(tile.base());
	const Json removed = CallCommandAs({
		{"command", "CmdLandscapeClear"},
		{"args", args},
		{"expected_targets", targets},
	}, company.Id());
	REQUIRE(removed["ok"].get<bool>());
	CHECK(removed["guard"] == "checked");
	CHECK_FALSE(IsTileType(tile, TileType::Station));
	CHECK(RoadStop::GetNumItems() == 0);

	/*
	 * The reference the removal was planned with is stale afterwards: the tile carries no structure,
	 * so a second removal of the stop the caller saw is rejected instead of clearing whatever is
	 * there now.
	 */
	const Json stale = ResolveObjectRef(ref, true);
	CHECK(stale["found"] == false);
	CHECK(stale["error"] == "stale_ref");
	CHECK(stale["reason"] == "tile_type_mismatch");
}

TEST_CASE("Agent bridge refs: a rebuilt station on the same tile is another structure lifetime")
{
	MapFixture map;
	StructureTestCompany company;

	const TileIndex tile = TileXY(62, 62);
	TestBusStop stop(tile, company.Id());
	REQUIRE(stop.Get() != nullptr);

	const Json ref = ReadStructureRef(tile);
	const StationID id = stop.Get()->index;
	const uint64_t generation = stop.Get()->generation;

	/* The stop is removed and built again on the same tile: the pool reuses the id, so the tile
	 * carries a structure the caller has never seen. */
	Station *rebuilt = stop.Rebuild(tile, company.Id());
	REQUIRE(rebuilt != nullptr);
	CHECK(rebuilt->index == id);
	REQUIRE(rebuilt->generation != generation);

	const Json changed = ResolveObjectRef(ref, true);
	CHECK(changed["found"] == false);
	CHECK(changed["error"] == "stale_ref");
	CHECK(changed["reason"] == "tile_state_changed");
	CHECK(changed["fingerprint_now"].is_number_unsigned());
	CHECK(changed["fingerprint_now"] != ref["fingerprint"]);

	/* A read of the old reference still resolves and reports the structure that stands there now. */
	const Json now = ResolveObjectRef(ref, false);
	CHECK(now["found"] == true);
	CHECK(now["fingerprint"] == changed["fingerprint_now"]);
}

TEST_CASE("Agent bridge refs: an industry structure tile is a generic tile object as well")
{
	MapFixture map;

	const TileIndex tile = TileXY(58, 58);
	TestIndustry industry(tile);
	REQUIRE(industry.Get() != nullptr);

	const Json row = ResolveObjectRef(Json{{"type", "tile"}, {"tile", TilePoint(58, 58)}}, false);
	REQUIRE(row["found"] == true);
	CHECK(row["type"] == "tile");
	CHECK(row["what"] == "industry");
	CHECK(row["subtype"] == "type_0");
	CHECK(row["identity"] == "tile");
	/* An industry tile stores no company owner. */
	CHECK(row["owner"].is_null());
	CHECK(row["fingerprint"].is_number_unsigned());

	const Json ref = ReadStructureRef(tile);
	CHECK(ResolveObjectRef(ref, true)["found"] == true);

	/* A station `what` on an industry tile is a mismatch, never a retarget. */
	const Json station_what = ResolveObjectRef(Json{{"type", "tile"}, {"what", "station"}, {"tile", TilePoint(58, 58)}}, false);
	CHECK(station_what["found"] == false);
	CHECK(station_what["reason"] == "tile_type_mismatch");
	REQUIRE(station_what["what"].size() == 1);
	CHECK(station_what["what"][0] == "industry");

	/* The lifetime of the industry is part of the tile identity: the same id and the same footprint
	 * with a new lifetime is another industry. */
	const IndustryID id = industry.Get()->index;
	const uint64_t generation = industry.Get()->generation;
	Industry *rebuilt = industry.Rebuild(tile);
	REQUIRE(rebuilt != nullptr);
	CHECK(rebuilt->index == id);
	REQUIRE(rebuilt->generation != generation);

	const Json changed = ResolveObjectRef(ref, true);
	CHECK(changed["found"] == false);
	CHECK(changed["reason"] == "tile_state_changed");
	CHECK(changed["fingerprint_now"] != ref["fingerprint"]);

	/* The tile itself is part of the identity as well: another industry gfx is another state. */
	const Json rebuilt_ref = ReadStructureRef(tile);
	CHECK(ResolveObjectRef(rebuilt_ref, true)["found"] == true);
	MakeIndustry(tile, rebuilt->index, IndustryGfx{1}, 0, WaterClass::Invalid);
	const Json other_gfx = ResolveObjectRef(rebuilt_ref, true);
	CHECK(other_gfx["found"] == false);
	CHECK(other_gfx["reason"] == "tile_state_changed");
}
