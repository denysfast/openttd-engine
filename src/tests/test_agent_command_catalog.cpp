/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/gpl-2.0>.
 */

/** @file test_agent_command_catalog.cpp Test the engine command catalog and the generic argument decoding. */

#include "../stdafx.h"

#include "../3rdparty/catch2/catch.hpp"

#include "../agent_command_catalog.h"
#include "../command_func.h"
#include "../command_type.h"
#include "../company_type.h"

#include "../safeguards.h"

using namespace InGameAgent;

/** Build a JSON array of command arguments, the way the backend sends them. */
template <typename... Ts>
static Json Arguments(Ts... values)
{
	Json args = Json::array();
	(args.push_back(values), ...);
	return args;
}

/** Read the catalog as the bridge would, for a read that needs no company. */
static Json ReadCatalog(const Json &params)
{
	const std::optional<Json> result = DispatchCommandCatalog("world.commands", params, CompanyID::Invalid());
	REQUIRE(result.has_value());
	return *result;
}

/** Call world.command, which validates the arguments before it needs a company. */
static Json CallCommand(const Json &params)
{
	const std::optional<Json> result = DispatchCommandCatalog("world.command", params, CompanyID::Invalid());
	REQUIRE(result.has_value());
	return *result;
}

/** Find a catalog entry by command name. */
static const Json &FindEntry(const Json &commands, std::string_view name)
{
	for (const Json &entry : commands) {
		if (entry["name"].get<std::string>() == name) return entry;
	}
	FAIL("command " << name << " is missing from the catalog");
	return commands[0];
}

TEST_CASE("AgentCommandCatalogCoversEveryCommand")
{
	const Json catalog = ReadCatalog({{"op", "list"}});
	REQUIRE(catalog["ok"].get<bool>());
	CHECK(catalog["total"].get<size_t>() == to_underlying(Commands::End));
	CHECK(catalog["count"].get<size_t>() == to_underlying(Commands::End));

	/* Every engine command appears exactly once, with its own id, name and alias. */
	const Json &commands = catalog["commands"];
	REQUIRE(commands.size() == to_underlying(Commands::End));
	for (size_t i = 0; i < commands.size(); i++) {
		const Json &entry = commands[i];
		CHECK(entry["id"].get<size_t>() == i);
		CHECK_FALSE(entry["name"].get<std::string>().empty());
		CHECK_FALSE(entry["alias"].get<std::string>().empty());
		CHECK(entry.contains("flags"));
		CHECK(entry.contains("pause"));
		CHECK(entry.contains("company"));
		CHECK(entry["support"].contains("status"));
		/* The name the catalog reports is the engine's own name for that command. */
		CHECK(entry["name"].get<std::string>() == std::string(GetCommandName(static_cast<Commands>(i))));
	}

	/* The catalog must not claim more dispatchable commands than the engine has. */
	const size_t dispatchable = catalog["dispatchable"].get<size_t>();
	CHECK(dispatchable <= to_underlying(Commands::End));
	/* The generic decoder has to cover the plain commands, not just a handful. */
	CHECK(dispatchable > 100);
}

TEST_CASE("AgentCommandCatalogDescribesArguments")
{
	const Json catalog = ReadCatalog({{"op", "describe"}});
	const Json &commands = catalog["commands"];
	REQUIRE(commands.size() == to_underlying(Commands::End));

	/* A command whose single argument is a Money is decoded as a strong id. */
	const Json &cheat = FindEntry(commands, "CmdMoneyCheat");
	REQUIRE(cheat["arguments"].size() == 1);
	CHECK(cheat["arguments"][0]["type"].get<std::string>() == "Money");
	CHECK(cheat["arguments"][0]["kind"].get<std::string>() == "strong_id");
	CHECK(cheat["arguments"][0]["supported"].get<bool>());
	CHECK(cheat["support"]["status"].get<std::string>() == "dispatchable");
	CHECK(cheat["returns"].empty());

	/* A tile command keeps the engine's argument order and types. */
	const Json &road = FindEntry(commands, "CmdBuildRoad");
	REQUIRE(road["arguments"].size() == 5);
	CHECK(road["arguments"][0]["type"].get<std::string>() == "TileIndex");
	CHECK(road["arguments"][1]["kind"].get<std::string>() == "enum_bit_set");
	CHECK(road["arguments"][1]["type"].get<std::string>() == "EnumBitSet<RoadBit>");
	CHECK(road["arguments"][4]["type"].get<std::string>() == "TownID");
	CHECK_FALSE(road["location_argument"].get<bool>());
	CHECK(road["flags"].size() > 0);

	/* A command that takes its location outside its arguments says so. */
	const Json &town_action = FindEntry(commands, "CmdDoTownAction");
	CHECK(town_action["location_argument"].get<bool>());
	CHECK(town_action["type"].get<std::string>() == "LandscapeConstruction");

	/* Commands whose arguments are engine structs are catalogued as an explicit gap. */
	const Json &blueprint = FindEntry(commands, "CmdPasteBlueprint");
	CHECK_FALSE(blueprint["support"]["supported"].get<bool>());
	CHECK(blueprint["support"]["status"].get<std::string>() == "gap");
	CHECK(blueprint["support"]["reason"].get<std::string>().find("BlueprintChunkData") != std::string::npos);
	CHECK_FALSE(blueprint["arguments"][1]["supported"].get<bool>());

	/* Extra return values of a proc are reported, they are just not captured by the common callback. */
	const Json &build_vehicle = FindEntry(commands, "CmdBuildVehicle");
	REQUIRE(build_vehicle["returns"].size() == 4);
	CHECK(build_vehicle["returns"][0].get<std::string>() == "VehicleID");
}

