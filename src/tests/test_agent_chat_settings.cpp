/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <http://www.gnu.org/licenses/>.
 */

/**
 * @file test_agent_chat_settings.cpp Test the settings the settings picker of the agent chat offers.
 *
 * The picker is built from the engine's own setting descriptors, so the tests check the real list
 * against the real descriptors and the real values: the list is not truncated, it is sorted by the
 * exact key, the filter narrows it down, and every row it offers is a setting reference the bridge
 * resolves to the same value the picker shows - for a company setting with the company the picker
 * shows it for and that company's lifetime. No window, no video driver and no map is needed.
 */

#include "../stdafx.h"

#include "../3rdparty/catch2/catch.hpp"

#include "../agent_bridge.h"
#include "../agent_chat_gui.h"
#include "../company_base.h"
#include "../company_func.h"
#include "../openttd.h"

#include "../safeguards.h"

using namespace InGameAgent;

namespace {

/** The company the picker shows company settings for in these tests. */
const CompanyID TEST_OWNER{0};

/**
 * One real company in the company pool, and that company as the local company.
 *
 * A company setting is read from the settings of a company, so the picker needs a company that
 * really exists in the pool: the id and the lifetime generation are the ones the engine hands out,
 * which is what a company scoped reference has to name. The local company is the company the
 * picker opens with, so the test exercises the default owner and not a special test path.
 */
class TestOwner {
public:
	TestOwner()
	{
		REQUIRE_FALSE(Company::IsValidID(TEST_OWNER));
		this->previous_local_company = _local_company;
		this->company = Company::CreateAtIndex(TEST_OWNER);
		REQUIRE(this->company != nullptr);
		_local_company = TEST_OWNER;
	}

	~TestOwner()
	{
		_local_company = this->previous_local_company;
		delete this->company;
	}

	Company *company = nullptr; ///< The company the fixture created.

private:
	CompanyID previous_local_company; ///< `_local_company` before the fixture ran.
};

/** Find one picker row by its exact key, or an empty object when the list does not offer it. */
Json FindSettingRow(const Json &rows, const std::string &name)
{
	for (const Json &row : rows) {
		if (row["name"] == name) return row;
	}
	return Json::object();
}

/**
 * Build the reference a picker row would attach.
 * @param row The picker row.
 * @return The reference, with the owner the row was shown for.
 */
Json RowToRef(const Json &row)
{
	Json ref = {{"type", "setting"}, {"setting_name", row["name"]}, {"scope", row["scope"]}};
	if (row["scope"] == "company") {
		ref["company_id"] = row["company_id"];
		ref["company_generation"] = row["company_generation"];
	}
	return ref;
}

} // namespace

TEST_CASE("Agent chat settings picker: the list is the engine's own settings")
{
	TestOwner owner;

	/* The picker must offer hundreds of settings, not a hand written sample. */
	const Json rows = SettingPickerRowsForTest("");
	CHECK(rows.size() > 100);

	/* Sorted by the exact key, so a player can scan the list and a search narrows it down. */
	std::string previous;
	for (const Json &row : rows) {
		const std::string name = row["name"].get<std::string>();
		CHECK(name > previous);
		previous = name;
	}

	/* The setting of the shared contract is offered by its exact engine key, with its scope and
	 * its value; a company setting is offered with the owner it is shown for. */
	const Json growth = FindSettingRow(rows, "economy.town_growth_rate");
	REQUIRE_FALSE(growth.empty());
	CHECK(growth["scope"] == "game");
	CHECK(growth["value"].is_string());
	CHECK_FALSE(growth["value"].get<std::string>().empty());

	const Json renew = FindSettingRow(rows, "engine_renew");
	REQUIRE_FALSE(renew.empty());
	CHECK(renew["scope"] == "company");
	CHECK(renew["company_id"] == TEST_OWNER.base());
	CHECK(renew["company_generation"] == static_cast<int64_t>(owner.company->generation));
	CHECK_FALSE(renew["value"].get<std::string>().empty());

	/* Every offered row is a reference the bridge resolves, and it shows the same value the bridge
	 * reports: the picker reads the same setting object the resolver reads, including the owner of
	 * a company setting. */
	for (const Json &row : rows) {
		const Json resolved = ResolveObjectRef(RowToRef(row), false);
		REQUIRE(resolved["found"] == true);
		CHECK(resolved["setting_name"] == row["name"]);
		CHECK(resolved["scope"] == row["scope"]);
		CHECK(resolved["value_text"] == row["value"]);
	}
}

