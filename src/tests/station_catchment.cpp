/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file station_catchment.cpp Tests for the cost of bought station catchment levels. */

#include "../stdafx.h"

#include "../3rdparty/catch2/catch.hpp"

#include "../station_base.h"

#include "../safeguards.h"

/**
 * Get the cost of a catchment level as a plain number.
 * @param level The level being bought, 1-based.
 * @return The cost.
 */
static int64_t Cost(uint level)
{
	return static_cast<int64_t>(GetStationCatchmentUpgradeCost(level));
}

TEST_CASE("StationCatchmentUpgradeCost - known levels")
{
	CHECK(Cost(1) == 500'000'000);
	CHECK(Cost(2) == 550'000'000);
	CHECK(Cost(3) == 605'000'000);
	CHECK(Cost(10) == 1'178'973'845);
	CHECK(Cost(25) == 4'924'866'321);
	CHECK(Cost(MAX_STATION_CATCHMENT_LEVEL) == 53'359'478'360);
}

TEST_CASE("StationCatchmentUpgradeCost - every level costs about 10% more")
{
	int64_t total = 0;
	for (uint level = 1; level <= MAX_STATION_CATCHMENT_LEVEL; level++) {
		total += Cost(level);
		if (level == 1) continue;

		CHECK(Cost(level) == Cost(level - 1) * 11 / 10);
	}
	CHECK(total == 581'954'262'127);
}

TEST_CASE("StationCatchment - lookups reach upgraded stations")
{
	CHECK(MAX_CATCHMENT == MAX_AIRPORT_CATCHMENT_SPEC * CATCHMENT_SCALE + MAX_STATION_CATCHMENT_LEVEL);
	CHECK(MAX_CATCHMENT_UNMODIFIED == CA_UNMODIFIED + MAX_STATION_CATCHMENT_LEVEL);
	CHECK(MAX_CATCHMENT >= CA_DOCK + MAX_STATION_CATCHMENT_LEVEL);
}
