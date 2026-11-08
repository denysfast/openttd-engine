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

#include <filesystem>

#include "../agent_bridge.h"
#include "../agent_chat_gui.h"
#include "../company_base.h"
#include "../company_func.h"
#include "../language.h"
#include "../openttd.h"
#include "../strings_func.h"
#include "../table/strings.h"

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

/**
 * Load the language pack the build generated, for the duration of one test case.
 *
 * The test binary has no language pack of its own: `GetString()` aborts on the first string that is
 * not in the loaded table ("String 0x88CF is invalid"), and the tray label of a setting is built
 * from STR_AGENT_CHAT_ATTACHMENT_SETTING_COMPANY and the name of the owning company. The pack is the
 * real `english.lng` the build writes to `${CMAKE_BINARY_DIR}/lang`; CTest passes its path in
 * `OTTD_TEST_LANGUAGE_FILE`, and the relative path is the fallback for a manual run from the build
 * directory. The language that was current before is put back when the fixture goes out of scope, so
 * the other cases of this binary keep the table they had.
 *
 * The metadata is static because `ReadLanguagePack()` keeps the pointer it is given as the current
 * language, so a stack object would dangle once the pack is loaded.
 */
class TestLanguagePack {
public:
	TestLanguagePack()
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
			this->previous_language = _current_language;
			this->previous_text_dir = _current_text_dir;
			REQUIRE(ReadLanguagePack(&english));
			this->loaded = true;
			return;
		}
		FAIL("no english.lng found: CTest passes OTTD_TEST_LANGUAGE_FILE, a manual run needs the build directory as its working directory");
	}

	~TestLanguagePack()
	{
		if (!this->loaded) return;
		_current_language = this->previous_language;
		_current_text_dir = this->previous_text_dir;
	}

private:
	const LanguageMetadata *previous_language = nullptr; ///< Language that was current before the fixture ran.
	TextDirection previous_text_dir = TD_LTR;            ///< Text direction that was current before the fixture ran.
	bool loaded = false;                                 ///< Whether this fixture loaded a pack and has to restore it.
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
	/* The label is built from the real strings of the build, so the pack is loaded here. */
	TestLanguagePack language;
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

	/* The owner caption of the picker formats that same owner label, so its string has to take the
	 * raw owner text and not a string id: a {STRING} slot would read the text as a number and throw.
	 * The caption needs the picker window, which the real UI test covers. */
	const std::string caption = GetString(STR_AGENT_CHAT_SETTING_PICKER_OWNER, owner.company->name);
	CHECK(caption.find(owner.company->name) != std::string::npos);
	CHECK(caption != owner.company->name);
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
