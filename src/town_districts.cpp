/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/**
 * @file town_districts.cpp Districts of large towns.
 *
 * Small towns build their houses by town zone only, so every big town used to look the same: a
 * dense round centre fading out to small houses. Real big cities are made of districts. From
 * #TOWN_DISTRICTS_MIN_POPULATION residents on, every tile of a town belongs to a district, and the
 * district decides which kind of buildings the town builds there:
 *  - the old town (#TownDistrict::Historic) around the town centre: low and mid-rise houses, shops and churches;
 *  - the business district (#TownDistrict::Business), a cluster of office towers next to the old town,
 *    which grows with the town;
 *  - sub-centres (#TownDistrict::SubCentre) of the outer parts of very big towns, on rings around the
 *    centre: shops, offices, cinemas, theatres, churches and stadiums;
 *  - parks (#TownDistrict::Park): whole cells of 4x4 blocks, and squares of a single block;
 *  - residential districts: tall blocks of flats and towers with shops (#TownDistrict::HighRise, house types of
 *    at least #TALL_HOUSE_POPULATION residents), small flats, houses and shops (#TownDistrict::MidRise, smaller types)
 *    and small houses (#TownDistrict::LowRise). They come in large patches:
 *    a sector of 45 degrees and 40 tiles of distance shares one kind, most cells of 4x4 blocks follow it.
 *
 * The district of a tile is a pure function of the tile, the town centre, the population and the
 * radius of the town, so it is never saved and every client computes the same. A district only
 * steers new houses and the replacement of old ones; existing buildings stay. A big town gets more
 * than one church (one per 200 000 residents) and stadium (one per 600 000 residents).
 */

#include "stdafx.h"
#include "town.h"
#include "town_districts.h"
#include "core/math_func.hpp"
#include "map_func.h"

#include "table/strings.h"

#include "safeguards.h"

static constexpr int HISTORIC_RADIUS = 9; ///< Radius of the old town in tiles.
static constexpr uint32_t BUSINESS_MIN_POPULATION = 60000; ///< Population from which the town has a business district.
static constexpr int BUSINESS_OFFSET = 16; ///< Distance between the town centre and the centre of the business district.
static constexpr int BUSINESS_BASE_RADIUS = 6; ///< Radius of the business district of a town with no radius.
static constexpr int BUSINESS_MAX_RADIUS = 22; ///< Largest radius of the business district.
static constexpr uint32_t SUBCENTRE_MIN_POPULATION = 200000; ///< Population from which the town has sub-centres.
static constexpr int SUBCENTRE_FIRST_RING = 44; ///< Distance of the first ring of sub-centres to the town centre.
static constexpr int SUBCENTRE_RING_STEP = 32; ///< Distance between two rings of sub-centres.
static constexpr int SUBCENTRE_RINGS = 6; ///< Most rings of sub-centres.
static constexpr int SUBCENTRE_RADIUS = 6; ///< Radius of a sub-centre.
static constexpr uint DISTRICT_CELL = 16; ///< Size in tiles of a district cell: 4x4 blocks.
static constexpr uint PARK_CELL_PERCENT = 6; ///< Share of the cells that are parks.
static constexpr int PARK_MIN_DISTANCE = HISTORIC_RADIUS + 10; ///< Parks begin this far from the town centre.
static constexpr uint SQUARE_BLOCK_ONE_IN = 22; ///< One in this many blocks is a square.
static constexpr int SECTOR_BAND = 40; ///< Depth in tiles of a band of residential sectors.
static constexpr uint SECTOR_FOLLOW_PERCENT = 65; ///< Share of the cells that follow the kind of their sector.
static constexpr uint32_t CHURCH_RESIDENTS = 200000; ///< A big town gets a church per this many residents.
static constexpr uint32_t STADIUM_RESIDENTS = 600000; ///< A big town gets a stadium per this many residents.
static constexpr uint DENSE_WEIGHT_BASE = 64; ///< Dense districts weigh a house by (population + this) / this.
static constexpr uint BLOCK_FAVOURITE_WEIGHT = 6; ///< Weight of the house types a block favours.
static constexpr uint8_t TALL_HOUSE_POPULATION = 70; ///< House types with this many residents are tall: high-rise, not mid-rise.
static constexpr uint TOWER_BLOCK_ONE_IN = 3; ///< One in this many blocks of a high-rise district has towers with shops instead of flats.
static constexpr uint HIGH_RISE_PERCENT = 32; ///< Share of the residential area of tall flats.
static constexpr uint MID_RISE_PERCENT = 36; ///< Share of the residential area of flats and houses; the rest is low-rise.

