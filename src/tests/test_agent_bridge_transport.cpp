/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file test_agent_bridge_transport.cpp Test the duration accumulator of the bridge transport diagnostics. */

#include "../stdafx.h"

#include "../3rdparty/catch2/catch.hpp"

#include "../agent_bridge.h"

#include "../safeguards.h"

TEST_CASE("Agent bridge: transport durations accumulate, average and reset")
{
	/* An empty class reports zero instead of dividing by zero, so a world.info of a fresh epoch can
	 * be read without a special case. */
	InGameAgent::DurationStats stats;
	CHECK(stats.count == 0);
	CHECK(stats.total == 0);
	CHECK(stats.last == 0);
	CHECK(stats.max == 0);
	CHECK(stats.Average() == 0);

	SECTION("a zero duration is a measurement, not a missing one") {
		stats.Record(0);
		CHECK(stats.count == 1);
		CHECK(stats.total == 0);
		CHECK(stats.max == 0);
		CHECK(stats.Average() == 0);
	}

	SECTION("last, max and average follow the recorded values") {
		stats.Record(250);
		stats.Record(100);
		stats.Record(150);
		CHECK(stats.count == 3);
		CHECK(stats.last == 150);
		CHECK(stats.max == 250);
		CHECK(stats.total == 500);
		CHECK(stats.Average() == 166); // integer division of 500 / 3

		/* A later, smaller measurement moves `last` but never lowers `max`. */
		stats.Record(1);
		CHECK(stats.last == 1);
		CHECK(stats.max == 250);
	}

	SECTION("a reset drops every number, so no epoch reports the previous one") {
		stats.Record(7000);
		stats = InGameAgent::DurationStats{};
		CHECK(stats.count == 0);
		CHECK(stats.total == 0);
		CHECK(stats.last == 0);
		CHECK(stats.max == 0);
		CHECK(stats.Average() == 0);
	}

	SECTION("the boundary of the unsigned accumulator still adds up") {
		const uint64_t half = UINT64_MAX / 2;
		stats.Record(half);
		stats.Record(half);
		CHECK(stats.count == 2);
		CHECK(stats.max == half);
		CHECK(stats.total == half * 2);
		CHECK(stats.Average() == half);
	}
}
