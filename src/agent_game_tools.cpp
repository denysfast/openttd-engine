/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/>.
 */

/** @file agent_game_tools.cpp Dispatch of the agent game tool methods onto real OpenTTD commands. */

#include "stdafx.h"
#include "agent_game_tools.h"

#include "aircraft.h"
#include "articulated_vehicles.h"
#include "cargotype.h"
#include "command_func.h"
#include "company_base.h"
#include "depot_base.h"
#include "depot_map.h"
#include "direction_func.h"
#include "direction_type.h"
#include "engine_base.h"
#include "group.h"
#include "group_cmd.h"
#include "landscape_cmd.h"
#include "map_func.h"
#include "network/network.h"
#include "order_base.h"
#include "order_cmd.h"
#include "order_type.h"
#include "road.h"
#include "road_cmd.h"
#include "road_func.h"
#include "road_type.h"
#include "road_map.h"
#include "roadveh.h"
#include "station_base.h"
#include "station_cmd.h"
#include "station_map.h"
#include "station_type.h"
#include "strings_func.h"
#include "tile_cmd.h"
#include "timer/timer_game_calendar.h"
#include "timetable_cmd.h"
#include "track_func.h"
#include "train.h"
#include "vehicle_base.h"
#include "vehicle_cmd.h"
#include "vehicle_func.h"
#include "vehiclelist.h"
#include "waypoint_base.h"

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
	VehicleID vehicle = VehicleID::Invalid(); ///< Vehicle created by `Commands::BuildVehicle` or `Commands::CloneVehicle`.
	GroupID group = GroupID::Invalid(); ///< Group a vehicle ended up in.
	uint refit_capacity = 0; ///< Capacity of a refitted consist for the new cargo.
	uint16_t mail_capacity = 0; ///< Secondary (mail) capacity of a refitted aircraft.
	CargoArray cargo_capacities{}; ///< Capacities of a refitted consist per cargo.
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

void AgentRefitVehicleCallback(Commands, const CommandCost &result, uint refit_capacity, uint16_t mail_capacity, CargoArray cargo_capacities)
{
	_posted.captured = true;
	_posted.cost = result;
	_posted.refit_capacity = refit_capacity;
	_posted.mail_capacity = mail_capacity;
	_posted.cargo_capacities = cargo_capacities;
}