/**
 * Deterministic hash of a few numbers.
 * @param a First number.
 * @param b Second number.
 * @param salt What the hash is for.
 * @return The hash.
 */
static uint32_t DistrictHash(uint32_t a, uint32_t b, uint32_t salt)
{
	uint32_t h = a * 0x9E3779B1U ^ b * 0x85EBCA77U ^ (salt + 1) * 0xC2B2AE3DU;
	h ^= h >> 15;
	h *= 0x2C1B3C6DU;
	h ^= h >> 12;
	h *= 0x297A2D39U;
	h ^= h >> 15;
	return h;
}

/**
 * Cosine of one of 32 directions, without floating point (multiplayer).
 * @param dir Direction in steps of 11.25 degrees.
 * @return The cosine times 1000.
 */
static int DirCos(uint dir)
{
	static const std::array<int, 9> quarter = {1000, 981, 924, 831, 707, 556, 383, 195, 0};
	dir %= 32;
	if (dir <= 8) return quarter[dir];
	if (dir <= 16) return -quarter[16 - dir];
	if (dir <= 24) return -quarter[dir - 16];
	return quarter[32 - dir];
}

/**
 * Sine of one of 32 directions.
 * @param dir Direction in steps of 11.25 degrees.
 * @return The sine times 1000.
 */
static int DirSin(uint dir)
{
	return DirCos(dir + 24);
}

/**
 * Is a point within a circle?
 * @param dx X of the point relative to the town centre.
 * @param dy Y of the point relative to the town centre.
 * @param dir Direction of the circle centre from the town centre.
 * @param distance Distance of the circle centre from the town centre.
 * @param radius Radius of the circle.
 * @return True if the point lies in the circle.
 */
static bool IsInCircle(int dx, int dy, uint dir, int distance, int radius)
{
	int cx = DirCos(dir) * distance / 1000;
	int cy = DirSin(dir) * distance / 1000;
	return (dx - cx) * (dx - cx) + (dy - cy) * (dy - cy) <= radius * radius;
}

/**
 * Get the radius of a town: the distance between its centre and the centre of its farthest block.
 * @param t The town.
 * @return Radius in tiles.
 */
static int GetTownDistrictRadius(const Town *t)
{
	/* The edge zone reaches 4 tiles past the farthest block, see UpdateTownRadius. */
	return std::max<int>(IntSqrt(t->cache.squared_town_zone_radius[to_underlying(HouseZone::TownEdge)]) - 4, 0);
}

/**
 * Get the district of a tile of a town.
 * @param t The town.
 * @param tile The tile.
 * @return The district.
 */
