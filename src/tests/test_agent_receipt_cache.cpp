/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <http://www.gnu.org/licenses/>.
 */

/**
 * @file test_agent_receipt_cache.cpp Test the receipt cache of the bridge: a request that waits for
 * the draw tick reserves its receipt slot before it is scheduled, a full cache of acknowledged
 * receipts makes room for it, a full cache of unacknowledged receipts does not, and a reserved slot
 * is never taken by an ordinary result.
 *
 * No backend and no video driver run in this test binary, so the cases use the bridge's receipt
 * seams: they store, acknowledge and reserve exactly what an executed request and a scheduled
 * request do, and nothing else.
 */

#include "../stdafx.h"

#include "../3rdparty/catch2/catch.hpp"

#include "../agent_bridge.h"
#include "../core/format.hpp"

#include "../safeguards.h"

using namespace InGameAgent;

namespace {

/** The number of receipts the cache holds (agent_bridge.cpp: MAX_RECEIPTS). */
constexpr size_t RECEIPT_LIMIT = 512;

/** Store one receipt and acknowledge it, as a delivered ordinary read does. */
void StoreAcknowledged(const std::string &request_id)
{
	StoreReceiptForTest(request_id, Json{{"ok", true}});
	AcknowledgeReceiptForTest(request_id);
}

} // namespace

TEST_CASE("Agent bridge: a full cache of acknowledged receipts still reserves a draw-tick slot")
{
	ClearReceiptCacheForTest();

	/* A long run of ordinary reads whose results the backend already acknowledged: the cache is
	 * full, and every receipt in it may be forgotten. */
	for (size_t i = 0; i < RECEIPT_LIMIT; i++) StoreAcknowledged(fmt::format("ack-{}", i));
	REQUIRE(ReceiptCountForTest() == RECEIPT_LIMIT);

	/* The camera request reserves its receipt slot: one acknowledged receipt makes room for it, so
	 * the deferred request is not refused with `busy` any more. */
	REQUIRE(ReserveDeferredForTest("camera-1"));
	CHECK(ReceiptCountForTest() == RECEIPT_LIMIT - 1);
	CHECK(DeferredReservationCountForTest() == 1);

	/* An ordinary result may not take the reserved slot: the cache stays within its bound and the
	 * reservation survives. */
	StoreReceiptForTest("ordinary-1", Json{{"ok", true}});
	CHECK(DeferredReservationCountForTest() == 1);
	CHECK(ReceiptCountForTest() + DeferredReservationCountForTest() <= RECEIPT_LIMIT);

	ClearReceiptCacheForTest();
}

TEST_CASE("Agent bridge: a full cache of unacknowledged receipts refuses the reservation")
{
	ClearReceiptCacheForTest();

	/* Nothing in the cache may be forgotten: dropping an unacknowledged receipt would execute its
	 * request a second time when the backend replays it. */
	for (size_t i = 0; i < RECEIPT_LIMIT; i++) StoreReceiptForTest(fmt::format("unack-{}", i), Json{{"ok", true}});
	REQUIRE(ReceiptCountForTest() == RECEIPT_LIMIT);

	CHECK_FALSE(ReserveDeferredForTest("camera-1"));
	CHECK(ReceiptCountForTest() == RECEIPT_LIMIT);
	CHECK(DeferredReservationCountForTest() == 0);
	/* The oldest receipt is still there, so its replay is still answered instead of executed again. */
	CHECK(HasReceiptForTest("unack-0"));

	ClearReceiptCacheForTest();
}

TEST_CASE("Agent bridge: only the acknowledged receipts make room, and the reservations stay bounded")
{
	ClearReceiptCacheForTest();

	/* 500 receipts nobody acknowledged and 12 the backend acknowledged: only those 12 may be
	 * forgotten, so at most 12 draw-tick requests can reserve a slot. */
	for (size_t i = 0; i < 500; i++) StoreReceiptForTest(fmt::format("u-{}", i), Json{{"ok", true}});
	for (size_t i = 0; i < 12; i++) StoreAcknowledged(fmt::format("a-{}", i));
	REQUIRE(ReceiptCountForTest() == RECEIPT_LIMIT);

	size_t reserved = 0;
	while (ReserveDeferredForTest(fmt::format("camera-{}", reserved))) {
		reserved++;
		REQUIRE(reserved <= 12);
	}
	CHECK(reserved == 12);
	CHECK(DeferredReservationCountForTest() == 12);
	CHECK(ReceiptCountForTest() == 500);
	CHECK(ReceiptCountForTest() + DeferredReservationCountForTest() == RECEIPT_LIMIT);

	/* Not one unacknowledged receipt was dropped for the reservations. */
	for (size_t i = 0; i < 500; i++) CHECK(HasReceiptForTest(fmt::format("u-{}", i)));

	ClearReceiptCacheForTest();
}

TEST_CASE("Agent bridge: an unacknowledged receipt survives the eviction of its neighbours")
{
	ClearReceiptCacheForTest();

	/* One receipt nobody acknowledged yet: a replay of that request has to be answered from it. */
	StoreReceiptForTest("first", Json{{"ok", true}});
	REQUIRE(HasReceiptForTest("first"));

	/* Churn the cache with acknowledged receipts far beyond its bound: only acknowledged receipts are
	 * forgotten, so the unacknowledged one is still there afterwards. */
	for (size_t i = 0; i < 3 * RECEIPT_LIMIT; i++) StoreAcknowledged(fmt::format("churn-{}", i));
	CHECK(HasReceiptForTest("first"));
	CHECK(ReceiptCountForTest() <= RECEIPT_LIMIT);

	ClearReceiptCacheForTest();
}