TEST_CASE("AgentCommandCatalogDescribesOneCommand")
{
	const Json single = ReadCatalog({{"op", "describe"}, {"command", "CmdIncreaseLoan"}});
	REQUIRE(single["count"].get<size_t>() == 1);
	const Json &entry = single["commands"][0];
	CHECK(entry["name"].get<std::string>() == "CmdIncreaseLoan");
	CHECK(entry["alias"].get<std::string>() == "IncreaseLoan");
	CHECK(entry["type"].get<std::string>() == "MoneyManagement");
	REQUIRE(entry["arguments"].size() == 2);

	/* The alias and the numeric id name the same command. */
	const Json by_alias = ReadCatalog({{"op", "describe"}, {"command", "IncreaseLoan"}});
	CHECK(by_alias["commands"][0]["id"] == entry["id"]);
	const Json by_id = ReadCatalog({{"op", "describe"}, {"command", entry["id"].get<size_t>()}});
	CHECK(by_id["commands"][0]["name"] == entry["name"]);

	CHECK_FALSE(ReadCatalog({{"op", "describe"}, {"command", "CmdNoSuchCommand"}})["ok"].get<bool>());
	CHECK_FALSE(ReadCatalog({{"op", "nonsense"}})["ok"].get<bool>());
}

/** Call the read-only argument decoder of one command. */
static Json ValidateCommand(const char *command, const Json &args)
{
	return ReadCatalog({{"op", "validate"}, {"command", command}, {"args", args}});
}

TEST_CASE("AgentCommandCatalogDecodesArguments")
{
	/* Money is decoded through its base type, and reported back as the integer it holds. */
	const Json money = ValidateCommand("CmdMoneyCheat", Arguments(100));
	REQUIRE(money["ok"].get<bool>());
	CHECK(money["command"].get<std::string>() == "CmdMoneyCheat");
	CHECK(money["arguments"][0] == 100);

	/* A tile command: strong id, bitset, enum and another strong id, in the engine's order. */
	const Json road = ValidateCommand("CmdBuildRoad", Arguments(1000, 1, 0, 0, 0));
	REQUIRE(road["ok"].get<bool>());
	CHECK(road["arguments"].size() == 5);
	CHECK(road["arguments"][0] == 1000);
	CHECK(road["arguments"][1] == 1);

	/* A bitset can also be given as the bit indices of the enum values it holds. */
	const Json road_bits = ValidateCommand("CmdBuildRoad", Arguments(1000, Arguments(0, 1), 0, 0, 0));
	REQUIRE(road_bits["ok"].get<bool>());
	CHECK(road_bits["arguments"][1] == 3);

	/* Strings, and the alias of a command name. */
	const Json rename = ReadCatalog({{"op", "validate"}, {"command", "RenameCompany"}, {"args", Arguments("Test Co")}});
	REQUIRE(rename["ok"].get<bool>());
	CHECK(rename["arguments"][0].get<std::string>() == "Test Co");

	/* An enum and a boolean. */
	const Json pause = ValidateCommand("CmdPause", Arguments(1, true));
	REQUIRE(pause["ok"].get<bool>());
	CHECK(pause["arguments"][0] == 1);
	CHECK(pause["arguments"][1] == true);

	/* A command whose arguments are an engine struct is an explicit gap, not a silent success. */
	const Json blueprint = ValidateCommand("CmdPasteBlueprint", Arguments(1000, 0));
	CHECK_FALSE(blueprint["ok"].get<bool>());
	CHECK(blueprint["error"].get<std::string>() == "unsupported_argument_types");

	CHECK(ValidateCommand("CmdNoSuchCommand", Arguments())["error"].get<std::string>() == "unknown_command");
	CHECK(ReadCatalog({{"op", "validate"}})["error"].get<std::string>() == "invalid_params");
	CHECK(ReadCatalog({{"op", "validate"}, {"command", "CmdMoneyCheat"}, {"args", 5}})["error"].get<std::string>() == "invalid_params");
}