TownDistrict GetTownDistrict(const Town *t, TileIndex tile)
{
	uint32_t population = t->cache.population;
	if (population < TOWN_DISTRICTS_MIN_POPULATION) return TownDistrict::Normal;

	int dx = static_cast<int>(TileX(tile)) - static_cast<int>(TileX(t->xy));
	int dy = static_cast<int>(TileY(tile)) - static_cast<int>(TileY(t->xy));
	int distance_sq = dx * dx + dy * dy;
	int distance = IntSqrt(distance_sq);
	int radius = GetTownDistrictRadius(t);
	uint32_t seed = t->index.base();

	if (distance_sq <= HISTORIC_RADIUS * HISTORIC_RADIUS) return TownDistrict::Historic;

	if (population >= BUSINESS_MIN_POPULATION) {
		int business_radius = std::min(BUSINESS_BASE_RADIUS + radius / 10, BUSINESS_MAX_RADIUS);
		if (IsInCircle(dx, dy, DistrictHash(seed, 0, 1) % 32, BUSINESS_OFFSET, business_radius)) return TownDistrict::Business;
	}

	if (population >= SUBCENTRE_MIN_POPULATION) {
		for (int ring = 0; ring < SUBCENTRE_RINGS; ring++) {
			int ring_distance = SUBCENTRE_FIRST_RING + ring * SUBCENTRE_RING_STEP;
			if (ring_distance + SUBCENTRE_RADIUS > radius) break;
			/* Only the ring the tile is near can hold its sub-centre. */
			if (std::abs(distance - ring_distance) > SUBCENTRE_RADIUS + 1) continue;
			uint count = 3 + 2 * ring;
			uint base = DistrictHash(seed, ring, 2) % 32;
			for (uint k = 0; k < count; k++) {
				uint dir = base + (k * 32 + count / 2) / count;
				if (IsInCircle(dx, dy, dir, ring_distance, SUBCENTRE_RADIUS)) return TownDistrict::SubCentre;
			}
		}
	}

	/* Cells of the global district grid, 4x4 blocks each. */
	uint cell_x = TileX(tile) / DISTRICT_CELL;
	uint cell_y = TileY(tile) / DISTRICT_CELL;
	uint32_t cell = cell_y << 16 | cell_x;
	int cell_dx = static_cast<int>(cell_x * DISTRICT_CELL + DISTRICT_CELL / 2) - static_cast<int>(TileX(t->xy));
	int cell_dy = static_cast<int>(cell_y * DISTRICT_CELL + DISTRICT_CELL / 2) - static_cast<int>(TileY(t->xy));
	if (cell_dx * cell_dx + cell_dy * cell_dy >= PARK_MIN_DISTANCE * PARK_MIN_DISTANCE && DistrictHash(cell, seed, 3) % 100 < PARK_CELL_PERCENT) {
		return TownDistrict::Park;
	}

	uint32_t block = (TileY(tile) / 4) << 16 | (TileX(tile) / 4);
	if (DistrictHash(block, seed, 4) % SQUARE_BLOCK_ONE_IN == 0) return TownDistrict::Park;

	/* Residential: a sector of 45 degrees and SECTOR_BAND tiles shares a kind, most cells follow it. */
	uint sector = 0;
	int best = INT_MIN;
	for (uint s = 0; s < 8; s++) {
		int dot = dx * DirCos(s * 4) + dy * DirSin(s * 4);
		if (dot > best) {
			best = dot;
			sector = s;
		}
	}
	uint32_t sector_hash = DistrictHash(sector + 8 * (distance / SECTOR_BAND), seed, 5);
	uint32_t cell_hash = DistrictHash(cell, seed, 6);
	uint32_t kind = (cell_hash % 100 < SECTOR_FOLLOW_PERCENT ? sector_hash : cell_hash) >> 8;

	uint weight = kind % 100;
	TownDistrict district = weight < HIGH_RISE_PERCENT ? TownDistrict::HighRise :
		(weight < HIGH_RISE_PERCENT + MID_RISE_PERCENT ? TownDistrict::MidRise : TownDistrict::LowRise);
	/* The inner city has no small houses. */
	if (district == TownDistrict::LowRise && distance * 10 < radius * 3) district = TownDistrict::MidRise;
	return district;
}

/**
 * Get the houses a town may build on a tile.
 * @param t The town.
 * @param tile The tile.
 * @param zone The town zone of the tile.
 * @param crowded The town has no more land to grow on: residential districts allow one step taller buildings.
 * @return The district of the tile and what it allows.
 */
