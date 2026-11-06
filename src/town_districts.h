/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file town_districts.h Districts of large towns: which kind of buildings a town puts where. */

#ifndef TOWN_DISTRICTS_H
#define TOWN_DISTRICTS_H

#include "core/enum_type.hpp"
#include <map>
#include "house.h"
#include "tile_type.h"
#include "town_type.h"

struct Town;

/** Kind of a district of a town. */
enum class TownDistrict : uint8_t {
	Normal, ///< Small town, or no district: houses follow the town zones like before.
	Historic, ///< Old town around the town centre: low and mid-rise houses, shops, churches; no skyscrapers.
	Business, ///< Central business district next to the old town: office towers and hotels.
	SubCentre, ///< Centre of an outer part of a big town: shops, offices, cinemas, theatres, churches, stadiums.
	HighRise, ///< Residential district of tall blocks of flats and towers with shops.
	MidRise, ///< Residential district of small flats, houses and shops.
	LowRise, ///< Residential district of small houses and cottages.
	Park, ///< Park: parks, fountains, statues and stadiums, no residents.
	End,
};

/** What a house type is, for the districts. */
enum class HouseCategory : uint8_t {
	Other, ///< Anything not known, e.g. toyland houses.
	Houses, ///< Small houses, cottages, town houses.
	Flats, ///< Blocks of flats.
	Office, ///< Office blocks and towers.
	Commercial, ///< Shops and offices, hotels, shopping malls, warehouses.
	Civic, ///< Churches, theatres, cinemas.
	Sports, ///< Stadiums.
	Park, ///< Parks, fountains, statues.
	End,
};
using HouseCategories = EnumBitSet<HouseCategory, uint8_t>;

/** The houses a town may build on a tile of a district. */
struct TownHousePick {
	TownDistrict district; ///< The district of the tile.
	HouseZones zones; ///< A house must be available in one of these town zones.
	HouseCategories categories; ///< A house must be of one of these categories.
	bool prefer_dense; ///< Weight the candidates by their population as well.
	uint32_t block; ///< Key of the block of the tile: each block of a district favours a few house types.
	uint8_t min_population = 0; ///< Smallest population of a house type (the original population, not scaled by the town density).
	uint8_t max_population = UINT8_MAX; ///< Largest population of a house type.

	bool Allows(const HouseSpec &hs) const;
	uint GetProbability(const HouseSpec &hs) const;
};

/**
 * Memory of one builder step: the most residents a house type allowed by a pick can have. Lives for one
 * step only, so it never sees other house types or another year than the step it was made for.
 */
struct TownReplaceCache {
	std::map<uint64_t, uint8_t> max_population{}; ///< Largest population per pick (district, zones, categories, limits, climate).
};

/** Population from which a town gets districts. */
static constexpr uint32_t TOWN_DISTRICTS_MIN_POPULATION = 20000;

TownDistrict GetTownDistrict(const Town *t, TileIndex tile);
TownHousePick GetTownHousePick(const Town *t, TileIndex tile, HouseZone zone);
HouseCategory GetHouseCategory(const HouseSpec &hs);
HouseZone GetTownRoadsideZone(const Town *t, TileIndex tile, HouseZone zone);
uint GetTownLandmarkLimit(const Town *t, bool stadium);
std::string_view GetTownDistrictName(TownDistrict district);
char GetTownDistrictLetter(TownDistrict district);

#endif /* TOWN_DISTRICTS_H */