TEST_CASE("AgentCommandCatalogRejectsBadArgumentsBeforePosting")
{
	/* Wrong number of arguments. */
	const Json arity = ValidateCommand("CmdMoneyCheat", Arguments());
	CHECK(arity["error"].get<std::string>() == "invalid_arguments");
	CHECK(arity["message"].get<std::string>().find("argument(s)") != std::string::npos);

	/* Wrong type: a string where an enum is expected. */
	const Json wrong_type = ValidateCommand("CmdPause", Arguments("not-a-mode", true));
	CHECK(wrong_type["error"].get<std::string>() == "invalid_arguments");
	CHECK(wrong_type["message"].get<std::string>().find("argument 0") != std::string::npos);

	/* Wrong type: a number where a boolean is expected. */
	const Json wrong_bool = ValidateCommand("CmdPause", Arguments(1, 1));
	CHECK(wrong_bool["error"].get<std::string>() == "invalid_arguments");

	/* Overflow: the value does not fit the argument type. */
	const Json overflow = ValidateCommand("CmdSetCompanyManagerFace", Arguments(5000000000ULL, 0));
	CHECK(overflow["error"].get<std::string>() == "invalid_arguments");
	CHECK(overflow["message"].get<std::string>().find("does not fit") != std::string::npos);

	/* Overflow of a bitset: RoadBits is stored in one byte. */
	const Json bitset_overflow = ValidateCommand("CmdBuildRoad", Arguments(1000, 256, 0, 0, 0));
	CHECK(bitset_overflow["error"].get<std::string>() == "invalid_arguments");
	CHECK(bitset_overflow["message"].get<std::string>().find("argument 1") != std::string::npos);

	/* A bit index outside the enum of the bitset. */
	const Json bit_out_of_range = ValidateCommand("CmdBuildRoad", Arguments(1000, Arguments(0, 64), 0, 0, 0));
	CHECK(bit_out_of_range["error"].get<std::string>() == "invalid_arguments");

	/* A negative value for an unsigned argument. */
	const Json negative = ValidateCommand("CmdBuildRoad", Arguments(1000, 1, -1, 0, 0));
	CHECK(negative["error"].get<std::string>() == "invalid_arguments");

	/* A missing argument is an arity error, not a defaulted zero. */
	const Json partial = ValidateCommand("CmdBuildRoad", Arguments(1000));
	CHECK(partial["error"].get<std::string>() == "invalid_arguments");

	/* world.command itself: a valid call decodes and then needs a real company, which this call does not have. */
	const Json valid = CallCommand({{"command", "CmdMoneyCheat"}, {"args", Arguments(100)}});
	CHECK_FALSE(valid["ok"].get<bool>());
	CHECK(valid["error"].get<std::string>() == "invalid_company");

	CHECK(CallCommand({{"command", "CmdNoSuchCommand"}, {"args", Arguments()}})["error"].get<std::string>() == "unknown_command");
	CHECK(CallCommand({{"command", "CmdPasteBlueprint"}, {"args", Arguments(1000, 0)}})["error"].get<std::string>() == "unsupported_argument_types");
	CHECK(CallCommand({{"args", Arguments()}})["error"].get<std::string>() == "invalid_params");
	CHECK(CallCommand({{"command", "CmdMoneyCheat"}, {"args", 5}})["error"].get<std::string>() == "invalid_params");
	CHECK(CallCommand({{"command", "CmdMoneyCheat"}, {"args", Arguments(1)}, {"dry_run", 1}})["error"].get<std::string>() == "invalid_params");
	CHECK(CallCommand({{"command", "CmdMoneyCheat"}, {"args", Arguments(1)}, {"location", -1}})["error"].get<std::string>() == "invalid_params");
}

TEST_CASE("AgentCommandCatalogReportsTruthfulCapabilities")
{
	const Json caps = CommandCatalogCapabilities();
	REQUIRE(caps["ok"].get<bool>());

	/* The counts come from the engine's own command table. */
	CHECK(caps["commands"]["total"].get<size_t>() == to_underlying(Commands::End));
	CHECK(caps["commands"]["engine_end"].get<size_t>() == to_underlying(Commands::End));
	const size_t total = caps["commands"]["total"].get<size_t>();
	const size_t dispatchable = caps["commands"]["dispatchable"].get<size_t>();
	CHECK(caps["commands"]["gaps"].get<size_t>() == total - dispatchable);

	/* Every gap is named, and the gap commands really are the undecodable ones. */
	const Json &complex = caps["gaps"][2];
	REQUIRE(complex["gap"].get<std::string>() == "complex_argument_types");
	CHECK(complex["commands"].size() == total - dispatchable);
	for (const Json &gap : complex["commands"]) {
		CHECK_FALSE(gap["reason"].get<std::string>().empty());
	}

	/* The catalog lists the methods it answers. */
	bool has_list = false;
	bool has_write = false;
	for (const Json &method : caps["methods"]) {
		if (method["method"] == "world.commands" && method["op"] == "list") has_list = true;
		if (method["method"] == "world.command") has_write = true;
	}
	CHECK(has_list);
	CHECK(has_write);

	/* Other methods are not ours. */
	CHECK_FALSE(DispatchCommandCatalog("world.info", Json::object(), CompanyID::Invalid()).has_value());
}