TownHousePick GetTownHousePick(const Town *t, TileIndex tile, HouseZone zone, bool crowded)
{
	using enum HouseZone;
	using enum HouseCategory;
	TownHousePick pick{GetTownDistrict(t, tile), {}, {}, false, (TileY(tile) / 4) << 16 | (TileX(tile) / 4)};
	if (crowded) {
		if (pick.district == TownDistrict::MidRise) pick.district = TownDistrict::HighRise;
		if (pick.district == TownDistrict::LowRise) pick.district = TownDistrict::MidRise;
	}
	switch (pick.district) {
		case TownDistrict::Normal:
		case TownDistrict::End:
			pick.zones = zone;
			pick.categories = {Other, Houses, Flats, Office, Commercial, Civic, Sports, Park};
			break;

		case TownDistrict::Historic:
			pick.zones = {TownCentre, TownInnerSuburb, TownOuterSuburb};
			pick.categories = {Other, Houses, Flats, Commercial, Civic, Park};
			break;

		case TownDistrict::Business:
			pick.zones = {TownCentre, TownInnerSuburb};
			pick.categories = {Office, Commercial, Civic};
			pick.prefer_dense = true;
			break;

		case TownDistrict::SubCentre:
			pick.zones = {TownCentre, TownInnerSuburb};
			pick.categories = {Office, Commercial, Civic, Sports, Flats, Park};
			break;

		case TownDistrict::HighRise:
			pick.zones = {TownCentre, TownInnerSuburb, TownOuterSuburb};
			/* Blocks of tall flats; one in TOWER_BLOCK_ONE_IN is a block of towers with shops and hotels. */
			pick.categories = DistrictHash(pick.block, t->index.base(), 8) % TOWER_BLOCK_ONE_IN == 0 ? HouseCategories{Commercial} : HouseCategories{Flats};
			pick.min_population = TALL_HOUSE_POPULATION;
			break;

		case TownDistrict::MidRise:
			pick.zones = {TownInnerSuburb, TownOuterSuburb};
			pick.zones.Set(zone);
			pick.categories = {Other, Houses, Flats, Commercial};
			pick.max_population = TALL_HOUSE_POPULATION - 1;
			break;

		case TownDistrict::LowRise:
			pick.zones = {TownOuterSuburb, TownOutskirt, TownEdge};
			pick.categories = {Other, Houses};
			break;

		case TownDistrict::Park:
			pick.zones = {TownCentre, TownInnerSuburb, TownOuterSuburb, TownOutskirt, TownEdge};
			pick.categories = {Park, Sports};
			break;
	}
	return pick;
}

/**
 * May a house type be built here? Its category and one of its town zones must fit; the climate is checked by the caller.
 * @param hs The house type.
 * @return True if the house fits.
 */
bool TownHousePick::Allows(const HouseSpec &hs) const
{
	if (hs.population < this->min_population || hs.population > this->max_population) return false;
	return hs.building_availability.Any(this->zones) && this->categories.Test(GetHouseCategory(hs));
}

/**
 * Get the probability of a house type among the candidates of a pick. In a district every block favours about
 * a quarter of the house types, so a block looks like one development and its neighbours look different;
 * where the district wants dense buildings, denser houses weigh somewhat more.
 * @param hs The house type.
 * @return The probability.
 */
uint TownHousePick::GetProbability(const HouseSpec &hs) const
{
	uint probability = hs.probability;
	if (this->district == TownDistrict::Normal) return probability;
	if (this->prefer_dense) probability = probability * (hs.population + DENSE_WEIGHT_BASE) / DENSE_WEIGHT_BASE;
	if (DistrictHash(this->block, hs.Index(), 7) % 4 == 0) probability *= BLOCK_FAVOURITE_WEIGHT;
	return probability;
}

static HouseCategory FindHouseCategory(const HouseSpec &hs);

