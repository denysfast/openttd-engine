/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file test_agent_bridge_poll.cpp Test the poll cadence contract of the agent bridge. */

#include "../stdafx.h"

#include "../3rdparty/catch2/catch.hpp"

#include "../agent_bridge.h"

#include "../safeguards.h"

TEST_CASE("Agent bridge: the healthy poll cadence is bounded on both sides")
{
	/* The cadence is part of the tool latency: a request waits at most one interval for a poll, and
	 * the measured round trip spans two of them, so the interval has to stay well inside the 250 ms
	 * round-trip budget of the plan (E16: 150 ms gave 256..408 ms). It also has to stay non-zero,
	 * because the loop sleeps between polls instead of spinning on the HTTP layer. */
	STATIC_REQUIRE(InGameAgent::POLL_IDLE.count() > 0);
	STATIC_REQUIRE(InGameAgent::POLL_IDLE.count() <= 50);

	/* A poll that failed backs off much further than the healthy cadence, so a backend that is down
	 * is not hammered with retries. */
	STATIC_REQUIRE(InGameAgent::POLL_RETRY > InGameAgent::POLL_IDLE);
	STATIC_REQUIRE(InGameAgent::POLL_RETRY >= std::chrono::seconds(1));
}