void AgentCloneVehicleCallback(Commands, const CommandCost &result, VehicleID veh_id)
{
	_posted.captured = true;
	_posted.cost = result;
	_posted.vehicle = veh_id;
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
 *
 * Both flags are always written, also for an actual execution: a receipt that leaves `dry_run` out
 * cannot be told apart from one that never carried the flag, and the audit reads this boolean to
 * decide whether a write really applied. `estimated` and `dry_run` state the same thing here, the
 * mode of this very call.
 *
 * @param result Tool result to complete.
 * @param dry_run True when the result is a `dry_run` estimate, false for an actual execution.
 * @return The completed tool result.
 */
Json MarkExecution(Json result, bool dry_run)
{
	result["estimated"] = dry_run;
	result["dry_run"] = dry_run;
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

/**
 * Read a pool id value without narrowing it to a type smaller than the pool uses.
 *
 * A plain cast would wrap: `0xFF000` would become another valid vehicle, a negative or a
 * fractional value would become some other object, and a JSON boolean would become 0 or 1. Every
 * id goes through here, so an unusable id is an error and never a different object of the same
 * pool.
 *
 * @tparam Tpool The pool id type.
 * @param value JSON value holding the id.
 * @param key Parameter name, used in the error message.
 * @param id Receives the id.
 * @param code Error code to reject with.
 * @return Error result, or an empty optional when the value is an id of that pool.
 */
template <typename Tpool>
std::optional<Json> ReadPoolIdValue(const Json &value, std::string_view key, Tpool &id, std::string_view code)
{
	using Tbase = typename Tpool::BaseType;
	if (!value.is_number_integer()) return MakeError(code, std::string(key) + " must be an integer");
	const int64_t raw = value.get<int64_t>();
	if (raw < 0 || raw >= static_cast<int64_t>(Tpool::End().base())) {
		Json detail = Json::object();
		detail[std::string(key)] = raw;
		detail["max"] = static_cast<int64_t>(Tpool::End().base()) - 1;
		return MakeError(code, std::string(key) + " is outside its pool", std::move(detail));
	}
	id = Tpool(static_cast<Tbase>(raw));
	return std::nullopt;
}

/**
 * Read a pool id of a query filter. A filter that names no object is an invalid argument.
 * @tparam Tpool The pool id type.
 * @param value JSON value holding the id.
 * @param key Filter name, used in the error message.
 * @param id Receives the id.
 * @return Error result, or an empty optional when the value is an id of that pool.
 */
template <typename Tpool>
std::optional<Json> ReadPoolId(const Json &value, std::string_view key, Tpool &id)
{
	return ReadPoolIdValue(value, key, id, "invalid_arguments");
}

/**
 * Read a pool id parameter of a write op, when it is present, and check it before the cast.
 *
 * An id that cannot name an object is `invalid_params`, the code the parent bridge uses for its
 * own id checks; it is never wrapped onto another object of the pool.
 *
 * @tparam Tpool The pool id type.
 * @param params Method parameters.
 * @param key Parameter name.
 * @param id Receives the id.
 * @return Error result, or an empty optional when the parameter is absent or a valid id.
 */
template <typename Tpool>
std::optional<Json> ReadPoolIdParam(const Json &params, const char *key, Tpool &id)
{
	const auto it = params.find(key);
	if (it == params.end()) return std::nullopt;
	return ReadPoolIdValue(*it, key, id, "invalid_params");
}

/**
 * Read a cargo id parameter and check it against the cargo types of the game.
 * @param params Method parameters.
 * @param key Parameter name.
 * @param cargo Receives the cargo type.
 * @return Error result, or an empty optional when the parameter is absent or a valid cargo.
 */
std::optional<Json> ReadCargoId(const Json &params, const char *key, CargoType &cargo)
{
	if (!params.contains(key)) return std::nullopt;
	int64_t cargo_id = 0;
	if (!ReadOptionalInt(params, key, cargo_id)) return MakeError("invalid_params", std::string(key) + " must be an integer");
	if (cargo_id < 0 || cargo_id >= static_cast<int64_t>(NUM_CARGO)) {
		Json detail = Json::object();
		detail[std::string(key)] = cargo_id;
		detail["max"] = NUM_CARGO - 1;
		return MakeError("invalid_params", std::string(key) + " is not a cargo", std::move(detail));
	}
	cargo = static_cast<CargoType>(static_cast<uint8_t>(cargo_id));
	return std::nullopt;
}

/**
 * Reject parameters the op does not read, so nothing is silently ignored.
 *
 * `op`, `kind`, `company_id` and `expected_refs` belong to the family or to the parent bridge and
 * are accepted everywhere; `expected_refs` is validated by the bridge before the call is
 * dispatched, never here.
 *
 * @param params Method parameters.
 * @param method Engine method name, used in the error message.
 * @param op Op or kind that is being dispatched.
 * @param known Parameters the op actually reads.
 * @return Error result, or an empty optional when every parameter is read.
 */
std::optional<Json> CheckKnownParams(const Json &params, std::string_view method, std::string_view op, std::initializer_list<std::string_view> known)
{
	static constexpr std::string_view common[] = {"op", "kind", "company_id", "expected_refs"};

	Json unknown = Json::array();
	for (const auto &item : params.items()) {
		const std::string &key = item.key();
		bool found = false;
		for (const std::string_view name : common) {
			if (key == name) {
				found = true;
				break;
			}
		}
		if (!found) {
			for (const std::string_view name : known) {
				if (key == name) {
					found = true;
					break;
				}
			}
		}
		if (!found) unknown.push_back(key);
	}
	if (unknown.empty()) return std::nullopt;

	Json detail = Json::object();
	detail["method"] = std::string(method);
	detail["op"] = std::string(op);
	detail["unknown"] = std::move(unknown);
	detail["supported"] = Json::array();
	for (const std::string_view name : known) detail["supported"].push_back(std::string(name));
	return MakeError("invalid_arguments", "parameter is not read by this op", std::move(detail));
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
 * Generation of a pooled object, when the pool tracks one.
 *
 * The bridge kernel gives every pooled object a generation counter, so a typed reference can name
 * the lifetime of the object it was read from and an id that was deleted and reused is reported
 * as stale instead of being resolved to the new object. This module is also compiled against trees
 * that do not have the counter yet; it then reports 0, which the kernel reads as "no generation".
 *
 * @param item The pooled object.
 * @return The generation of the object, or 0 when the pool does not track one.
 */
template <typename Titem>
uint64_t ItemGeneration(const Titem *item)
{
	if constexpr (requires { item->generation; }) {
		return item->generation;
	} else {
		return 0;
	}
}

/**
 * Serialise a typed reference to a pooled object.
 *
 * The reference is the protocol shape `{type, id, generation}`; the world id and the epoch are the
 * outer world binding the bridge adds around a whole result, not part of a single reference.
 *
 * @param type Protocol name of the object type.
 * @param item The pooled object.
 * @return JSON reference with the type, the id and the generation.
 */
template <typename Titem>
Json PoolRef(std::string_view type, const Titem *item)
{
	Json ref = Json::object();
	ref["type"] = std::string(type);
	ref["id"] = item->index.base();
	ref["generation"] = ItemGeneration(item);
	return ref;
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

/**
 * Name of a diagonal direction, as the tool protocol spells it.
 * @param dir The direction.
 * @return The name.
 */
std::string DiagDirName(DiagDirection dir)
{
	switch (dir) {
		case DiagDirection::NE: return "ne";
		case DiagDirection::SE: return "se";
		case DiagDirection::SW: return "sw";
		case DiagDirection::NW: return "nw";
		default: return "invalid";
	}
}

/**
 * Name of one road piece, as the tool protocol spells it.
 * @param bit The road piece.
 * @return The name.
 */
std::string RoadBitName(RoadBit bit)
{
	switch (bit) {
		case RoadBit::NW: return "nw";
		case RoadBit::SW: return "sw";
		case RoadBit::SE: return "se";
		case RoadBit::NE: return "ne";
		default: return "invalid";
	}
}

/**
 * Serialise the road pieces of a tile as protocol names.
 * @param bits The pieces.
 * @return JSON array of names, in the engine's own piece order.
 */
Json RoadBitsJson(RoadBits bits)
{
	static constexpr RoadBit all[]{RoadBit::NW, RoadBit::SW, RoadBit::SE, RoadBit::NE};

	Json result = Json::array();
	for (const RoadBit bit : all) {
		if (bits.Test(bit)) result.push_back(RoadBitName(bit));
	}
	return result;
}

/**
 * Name of one disallowed travel direction, as the engine stores it on a one-way road tile.
 * @param dir The engine's own value; it is not a screen direction.
 * @return The name.
 */
std::string_view DisallowedRoadDirectionName(DisallowedRoadDirection dir)
{
	switch (dir) {
		case DisallowedRoadDirection::Southbound: return "southbound";
		case DisallowedRoadDirection::Northbound: return "northbound";
		default: return "invalid";
	}
}

/**
 * The one road or tram family a tile carries, as the engine records it.
 *
 * A depot tile stores exactly one family, so it is read from the tile and never assumed. A stop tile
 * can carry both at once; its reader walks the families instead of choosing one.
 * @param tile The tile.
 * @return \c RoadTramType::Road when the tile carries a road-type road, else \c RoadTramType::Tram.
 */
RoadTramType TileRoadTramType(TileIndex tile)
{
	if (IsValidTile(tile) && MayHaveRoad(tile) && GetRoadType(tile, RoadTramType::Road) != INVALID_ROADTYPE) {
		return RoadTramType::Road;
	}
	return RoadTramType::Tram;
}

/**
 * The neighbour of one road connection: the tile in a direction and what that tile carries.
 *
 * The piece the neighbour needs is the one on its side facing the reported tile, which is the piece
 * `ScriptRoad::AreRoadTilesConnected` and the road pathfinder both look for.
 * @param tile The reported tile.
 * @param dir The direction from \a tile to the neighbour tile.
 * @param rtt The road or tram family.
 * @return `{direction, tile, required_road_bit, road_bits, has_required_bit}`.
 */
Json RoadNeighbourJson(TileIndex tile, DiagDirection dir, RoadTramType rtt)
{
	const TileIndex neighbour = TileAddByDiagDir(tile, dir);
	const RoadBits required = DiagDirToRoadBits(ReverseDiagDir(dir));
	const RoadBits present = IsValidTile(neighbour) ? GetAnyRoadBits(neighbour, rtt, false) : RoadBits{};
	const std::optional<RoadBit> required_bit = required.GetNthSetBit(0);

	Json result = Json::object();
	result["family"] = rtt == RoadTramType::Tram ? "tram" : "road";
	result["direction"] = DiagDirName(dir);
	result["tile"] = IsValidTile(neighbour) ? TileJson(neighbour) : Json(nullptr);
	result["required_road_bit"] = required_bit.has_value() ? Json(RoadBitName(*required_bit)) : Json(nullptr);
	result["road_bits"] = RoadBitsJson(present);
	result["has_required_bit"] = present.Any(required);
	return result;
}

/**
 * One road connection of a tile: the neighbour tile in a direction and whether a vehicle can drive
 * between the two.
 *
 * The engine lets a road vehicle drive into a tile only if the tile it comes from carries the road
 * piece towards that tile and the tile it drives into offers a track reachable from the direction it
 * moves in: `RoadFindPathToDest` keeps only
 * `GetTileTrackStatus(entered) & DiagdirReachesTrackdirs(enterdir)`, and `enterdir` there is the
 * direction of motion, not the side the vehicle comes from. The depot and bay stop resets in that
 * same function show it: a vehicle entering a depot through its door moves opposite to the stored
 * depot direction, and it is the stored direction that is rejected. The track status of the entered
 * tile already has its one-way restriction applied (`GetTileTrackStatus_Road` scales the trackdirs
 * by the disallowed directions), so a one-way tile only lets a vehicle in along its arrow.
 * The raw piece test alone is not enough: a one-piece stub carries the piece but has no drivable
 * track at all, so \c connected is the pair and a stub never reads as a usable connection.
 * @param tile The reported tile.
 * @param dir The direction from \a tile to the neighbour tile.
 * @param rtt The road or tram family.
 * @param leaving True when the vehicle drives out of \a tile into the neighbour (a depot exit),
 *                false when it drives from the neighbour into \a tile (a stop entry).
 * @return `{direction, tile, required_road_bit, road_bits, has_required_bit, drivable, connected}`.
 */
Json RoadLinkJson(TileIndex tile, DiagDirection dir, RoadTramType rtt, bool leaving)
{
	const TileIndex neighbour = TileAddByDiagDir(tile, dir);
	const TileIndex entered = leaving ? neighbour : tile;
	/* The direction of motion when the vehicle drives into `entered`. */
	const DiagDirection motion = leaving ? dir : ReverseDiagDir(dir);
	const bool valid = IsValidTile(neighbour) && IsValidTile(entered);

	const RoadBits own = IsValidTile(tile) ? GetAnyRoadBits(tile, rtt, false) : RoadBits{};
	const RoadBits required = DiagDirToRoadBits(ReverseDiagDir(dir));
	const RoadBits present = IsValidTile(neighbour) ? GetAnyRoadBits(neighbour, rtt, false) : RoadBits{};
	const bool has_required_bit = present.Any(required);
	/* The tile the vehicle comes from must carry the piece towards the other one. */
	const bool from_ok = leaving ? own.Any(DiagDirToRoadBits(dir)) : has_required_bit;
	/* The engine's own reachability test on the tile the vehicle drives into. */
	const bool drivable = valid && from_ok &&
			(GetTileTrackStatus(entered, TransportType::Road, rtt).trackdirs & DiagdirReachesTrackdirs(motion)).Any();

	Json result = RoadNeighbourJson(tile, dir, rtt);
	result["drivable"] = drivable;
	result["connected"] = has_required_bit && drivable;
	return result;
}

/**
 * The exit of a road depot: a vehicle drives out of the depot tile into the neighbour tile.
 * @param depot_tile Tile of the depot.
 * @param dir The direction of the depot exit.
 * @param rtt The road or tram family.
 * @return The connection object described at #RoadLinkJson.
 */
Json RoadExitJson(TileIndex depot_tile, DiagDirection dir, RoadTramType rtt)
{
	return RoadLinkJson(depot_tile, dir, rtt, true);
}

/**
 * One entry of a road stop: a vehicle drives from the neighbour tile into the stop tile.
 * @param stop_tile Tile of the stop.
 * @param dir The direction from the stop tile to the neighbour the vehicle comes from.
 * @param rtt The road or tram family.
 * @return The connection object described at #RoadLinkJson.
 */
Json RoadEntryJson(TileIndex stop_tile, DiagDirection dir, RoadTramType rtt)
{
	return RoadLinkJson(stop_tile, dir, rtt, false);
}

/**
 * The entries of one road stop tile for one road or tram family.
 *
 * A bay stop is entered from the one side the engine stored for it; a drive-through stop is entered
 * from either end of the axis the engine stored, so it has two.
 * @param stop_tile Tile of the stop.
 * @param rtt The family to read.
 * @param bay True for a bay stop, false for a drive-through stop.
 * @return The list of connection objects of that family.
 */
Json RoadStopEntriesJson(TileIndex stop_tile, RoadTramType rtt, bool bay)
{
	Json entries = Json::array();
	if (bay) {
		entries.push_back(RoadEntryJson(stop_tile, GetBayRoadStopDir(stop_tile), rtt));
	} else {
		const Axis axis = GetDriveThroughStopAxis(stop_tile);
		for (const DiagDirection dir : {DiagDirection::NE, DiagDirection::SE, DiagDirection::SW, DiagDirection::NW}) {
			if (DiagDirToAxis(dir) != axis) continue;
			entries.push_back(RoadEntryJson(stop_tile, dir, rtt));
		}
	}
	return entries;
}

/* -- read-only inventory ----------------------------------------------------------------- */

/** Default number of rows one `world.query` returns. */
constexpr int64_t DEFAULT_QUERY_LIMIT = 100;
/** Largest number of rows one `world.query` may return, so an answer stays bounded. */
constexpr int64_t MAX_QUERY_LIMIT = 500;
/** Longest accepted `name_contains` filter, so a query cannot scan for an unbounded pattern. */
constexpr size_t MAX_NAME_FILTER = 64;
/** Largest number of order summaries one vehicle row carries. */
constexpr size_t MAX_ROW_ORDERS = 64;
/** Largest number of consist parts one vehicle row carries. */
constexpr size_t MAX_CONSIST_PARTS = 128;

/**
 * Readable name of a cargo type.
 * @param cargo The cargo type.
 * @return The name the game shows for the cargo, or an empty string for an invalid cargo.
 */
std::string CargoName(CargoType cargo)
{
	if (!IsValidCargoType(cargo)) return {};
	return GetString(CargoSpec::Get(cargo)->name);
}

/**
 * Readable four character label of a cargo type.
 * @param cargo The cargo type.
 * @return The label the game shows, or an empty string for an invalid cargo.
 */
std::string CargoLabelText(CargoType cargo)
{
	if (!IsValidCargoType(cargo)) return {};
	return CargoSpec::Get(cargo)->label.AsString();
}

/**
 * Read the engine's own stopped-in-depot state of the chain a vehicle belongs to.
 *
 * `Vehicle::IsStoppedInDepot()` asserts that it is asked for the head of a chain, and what it answers
 * is a property of the whole chain: it is true only when every part is in the depot
 * (`GroundVehicle::IsChainInDepot()`). A part of a chain is therefore read through the head, and a
 * vehicle that stands alone is its own head, so every row reports the state of the chain it is in.
 * @param v The vehicle.
 * @return True if and only if the chain of the vehicle is in a depot and stopped there.
 */
bool ChainIsStoppedInDepot(const Vehicle *v)
{
	return v->First()->IsStoppedInDepot();
}

/**
 * Read the engine's own depot state (`IsInDepot()`) of one vehicle.
 *
 * Every vehicle type answers this for every part of a chain, except the aircraft: `Aircraft::IsInDepot()`
 * asserts that it is asked for a primary vehicle, and the shadow of an aircraft is a part that stands in
 * the hangar of the aircraft it belongs to. The shadow is read through its aircraft and every other part
 * is read directly, so the field stays the state of that part and not the state of its chain.
 * @param v The vehicle.
 * @return True if and only if the vehicle is in the depot.
 */
bool VehicleIsInDepot(const Vehicle *v)
{
	if (v->type == VehicleType::Aircraft && !v->IsPrimaryVehicle()) return v->First()->IsInDepot();
	return v->IsInDepot();
}

/**
 * Map the state of a vehicle onto the protocol name.
 * @param v The vehicle.
 * @return Protocol name of the state the vehicle is in.
 */
std::string_view VehicleStatusName(const Vehicle *v)
{
	if (v->vehstatus.Test(VehState::Crashed)) return "crashed";
	if (v->breakdown_ctr == 1) return "broken_down";
	if (v->vehstatus.Test(VehState::Stopped)) return ChainIsStoppedInDepot(v) ? "in_depot" : "stopped";
	if (v->current_order.IsType(OT_LOADING)) return "loading";
	return "travelling";
}

/**
 * Map an order type onto the protocol name.
 * @param type The order type.
 * @return Protocol name of the order type.
 */
std::string_view OrderTypeName(OrderType type)
{
	switch (type) {
		case OT_GOTO_STATION: return "station";
		case OT_GOTO_DEPOT: return "depot";
		case OT_GOTO_WAYPOINT: return "waypoint";
		case OT_CONDITIONAL: return "conditional";
		case OT_IMPLICIT: return "implicit";
		case OT_LOADING: return "loading";
		case OT_LEAVESTATION: return "leave_station";
		case OT_DUMMY: return "dummy";
		default: return "nothing";
	}
}

/**
 * Map a load behaviour onto the protocol name.
 * @param type The load behaviour.
 * @return Protocol name of the load behaviour.
 */
std::string_view LoadTypeName(OrderLoadType type)
{
	switch (type) {
		case OrderLoadType::FullLoad: return "full";
		case OrderLoadType::FullLoadAny: return "any";
		case OrderLoadType::NoLoad: return "none";
		default: return "if_possible";
	}
}

/**
 * Map an unload behaviour onto the protocol name.
 * @param type The unload behaviour.
 * @return Protocol name of the unload behaviour.
 */
std::string_view UnloadTypeName(OrderUnloadType type)
{
	switch (type) {
		case OrderUnloadType::Unload: return "all";
		case OrderUnloadType::Transfer: return "transfer";
		case OrderUnloadType::NoUnload: return "none";
		default: return "if_possible";
	}
}

/**
 * Map the non-stop flags of an order onto the protocol name.
 * @param flags The non-stop flags.
 * @return Protocol name of the non-stop behaviour.
 */
std::string_view NonStopName(OrderNonStopFlags flags)
{
	if (flags.Test(OrderNonStopFlag::GoVia)) return "via";
	if (flags.Test(OrderNonStopFlag::NonStop)) return "non_stop";
	return "none";
}

/**
 * Map the stop location of an order onto the protocol name.
 * @param location The stop location.
 * @return Protocol name of the stop location.
 */
std::string_view StopLocationName(OrderStopLocation location)
{
	switch (location) {
		case OrderStopLocation::Middle: return "middle";
		case OrderStopLocation::FarEnd: return "far_end";
		default: return "near_end";
	}
}

/**
 * Map the depot action of an order onto the protocol name.
 * @param flags The depot action flags.
 * @return Protocol name of the depot action.
 */
std::string_view DepotActionName(OrderDepotActionFlags flags)
{
	if (flags.Test(OrderDepotActionFlag::Unbunch)) return "unbunch";
	if (flags.Test(OrderDepotActionFlag::NearestDepot)) return "nearest_depot";
	return "halt";
}

/**
 * Serialise the cargo a vehicle carries, with the capacity it has for that cargo.
 * @param v The vehicle.
 * @return JSON object with the cargo type, subtype, load and capacity.
 */
Json VehicleCargoJson(const Vehicle *v)
{
	Json cargo = Json::object();
	cargo["type"] = static_cast<uint8_t>(v->cargo_type);
	cargo["label"] = IsValidCargoType(v->cargo_type) ? Json(CargoLabelText(v->cargo_type)) : Json(nullptr);
	cargo["name"] = IsValidCargoType(v->cargo_type) ? Json(CargoName(v->cargo_type)) : Json(nullptr);
	cargo["subtype"] = v->cargo_subtype;
	cargo["load"] = v->cargo.TotalCount();
	cargo["capacity"] = v->cargo_cap;
	return cargo;
}

/**
 * Serialise the consist a primary vehicle actually hauls.
 *
 * A vehicle row is one vehicle, so the row of a locomotive says nothing about the wagons behind it:
 * a wagon row carries its own cargo but not the head it is coupled to, and a wagon that stands alone
 * in a depot is a row of the same shape as a wagon of a train. The consist is the engine's own chain,
 * walked from the primary vehicle with `Next()`, so the order of the parts and every part's own cargo
 * are the engine's values and not a reconstruction from the pool.
 *
 * Only the primary row of a chain carries this field: a chain is walked once, from its head, so a
 * query that returns every vehicle does not walk the same chain once per part.
 *
 * @param v The primary vehicle the chain starts at.
 * @return `{total_parts, truncated, parts, cargo}`: the number of parts in the chain, whether the
 *         parts list was cut at #MAX_CONSIST_PARTS, the parts in chain order with their own cargo,
 *         and the exact per-cargo load and capacity totals over the whole chain.
 */
Json VehicleConsistJson(const Vehicle *v)
{
	/** Cargo of a whole chain, per cargo type. */
	struct ConsistCargo {
		uint64_t load = 0;     ///< Cargo on board over the whole chain.
		uint64_t capacity = 0; ///< Capacity over the whole chain.
	};

	/* The totals are summed over every part even when the parts list below is truncated, and the
	 * counters are wide: the parts are bounded but the cargo of a chain is not, so a total must never
	 * wrap. The map orders the answer by cargo type, so the same chain always reads the same way. */
	std::map<CargoType, ConsistCargo> cargo_totals;

	Json parts = Json::array();
	int64_t total_parts = 0;
	for (const Vehicle *part = v; part != nullptr; part = part->Next()) {
		total_parts++;
		if (parts.size() < MAX_CONSIST_PARTS) {
			Json entry = Json::object();
			entry["ref"] = PoolRef("vehicle", part);
			entry["cargo"] = VehicleCargoJson(part);
			parts.push_back(std::move(entry));
		}

		ConsistCargo &total = cargo_totals[part->cargo_type];
		total.load += part->cargo.TotalCount();
		total.capacity += part->cargo_cap;
	}

	Json cargo = Json::array();
	for (const auto &[type, total] : cargo_totals) {
		Json entry = Json::object();
		entry["type"] = static_cast<uint8_t>(type);
		entry["label"] = IsValidCargoType(type) ? Json(CargoLabelText(type)) : Json(nullptr);
		entry["load"] = total.load;
		entry["capacity"] = total.capacity;
		cargo.push_back(std::move(entry));
	}

	Json consist = Json::object();
	consist["total_parts"] = total_parts;
	consist["truncated"] = total_parts > static_cast<int64_t>(MAX_CONSIST_PARTS);
	consist["parts"] = std::move(parts);
	consist["cargo"] = std::move(cargo);
	return consist;
}

/**
 * Union of the refit masks of every part of a vehicle chain.
 *
 * `Commands::RefitVehicle` refits the whole chain, and a train wagon or an articulated part may be
 * refittable to a cargo the front engine is not, so the compatibility check has to look at the
 * whole consist instead of the vehicle that was named.
 *
 * @param v The vehicle.
 * @return Cargo types at least one part of the chain can be refitted to.
 */
CargoTypes ConsistRefitMasks(const Vehicle *v)
{
	CargoTypes masks{};
	for (const Vehicle *part = v->First(); part != nullptr; part = part->Next()) {
		masks.Set(GetUnionOfArticulatedRefitMasks(part->engine_type, true));
	}
	return masks;
}

/**
 * Serialise the order a vehicle is currently executing.
 * @param v The vehicle.
 * @return JSON object with the order type and its destination.
 */
Json CurrentOrderJson(const Vehicle *v)
{
	const Order &order = v->current_order;
	Json current = Json::object();
	current["type"] = std::string(OrderTypeName(order.GetType()));
	current["index"] = v->cur_real_order_index;
	if (order.IsType(OT_GOTO_STATION) || order.IsType(OT_GOTO_WAYPOINT) || order.IsType(OT_GOTO_DEPOT) || order.IsType(OT_LOADING)) {
		current["destination_id"] = order.GetDestination().base();
	} else {
		current["destination_id"] = nullptr;
	}
	return current;
}

/**
 * Serialise the destination of a goto order as a typed reference.
 * @param order The order.
 * @return Reference to the station, waypoint or depot, or null for other orders.
 */
Json OrderDestinationRef(const Order *order)
{
	if (order->IsType(OT_GOTO_STATION)) {
		const Station *st = Station::GetIfValid(order->GetDestination().ToStationID());
		return st == nullptr ? Json(nullptr) : PoolRef("station", st);
	}
	if (order->IsType(OT_GOTO_WAYPOINT)) {
		const Waypoint *wp = Waypoint::GetIfValid(order->GetDestination().ToStationID());
		return wp == nullptr ? Json(nullptr) : PoolRef("waypoint", wp);
	}
	if (order->IsType(OT_GOTO_DEPOT)) {
		const Depot *depot = Depot::GetIfValid(order->GetDestination().ToDepotID());
		return depot == nullptr ? Json(nullptr) : PoolRef("depot", depot);
	}
	return Json(nullptr);
}

/**
 * Readable name of the destination of a goto order.
 * @param order The order.
 * @return The name the game shows, or an empty string when the order has no named destination.
 */
std::string OrderDestinationName(const Order *order)
{
	if (order->IsType(OT_GOTO_STATION)) {
		const Station *st = Station::GetIfValid(order->GetDestination().ToStationID());
		return st == nullptr ? std::string{} : st->GetCachedName();
	}
	if (order->IsType(OT_GOTO_WAYPOINT)) {
		const Waypoint *wp = Waypoint::GetIfValid(order->GetDestination().ToStationID());
		return wp == nullptr ? std::string{} : wp->GetCachedName();
	}
	return {};
}

/**
 * Serialise one order of a vehicle compactly, for the order list of a vehicle row.
 * @param index Index of the order in the order list.
 * @param order The order.
 * @return JSON object with the index, the type and the destination.
 */
Json OrderSummaryJson(VehicleOrderID index, const Order *order)
{
	const bool has_destination = order->IsType(OT_GOTO_STATION) || order->IsType(OT_GOTO_WAYPOINT) || order->IsType(OT_GOTO_DEPOT);
	const std::string name = OrderDestinationName(order);

	Json summary = Json::object();
	summary["index"] = index;
	summary["type"] = std::string(OrderTypeName(order->GetType()));
	summary["destination_id"] = has_destination ? Json(order->GetDestination().base()) : Json(nullptr);
	summary["destination_ref"] = OrderDestinationRef(order);
	summary["destination_name"] = name.empty() ? Json(nullptr) : Json(name);
	return summary;
}

/**
 * Add the road type of a vehicle to its row.
 *
 * A bus and a tram are both `road` vehicles, so the vehicle type cannot tell their family apart: only
 * the road type the vehicle actually runs on can. The fields are null, not false, for a vehicle that
 * has no road type at all (a train, a ship, an aircraft) and for a road vehicle whose road type is not
 * set yet, so a caller never reads "not a tram" out of a missing answer.
 *
 * @param row The row to add the fields to.
 * @param v The vehicle.
 */
void AddRoadTypeFields(Json &row, const Vehicle *v)
{
	if (v->type != VehicleType::Road) {
		row["road_type_id"] = nullptr;
		row["road_type"] = nullptr;
		row["is_tram"] = nullptr;
		return;
	}

	const RoadType roadtype = RoadVehicle::From(v)->roadtype;
	if (roadtype == INVALID_ROADTYPE) {
		row["road_type_id"] = nullptr;
		row["road_type"] = nullptr;
		row["is_tram"] = nullptr;
		return;
	}

	/* The engine's own classification, not a name or a guess. */
	const bool tram = RoadTypeIsTram(roadtype);
	row["road_type_id"] = static_cast<uint8_t>(roadtype);
	row["road_type"] = std::string(tram ? "tram" : "road");
	row["is_tram"] = tram;
}

/**
 * Add the engine name fields of a vehicle row.
 *
 * `engine_name` is the name the game shows for the engine, so a vehicle of an engine nobody renamed
 * reports its real name instead of an empty string; `engine_custom_name` is the player's override
 * on its own, null when there is none.
 *
 * @param row The vehicle row being built.
 * @param v The vehicle.
 */
void AddVehicleEngineNameFields(Json &row, const Vehicle *v)
{
	const Engine *engine = Engine::GetIfValid(v->engine_type);
	row["engine_name"] = engine != nullptr ? Json(EngineDisplayName(engine)) : Json(nullptr);
	row["engine_custom_name"] = engine != nullptr && !engine->name.empty() ? Json(engine->name) : Json(nullptr);
}

/**
 * Serialise one vehicle.
 * @param v The vehicle.
 * @return JSON row with the current state of the vehicle.
 */
Json VehicleRow(const Vehicle *v)
{
	Json row = Json::object();
	row["ref"] = PoolRef("vehicle", v);
	row["id"] = v->index.base();
	row["type"] = std::string(VehicleTypeName(v->type));
	/* The chain the vehicle belongs to, as the engine has it now. `is_primary` is the engine's own
	 * test for the vehicle that carries the orders and the income of a consist; `primary_ref` names
	 * the head of the chain only when that head really is a primary vehicle. A wagon that stands
	 * alone is its own head and is no primary vehicle, so its head is null instead of the wagon
	 * itself or a guessed front engine, and a stale head can never be reported as a live one. */
	row["is_primary"] = v->IsPrimaryVehicle();
	const Vehicle *head = v->First();
	row["primary_ref"] = head->IsPrimaryVehicle() ? PoolRef("vehicle", head) : Json(nullptr);
	/* Only the head of a chain carries the consist: the other parts of the same chain are already in
	 * that answer, so a query that returns every vehicle walks each chain once. */
	if (v->IsPrimaryVehicle()) row["consist"] = VehicleConsistJson(v);
	AddRoadTypeFields(row, v);
	row["owner"] = v->owner.base();
	row["name"] = v->name;
	row["unit_number"] = v->unitnumber;
	row["engine_id"] = v->engine_type.base();
	const Engine *engine = Engine::GetIfValid(v->engine_type);
	if (engine != nullptr) row["engine_ref"] = PoolRef("engine", engine);
	AddVehicleEngineNameFields(row, v);
	row["tile"] = TileJson(v->tile);
	row["status"] = std::string(VehicleStatusName(v));
	row["stopped"] = v->vehstatus.Test(VehState::Stopped);
	/* The engine's own stopped-in-depot state of the chain this vehicle belongs to: a part of a chain
	 * is in the depot exactly when its chain is, and the engine asks the head of the chain for it. */
	row["in_depot"] = ChainIsStoppedInDepot(v);
	/* Exactly the engine's own depot state of this vehicle (`IsInDepot()`, i.e. `RVSB_IN_DEPOT`). It
	 * stays true for a vehicle that was started but has not left the depot yet, where `in_depot` above
	 * is already false. It is a state flag, not proof of movement: a vehicle turning around on its
	 * depot tile is out of this state and still on the tile, which is what `on_depot_tile` below
	 * reports. */
	row["in_depot_state"] = VehicleIsInDepot(v);
	/* Exactly `IsDepotTile(v->tile)`: the tile the vehicle is on now is a depot tile. This is a map
	 * read of the current tile and does not depend on any vehicle state. */
	row["on_depot_tile"] = IsDepotTile(v->tile);
	row["crashed"] = v->vehstatus.Test(VehState::Crashed);
	row["broken_down"] = v->breakdown_ctr == 1;
	row["speed"] = v->cur_speed;
	row["max_speed"] = v->GetDisplayMaxSpeed();
	row["age_days"] = v->age.base();
	row["max_age_days"] = v->max_age.base();
	row["reliability"] = v->reliability;
	row["breakdowns_since_last_service"] = v->breakdowns_since_last_service;
	row["service_interval"] = v->GetServiceInterval();
	row["profit_this_year"] = static_cast<int64_t>(v->GetDisplayProfitThisYear());
	row["profit_last_year"] = static_cast<int64_t>(v->GetDisplayProfitLastYear());
	row["value"] = static_cast<int64_t>(v->value);
	row["running_cost"] = static_cast<int64_t>(v->GetRunningCost());
	row["halted_in_depot"] = v->vehstatus.Test(VehState::Stopped) && ChainIsStoppedInDepot(v);
	/* The wait state: the engine's own tick counters, the type specific ones null elsewhere. */
	row.update(VehicleWaitJson(v));

	const Group *group = Group::GetIfValid(v->group_id);
	row["group_id"] = group != nullptr ? Json(v->group_id.base()) : Json(nullptr);
	row["group_ref"] = group != nullptr ? PoolRef("group", group) : Json(nullptr);
	row["group_name"] = group != nullptr ? Json(group->name) : Json(nullptr);

	/* `Depot::GetByTile()` needs a rail, road or ship depot tile: a hangar stores no depot, so an
	 * aircraft that is stopped in its hangar has no depot reference. The head of the chain stands on
	 * the tile the whole chain stands on. */
	const bool depot_tile = IsRailDepotTile(head->tile) || IsRoadDepotTile(head->tile) || IsShipDepotTile(head->tile);
	const Depot *depot = depot_tile && ChainIsStoppedInDepot(v) ? Depot::GetByTile(head->tile) : nullptr;
	row["depot_ref"] = depot != nullptr ? PoolRef("depot", depot) : Json(nullptr);

	row["orders"] = v->GetNumOrders();
	row["current_orders"] = Json::array();
	for (VehicleOrderID index = 0; index < v->GetNumOrders() && index < static_cast<VehicleOrderID>(MAX_ROW_ORDERS); index++) {
		const Order *order = v->GetOrder(index);
		if (order != nullptr) row["current_orders"].push_back(OrderSummaryJson(index, order));
	}
	row["current_orders_truncated"] = v->GetNumOrders() > static_cast<VehicleOrderID>(MAX_ROW_ORDERS);
	row["cargo"] = VehicleCargoJson(v);
	row["current_order"] = CurrentOrderJson(v);
	row["last_station_id"] = v->last_station_visited == StationID::Invalid() ? Json(nullptr) : Json(v->last_station_visited.base());
	row["last_loading_station_id"] = v->last_loading_station == StationID::Invalid() ? Json(nullptr) : Json(v->last_loading_station.base());
	return row;
}

/**
 * Serialise one engine.
 * @param e The engine.
 * @param company Company the availability is reported for, or an invalid company to omit it.
 * @return JSON row with the purchase data of the engine.
 */
Json EngineRow(const Engine *e, CompanyID company)
{
	Json row = Json::object();
	row["ref"] = PoolRef("engine", e);
	row["id"] = e->index.base();
	row["type"] = std::string(VehicleTypeName(e->type));
	row["name"] = EngineDisplayName(e);
	row["custom_name"] = e->name.empty() ? Json(nullptr) : Json(e->name);
	row["cost"] = static_cast<int64_t>(e->GetCost());
	row["running_cost"] = static_cast<int64_t>(e->GetRunningCost());
	row["max_speed"] = e->GetDisplayMaxSpeed();
	row["cargo_id"] = static_cast<uint8_t>(e->GetDefaultCargoType());
	row["cargo_label"] = IsValidCargoType(e->GetDefaultCargoType()) ? Json(CargoLabelText(e->GetDefaultCargoType())) : Json(nullptr);
	row["cargo_name"] = IsValidCargoType(e->GetDefaultCargoType()) ? Json(CargoName(e->GetDefaultCargoType())) : Json(nullptr);
	row["capacity"] = e->GetDisplayDefaultCapacity();
	row["refittable"] = e->info.refit_mask.Any();
	row["refit_cargo_ids"] = Json::array();
	row["refit_cargo_names"] = Json::array();
	for (const CargoType cargo : e->info.refit_mask) {
		row["refit_cargo_ids"].push_back(static_cast<uint8_t>(cargo));
		row["refit_cargo_names"].push_back(CargoName(cargo));
	}
	row["intro_date"] = e->intro_date.base();
	row["intro_year"] = TimerGameCalendar::ConvertDateToYMD(e->intro_date).year.base();
	row["life_length_days"] = e->GetLifeLengthInDays().base();
	row["reliability"] = e->reliability;
	row["enabled"] = e->IsEnabled();
	if (e->IsGroundVehicle()) {
		row["power"] = e->GetPower();
		row["weight"] = e->GetDisplayWeight();
		row["tractive_effort"] = e->GetDisplayMaxTractiveEffort();
	}
	if (company != CompanyID::Invalid()) row["available"] = e->company_avail.Test(company);
	return row;
}

/**
 * Serialise one order of a vehicle.
 * @param v The vehicle the order belongs to.
 * @param index Index of the order in the order list.
 * @param order The order.
 * @return JSON row with the order and its timetable.
 */
Json OrderRow(const Vehicle *v, VehicleOrderID index, const Order *order)
{
	Json row = Json::object();
	row["vehicle_ref"] = PoolRef("vehicle", v);
	row["vehicle_id"] = v->index.base();
	row["index"] = index;
	row["type"] = std::string(OrderTypeName(order->GetType()));
	if (order->IsType(OT_GOTO_STATION) || order->IsType(OT_GOTO_WAYPOINT) || order->IsType(OT_GOTO_DEPOT)) {
		row["destination_id"] = order->GetDestination().base();
		row["destination_ref"] = OrderDestinationRef(order);
	} else {
		row["destination_id"] = nullptr;
		row["destination_ref"] = nullptr;
	}
	row["destination_name"] = order->IsType(OT_GOTO_STATION) || order->IsType(OT_GOTO_WAYPOINT) || order->IsType(OT_GOTO_DEPOT) ? Json(OrderDestinationName(order)) : Json(nullptr);
	row["load"] = std::string(LoadTypeName(order->GetLoadType()));
	row["unload"] = std::string(UnloadTypeName(order->GetUnloadType()));
	row["refit_cargo_id"] = order->IsRefit() ? Json(static_cast<uint8_t>(order->GetRefitCargo())) : Json(nullptr);
	row["refit_cargo_name"] = order->IsRefit() ? Json(CargoName(order->GetRefitCargo())) : Json(nullptr);
	row["wait_time"] = order->GetWaitTime();
	row["travel_time"] = order->GetTravelTime();
	row["max_speed"] = order->GetMaxSpeed();
	row["wait_timetabled"] = order->IsWaitTimetabled();
	row["travel_timetabled"] = order->IsTravelTimetabled();
	row["non_stop"] = std::string(NonStopName(order->GetNonStopType()));
	row["stop_location"] = std::string(StopLocationName(order->GetStopLocation()));
	row["depot_action"] = std::string(DepotActionName(order->GetDepotActionType()));
	return row;
}

/** Filters of a `world.query` call, already validated. */
struct QueryFilters {
	CompanyID company = CompanyID::Invalid(); ///< Restrict to one company, or an invalid company for every owner.
	bool company_explicit = false; ///< The caller named the company, so the company of the call must not be used.
	std::optional<VehicleType> vehicle_type; ///< Restrict to one vehicle type.
	VehicleID vehicle_id = VehicleID::Invalid(); ///< Restrict to one vehicle.
	EngineID engine_id = EngineID::Invalid(); ///< Restrict to one engine model.
	GroupID group_id = GroupID::Invalid(); ///< Restrict to one group.
	CargoType cargo = INVALID_CARGO; ///< Restrict to vehicles carrying this cargo, or engines that can carry it.
	std::string name_contains; ///< Restrict to names containing this text.
	std::string label; ///< Restrict to the cargo with this four character label.
	std::optional<bool> in_depot; ///< Restrict to vehicles inside or outside a depot.
	std::optional<bool> stopped; ///< Restrict to stopped or running vehicles.
	std::optional<bool> refittable; ///< Restrict to refittable or non-refittable engines.
	std::optional<bool> replace_protection; ///< Restrict to groups with or without replace protection.
	std::optional<bool> is_freight; ///< Restrict to freight or non-freight cargoes.
};

/**
 * Read a company id value without truncating it to the width of a company id.
 *
 * A plain `static_cast<uint8_t>` would silently turn 300 into 44 and read another company's rows;
 * an id that does not fit is rejected instead.
 *
 * @param value JSON value holding the company id.
 * @param company Receives the company id.
 * @return Error result, or an empty optional when the value is a company id.
 */
std::optional<Json> ReadCompanyIdValue(const Json &value, CompanyID &company)
{
	if (!value.is_number_integer()) return MakeError("invalid_arguments", "company_id must be an integer");
	const int64_t company_id = value.get<int64_t>();
	if (company_id < 0 || company_id > UINT8_MAX) {
		Json detail = Json::object();
		detail["company_id"] = company_id;
		detail["max"] = UINT8_MAX;
		return MakeError("invalid_arguments", "company_id does not fit in a company id", std::move(detail));
	}
	company = CompanyID(static_cast<uint8_t>(company_id));
	return std::nullopt;
}

/**
 * Read the `filters` object of a `world.query` call.
 *
 * Every filter name is checked against the ones the kind understands, so a filter that would
 * not be applied is an error instead of being silently dropped.
 *
 * @param params Method parameters.
 * @param kind Query kind, used in the error message.
 * @param allowed Filter names this kind understands.
 * @param filters Receives the filters.
 * @return Error result, or an empty optional when the filters are usable.
 */
std::optional<Json> ReadQueryFilters(const Json &params, std::string_view kind, std::initializer_list<std::string_view> allowed, QueryFilters &filters)
{
	const auto it = params.find("filters");
	if (it == params.end()) return std::nullopt;
	if (!it->is_object()) return MakeError("invalid_arguments", "filters must be an object");

	for (const auto &item : it->items()) {
		const std::string &key = item.key();
		const Json &value = item.value();

		bool known = false;
		for (const std::string_view name : allowed) {
			if (key == name) {
				known = true;
				break;
			}
		}
		if (!known) {
			Json detail = Json::object();
			detail["kind"] = std::string(kind);
			detail["filter"] = key;
			detail["supported"] = Json::array();
			for (const std::string_view name : allowed) detail["supported"].push_back(std::string(name));
			return MakeError("invalid_arguments", "filter is not read by this query kind", std::move(detail));
		}

		if (key == "company_id") {
			/* A null company explicitly asks for every owner, including objects without one. */
			if (value.is_null()) {
				filters.company = CompanyID::Invalid();
				filters.company_explicit = true;
			} else {
				if (const std::optional<Json> error = ReadCompanyIdValue(value, filters.company); error.has_value()) return error;
				filters.company_explicit = true;
			}
		} else if (key == "vehicle_type") {
			if (!value.is_string()) return MakeError("invalid_arguments", "filters.vehicle_type must be a string");
			const std::string name = value.get<std::string>();
			if (name == "train") {
				filters.vehicle_type = VehicleType::Train;
			} else if (name == "road") {
				filters.vehicle_type = VehicleType::Road;
			} else if (name == "ship") {
				filters.vehicle_type = VehicleType::Ship;
			} else if (name == "aircraft") {
				filters.vehicle_type = VehicleType::Aircraft;
			} else {
				return MakeError("invalid_arguments", "filters.vehicle_type must be one of train, road, ship, aircraft");
			}
		} else if (key == "vehicle_id") {
			if (const std::optional<Json> error = ReadPoolId(value, "filters.vehicle_id", filters.vehicle_id); error.has_value()) return error;
		} else if (key == "engine_id") {
			if (const std::optional<Json> error = ReadPoolId(value, "filters.engine_id", filters.engine_id); error.has_value()) return error;
		} else if (key == "group_id") {
			if (const std::optional<Json> error = ReadPoolId(value, "filters.group_id", filters.group_id); error.has_value()) return error;
		} else if (key == "cargo_id") {
			if (!value.is_number_integer()) return MakeError("invalid_arguments", "filters.cargo_id must be an integer");
			const int64_t cargo_id = value.get<int64_t>();
			if (cargo_id < 0 || cargo_id >= static_cast<int64_t>(NUM_CARGO)) {
				Json detail = Json::object();
				detail["cargo_id"] = cargo_id;
				detail["max"] = NUM_CARGO - 1;
				return MakeError("invalid_arguments", "filters.cargo_id is not a cargo", std::move(detail));
			}
			filters.cargo = static_cast<CargoType>(static_cast<uint8_t>(cargo_id));
		} else if (key == "name_contains") {
			if (!value.is_string()) return MakeError("invalid_arguments", "filters.name_contains must be a string");
			filters.name_contains = value.get<std::string>();
			if (filters.name_contains.size() > MAX_NAME_FILTER) {
				Json detail = Json::object();
				detail["max_length"] = MAX_NAME_FILTER;
				detail["length"] = filters.name_contains.size();
				return MakeError("invalid_arguments", "filters.name_contains is too long", std::move(detail));
			}
		} else if (key == "label") {
			if (!value.is_string()) return MakeError("invalid_arguments", "filters.label must be a string");
			filters.label = value.get<std::string>();
			if (filters.label.empty() || filters.label.size() > 4) return MakeError("invalid_arguments", "filters.label must be one to four characters");
		} else if (key == "in_depot") {
			if (!value.is_boolean()) return MakeError("invalid_arguments", "filters.in_depot must be a boolean");
			filters.in_depot = value.get<bool>();
		} else if (key == "stopped") {
			if (!value.is_boolean()) return MakeError("invalid_arguments", "filters.stopped must be a boolean");
			filters.stopped = value.get<bool>();
		} else if (key == "refittable") {
			if (!value.is_boolean()) return MakeError("invalid_arguments", "filters.refittable must be a boolean");
			filters.refittable = value.get<bool>();
		} else if (key == "replace_protection") {
			if (!value.is_boolean()) return MakeError("invalid_arguments", "filters.replace_protection must be a boolean");
			filters.replace_protection = value.get<bool>();
		} else if (key == "is_freight") {
			if (!value.is_boolean()) return MakeError("invalid_arguments", "filters.is_freight must be a boolean");
			filters.is_freight = value.get<bool>();
		}
	}
	return std::nullopt;
}

/**
 * Read the pagination and company parameters shared by every query kind.
 *
 * A top-level `company_id` wins over `filters.company_id`. `company_id: null` asks for every
 * owner, including objects that have none. When the caller names no company, a company scoped
 * kind is answered for the company of the call, and the scope is reported in the result, so a
 * default is never silent. The page is bounded: `limit` is clamped to
 * [#DEFAULT_QUERY_LIMIT] .. [#MAX_QUERY_LIMIT] and `offset` is never negative.
 *
 * @param params Method parameters.
 * @param company Company of the call.
 * @param company_scoped Whether the kind answers for one company and may fall back to it.
 * @param filters Receives the company filter.
 * @param limit Receives the row limit.
 * @param offset Receives the row offset.
 * @return Error result, or an empty optional when the parameters are usable.
 */
std::optional<Json> ReadQueryPaging(const Json &params, CompanyID company, bool company_scoped, QueryFilters &filters, int64_t &limit, int64_t &offset)
{
	limit = DEFAULT_QUERY_LIMIT;
	offset = 0;
	if (!ReadOptionalInt(params, "limit", limit)) return MakeError("invalid_arguments", "limit must be an integer");
	if (!ReadOptionalInt(params, "offset", offset)) return MakeError("invalid_arguments", "offset must be an integer");
	limit = Clamp<int64_t>(limit, 1, MAX_QUERY_LIMIT);
	offset = std::max<int64_t>(offset, 0);

	const auto company_param = params.find("company_id");
	if (company_param != params.end()) {
		if (company_param->is_null()) {
			filters.company = CompanyID::Invalid();
			filters.company_explicit = true;
		} else {
			if (const std::optional<Json> error = ReadCompanyIdValue(*company_param, filters.company); error.has_value()) return error;
			filters.company_explicit = true;
		}
	}
	if (!filters.company_explicit && company_scoped) filters.company = company;
	return std::nullopt;
}

/**
 * Add the page and the ownership scope a query was answered with.
 *
 * The agent has to be able to tell an empty answer from a page of a longer answer, and has to see
 * which owner the rows were filtered to, so both are part of every query result.
 *
 * @param result Result to extend.
 * @param filters The filters that were applied.
 * @param total Number of matching rows before pagination.
 * @param limit The row limit that was applied.
 * @param offset The row offset that was applied.
 * @param returned Number of rows in this answer.
 */
void AddQueryMeta(Json &result, const QueryFilters &filters, int64_t total, int64_t limit, int64_t offset, size_t returned)
{
	result["total"] = total;
	result["limit"] = limit;
	result["offset"] = offset;
	result["has_more"] = offset + static_cast<int64_t>(returned) < total;
	result["scope"] = Json::object();
	result["scope"]["company_id"] = filters.company == CompanyID::Invalid() ? Json(nullptr) : Json(filters.company.base());
}

/**
 * Read the vehicle inventory.
 * @param params Method parameters: `limit`, `offset`, `company_id`, `filters`.
 * @param company Company of the call, used when no company filter is given.
 * @return Tool result with the matching rows and the total count.
 */
Json QueryVehicles(const Json &params, CompanyID company)
{
	QueryFilters filters;
	if (const std::optional<Json> error = ReadQueryFilters(params, "vehicles",
			{"company_id", "vehicle_type", "vehicle_id", "group_id", "cargo_id", "name_contains", "in_depot", "stopped"}, filters); error.has_value()) return *error;

	int64_t limit = 0;
	int64_t offset = 0;
	if (const std::optional<Json> error = ReadQueryPaging(params, company, true, filters, limit, offset); error.has_value()) return *error;

	Json rows = Json::array();
	int64_t total = 0;
	for (const Vehicle *v : Vehicle::Iterate()) {
		if (filters.company != CompanyID::Invalid() && v->owner != filters.company) continue;
		if (filters.vehicle_type.has_value() && v->type != *filters.vehicle_type) continue;
		if (filters.vehicle_id != VehicleID::Invalid() && v->index != filters.vehicle_id) continue;
		if (filters.group_id != GroupID::Invalid() && v->group_id != filters.group_id) continue;
		if (IsValidCargoType(filters.cargo) && v->cargo_type != filters.cargo) continue;
		if (!filters.name_contains.empty() && v->name.find(filters.name_contains) == std::string::npos) continue;
		if (filters.in_depot.has_value() && ChainIsStoppedInDepot(v) != *filters.in_depot) continue;
		if (filters.stopped.has_value() && v->vehstatus.Test(VehState::Stopped) != *filters.stopped) continue;
		if (total++ < offset) continue;
		if (static_cast<int64_t>(rows.size()) >= limit) continue;
		rows.push_back(VehicleRow(v));
	}

	Json result = Json::object();
	result["ok"] = true;
	result["kind"] = "vehicles";
	result["rows"] = std::move(rows);
	AddQueryMeta(result, filters, total, limit, offset, result["rows"].size());
	return result;
}

/**
 * Read the engine inventory.
 * @param params Method parameters: `limit`, `offset`, `company_id`, `filters`.
 * @param company Company of the call, used for the availability flag.
 * @return Tool result with the matching rows and the total count.
 */
Json QueryEngines(const Json &params, CompanyID company)
{
	QueryFilters filters;
	if (const std::optional<Json> error = ReadQueryFilters(params, "engines",
			{"company_id", "vehicle_type", "engine_id", "cargo_id", "name_contains", "refittable"}, filters); error.has_value()) return *error;

	int64_t limit = 0;
	int64_t offset = 0;
	/* Engines belong to no company, so the company only selects whose availability is reported. */
	if (const std::optional<Json> error = ReadQueryPaging(params, company, false, filters, limit, offset); error.has_value()) return *error;
	const CompanyID availability = filters.company_explicit ? filters.company : company;

	Json rows = Json::array();
	int64_t total = 0;
	for (const Engine *e : Engine::Iterate()) {
		if (filters.vehicle_type.has_value() && e->type != *filters.vehicle_type) continue;
		if (filters.engine_id != EngineID::Invalid() && e->index != filters.engine_id) continue;
		if (IsValidCargoType(filters.cargo) && e->GetDefaultCargoType() != filters.cargo && !e->info.refit_mask.Test(filters.cargo)) continue;
		if (!filters.name_contains.empty() && EngineDisplayName(e).find(filters.name_contains) == std::string::npos) continue;
		if (filters.refittable.has_value() && e->info.refit_mask.Any() != *filters.refittable) continue;
		if (total++ < offset) continue;
		if (static_cast<int64_t>(rows.size()) >= limit) continue;
		rows.push_back(EngineRow(e, availability));
	}

	Json result = Json::object();
	result["ok"] = true;
	result["kind"] = "engines";
	result["rows"] = std::move(rows);
	AddQueryMeta(result, filters, total, limit, offset, result["rows"].size());
	return result;
}

/**
 * Read the order lists of the vehicles of a company.
 * @param params Method parameters: `limit`, `offset`, `company_id`, `filters`.
 * @param company Company of the call, used when no company filter is given.
 * @return Tool result with one row per order and the total count.
 */
Json QueryOrders(const Json &params, CompanyID company)
{
	QueryFilters filters;
	if (const std::optional<Json> error = ReadQueryFilters(params, "orders",
			{"company_id", "vehicle_id"}, filters); error.has_value()) return *error;

	int64_t limit = 0;
	int64_t offset = 0;
	if (const std::optional<Json> error = ReadQueryPaging(params, company, true, filters, limit, offset); error.has_value()) return *error;

	Json rows = Json::array();
	int64_t total = 0;
	for (const Vehicle *v : Vehicle::Iterate()) {
		if (!v->IsPrimaryVehicle()) continue;
		if (filters.company != CompanyID::Invalid() && v->owner != filters.company) continue;
		if (filters.vehicle_id != VehicleID::Invalid() && v->index != filters.vehicle_id) continue;
		for (VehicleOrderID index = 0; index < v->GetNumOrders(); index++) {
			const Order *order = v->GetOrder(index);
			if (order == nullptr) continue;
			if (total++ < offset) continue;
			if (static_cast<int64_t>(rows.size()) >= limit) continue;
			rows.push_back(OrderRow(v, index, order));
		}
	}

	Json result = Json::object();
	result["ok"] = true;
	result["kind"] = "orders";
	result["rows"] = std::move(rows);
	AddQueryMeta(result, filters, total, limit, offset, result["rows"].size());
	return result;
}

/**
 * Serialise one group.
 * @param g The group.
 * @return JSON row with the group, its parent and its statistics.
 */
Json GroupRow(const Group *g)
{
	Json row = Json::object();
	row["ref"] = PoolRef("group", g);
	row["id"] = g->index.base();
	row["name"] = g->name;
	row["owner"] = g->owner.base();
	row["vehicle_type"] = std::string(VehicleTypeName(g->vehicle_type));
	row["number"] = g->number;
	row["replace_protection"] = g->flags.Test(GroupFlag::ReplaceProtection);

	const Group *parent = Group::GetIfValid(g->parent);
	row["parent_id"] = parent != nullptr ? Json(g->parent.base()) : Json(nullptr);
	row["parent_ref"] = parent != nullptr ? PoolRef("group", parent) : Json(nullptr);

	row["num_vehicles"] = g->statistics.num_vehicle;
	row["profit_last_year"] = static_cast<int64_t>(g->statistics.profit_last_year);
	row["num_engines"] = Json::object();
	for (const auto &[engine, count] : g->statistics.num_engines) row["num_engines"][fmt::format("{}", engine.base())] = count;
	return row;
}

/**
 * Read the group inventory.
 * @param params Method parameters: `limit`, `offset`, `company_id`, `filters`.
 * @param company Company of the call, used when no company filter is given.
 * @return Tool result with the matching rows and the total count.
 */
Json QueryGroups(const Json &params, CompanyID company)
{
	QueryFilters filters;
	if (const std::optional<Json> error = ReadQueryFilters(params, "groups",
			{"company_id", "group_id", "vehicle_type", "name_contains", "replace_protection"}, filters); error.has_value()) return *error;

	int64_t limit = 0;
	int64_t offset = 0;
	if (const std::optional<Json> error = ReadQueryPaging(params, company, true, filters, limit, offset); error.has_value()) return *error;

	Json rows = Json::array();
	int64_t total = 0;
	for (const Group *g : Group::Iterate()) {
		if (filters.company != CompanyID::Invalid() && g->owner != filters.company) continue;
		if (filters.group_id != GroupID::Invalid() && g->index != filters.group_id) continue;
		if (filters.vehicle_type.has_value() && g->vehicle_type != *filters.vehicle_type) continue;
		if (!filters.name_contains.empty() && g->name.find(filters.name_contains) == std::string::npos) continue;
		if (filters.replace_protection.has_value() && g->flags.Test(GroupFlag::ReplaceProtection) != *filters.replace_protection) continue;
		if (total++ < offset) continue;
		if (static_cast<int64_t>(rows.size()) >= limit) continue;
		rows.push_back(GroupRow(g));
	}

	Json result = Json::object();
	result["ok"] = true;
	result["kind"] = "groups";
	result["rows"] = std::move(rows);
	AddQueryMeta(result, filters, total, limit, offset, result["rows"].size());
	return result;
}

/**
 * Serialise one cargo type.
 * @param cs The cargo specification.
 * @return JSON row with the id, the label and the values the game shows.
 */
Json CargoRow(const CargoSpec *cs)
{
	Json row = Json::object();
	row["id"] = static_cast<uint8_t>(cs->Index());
	row["label"] = cs->label.AsString();
	row["name"] = GetString(cs->name);
	row["abbrev"] = GetString(cs->abbrev);
	row["is_freight"] = cs->is_freight;
	row["weight"] = cs->weight;
	row["payment"] = static_cast<int64_t>(cs->current_payment);
	return row;
}

/**
 * Read the cargo inventory.
 *
 * Cargoes belong to no company, so `company_id` is accepted but does not filter them.
 *
 * @param params Method parameters: `limit`, `offset`, `filters`.
 * @param company Company of the call.
 * @return Tool result with the matching rows and the total count.
 */
Json QueryCargos(const Json &params, CompanyID company)
{
	QueryFilters filters;
	if (const std::optional<Json> error = ReadQueryFilters(params, "cargos",
			{"cargo_id", "label", "name_contains", "is_freight"}, filters); error.has_value()) return *error;

	int64_t limit = 0;
	int64_t offset = 0;
	if (const std::optional<Json> error = ReadQueryPaging(params, company, false, filters, limit, offset); error.has_value()) return *error;

	Json rows = Json::array();
	int64_t total = 0;
	for (const CargoSpec *cs : CargoSpec::Iterate()) {
		if (filters.cargo != INVALID_CARGO && cs->Index() != filters.cargo) continue;
		if (!filters.label.empty() && cs->label.AsString() != filters.label) continue;
		if (!filters.name_contains.empty() && GetString(cs->name).find(filters.name_contains) == std::string::npos) continue;
		if (filters.is_freight.has_value() && cs->is_freight != *filters.is_freight) continue;
		if (total++ < offset) continue;
		if (static_cast<int64_t>(rows.size()) >= limit) continue;
		rows.push_back(CargoRow(cs));
	}

	Json result = Json::object();
	result["ok"] = true;
	result["kind"] = "cargos";
	result["rows"] = std::move(rows);
	AddQueryMeta(result, filters, total, limit, offset, result["rows"].size());
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

	/* A depot whose entrance tile has no road on the side facing the depot is accepted by
	 * CmdBuildRoadDepot and then traps every vehicle that leaves it. A depot that is on the map is
	 * read as it is now, exactly as `world.query` and `world.objects` read it, so a caller that lays
	 * the road afterwards can see the answer change. A dry run builds nothing and a posted command
	 * may not have run yet: then the depot tile carries no exit piece, so only the neighbour is
	 * reported and no `connected` is claimed. */
	if (!dry_run && IsRoadDepotTile(tile)) {
		result["entrance"] = RoadDepotEntranceJson(tile);
	} else {
		result["entrance"] = RoadDepotExitProposalJson(tile, dir, GetRoadTramType(rt));
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
	EngineID engine_id = EngineID::Invalid();
	if (!params.contains("engine_id")) return MakeError("invalid_arguments", "build needs engine_id");
	if (const std::optional<Json> error = ReadPoolIdParam(params, "engine_id", engine_id); error.has_value()) return *error;

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
	if (const std::optional<Json> error = ReadCargoId(params, "cargo_id", cargo); error.has_value()) return *error;

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

	const bool share = params.contains("share_orders_from");
	VehicleID share_from = VehicleID::Invalid();
	if (const std::optional<Json> error = ReadPoolIdParam(params, "share_orders_from", share_from); error.has_value()) return *error;

	DepotID depot_id = DepotID::Invalid();
	const bool has_depot = params.contains("depot_id");
	if (const std::optional<Json> error = ReadPoolIdParam(params, "depot_id", depot_id); error.has_value()) return *error;

	bool dry_run = false;
	if (!ReadOptionalBool(params, "dry_run", dry_run)) return MakeError("invalid_arguments", "dry_run must be a boolean");

	if (const std::optional<Json> error = CheckWriteContext(company, dry_run); error.has_value()) return *error;

	const Engine *e = Engine::GetIfValid(engine_id);
	if (e == nullptr) {
		Json detail = Json::object();
		detail["engine_id"] = engine_id.base();
		return MakeError("not_found", "engine does not exist", std::move(detail));
	}

	TileIndex tile = INVALID_TILE;
	if (has_depot) {
		const Depot *d = Depot::GetIfValid(depot_id);
		if (d == nullptr) {
			Json detail = Json::object();
			detail["depot_id"] = depot_id.base();
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
		const Vehicle *source = Vehicle::GetIfValid(share_from);
		if (source == nullptr) {
			Json detail = Json::object();
			detail["share_orders_from"] = share_from.base();
			return MakeError("not_found", "share_orders_from vehicle does not exist", std::move(detail));
		}
		if (source->owner != company) {
			Json detail = Json::object();
			detail["share_orders_from"] = share_from.base();
			detail["owner"] = source->owner.base();
			return MakeError("invalid_arguments", "share_orders_from vehicle belongs to another company", std::move(detail));
		}
		if (source->type != e->type) {
			Json detail = Json::object();
			detail["share_orders_from"] = share_from.base();
			detail["type"] = std::string(VehicleTypeName(source->type));
			return MakeError("invalid_arguments", "share_orders_from vehicle has another type", std::move(detail));
		}
		if (!source->IsPrimaryVehicle()) {
			Json detail = Json::object();
			detail["share_orders_from"] = share_from.base();
			return MakeError("invalid_arguments", "share_orders_from vehicle is not a primary vehicle", std::move(detail));
		}
		source_id = source->index;
	}

	CompanyContext context(company);

	Money spent = 0;
	Json created = Json::array();
	for (int64_t i = 0; i < count; i++) {
		if (!RunCommand<Commands::BuildVehicle>(dry_run, &AgentBuildVehicleCallback, tile, e->index, false, cargo, ClientID::Invalid)) {
			Json result = Json::object();
			result["op"] = "build";
			result["vehicle_ids"] = std::move(created);
			if (!_posted.captured) return NotExecuted(std::move(result));
			return MarkExecution(PartialResult(std::move(result), spent, _posted), dry_run);
		}
		const VehicleID veh_id = _posted.vehicle;
		spent += _posted.cost.GetCost();
		if (!dry_run) created.push_back(veh_id.base());

		if (rename && !dry_run) {
			if (!RunCommand<Commands::RenameVehicle>(false, &AgentCommandCallback, veh_id, name)) {
				Json result = Json::object();
				result["op"] = "build";
				result["vehicle_ids"] = std::move(created);
				result["renamed"] = false;
				if (!_posted.captured) return NotExecuted(std::move(result));
				return MarkExecution(PartialResult(std::move(result), spent, _posted), dry_run);
			}
			spent += _posted.cost.GetCost();
		}

		if (source_id != VehicleID::Invalid() && !dry_run) {
			if (!RunCommandAt<Commands::CloneOrder>(false, tile, &AgentCommandCallback, CO_SHARE, veh_id, source_id)) {
				Json result = Json::object();
				result["op"] = "build";
				result["vehicle_ids"] = std::move(created);
				result["orders_shared"] = false;
				if (!_posted.captured) return NotExecuted(std::move(result));
				return MarkExecution(PartialResult(std::move(result), spent, _posted), dry_run);
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
	return MarkExecution(std::move(result), dry_run);
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
	VehicleID vehicle_id = VehicleID::Invalid();
	if (!params.contains("vehicle_id")) return MakeError("invalid_arguments", "start/stop needs vehicle_id");
	if (const std::optional<Json> error = ReadPoolIdParam(params, "vehicle_id", vehicle_id); error.has_value()) return *error;

	bool dry_run = false;
	if (!ReadOptionalBool(params, "dry_run", dry_run)) return MakeError("invalid_arguments", "dry_run must be a boolean");

	if (const std::optional<Json> error = CheckWriteContext(company, dry_run); error.has_value()) return *error;

	Vehicle *v = Vehicle::GetIfValid(vehicle_id);
	if (v == nullptr) {
		Json detail = Json::object();
		detail["vehicle_id"] = vehicle_id.base();
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

	if (!RunCommandAt<Commands::StartStopVehicle>(dry_run, v->tile, &AgentCommandCallback, veh_id, false)) {
		result["changed"] = false;
		if (!_posted.captured) return NotExecuted(std::move(result));
		return MarkExecution(CommandResult(_posted, std::move(result)), dry_run);
	}

	result["changed"] = !dry_run;
	const Vehicle *after = Vehicle::GetIfValid(veh_id);
	result["stopped"] = after != nullptr ? after->vehstatus.Test(VehState::Stopped) : stopped;
	return MarkExecution(CommandResult(_posted, std::move(result)), dry_run);
}

/**
 * Sell a vehicle.
 * @param params Method parameters.
 * @param company Company the vehicle belongs to.
 * @return Tool result with the sold vehicle id and the cost.
 */
Json VehicleSell(const Json &params, CompanyID company)
{
	VehicleID vehicle_id = VehicleID::Invalid();
	if (!params.contains("vehicle_id")) return MakeError("invalid_arguments", "sell needs vehicle_id");
	if (const std::optional<Json> error = ReadPoolIdParam(params, "vehicle_id", vehicle_id); error.has_value()) return *error;

	DepotID depot_id = DepotID::Invalid();
	const bool has_depot = params.contains("depot_id");
	if (const std::optional<Json> error = ReadPoolIdParam(params, "depot_id", depot_id); error.has_value()) return *error;

	bool dry_run = false;
	if (!ReadOptionalBool(params, "dry_run", dry_run)) return MakeError("invalid_arguments", "dry_run must be a boolean");

	if (const std::optional<Json> error = CheckWriteContext(company, dry_run); error.has_value()) return *error;

	Vehicle *v = Vehicle::GetIfValid(vehicle_id);
	if (v == nullptr) {
		Json detail = Json::object();
		detail["vehicle_id"] = vehicle_id.base();
		return MakeError("not_found", "vehicle does not exist", std::move(detail));
	}

	/* `depot_id` is documented for sell as well; when it is given the vehicle has to be there. */
	if (has_depot) {
		const Depot *d = Depot::GetIfValid(depot_id);
		if (d == nullptr) {
			Json detail = Json::object();
			detail["depot_id"] = depot_id.base();
			return MakeError("not_found", "depot does not exist", std::move(detail));
		}
		if (v->tile != d->xy) {
			Json detail = Json::object();
			detail["vehicle_id"] = vehicle_id.base();
			detail["depot_id"] = depot_id.base();
			detail["tile"] = TileJson(v->tile);
			return MakeError("invalid_arguments", "the vehicle is not in that depot", std::move(detail));
		}
	}

	CompanyContext context(company);

	Json result = Json::object();
	result["op"] = "sell";
	result["vehicle_id"] = v->index.base();
	if (!RunCommandAt<Commands::SellVehicle>(dry_run, v->tile, &AgentCommandCallback, v->index, false, false, ClientID::Invalid)) {
		if (!_posted.captured) return NotExecuted(std::move(result));
	}
	return MarkExecution(CommandResult(_posted, std::move(result)), dry_run);
}

/**
 * Refit a vehicle to another cargo.
 *
 * The refit masks of the whole consist (the engine, its articulated parts and every wagon) are
 * what the engine itself tests, so an incompatible cargo is rejected before the command is posted.
 * The result reports the capacities the engine actually produced, not the ones that were asked for.
 *
 * @param params Method parameters.
 * @param company Company the vehicle belongs to.
 * @return Tool result with the new cargo, the capacities and the cost.
 */
Json VehicleRefit(const Json &params, CompanyID company)
{
	VehicleID vehicle_id = VehicleID::Invalid();
	if (!params.contains("vehicle_id")) return MakeError("invalid_arguments", "refit needs vehicle_id");
	if (const std::optional<Json> error = ReadPoolIdParam(params, "vehicle_id", vehicle_id); error.has_value()) return *error;

	CargoType cargo = INVALID_CARGO;
	if (const std::optional<Json> error = ReadCargoId(params, "cargo_id", cargo); error.has_value()) return *error;
	if (!IsValidCargoType(cargo)) return MakeError("invalid_arguments", "refit needs cargo_id");

	bool only_this = false;
	if (!ReadOptionalBool(params, "only_this", only_this)) return MakeError("invalid_arguments", "only_this must be a boolean");

	bool dry_run = false;
	if (!ReadOptionalBool(params, "dry_run", dry_run)) return MakeError("invalid_arguments", "dry_run must be a boolean");

	if (const std::optional<Json> error = CheckWriteContext(company, dry_run); error.has_value()) return *error;

	Vehicle *v = Vehicle::GetIfValid(vehicle_id);
	if (v == nullptr) {
		Json detail = Json::object();
		detail["vehicle_id"] = vehicle_id.base();
		return MakeError("not_found", "vehicle does not exist", std::move(detail));
	}
	if (v->owner != company) {
		Json detail = Json::object();
		detail["vehicle_id"] = vehicle_id.base();
		detail["owner"] = v->owner.base();
		return MakeError("not_owned", "vehicle belongs to another company", std::move(detail));
	}
	if (!IsCompanyBuildableVehicleType(v)) {
		Json detail = Json::object();
		detail["vehicle_id"] = vehicle_id.base();
		detail["type"] = std::string(VehicleTypeName(v->type));
		return MakeError("invalid_arguments", "this vehicle type cannot be refitted", std::move(detail));
	}

	/* `Commands::RefitVehicle` refits the whole chain unless `only_this` is set, and the chain has
	 * to be stopped in a depot. Both are checked before anything is mutated. */
	Vehicle *front = v->First();
	if (front->vehstatus.Test(VehState::Crashed)) {
		Json detail = Json::object();
		detail["vehicle_id"] = vehicle_id.base();
		return MakeError("invalid_arguments", "a crashed vehicle cannot be refitted", std::move(detail));
	}
	if (!only_this && !front->IsStoppedInDepot()) {
		Json detail = Json::object();
		detail["vehicle_id"] = vehicle_id.base();
		detail["tile"] = TileJson(front->tile);
		return MakeError("invalid_arguments", "the vehicle must be stopped in a depot to be refitted", std::move(detail));
	}

	/* `only_this` refits just the named vehicle, so only its own parts have to accept the cargo. */
	const CargoTypes refittable = only_this ? GetUnionOfArticulatedRefitMasks(v->engine_type, true) : ConsistRefitMasks(v);
	if (!refittable.Test(cargo)) {
		Json detail = Json::object();
		detail["vehicle_id"] = vehicle_id.base();
		detail["engine_id"] = v->engine_type.base();
		detail["cargo_id"] = static_cast<uint8_t>(cargo);
		detail["refit_cargo_ids"] = Json::array();
		for (const CargoType option : refittable) detail["refit_cargo_ids"].push_back(static_cast<uint8_t>(option));
		return MakeError("invalid_arguments", "the vehicle cannot be refitted to that cargo", std::move(detail));
	}

	CompanyContext context(company);

	Json result = Json::object();
	result["op"] = "refit";
	result["vehicle_id"] = v->index.base();
	result["cargo_id"] = static_cast<uint8_t>(cargo);
	result["only_this"] = only_this;
	/* 0xFF asks the engine for the best fitting subtype, which is what the depot window does. */
	if (!RunCommandAt<Commands::RefitVehicle>(dry_run, v->tile, &AgentRefitVehicleCallback, v->index, cargo, 0xFF, false, only_this, 0)) {
		if (!_posted.captured) return NotExecuted(std::move(result));
		return MarkExecution(CommandResult(_posted, std::move(result)), dry_run);
	}

	if (!dry_run) {
		result["refit_capacity"] = _posted.refit_capacity;
		result["mail_capacity"] = _posted.mail_capacity;
		result["cargo_capacities"] = Json::object();
		for (const CargoType option : EnumRange(NUM_CARGO)) {
			if (_posted.cargo_capacities[option] == 0) continue;
			result["cargo_capacities"][fmt::format("{}", static_cast<uint8_t>(option))] = _posted.cargo_capacities[option];
		}
		const Vehicle *after = Vehicle::GetIfValid(v->index);
		if (after != nullptr) result["cargo"] = VehicleCargoJson(after);
	}
	return MarkExecution(CommandResult(_posted, std::move(result)), dry_run);
}

/**
 * Clone a vehicle, optionally sharing its orders.
 *
 * The depot the clone is built in, its owner and the availability of the engine are checked
 * before the command is posted, so a clone the engine would reject never reaches the world.
 *
 * @param params Method parameters.
 * @param company Company that pays and owns the clone.
 * @return Tool result with the created vehicle ids and the cost.
 */
Json VehicleClone(const Json &params, CompanyID company)
{
	VehicleID vehicle_id = VehicleID::Invalid();
	if (!params.contains("vehicle_id")) return MakeError("invalid_arguments", "clone needs vehicle_id");
	if (const std::optional<Json> error = ReadPoolIdParam(params, "vehicle_id", vehicle_id); error.has_value()) return *error;

	DepotID depot_id = DepotID::Invalid();
	const bool has_depot = params.contains("depot_id");
	if (const std::optional<Json> error = ReadPoolIdParam(params, "depot_id", depot_id); error.has_value()) return *error;

	int64_t count = 1;
	if (!ReadOptionalInt(params, "count", count)) return MakeError("invalid_arguments", "count must be an integer");
	if (count < 1 || count > 100) {
		Json detail = Json::object();
		detail["count"] = count;
		detail["min"] = 1;
		detail["max"] = 100;
		return MakeError("invalid_arguments", "count must be between 1 and 100", std::move(detail));
	}

	bool share_orders = false;
	if (!ReadOptionalBool(params, "share_orders", share_orders)) return MakeError("invalid_arguments", "share_orders must be a boolean");

	bool dry_run = false;
	if (!ReadOptionalBool(params, "dry_run", dry_run)) return MakeError("invalid_arguments", "dry_run must be a boolean");

	if (const std::optional<Json> error = CheckWriteContext(company, dry_run); error.has_value()) return *error;

	Vehicle *v = Vehicle::GetIfValid(vehicle_id);
	if (v == nullptr) {
		Json detail = Json::object();
		detail["vehicle_id"] = vehicle_id.base();
		return MakeError("not_found", "vehicle does not exist", std::move(detail));
	}
	if (v->owner != company) {
		Json detail = Json::object();
		detail["vehicle_id"] = vehicle_id.base();
		detail["owner"] = v->owner.base();
		return MakeError("not_owned", "vehicle belongs to another company", std::move(detail));
	}
	if (!IsCompanyBuildableVehicleType(v) || !v->IsPrimaryVehicle()) {
		Json detail = Json::object();
		detail["vehicle_id"] = vehicle_id.base();
		detail["type"] = std::string(VehicleTypeName(v->type));
		return MakeError("invalid_arguments", "clone needs a primary vehicle of a buildable type", std::move(detail));
	}

	/* The clone is built in a depot of the company; the tile of the source vehicle is used when
	 * the caller names neither a depot nor a tile, which is what the depot window does. */
	TileIndex tile = v->tile;
	if (has_depot) {
		const Depot *d = Depot::GetIfValid(depot_id);
		if (d == nullptr) {
			Json detail = Json::object();
			detail["depot_id"] = depot_id.base();
			return MakeError("not_found", "depot does not exist", std::move(detail));
		}
		tile = d->xy;
	} else if (params.contains("tile")) {
		RawTile raw{};
		if (!ReadRawTile(params["tile"], raw)) return MakeError("invalid_arguments", "tile must be a tile object with integer x and y");
		if (!ToTileIndex(raw, tile)) return MakeError("invalid_arguments", "tile must be inside the map");
	}

	if (!IsDepotTile(tile)) {
		Json detail = Json::object();
		detail["tile"] = TileJson(tile);
		return MakeError("invalid_arguments", "clone needs a depot tile", std::move(detail));
	}
	if (GetDepotVehicleType(tile) != v->type) {
		Json detail = Json::object();
		detail["tile"] = TileJson(tile);
		detail["depot_type"] = std::string(VehicleTypeName(GetDepotVehicleType(tile)));
		detail["vehicle_type"] = std::string(VehicleTypeName(v->type));
		return MakeError("invalid_arguments", "the depot holds another vehicle type", std::move(detail));
	}
	if (!IsTileOwner(tile, company)) {
		Json detail = Json::object();
		detail["tile"] = TileJson(tile);
		return MakeError("not_owned", "the depot belongs to another company", std::move(detail));
	}
	if (!Engine::Get(v->engine_type)->company_avail.Test(company)) {
		Json detail = Json::object();
		detail["engine_id"] = v->engine_type.base();
		return MakeError("invalid_arguments", "the engine is not available to the company", std::move(detail));
	}

	const VehicleID source_id = v->index;
	CompanyContext context(company);

	Money spent = 0;
	Json created = Json::array();
	for (int64_t i = 0; i < count; i++) {
		if (!RunCommand<Commands::CloneVehicle>(dry_run, &AgentCloneVehicleCallback, tile, source_id, share_orders)) {
			Json result = Json::object();
			result["op"] = "clone";
			result["vehicle_id"] = source_id.base();
			result["share_orders"] = share_orders;
			result["tile"] = TileJson(tile);
			result["vehicle_ids"] = std::move(created);
			if (!_posted.captured) return NotExecuted(std::move(result));
			return MarkExecution(PartialResult(std::move(result), spent, _posted), dry_run);
		}
		spent += _posted.cost.GetCost();
		if (!dry_run) created.push_back(_posted.vehicle.base());
	}

	Json result = Json::object();
	result["ok"] = true;
	result["op"] = "clone";
	result["vehicle_id"] = source_id.base();
	result["share_orders"] = share_orders;
	result["tile"] = TileJson(tile);
	result["vehicle_ids"] = std::move(created);
	result["cost"] = static_cast<int64_t>(spent);
	return MarkExecution(std::move(result), dry_run);
}

/* -- orders ------------------------------------------------------------------------------ */

/**
 * Build one order from the tool's order object.
 *
 * The id of a goto order is checked against the pool it names before the cast, so an id that does
 * not fit is `invalid_params` instead of silently naming another station, waypoint or depot.
 *
 * @param entry The order object.
 * @param order Receives the order.
 * @return Error result, or an empty optional when the order was built.
 */
std::optional<Json> ReadOrder(const Json &entry, Order &order)
{
	if (!entry.is_object()) return MakeError("invalid_arguments", "order must be an object");

	std::string type;
	if (!ReadOptionalString(entry, "type", type)) return MakeError("invalid_arguments", "order type must be a string");
	if (type.empty()) type = "station";

	if (!entry.contains("id")) return MakeError("invalid_arguments", "order needs an id");
	if (type == "station" || type == "waypoint") {
		StationID station = StationID::Invalid();
		if (const std::optional<Json> error = ReadPoolIdParam(entry, "id", station); error.has_value()) return error;
		if (type == "station") {
			order.MakeGoToStation(station);
			/* CmdInsertOrder only accepts the near end and the middle of a platform for trains, and
			 * MakeGoToStation() leaves the stop location at the near end: a bus, tram, ship or
			 * aircraft order built here would be refused with a bare command failure. Ask for the
			 * far end, the value the order window uses for every non-train and the one old savegames
			 * are given, which is valid for trains too. */
			order.SetStopLocation(OrderStopLocation::FarEnd);
		} else {
			order.MakeGoToWaypoint(station);
		}
	} else if (type == "depot") {
		DepotID depot = DepotID::Invalid();
		if (const std::optional<Json> error = ReadPoolIdParam(entry, "id", depot); error.has_value()) return error;
		order.MakeGoToDepot(depot, OrderDepotTypeFlag::PartOfOrders);
	} else {
		return MakeError("invalid_arguments", "unsupported order type `" + type + "`");
	}

	std::string load;
	if (!ReadOptionalString(entry, "load", load)) return MakeError("invalid_arguments", "order load behaviour must be a string");
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
			return MakeError("invalid_arguments", "unsupported load behaviour `" + load + "`");
		}
	}

	std::string unload;
	if (!ReadOptionalString(entry, "unload", unload)) return MakeError("invalid_arguments", "order unload behaviour must be a string");
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
			return MakeError("invalid_arguments", "unsupported unload behaviour `" + unload + "`");
		}
	}

	CargoType refit_cargo = INVALID_CARGO;
	if (const std::optional<Json> error = ReadCargoId(entry, "refit_cargo_id", refit_cargo); error.has_value()) return error;
	if (refit_cargo != INVALID_CARGO) order.SetRefit(refit_cargo);

	return std::nullopt;
}

/**
 * Check one order's refit request against the rules Commands::OrderRefit applies.
 *
 * The refit is a second command after the insert, so a request the engine will refuse has to be
 * rejected before the order list is touched: otherwise `set` deletes the old list and then cannot
 * build the new one.
 *
 * @param order The order as the tool built it.
 * @param index Position of the order in the requested list.
 * @return Error result, or an empty optional when Commands::OrderRefit will accept the refit.
 */
std::optional<Json> CheckOrderRefit(const Order &order, size_t index)
{
	if (!order.IsRefit()) return std::nullopt;

	const CargoType cargo = order.GetRefitCargo();
	std::string reason;
	if (!order.IsType(OT_GOTO_DEPOT) && !order.IsType(OT_GOTO_STATION)) {
		reason = "only station and depot orders can carry a refit";
	} else if (cargo == CARGO_AUTO_REFIT && !order.IsType(OT_GOTO_STATION)) {
		reason = "an automatic refit is only possible on a station order";
	} else if (order.GetLoadType() == OrderLoadType::NoLoad) {
		reason = "an order that does not load cannot be refitted";
	}
	if (reason.empty()) return std::nullopt;

	Json detail = Json::object();
	detail["order_index"] = index;
	detail["reason"] = reason;
	return MakeError("invalid_arguments", fmt::format("the refit of order {} cannot be applied: {}", index, reason), std::move(detail));
}

/**
 * What has to be done for one built order: the order to insert, and the refit that follows it.
 *
 * Commands::InsertOrder only accepts an order whose refit cargo, wait time, travel time and max speed
 * are the defaults - those are set by other commands - so the refit travels in a Commands::OrderRefit
 * for the inserted index instead of in the order.
 */
struct OrderInsertPlan {
	Order order;                    ///< The order to insert, with the refit cleared.
	std::optional<CargoType> refit; ///< Cargo of the refit that follows the insert, if the order asks for one.
};

/**
 * Split a built order into the insert payload and the refit that follows it.
 * @param built The order the tool built.
 * @return The insert payload and the refit cargo, if any.
 */
OrderInsertPlan PlanOrderInsert(const Order &built)
{
	OrderInsertPlan plan;
	plan.order = built;
	if (built.IsRefit()) {
		plan.refit = built.GetRefitCargo();
		plan.order.SetRefit(CARGO_NO_REFIT);
	}
	return plan;
}

/**
 * Replace or extend the order list of a vehicle.
 *
 * Every order is inserted through Commands::InsertOrder with its refit cleared, and an order that asks
 * for a refit is followed by a Commands::OrderRefit for the index it was inserted at. The two commands
 * are not atomic: when the refit fails, the order is in the list without it and the result says so.
 *
 * @param params Method parameters.
 * @param company Company the vehicle belongs to.
 * @param replace True to replace the list, false to append.
 * @return Tool result with the resulting order count and the cost.
 */
Json OrdersSet(const Json &params, CompanyID company, bool replace)
{
	VehicleID vehicle_id = VehicleID::Invalid();
	if (!params.contains("vehicle_id")) return MakeError("invalid_arguments", "orders need vehicle_id");
	if (const std::optional<Json> error = ReadPoolIdParam(params, "vehicle_id", vehicle_id); error.has_value()) return *error;

	const auto entries = params.find("orders");
	if (entries == params.end() || !entries->is_array() || entries->empty()) {
		return MakeError("invalid_arguments", "orders need a non-empty `orders` array");
	}

	/* Build every order before touching the vehicle, so a bad entry cannot leave a half list. */
	std::vector<Order> orders;
	for (const Json &entry : *entries) {
		Order order;
		if (const std::optional<Json> error = ReadOrder(entry, order); error.has_value()) return *error;
		orders.push_back(order);
	}

	/* Check every refit before the first command, so an unsupported one cannot destroy the list. */
	for (size_t index = 0; index < orders.size(); index++) {
		if (const std::optional<Json> error = CheckOrderRefit(orders[index], index); error.has_value()) return *error;
	}

	if (const std::optional<Json> error = CheckWriteContext(company, false); error.has_value()) return *error;

	Vehicle *v = Vehicle::GetIfValid(vehicle_id);
	if (v == nullptr) {
		Json detail = Json::object();
		detail["vehicle_id"] = vehicle_id.base();
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
				return MarkExecution(PartialResult(std::move(result), spent, _posted), false);
			}
			spent += _posted.cost.GetCost();
			deleted++;
		}
	}

	uint inserted = 0;
	for (const Order &built : orders) {
		const Vehicle *current = Vehicle::GetIfValid(veh_id);
		if (current == nullptr) {
			Json detail = Json::object();
			detail["vehicle_id"] = vehicle_id.base();
			return MakeError("not_found", "vehicle disappeared while its orders were set", std::move(detail));
		}

		/* Orders are appended one by one, so the index the order gets is the current count. */
		const VehicleOrderID insert_index = current->GetNumOrders();
		const OrderInsertPlan plan = PlanOrderInsert(built);
		if (!RunCommandAt<Commands::InsertOrder>(false, current->tile, &AgentCommandCallback, veh_id, insert_index, plan.order)) {
			result["deleted"] = deleted;
			result["inserted"] = inserted;
			if (!_posted.captured) return NotExecuted(std::move(result));
			return MarkExecution(PartialResult(std::move(result), spent, _posted), false);
		}
		spent += _posted.cost.GetCost();
		inserted++;

		if (plan.refit.has_value()) {
			const CargoType cargo = *plan.refit;
			if (!RunCommandAt<Commands::OrderRefit>(false, current->tile, &AgentCommandCallback, veh_id, insert_index, cargo)) {
				/* The order is in the list but its refit is not: report exactly that. */
				result["deleted"] = deleted;
				result["inserted"] = inserted;
				result["refit_cargo_id"] = static_cast<uint8_t>(cargo);
				result["refit_order_index"] = insert_index;
				if (!_posted.captured) return NotExecuted(std::move(result));
				return MarkExecution(PartialResult(std::move(result), spent, _posted), false);
			}
			spent += _posted.cost.GetCost();
		}
	}

	const Vehicle *after = Vehicle::GetIfValid(veh_id);
	result["ok"] = true;
	result["deleted"] = deleted;
	result["inserted"] = inserted;
	result["orders"] = after != nullptr ? after->GetNumOrders() : 0;
	result["cost"] = static_cast<int64_t>(spent);
	return MarkExecution(std::move(result), false);
}

/**
 * Share the orders of another vehicle.
 * @param params Method parameters.
 * @param company Company the vehicle belongs to.
 * @return Tool result with both vehicle ids and the cost.
 */
Json OrdersShared(const Json &params, CompanyID company)
{
	VehicleID vehicle_id = VehicleID::Invalid();
	VehicleID source_id = VehicleID::Invalid();
	if (!params.contains("vehicle_id")) return MakeError("invalid_arguments", "shared needs vehicle_id");
	if (!params.contains("share_orders_from")) return MakeError("invalid_arguments", "shared needs share_orders_from");
	if (const std::optional<Json> error = ReadPoolIdParam(params, "vehicle_id", vehicle_id); error.has_value()) return *error;
	if (const std::optional<Json> error = ReadPoolIdParam(params, "share_orders_from", source_id); error.has_value()) return *error;

	if (const std::optional<Json> error = CheckWriteContext(company, false); error.has_value()) return *error;

	Vehicle *v = Vehicle::GetIfValid(vehicle_id);
	if (v == nullptr) {
		Json detail = Json::object();
		detail["vehicle_id"] = vehicle_id.base();
		return MakeError("not_found", "vehicle does not exist", std::move(detail));
	}
	Vehicle *source = Vehicle::GetIfValid(source_id);
	if (source == nullptr) {
		Json detail = Json::object();
		detail["share_orders_from"] = source_id.base();
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
	return MarkExecution(CommandResult(_posted, std::move(result)), false);
}

/**
 * Assign a vehicle to a group.
 * @param params Method parameters.
 * @param company Company the vehicle belongs to.
 * @return Tool result with the group the vehicle ended up in and the cost.
 */
Json OrdersGroup(const Json &params, CompanyID company)
{
	VehicleID vehicle_id = VehicleID::Invalid();
	GroupID group_id = GroupID::Invalid();
	if (!params.contains("vehicle_id")) return MakeError("invalid_arguments", "group needs vehicle_id");
	if (!params.contains("group_id")) return MakeError("invalid_arguments", "group needs group_id");
	if (const std::optional<Json> error = ReadPoolIdParam(params, "vehicle_id", vehicle_id); error.has_value()) return *error;
	if (const std::optional<Json> error = ReadPoolIdParam(params, "group_id", group_id); error.has_value()) return *error;

	if (const std::optional<Json> error = CheckWriteContext(company, false); error.has_value()) return *error;

	Vehicle *v = Vehicle::GetIfValid(vehicle_id);
	if (v == nullptr) {
		Json detail = Json::object();
		detail["vehicle_id"] = vehicle_id.base();
		return MakeError("not_found", "vehicle does not exist", std::move(detail));
	}

	CompanyContext context(company);

	Json result = Json::object();
	result["op"] = "group";
	result["vehicle_id"] = v->index.base();
	if (!RunCommand<Commands::AddVehicleToGroup>(false, &AgentAddVehicleToGroupCallback, group_id, v->index, false, VehicleListIdentifier{})) {
		result["group_id"] = group_id.base();
		if (!_posted.captured) return NotExecuted(std::move(result));
		return MarkExecution(CommandResult(_posted, std::move(result)), false);
	}
	result["group_id"] = _posted.group.base();
	return MarkExecution(CommandResult(_posted, std::move(result)), false);
}

/**
 * Map a timetable field onto the protocol name.
 * @param field The timetable field.
 * @return Protocol name of the field.
 */
std::string_view TimetableFieldName(ModifyTimetableFlags field)
{
	switch (field) {
		case MTF_WAIT_TIME: return "wait_time";
		case MTF_TRAVEL_TIME: return "travel_time";
		case MTF_TRAVEL_SPEED: return "max_speed";
		default: return "unknown";
	}
}

/**
 * Set the timetable of the orders of a vehicle.
 *
 * Every entry is validated against the current order list before the first command is posted, so
 * a bad entry cannot leave a half-applied timetable. The result reports the timetable the engine
 * actually stored, not the values that were asked for.
 *
 * @param params Method parameters.
 * @param company Company the vehicle belongs to.
 * @return Tool result with the applied entries, the resulting timetable and the cost.
 */
Json OrdersTimetable(const Json &params, CompanyID company)
{
	VehicleID vehicle_id = VehicleID::Invalid();
	if (!params.contains("vehicle_id")) return MakeError("invalid_arguments", "timetable needs vehicle_id");
	if (const std::optional<Json> error = ReadPoolIdParam(params, "vehicle_id", vehicle_id); error.has_value()) return *error;

	const auto entries = params.find("timetable");
	if (entries == params.end() || !entries->is_array() || entries->empty()) {
		return MakeError("invalid_arguments", "timetable needs a non-empty `timetable` array");
	}

	bool dry_run = false;
	if (!ReadOptionalBool(params, "dry_run", dry_run)) return MakeError("invalid_arguments", "dry_run must be a boolean");

	/** One timetable field to set on one order. */
	struct TimetableChange {
		VehicleOrderID index; ///< Order the change applies to.
		ModifyTimetableFlags field; ///< Field that is set.
		uint16_t value; ///< Value that is set.
	};

	/* The entries are checked on their own first, so a malformed request is rejected before the
	 * company and the vehicle are even looked at. */
	std::vector<TimetableChange> changes;
	for (const Json &entry : *entries) {
		if (!entry.is_object()) return MakeError("invalid_arguments", "timetable entries must be objects");
		if (const std::optional<Json> error = CheckKnownParams(entry, "world.orders", "timetable", {"index", "wait_time", "travel_time", "max_speed"}); error.has_value()) return *error;

		int64_t index = 0;
		if (!ReadInt(entry, "index", index)) return MakeError("invalid_arguments", "timetable entry needs an order index");
		/* An order index is a VehicleOrderID; 0xFF is the invalid marker, so the last index is 0xFE. */
		if (index < 0 || index > MAX_VEH_ORDER_ID) {
			Json detail = Json::object();
			detail["index"] = index;
			detail["max"] = MAX_VEH_ORDER_ID;
			return MakeError("invalid_params", "timetable entry index must be an order index", std::move(detail));
		}

		const bool has_wait = entry.contains("wait_time");
		const bool has_travel = entry.contains("travel_time");
		const bool has_speed = entry.contains("max_speed");
		if (!has_wait && !has_travel && !has_speed) {
			return MakeError("invalid_arguments", "timetable entry needs wait_time, travel_time or max_speed");
		}

		int64_t wait_time = 0;
		int64_t travel_time = 0;
		int64_t max_speed = 0;
		if (!ReadOptionalInt(entry, "wait_time", wait_time)) return MakeError("invalid_arguments", "wait_time must be an integer");
		if (!ReadOptionalInt(entry, "travel_time", travel_time)) return MakeError("invalid_arguments", "travel_time must be an integer");
		if (!ReadOptionalInt(entry, "max_speed", max_speed)) return MakeError("invalid_arguments", "max_speed must be an integer");
		if (wait_time < 0 || wait_time > UINT16_MAX || travel_time < 0 || travel_time > UINT16_MAX || max_speed < 0 || max_speed > UINT16_MAX) {
			Json detail = Json::object();
			detail["max"] = UINT16_MAX;
			return MakeError("invalid_arguments", "timetable values must fit in 16 bits", std::move(detail));
		}

		const VehicleOrderID order_index = static_cast<VehicleOrderID>(index);
		if (has_wait) changes.push_back({order_index, MTF_WAIT_TIME, static_cast<uint16_t>(wait_time)});
		if (has_travel) changes.push_back({order_index, MTF_TRAVEL_TIME, static_cast<uint16_t>(travel_time)});
		if (has_speed) changes.push_back({order_index, MTF_TRAVEL_SPEED, static_cast<uint16_t>(max_speed)});
	}

	if (const std::optional<Json> error = CheckWriteContext(company, dry_run); error.has_value()) return *error;

	Vehicle *v = Vehicle::GetIfValid(vehicle_id);
	if (v == nullptr) {
		Json detail = Json::object();
		detail["vehicle_id"] = vehicle_id.base();
		return MakeError("not_found", "vehicle does not exist", std::move(detail));
	}
	if (v->owner != company) {
		Json detail = Json::object();
		detail["vehicle_id"] = vehicle_id.base();
		detail["owner"] = v->owner.base();
		return MakeError("not_owned", "vehicle belongs to another company", std::move(detail));
	}
	if (!IsCompanyBuildableVehicleType(v) || !v->IsPrimaryVehicle()) {
		Json detail = Json::object();
		detail["vehicle_id"] = vehicle_id.base();
		detail["type"] = std::string(VehicleTypeName(v->type));
		return MakeError("invalid_arguments", "timetable needs a primary vehicle of a buildable type", std::move(detail));
	}

	/* Now that the order list is known, every index has to name a real, non-implicit order. */
	for (const TimetableChange &change : changes) {
		if (change.index >= v->GetNumOrders()) {
			Json detail = Json::object();
			detail["index"] = change.index;
			detail["orders"] = v->GetNumOrders();
			return MakeError("invalid_arguments", "timetable entry index is not an order of the vehicle", std::move(detail));
		}
		const Order *order = v->GetOrder(change.index);
		if (order == nullptr || order->IsType(OT_IMPLICIT)) {
			Json detail = Json::object();
			detail["index"] = change.index;
			return MakeError("invalid_arguments", "an implicit order has no timetable", std::move(detail));
		}
	}

	const VehicleID veh_id = v->index;
	CompanyContext context(company);

	Money spent = 0;
	Json applied = Json::array();
	for (const TimetableChange &change : changes) {
		if (!RunCommand<Commands::ChangeTimetable>(dry_run, &AgentCommandCallback, veh_id, change.index, change.field, change.value)) {
			Json result = Json::object();
			result["op"] = "timetable";
			result["vehicle_id"] = veh_id.base();
			result["applied"] = std::move(applied);
			if (!_posted.captured) return NotExecuted(std::move(result));
			return MarkExecution(PartialResult(std::move(result), spent, _posted), dry_run);
		}
		spent += _posted.cost.GetCost();

		Json receipt = Json::object();
		receipt["index"] = change.index;
		receipt["field"] = std::string(TimetableFieldName(change.field));
		receipt["value"] = change.value;
		applied.push_back(std::move(receipt));
	}

	Json result = Json::object();
	result["ok"] = true;
	result["op"] = "timetable";
	result["vehicle_id"] = veh_id.base();
	result["applied"] = std::move(applied);
	result["cost"] = static_cast<int64_t>(spent);

	if (!dry_run) {
		/* Read back what the engine stored, so the caller sees the real timetable. */
		const Vehicle *after = Vehicle::GetIfValid(veh_id);
		Json timetable = Json::array();
		std::vector<VehicleOrderID> seen;
		if (after != nullptr) {
			for (const TimetableChange &change : changes) {
				if (std::find(seen.begin(), seen.end(), change.index) != seen.end()) continue;
				seen.push_back(change.index);
				const Order *order = after->GetOrder(change.index);
				if (order == nullptr) continue;
				Json row = Json::object();
				row["index"] = change.index;
				row["wait_time"] = order->GetWaitTime();
				row["travel_time"] = order->GetTravelTime();
				row["max_speed"] = order->GetMaxSpeed();
				row["wait_timetabled"] = order->IsWaitTimetabled();
				row["travel_timetabled"] = order->IsTravelTimetabled();
				timetable.push_back(std::move(row));
			}
		}
		result["timetable"] = std::move(timetable);
	}
	return MarkExecution(std::move(result), dry_run);
}

/* -- method dispatch --------------------------------------------------------------------- */

/**
 * Map a `world.query` kind onto the canonical kind this module implements.
 *
 * The protocol names kinds in the plural; the singular spelling is accepted as an alias so a
 * caller that names one object still reaches the same rows. The result always reports the
 * canonical name, so a caller never has to know which spelling it used.
 *
 * @param kind The kind as sent by the caller.
 * @return The canonical kind, or an empty view for a kind another module owns.
 */
std::string_view CanonicalQueryKind(std::string_view kind)
{
	if (kind == "vehicles" || kind == "vehicle") return "vehicles";
	if (kind == "engines" || kind == "engine") return "engines";
	if (kind == "orders" || kind == "order") return "orders";
	if (kind == "groups" || kind == "group") return "groups";
	if (kind == "cargos" || kind == "cargo") return "cargos";
	return {};
}

/**
 * Dispatch `world.query`.
 * @param params Method parameters.
 * @param company Company of the call.
 * @return Tool result, or an empty optional for a kind another module owns.
 */
std::optional<Json> DispatchQuery(const Json &params, CompanyID company)
{
	std::string kind;
	if (!ReadString(params, "kind", kind)) return MakeError("invalid_arguments", "world.query needs kind");
	const std::string_view canonical = CanonicalQueryKind(kind);
	/* The other kinds belong to the bridge kernel or to another module; do not shadow them. */
	if (canonical.empty()) return std::nullopt;

	if (const std::optional<Json> error = CheckKnownParams(params, "world.query", kind, {"filters", "limit", "offset"}); error.has_value()) return error;
	if (canonical == "vehicles") return QueryVehicles(params, company);
	if (canonical == "engines") return QueryEngines(params, company);
	if (canonical == "orders") return QueryOrders(params, company);
	if (canonical == "groups") return QueryGroups(params, company);
	return QueryCargos(params, company);
}

/**
 * Dispatch `world.construct`.
 * @param params Method parameters.
 * @param company Company that pays.
 * @return Tool result, or an empty optional for an op another module owns.
 */
std::optional<Json> DispatchConstruct(const Json &params, CompanyID company)
{
	std::string op;
	if (!ReadString(params, "op", op)) return MakeError("invalid_arguments", "world.construct needs op");
	/* Rail, air, water and terrain construction belong to another module; do not shadow it. */
	if (op != "road" && op != "tram" && op != "station" && op != "depot" && op != "demolish") return std::nullopt;

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
 *
 * Every op declares the parameters it reads, so a parameter that would not be applied is an
 * error instead of being silently dropped.
 *
 * @param params Method parameters.
 * @param company Company that pays and owns the vehicles.
 * @return Tool result, or an empty optional for an op another module owns.
 */
std::optional<Json> DispatchVehicles(const Json &params, CompanyID company)
{
	std::string op;
	if (!ReadString(params, "op", op)) return MakeError("invalid_arguments", "world.vehicles needs op");

	if (op == "build") {
		if (const std::optional<Json> error = CheckKnownParams(params, "world.vehicles", op,
				{"engine_id", "depot_id", "tile", "cargo_id", "count", "name", "share_orders_from", "dry_run"}); error.has_value()) return error;
		return VehicleBuild(params, company);
	}
	if (op == "sell") {
		if (const std::optional<Json> error = CheckKnownParams(params, "world.vehicles", op,
				{"vehicle_id", "depot_id", "dry_run"}); error.has_value()) return error;
		return VehicleSell(params, company);
	}
	if (op == "start" || op == "stop") {
		if (const std::optional<Json> error = CheckKnownParams(params, "world.vehicles", op,
				{"vehicle_id", "dry_run"}); error.has_value()) return error;
		return VehicleStartStop(params, company, op == "start");
	}
	if (op == "refit") {
		if (const std::optional<Json> error = CheckKnownParams(params, "world.vehicles", op,
				{"vehicle_id", "cargo_id", "only_this", "dry_run"}); error.has_value()) return error;
		return VehicleRefit(params, company);
	}
	if (op == "clone") {
		if (const std::optional<Json> error = CheckKnownParams(params, "world.vehicles", op,
				{"vehicle_id", "depot_id", "tile", "count", "share_orders", "dry_run"}); error.has_value()) return error;
		return VehicleClone(params, company);
	}
	return std::nullopt;
}

/**
 * Dispatch `world.orders`.
 *
 * Every op declares the parameters it reads, so a parameter that would not be applied is an
 * error instead of being silently dropped.
 *
 * @param params Method parameters.
 * @param company Company the vehicle belongs to.
 * @return Tool result, or an empty optional for an op another module owns.
 */
std::optional<Json> DispatchOrders(const Json &params, CompanyID company)
{
	std::string op;
	if (!ReadString(params, "op", op)) return MakeError("invalid_arguments", "world.orders needs op");

	if (op == "set" || op == "append") {
		if (const std::optional<Json> error = CheckKnownParams(params, "world.orders", op,
				{"vehicle_id", "orders"}); error.has_value()) return error;
		return OrdersSet(params, company, op == "set");
	}
	if (op == "shared") {
		if (const std::optional<Json> error = CheckKnownParams(params, "world.orders", op,
				{"vehicle_id", "share_orders_from"}); error.has_value()) return error;
		return OrdersShared(params, company);
	}
	if (op == "group") {
		if (const std::optional<Json> error = CheckKnownParams(params, "world.orders", op,
				{"vehicle_id", "group_id"}); error.has_value()) return error;
		return OrdersGroup(params, company);
	}
	if (op == "timetable") {
		if (const std::optional<Json> error = CheckKnownParams(params, "world.orders", op,
				{"vehicle_id", "timetable", "dry_run"}); error.has_value()) return error;
		return OrdersTimetable(params, company);
	}
	return std::nullopt;
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

/* Outside the anonymous namespace: the header declares this seam, so the test binary has to link it. */
/**
 * Build one order from the tool's order object, for the tests.
 * @param entry The order object.
 * @param order Receives the order.
 * @return Error result, or an empty optional when the order was built.
 */
std::optional<Json> ReadOrderForTest(const Json &entry, Order &order)
{
	return ReadOrder(entry, order);
}

Json RoadDepotEntranceJson(TileIndex depot_tile)
{
	if (!IsValidTile(depot_tile) || !IsRoadDepotTile(depot_tile)) return Json(nullptr);

	/* Both facts are the engine's: the exit direction it stores on the depot tile, and the road or
	 * tram family the tile carries. Nothing here is remembered from the build. */
	return RoadExitJson(depot_tile, GetRoadDepotDirection(depot_tile), TileRoadTramType(depot_tile));
}

Json RoadDepotExitProposalJson(TileIndex depot_tile, DiagDirection dir, RoadTramType rtt)
{
	const TileIndex neighbour = TileAddByDiagDir(depot_tile, dir);

	/* The planned depot is not on the map yet, so it has no exit piece: only the neighbour tile can
	 * be read. `neighbour_drivable` is the engine's test on that tile alone (a vehicle entering it
	 * from the depot side), so a one-piece stub is caught even before the depot exists. */
	const bool neighbour_drivable = IsValidTile(neighbour) &&
			(GetTileTrackStatus(neighbour, TransportType::Road, rtt).trackdirs & DiagdirReachesTrackdirs(dir)).Any();

	Json result = RoadNeighbourJson(depot_tile, dir, rtt);
	result["planned"] = true;
	result["neighbour_drivable"] = neighbour_drivable;
	return result;
}

Json RoadStopAccessJson(TileIndex stop_tile)
{
	if (!IsValidTile(stop_tile) || !IsStationRoadStopTile(stop_tile)) return Json(nullptr);

	const RoadStopType rst = GetRoadStopType(stop_tile);
	const bool bay = IsBayRoadStopTile(stop_tile);

	Json result = Json::object();
	result["tile"] = TileJson(stop_tile);
	result["station_id"] = GetStationIndex(stop_tile).base();
	result["type"] = rst == RoadStopType::Bus ? "bus" : "truck";
	result["kind"] = bay ? "bay" : "drive_through";
	if (!bay) result["axis"] = GetDriveThroughStopAxis(stop_tile) == Axis::X ? "x" : "y";

	/* A stop tile can carry a road and a tram road type at once, and the two are separate track: each
	 * family the tile carries is read on its own, so a road-only neighbour never answers for tram and
	 * the road-only one-way rule never reaches the tram track. `entries` stays as the combined list
	 * for existing readers: road entries first, then tram entries, each labelled with its family. */
	Json combined = Json::array();
	for (const RoadTramType rtt : {RoadTramType::Road, RoadTramType::Tram}) {
		if (!HasTileRoadType(stop_tile, rtt)) continue;
		Json entries = RoadStopEntriesJson(stop_tile, rtt, bay);
		for (const Json &entry : entries) combined.push_back(entry);
		result[rtt == RoadTramType::Tram ? "tram" : "road"] = Json{
			{"road_type_id", static_cast<uint8_t>(GetRoadType(stop_tile, rtt))},
			{"entries", std::move(entries)},
		};
	}
	result["entries"] = std::move(combined);
	return result;
}

Json RoadTileJson(TileIndex tile, RoadTramType rtt)
{
	const bool valid = IsValidTile(tile);
	const bool may_have_road = valid && MayHaveRoad(tile);
	const RoadBits present = may_have_road ? GetAnyRoadBits(tile, rtt, false) : RoadBits{};
	const RoadType rt = may_have_road ? GetRoadType(tile, rtt) : INVALID_ROADTYPE;
	const TrackStatus status = may_have_road ? GetTileTrackStatus(tile, TransportType::Road, rtt) : TrackStatus{};
	const DisallowedRoadDirections disallowed = valid && IsNormalRoadTile(tile) ? GetDisallowedRoadDirections(tile) : DisallowedRoadDirections{};

	/* The directions a vehicle can actually drive off this tile in. `TrackdirToExitdir` is the
	 * engine's own map from a trackdir to the tile edge it leaves by, and the track status already
	 * carries the one-way restriction of this tile (`GetTileTrackStatus_Road` scales the trackdirs by
	 * the disallowed directions), so a one-way tile offers only the direction it allows. The
	 * one-way state of the neighbouring tiles is a per-pair test and is not folded in here. */
	bool exit_dirs[4] = {false, false, false, false};
	for (const Trackdir trackdir : status.trackdirs) {
		exit_dirs[to_underlying(TrackdirToExitdir(trackdir))] = true;
	}

	Json exits = Json::array();
	for (const DiagDirection dir : {DiagDirection::NE, DiagDirection::SE, DiagDirection::SW, DiagDirection::NW}) {
		if (exit_dirs[to_underlying(dir)]) exits.push_back(DiagDirName(dir));
	}

	Json directions = Json::array();
	for (const DisallowedRoadDirection dir : {DisallowedRoadDirection::Southbound, DisallowedRoadDirection::Northbound}) {
		if (disallowed.Test(dir)) directions.push_back(std::string(DisallowedRoadDirectionName(dir)));
	}

	Json result = Json::object();
	result["road_bits"] = RoadBitsJson(present);
	/* The same pair of fields a vehicle row carries: the engine's type id and the engine's own
	 * classification of it. */
	result["road_type_id"] = rt == INVALID_ROADTYPE ? Json(nullptr) : Json(static_cast<uint8_t>(rt));
	result["road_type"] = rt == INVALID_ROADTYPE ? Json(nullptr) : Json(std::string(RoadTypeIsTram(rt) ? "tram" : "road"));
	result["one_way"] = valid && IsOneWayRoadTile(tile);
	result["disallowed_directions"] = std::move(directions);
	result["drivable_exits"] = std::move(exits);
	return result;
}

Json OrdersPlanForTest(const Json &orders)
{
	Json result = Json::object();
	Json rows = Json::array();
	for (size_t index = 0; index < orders.size(); index++) {
		Order built;
		if (const std::optional<Json> error = ReadOrder(orders[index], built); error.has_value()) {
			result["ok"] = false;
			result["order_index"] = index;
			result["error"] = *error;
			return result;
		}
		if (const std::optional<Json> error = CheckOrderRefit(built, index); error.has_value()) {
			result["ok"] = false;
			result["order_index"] = index;
			result["error"] = *error;
			return result;
		}

		const OrderInsertPlan plan = PlanOrderInsert(built);
		Json row = Json::object();
		row["index"] = index;
		row["type"] = std::string(OrderTypeName(built.GetType()));
		row["load"] = std::string(LoadTypeName(built.GetLoadType()));
		row["insert_refit_cargo_id"] = plan.order.IsRefit() ? Json(static_cast<uint8_t>(plan.order.GetRefitCargo())) : Json(nullptr);
		row["order_refit_cargo_id"] = plan.refit.has_value() ? Json(static_cast<uint8_t>(*plan.refit)) : Json(nullptr);
		rows.push_back(std::move(row));
	}
	result["ok"] = true;
	result["orders"] = std::move(rows);
	return result;
}

/**
 * The name the game itself shows for an engine.
 *
 * `Engine::name` only holds the player's custom name, so it is empty for every engine nobody
 * renamed. The engine resolves its display name through `STR_ENGINE_NAME`: the custom name while the
 * engine is enabled, otherwise the NewGRF or default name. The build vehicle window, the vehicle
 * details and the autoreplace window all show that name, so the tool reports it too instead of the
 * raw field.
 *
 * @param e The engine.
 * @return The display name.
 */
std::string EngineDisplayName(const Engine *e)
{
	return GetString(STR_ENGINE_NAME, PackEngineNameDParam(e->index, EngineNameContext::PurchaseList));
}

Json VehicleRoadTypeFieldsForTest(const Vehicle *v)
{
	Json row = Json::object();
	AddRoadTypeFields(row, v);
	return row;
}

Json VehicleEngineNameFieldsForTest(const Vehicle *v)
{
	Json row = Json::object();
	AddVehicleEngineNameFields(row, v);
	return row;
}

/* Outside the anonymous namespace: the header declares this helper, so the test binary has to link
 * it. `VehicleRow` above calls it through that declaration. */
/**
 * Serialise the wait state of one vehicle.
 *
 * Every field is a raw engine tick counter, never a duration in seconds or in days: the engine
 * ticks a vehicle 74 times per game day (`Ticks::DAY_TICKS`), so a counter is only comparable
 * with another counter or with a timetable that is in the same unit.
 *  - `current_order_time`: ticks that passed since the current order started
 *    (`BaseConsist::current_order_time`), the base of the engine's lateness calculation. The field
 *    is signed and the engine marks "not known" with -1 (`TimerGameTick::Ticks::INVALID_TICKS`), so a
 *    negative value means "not known", not "one tick before the order started".
 *  - `load_unload_ticks`: ticks the vehicle still waits before the next step of its
 *    loading/unloading cycle (`Vehicle::load_unload_ticks`, counted down by the engine).
 *  - `wait_counter`: the train's own counter. The engine uses this single field for three
 *    different situations — ticks waiting in front of a signal, ticks being stuck, and the
 *    counter for forced proceeding through signals (`Train::wait_counter`) — so it is reported
 *    as the raw value and never as "seconds until the train moves again".
 *  - `turn_counter`: the aircraft's countdown until the next turn is allowed, which the engine
 *    throttles so an aircraft never turns more than 45 degrees at once (`Aircraft::turn_counter`).
 * A counter that belongs to another vehicle type is `null`, not 0, so a caller can tell "this
 * vehicle has no such counter" from "the counter is zero".
 * @param v The vehicle.
 * @return The wait fields of a vehicle row.
 */
Json VehicleWaitJson(const Vehicle *v)
{
	Json wait = Json::object();
	wait["current_order_time"] = v->current_order_time;
	wait["load_unload_ticks"] = v->load_unload_ticks;
	wait["wait_counter"] = v->type == VehicleType::Train ? Json(Train::From(v)->wait_counter) : Json(nullptr);
	wait["turn_counter"] = v->type == VehicleType::Aircraft ? Json(Aircraft::From(v)->turn_counter) : Json(nullptr);
	return wait;
}

Json GameToolCapabilities()
{
	Json methods = Json::array();
	methods.push_back(Capability("world.query", "read",
			{"vehicles", "engines", "orders", "groups", "cargos"},
			{"towns", "companies", "tiles", "stations", "industries", "settings", "signs", "houses", "finances"}));
	methods.push_back(Capability("world.construct", "write",
			{"road", "tram", "station", "depot", "demolish"},
			{"rail", "signal", "airport", "dock", "bridge", "tunnel", "canal", "terraform"}));
	methods.push_back(Capability("world.vehicles", "write",
			{"build", "sell", "start", "stop", "refit", "clone"},
			{}));
	methods.push_back(Capability("world.orders", "write",
			{"set", "append", "shared", "group", "timetable"},
			{}));

	Json result = Json::object();
	/* The exact `world.query` kind spellings this module answers, so the bridge can map a
	 * canonical name onto them without guessing. */
	Json aliases = Json::object();
	for (const std::string_view kind : {"vehicles", "engines", "orders", "groups", "cargos"}) aliases[std::string(kind)] = std::string(kind);
	for (const std::string_view kind : {"vehicle", "engine", "order", "group", "cargo"}) aliases[std::string(kind)] = std::string(CanonicalQueryKind(kind));

	result["module"] = "agent_game_tools";
	result["methods"] = std::move(methods);
	result["query_aliases"] = std::move(aliases);
	result["unimplemented_methods"] = Json::array({"world.info", "world.objects", "world.pause", "world.rename", "world.settings.get", "world.settings.set", "world.capture", "task.barrier"});
	result["notes"] = Json::array({
		"Actual mutations go through Command::Post, the normal player command path, so the engine's own eligibility, pause and finance checks apply; results carry the actual cost and the created ids, and every command receipt states its mode explicitly (`dry_run` and `estimated` are always present, and false for an actual execution).",
		"Ops and kinds this module does not implement are left to the other modules: DispatchGameTool returns nullopt instead of a gap, so nothing shadows the bridge kernel, transport or admin.",
		"Every op declares the parameters it reads; a parameter that would not be applied is rejected with `invalid_arguments` instead of being silently dropped.",
		"world.construct accepts `dry_run`, which only queries the cost and reports `estimated: true`.",
		"world.construct op=demolish clears the tile (Commands::LandscapeClear); the `demolish` flag on the other ops is reported as a gap.",
		"world.construct op=station builds one road stop rectangle; `stop_type` is `bus` (default) or `truck`, and a span above 255 tiles is rejected instead of truncated.",
		"world.construct op=station/depot take one tile per call; a longer `tiles` array is rejected with `unsupported_multi_item` instead of using only the first tile.",
		"world.vehicles op=build honours `cargo_id` (build cargo), `name` (Commands::RenameVehicle) and `share_orders_from` (Commands::CloneOrder); a batch reports partial receipts and the money it actually spent.",
		"world.vehicles op=refit needs the consist stopped in a depot, checks the refit masks of every part of the chain before posting, and reports the capacities the engine actually produced.",
		"world.vehicles op=clone needs a depot tile of the company (`depot_id`, `tile`, or the source vehicle's own tile when it stands in a depot), checks the owner and the engine availability before posting, and reports the created vehicle ids and the cost the engine charged; a `dry_run` clone cost is an estimate, because the command is flagged NoTest.",
		"world.vehicles and world.orders accept `dry_run`, which only queries the cost and reports `estimated: true`.",
		"world.orders op=timetable validates every entry against the current order list before the first command, and reads the stored timetable back.",
		"world.query kind=vehicles/engines/orders/groups/cargos is read-only inventory with validated filters; the other kinds are left to the other modules.",
		"world.query rows carry the readable values the game shows next to the raw ids: cargo `label`/`name`, engine `name`, group `name`, order `destination_name`, and `refit_cargo_name`.",
		"world.query never writes to the world and never uses a private counter: every value is read through the engine's own accessors, so the agent can verify a command's effect against the next read.",
		"world.query pages are bounded: `limit` is clamped to 1..500 (default 100), `offset` is never negative, and the result reports `total`, `limit`, `offset` and `has_more`.",
		"world.query ownership is explicit: `company_id` filters a company scoped kind, `company_id: null` asks for every owner, and the scope that was applied is reported in `scope.company_id`.",
		"A vehicle row carries its order list compactly (`current_orders`, at most 64 entries, with `current_orders_truncated`), its group and depot references and the running cost, so the fleet can be audited without a second query.",
		"A vehicle row carries the engine's own wait counters as raw ticks: `current_order_time` (ticks since the current order started), `load_unload_ticks` (ticks left before the next loading step), `wait_counter` for a train and `turn_counter` for an aircraft. `wait_counter` is one engine field for waiting in front of a signal, being stuck or the forced-proceeding counter, so it is the raw value and not a duration; a counter of another vehicle type is null.",
		"A vehicle row names its place in a consist: `is_primary` is the engine's own test for the vehicle that carries the orders and the income, and `primary_ref` is the full generation reference of the head of the chain, null when that head is not a primary vehicle (a free wagon is its own head). The primary row adds `consist` with the number of parts in the chain (`total_parts`), whether the parts list was cut at 128 entries (`truncated`), the parts in chain order with their own ref and their own cargo, and the exact per-cargo `load`/`capacity` totals over the whole chain. The row's own `cargo` stays the cargo of that one vehicle.",
	});
	return result;
}

std::optional<Json> DispatchGameTool(std::string_view method, const Json &params, CompanyID company)
{
	if (method != "world.query" && method != "world.construct" && method != "world.vehicles" && method != "world.orders") return std::nullopt;

	if (!params.is_object()) return MakeError("invalid_arguments", "params must be an object");

	if (method == "world.query") return DispatchQuery(params, company);
	if (method == "world.construct") return DispatchConstruct(params, company);
	if (method == "world.vehicles") return DispatchVehicles(params, company);
	return DispatchOrders(params, company);
}

} // namespace InGameAgent
