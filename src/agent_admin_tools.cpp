/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/>.
 */

/** @file agent_admin_tools.cpp Administrative and game-settings tool methods of the in-game agent. */

#include "stdafx.h"
#include "agent_admin_tools.h"

#include "command_func.h"
#include "company_base.h"
#include "company_cmd.h"
#include "company_func.h"
#include "core/backup_type.hpp"
#include "core/enum_type.hpp"
#include "economy_type.h"
#include "industry.h"
#include "map_func.h"
#include "misc_cmd.h"
#include "settings_cmd.h"
#include "settings_internal.h"
#include "settings_table.h"
#include "settings_type.h"
#include "station_base.h"
#include "station_cmd.h"
#include "strings_func.h"
#include "town.h"
#include "town_cmd.h"
#include "town_districts.h"
#include "town_growth.h"
#include "town_growth_type.h"
#include "town_type.h"

#include <array>
#include <charconv>
#include <cmath>
#include <optional>
#include <string>
#include <vector>

#include "table/strings.h"

#include "safeguards.h"

namespace InGameAgent {

namespace {

/** Result of the last posted command, filled in by AgentAdminCommandCallback. */
struct PostedCommand {
	bool captured = false; ///< The engine called the callback, so the command really ran.
	CommandCost cost{}; ///< The result the engine reported.
};

/** The last posted command of this module. The command path is synchronous in single player. */
PostedCommand _posted;

/** Maximum number of setting names accepted by one world.settings.get call. */
constexpr size_t MAX_SETTING_NAMES = 64;
/** Default number of settings returned by one world.settings.get call. */
constexpr size_t DEFAULT_SETTING_LIMIT = 50;
/** Maximum number of settings returned by one world.settings.get call. */
constexpr size_t MAX_SETTING_LIMIT = 500;

/**
 * Build an error document.
 * @param code Machine readable error code.
 * @param message Human readable explanation.
 * @param detail Extra machine readable context.
 * @return The result document.
 */
Json MakeError(std::string_view code, std::string_view message, Json detail = Json::object())
{
	Json result = Json::object();
	result["ok"] = false;
	result["error"] = std::string(code);
	result["message"] = std::string(message);
	if (!detail.empty()) result["detail"] = std::move(detail);
	return result;
}

/**
 * Build the document of an operation this module does not implement yet.
 * @param method The method name.
 * @param op The operation name, empty when the whole method is missing.
 * @return The result document.
 */
Json MakeGap(std::string_view method, std::string_view op)
{
	Json detail = Json::object();
	detail["method"] = std::string(method);
	if (!op.empty()) detail["op"] = std::string(op);
	return MakeError(op.empty() ? "unsupported_method" : "unsupported_op",
			"the admin tool module does not implement this yet", std::move(detail));
}

/** State of an optional parameter. */
enum class ParamState {
	Absent, ///< The parameter was not given.
	Invalid, ///< The parameter was given but has the wrong type or value.
	Present, ///< The parameter was given and is usable.
};

/**
 * Parse a whole string as a base 10 integer.
 * @param text The text to parse.
 * @param value Receives the number.
 * @return True when the whole text is one integer.
 */
bool ParseInt64(std::string_view text, int64_t &value)
{
	if (text.empty()) return false;
	const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
	return error == std::errc() && end == text.data() + text.size();
}

/**
 * Read an optional integer parameter.
 * @param params The method parameters.
 * @param key The parameter name.
 * @param value Receives the number.
 * @return Whether the parameter was absent, invalid or usable.
 */
ParamState ReadInt(const Json &params, std::string_view key, int64_t &value)
{
	const auto it = params.find(std::string(key));
	if (it == params.end() || it->is_null()) return ParamState::Absent;
	if (it->is_number_integer()) {
		value = it->get<int64_t>();
		return ParamState::Present;
	}
	if (it->is_number_unsigned()) {
		value = static_cast<int64_t>(it->get<uint64_t>());
		return ParamState::Present;
	}
	if (it->is_number_float()) {
		const double number = it->get<double>();
		if (number != std::floor(number) || number < -9.0e15 || number > 9.0e15) return ParamState::Invalid;
		value = static_cast<int64_t>(number);
		return ParamState::Present;
	}
	if (it->is_string()) return ParseInt64(it->get<std::string>(), value) ? ParamState::Present : ParamState::Invalid;
	return ParamState::Invalid;
}

/**
 * Read an optional string parameter.
 * @param params The method parameters.
 * @param key The parameter name.
 * @param value Receives the text.
 * @return Whether the parameter was absent, invalid or usable.
 */
ParamState ReadString(const Json &params, std::string_view key, std::string &value)
{
	const auto it = params.find(std::string(key));
	if (it == params.end() || it->is_null()) return ParamState::Absent;
	if (!it->is_string()) return ParamState::Invalid;
	value = it->get<std::string>();
	return ParamState::Present;
}

/**
 * Read an optional boolean parameter.
 * @param params The method parameters.
 * @param key The parameter name.
 * @param value Receives the flag.
 * @return Whether the parameter was absent, invalid or usable.
 */
ParamState ReadBool(const Json &params, std::string_view key, bool &value)
{
	const auto it = params.find(std::string(key));
	if (it == params.end() || it->is_null()) return ParamState::Absent;
	if (it->is_boolean()) {
		value = it->get<bool>();
		return ParamState::Present;
	}
	if (it->is_number_integer()) {
		const int64_t number = it->get<int64_t>();
		if (number != 0 && number != 1) return ParamState::Invalid;
		value = number != 0;
		return ParamState::Present;
	}
	if (it->is_string()) {
		const std::optional<bool> parsed = BoolSettingDesc::ParseSingleValue(it->get<std::string>());
		if (!parsed.has_value()) return ParamState::Invalid;
		value = *parsed;
		return ParamState::Present;
	}
	return ParamState::Invalid;
}

/**
 * Read an optional `{x, y}` tile object.
 * @param params The method parameters.
 * @param key The parameter name.
 * @param tile Receives the tile.
 * @return Whether the parameter was absent, invalid or usable.
 */
ParamState ReadTile(const Json &params, std::string_view key, TileIndex &tile)
{
	const auto it = params.find(std::string(key));
	if (it == params.end() || it->is_null()) return ParamState::Absent;
	if (!it->is_object()) return ParamState::Invalid;
	int64_t x = 0;
	int64_t y = 0;
	if (ReadInt(*it, "x", x) != ParamState::Present || ReadInt(*it, "y", y) != ParamState::Present) return ParamState::Invalid;
	if (x < 0 || y < 0 || x >= static_cast<int64_t>(Map::SizeX()) || y >= static_cast<int64_t>(Map::SizeY())) return ParamState::Invalid;
	tile = TileXY(static_cast<uint>(x), static_cast<uint>(y));
	return ParamState::Present;
}

/**
 * Check that a company exists.
 * @param company The company to check.
 * @return An error document, or nothing when the company is usable.
 */
std::optional<Json> CheckCompany(CompanyID company)
{
	if (!Company::IsValidID(company)) {
		return MakeError("unknown_company", "there is no company with that id", {{"company_id", company.base()}});
	}
	return std::nullopt;
}

/**
 * Check that a town exists.
 * @param town The town to check.
 * @return An error document, or nothing when the town is usable.
 */
std::optional<Json> CheckTown(TownID town)
{
	if (!Town::IsValidID(town)) {
		return MakeError("unknown_town", "there is no town with that id", {{"town_id", town.base()}});
	}
	return std::nullopt;
}

/**
 * Read an optional company id parameter.
 * @param params The method parameters.
 * @param key The parameter name.
 * @param company Receives the company.
 * @return An error document, or nothing when the parameter is usable or absent.
 */
std::optional<Json> ReadCompanyId(const Json &params, std::string_view key, CompanyID &company)
{
	int64_t id = 0;
	switch (ReadInt(params, key, id)) {
		case ParamState::Absent:
			return std::nullopt;
		case ParamState::Invalid:
			return MakeError("invalid_arguments", "`" + std::string(key) + "` must be a company id");
		case ParamState::Present:
			break;
	}
	if (id < 0 || id >= MAX_COMPANIES) {
		return MakeError("unknown_company", "the company id is out of range", {{"company_id", id}, {"max", MAX_COMPANIES - 1}});
	}
	company = CompanyID(static_cast<uint8_t>(id));
	return std::nullopt;
}

/**
 * Read an optional town id parameter.
 * @param params The method parameters.
 * @param key The parameter name.
 * @param town Receives the town.
 * @return An error document, or nothing when the parameter is usable or absent.
 */
std::optional<Json> ReadTownId(const Json &params, std::string_view key, TownID &town)
{
	int64_t id = 0;
	switch (ReadInt(params, key, id)) {
		case ParamState::Absent:
			return std::nullopt;
		case ParamState::Invalid:
			return MakeError("invalid_arguments", "`" + std::string(key) + "` must be a town id");
		case ParamState::Present:
			break;
	}
	if (id < 0 || id >= static_cast<int64_t>(TownID::End().base())) {
		return MakeError("unknown_town", "the town id is out of range", {{"town_id", id}});
	}
	town = TownID(static_cast<uint16_t>(id));
	return std::nullopt;
}

/**
 * Post one command through the normal player command path and remember its result.
 * @param args The command arguments.
 * @return True when the engine executed the command and it succeeded.
 */
template <Commands Tcmd, typename... Targs>
bool RunCommand(Targs... args)
{
	_posted = PostedCommand{};
	Command<Tcmd>::Post(STR_NULL, &AgentAdminCommandCallback, std::forward<Targs>(args)...);
	return _posted.captured && _posted.cost.Succeeded();
}

/**
 * Add the cost and the engine error of the last command to a result document.
 * @param result The result document to complete.
 * @return The completed document.
 */
Json AddCommandResult(Json result)
{
	result["cost"] = static_cast<int64_t>(_posted.cost.GetCost());
	if (_posted.cost.Failed()) {
		result["ok"] = false;
		result["error"] = "command_failed";
		const StringID message = _posted.cost.GetErrorMessage();
		if (message != INVALID_STRING_ID) {
			result["message_id"] = message.base();
			result["message"] = GetString(message);
		} else {
			result["message"] = "the engine rejected the command";
		}
	} else {
		result["ok"] = true;
	}
	return result;
}

/**
 * Report that the engine did not run the command at all.
 * @param result The result document to complete.
 * @return The completed document.
 */
Json AddNotExecuted(Json result)
{
	result["ok"] = false;
	result["error"] = "command_not_executed";
	result["message"] = "the engine did not execute the command; it was only estimated, the game is paused, or the command was rejected before dispatch";
	return result;
}

/**
 * Name of a setting scope for the protocol.
 * @param type The setting type.
 * @return The scope name.
 */
std::string_view ScopeName(SettingType type)
{
	switch (type) {
		case ST_GAME: return "game";
		case ST_COMPANY: return "company";
		case ST_CLIENT: return "client";
		default: return "unknown";
	}
}

/**
 * Name of a setting category for the protocol.
 * @param category The setting category.
 * @return The category name.
 */
std::string_view CategoryName(SettingCategory category)
{
	switch (category) {
		case SC_BASIC: return "basic";
		case SC_ADVANCED: return "advanced";
		case SC_EXPERT: return "expert";
		default: return "none";
	}
}

/**
 * The object a setting of the given scope lives in.
 * @param sd The setting.
 * @param company The company for per-company settings; must be valid then.
 * @return The object the setting is read from and written to.
 */
const void *SettingObject(const SettingDesc &sd, CompanyID company)
{
	switch (sd.GetType()) {
		case ST_GAME: return &GetGameSettings();
		case ST_COMPANY: return &Company::Get(company)->settings;
		case ST_CLIENT: return &_settings_client;
		default: NOT_REACHED();
	}
}

/**
 * Turn the allowed values of a one-of-many setting into a JSON array.
 * @param many The allowed values.
 * @return The array of names.
 */
Json ManyNames(const std::vector<std::string_view> &many)
{
	Json names = Json::array();
	for (const std::string_view name : many) names.push_back(std::string(name));
	return names;
}

/**
 * Describe one setting: its current value and the values it accepts.
 * @param sd The setting.
 * @param object The object the setting lives in.
 * @return The description.
 */
Json DescribeSetting(const SettingDesc &sd, const void *object)
{
	Json row = Json::object();
	row["name"] = sd.GetName();
	row["scope"] = ScopeName(sd.GetType());
	row["editable"] = sd.IsEditable(true);
	row["newgame_only"] = sd.flags.Test(SettingFlag::NewgameOnly);
	row["sandbox"] = sd.flags.Test(SettingFlag::Sandbox);
	row["formatted"] = sd.FormatValue(object);

	if (sd.IsIntSetting()) {
		const IntSettingDesc &isd = *sd.AsIntSetting();
		const auto [min_value, max_value] = isd.GetRange();
		row["value"] = isd.Read(object);
		row["min"] = min_value;
		row["max"] = max_value;
		row["default"] = isd.GetDefaultValue();
		row["category"] = CategoryName(isd.cat);
		if (isd.IsBoolSetting()) {
			row["type"] = "bool";
		} else if (const auto *many = dynamic_cast<const ManyOfManySettingDesc *>(&isd); many != nullptr) {
			row["type"] = "many_of_many";
			row["allowed"] = ManyNames(many->many);
		} else if (const auto *one = dynamic_cast<const OneOfManySettingDesc *>(&isd); one != nullptr) {
			row["type"] = "one_of_many";
			row["allowed"] = ManyNames(one->many);
		} else {
			row["type"] = "int";
		}
	} else if (sd.IsStringSetting()) {
		row["type"] = "string";
		row["value"] = sd.AsStringSetting()->Read(object);
	} else {
		row["type"] = "list";
	}
	return row;
}

/**
 * Every setting descriptor this module can see, in a stable order.
 * @return The descriptors.
 */
std::vector<const SettingDesc *> AllSettings()
{
	std::vector<const SettingDesc *> settings = GetFilteredSettingCollection([](const SettingDesc &sd) {
		return SlIsObjectCurrentlyValid(sd.save.version_from, sd.save.version_to);
	});
	for (const SettingVariant &desc : _company_settings) {
		const SettingDesc *sd = GetSettingDesc(desc);
		if (SlIsObjectCurrentlyValid(sd->save.version_from, sd->save.version_to)) settings.push_back(sd);
	}
	return settings;
}

/**
 * Turn a JSON value into the integer a setting accepts, checking the allowed range.
 * @param sd The setting.
 * @param value The value as sent by the caller.
 * @param parsed Receives the value to write.
 * @return An error document, or nothing when the value is usable.
 */
std::optional<Json> ParseSettingValue(const SettingDesc &sd, const Json &value, int32_t &parsed)
{
	const IntSettingDesc &isd = *sd.AsIntSetting();
	const auto [min_value, max_value] = isd.GetRange();
	const Json detail = {{"name", sd.GetName()}, {"min", min_value}, {"max", max_value}};

	if (isd.IsBoolSetting()) {
		std::optional<bool> parsed_bool;
		if (value.is_boolean()) {
			parsed_bool = value.get<bool>();
		} else if (value.is_number_integer()) {
			const int64_t number = value.get<int64_t>();
			if (number == 0 || number == 1) parsed_bool = number != 0;
		} else if (value.is_string()) {
			parsed_bool = BoolSettingDesc::ParseSingleValue(value.get<std::string>());
		}
		if (!parsed_bool.has_value()) return MakeError("invalid_value", "the value is not a boolean", detail);
		parsed = *parsed_bool ? 1 : 0;
		return std::nullopt;
	}

	if (dynamic_cast<const ManyOfManySettingDesc *>(&isd) != nullptr) {
		/* A combination of flags: only the bit mask itself is accepted. */
		int64_t number = 0;
		if (value.is_number_integer()) {
			number = value.get<int64_t>();
		} else if (value.is_string() && !ParseInt64(value.get<std::string>(), number)) {
			return MakeError("invalid_value", "a many-of-many setting takes the bit mask as an integer", detail);
		} else if (!value.is_string()) {
			return MakeError("invalid_value", "a many-of-many setting takes the bit mask as an integer", detail);
		}
		if (number < min_value || number > static_cast<int64_t>(max_value)) {
			return MakeError("value_out_of_range", "the value is outside the allowed range", detail);
		}
		parsed = static_cast<int32_t>(number);
		return std::nullopt;
	}

	if (const auto *one = dynamic_cast<const OneOfManySettingDesc *>(&isd); one != nullptr) {
		if (value.is_number_integer()) {
			const int64_t number = value.get<int64_t>();
			if (number < min_value || number > static_cast<int64_t>(max_value)) {
				return MakeError("value_out_of_range", "the value is outside the allowed range", detail);
			}
			parsed = static_cast<int32_t>(number);
			return std::nullopt;
		}
		if (value.is_string()) {
			const std::optional<uint32_t> index = OneOfManySettingDesc::ParseSingleValue(value.get<std::string>(), one->many);
			if (!index.has_value()) {
				Json allowed = detail;
				allowed["allowed"] = ManyNames(one->many);
				return MakeError("invalid_value", "the value is not one of the allowed values", std::move(allowed));
			}
			parsed = static_cast<int32_t>(*index);
			return std::nullopt;
		}
		return MakeError("invalid_value", "the value is not one of the allowed values", detail);
	}

	std::optional<int32_t> number;
	if (value.is_number_integer()) {
		const int64_t raw = value.get<int64_t>();
		if (raw < INT32_MIN || raw > INT32_MAX) return MakeError("value_out_of_range", "the value is outside the allowed range", detail);
		number = static_cast<int32_t>(raw);
	} else if (value.is_string()) {
		number = IntSettingDesc::ParseSingleValue(value.get<std::string>(), min_value, max_value);
	}
	if (!number.has_value()) return MakeError("invalid_value", "the value is not an integer", detail);
	if (*number < min_value || *number > static_cast<int64_t>(max_value)) {
		return MakeError("value_out_of_range", "the value is outside the allowed range", detail);
	}
	parsed = *number;
	return std::nullopt;
}

/**
 * Name of a town need for the protocol.
 * @param need The need.
 * @return The name.
 */
std::string_view NeedName(TownNeed need)
{
	switch (need) {
		case TownNeed::Passengers: return "passengers";
		case TownNeed::Mail: return "mail";
		case TownNeed::Goods: return "goods";
		case TownNeed::Electricity: return "electricity";
		case TownNeed::Valuables: return "valuables";
		default: return "unknown";
	}
}

/**
 * Read a town need by name.
 * @param name The name, or "all" for every need.
 * @param need Receives the need.
 * @return True when the name is known.
 */
bool ParseNeedName(std::string_view name, TownNeed &need)
{
	if (name == "all") {
		need = TownNeed::End;
		return true;
	}
	for (TownNeed candidate : EnumRange(TownNeed::End)) {
		if (NeedName(candidate) == name) {
			need = candidate;
			return true;
		}
	}
	return false;
}

/**
 * Name of a town growth status for the protocol.
 * @param status The status.
 * @return The name.
 */
std::string_view GrowthStatusName(TownGrowthStatus status)
{
	switch (status) {
		case TownGrowthStatus::Growing: return "growing";
		case TownGrowthStatus::Blocked: return "blocked";
		case TownGrowthStatus::NoSpace: return "no_space";
		case TownGrowthStatus::Disabled: return "disabled";
		case TownGrowthStatus::Custom: return "custom";
		default: return "unknown";
	}
}

/**
 * Name of a town acceptance effect for the protocol.
 * @param effect The effect.
 * @return The name.
 */
std::string_view AcceptanceEffectName(TownAcceptanceEffect effect)
{
	switch (effect) {
		case TownAcceptanceEffect::None: return "none";
		case TownAcceptanceEffect::Passengers: return "passengers";
		case TownAcceptanceEffect::Mail: return "mail";
		case TownAcceptanceEffect::Goods: return "goods";
		case TownAcceptanceEffect::Water: return "water";
		case TownAcceptanceEffect::Food: return "food";
		default: return "unknown";
	}
}

/**
 * Read a town acceptance effect by name.
 * @param name The name.
 * @param effect Receives the effect.
 * @return True when the name is known.
 */
bool ParseAcceptanceEffectName(std::string_view name, TownAcceptanceEffect &effect)
{
	for (TownAcceptanceEffect candidate : EnumRange(TownAcceptanceEffect::End)) {
		if (AcceptanceEffectName(candidate) == name) {
			effect = candidate;
			return true;
		}
	}
	return false;
}

/**
 * The name of a town, custom or generated.
 * @param t The town.
 * @return The name.
 */
std::string TownName(const Town *t)
{
	return t->name.empty() ? GetString(STR_TOWN_NAME, t->index) : t->name;
}

/**
 * The name of a company, custom or generated.
 * @param c The company.
 * @return The name.
 */
std::string CompanyName(const Company *c)
{
	return c->name.empty() ? GetString(c->name_1, c->name_2) : c->name;
}

/**
 * The name of the president of a company, custom or generated.
 * @param c The company.
 * @return The name.
 */
std::string PresidentName(const Company *c)
{
	return c->president_name.empty() ? GetString(c->president_name_1, c->president_name_2) : c->president_name;
}

/**
 * The finance and context of one company.
 * @param company The company; must be valid.
 * @return The description.
 */
Json CompanyInfo(CompanyID company)
{
	const Company *c = Company::Get(company);
	Json info = Json::object();
	info["company_id"] = company.base();
	info["name"] = CompanyName(c);
	info["president_name"] = PresidentName(c);
	info["colour"] = to_underlying(c->colour);
	info["is_ai"] = c->is_ai;
	info["is_local"] = company == _local_company;
	info["is_current"] = company == _current_company;
	info["money"] = static_cast<int64_t>(c->money);
	info["loan"] = static_cast<int64_t>(c->current_loan);
	info["max_loan"] = static_cast<int64_t>(c->GetMaxLoan());
	info["loan_interval"] = LOAN_INTERVAL;
	info["company_value"] = static_cast<int64_t>(c->cur_economy.company_value);
	info["income"] = static_cast<int64_t>(c->cur_economy.income);
	info["expenses"] = static_cast<int64_t>(c->cur_economy.expenses);
	info["performance_history"] = c->cur_economy.performance_history;
	info["months_of_bankruptcy"] = c->months_of_bankruptcy;
	info["inaugurated_year"] = c->inaugurated_year.base();
	return info;
}

/**
 * The growth state of one town: the needs, the investment and the power links.
 * @param town The town; must be valid.
 * @return The description.
 */
Json TownNeeds(TownID town_id)
{
	const Town *t = Town::Get(town_id);
	Json result = Json::object();
	result["town_id"] = town_id.base();
	result["name"] = TownName(t);
	result["population"] = t->cache.population;
	result["houses"] = t->cache.num_houses;
	result["growth_status"] = GrowthStatusName(t->growth.status);
	result["blocking_need"] = t->growth.blocking_need == TownNeed::End ? Json(nullptr) : Json(NeedName(t->growth.blocking_need));
	result["multiplier"] = t->growth.multiplier;
	result["yearly_growth"] = t->growth.yearly_growth;
	result["budget"] = t->growth.budget;
	result["investment"] = t->growth.investment;
	result["investment_bonus"] = GetTownInvestmentBonus(t);
	result["custom_growth"] = t->flags.Test(TownFlag::CustomGrowth);
	result["growth_rate"] = t->growth_rate;
	result["grow_counter"] = t->grow_counter;
	result["electricity_available"] = IsElectricityAvailable();
	result["power_received"] = GetTownPowerReceived(t);
	result["power_links_wanted"] = GetTownWantedPowerLinks(t);

	Json amounts = Json::array();
	for (const int64_t amount : TownGrowthBalance::INVESTMENT_AMOUNTS) amounts.push_back(amount);
	result["investment_amounts"] = std::move(amounts);

	Json needs = Json::array();
	for (const TownNeed need : EnumRange(TownNeed::End)) {
		const TownNeedState &state = t->growth.needs[need];
		const uint8_t effective = GetTownNeedEffectiveLevel(t, need);
		Json row = Json::object();
		row["need"] = NeedName(need);
		row["level"] = state.level;
		row["effective_level"] = effective;
		row["forced"] = state.forced == TOWN_NEED_NOT_FORCED ? Json(nullptr) : Json(state.forced);
		row["coverage"] = state.coverage;
		row["supplied"] = state.supplied;
		row["demand"] = state.demand;
		row["unlocked"] = IsTownNeedUnlocked(t, need);
		row["unlock_population"] = GetTownNeedUnlockPopulation(need);
		row["bonus"] = IsTownNeedBonus(need);
		row["multiplier"] = GetTownNeedMultiplier(need, effective);
		needs.push_back(std::move(row));
	}
	result["needs"] = std::move(needs);

	Json links = Json::array();
	for (const TownPowerLink &link : t->growth.power_links) {
		links.push_back(Json{{"plant", link.plant.base()}, {"delivered", link.delivered}});
	}
	result["power_links"] = std::move(links);
	return result;
}

/**
 * The state of one power plant.
 * @param i The industry; must be a power plant.
 * @return The description.
 */
Json PowerPlantInfo(const Industry *i)
{
	Json info = Json::object();
	info["industry_id"] = i->index.base();
	info["name"] = GetString(STR_INDUSTRY_NAME, i->index);
	info["fuel"] = GetPowerPlantFuelStock(i);
	info["output"] = i->power_output;
	info["demand"] = i->power_demand;
	Json towns = Json::array();
	for (const Town *t : Town::Iterate()) {
		for (const TownPowerLink &link : t->growth.power_links) {
			if (link.plant == i->index) towns.push_back(t->index.base());
		}
	}
	info["linked_towns"] = std::move(towns);
	return info;
}

/* --- world.settings.get --- */

/**
 * Read settings with their descriptors.
 * @param params The method parameters.
 * @param company The company the call is made for.
 * @return The result document.
 */
Json SettingsGet(const Json &params, CompanyID company)
{
	/* The per-company settings are read from the company the call names, or from the caller's company. */
	CompanyID target = company;
	if (const auto error = ReadCompanyId(params, "company_id", target); error.has_value()) return *error;
	if (params.contains("company_id")) {
		if (const auto error = CheckCompany(target); error.has_value()) return *error;
	}

	std::string prefix;
	switch (ReadString(params, "prefix", prefix)) {
		case ParamState::Invalid: return MakeError("invalid_arguments", "`prefix` must be a string");
		case ParamState::Present: break;
		case ParamState::Absent: break;
	}

	bool writable_only = false;
	switch (ReadBool(params, "writable_only", writable_only)) {
		case ParamState::Invalid: return MakeError("invalid_arguments", "`writable_only` must be a boolean");
		case ParamState::Present: break;
		case ParamState::Absent: break;
	}

	int64_t limit = DEFAULT_SETTING_LIMIT;
	switch (ReadInt(params, "limit", limit)) {
		case ParamState::Invalid: return MakeError("invalid_arguments", "`limit` must be an integer");
		case ParamState::Present:
			if (limit < 1 || limit > static_cast<int64_t>(MAX_SETTING_LIMIT)) {
				return MakeError("invalid_arguments", "`limit` is out of range", {{"min", 1}, {"max", MAX_SETTING_LIMIT}});
			}
			break;
		case ParamState::Absent: break;
	}

	int64_t offset = 0;
	switch (ReadInt(params, "offset", offset)) {
		case ParamState::Invalid: return MakeError("invalid_arguments", "`offset` must be an integer");
		case ParamState::Present:
			if (offset < 0) return MakeError("invalid_arguments", "`offset` must not be negative");
			break;
		case ParamState::Absent: break;
	}

	std::vector<std::string> names;
	const auto names_it = params.find("names");
	if (names_it != params.end() && !names_it->is_null()) {
		if (!names_it->is_array()) return MakeError("invalid_arguments", "`names` must be an array of setting names");
		if (names_it->size() > MAX_SETTING_NAMES) {
			return MakeError("invalid_arguments", "too many setting names in one call", {{"max", MAX_SETTING_NAMES}});
		}
		for (const Json &item : *names_it) {
			if (!item.is_string()) return MakeError("invalid_arguments", "`names` must be an array of setting names");
			names.push_back(item.get<std::string>());
		}
	}

	Json rows = Json::array();
	std::vector<std::string> unknown;
	size_t total = 0;

	if (!names.empty()) {
		for (const std::string &name : names) {
			const SettingDesc *sd = GetSettingFromName(name);
			if (sd == nullptr || !SlIsObjectCurrentlyValid(sd->save.version_from, sd->save.version_to)) {
				unknown.push_back(name);
				continue;
			}
			if (sd->GetType() == ST_COMPANY && !Company::IsValidID(target)) {
				unknown.push_back(name);
				continue;
			}
			rows.push_back(DescribeSetting(*sd, SettingObject(*sd, target)));
		}
		total = rows.size();
	} else {
		for (const SettingDesc *sd : AllSettings()) {
			if (!prefix.empty() && !sd->GetName().starts_with(prefix)) continue;
			if (writable_only && !(sd->IsIntSetting() && sd->IsEditable(true))) continue;
			if (sd->GetType() == ST_COMPANY && !Company::IsValidID(target)) continue;
			if (total >= static_cast<size_t>(offset) && rows.size() < static_cast<size_t>(limit)) {
				rows.push_back(DescribeSetting(*sd, SettingObject(*sd, target)));
			}
			total++;
		}
	}

	Json result = Json::object();
	result["ok"] = true;
	result["settings"] = std::move(rows);
	result["total"] = total;
	result["offset"] = offset;
	result["limit"] = limit;
	if (!unknown.empty()) result["unknown"] = std::move(unknown);
	return result;
}

/* --- world.settings.set --- */

/**
 * Change one setting through the normal command path.
 * @param params The method parameters.
 * @param company The company the call is made for.
 * @return The result document.
 */
Json SettingsSet(const Json &params, CompanyID company)
{
	std::string name;
	switch (ReadString(params, "name", name)) {
		case ParamState::Invalid: return MakeError("invalid_arguments", "`name` must be a string");
		case ParamState::Absent: return MakeError("invalid_arguments", "`name` is required");
		case ParamState::Present: break;
	}
	if (name.empty()) return MakeError("invalid_arguments", "`name` is required");

	const auto value_it = params.find("value");
	if (value_it == params.end()) return MakeError("invalid_arguments", "`value` is required");

	const SettingDesc *sd = GetSettingFromName(name);
	if (sd == nullptr || !SlIsObjectCurrentlyValid(sd->save.version_from, sd->save.version_to)) {
		return MakeError("unknown_setting", "there is no setting with that name", {{"name", name}});
	}

	const SettingType scope = sd->GetType();
	CompanyID target = company;
	if (scope == ST_COMPANY) {
		if (const auto error = ReadCompanyId(params, "company_id", target); error.has_value()) return *error;
		if (const auto error = CheckCompany(target); error.has_value()) return *error;
	} else if (params.contains("company_id")) {
		return MakeError("invalid_arguments", "`company_id` only applies to per-company settings", {{"name", name}});
	}

	if (scope == ST_CLIENT) {
		return MakeGap("world.settings.set", "client_scope");
	}
	if (!sd->IsIntSetting()) {
		return MakeGap("world.settings.set", "string_setting");
	}
	if (!sd->IsEditable(true)) {
		return MakeError("setting_not_editable", "the setting cannot be changed now", {{"name", name}, {"newgame_only", sd->flags.Test(SettingFlag::NewgameOnly)}});
	}

	int32_t parsed = 0;
	if (const auto error = ParseSettingValue(*sd, *value_it, parsed); error.has_value()) return *error;

	const IntSettingDesc &isd = *sd->AsIntSetting();
	const void *object = SettingObject(*sd, target);
	const int32_t old_value = isd.Read(object);

	Json result = Json::object();
	result["name"] = sd->GetName();
	result["scope"] = ScopeName(scope);
	if (scope == ST_COMPANY) result["company_id"] = target.base();
	result["old_value"] = old_value;
	result["old_formatted"] = sd->FormatValue(object);

	bool ok;
	if (scope == ST_COMPANY) {
		const Backup<CompanyID> context(_current_company, target);
		ok = RunCommand<Commands::ChangeCompanySetting>(sd->GetName(), parsed);
	} else {
		ok = RunCommand<Commands::ChangeSetting>(sd->GetName(), parsed);
	}
	if (!_posted.captured) return AddNotExecuted(std::move(result));
	result = AddCommandResult(std::move(result));
	if (!ok) return result;

	/* Report what the engine actually applied, not what was asked for. */
	const void *after = SettingObject(*sd, target);
	result["value"] = isd.Read(after);
	result["formatted"] = sd->FormatValue(after);
	result["changed"] = isd.Read(after) != old_value;
	return result;
}

/* --- world.company --- */

/**
 * Finance, loan and company creation operations.
 * @param params The method parameters.
 * @param company The company the call is made for.
 * @return The result document.
 */
Json CompanyOp(const Json &params, CompanyID company)
{
	std::string op;
	switch (ReadString(params, "op", op)) {
		case ParamState::Invalid: return MakeError("invalid_arguments", "`op` must be a string");
		case ParamState::Absent: return MakeError("invalid_arguments", "`op` is required");
		case ParamState::Present: break;
	}

	if (op == "info") {
		CompanyID target = company;
		if (const auto error = ReadCompanyId(params, "company_id", target); error.has_value()) return *error;
		if (const auto error = CheckCompany(target); error.has_value()) return *error;
		Json result = Json::object();
		result["ok"] = true;
		result["company"] = CompanyInfo(target);
		return result;
	}

	if (op == "loan_increase" || op == "loan_decrease") {
		/* The argument shape is checked before the world is touched. */
		bool all = false;
		switch (ReadBool(params, "all", all)) {
			case ParamState::Invalid: return MakeError("invalid_arguments", "`all` must be a boolean");
			case ParamState::Present: break;
			case ParamState::Absent: break;
		}

		int64_t amount = LOAN_INTERVAL;
		switch (ReadInt(params, "amount", amount)) {
			case ParamState::Invalid: return MakeError("invalid_arguments", "`amount` must be an integer");
			case ParamState::Present: break;
			case ParamState::Absent: break;
		}
		if (!all && (amount < LOAN_INTERVAL || amount % LOAN_INTERVAL != 0)) {
			return MakeError("invalid_amount", "the amount must be a positive multiple of the loan interval",
					{{"amount", amount}, {"loan_interval", LOAN_INTERVAL}});
		}

		CompanyID target = company;
		if (const auto error = ReadCompanyId(params, "company_id", target); error.has_value()) return *error;
		if (const auto error = CheckCompany(target); error.has_value()) return *error;

		Json result = Json::object();
		result["company_id"] = target.base();
		result["before"] = CompanyInfo(target);

		const Backup<CompanyID> context(_current_company, target);
		const bool ok = op == "loan_increase"
				? (all ? RunCommand<Commands::IncreaseLoan>(LoanCommand::Max, Money(0)) : RunCommand<Commands::IncreaseLoan>(LoanCommand::Amount, Money(amount)))
				: (all ? RunCommand<Commands::DecreaseLoan>(LoanCommand::Max, Money(0)) : RunCommand<Commands::DecreaseLoan>(LoanCommand::Amount, Money(amount)));
		if (!_posted.captured) return AddNotExecuted(std::move(result));
		result = AddCommandResult(std::move(result));
		if (ok) result["after"] = CompanyInfo(target);
		return result;
	}

	if (op == "set_max_loan") {
		/* The argument shape is checked before the world is touched. */
		bool reset = false;
		switch (ReadBool(params, "reset", reset)) {
			case ParamState::Invalid: return MakeError("invalid_arguments", "`reset` must be a boolean");
			case ParamState::Present: break;
			case ParamState::Absent: break;
		}

		int64_t amount = 0;
		switch (ReadInt(params, "amount", amount)) {
			case ParamState::Invalid: return MakeError("invalid_arguments", "`amount` must be an integer");
			case ParamState::Present: break;
			case ParamState::Absent:
				if (!reset) return MakeError("invalid_arguments", "`amount` is required unless `reset` is set");
				break;
		}
		if (!reset && (amount < 0 || amount > MAX_LOAN_LIMIT)) {
			return MakeError("invalid_amount", "the maximum loan is out of range", {{"amount", amount}, {"max", MAX_LOAN_LIMIT}});
		}

		CompanyID target = company;
		if (const auto error = ReadCompanyId(params, "company_id", target); error.has_value()) return *error;
		if (const auto error = CheckCompany(target); error.has_value()) return *error;

		Json result = Json::object();
		result["company_id"] = target.base();
		result["before"] = CompanyInfo(target);

		const Backup<CompanyID> context(_current_company, OWNER_DEITY);
		const bool ok = RunCommand<Commands::SetCompanyMaxLoan>(target, reset ? Money(COMPANY_MAX_LOAN_DEFAULT) : Money(amount));
		if (!_posted.captured) return AddNotExecuted(std::move(result));
		result = AddCommandResult(std::move(result));
		if (ok) result["after"] = CompanyInfo(target);
		return result;
	}

	if (op == "create") {
		std::array<bool, MAX_COMPANIES> before{};
		for (const Company *c : Company::Iterate()) before[c->index.base()] = true;

		Json result = Json::object();
		const bool ok = RunCommand<Commands::CompanyControl>(CompanyCtrlAction::NewAI, CompanyID::Invalid(), CompanyRemoveReason::None, ClientID::Invalid);
		if (!_posted.captured) return AddNotExecuted(std::move(result));
		result = AddCommandResult(std::move(result));
		if (!ok) return result;

		Json created = Json::array();
		for (const Company *c : Company::Iterate()) {
			if (!before[c->index.base()]) created.push_back(c->index.base());
		}
		result["created"] = std::move(created);
		if (created.empty()) {
			/* The command succeeded but the pool was full, so no company exists. */
			result["ok"] = false;
			result["error"] = "no_company_created";
			result["message"] = "the engine accepted the command but no company was created; the company pool is full";
		}
		return result;
	}

	if (op == "delete") {
		CompanyID target = CompanyID::Invalid();
		if (const auto error = ReadCompanyId(params, "company_id", target); error.has_value()) return *error;
		if (target == CompanyID::Invalid()) return MakeError("invalid_arguments", "`company_id` is required");
		if (const auto error = CheckCompany(target); error.has_value()) return *error;
		if (Company::GetNumItems() <= 1) {
			return MakeError("last_company", "the last company of a single player game cannot be deleted", {{"company_id", target.base()}});
		}

		Json result = Json::object();
		result["company_id"] = target.base();
		result["before"] = CompanyInfo(target);
		const bool ok = RunCommand<Commands::CompanyControl>(CompanyCtrlAction::Delete, target, CompanyRemoveReason::Manual, ClientID::Invalid);
		if (!_posted.captured) return AddNotExecuted(std::move(result));
		result = AddCommandResult(std::move(result));
		if (ok) {
			result["deleted"] = !Company::IsValidID(target);
			result["local_company"] = _local_company.base();
		}
		return result;
	}

	if (op == "give_money") {
		/* The argument shape is checked before the world is touched. */
		int64_t amount = 0;
		switch (ReadInt(params, "amount", amount)) {
			case ParamState::Invalid: return MakeError("invalid_arguments", "`amount` must be an integer");
			case ParamState::Absent: return MakeError("invalid_arguments", "`amount` is required");
			case ParamState::Present: break;
		}
		if (amount <= 0) return MakeError("invalid_amount", "the amount must be positive", {{"amount", amount}});

		CompanyID to = CompanyID::Invalid();
		if (const auto error = ReadCompanyId(params, "to_company_id", to); error.has_value()) return *error;
		if (to == CompanyID::Invalid()) return MakeError("invalid_arguments", "`to_company_id` is required");

		CompanyID from = company;
		if (const auto error = ReadCompanyId(params, "company_id", from); error.has_value()) return *error;
		if (const auto error = CheckCompany(from); error.has_value()) return *error;
		if (const auto error = CheckCompany(to); error.has_value()) return *error;
		if (to == from) return MakeError("invalid_arguments", "a company cannot give money to itself", {{"company_id", from.base()}});

		Json result = Json::object();
		result["company_id"] = from.base();
		result["to_company_id"] = to.base();
		result["amount"] = amount;
		const Backup<CompanyID> context(_current_company, from);
		const bool ok = RunCommand<Commands::GiveMoney>(Money(amount), to);
		if (!_posted.captured) return AddNotExecuted(std::move(result));
		result = AddCommandResult(std::move(result));
		if (ok) {
			result["from_money"] = static_cast<int64_t>(Company::Get(from)->money);
			result["to_money"] = static_cast<int64_t>(Company::Get(to)->money);
		}
		return result;
	}

	return MakeGap("world.company", op);
}

/* --- world.towns --- */

/**
 * Town needs, investment and growth operations.
 * @param params The method parameters.
 * @param company The company the call is made for.
 * @return The result document.
 */
Json TownsOp(const Json &params, CompanyID company)
{
	std::string op;
	switch (ReadString(params, "op", op)) {
		case ParamState::Invalid: return MakeError("invalid_arguments", "`op` must be a string");
		case ParamState::Absent: return MakeError("invalid_arguments", "`op` is required");
		case ParamState::Present: break;
	}

	if (op == "needs") {
		TownID town = TownID::Invalid();
		if (const auto error = ReadTownId(params, "town_id", town); error.has_value()) return *error;
		if (town == TownID::Invalid()) return MakeError("invalid_arguments", "`town_id` is required");
		if (const auto error = CheckTown(town); error.has_value()) return *error;
		Json result = Json::object();
		result["ok"] = true;
		result["town"] = TownNeeds(town);
		return result;
	}

	if (op == "force_need") {
		TownID town = TownID::Invalid();
		if (const auto error = ReadTownId(params, "town_id", town); error.has_value()) return *error;
		if (town != TownID::Invalid()) {
			if (const auto error = CheckTown(town); error.has_value()) return *error;
		}

		std::string need_name = "all";
		switch (ReadString(params, "need", need_name)) {
			case ParamState::Invalid: return MakeError("invalid_arguments", "`need` must be a string");
			case ParamState::Present: break;
			case ParamState::Absent: break;
		}
		TownNeed need = TownNeed::End;
		if (!ParseNeedName(need_name, need)) {
			return MakeError("invalid_arguments", "unknown town need", {{"need", need_name}, {"allowed", Json::array({"passengers", "mail", "goods", "electricity", "valuables", "all"})}});
		}

		/* `level` is a number, or "auto" to let the engine compute it from the supply again. */
		uint8_t level = TOWN_NEED_NOT_FORCED;
		const auto level_it = params.find("level");
		if (level_it != params.end() && !level_it->is_null()) {
			if (level_it->is_string() && level_it->get<std::string>() == "auto") {
				level = TOWN_NEED_NOT_FORCED;
			} else {
				int64_t number = 0;
				if (ReadInt(params, "level", number) != ParamState::Present) return MakeError("invalid_arguments", "`level` must be an integer or \"auto\"");
				if (number < 0 || number > TOWN_NEED_LEVEL_MAX) {
					return MakeError("invalid_arguments", "the need level is out of range", {{"level", number}, {"min", 0}, {"max", TOWN_NEED_LEVEL_MAX}});
				}
				level = static_cast<uint8_t>(number);
			}
		}

		Json result = Json::object();
		result["town_id"] = town == TownID::Invalid() ? Json(nullptr) : Json(town.base());
		result["need"] = need_name;
		result["level"] = level == TOWN_NEED_NOT_FORCED ? Json("auto") : Json(level);
		const Backup<CompanyID> context(_current_company, OWNER_DEITY);
		const bool ok = RunCommand<Commands::TownForceNeedLevel>(town, need, level);
		if (!_posted.captured) return AddNotExecuted(std::move(result));
		result = AddCommandResult(std::move(result));
		if (ok && town != TownID::Invalid()) result["town"] = TownNeeds(town);
		return result;
	}

	if (op == "invest") {
		/* The argument shape is checked before the world is touched. */
		int64_t index = 0;
		switch (ReadInt(params, "amount_index", index)) {
			case ParamState::Invalid: return MakeError("invalid_arguments", "`amount_index` must be an integer");
			case ParamState::Absent: return MakeError("invalid_arguments", "`amount_index` is required");
			case ParamState::Present: break;
		}
		if (index < 0 || index >= static_cast<int64_t>(TownGrowthBalance::INVESTMENT_AMOUNTS.size())) {
			return MakeError("invalid_arguments", "the investment index is out of range",
					{{"amount_index", index}, {"max", TownGrowthBalance::INVESTMENT_AMOUNTS.size() - 1}});
		}

		TownID town = TownID::Invalid();
		if (const auto error = ReadTownId(params, "town_id", town); error.has_value()) return *error;
		if (town == TownID::Invalid()) return MakeError("invalid_arguments", "`town_id` is required");
		if (const auto error = CheckTown(town); error.has_value()) return *error;

		CompanyID investor = company;
		if (const auto error = ReadCompanyId(params, "company_id", investor); error.has_value()) return *error;
		if (const auto error = CheckCompany(investor); error.has_value()) return *error;

		Json result = Json::object();
		result["town_id"] = town.base();
		result["company_id"] = investor.base();
		result["amount_index"] = index;
		result["amount"] = TownGrowthBalance::INVESTMENT_AMOUNTS[index];
		result["investment_before"] = Town::Get(town)->growth.investment;
		const Backup<CompanyID> context(_current_company, investor);
		const bool ok = RunCommand<Commands::InvestInTown>(town, static_cast<uint8_t>(index));
		if (!_posted.captured) return AddNotExecuted(std::move(result));
		result = AddCommandResult(std::move(result));
		if (ok) result["investment_after"] = Town::Get(town)->growth.investment;
		return result;
	}

	if (op == "set_investment") {
		TownID town = TownID::Invalid();
		if (const auto error = ReadTownId(params, "town_id", town); error.has_value()) return *error;
		if (town != TownID::Invalid()) {
			if (const auto error = CheckTown(town); error.has_value()) return *error;
		}

		int64_t amount = 0;
		switch (ReadInt(params, "amount", amount)) {
			case ParamState::Invalid: return MakeError("invalid_arguments", "`amount` must be an integer");
			case ParamState::Absent: return MakeError("invalid_arguments", "`amount` is required");
			case ParamState::Present: break;
		}
		if (amount < 0) return MakeError("invalid_amount", "the investment must not be negative", {{"amount", amount}});

		bool add = false;
		switch (ReadBool(params, "add", add)) {
			case ParamState::Invalid: return MakeError("invalid_arguments", "`add` must be a boolean");
			case ParamState::Present: break;
			case ParamState::Absent: break;
		}

		Json result = Json::object();
		result["town_id"] = town == TownID::Invalid() ? Json(nullptr) : Json(town.base());
		result["amount"] = amount;
		result["add"] = add;
		const Backup<CompanyID> context(_current_company, OWNER_DEITY);
		const bool ok = RunCommand<Commands::SetTownInvestment>(town, Money(amount), add);
		if (!_posted.captured) return AddNotExecuted(std::move(result));
		result = AddCommandResult(std::move(result));
		if (ok && town != TownID::Invalid()) result["investment_after"] = Town::Get(town)->growth.investment;
		return result;
	}

	if (op == "growth_rate") {
		TownID town = TownID::Invalid();
		if (const auto error = ReadTownId(params, "town_id", town); error.has_value()) return *error;
		if (town == TownID::Invalid()) return MakeError("invalid_arguments", "`town_id` is required");
		if (const auto error = CheckTown(town); error.has_value()) return *error;

		int64_t rate = 0;
		switch (ReadInt(params, "growth_rate", rate)) {
			case ParamState::Invalid: return MakeError("invalid_arguments", "`growth_rate` must be an integer");
			case ParamState::Absent: return MakeError("invalid_arguments", "`growth_rate` is required");
			case ParamState::Present: break;
		}
		if (rate < 0 || rate > UINT16_MAX) {
			return MakeError("invalid_arguments", "the growth rate is out of range", {{"growth_rate", rate}, {"max", UINT16_MAX}});
		}

		Json result = Json::object();
		result["town_id"] = town.base();
		result["growth_rate"] = rate;
		result["growth_rate_before"] = Town::Get(town)->growth_rate;
		const Backup<CompanyID> context(_current_company, OWNER_DEITY);
		const bool ok = RunCommand<Commands::TownGrowthRate>(town, static_cast<uint16_t>(rate));
		if (!_posted.captured) return AddNotExecuted(std::move(result));
		result = AddCommandResult(std::move(result));
		if (ok) {
			result["growth_rate_after"] = Town::Get(town)->growth_rate;
			result["custom_growth"] = Town::Get(town)->flags.Test(TownFlag::CustomGrowth);
		}
		return result;
	}

	if (op == "expand") {
		/* The argument shape is checked before the world is touched. */
		int64_t grow_amount = 0;
		switch (ReadInt(params, "grow_amount", grow_amount)) {
			case ParamState::Invalid: return MakeError("invalid_arguments", "`grow_amount` must be an integer");
			case ParamState::Present: break;
			case ParamState::Absent: break;
		}
		if (grow_amount < 0 || grow_amount > UINT32_MAX) {
			return MakeError("invalid_arguments", "the grow amount is out of range", {{"grow_amount", grow_amount}});
		}

		TownExpandModes modes;
		const auto modes_it = params.find("modes");
		if (modes_it != params.end() && !modes_it->is_null()) {
			if (!modes_it->is_array()) return MakeError("invalid_arguments", "`modes` must be an array of \"buildings\" and \"roads\"");
			for (const Json &item : *modes_it) {
				if (!item.is_string()) return MakeError("invalid_arguments", "`modes` must be an array of \"buildings\" and \"roads\"");
				const std::string mode = item.get<std::string>();
				if (mode == "buildings") {
					modes.Set(TownExpandMode::Buildings);
				} else if (mode == "roads") {
					modes.Set(TownExpandMode::Roads);
				} else {
					return MakeError("invalid_arguments", "unknown expand mode", {{"mode", mode}, {"allowed", Json::array({"buildings", "roads"})}});
				}
			}
		} else {
			modes.Set(TownExpandMode::Buildings);
			modes.Set(TownExpandMode::Roads);
		}
		if (modes.None()) return MakeError("invalid_arguments", "`modes` must not be empty");

		TownID town = TownID::Invalid();
		if (const auto error = ReadTownId(params, "town_id", town); error.has_value()) return *error;
		if (town == TownID::Invalid()) return MakeError("invalid_arguments", "`town_id` is required");
		if (const auto error = CheckTown(town); error.has_value()) return *error;

		Json result = Json::object();
		result["town_id"] = town.base();
		result["grow_amount"] = grow_amount;
		result["population_before"] = Town::Get(town)->cache.population;
		const Backup<CompanyID> context(_current_company, OWNER_DEITY);
		const bool ok = RunCommand<Commands::ExpandTown>(town, static_cast<uint32_t>(grow_amount), modes);
		if (!_posted.captured) return AddNotExecuted(std::move(result));
		result = AddCommandResult(std::move(result));
		if (ok) result["population_after"] = Town::Get(town)->cache.population;
		return result;
	}

	if (op == "cargo_goal") {
		TownID town = TownID::Invalid();
		if (const auto error = ReadTownId(params, "town_id", town); error.has_value()) return *error;
		if (town == TownID::Invalid()) return MakeError("invalid_arguments", "`town_id` is required");
		if (const auto error = CheckTown(town); error.has_value()) return *error;

		std::string effect_name;
		switch (ReadString(params, "effect", effect_name)) {
			case ParamState::Invalid: return MakeError("invalid_arguments", "`effect` must be a string");
			case ParamState::Absent: return MakeError("invalid_arguments", "`effect` is required");
			case ParamState::Present: break;
		}
		TownAcceptanceEffect effect = TownAcceptanceEffect::None;
		if (!ParseAcceptanceEffectName(effect_name, effect)) {
			return MakeError("invalid_arguments", "unknown town acceptance effect", {{"effect", effect_name}});
		}

		int64_t goal = 0;
		switch (ReadInt(params, "goal", goal)) {
			case ParamState::Invalid: return MakeError("invalid_arguments", "`goal` must be an integer");
			case ParamState::Absent: return MakeError("invalid_arguments", "`goal` is required");
			case ParamState::Present: break;
		}
		if (goal < 0 || goal > UINT32_MAX) return MakeError("invalid_arguments", "the goal is out of range", {{"goal", goal}});

		Json result = Json::object();
		result["town_id"] = town.base();
		result["effect"] = effect_name;
		result["goal"] = goal;
		const Backup<CompanyID> context(_current_company, OWNER_DEITY);
		RunCommand<Commands::TownCargoGoal>(town, effect, static_cast<uint32_t>(goal));
		if (!_posted.captured) return AddNotExecuted(std::move(result));
		return AddCommandResult(std::move(result));
	}

	if (op == "rating") {
		TownID town = TownID::Invalid();
		if (const auto error = ReadTownId(params, "town_id", town); error.has_value()) return *error;
		if (town == TownID::Invalid()) return MakeError("invalid_arguments", "`town_id` is required");
		if (const auto error = CheckTown(town); error.has_value()) return *error;

		CompanyID rated = company;
		if (const auto error = ReadCompanyId(params, "company_id", rated); error.has_value()) return *error;
		if (const auto error = CheckCompany(rated); error.has_value()) return *error;

		int64_t rating = 0;
		switch (ReadInt(params, "rating", rating)) {
			case ParamState::Invalid: return MakeError("invalid_arguments", "`rating` must be an integer");
			case ParamState::Absent: return MakeError("invalid_arguments", "`rating` is required");
			case ParamState::Present: break;
		}
		if (rating < RATING_MINIMUM || rating > RATING_MAXIMUM) {
			return MakeError("invalid_arguments", "the rating is out of range", {{"rating", rating}, {"min", RATING_MINIMUM}, {"max", RATING_MAXIMUM}});
		}

		Json result = Json::object();
		result["town_id"] = town.base();
		result["company_id"] = rated.base();
		result["rating"] = rating;
		const Backup<CompanyID> context(_current_company, OWNER_DEITY);
		const bool ok = RunCommand<Commands::TownRating>(town, rated, static_cast<int16_t>(rating));
		if (!_posted.captured) return AddNotExecuted(std::move(result));
		result = AddCommandResult(std::move(result));
		if (ok) result["rating_after"] = Town::Get(town)->ratings[rated];
		return result;
	}

	if (op == "destroy") {
		TownID town = TownID::Invalid();
		if (const auto error = ReadTownId(params, "town_id", town); error.has_value()) return *error;
		if (town == TownID::Invalid()) return MakeError("invalid_arguments", "`town_id` is required");
		if (const auto error = CheckTown(town); error.has_value()) return *error;

		CompanyID payer = OWNER_DEITY;
		if (const auto error = ReadCompanyId(params, "company_id", payer); error.has_value()) return *error;
		if (payer != OWNER_DEITY) {
			if (const auto error = CheckCompany(payer); error.has_value()) return *error;
		}

		const auto [houses, roads] = GetTownDestructionCounts(Town::Get(town));
		Json result = Json::object();
		result["town_id"] = town.base();
		result["houses"] = houses;
		result["road_tiles"] = roads;
		const Backup<CompanyID> context(_current_company, payer);
		const bool ok = RunCommand<Commands::DestroyTown>(town);
		if (!_posted.captured) return AddNotExecuted(std::move(result));
		result = AddCommandResult(std::move(result));
		if (ok) result["destroyed"] = !Town::IsValidID(town);
		return result;
	}

	return MakeGap("world.towns", op);
}

/* --- world.project --- */

/**
 * Project specific operations: the power grid, the station town and the town districts.
 * @param params The method parameters.
 * @param company The company the call is made for.
 * @return The result document.
 */
Json ProjectOp(const Json &params, [[maybe_unused]] CompanyID company)
{
	std::string op;
	switch (ReadString(params, "op", op)) {
		case ParamState::Invalid: return MakeError("invalid_arguments", "`op` must be a string");
		case ParamState::Absent: return MakeError("invalid_arguments", "`op` is required");
		case ParamState::Present: break;
	}

	if (op == "power") {
		Json result = Json::object();
		result["ok"] = true;
		result["electricity_available"] = IsElectricityAvailable();

		TownID town = TownID::Invalid();
		if (const auto error = ReadTownId(params, "town_id", town); error.has_value()) return *error;
		if (town != TownID::Invalid()) {
			if (const auto error = CheckTown(town); error.has_value()) return *error;
			result["town"] = TownNeeds(town);
			return result;
		}

		int64_t industry_id = 0;
		switch (ReadInt(params, "industry_id", industry_id)) {
			case ParamState::Invalid: return MakeError("invalid_arguments", "`industry_id` must be an integer");
			case ParamState::Present: {
				if (industry_id < 0 || industry_id >= static_cast<int64_t>(IndustryID::End().base())) {
					return MakeError("unknown_industry", "the industry id is out of range", {{"industry_id", industry_id}});
				}
				const Industry *i = Industry::GetIfValid(IndustryID(static_cast<uint16_t>(industry_id)));
				if (i == nullptr) return MakeError("unknown_industry", "there is no industry with that id", {{"industry_id", industry_id}});
				if (!IsPowerPlant(i)) return MakeError("not_a_power_plant", "the industry is not a power plant", {{"industry_id", industry_id}});
				result["plant"] = PowerPlantInfo(i);
				return result;
			}
			case ParamState::Absent:
				break;
		}

		/* No id given: list every power plant. */
		Json plants = Json::array();
		for (const Industry *i : Industry::Iterate()) {
			if (IsPowerPlant(i)) plants.push_back(PowerPlantInfo(i));
		}
		result["plants"] = std::move(plants);
		return result;
	}

	if (op == "power_fuel") {
		IndustryID plant = IndustryID::Invalid();
		int64_t industry_id = 0;
		switch (ReadInt(params, "industry_id", industry_id)) {
			case ParamState::Invalid: return MakeError("invalid_arguments", "`industry_id` must be an integer");
			case ParamState::Present: {
				if (industry_id < 0 || industry_id >= static_cast<int64_t>(IndustryID::End().base())) {
					return MakeError("unknown_industry", "the industry id is out of range", {{"industry_id", industry_id}});
				}
				plant = IndustryID(static_cast<uint16_t>(industry_id));
				const Industry *i = Industry::GetIfValid(plant);
				if (i == nullptr) return MakeError("unknown_industry", "there is no industry with that id", {{"industry_id", industry_id}});
				if (!IsPowerPlant(i)) return MakeError("not_a_power_plant", "the industry is not a power plant", {{"industry_id", industry_id}});
				break;
			}
			case ParamState::Absent:
				break;
		}

		int64_t amount = 0;
		switch (ReadInt(params, "amount", amount)) {
			case ParamState::Invalid: return MakeError("invalid_arguments", "`amount` must be an integer");
			case ParamState::Absent: return MakeError("invalid_arguments", "`amount` is required");
			case ParamState::Present: break;
		}
		if (amount <= 0 || amount > UINT16_MAX) {
			return MakeError("invalid_amount", "the amount is out of range", {{"amount", amount}, {"min", 1}, {"max", UINT16_MAX}});
		}

		Json result = Json::object();
		result["industry_id"] = plant == IndustryID::Invalid() ? Json(nullptr) : Json(plant.base());
		result["amount"] = amount;
		const Backup<CompanyID> context(_current_company, OWNER_DEITY);
		const bool ok = RunCommand<Commands::PowerPlantAddFuel>(plant, static_cast<uint16_t>(amount));
		if (!_posted.captured) return AddNotExecuted(std::move(result));
		result = AddCommandResult(std::move(result));
		if (ok && plant != IndustryID::Invalid()) result["fuel_after"] = GetPowerPlantFuelStock(Industry::Get(plant));
		return result;
	}

	if (op == "station_town") {
		int64_t station_id = 0;
		switch (ReadInt(params, "station_id", station_id)) {
			case ParamState::Invalid: return MakeError("invalid_arguments", "`station_id` must be an integer");
			case ParamState::Absent: return MakeError("invalid_arguments", "`station_id` is required");
			case ParamState::Present: break;
		}
		if (station_id < 0 || station_id >= static_cast<int64_t>(StationID::End().base())) {
			return MakeError("unknown_station", "the station id is out of range", {{"station_id", station_id}});
		}
		const Station *st = Station::GetIfValid(StationID(static_cast<uint16_t>(station_id)));
		if (st == nullptr) return MakeError("unknown_station", "there is no station with that id", {{"station_id", station_id}});

		TownID town = TownID::Invalid();
		if (const auto error = ReadTownId(params, "town_id", town); error.has_value()) return *error;
		if (town == TownID::Invalid()) return MakeError("invalid_arguments", "`town_id` is required");
		if (const auto error = CheckTown(town); error.has_value()) return *error;

		/* The engine only accepts a town that is a candidate of the station; report the candidates. */
		Json candidates = Json::array();
		bool is_candidate = false;
		for (const Town *candidate : GetStationTownCandidates(st)) {
			candidates.push_back(candidate->index.base());
			if (candidate->index == town) is_candidate = true;
		}
		if (!is_candidate) {
			return MakeError("town_not_a_candidate", "the town is not one of the towns nearest to the station",
					{{"station_id", station_id}, {"town_id", town.base()}, {"candidates", std::move(candidates)}});
		}

		Json result = Json::object();
		result["station_id"] = station_id;
		result["town_id"] = town.base();
		result["town_before"] = st->town == nullptr ? Json(nullptr) : Json(st->town->index.base());
		const Backup<CompanyID> context(_current_company, OWNER_DEITY);
		const bool ok = RunCommand<Commands::SetStationTown>(st->index, town);
		if (!_posted.captured) return AddNotExecuted(std::move(result));
		result = AddCommandResult(std::move(result));
		if (ok) {
			const Station *after = Station::GetIfValid(st->index);
			result["town_after"] = (after == nullptr || after->town == nullptr) ? Json(nullptr) : Json(after->town->index.base());
		}
		return result;
	}

	if (op == "district") {
		/* The argument shape is checked before the world is touched. */
		TileIndex tile = INVALID_TILE;
		switch (ReadTile(params, "tile", tile)) {
			case ParamState::Invalid: return MakeError("invalid_arguments", "`tile` must be a `{x, y}` object inside the map");
			case ParamState::Absent: return MakeError("invalid_arguments", "`tile` is required");
			case ParamState::Present: break;
		}

		TownID town = TownID::Invalid();
		if (const auto error = ReadTownId(params, "town_id", town); error.has_value()) return *error;
		if (town == TownID::Invalid()) return MakeError("invalid_arguments", "`town_id` is required");
		if (const auto error = CheckTown(town); error.has_value()) return *error;

		const Town *t = Town::Get(town);
		const TownDistrict district = GetTownDistrict(t, tile);
		Json result = Json::object();
		result["ok"] = true;
		result["town_id"] = town.base();
		result["tile"] = Json{{"x", TileX(tile)}, {"y", TileY(tile)}};
		result["district"] = GetTownDistrictName(district);
		result["districts_from_population"] = TOWN_DISTRICTS_MIN_POPULATION;
		result["population"] = t->cache.population;
		return result;
	}

	return MakeGap("world.project", op);
}

/**
 * Build one capability entry.
 * @param method The method name.
 * @param authority "read" or "write".
 * @param implemented The operations that work.
 * @param gaps The operations that do not work yet.
 * @return The entry.
 */
Json Capability(std::string_view method, std::string_view authority, std::initializer_list<std::string_view> implemented, std::initializer_list<std::string_view> gaps)
{
	Json entry = Json::object();
	entry["method"] = std::string(method);
	entry["authority"] = std::string(authority);
	entry["implemented"] = Json::array();
	for (const std::string_view op : implemented) entry["implemented"].push_back(std::string(op));
	entry["gaps"] = Json::array();
	for (const std::string_view op : gaps) entry["gaps"].push_back(std::string(op));
	return entry;
}

} // anonymous namespace

void AgentAdminCommandCallback([[maybe_unused]] Commands cmd, const CommandCost &result, [[maybe_unused]] TileIndex tile)
{
	_posted.captured = true;
	_posted.cost = result;
}

Json AdminToolCapabilities()
{
	Json methods = Json::array();
	methods.push_back(Capability("world.settings.get", "read",
			{"game", "company", "client", "names", "prefix"},
			{"write"}));
	methods.push_back(Capability("world.settings.set", "write",
			{"game", "company", "bool", "int", "one_of_many", "many_of_many"},
			{"client_scope", "string_setting"}));
	methods.push_back(Capability("world.company", "write",
			{"info", "loan_increase", "loan_decrease", "set_max_loan", "create", "delete", "give_money"},
			{"buy", "colour", "manager_face", "rename_president"}));
	methods.push_back(Capability("world.towns", "write",
			{"needs", "force_need", "invest", "set_investment", "growth_rate", "expand", "cargo_goal", "rating", "destroy"},
			{"found", "town_action"}));
	methods.push_back(Capability("world.project", "write",
			{"power", "power_fuel", "station_town", "district"},
			{"save", "load"}));

	Json result = Json::object();
	result["module"] = "agent_admin_tools";
	result["methods"] = std::move(methods);
	result["unimplemented_methods"] = Json::array({"world.pause", "world.rename", "world.objects", "world.info", "world.capture", "task.barrier"});
	result["notes"] = Json::array({
		"Actual mutations go through Command<Commands::...>::Post, the normal player command path, so the engine's own eligibility, pause and finance checks apply; results carry the actual cost and the engine error.",
		"world.settings.get returns the setting descriptors with their current value, type, scope, allowed values or range, default and whether they can be changed now.",
		"world.settings.set validates the name, the scope, the type and the range before it posts Commands::ChangeSetting or Commands::ChangeCompanySetting, and reports the value the engine actually applied.",
		"world.company op=create uses Commands::CompanyControl with CompanyCtrlAction::NewAI, the only company creation the single player engine has; the created id is read back from the company pool.",
		"world.towns op=invest is paid by the company and reports the real cost; op=set_investment, op=force_need, op=growth_rate, op=expand, op=cargo_goal, op=rating and op=destroy are the free administrative commands and run as the deity.",
		"world.project op=station_town only accepts a town that is a candidate of the station and reports the candidates otherwise; op=district reports the district of one tile of a town.",
		"world.project op=save/load is not implemented: the durable save lifecycle is left to the bridge.",
	});

	return result;
}

bool IsAdminMutation(std::string_view method, const Json &params)
{
	if (method == "world.settings.set") return true;
	if (method != "world.company" && method != "world.towns" && method != "world.project") return false;

	std::string op;
	if (ReadString(params, "op", op) != ParamState::Present) return true;
	if (method == "world.company") return op != "info";
	if (method == "world.towns") return op != "needs";
	return op != "power" && op != "district";
}

std::optional<Json> DispatchAdminTool(std::string_view method, const Json &params, CompanyID company)
{
	if (method != "world.settings.get" && method != "world.settings.set" &&
			method != "world.company" && method != "world.towns" && method != "world.project") {
		return std::nullopt;
	}
	if (!params.is_object()) return MakeError("invalid_arguments", "params must be an object");

	if (method == "world.settings.get") return SettingsGet(params, company);
	if (method == "world.settings.set") return SettingsSet(params, company);
	if (method == "world.company") return CompanyOp(params, company);
	if (method == "world.towns") return TownsOp(params, company);
	return ProjectOp(params, company);
}

} // namespace InGameAgent