TEST_CASE("Agent chat settings picker: the search filters the list")
{
	TestOwner owner;

	const Json all = SettingPickerRowsForTest("");
	const Json growth = SettingPickerRowsForTest("town_growth_rate");
	CHECK_FALSE(growth.empty());
	CHECK(growth.size() < all.size());
	CHECK_FALSE(FindSettingRow(growth, "economy.town_growth_rate").empty());

	/* The filter ignores case, like every other list filter of the game. */
	CHECK(SettingPickerRowsForTest("TOWN_GROWTH_RATE").size() == growth.size());

	/* A filter that matches nothing leaves an empty list instead of the whole list. */
	CHECK(SettingPickerRowsForTest("no-such-setting-exists-anywhere").empty());
}

TEST_CASE("Agent chat settings picker: the tray label of a setting names the key and the owner")
{
	TestOwner owner;
	owner.company->name = "Acme Transport";

	/* A game setting is labelled by its exact engine key, which is the identity the backend
	 * resolves, and it names no company. */
	const std::string game = SettingAttachmentLabelForTest("economy.town_growth_rate", "game", -1);
	CHECK(game == "economy.town_growth_rate");

	/* A company setting keeps the exact key and names the company that owns it, even though the
	 * picker stores that same key as the label of the attachment. */
	const std::string company = SettingAttachmentLabelForTest("engine_renew", "company", TEST_OWNER.base());
	CHECK(company.find("engine_renew") != std::string::npos);
	CHECK(company.find(owner.company->name) != std::string::npos);
	CHECK(company != game);

	/* A company that is gone is named by its id instead of losing the owner it was attached with. */
	const std::string gone = SettingAttachmentLabelForTest("engine_renew", "company", 5);
	CHECK(gone.find("engine_renew") != std::string::npos);
	CHECK(gone.find("#5") != std::string::npos);

	/* A setting without an owner id keeps the bare key, so a scope/owner mismatch stays visible. */
	const std::string no_owner = SettingAttachmentLabelForTest("engine_renew", "company", -1);
	CHECK(no_owner == "engine_renew");
}

TEST_CASE("Agent chat settings picker: a company setting belongs to one company lifetime")
{
	TestOwner owner;

	const Json rows = SettingPickerRowsForTest("engine_renew");
	const Json renew = FindSettingRow(rows, "engine_renew");
	REQUIRE_FALSE(renew.empty());

	const Json ref = RowToRef(renew);
	REQUIRE(ResolveObjectRef(ref, false)["found"] == true);

	/* The row pins the lifetime of the company, so a recreated company is not mistaken for it. */
	const int64_t generation = renew["company_generation"].get<int64_t>();
	delete owner.company;
	owner.company = Company::CreateAtIndex(TEST_OWNER);
	REQUIRE(owner.company != nullptr);
	REQUIRE(static_cast<int64_t>(owner.company->generation) != generation);

	const Json stale = ResolveObjectRef(ref, false);
	CHECK(stale["found"] == false);
	CHECK(stale["error"] == "stale_ref");
	CHECK(stale["reason"] == "company_lifetime_mismatch");

	/* The picker shows the values of the recreated company now, with its new lifetime. */
	const Json after = FindSettingRow(SettingPickerRowsForTest("engine_renew"), "engine_renew");
	REQUIRE_FALSE(after.empty());
	CHECK(after["company_generation"] == static_cast<int64_t>(owner.company->generation));
	CHECK(ResolveObjectRef(RowToRef(after), false)["found"] == true);
}