/**
 * Get the category of a house type from its name; NewGRF houses take the one of the original house they replace.
 * The category is cached per house type and looked up again when the name of the type changes (e.g. other NewGRFs).
 * @param hs The house type.
 * @return The category.
 */
HouseCategory GetHouseCategory(const HouseSpec &hs)
{
	/** A cached category with the name it was found for. */
	struct CachedCategory {
		StringID name = INVALID_STRING_ID; ///< Name of the house type.
		HouseID subst = INVALID_HOUSE_ID; ///< Original house the type replaces.
		HouseCategory category = HouseCategory::Other; ///< The category.
	};
	/* Only the game loop picks houses (the console reads as well, also from the game thread), so a plain cache is enough. */
	static std::vector<CachedCategory> cache;
	size_t index = hs.Index();
	if (index >= cache.size()) cache.resize(index + 1);
	CachedCategory &c = cache[index];
	if (c.name != hs.building_name || c.subst != hs.grf_prop.subst_id) {
		c.name = hs.building_name;
		c.subst = hs.grf_prop.subst_id;
		c.category = FindHouseCategory(hs);
	}
	return c.category;
}

/**
 * Find the category of a house type from its name.
 * @param hs The house type.
 * @return The category.
 */
static HouseCategory FindHouseCategory(const HouseSpec &hs)
{
	const HouseSpec *orig = &hs;
	if (hs.Index() >= NEW_HOUSE_OFFSET && hs.grf_prop.subst_id < NEW_HOUSE_OFFSET) orig = HouseSpec::Get(hs.grf_prop.subst_id);

	if (orig->building_flags.Test(BuildingFlag::IsStadium)) return HouseCategory::Sports;
	if (orig->building_flags.Test(BuildingFlag::IsChurch)) return HouseCategory::Civic;

	static const std::array<std::pair<StringID, HouseCategory>, 32> names = {{
		{STR_TOWN_BUILDING_NAME_TALL_OFFICE_BLOCK_1, HouseCategory::Office},
		{STR_TOWN_BUILDING_NAME_TALL_OFFICE_BLOCK_2, HouseCategory::Office},
		{STR_TOWN_BUILDING_NAME_OFFICE_BLOCK_1, HouseCategory::Office},
		{STR_TOWN_BUILDING_NAME_OFFICE_BLOCK_2, HouseCategory::Office},
		{STR_TOWN_BUILDING_NAME_OFFICE_BLOCK_3, HouseCategory::Office},
		{STR_TOWN_BUILDING_NAME_LARGE_OFFICE_BLOCK_1, HouseCategory::Office},
		{STR_TOWN_BUILDING_NAME_MODERN_OFFICE_BUILDING_1, HouseCategory::Office},
		{STR_TOWN_BUILDING_NAME_OFFICES_1, HouseCategory::Office},
		{STR_TOWN_BUILDING_NAME_SHOPS_AND_OFFICES_1, HouseCategory::Commercial},
		{STR_TOWN_BUILDING_NAME_SHOPS_AND_OFFICES_2, HouseCategory::Commercial},
		{STR_TOWN_BUILDING_NAME_SHOPS_AND_OFFICES_3, HouseCategory::Commercial},
		{STR_TOWN_BUILDING_NAME_HOTEL_1, HouseCategory::Commercial},
		{STR_TOWN_BUILDING_NAME_SHOPPING_MALL_1, HouseCategory::Commercial},
		{STR_TOWN_BUILDING_NAME_WAREHOUSE_1, HouseCategory::Commercial},
		{STR_TOWN_BUILDING_NAME_PIGGY_BANK_1, HouseCategory::Commercial},
		{STR_TOWN_BUILDING_NAME_THEATER_1, HouseCategory::Civic},
		{STR_TOWN_BUILDING_NAME_CINEMA_1, HouseCategory::Civic},
		{STR_TOWN_BUILDING_NAME_CHURCH_1, HouseCategory::Civic},
		{STR_TOWN_BUILDING_NAME_STADIUM_1, HouseCategory::Sports},
		{STR_TOWN_BUILDING_NAME_STADIUM_2, HouseCategory::Sports},
		{STR_TOWN_BUILDING_NAME_PARK_1, HouseCategory::Park},
		{STR_TOWN_BUILDING_NAME_FOUNTAIN_1, HouseCategory::Park},
		{STR_TOWN_BUILDING_NAME_STATUE_1, HouseCategory::Park},
		{STR_TOWN_BUILDING_NAME_FLATS_1, HouseCategory::Flats},
		{STR_TOWN_BUILDING_NAME_SMALL_BLOCK_OF_FLATS_1, HouseCategory::Flats},
		{STR_TOWN_BUILDING_NAME_HOUSES_1, HouseCategory::Houses},
		{STR_TOWN_BUILDING_NAME_HOUSES_2, HouseCategory::Houses},
		{STR_TOWN_BUILDING_NAME_OLD_HOUSES_1, HouseCategory::Houses},
		{STR_TOWN_BUILDING_NAME_COTTAGES_1, HouseCategory::Houses},
		{STR_TOWN_BUILDING_NAME_TOWN_HOUSES_1, HouseCategory::Houses},
		{STR_TOWN_BUILDING_NAME_IGLOO_1, HouseCategory::Houses},
		{STR_TOWN_BUILDING_NAME_TEPEES_1, HouseCategory::Houses},
	}};
	for (const auto &[name, category] : names) {
		if (orig->building_name == name) return category;
	}
	return HouseCategory::Other;
}

