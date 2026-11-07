/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <http://www.gnu.org/licenses/>.
 */

/**
 * @file test_agent_company_economy.cpp Test the delivered-cargo fields of the company info: the
 * engine's own counters of the cargo a company really delivered, the economy period those counters
 * belong to and the finished periods the engine still keeps.
 *
 * No game runs in this test binary and no cargo table is loaded, so the cases work on the company
 * pool directly: the amounts are the engine's own fields, and a cargo label is allowed to be missing
 * because it comes from the cargo table. Nothing here is a station or a delivery the test invented.
 */

#include "../stdafx.h"

#include "../3rdparty/catch2/catch.hpp"

#include "../agent_admin_tools.h"
#include "../cargotype.h"
#include "../company_base.h"
#include "../core/backup_type.hpp"
#include "../economy_type.h"
#include "../timer/timer_game_economy.h"

#include "../safeguards.h"

using namespace InGameAgent;

namespace {

/**
 * A real company in the engine's own company pool, removed again when the guard goes out of scope.
 *
 * The custom name and president name keep the company info off the language pack, which is not
 * loaded in this binary.
 */
class EconomyCompanyGuard {
public:
	EconomyCompanyGuard()
	{
		/* The pool asserts that the allocation was checked, as the other agent cases do. */
		REQUIRE(Company::CanAllocateItem());
		this->company = Company::Create();
		REQUIRE(this->company != nullptr);
		this->company->name = "Economy test";
		this->company->president_name = "Tester";
	}

	~EconomyCompanyGuard()
	{
		if (this->company != nullptr) delete this->company;
	}

	Company *Get() const { return this->company; }

private:
	Company *company = nullptr; ///< The company, or nullptr when it was not created.
};

/** Read the company info of a real company, as the bridge would. */
Json CompanyInfoOf(CompanyID company)
{
	const std::optional<Json> result = DispatchAdminTool("world.company", {{"op", "info"}}, company);
	REQUIRE(result.has_value());
	REQUIRE((*result)["ok"].get<bool>());
	return (*result)["company"];
}

/** The delivered-cargo row of one cargo id, or an empty object when the row is missing. */
Json DeliveredRow(const Json &info, uint cargo_id)
{
	for (const Json &row : info["delivered_cargo"]) {
		if (row["cargo_id"] == cargo_id) return row;
	}
	return Json::object();
}

} // namespace

TEST_CASE("agent_admin_tools: the company info reports the cargo the company really delivered")
{
	EconomyCompanyGuard company;
	Company *c = company.Get();

	/* The two cargo ids the cases below use; the label of a cargo comes from the cargo table, which
	 * this binary does not load, so only the id and the amount are asserted. */
	const CargoType passengers = static_cast<CargoType>(0);
	const CargoType coal = static_cast<CargoType>(1);

	const Json fresh = CompanyInfoOf(c->index);

	/* Every cargo type is listed, and a cargo that delivered nothing is a zero, not a missing value
	 * and not null. */
	REQUIRE(fresh["delivered_cargo"].is_array());
	CHECK(fresh["delivered_cargo"].size() == static_cast<size_t>(NUM_CARGO));
	for (const Json &row : fresh["delivered_cargo"]) {
		CHECK(row["cargo_id"].is_number_unsigned());
		CHECK(row["amount"] == 0);
		CHECK((row["label"].is_string() || row["label"].is_null()));
	}
	CHECK(fresh["delivered_cargo_total"] == 0);
	CHECK(fresh["delivered_cargo_count"] == 0);
	REQUIRE(fresh["delivered_cargo_history"].is_array());
	CHECK(fresh["delivered_cargo_history"].empty());

	/* The counter belongs to the engine's own economy quarter, and the descriptor says which one. */
	const Json period = fresh["delivered_cargo_period"];
	const int year = TimerGameEconomy::year.base();
	const int month = static_cast<int>(TimerGameEconomy::month);
	CHECK(period["year"] == year);
	CHECK(period["month"] == month);
	CHECK(period["quarter"] == month / 3 + 1);
	CHECK(period["months_into_period"] == month % 3);
	CHECK(period["length_months"] == 3);
	CHECK(period["period_index"] == year * 4 + month / 3);
	CHECK(period["date"].is_number_integer());

	/* The engine's own counter is what is reported: an amount appears as it is, per cargo, and the
	 * cargos that delivered nothing stay zero. */
	c->cur_economy.delivered_cargo[passengers] = 123;
	const Json delivered = CompanyInfoOf(c->index);
	const Json passengers_row = DeliveredRow(delivered, 0);
	REQUIRE(!passengers_row.empty());
	CHECK(passengers_row["amount"] == 123);
	const Json coal_row = DeliveredRow(delivered, 1);
	REQUIRE(!coal_row.empty());
	CHECK(coal_row["amount"] == 0);
	CHECK(delivered["delivered_cargo_total"] == 123);
	CHECK(delivered["delivered_cargo_count"] == 1);

	/* A finished quarter is the entry the engine itself keeps in `old_economy`, and it is reported
	 * with the period it belongs to: the current one minus one. */
	c->old_economy[0].delivered_cargo[coal] = 42;
	c->num_valid_stat_ent = 1;
	const Json finished = CompanyInfoOf(c->index);
	REQUIRE(finished["delivered_cargo_history"].is_array());
	REQUIRE(finished["delivered_cargo_history"].size() == 1);
	const Json &past = finished["delivered_cargo_history"][0];
	CHECK(past["periods_ago"] == 1);
	CHECK(past["period_index"].get<int>() == finished["delivered_cargo_period"]["period_index"].get<int>() - 1);
	CHECK(past["total"] == 42);
	CHECK(past["cargo_count"] == 1);
	REQUIRE(past["delivered_cargo"].is_array());
	REQUIRE(past["delivered_cargo"].size() == 1);
	CHECK(past["delivered_cargo"][0]["cargo_id"] == 1);
	CHECK(past["delivered_cargo"][0]["amount"] == 42);

	/* A quarter rollover is visible in the descriptor: the index changes, so a caller knows that the
	 * current counter restarted and that the two samples are not subtractable. The engine's field
	 * itself is untouched here, which is what makes the difference the period and not the amount. */
	{
		Backup<uint8_t> restore_month(TimerGameEconomy::month, static_cast<uint8_t>((month + 3) % 12));
		const Json next = CompanyInfoOf(c->index);
		CHECK(next["delivered_cargo_period"]["period_index"].get<int>() != finished["delivered_cargo_period"]["period_index"].get<int>());
		CHECK(next["delivered_cargo_total"] == 123);
		CHECK(next["delivered_cargo_history"].size() == 1);
	}
}
