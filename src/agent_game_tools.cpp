/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/>.
 */

/** @file agent_game_tools.cpp Dispatch of the agent game tool methods onto real OpenTTD commands. */

#include "stdafx.h"
#include "agent_game_tools.h"

#include "command_func.h"
#include "company_base.h"
#include "depot_base.h"
#include "direction_type.h"
#include "engine_base.h"
#include "group_cmd.h"
#include "landscape_cmd.h"
#include "map_func.h"
#include "network/network.h"
#include "order_base.h"
#include "order_cmd.h"
#include "order_type.h"
#include "road_cmd.h"
#include "road_type.h"
#include "station_base.h"
#include "station_cmd.h"
#include "station_type.h"
#include "strings_func.h"
#include "vehicle_base.h"
#include "vehicle_cmd.h"
#include "vehiclelist.h"

#include <vector>

#include "table/strings.h"

#include "safeguards.h"

namespace InGameAgent {

namespace {

/**
 * What the engine reported for the command that was posted last.
 *
 * The command callbacks fill this; in single player they run synchronously inside `Post`, so the
 * values belong to the command that was just issued.
 */
struct PostedCommand {
	bool captured = false; ///< The engine reported a result, so the command really ran.
	CommandCost cost; ///< Actual cost, or the failure of the command.
	VehicleID vehicle = VehicleID::Invalid(); ///< Vehicle created by `Commands::BuildVehicle`.
	GroupID group = GroupID::Invalid(); ///< Group a vehicle ended up in.
};

/** Capture slot of the command that is currently being posted. */
PostedCommand _posted;

} // namespace

void AgentCommandCallback(Commands, const CommandCost &result, TileIndex)
{
	_posted.captured = true;
	_posted.cost = result;
}

void AgentBuildVehicleCallback(Commands, const CommandCost &result, VehicleID veh_id, uint, uint16_t, CargoArray)
{
	_posted.captured = true;
	_posted.cost = result;
	_posted.vehicle = veh_id;
}

void AgentAddVehicleToGroupCallback(Commands, const CommandCost &result, GroupID group_id)
{
	_posted.captured = true;
	_posted.cost = result;
	_posted.group = group_id;
}

namespace {

/* -- result helpers ---------------------------------------------------------------------- */

/**
 * Build a failed tool result.
 * @param code Machine readable error code, e.g. `invalid_arguments`.
 * @param message Human readable reason.
 * @param detail Optional structured detail.
 * @return Tool result with `ok` set to false.
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
 * Report an op or method this module does not implement yet. A gap is never a fake success.
 * @param method Engine method name.
 * @param op Op or kind that is missing, empty for a whole method.
 * @return Tool result with `ok` set to false and error `unsupported_op` (or `unsupported_method`
 *         for a whole method, which is the code the protocol documents for that case).
 */
Json MakeGap(std::string_view method, std::string_view op)
{
	Json detail = Json::object();
	detail["method"] = std::string(method);
	if (!op.empty()) detail["op"] = std::string(op);
	const std::string_view code = op.empty() ? "unsupported_method" : "unsupported_op";
	return MakeError(code, "not implemented by the game tool module yet", std::move(detail));
}

/**
 * Merge the captured command result into a tool result: the cost, and the engine's own error.
 * @param posted Result of the command that was actually executed.
 * @param result Tool result to complete.
 * @return The completed tool result.
 */
Json CommandResult(const PostedCommand &posted, Json result)
{
	result["cost"] = static_cast<int64_t>(posted.cost.GetCost());
	if (posted.cost.Failed()) {
		result["ok"] = false;
		result["error"] = "command_failed";
		result["message_id"] = posted.cost.GetErrorMessage().base();
		if (posted.cost.GetErrorMessage() != INVALID_STRING_ID) result["message"] = GetString(posted.cost.GetErrorMessage());
	} else {
		result["ok"] = true;
	}
	return result;
}

/**
 * Complete a result of a batch that stopped after some items were already applied.
 * @param result Tool result holding the items that were applied.
 * @param spent Money the applied items actually cost.
 * @param posted Result of the command that failed.
 * @return The completed tool result, with the partial receipts and both costs.
 */
Json PartialResult(Json result, Money spent, const PostedCommand &posted)
{
	result["partial"] = true;
	result = CommandResult(posted, std::move(result));
	result["cost"] = static_cast<int64_t>(spent);
	result["failed_cost"] = static_cast<int64_t>(posted.cost.GetCost());
	return result;
}

/**
 * Complete a result whose command never ran: the engine only estimated it, or rejected it before
 * dispatch. Nothing was mutated, so the caller must not treat this as a success.
 * @param result Tool result to complete.
 * @return The completed tool result.
 */
Json NotExecuted(Json result)
{
	result["ok"] = false;
	result["error"] = "command_not_executed";
	result["message"] = "the engine did not execute the command (rejected before dispatch, or only estimated)";
	result["executed"] = false;
	return result;
}

/**
 * Mark whether a result holds an actual execution or a cost estimate.
 * @param result Tool result to complete.
 * @param dry_run True when the result is a `dry_run` estimate.
 * @return The completed tool result.
 */
Json MarkExecution(Json result, bool dry_run)
{
	result["estimated"] = dry_run;
	if (dry_run) result["dry_run"] = true;
	return result;
}

/* -- command helpers --------------------------------------------------------------------- */

/**
 * Run one command and capture what the engine did.
 *
 * An actual run goes through `Command<Tcmd>::Post`, the normal player command path, so the
 * engine's own eligibility, pause and finance checks apply and the money is really deducted. A
 * dry run only queries the cost and mutates nothing.
 *
 * @param dry_run True to only query the cost.
 * @param callback Callback that captures the result; must be registered for network dispatch.
 * @param args Command arguments.
 * @return True when the command succeeded (or the estimate is valid).
 */
template <Commands Tcmd, typename Tcallback, typename... Targs>
bool RunCommand(bool dry_run, Tcallback *callback, Targs... args)
{
	_posted = PostedCommand{};
	if (dry_run) {
		auto estimate = Command<Tcmd>::Do(DoCommandFlags{DoCommandFlag::QueryCost}, args...);
		_posted.captured = true;
		_posted.cost = ExtractCommandCost(estimate);
	} else {
		Command<Tcmd>::Post(STR_NULL, callback, std::forward<Targs>(args)...);
	}
	return _posted.captured && _posted.cost.Succeeded();
}

/**
 * Run one command that has no tile of its own.
 *
 * `Command<Tcmd>::Post` needs an explicit location for those commands; it is only used for the
 * error window and the text effects, never as a command argument.
 *
 * @param dry_run True to only query the cost.
 * @param location Tile used for user feedback.
 * @param callback Callback that captures the result; must be registered for network dispatch.
 * @param args Command arguments.
 * @return True when the command succeeded (or the estimate is valid).
 */
template <Commands Tcmd, typename Tcallback, typename... Targs>
bool RunCommandAt(bool dry_run, TileIndex location, Tcallback *callback, Targs... args)
{
	_posted = PostedCommand{};
	if (dry_run) {
		auto estimate = Command<Tcmd>::Do(DoCommandFlags{DoCommandFlag::QueryCost}, args...);
		_posted.captured = true;
		_posted.cost = ExtractCommandCost(estimate);
	} else {
		Command<Tcmd>::Post(STR_NULL, callback, location, std::forward<Targs>(args)...);
	}
	return _posted.captured && _posted.cost.Succeeded();
}

/**
 * Switch the company that pays and owns the result of a command, and restore the previous
 * company when the call returns. The caller validates the company first.
 */
class CompanyContext {
public:
	explicit CompanyContext(CompanyID company) : backup(_current_company, company) {}

private:
	Backup<CompanyID> backup; ///< Restores the previous company on destruction.
};

/**
 * Validate the company a write method acts for.
 * @param company The company from the call.
 * @return Error result, or an empty optional when the company is usable.
 */
std::optional<Json> CheckCompany(CompanyID company)
{
	if (Company::IsValidID(company)) return std::nullopt;
	Json detail = Json::object();
	detail["company_id"] = company.base();
	return MakeError("invalid_company", "company does not exist", std::move(detail));
}

/**
 * Reject a write method outside single player.
 *
 * `Command<Tcmd>::Post` is the player command path: in a network game it only queues the command
 * for the server, so the callback would not report what the engine did. The bridge runs in single
 * player; anything else is refused instead of being executed behind the server's back.
 * @return Error result, or an empty optional when the command path is usable.
 */
std::optional<Json> CheckSinglePlayer()
{
	if (!_networking) return std::nullopt;
	return MakeError("multiplayer_unsupported", "game tool commands run through the single-player command path only");
}

/**
 * Check everything a write method needs before it touches the world.
 * @param company Company that pays and owns the result.
 * @param dry_run True when the call only estimates the cost.
 * @return Error result, or an empty optional when the call may run.
 */
std::optional<Json> CheckWriteContext(CompanyID company, bool dry_run)
{
	if (const std::optional<Json> error = CheckCompany(company); error.has_value()) return error;
	if (dry_run) return std::nullopt;
	return CheckSinglePlayer();
}

/* -- parameter helpers ------------------------------------------------------------------- */

/**
 * Read a required integer parameter.
 * @param params Method parameters.
 * @param key Parameter name.
 * @param value Receives the value when present.
 * @return True when the parameter is present and an integer.
 */
bool ReadInt(const Json &params, const char *key, int64_t &value)
{
	const auto it = params.find(key);
	if (it == params.end() || !it->is_number_integer()) return false;
	value = it->get<int64_t>();
	return true;
}

/**
 * Read an optional integer parameter.
 * @param params Method parameters.
 * @param key Parameter name.
 * @param value Receives the value when present.
 * @return False when the parameter is present but not an integer.
 */
bool ReadOptionalInt(const Json &params, const char *key, int64_t &value)
{
	const auto it = params.find(key);
	if (it == params.end()) return true;
	if (!it->is_number_integer()) return false;
	value = it->get<int64_t>();
	return true;
}

/**
 * Read a required string parameter.
 * @param params Method parameters.
 * @param key Parameter name.
 * @param value Receives the value when present.
 * @return True when the parameter is present and a string.
 */
bool ReadString(const Json &params, const char *key, std::string &value)
{
	const auto it = params.find(key);
	if (it == params.end() || !it->is_string()) return false;
	value = it->get<std::string>();
	return true;
}

/**
 * Read an optional string parameter.
 * @param params Method parameters.
 * @param key Parameter name.
 * @param value Receives the value when present.
 * @return False when the parameter is present but not a string.
 */
bool ReadOptionalString(const Json &params, const char *key, std::string &value)
{
	const auto it = params.find(key);
	if (it == params.end()) return true;
	if (!it->is_string()) return false;
	value = it->get<std::string>();
	return true;
}

/**
 * Read an optional boolean parameter.
 * @param params Method parameters.
 * @param key Parameter name.
 * @param value Receives the value when present.
 * @return False when the parameter is present but not a boolean.
 */
bool ReadOptionalBool(const Json &params, const char *key, bool &value)
{
	const auto it = params.find(key);
	if (it == params.end()) return true;
	if (!it->is_boolean()) return false;
	value = it->get<bool>();
	return true;
}

/** A tile as sent by the backend, before it is checked against the map. */
struct RawTile {
	int64_t x; ///< X coordinate on the map.
	int64_t y; ///< Y coordinate on the map.
};

/**
 * Read a `{x, y}` tile object without checking it against the map.
 * @param value JSON value holding the tile.
 * @param tile Receives the coordinates.
 * @return True when the value is a tile object with integer coordinates.
 */
bool ReadRawTile(const Json &value, RawTile &tile)
{
	if (!value.is_object()) return false;
	const auto x = value.find("x");
	const auto y = value.find("y");
	if (x == value.end() || y == value.end() || !x->is_number_integer() || !y->is_number_integer()) return false;
	tile.x = x->get<int64_t>();
	tile.y = y->get<int64_t>();
	return true;
}

/**
 * Turn tile coordinates into a tile index, checking them against the map.
 * @param raw The coordinates.
 * @param tile Receives the tile index.
 * @return True when the tile is inside the map.
 */
bool ToTileIndex(const RawTile &raw, TileIndex &tile)
{
	if (raw.x < 0 || raw.y < 0 || raw.x >= static_cast<int64_t>(Map::SizeX()) || raw.y >= static_cast<int64_t>(Map::SizeY())) return false;
	tile = TileXY(static_cast<uint>(raw.x), static_cast<uint>(raw.y));
	return true;
}

/**
 * Serialise a tile index as `{x, y}`.
 * @param tile The tile.
 * @return JSON object with the map coordinates.
 */
Json TileJson(TileIndex tile)
{
	Json result = Json::object();
	result["x"] = TileX(tile);
	result["y"] = TileY(tile);
	return result;
}

/**
 * Read the `tiles` array of a method that handles exactly one tile.
 * @param params Method parameters.
 * @param what Name of the op, used in the error message.
 * @param tile Receives the single tile.
 * @return Error result, or an empty optional when exactly one tile was given.
 */
std::optional<Json> ReadSingleTile(const Json &params, std::string_view what, RawTile &tile)
{
	const auto tiles = params.find("tiles");
	if (tiles == params.end() || !tiles->is_array() || tiles->empty()) {
		return MakeError("invalid_arguments", std::string(what) + " needs `tiles` or `tile`");
	}
	if (tiles->size() > 1) {
		Json detail = Json::object();
		detail["count"] = tiles->size();
		detail["supported"] = 1;
		return MakeError("unsupported_multi_item", std::string(what) + " handles one tile per call; pass a single tile", std::move(detail));
	}
	if (!ReadRawTile((*tiles)[0], tile)) return MakeError("invalid_arguments", "tiles must be tile objects with integer x and y");
	return std::nullopt;
}

/**
 * Map a vehicle type onto the protocol name.
 * @param type The vehicle type.
 * @return Protocol name of the type.
 */
std::string_view VehicleTypeName(VehicleType type)
{
	switch (type) {
		case VehicleType::Train: return "train";
		case VehicleType::Road: return "road";
		case VehicleType::Ship: return "ship";
		case VehicleType::Aircraft: return "aircraft";
		default: return "other";
	}
}

/**
 * Map a road type name onto the road type.
 * @param params Method parameters.
 * @param rt Default road type, replaced when `road_type` is given.
 * @return False when `road_type` is not a string or names a type this module does not map.
 */
bool ReadRoadType(const Json &params, RoadType &rt)
{
	std::string name;
	if (!ReadOptionalString(params, "road_type", name)) return false;
	if (name.empty()) return true;
	if (name == "road") {
		rt = ROADTYPE_ROAD;
		return true;
	}
	if (name == "tram") {
		rt = ROADTYPE_TRAM;
		return true;
	}
	return false;
}

/**
 * Map a diagonal direction name onto the direction.
 * @param params Method parameters.
 * @param dir Receives the direction; defaults to north-east.
 * @return False when `direction` is not a string or names a direction this module does not map.
 */
bool ReadDiagDirection(const Json &params, DiagDirection &dir)
{
	std::string name;
	if (!ReadOptionalString(params, "direction", name)) return false;
	if (name.empty()) {
		dir = DiagDirection::NE;
		return true;
	}
	if (name == "ne") {
		dir = DiagDirection::NE;
		return true;
	}
	if (name == "se") {
		dir = DiagDirection::SE;
		return true;
	}
	if (name == "sw") {
		dir = DiagDirection::SW;
		return true;
	}
	if (name == "nw") {
		dir = DiagDirection::NW;
		return true;
	}
	return false;
}

/* -- read-only inventory ----------------------------------------------------------------- */

/**
 * Serialise one vehicle.
 * @param v The vehicle.
 * @return JSON row with the current state of the vehicle.
 */
Json VehicleRow(const Vehicle *v)
{
	Json row = Json::object();
	row["id"] = v->index.base();
	row["type"] = std::string(VehicleTypeName(v->type));
	row["owner"] = v->owner.base();
	row["name"] = v->name;
	row["engine_id"] = v->engine_type.base();
	row["tile"] = TileJson(v->tile);
	row["stopped"] = v->vehstatus.Test(VehState::Stopped);
	row["speed"] = v->cur_speed;
	row["profit_last_year"] = static_cast<int64_t>(v->GetDisplayProfitLastYear());
	return row;
}

/**
 * Serialise one engine.
 * @param e The engine.
 * @return JSON row with the purchase data of the engine.
 */
Json EngineRow(const Engine *e)
{
	Json row = Json::object();
	row["id"] = e->index.base();
	row["type"] = std::string(VehicleTypeName(e->type));
	row["name"] = e->name;
	row["cost"] = static_cast<int64_t>(e->GetCost());
	row["running_cost"] = static_cast<int64_t>(e->GetRunningCost());
	row["max_speed"] = e->GetDisplayMaxSpeed();
	row["cargo_id"] = static_cast<uint8_t>(e->GetDefaultCargoType());
	return row;
}

/**
 * Read the vehicle inventory.
 * @param params Method parameters: `limit`, `offset`, `company_id`.
 * @param company Company of the call, used when `company_id` is absent.
 * @return Tool result with the matching rows and the total count.
 */
Json QueryVehicles(const Json &params, CompanyID company)
{
	int64_t limit = 100;
	int64_t offset = 0;
	int64_t company_id = 0;
	if (!ReadOptionalInt(params, "limit", limit)) return MakeError("invalid_arguments", "limit must be an integer");
	if (!ReadOptionalInt(params, "offset", offset)) return MakeError("invalid_arguments", "offset must be an integer");
	if (limit < 1) limit = 1;
	if (offset < 0) offset = 0;

	CompanyID filter = company;
	if (params.contains("company_id")) {
		if (!ReadOptionalInt(params, "company_id", company_id)) return MakeError("invalid_arguments", "company_id must be an integer");
		filter = CompanyID(static_cast<uint8_t>(company_id));
	}

	Json rows = Json::array();
	int64_t total = 0;
	for (const Vehicle *v : Vehicle::Iterate()) {
		if (filter != CompanyID::Invalid() && v->owner != filter) continue;
		if (total++ < offset) continue;
		if (static_cast<int64_t>(rows.size()) >= limit) continue;
		rows.push_back(VehicleRow(v));
	}

	Json result = Json::object();
	result["ok"] = true;
	result["kind"] = "vehicles";
	result["rows"] = std::move(rows);
	result["total"] = total;
	return result;
}

/**
 * Read the engine inventory.
 * @param params Method parameters: `limit`, `offset`, `vehicle_type`.
 * @return Tool result with the matching rows and the total count.
 */
Json QueryEngines(const Json &params)
{
	int64_t limit = 100;
	int64_t offset = 0;
	if (!ReadOptionalInt(params, "limit", limit)) return MakeError("invalid_arguments", "limit must be an integer");
	if (!ReadOptionalInt(params, "offset", offset)) return MakeError("invalid_arguments", "offset must be an integer");
	if (limit < 1) limit = 1;
	if (offset < 0) offset = 0;

	std::string type_filter;
	if (!ReadOptionalString(params, "vehicle_type", type_filter)) return MakeError("invalid_arguments", "vehicle_type must be a string");

	Json rows = Json::array();
	int64_t total = 0;
	for (const Engine *e : Engine::Iterate()) {
		if (!type_filter.empty() && VehicleTypeName(e->type) != type_filter) continue;
		if (total++ < offset) continue;
		if (static_cast<int64_t>(rows.size()) >= limit) continue;
		rows.push_back(EngineRow(e));
	}

	Json result = Json::object();
	result["ok"] = true;
	result["kind"] = "engines";
	result["rows"] = std::move(rows);
	result["total"] = total;
	return result;
}

/* -- construction ------------------------------------------------------------------------ */

/**
 * Build or remove road/tram on the given tiles, or along a straight run.
 * @param params Method parameters.
 * @param company Company that pays.
 * @param rt Road type to build.
 * @param dry_run True to only query the cost.
 * @return Tool result with the tiles that were handled and the cost.
 */
Json ConstructRoad(const Json &params, CompanyID company, RoadType rt, bool dry_run)
{
	const std::string op_name = rt == ROADTYPE_TRAM ? "tram" : "road";

	/* Validate the shape of the request before anything is looked up or mutated. */
	std::vector<RawTile> raw_tiles;
	RawTile from{};
	RawTile to{};
	bool use_run = false;
	if (params.contains("from") || params.contains("to")) {
		if (!params.contains("from") || !params.contains("to")) return MakeError("invalid_arguments", "road needs both `from` and `to`");
		if (!ReadRawTile(params["from"], from) || !ReadRawTile(params["to"], to)) {
			return MakeError("invalid_arguments", "from/to must be tile objects with integer x and y");
		}
		if (from.x != to.x && from.y != to.y) return MakeError("invalid_arguments", "from/to must share a row or a column");
		use_run = true;
	} else {
		const auto tiles = params.find("tiles");
		if (tiles == params.end() || !tiles->is_array() || tiles->empty()) {
			return MakeError("invalid_arguments", "road needs `tiles` or `from`/`to`");
		}
		for (const Json &entry : *tiles) {
			RawTile raw{};
			if (!ReadRawTile(entry, raw)) return MakeError("invalid_arguments", "tiles must be tile objects with integer x and y");
			raw_tiles.push_back(raw);
		}
	}

	if (const std::optional<Json> error = CheckWriteContext(company, dry_run); error.has_value()) return *error;

	TileIndex from_tile = INVALID_TILE;
	TileIndex to_tile = INVALID_TILE;
	std::vector<TileIndex> tiles;
	if (use_run) {
		if (!ToTileIndex(from, from_tile) || !ToTileIndex(to, to_tile)) return MakeError("invalid_arguments", "from/to must be tiles inside the map");
	} else {
		for (const RawTile &raw : raw_tiles) {
			TileIndex tile;
			if (!ToTileIndex(raw, tile)) return MakeError("invalid_arguments", "tiles must be tiles inside the map");
			tiles.push_back(tile);
		}
	}

	CompanyContext context(company);

	if (use_run) {
		Axis axis;
		if (TileX(from_tile) == TileX(to_tile)) {
			axis = Axis::Y;
		} else {
			axis = Axis::X;
		}

		Json result = Json::object();
		result["op"] = op_name;
		result["from"] = TileJson(from_tile);
		result["to"] = TileJson(to_tile);
		if (!RunCommand<Commands::BuildRoadLong>(dry_run, &AgentCommandCallback, to_tile, from_tile, rt, axis, DisallowedRoadDirections{}, false, false, false)) {
			if (!_posted.captured) return NotExecuted(std::move(result));
		}
		return MarkExecution(CommandResult(_posted, std::move(result)), dry_run);
	}

	Money spent = 0;
	Json built = Json::array();
	for (const TileIndex &tile : tiles) {
		if (!RunCommand<Commands::BuildRoad>(dry_run, &AgentCommandCallback, tile, ROAD_ALL, rt, DisallowedRoadDirections{}, TownID::Invalid())) {
			Json result = Json::object();
			result["op"] = op_name;
			result["built"] = std::move(built);
			if (!_posted.captured) return NotExecuted(std::move(result));
			return MarkExecution(PartialResult(std::move(result), spent, _posted), dry_run);
		}
		spent += _posted.cost.GetCost();
		built.push_back(TileJson(tile));
	}

	Json result = Json::object();
	result["ok"] = true;
	result["op"] = op_name;
	result["built"] = std::move(built);
	result["cost"] = static_cast<int64_t>(spent);
	return MarkExecution(std::move(result), dry_run);
}

/**
 * Build a road stop (bus or truck station).
 * @param params Method parameters.
 * @param company Company that pays.
 * @param rt Road type of the stop.
 * @param dry_run True to only query the cost.
 * @return Tool result with the stop area, the created station and the cost.
 */
Json ConstructRoadStop(const Json &params, CompanyID company, RoadType rt, bool dry_run)
{
	RoadStopType stop_type = RoadStopType::Bus;
	std::string stop_name;
	if (!ReadOptionalString(params, "stop_type", stop_name)) return MakeError("invalid_arguments", "stop_type must be a string");
	if (!stop_name.empty()) {
		if (stop_name == "bus") {
			stop_type = RoadStopType::Bus;
		} else if (stop_name == "truck") {
			stop_type = RoadStopType::Truck;
		} else {
			return MakeError("invalid_arguments", "stop_type must be `bus` or `truck`");
		}
	}

	bool drive_through = false;
	if (!ReadOptionalBool(params, "drive_through", drive_through)) return MakeError("invalid_arguments", "drive_through must be a boolean");
	bool adjacent = false;
	if (!ReadOptionalBool(params, "adjacent", adjacent)) return MakeError("invalid_arguments", "adjacent must be a boolean");

	DiagDirection dir;
	if (!ReadDiagDirection(params, dir)) return MakeError("invalid_arguments", "direction must be one of ne, se, sw, nw");

	/* The station area: one rectangle, given as a single tile or as two opposite corners. */
	RawTile corner{};
	int64_t width = 1;
	int64_t length = 1;
	if (params.contains("from") || params.contains("to")) {
		RawTile from{};
		RawTile to{};
		if (!params.contains("from") || !params.contains("to")) return MakeError("invalid_arguments", "station needs both `from` and `to`");
		if (!ReadRawTile(params["from"], from) || !ReadRawTile(params["to"], to)) {
			return MakeError("invalid_arguments", "from/to must be tile objects with integer x and y");
		}
		width = (from.x > to.x ? from.x - to.x : to.x - from.x) + 1;
		length = (from.y > to.y ? from.y - to.y : to.y - from.y) + 1;
		corner.x = from.x < to.x ? from.x : to.x;
		corner.y = from.y < to.y ? from.y : to.y;
	} else {
		const std::optional<Json> error = ReadSingleTile(params, "station", corner);
		if (error.has_value()) return *error;
	}

	/* The command takes the span as uint8_t; a larger rectangle would be silently truncated. */
	if (width > 255 || length > 255) {
		Json detail = Json::object();
		detail["width"] = width;
		detail["length"] = length;
		detail["max"] = 255;
		return MakeError("invalid_arguments", "station rectangle spans more than 255 tiles", std::move(detail));
	}

	if (const std::optional<Json> error = CheckWriteContext(company, dry_run); error.has_value()) return *error;

	TileIndex tile;
	if (!ToTileIndex(corner, tile)) return MakeError("invalid_arguments", "station tile must be inside the map");

	CompanyContext context(company);

	Json result = Json::object();
	result["op"] = "station";
	result["tile"] = TileJson(tile);
	result["width"] = width;
	result["length"] = length;
	if (!RunCommand<Commands::BuildRoadStop>(dry_run, &AgentCommandCallback, tile, static_cast<uint8_t>(width), static_cast<uint8_t>(length), stop_type, drive_through, dir, rt, ROADSTOP_CLASS_DFLT, 0, StationID::Invalid(), adjacent)) {
		if (!_posted.captured) return NotExecuted(std::move(result));
		return MarkExecution(CommandResult(_posted, std::move(result)), dry_run);
	}

	if (!dry_run) {
		const Station *st = Station::GetByTile(tile);
		if (st != nullptr) result["station_id"] = st->index.base();
	}
	return MarkExecution(CommandResult(_posted, std::move(result)), dry_run);
}

/**
 * Build a road depot.
 * @param params Method parameters.
 * @param company Company that pays.
 * @param rt Road type of the depot.
 * @param dry_run True to only query the cost.
 * @return Tool result with the depot tile, the created depot and the cost.
 */
Json ConstructDepot(const Json &params, CompanyID company, RoadType rt, bool dry_run)
{
	RawTile raw{};
	if (params.contains("tile")) {
		if (!ReadRawTile(params["tile"], raw)) return MakeError("invalid_arguments", "tile must be a tile object with integer x and y");
	} else {
		const std::optional<Json> error = ReadSingleTile(params, "depot", raw);
		if (error.has_value()) return *error;
	}

	DiagDirection dir;
	if (!ReadDiagDirection(params, dir)) return MakeError("invalid_arguments", "direction must be one of ne, se, sw, nw");

	if (const std::optional<Json> error = CheckWriteContext(company, dry_run); error.has_value()) return *error;

	TileIndex tile;
	if (!ToTileIndex(raw, tile)) return MakeError("invalid_arguments", "depot tile must be inside the map");

	CompanyContext context(company);

	Json result = Json::object();
	result["op"] = "depot";
	result["tile"] = TileJson(tile);
	if (!RunCommand<Commands::BuildRoadDepot>(dry_run, &AgentCommandCallback, tile, rt, dir)) {
		if (!_posted.captured) return NotExecuted(std::move(result));
		return MarkExecution(CommandResult(_posted, std::move(result)), dry_run);
	}

	if (!dry_run) {
		const Depot *d = Depot::GetByTile(tile);
		if (d != nullptr) result["depot_id"] = d->index.base();
	}
	return MarkExecution(CommandResult(_posted, std::move(result)), dry_run);
}

/**
 * Clear the given tiles.
 * @param params Method parameters.
 * @param company Company that pays.
 * @param dry_run True to only query the cost.
 * @return Tool result with the cleared tiles and the cost.
 */
Json DemolishTiles(const Json &params, CompanyID company, bool dry_run)
{
	const auto tiles = params.find("tiles");
	if (tiles == params.end() || !tiles->is_array() || tiles->empty()) {
		return MakeError("invalid_arguments", "demolish needs `tiles`");
	}

	std::vector<RawTile> raw_tiles;
	for (const Json &entry : *tiles) {
		RawTile raw{};
		if (!ReadRawTile(entry, raw)) return MakeError("invalid_arguments", "tiles must be tile objects with integer x and y");
		raw_tiles.push_back(raw);
	}

	if (const std::optional<Json> error = CheckWriteContext(company, dry_run); error.has_value()) return *error;

	std::vector<TileIndex> targets;
	for (const RawTile &raw : raw_tiles) {
		TileIndex tile;
		if (!ToTileIndex(raw, tile)) return MakeError("invalid_arguments", "tiles must be tiles inside the map");
		targets.push_back(tile);
	}

	CompanyContext context(company);

	Money spent = 0;
	Json cleared = Json::array();
	for (const TileIndex &tile : targets) {
		if (!RunCommand<Commands::LandscapeClear>(dry_run, &AgentCommandCallback, tile)) {
			Json result = Json::object();
			result["op"] = "demolish";
			result["cleared"] = std::move(cleared);
			if (!_posted.captured) return NotExecuted(std::move(result));
			return MarkExecution(PartialResult(std::move(result), spent, _posted), dry_run);
		}
		spent += _posted.cost.GetCost();
		cleared.push_back(TileJson(tile));
	}

	Json result = Json::object();
	result["ok"] = true;
	result["op"] = "demolish";
	result["cleared"] = std::move(cleared);
	result["cost"] = static_cast<int64_t>(spent);
	return MarkExecution(std::move(result), dry_run);
}

/* -- vehicles ---------------------------------------------------------------------------- */

/**
 * Build vehicles in a depot.
 *
 * `cargo_id`, `name` and `share_orders_from` are honoured through real commands: the cargo is
 * passed to the build, the name is set with `Commands::RenameVehicle` and the orders are shared
 * with `Commands::CloneOrder`. Everything that can be rejected is rejected before the first
 * vehicle is created.
 *
 * @param params Method parameters.
 * @param company Company that pays and owns the vehicles.
 * @return Tool result with the created vehicle ids and the cost.
 */
Json VehicleBuild(const Json &params, CompanyID company)
{
	int64_t engine_id = 0;
	if (!ReadInt(params, "engine_id", engine_id)) return MakeError("invalid_arguments", "build needs engine_id");

	int64_t count = 1;
	if (!ReadOptionalInt(params, "count", count)) return MakeError("invalid_arguments", "count must be an integer");
	if (count < 1 || count > 100) {
		Json detail = Json::object();
		detail["count"] = count;
		detail["min"] = 1;
		detail["max"] = 100;
		return MakeError("invalid_arguments", "count must be between 1 and 100", std::move(detail));
	}

	CargoType cargo = INVALID_CARGO;
	int64_t cargo_id = 0;
	if (params.contains("cargo_id")) {
		if (!ReadOptionalInt(params, "cargo_id", cargo_id)) return MakeError("invalid_arguments", "cargo_id must be an integer");
		if (cargo_id < 0 || cargo_id >= static_cast<int64_t>(NUM_CARGO)) {
			Json detail = Json::object();
			detail["cargo_id"] = cargo_id;
			detail["max"] = NUM_CARGO - 1;
			return MakeError("invalid_arguments", "cargo_id is not a cargo", std::move(detail));
		}
		cargo = static_cast<CargoType>(static_cast<uint8_t>(cargo_id));
	}

	std::string name;
	const bool rename = params.contains("name");
	if (rename) {
		if (!ReadOptionalString(params, "name", name)) return MakeError("invalid_arguments", "name must be a string");
		if (name.empty() || name.size() > 128) {
			Json detail = Json::object();
			detail["length"] = name.size();
			detail["max"] = 128;
			return MakeError("invalid_arguments", "name must be 1 to 128 bytes", std::move(detail));
		}
	}

	int64_t share_from = 0;
	const bool share = params.contains("share_orders_from");
	if (share && !ReadOptionalInt(params, "share_orders_from", share_from)) {
		return MakeError("invalid_arguments", "share_orders_from must be an integer");
	}

	if (const std::optional<Json> error = CheckWriteContext(company, false); error.has_value()) return *error;

	const Engine *e = Engine::GetIfValid(EngineID(static_cast<uint16_t>(engine_id)));
	if (e == nullptr) {
		Json detail = Json::object();
		detail["engine_id"] = engine_id;
		return MakeError("not_found", "engine does not exist", std::move(detail));
	}

	TileIndex tile = INVALID_TILE;
	int64_t depot_id = 0;
	if (params.contains("depot_id")) {
		if (!ReadOptionalInt(params, "depot_id", depot_id)) return MakeError("invalid_arguments", "depot_id must be an integer");
		const Depot *d = Depot::GetIfValid(DepotID(static_cast<uint16_t>(depot_id)));
		if (d == nullptr) {
			Json detail = Json::object();
			detail["depot_id"] = depot_id;
			return MakeError("not_found", "depot does not exist", std::move(detail));
		}
		tile = d->xy;
	} else if (params.contains("tile")) {
		RawTile raw{};
		if (!ReadRawTile(params["tile"], raw)) return MakeError("invalid_arguments", "tile must be a tile object with integer x and y");
		if (!ToTileIndex(raw, tile)) return MakeError("invalid_arguments", "tile must be inside the map");
	} else {
		return MakeError("invalid_arguments", "build needs depot_id or tile");
	}

	VehicleID source_id = VehicleID::Invalid();
	if (share) {
		const Vehicle *source = Vehicle::GetIfValid(VehicleID(static_cast<uint16_t>(share_from)));
		if (source == nullptr) {
			Json detail = Json::object();
			detail["share_orders_from"] = share_from;
			return MakeError("not_found", "share_orders_from vehicle does not exist", std::move(detail));
		}
		if (source->owner != company) {
			Json detail = Json::object();
			detail["share_orders_from"] = share_from;
			detail["owner"] = source->owner.base();
			return MakeError("invalid_arguments", "share_orders_from vehicle belongs to another company", std::move(detail));
		}
		if (source->type != e->type) {
			Json detail = Json::object();
			detail["share_orders_from"] = share_from;
			detail["type"] = std::string(VehicleTypeName(source->type));
			return MakeError("invalid_arguments", "share_orders_from vehicle has another type", std::move(detail));
		}
		if (!source->IsPrimaryVehicle()) {
			Json detail = Json::object();
			detail["share_orders_from"] = share_from;
			return MakeError("invalid_arguments", "share_orders_from vehicle is not a primary vehicle", std::move(detail));
		}
		source_id = source->index;
	}

	CompanyContext context(company);

	Money spent = 0;
	Json created = Json::array();
	for (int64_t i = 0; i < count; i++) {
		if (!RunCommand<Commands::BuildVehicle>(false, &AgentBuildVehicleCallback, tile, e->index, false, cargo, ClientID::Invalid)) {
			Json result = Json::object();
			result["op"] = "build";
			result["vehicle_ids"] = std::move(created);
			if (!_posted.captured) return NotExecuted(std::move(result));
			return PartialResult(std::move(result), spent, _posted);
		}
		const VehicleID veh_id = _posted.vehicle;
		spent += _posted.cost.GetCost();
		created.push_back(veh_id.base());

		if (rename) {
			if (!RunCommand<Commands::RenameVehicle>(false, &AgentCommandCallback, veh_id, name)) {
				Json result = Json::object();
				result["op"] = "build";
				result["vehicle_ids"] = std::move(created);
				result["renamed"] = false;
				if (!_posted.captured) return NotExecuted(std::move(result));
				return PartialResult(std::move(result), spent, _posted);
			}
			spent += _posted.cost.GetCost();
		}

		if (source_id != VehicleID::Invalid()) {
			if (!RunCommandAt<Commands::CloneOrder>(false, tile, &AgentCommandCallback, CO_SHARE, veh_id, source_id)) {
				Json result = Json::object();
				result["op"] = "build";
				result["vehicle_ids"] = std::move(created);
				result["orders_shared"] = false;
				if (!_posted.captured) return NotExecuted(std::move(result));
				return PartialResult(std::move(result), spent, _posted);
			}
			spent += _posted.cost.GetCost();
		}
	}

	Json result = Json::object();
	result["ok"] = true;
	result["op"] = "build";
	result["vehicle_ids"] = std::move(created);
	result["cost"] = static_cast<int64_t>(spent);
	if (rename) result["name"] = name;
	if (source_id != VehicleID::Invalid()) result["share_orders_from"] = source_id.base();
	return result;
}

/**
 * Start or stop a vehicle.
 * @param params Method parameters.
 * @param company Company the vehicle belongs to.
 * @param start True to start, false to stop.
 * @return Tool result with the state the vehicle is in after the call.
 */
Json VehicleStartStop(const Json &params, CompanyID company, bool start)
{
	int64_t vehicle_id = 0;
	if (!ReadInt(params, "vehicle_id", vehicle_id)) return MakeError("invalid_arguments", "start/stop needs vehicle_id");

	if (const std::optional<Json> error = CheckWriteContext(company, false); error.has_value()) return *error;

	Vehicle *v = Vehicle::GetIfValid(VehicleID(static_cast<uint16_t>(vehicle_id)));
	if (v == nullptr) {
		Json detail = Json::object();
		detail["vehicle_id"] = vehicle_id;
		return MakeError("not_found", "vehicle does not exist", std::move(detail));
	}

	const VehicleID veh_id = v->index;
	const bool stopped = v->vehstatus.Test(VehState::Stopped);
	Json result = Json::object();
	result["op"] = start ? "start" : "stop";
	result["vehicle_id"] = veh_id.base();

	if (stopped != start) {
		/* Already in the requested state; report it instead of issuing a no-op command. */
		result["ok"] = true;
		result["changed"] = false;
		result["stopped"] = stopped;
		return result;
	}

	CompanyContext context(company);

	if (!RunCommandAt<Commands::StartStopVehicle>(false, v->tile, &AgentCommandCallback, veh_id, false)) {
		result["changed"] = false;
		if (!_posted.captured) return NotExecuted(std::move(result));
		return CommandResult(_posted, std::move(result));
	}

	result["changed"] = true;
	const Vehicle *after = Vehicle::GetIfValid(veh_id);
	result["stopped"] = after != nullptr ? after->vehstatus.Test(VehState::Stopped) : stopped;
	return CommandResult(_posted, std::move(result));
}

/**
 * Sell a vehicle.
 * @param params Method parameters.
 * @param company Company the vehicle belongs to.
 * @return Tool result with the sold vehicle id and the cost.
 */
Json VehicleSell(const Json &params, CompanyID company)
{
	int64_t vehicle_id = 0;
	if (!ReadInt(params, "vehicle_id", vehicle_id)) return MakeError("invalid_arguments", "sell needs vehicle_id");

	if (const std::optional<Json> error = CheckWriteContext(company, false); error.has_value()) return *error;

	Vehicle *v = Vehicle::GetIfValid(VehicleID(static_cast<uint16_t>(vehicle_id)));
	if (v == nullptr) {
		Json detail = Json::object();
		detail["vehicle_id"] = vehicle_id;
		return MakeError("not_found", "vehicle does not exist", std::move(detail));
	}

	CompanyContext context(company);

	Json result = Json::object();
	result["op"] = "sell";
	result["vehicle_id"] = v->index.base();
	if (!RunCommandAt<Commands::SellVehicle>(false, v->tile, &AgentCommandCallback, v->index, false, false, ClientID::Invalid)) {
		if (!_posted.captured) return NotExecuted(std::move(result));
	}
	return CommandResult(_posted, std::move(result));
}

/* -- orders ------------------------------------------------------------------------------ */

/**
 * Build one order from the tool's order object.
 * @param entry The order object.
 * @param order Receives the order.
 * @param error Receives the reason when the order is unusable.
 * @return True when the order was built.
 */
bool ReadOrder(const Json &entry, Order &order, std::string &error)
{
	if (!entry.is_object()) {
		error = "order must be an object";
		return false;
	}

	std::string type;
	if (!ReadOptionalString(entry, "type", type)) {
		error = "order type must be a string";
		return false;
	}
	if (type.empty()) type = "station";

	int64_t id = 0;
	if (!ReadInt(entry, "id", id)) {
		error = "order needs an id";
		return false;
	}

	if (type == "station") {
		order.MakeGoToStation(StationID(static_cast<uint16_t>(id)));
	} else if (type == "waypoint") {
		order.MakeGoToWaypoint(StationID(static_cast<uint16_t>(id)));
	} else if (type == "depot") {
		order.MakeGoToDepot(DepotID(static_cast<uint16_t>(id)), OrderDepotTypeFlag::PartOfOrders);
	} else {
		error = "unsupported order type `" + type + "`";
		return false;
	}

	std::string load;
	if (!ReadOptionalString(entry, "load", load)) {
		error = "order load behaviour must be a string";
		return false;
	}
	if (!load.empty()) {
		if (load == "full") {
			order.SetLoadType(OrderLoadType::FullLoad);
		} else if (load == "any") {
			order.SetLoadType(OrderLoadType::FullLoadAny);
		} else if (load == "none") {
			order.SetLoadType(OrderLoadType::NoLoad);
		} else if (load == "if_possible") {
			order.SetLoadType(OrderLoadType::LoadIfPossible);
		} else {
			error = "unsupported load behaviour `" + load + "`";
			return false;
		}
	}

	std::string unload;
	if (!ReadOptionalString(entry, "unload", unload)) {
		error = "order unload behaviour must be a string";
		return false;
	}
	if (!unload.empty()) {
		if (unload == "all") {
			order.SetUnloadType(OrderUnloadType::Unload);
		} else if (unload == "transfer") {
			order.SetUnloadType(OrderUnloadType::Transfer);
		} else if (unload == "none") {
			order.SetUnloadType(OrderUnloadType::NoUnload);
		} else if (unload == "if_possible") {
			order.SetUnloadType(OrderUnloadType::UnloadIfPossible);
		} else {
			error = "unsupported unload behaviour `" + unload + "`";
			return false;
		}
	}

	int64_t refit_cargo_id = 0;
	if (entry.contains("refit_cargo_id")) {
		if (!ReadOptionalInt(entry, "refit_cargo_id", refit_cargo_id)) {
			error = "refit_cargo_id must be an integer";
			return false;
		}
		if (refit_cargo_id < 0 || refit_cargo_id >= static_cast<int64_t>(NUM_CARGO)) {
			error = "refit_cargo_id is not a cargo";
			return false;
		}
		order.SetRefit(static_cast<CargoType>(static_cast<uint8_t>(refit_cargo_id)));
	}

	return true;
}

/**
 * Replace or extend the order list of a vehicle.
 * @param params Method parameters.
 * @param company Company the vehicle belongs to.
 * @param replace True to replace the list, false to append.
 * @return Tool result with the resulting order count and the cost.
 */
Json OrdersSet(const Json &params, CompanyID company, bool replace)
{
	int64_t vehicle_id = 0;
	if (!ReadInt(params, "vehicle_id", vehicle_id)) return MakeError("invalid_arguments", "orders need vehicle_id");

	const auto entries = params.find("orders");
	if (entries == params.end() || !entries->is_array() || entries->empty()) {
		return MakeError("invalid_arguments", "orders need a non-empty `orders` array");
	}

	/* Build every order before touching the vehicle, so a bad entry cannot leave a half list. */
	std::vector<Order> orders;
	for (const Json &entry : *entries) {
		Order order;
		std::string error;
		if (!ReadOrder(entry, order, error)) return MakeError("invalid_arguments", error);
		orders.push_back(order);
	}

	if (const std::optional<Json> error = CheckWriteContext(company, false); error.has_value()) return *error;

	Vehicle *v = Vehicle::GetIfValid(VehicleID(static_cast<uint16_t>(vehicle_id)));
	if (v == nullptr) {
		Json detail = Json::object();
		detail["vehicle_id"] = vehicle_id;
		return MakeError("not_found", "vehicle does not exist", std::move(detail));
	}

	const VehicleID veh_id = v->index;
	CompanyContext context(company);

	Json result = Json::object();
	result["op"] = replace ? "set" : "append";
	result["vehicle_id"] = veh_id.base();

	Money spent = 0;
	uint deleted = 0;
	if (replace) {
		/* `set` replaces the list: delete the existing orders first, one real command each. */
		for (uint safety = 0; safety < 256; safety++) {
			const Vehicle *current = Vehicle::GetIfValid(veh_id);
			if (current == nullptr || current->GetNumOrders() == 0) break;
			if (!RunCommandAt<Commands::DeleteOrder>(false, current->tile, &AgentCommandCallback, veh_id, 0)) {
				result["deleted"] = deleted;
				if (!_posted.captured) return NotExecuted(std::move(result));
				return PartialResult(std::move(result), spent, _posted);
			}
			spent += _posted.cost.GetCost();
			deleted++;
		}
	}

	uint inserted = 0;
	for (const Order &order : orders) {
		const Vehicle *current = Vehicle::GetIfValid(veh_id);
		if (current == nullptr) {
			Json detail = Json::object();
			detail["vehicle_id"] = vehicle_id;
			return MakeError("not_found", "vehicle disappeared while its orders were set", std::move(detail));
		}
		if (!RunCommandAt<Commands::InsertOrder>(false, current->tile, &AgentCommandCallback, veh_id, current->GetNumOrders(), order)) {
			result["deleted"] = deleted;
			result["inserted"] = inserted;
			if (!_posted.captured) return NotExecuted(std::move(result));
			return PartialResult(std::move(result), spent, _posted);
		}
		spent += _posted.cost.GetCost();
		inserted++;
	}

	const Vehicle *after = Vehicle::GetIfValid(veh_id);
	result["ok"] = true;
	result["deleted"] = deleted;
	result["inserted"] = inserted;
	result["orders"] = after != nullptr ? after->GetNumOrders() : 0;
	result["cost"] = static_cast<int64_t>(spent);
	return result;
}

/**
 * Share the orders of another vehicle.
 * @param params Method parameters.
 * @param company Company the vehicle belongs to.
 * @return Tool result with both vehicle ids and the cost.
 */
Json OrdersShared(const Json &params, CompanyID company)
{
	int64_t vehicle_id = 0;
	int64_t source_id = 0;
	if (!ReadInt(params, "vehicle_id", vehicle_id)) return MakeError("invalid_arguments", "shared needs vehicle_id");
	if (!ReadInt(params, "share_orders_from", source_id)) return MakeError("invalid_arguments", "shared needs share_orders_from");

	if (const std::optional<Json> error = CheckWriteContext(company, false); error.has_value()) return *error;

	Vehicle *v = Vehicle::GetIfValid(VehicleID(static_cast<uint16_t>(vehicle_id)));
	if (v == nullptr) {
		Json detail = Json::object();
		detail["vehicle_id"] = vehicle_id;
		return MakeError("not_found", "vehicle does not exist", std::move(detail));
	}
	Vehicle *source = Vehicle::GetIfValid(VehicleID(static_cast<uint16_t>(source_id)));
	if (source == nullptr) {
		Json detail = Json::object();
		detail["share_orders_from"] = source_id;
		return MakeError("not_found", "source vehicle does not exist", std::move(detail));
	}

	/* Reject what `Commands::CloneOrder` would reject, before anything is touched. */
	if (v->index == source->index) return MakeError("invalid_arguments", "a vehicle cannot share orders with itself");
	if (v->owner != company || source->owner != company) return MakeError("invalid_arguments", "both vehicles must belong to the company");
	if (v->type != source->type) return MakeError("invalid_arguments", "both vehicles must be of the same type");
	if (!v->IsPrimaryVehicle() || !source->IsPrimaryVehicle()) return MakeError("invalid_arguments", "both vehicles must be primary vehicles");

	CompanyContext context(company);

	Json result = Json::object();
	result["op"] = "shared";
	result["vehicle_id"] = v->index.base();
	result["share_orders_from"] = source->index.base();
	if (!RunCommandAt<Commands::CloneOrder>(false, v->tile, &AgentCommandCallback, CO_SHARE, v->index, source->index)) {
		if (!_posted.captured) return NotExecuted(std::move(result));
	}
	return CommandResult(_posted, std::move(result));
}

/**
 * Assign a vehicle to a group.
 * @param params Method parameters.
 * @param company Company the vehicle belongs to.
 * @return Tool result with the group the vehicle ended up in and the cost.
 */
Json OrdersGroup(const Json &params, CompanyID company)
{
	int64_t vehicle_id = 0;
	int64_t group_id = 0;
	if (!ReadInt(params, "vehicle_id", vehicle_id)) return MakeError("invalid_arguments", "group needs vehicle_id");
	if (!ReadInt(params, "group_id", group_id)) return MakeError("invalid_arguments", "group needs group_id");

	if (const std::optional<Json> error = CheckWriteContext(company, false); error.has_value()) return *error;

	Vehicle *v = Vehicle::GetIfValid(VehicleID(static_cast<uint16_t>(vehicle_id)));
	if (v == nullptr) {
		Json detail = Json::object();
		detail["vehicle_id"] = vehicle_id;
		return MakeError("not_found", "vehicle does not exist", std::move(detail));
	}

	CompanyContext context(company);

	Json result = Json::object();
	result["op"] = "group";
	result["vehicle_id"] = v->index.base();
	if (!RunCommand<Commands::AddVehicleToGroup>(false, &AgentAddVehicleToGroupCallback, GroupID(static_cast<uint16_t>(group_id)), v->index, false, VehicleListIdentifier{})) {
		result["group_id"] = group_id;
		if (!_posted.captured) return NotExecuted(std::move(result));
		return CommandResult(_posted, std::move(result));
	}
	result["group_id"] = _posted.group.base();
	return CommandResult(_posted, std::move(result));
}

/* -- method dispatch --------------------------------------------------------------------- */

/**
 * Dispatch `world.query`.
 * @param params Method parameters.
 * @param company Company of the call.
 * @return Tool result.
 */
Json DispatchQuery(const Json &params, CompanyID company)
{
	std::string kind;
	if (!ReadString(params, "kind", kind)) return MakeError("invalid_arguments", "world.query needs kind");
	if (kind == "vehicles") return QueryVehicles(params, company);
	if (kind == "engines") return QueryEngines(params);
	return MakeGap("world.query", kind);
}

/**
 * Dispatch `world.construct`.
 * @param params Method parameters.
 * @param company Company that pays.
 * @return Tool result.
 */
Json DispatchConstruct(const Json &params, CompanyID company)
{
	std::string op;
	if (!ReadString(params, "op", op)) return MakeError("invalid_arguments", "world.construct needs op");
	if (op != "road" && op != "tram" && op != "station" && op != "depot" && op != "demolish") return MakeGap("world.construct", op);

	bool dry_run = false;
	if (!ReadOptionalBool(params, "dry_run", dry_run)) return MakeError("invalid_arguments", "dry_run must be a boolean");

	if (op == "demolish") return DemolishTiles(params, company, dry_run);

	RoadType rt = op == "tram" ? ROADTYPE_TRAM : ROADTYPE_ROAD;
	if (!ReadRoadType(params, rt)) return MakeError("invalid_arguments", "road_type must be `road` or `tram`");

	bool demolish = false;
	if (!ReadOptionalBool(params, "demolish", demolish)) return MakeError("invalid_arguments", "demolish must be a boolean");
	if (demolish) return MakeGap("world.construct", op + " with demolish=true (use op=demolish)");

	if (op == "road" || op == "tram") return ConstructRoad(params, company, rt, dry_run);
	if (op == "station") return ConstructRoadStop(params, company, rt, dry_run);
	return ConstructDepot(params, company, rt, dry_run);
}

/**
 * Dispatch `world.vehicles`.
 * @param params Method parameters.
 * @param company Company that pays and owns the vehicles.
 * @return Tool result.
 */
Json DispatchVehicles(const Json &params, CompanyID company)
{
	std::string op;
	if (!ReadString(params, "op", op)) return MakeError("invalid_arguments", "world.vehicles needs op");

	if (op != "build" && op != "sell" && op != "start" && op != "stop") return MakeGap("world.vehicles", op);

	if (op == "build") return VehicleBuild(params, company);
	if (op == "sell") return VehicleSell(params, company);
	if (op == "start") return VehicleStartStop(params, company, true);
	return VehicleStartStop(params, company, false);
}

/**
 * Dispatch `world.orders`.
 * @param params Method parameters.
 * @param company Company the vehicle belongs to.
 * @return Tool result.
 */
Json DispatchOrders(const Json &params, CompanyID company)
{
	std::string op;
	if (!ReadString(params, "op", op)) return MakeError("invalid_arguments", "world.orders needs op");

	if (op != "set" && op != "append" && op != "shared" && op != "group") return MakeGap("world.orders", op);

	if (op == "set") return OrdersSet(params, company, true);
	if (op == "append") return OrdersSet(params, company, false);
	if (op == "shared") return OrdersShared(params, company);
	return OrdersGroup(params, company);
}

/**
 * Is this one of the engine methods the protocol knows about?
 * @param method Engine method name.
 * @return True for a known engine method.
 */
bool IsKnownEngineMethod(std::string_view method)
{
	return method == "world.info" || method == "world.query" || method == "world.objects" ||
			method == "world.pause" || method == "world.rename" || method == "world.settings.get" ||
			method == "world.settings.set" || method == "world.construct" || method == "world.vehicles" ||
			method == "world.orders" || method == "world.capture" || method == "task.barrier";
}

/**
 * Build one capability entry.
 * @param method Engine method name.
 * @param authority `read` or `write`.
 * @param implemented Ops or kinds this module implements.
 * @param gaps Ops or kinds that are reported as gaps.
 * @return Capability entry.
 */
Json Capability(std::string_view method, std::string_view authority, std::initializer_list<std::string_view> implemented, std::initializer_list<std::string_view> gaps)
{
	Json entry = Json::object();
	entry["method"] = std::string(method);
	entry["authority"] = std::string(authority);
	entry["implemented"] = Json::array();
	for (const std::string_view item : implemented) entry["implemented"].push_back(std::string(item));
	entry["gaps"] = Json::array();
	for (const std::string_view item : gaps) entry["gaps"].push_back(std::string(item));
	return entry;
}

} // namespace

Json GameToolCapabilities()
{
	Json methods = Json::array();
	methods.push_back(Capability("world.query", "read",
			{"vehicles", "engines"},
			{"towns", "companies", "tiles", "stations", "industries", "cargos", "settings", "groups", "orders", "waypoints", "signs", "houses", "depots", "finances"}));
	methods.push_back(Capability("world.construct", "write",
			{"road", "tram", "station", "depot", "demolish"},
			{"rail", "signal", "airport", "dock", "bridge", "tunnel", "canal", "terraform"}));
	methods.push_back(Capability("world.vehicles", "write",
			{"build", "sell", "start", "stop"},
			{"refit", "clone"}));
	methods.push_back(Capability("world.orders", "write",
			{"set", "append", "shared", "group"},
			{"timetable"}));

	Json result = Json::object();
	result["module"] = "agent_game_tools";
	result["methods"] = std::move(methods);
	result["unimplemented_methods"] = Json::array({"world.info", "world.objects", "world.pause", "world.rename", "world.settings.get", "world.settings.set", "world.capture", "task.barrier"});
	result["notes"] = Json::array({
		"Actual mutations go through Command::Post, the normal player command path, so the engine's own eligibility, pause and finance checks apply; results carry the actual cost and the created ids.",
		"world.construct accepts `dry_run`, which only queries the cost and reports `estimated: true`.",
		"world.construct op=demolish clears the tile (Commands::LandscapeClear); the `demolish` flag on the other ops is reported as a gap.",
		"world.construct op=station builds one road stop rectangle; `stop_type` is `bus` (default) or `truck`, and a span above 255 tiles is rejected instead of truncated.",
		"world.construct op=station/depot take one tile per call; a longer `tiles` array is rejected with `unsupported_multi_item` instead of using only the first tile.",
		"world.vehicles op=build honours `cargo_id` (build cargo), `name` (Commands::RenameVehicle) and `share_orders_from` (Commands::CloneOrder); a batch reports partial receipts and the money it actually spent.",
		"world.query kind=vehicles/engines is read-only inventory; the other kinds are reported as gaps.",
	});
	return result;
}

std::optional<Json> DispatchGameTool(std::string_view method, const Json &params, CompanyID company)
{
	if (method != "world.query" && method != "world.construct" && method != "world.vehicles" && method != "world.orders") {
		if (IsKnownEngineMethod(method)) return MakeGap(method, {});
		return std::nullopt;
	}

	if (!params.is_object()) return MakeError("invalid_arguments", "params must be an object");

	if (method == "world.query") return DispatchQuery(params, company);
	if (method == "world.construct") return DispatchConstruct(params, company);
	if (method == "world.vehicles") return DispatchVehicles(params, company);
	return DispatchOrders(params, company);
}

} // namespace InGameAgent