/**
 * Get the town zone that decides the look of a road side: lamps in the centres, trees in parks, grass among small houses.
 * @param t The town.
 * @param tile The road tile.
 * @param zone The town zone of the tile.
 * @return The zone to use for the road side.
 */
HouseZone GetTownRoadsideZone(const Town *t, TileIndex tile, HouseZone zone)
{
	switch (GetTownDistrict(t, tile)) {
		case TownDistrict::Historic:
		case TownDistrict::Business:
		case TownDistrict::SubCentre:
			return HouseZone::TownCentre;

		case TownDistrict::Park:
			return HouseZone::TownInnerSuburb;

		case TownDistrict::HighRise:
			return std::max(zone, HouseZone::TownOuterSuburb);

		case TownDistrict::LowRise:
			return std::min(zone, HouseZone::TownOutskirt);

		default:
			return zone;
	}
}

/**
 * Get how many churches or stadiums a town may have.
 * @param t The town.
 * @param stadium Stadiums instead of churches.
 * @return The limit; small towns have one of each.
 */
uint GetTownLandmarkLimit(const Town *t, bool stadium)
{
	uint32_t population = t->cache.population;
	if (population < TOWN_DISTRICTS_MIN_POPULATION) return 1;
	return 1 + population / (stadium ? STADIUM_RESIDENTS : CHURCH_RESIDENTS);
}

/**
 * Get the name of a district.
 * @param district The district.
 * @return The name.
 */
std::string_view GetTownDistrictName(TownDistrict district)
{
	switch (district) {
		case TownDistrict::Normal: return "normal";
		case TownDistrict::Historic: return "old town";
		case TownDistrict::Business: return "business";
		case TownDistrict::SubCentre: return "sub-centre";
		case TownDistrict::HighRise: return "high-rise";
		case TownDistrict::MidRise: return "mid-rise";
		case TownDistrict::LowRise: return "low-rise";
		case TownDistrict::Park: return "park";
		default: return "?";
	}
}

/**
 * Get the letter of a district on the console map.
 * @param district The district.
 * @return The letter.
 */
char GetTownDistrictLetter(TownDistrict district)
{
	static const std::array<char, to_underlying(TownDistrict::End)> letters = {'.', 'O', 'B', 'S', 'H', 'M', 'L', 'P'};
	return district < TownDistrict::End ? letters[to_underlying(district)] : '?';
}
