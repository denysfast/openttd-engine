/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/>.
 */

/** @file agent_transport_tools.cpp Dispatch of the native construction tool methods onto real OpenTTD commands. */

#include "stdafx.h"
#include "agent_transport_tools.h"

#include "airport.h"
#include "bridge.h"
#include "command_func.h"
#include "company_base.h"
#include "depot_base.h"
#include "depot_map.h"
#include "direction_type.h"
#include "map_func.h"
#include "map_type.h"
#include "network/network.h"
#include "newgrf_airport.h"
#include "newgrf_station.h"
#include "rail.h"
#include "rail_cmd.h"
#include "road_cmd.h"
#include "road_type.h"
#include "station_base.h"
#include "station_cmd.h"
#include "station_map.h"
#include "station_type.h"
#include "strings_func.h"
#include "terraform_cmd.h"
#include "tile_map.h"
#include "tunnelbridge_cmd.h"
#include "water_cmd.h"
#include "water_map.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <string>
#include <utility>
#include <vector>

#include "table/strings.h"

#include "safeguards.h"

namespace InGameAgent {

namespace {

/** Result of the command that was posted last; filled by the synchronous single-player callback. */
struct PostedCommand {
	bool captured = false; ///< The callback ran, so the engine reported a result.
	CommandCost cost{};    ///< Actual cost, or the failure of the command.
};

PostedCommand _posted;

} // namespace

void CcAgentTransportCommand(Commands, const CommandCost &result, const CommandDataBuffer &, CommandDataBuffer)
{
	_posted.captured = true;
	_posted.cost = result;
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
 * Complete a result whose command never ran: the engine rejected it before dispatch. Nothing was
 * mutated, so the caller must not treat this as a success.
 * @param result Tool result to complete.
 * @return The completed tool result.
 */
Json NotExecuted(Json result)
{
	result["ok"] = false;
	result["error"] = "command_not_executed";
	result["message"] = "the engine did not execute the command (rejected before dispatch, or paused)";
	return result;
}

/**
 * Merge the captured command result into a tool result: the cost, and the engine's own error.
 * @param posted Result of the command that was actually executed.
 * @param result Tool result to complete.
 * @return The completed tool result.
 */
Json CommandResult(const PostedCommand &posted, Json result)
{
	if (!posted.captured) return NotExecuted(std::move(result));
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

/* -- command helpers --------------------------------------------------------------------- */

/**
 * Run one command and capture what the engine did.
 *
 * An actual run goes through `Command<Tcmd>::Post`, the normal player command path, so the
 * engine's own eligibility, pause and finance checks apply and the money is really deducted. A
 * dry run only queries the cost and mutates nothing.
 *
 * @param dry_run True to only query the cost.
 * @param args Command arguments.
 * @return True when the command succeeded (or the estimate is valid).
 */
template <Commands Tcmd, typename... Targs>
bool RunCommand(bool dry_run, Targs... args)
{
	_posted = PostedCommand{};
	if (dry_run) {
		auto estimate = Command<Tcmd>::Do(DoCommandFlags{DoCommandFlag::QueryCost}, args...);
		_posted.captured = true;
		_posted.cost = ExtractCommandCost(estimate);
	} else {
		Command<Tcmd>::Post(STR_NULL, CcAgentTransportCommand, std::forward<Targs>(args)...);
	}
	return _posted.captured && _posted.cost.Succeeded();
}

/**
 * Run one command per tile and stop at the first failure.
 *
 * Every tile is validated by the caller before the first command runs, so a batch never stops
 * half way because of an argument that could have been rejected up front.
 *
 * @param tiles Tiles to handle, in order.
 * @param result Tool result to complete.
 * @param dry_run True to only query the cost.
 * @param post Callable that runs the command for one tile.
 * @return The completed tool result, with the applied tiles and the money actually spent.
 */
template <typename Tpost>
Json RunBatch(const std::vector<TileIndex> &tiles, Json result, bool dry_run, Tpost post)
{
	Money spent = 0;
	Json done = Json::array();
	for (const TileIndex &tile : tiles) {
		const bool ok = post(tile);
		if (!_posted.captured) {
			result["done"] = std::move(done);
			result["cost"] = static_cast<int64_t>(spent);
			return MarkExecution(NotExecuted(std::move(result)), dry_run);
		}
		if (!ok) {
			result["done"] = std::move(done);
			return MarkExecution(PartialResult(std::move(result), spent, _posted), dry_run);
		}
		spent += _posted.cost.GetCost();
		done.push_back(TileJson(tile));
	}
	result["done"] = std::move(done);
	result["ok"] = true;
	result["cost"] = static_cast<int64_t>(spent);
	return MarkExecution(std::move(result), dry_run);
}

/**
 * Switch the company that pays and owns the result of a command, and restore the previous
 * company when the call returns. The caller validates the company first.
 */
class CompanyContext {
public:
	explicit CompanyContext(CompanyID company) : backup(_current_company, company) {}
	~CompanyContext() { this->backup.Restore(); }

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
	return MakeError("multiplayer_unsupported", "transport tool commands run through the single-player command path only");
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
 * Read one `{x, y}` tile and check it against the map.
 * @param value JSON value holding the tile.
 * @param tile Receives the tile index.
 * @return True when the value is a tile inside the map.
 */
bool ReadTile(const Json &value, TileIndex &tile)
{
	RawTile raw;
	if (!ReadRawTile(value, raw)) return false;
	return ToTileIndex(raw, tile);
}

/**
 * Read the `tiles` array of an op that handles a batch of tiles.
 *
 * Every tile is checked against the map here, before the caller mutates anything.
 *
 * @param params Method parameters.
 * @param tiles Receives the tiles, in the order they were sent.
 * @return True when `tiles` is a non-empty array of tiles inside the map.
 */
bool ReadTileList(const Json &params, std::vector<TileIndex> &tiles)
{
	const auto it = params.find("tiles");
	if (it == params.end() || !it->is_array() || it->empty()) return false;
	tiles.clear();
	tiles.reserve(it->size());
	for (const Json &entry : *it) {
		TileIndex tile;
		if (!ReadTile(entry, tile)) return false;
		tiles.push_back(tile);
	}
	return true;
}

/**
 * Read the single tile an op builds on: one entry in `tiles`, or `from`.
 * @param params Method parameters.
 * @param tile Receives the tile.
 * @return True when exactly one tile inside the map was given.
 */
bool ReadSingleTile(const Json &params, TileIndex &tile)
{
	const auto it = params.find("tiles");
	if (it != params.end()) {
		if (!it->is_array() || it->size() != 1) return false;
		return ReadTile((*it)[0], tile);
	}
	const auto from = params.find("from");
	if (from == params.end()) return false;
	return ReadTile(*from, tile);
}

/**
 * Read a straight run of tiles from `from`/`to`.
 * @param params Method parameters.
 * @param from Receives the first tile.
 * @param to Receives the last tile.
 * @param axis Receives the axis the run follows.
 * @return True when both tiles are inside the map and share a row or a column.
 */
bool ReadRun(const Json &params, TileIndex &from, TileIndex &to, Axis &axis)
{
	const auto from_it = params.find("from");
	const auto to_it = params.find("to");
	if (from_it == params.end() || to_it == params.end()) return false;
	if (!ReadTile(*from_it, from) || !ReadTile(*to_it, to)) return false;
	if (TileX(from) == TileX(to)) {
		axis = Axis::Y;
	} else if (TileY(from) == TileY(to)) {
		axis = Axis::X;
	} else {
		return false;
	}
	return true;
}

/**
 * Read an optional `axis` parameter.
 * @param params Method parameters.
 * @param axis Receives the axis; unchanged when the parameter is absent.
 * @return False when the parameter is present but not `x` or `y`.
 */
bool ReadAxis(const Json &params, Axis &axis)
{
	std::string name;
	if (!ReadOptionalString(params, "axis", name)) return false;
	if (name.empty()) return true;
	if (name == "x") {
		axis = Axis::X;
		return true;
	}
	if (name == "y") {
		axis = Axis::Y;
		return true;
	}
	return false;
}

/**
 * Read an optional `direction` parameter for a depot.
 * @param params Method parameters.
 * @param dir Receives the direction; defaults to north-east.
 * @return False when the parameter is present but not a diagonal direction.
 */
bool ReadDiagDirection(const Json &params, DiagDirection &dir)
{
	std::string name;
	if (!ReadOptionalString(params, "direction", name)) return false;
	if (name.empty()) return true;
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
 * Read a required `track` parameter.
 * @param params Method parameters.
 * @param track Receives the track.
 * @return True when the parameter names one of the six plain tracks.
 */
bool ReadTrack(const Json &params, Track &track)
{
	std::string name;
	if (!ReadString(params, "track", name)) return false;
	if (name == "x") {
		track = Track::X;
		return true;
	}
	if (name == "y") {
		track = Track::Y;
		return true;
	}
	if (name == "upper") {
		track = Track::Upper;
		return true;
	}
	if (name == "lower") {
		track = Track::Lower;
		return true;
	}
	if (name == "left") {
		track = Track::Left;
		return true;
	}
	if (name == "right") {
		track = Track::Right;
		return true;
	}
	return false;
}

/**
 * Read an optional `signal_type` parameter.
 * @param params Method parameters.
 * @param type Receives the signal type; defaults to block signals.
 * @return False when the parameter is present but not a known signal type.
 */
bool ReadSignalType(const Json &params, SignalType &type)
{
	std::string name;
	if (!ReadOptionalString(params, "signal_type", name)) return false;
	if (name.empty()) return true;
	if (name == "block") {
		type = SignalType::Block;
		return true;
	}
	if (name == "entry") {
		type = SignalType::Entry;
		return true;
	}
	if (name == "exit") {
		type = SignalType::Exit;
		return true;
	}
	if (name == "combo") {
		type = SignalType::Combo;
		return true;
	}
	if (name == "path") {
		type = SignalType::Path;
		return true;
	}
	if (name == "path_one_way") {
		type = SignalType::PathOneWay;
		return true;
	}
	return false;
}

/**
 * Read an optional `signal_variant` parameter.
 * @param params Method parameters.
 * @param variant Receives the variant; defaults to light signals.
 * @return False when the parameter is present but not a known variant.
 */
bool ReadSignalVariant(const Json &params, SignalVariant &variant)
{
	std::string name;
	if (!ReadOptionalString(params, "signal_variant", name)) return false;
	if (name.empty()) return true;
	if (name == "electric") {
		variant = SignalVariant::Electric;
		return true;
	}
	if (name == "semaphore") {
		variant = SignalVariant::Semaphore;
		return true;
	}
	return false;
}

/**
 * Read a required `rail_type` parameter: a four character label such as `RAIL`, or an index.
 * @param params Method parameters.
 * @param company Company that must have the rail type available.
 * @param rt Receives the rail type.
 * @return True when the parameter names a rail type the company can build.
 */
bool ReadRailType(const Json &params, CompanyID company, RailType &rt)
{
	const auto it = params.find("rail_type");
	if (it == params.end()) return false;

	RailType resolved = INVALID_RAILTYPE;
	if (it->is_string()) {
		const std::string label = it->get<std::string>();
		if (label.size() != 4) return false;
		const char text[5] = {label[0], label[1], label[2], label[3], '\0'};
		resolved = GetRailTypeByLabel(RailTypeLabel{text});
	} else if (it->is_number_integer()) {
		const int64_t value = it->get<int64_t>();
		if (value < 0 || value >= RAILTYPE_END) return false;
		resolved = static_cast<RailType>(value);
	} else {
		return false;
	}

	if (resolved == INVALID_RAILTYPE) return false;
	if (!GetCompanyRailTypes(company).Test(resolved)) return false;
	rt = resolved;
	return true;
}

/**
 * Read an optional `road_type` parameter.
 * @param params Method parameters.
 * @param rt Receives the road type; unchanged when the parameter is absent.
 * @return False when the parameter is present but not `road` or `tram`.
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
 * Map a transport type name onto the engine type.
 * @param name The name, e.g. `rail`.
 * @param transport Receives the transport type.
 * @return True when the name is a known transport type.
 */
bool ParseTransportName(std::string_view name, TransportType &transport)
{
	if (name == "rail") {
		transport = TransportType::Rail;
		return true;
	}
	if (name == "road") {
		transport = TransportType::Road;
		return true;
	}
	if (name == "water") {
		transport = TransportType::Water;
		return true;
	}
	if (name == "air") {
		transport = TransportType::Air;
		return true;
	}
	return false;
}

/**
 * Read an optional `transport_type` parameter.
 * @param params Method parameters.
 * @param transport Receives the transport type; unchanged when the parameter is absent.
 * @return False when the parameter is present but not a known transport type.
 */
bool ReadTransportType(const Json &params, TransportType &transport)
{
	std::string name;
	if (!ReadOptionalString(params, "transport_type", name)) return false;
	if (name.empty()) return true;
	return ParseTransportName(name, transport);
}

/**
 * Read an optional `water_class` parameter.
 * @param params Method parameters.
 * @param wc Receives the water class; unchanged when the parameter is absent.
 * @return False when the parameter is present but not `canal`, `river` or `sea`.
 */
bool ReadWaterClass(const Json &params, WaterClass &wc)
{
	std::string name;
	if (!ReadOptionalString(params, "water_class", name)) return false;
	if (name.empty()) return true;
	if (name == "canal") {
		wc = WaterClass::Canal;
		return true;
	}
	if (name == "river") {
		wc = WaterClass::River;
		return true;
	}
	if (name == "sea") {
		wc = WaterClass::Sea;
		return true;
	}
	return false;
}

/**
 * Compare two strings ignoring ASCII case.
 * @param a First string.
 * @param b Second string.
 * @return True when both strings are equal ignoring case.
 */
bool EqualsIgnoreCase(std::string_view a, std::string_view b)
{
	if (a.size() != b.size()) return false;
	for (size_t i = 0; i < a.size(); i++) {
		if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i]))) return false;
	}
	return true;
}

/**
 * Read a required `airport_type` parameter: a name, or an index.
 * @param params Method parameters.
 * @param type Receives the airport type.
 * @return True when the parameter names an airport the engine knows.
 */
bool ReadAirportType(const Json &params, uint8_t &type)
{
	const auto it = params.find("airport_type");
	if (it == params.end()) return false;

	if (it->is_number_integer()) {
		const int64_t value = it->get<int64_t>();
		if (value < 0 || value >= NUM_AIRPORTS) return false;
		if (AirportSpec::Get(static_cast<uint8_t>(value)) == nullptr) return false;
		type = static_cast<uint8_t>(value);
		return true;
	}
	if (!it->is_string()) return false;

	static const std::array<std::pair<std::string_view, uint8_t>, 10> names = {{
		{"small", AT_SMALL},
		{"large", AT_LARGE},
		{"heliport", AT_HELIPORT},
		{"metropolitan", AT_METROPOLITAN},
		{"international", AT_INTERNATIONAL},
		{"commuter", AT_COMMUTER},
		{"helidepot", AT_HELIDEPOT},
		{"intercontinental", AT_INTERCON},
		{"helistation", AT_HELISTATION},
		{"oilrig", AT_OILRIG},
	}};

	const std::string name = it->get<std::string>();
	for (const auto &[key, value] : names) {
		if (!EqualsIgnoreCase(key, name)) continue;
		if (AirportSpec::Get(value) == nullptr) return false;
		type = value;
		return true;
	}
	return false;
}

/**
 * Read a required `bridge_type` parameter: an index, or the bridge's own name.
 * @param params Method parameters.
 * @param type Receives the bridge type.
 * @return True when the parameter names a bridge the engine knows.
 */
bool ReadBridgeType(const Json &params, BridgeType &type)
{
	const auto it = params.find("bridge_type");
	if (it == params.end()) return false;

	if (it->is_number_integer()) {
		const int64_t value = it->get<int64_t>();
		if (value < 0 || value >= static_cast<int64_t>(MAX_BRIDGES)) return false;
		type = static_cast<BridgeType>(value);
		return true;
	}
	if (!it->is_string()) return false;

	const std::string name = it->get<std::string>();
	/* A decimal string is an index. */
	if (!name.empty() && name.size() <= 3 && std::all_of(name.begin(), name.end(), [](unsigned char c) { return std::isdigit(c) != 0; })) {
		int64_t value = 0;
		for (const char c : name) value = value * 10 + (c - '0');
		if (value >= static_cast<int64_t>(MAX_BRIDGES)) return false;
		type = static_cast<BridgeType>(value);
		return true;
	}
	/* Otherwise match the bridge's own description, which is what a player sees. */
	for (BridgeType i = 0; i < MAX_BRIDGES; i++) {
		if (!EqualsIgnoreCase(GetString(GetBridgeSpec(i)->material), name)) continue;
		type = i;
		return true;
	}
	return false;
}

/**
 * Read an optional `slope` corner mask for terraforming.
 * @param params Method parameters.
 * @param corners Receives the corners to terraform.
 * @return True when the parameter is present and names at least one corner.
 */
bool ReadSlope(const Json &params, Slope &corners)
{
	std::string name;
	if (!ReadOptionalString(params, "slope", name)) return false;
	if (name.empty()) return false;
	Slope mask = SLOPE_FLAT;
	for (const char c : name) {
		switch (c) {
			case 'n': mask |= SLOPE_N; break;
			case 'e': mask |= SLOPE_E; break;
			case 's': mask |= SLOPE_S; break;
			case 'w': mask |= SLOPE_W; break;
			default: return false;
		}
	}
	corners = mask;
	return true;
}

/**
 * Read an optional `station_id` parameter, i.e. the station a new part joins.
 * @param params Method parameters.
 * @param station Receives the station id, or an invalid id when the parameter is absent.
 * @return False when the parameter is present but not an existing station.
 */
bool ReadJoinStation(const Json &params, StationID &station)
{
	int64_t value = -1;
	if (!ReadOptionalInt(params, "station_id", value)) return false;
	if (value < 0) return true;
	if (value > UINT16_MAX) return false;
	const StationID candidate{static_cast<uint16_t>(value)};
	if (!Station::IsValidID(candidate)) return false;
	station = candidate;
	return true;
}

/* -- name helpers ------------------------------------------------------------------------ */

/**
 * Name of a track, as the tool protocol spells it.
 * @param track The track.
 * @return The name.
 */
std::string TrackName(Track track)
{
	switch (track) {
		case Track::X: return "x";
		case Track::Y: return "y";
		case Track::Upper: return "upper";
		case Track::Lower: return "lower";
		case Track::Left: return "left";
		case Track::Right: return "right";
		default: return "invalid";
	}
}

/**
 * Name of a signal type, as the tool protocol spells it.
 * @param type The signal type.
 * @return The name.
 */
std::string SignalTypeName(SignalType type)
{
	switch (type) {
		case SignalType::Block: return "block";
		case SignalType::Entry: return "entry";
		case SignalType::Exit: return "exit";
		case SignalType::Combo: return "combo";
		case SignalType::Path: return "path";
		case SignalType::PathOneWay: return "path_one_way";
		default: return "invalid";
	}
}

/**
 * Name of a diagonal direction, as the tool protocol spells it.
 * @param dir The direction.
 * @return The name.
 */
std::string DiagDirectionName(DiagDirection dir)
{
	switch (dir) {
		case DiagDirection::NE: return "ne";
		case DiagDirection::SE: return "se";
		case DiagDirection::SW: return "sw";
		case DiagDirection::NW: return "nw";
		default: return "invalid";
	}
}

/* -- map lookups ------------------------------------------------------------------------- */

/**
 * Find the station that owns a tile, if any.
 * @param tile The tile.
 * @return The station id, or an invalid id when the tile has no station.
 */
StationID StationAt(TileIndex tile)
{
	if (!IsRailStationTile(tile) && !IsAirportTile(tile) && !IsDockTile(tile)) return StationID::Invalid();
	const Station *station = Station::GetByTile(tile);
	return station == nullptr ? StationID::Invalid() : station->index;
}

/**
 * Find the depot that owns a tile, if any.
 * @param tile The tile.
 * @return The depot id, or an invalid id when the tile has no depot.
 */
DepotID DepotAt(TileIndex tile)
{
	if (!IsRailDepotTile(tile) && !IsRoadDepotTile(tile) && !IsShipDepotTile(tile)) return DepotID::Invalid();
	const Depot *depot = Depot::GetByTile(tile);
	return depot == nullptr ? DepotID::Invalid() : depot->index;
}

/* -- ops --------------------------------------------------------------------------------- */

/**
 * Build or remove rail track, either on a list of tiles or along a straight run.
 * @param params Method parameters.
 * @param company Company that pays and owns the result.
 * @param demolish True to remove track instead of building it.
 * @param dry_run True to only query the cost.
 * @return The tool result.
 */
Json ConstructRail(const Json &params, CompanyID company, bool demolish, bool dry_run)
{
	RailType rt = INVALID_RAILTYPE;
	if (!demolish && !ReadRailType(params, company, rt)) {
		return MakeError("invalid_arguments", "rail needs `rail_type`: a four character label such as RAIL, or an index, available to the company");
	}

	Json result = Json::object();
	result["op"] = "rail";
	result["demolish"] = demolish;

	TileIndex from;
	TileIndex to;
	Axis axis;
	if (params.contains("from") && params.contains("to")) {
		if (!ReadRun(params, from, to, axis)) return MakeError("invalid_arguments", "from/to must be tiles inside the map that share a row or a column");
		Track track;
		if (!ReadTrack(params, track)) track = axis == Axis::X ? Track::X : Track::Y;
		result["from"] = TileJson(from);
		result["to"] = TileJson(to);
		result["track"] = TrackName(track);
		if (demolish) {
			RunCommand<Commands::RemoveRailLong>(dry_run, to, from, track);
		} else {
			RunCommand<Commands::BuildRailLong>(dry_run, to, from, rt, track, false, false);
		}
		return MarkExecution(CommandResult(_posted, std::move(result)), dry_run);
	}

	std::vector<TileIndex> tiles;
	if (!ReadTileList(params, tiles)) return MakeError("invalid_arguments", "rail needs `tiles`, or `from`/`to` for a run");
	Track track;
	if (!ReadTrack(params, track)) return MakeError("invalid_arguments", "rail needs `track`: x, y, upper, lower, left or right");
	result["track"] = TrackName(track);
	return RunBatch(tiles, std::move(result), dry_run, [&](TileIndex tile) {
		return demolish
				? RunCommand<Commands::RemoveRail>(dry_run, tile, track)
				: RunCommand<Commands::BuildRail>(dry_run, tile, rt, track, false);
	});
}

/**
 * Build or remove one signal per tile.
 * @param params Method parameters.
 * @param demolish True to remove signals instead of building them.
 * @param dry_run True to only query the cost.
 * @return The tool result.
 */
Json ConstructSignal(const Json &params, bool demolish, bool dry_run)
{
	Track track;
	if (!ReadTrack(params, track)) return MakeError("invalid_arguments", "signal needs `track`: x, y, upper, lower, left or right");
	SignalType type = SignalType::Block;
	if (!ReadSignalType(params, type)) return MakeError("invalid_arguments", "signal_type must be one of block, entry, exit, combo, path, path_one_way");
	SignalVariant variant = SignalVariant::Electric;
	if (!ReadSignalVariant(params, variant)) return MakeError("invalid_arguments", "signal_variant must be `electric` or `semaphore`");

	std::vector<TileIndex> tiles;
	if (!ReadTileList(params, tiles)) return MakeError("invalid_arguments", "signal needs `tiles`");

	Json result = Json::object();
	result["op"] = "signal";
	result["track"] = TrackName(track);
	result["demolish"] = demolish;
	if (!demolish) {
		result["signal_type"] = SignalTypeName(type);
		result["signal_variant"] = variant == SignalVariant::Electric ? "electric" : "semaphore";
	}
	return RunBatch(tiles, std::move(result), dry_run, [&](TileIndex tile) {
		return demolish
				? RunCommand<Commands::RemoveSignal>(dry_run, tile, track)
				: RunCommand<Commands::BuildSignal>(dry_run, tile, track, type, variant, false, false, false, type, type, 0, 0);
	});
}

/**
 * Build one rail station rectangle.
 * @param params Method parameters.
 * @param company Company that pays and owns the result.
 * @param dry_run True to only query the cost.
 * @return The tool result.
 */
Json ConstructRailStation(const Json &params, CompanyID company, bool dry_run)
{
	RailType rt;
	if (!ReadRailType(params, company, rt)) {
		return MakeError("invalid_arguments", "a rail station needs `rail_type`: a four character label such as RAIL, or an index, available to the company");
	}

	TileIndex tile;
	Axis axis = Axis::X;
	uint8_t numtracks = 1;
	uint8_t plat_len = 1;

	TileIndex from;
	TileIndex to;
	Axis run_axis;
	if (params.contains("from") && params.contains("to")) {
		if (!ReadRun(params, from, to, run_axis)) return MakeError("invalid_arguments", "from/to must be tiles inside the map that share a row or a column");
		axis = run_axis;
		const uint dx = TileX(from) > TileX(to) ? TileX(from) - TileX(to) : TileX(to) - TileX(from);
		const uint dy = TileY(from) > TileY(to) ? TileY(from) - TileY(to) : TileY(to) - TileY(from);
		const uint along = axis == Axis::X ? dx : dy;
		const uint across = axis == Axis::X ? dy : dx;
		if (along + 1 > 255 || across + 1 > 255) {
			return MakeError("invalid_arguments", "a station span above 255 tiles is rejected instead of truncated");
		}
		plat_len = static_cast<uint8_t>(along + 1);
		numtracks = static_cast<uint8_t>(across + 1);
		tile = TileXY(std::min(TileX(from), TileX(to)), std::min(TileY(from), TileY(to)));
	} else {
		if (!ReadSingleTile(params, tile)) return MakeError("invalid_arguments", "a rail station needs `tiles`, or `from`/`to`");
		if (!ReadAxis(params, axis)) return MakeError("invalid_arguments", "axis must be `x` or `y`");
		int64_t value = 1;
		if (!ReadOptionalInt(params, "num_tracks", value)) return MakeError("invalid_arguments", "num_tracks must be an integer");
		if (value < 1 || value > 255) return MakeError("invalid_arguments", "num_tracks must be between 1 and 255");
		numtracks = static_cast<uint8_t>(value);
		value = 1;
		if (!ReadOptionalInt(params, "platform_length", value)) return MakeError("invalid_arguments", "platform_length must be an integer");
		if (value < 1 || value > 255) return MakeError("invalid_arguments", "platform_length must be between 1 and 255");
		plat_len = static_cast<uint8_t>(value);
	}

	StationID join = StationID::Invalid();
	if (!ReadJoinStation(params, join)) return MakeError("invalid_arguments", "station_id must be an existing station");
	bool adjacent = false;
	if (!ReadOptionalBool(params, "adjacent", adjacent)) return MakeError("invalid_arguments", "adjacent must be a boolean");

	Json result = Json::object();
	result["op"] = "station";
	result["station_type"] = "rail";
	result["tile"] = TileJson(tile);
	result["axis"] = axis == Axis::X ? "x" : "y";
	result["num_tracks"] = numtracks;
	result["platform_length"] = plat_len;

	const bool ok = RunCommand<Commands::BuildRailStation>(dry_run, tile, rt, axis, numtracks, plat_len, STAT_CLASS_DFLT, 0, join, adjacent);
	result = CommandResult(_posted, std::move(result));
	if (ok && !dry_run) {
		const StationID created = StationAt(tile);
		if (created != StationID::Invalid()) result["station_id"] = created.base();
	}
	return MarkExecution(std::move(result), dry_run);
}

/**
 * Build one airport.
 * @param params Method parameters.
 * @param dry_run True to only query the cost.
 * @return The tool result.
 */
Json ConstructAirport(const Json &params, bool dry_run)
{
	uint8_t type;
	if (!ReadAirportType(params, type)) {
		return MakeError("invalid_arguments", "airport needs `airport_type`: small, large, heliport, metropolitan, international, commuter, helidepot, intercontinental, helistation, oilrig, or an index");
	}
	TileIndex tile;
	if (!ReadSingleTile(params, tile)) return MakeError("invalid_arguments", "airport needs exactly one tile in `tiles`, or `from`");

	int64_t value = 0;
	if (!ReadOptionalInt(params, "layout", value)) return MakeError("invalid_arguments", "layout must be an integer");
	if (value < 0 || value > 255) return MakeError("invalid_arguments", "layout must be between 0 and 255");
	const uint8_t layout = static_cast<uint8_t>(value);

	StationID join = StationID::Invalid();
	if (!ReadJoinStation(params, join)) return MakeError("invalid_arguments", "station_id must be an existing station");
	bool adjacent = false;
	if (!ReadOptionalBool(params, "adjacent", adjacent)) return MakeError("invalid_arguments", "adjacent must be a boolean");

	Json result = Json::object();
	result["op"] = "airport";
	result["tile"] = TileJson(tile);
	result["airport_type"] = static_cast<int>(type);

	const bool ok = RunCommand<Commands::BuildAirport>(dry_run, tile, type, layout, join, adjacent);
	result = CommandResult(_posted, std::move(result));
	if (ok && !dry_run) {
		const StationID created = StationAt(tile);
		if (created != StationID::Invalid()) result["station_id"] = created.base();
	}
	return MarkExecution(std::move(result), dry_run);
}

/**
 * Build one dock.
 * @param params Method parameters.
 * @param dry_run True to only query the cost.
 * @return The tool result.
 */
Json ConstructDock(const Json &params, bool dry_run)
{
	TileIndex tile;
	if (!ReadSingleTile(params, tile)) return MakeError("invalid_arguments", "dock needs exactly one tile in `tiles`, or `from`");

	StationID join = StationID::Invalid();
	if (!ReadJoinStation(params, join)) return MakeError("invalid_arguments", "station_id must be an existing station");
	bool adjacent = false;
	if (!ReadOptionalBool(params, "adjacent", adjacent)) return MakeError("invalid_arguments", "adjacent must be a boolean");

	Json result = Json::object();
	result["op"] = "dock";
	result["tile"] = TileJson(tile);

	const bool ok = RunCommand<Commands::BuildDock>(dry_run, tile, join, adjacent);
	result = CommandResult(_posted, std::move(result));
	if (ok && !dry_run) {
		const StationID created = StationAt(tile);
		if (created != StationID::Invalid()) result["station_id"] = created.base();
	}
	return MarkExecution(std::move(result), dry_run);
}

/**
 * Build one rail or ship depot.
 * @param params Method parameters.
 * @param company Company that pays and owns the result.
 * @param dry_run True to only query the cost.
 * @return The tool result.
 */
Json ConstructDepot(const Json &params, CompanyID company, bool dry_run)
{
	std::string type = "rail";
	if (!ReadOptionalString(params, "depot_type", type)) return MakeError("invalid_arguments", "depot_type must be a string");
	if (type.empty()) type = "rail";

	TileIndex tile;
	if (!ReadSingleTile(params, tile)) return MakeError("invalid_arguments", "depot needs exactly one tile in `tiles`, or `from`");

	if (type == "air") {
		return MakeError("unsupported_op", "air depots are hangars that come with an airport; build an airport instead");
	}

	Json result = Json::object();
	result["op"] = "depot";
	result["depot_type"] = type;
	result["tile"] = TileJson(tile);

	bool ok = false;
	if (type == "rail") {
		RailType rt;
		if (!ReadRailType(params, company, rt)) {
			return MakeError("invalid_arguments", "a rail depot needs `rail_type`: a four character label such as RAIL, or an index, available to the company");
		}
		DiagDirection dir = DiagDirection::NE;
		if (!ReadDiagDirection(params, dir)) return MakeError("invalid_arguments", "direction must be ne, se, sw or nw");
		result["direction"] = DiagDirectionName(dir);
		ok = RunCommand<Commands::BuildRailDepot>(dry_run, tile, rt, dir);
	} else if (type == "ship") {
		Axis axis = Axis::X;
		if (!ReadAxis(params, axis)) return MakeError("invalid_arguments", "axis must be `x` or `y`");
		result["axis"] = axis == Axis::X ? "x" : "y";
		ok = RunCommand<Commands::BuildShipDepot>(dry_run, tile, axis);
	} else {
		return MakeError("invalid_arguments", "depot_type must be rail, air or ship");
	}

	result = CommandResult(_posted, std::move(result));
	if (ok && !dry_run) {
		const DepotID created = DepotAt(tile);
		if (created != DepotID::Invalid()) result["depot_id"] = created.base();
	}
	return MarkExecution(std::move(result), dry_run);
}

/**
 * Build one bridge.
 * @param params Method parameters.
 * @param company Company that pays and owns the result.
 * @param dry_run True to only query the cost.
 * @return The tool result.
 */
Json ConstructBridge(const Json &params, CompanyID company, bool dry_run)
{
	TileIndex from;
	TileIndex to;
	Axis axis;
	if (!ReadRun(params, from, to, axis)) return MakeError("invalid_arguments", "bridge needs `from` and `to` tiles inside the map that share a row or a column");

	TransportType transport = TransportType::Rail;
	if (!ReadTransportType(params, transport)) return MakeError("invalid_arguments", "transport_type must be rail, road or water");
	if (transport == TransportType::Air) return MakeError("invalid_arguments", "there are no air bridges");

	RailType rt = INVALID_RAILTYPE;
	RoadType roadtype = ROADTYPE_ROAD;
	if (transport == TransportType::Rail) {
		if (!ReadRailType(params, company, rt)) {
			return MakeError("invalid_arguments", "a rail bridge needs `rail_type`: a four character label such as RAIL, or an index, available to the company");
		}
	} else if (transport == TransportType::Road) {
		if (!ReadRoadType(params, roadtype)) return MakeError("invalid_arguments", "road_type must be `road` or `tram`");
	}

	BridgeType bridge_type;
	if (!ReadBridgeType(params, bridge_type)) {
		return MakeError("invalid_arguments", "bridge needs `bridge_type`: an index below 13, or the bridge's own name");
	}

	Json result = Json::object();
	result["op"] = "bridge";
	result["from"] = TileJson(from);
	result["to"] = TileJson(to);
	result["bridge_type"] = static_cast<int>(bridge_type);

	RunCommand<Commands::BuildBridge>(dry_run, to, from, transport, bridge_type, rt, roadtype);
	return MarkExecution(CommandResult(_posted, std::move(result)), dry_run);
}

/**
 * Build one tunnel.
 * @param params Method parameters.
 * @param company Company that pays and owns the result.
 * @param dry_run True to only query the cost.
 * @return The tool result.
 */
Json ConstructTunnel(const Json &params, CompanyID company, bool dry_run)
{
	TileIndex tile;
	if (!ReadSingleTile(params, tile)) return MakeError("invalid_arguments", "tunnel needs exactly one tile in `tiles`, or `from`");

	TransportType transport = TransportType::Rail;
	if (!ReadTransportType(params, transport)) return MakeError("invalid_arguments", "transport_type must be rail, road or water");
	if (transport == TransportType::Air) return MakeError("invalid_arguments", "there are no air tunnels");

	/* Tunnels have no type of their own; `tunnel_type` is the schema's name for the same thing. */
	std::string tunnel_type;
	if (!ReadOptionalString(params, "tunnel_type", tunnel_type)) return MakeError("invalid_arguments", "tunnel_type must be a string");
	if (!tunnel_type.empty()) {
		TransportType named;
		if (!ParseTransportName(tunnel_type, named)) return MakeError("invalid_arguments", "tunnel_type must be rail, road or water");
		if (named != transport) return MakeError("invalid_arguments", "tunnel_type and transport_type disagree");
	}

	RailType rt = INVALID_RAILTYPE;
	RoadType roadtype = ROADTYPE_ROAD;
	if (transport == TransportType::Rail) {
		if (!ReadRailType(params, company, rt)) {
			return MakeError("invalid_arguments", "a rail tunnel needs `rail_type`: a four character label such as RAIL, or an index, available to the company");
		}
	} else if (transport == TransportType::Road) {
		if (!ReadRoadType(params, roadtype)) return MakeError("invalid_arguments", "road_type must be `road` or `tram`");
	}

	Json result = Json::object();
	result["op"] = "tunnel";
	result["tile"] = TileJson(tile);

	RunCommand<Commands::BuildTunnel>(dry_run, tile, transport, rt, roadtype);
	return MarkExecution(CommandResult(_posted, std::move(result)), dry_run);
}

/**
 * Build one canal, river or stretch of sea along a straight run.
 * @param params Method parameters.
 * @param dry_run True to only query the cost.
 * @return The tool result.
 */
Json ConstructCanal(const Json &params, bool dry_run)
{
	TileIndex from;
	TileIndex to;
	Axis axis;
	if (!ReadRun(params, from, to, axis)) return MakeError("invalid_arguments", "canal needs `from` and `to` tiles inside the map that share a row or a column");

	WaterClass wc = WaterClass::Canal;
	if (!ReadWaterClass(params, wc)) return MakeError("invalid_arguments", "water_class must be canal, river or sea");
	bool diagonal = false;
	if (!ReadOptionalBool(params, "diagonal", diagonal)) return MakeError("invalid_arguments", "diagonal must be a boolean");

	Json result = Json::object();
	result["op"] = "canal";
	result["from"] = TileJson(from);
	result["to"] = TileJson(to);

	RunCommand<Commands::BuildCanal>(dry_run, to, from, wc, diagonal);
	return MarkExecution(CommandResult(_posted, std::move(result)), dry_run);
}

/**
 * Raise, lower or level one tile, or terraform selected corners of it.
 * @param params Method parameters.
 * @param dry_run True to only query the cost.
 * @return The tool result.
 */
Json ConstructTerraform(const Json &params, bool dry_run)
{
	TileIndex tile;
	if (!ReadSingleTile(params, tile)) return MakeError("invalid_arguments", "terraform needs exactly one tile in `tiles`, or `from`");

	std::string mode;
	if (!ReadString(params, "terraform", mode)) return MakeError("invalid_arguments", "terraform needs `terraform`: raise, lower or level");

	Json result = Json::object();
	result["op"] = "terraform";
	result["tile"] = TileJson(tile);
	result["terraform"] = mode;

	if (params.contains("slope")) {
		Slope corners;
		if (!ReadSlope(params, corners)) return MakeError("invalid_arguments", "slope must name corners with the letters n, e, s and w");
		if (mode != "raise" && mode != "lower") return MakeError("invalid_arguments", "a corner mask needs `terraform` raise or lower");
		result["slope"] = params["slope"];
		RunCommand<Commands::TerraformLand>(dry_run, tile, corners, mode == "raise");
		return MarkExecution(CommandResult(_posted, std::move(result)), dry_run);
	}

	LevelMode level_mode;
	if (mode == "raise") {
		level_mode = LevelMode::Raise;
	} else if (mode == "lower") {
		level_mode = LevelMode::Lower;
	} else if (mode == "level") {
		level_mode = LevelMode::Level;
	} else {
		return MakeError("invalid_arguments", "terraform must be raise, lower or level");
	}

	RunCommand<Commands::LevelLand>(dry_run, tile, tile, false, level_mode);
	return MarkExecution(CommandResult(_posted, std::move(result)), dry_run);
}

/**
 * Remove tram track along a straight run.
 *
 * This is the one tram case the road module reports as a gap: it builds tram but does not
 * demolish it. Everything else about trams stays with that module.
 *
 * @param params Method parameters.
 * @param dry_run True to only query the cost.
 * @return The tool result.
 */
Json DemolishTram(const Json &params, bool dry_run)
{
	TileIndex from;
	TileIndex to;
	Axis axis;
	if (!ReadRun(params, from, to, axis)) return MakeError("invalid_arguments", "tram removal needs `from` and `to` tiles inside the map that share a row or a column");

	Json result = Json::object();
	result["op"] = "tram";
	result["demolish"] = true;
	result["from"] = TileJson(from);
	result["to"] = TileJson(to);

	RunCommand<Commands::RemoveRoadLong>(dry_run, to, from, ROADTYPE_TRAM, axis, false, false);
	return MarkExecution(CommandResult(_posted, std::move(result)), dry_run);
}

/**
 * Decide whether an op=station call is one this module owns.
 *
 * Road stops stay with the road module, so a station call without an explicit rail, airport or
 * dock marker is left to it.
 *
 * @param params Method parameters.
 * @return True when the call names a rail station, an airport or a dock.
 */
bool IsStationOp(const Json &params)
{
	std::string type;
	if (ReadString(params, "station_type", type)) {
		return type == "rail" || type == "airport" || type == "dock";
	}
	return params.contains("rail_type") || params.contains("airport_type");
}

/**
 * Decide whether an op=depot call is one this module owns.
 *
 * Road and tram depots stay with the road module, so a depot call that names a road type is left
 * to it.
 *
 * @param params Method parameters.
 * @return True when the call names a rail, air or ship depot.
 */
bool IsDepotOp(const Json &params)
{
	std::string type;
	if (ReadString(params, "depot_type", type)) {
		return type == "rail" || type == "air" || type == "ship";
	}
	return !params.contains("road_type");
}

/**
 * Dispatch an op=station call onto the station family it names.
 * @param params Method parameters.
 * @param company Company that pays and owns the result.
 * @param dry_run True to only query the cost.
 * @return The tool result.
 */
Json ConstructStation(const Json &params, CompanyID company, bool dry_run)
{
	std::string type;
	if (!ReadString(params, "station_type", type)) {
		type = params.contains("airport_type") ? "airport" : "rail";
	}
	if (type == "rail") return ConstructRailStation(params, company, dry_run);
	if (type == "airport") return ConstructAirport(params, dry_run);
	if (type == "dock") return ConstructDock(params, dry_run);
	return MakeError("invalid_arguments", "station_type must be rail, airport or dock");
}

/**
 * Describe one method of the capability document.
 * @param method Engine method name.
 * @param authority `read` or `write`.
 * @param implemented Ops this module implements.
 * @param gaps Ops this module knows about but leaves to another module.
 * @return Capability entry.
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

} // namespace

Json TransportToolCapabilities()
{
	Json methods = Json::array();
	methods.push_back(Capability("world.construct", "write",
			{"rail", "rail.demolish", "signal", "signal.demolish", "station.rail", "station.airport", "station.dock",
					"airport", "dock", "depot.rail", "depot.ship", "bridge", "tunnel", "canal", "terraform", "tram.demolish"},
			{"road", "tram", "demolish", "station.road", "depot.road", "depot.tram", "depot.air"}));

	Json result = Json::object();
	result["module"] = "agent_transport_tools";
	result["methods"] = std::move(methods);
	result["unimplemented_methods"] = Json::array({"world.query", "world.vehicles", "world.orders", "world.info", "world.objects", "world.pause", "world.rename", "world.settings.get", "world.settings.set", "world.capture", "task.barrier"});
	result["notes"] = Json::array({
		"Actual mutations go through Command::Post, the normal player command path, so the engine's own eligibility, pause and finance checks apply; results carry the actual cost and the created station/depot ids.",
		"world.construct accepts `dry_run`, which only queries the cost and reports `estimated: true`.",
		"op=rail builds or removes track on `tiles`, or along a straight `from`/`to` run; `track` defaults to the run's axis.",
		"op=signal builds or removes one signal per tile; `signal_type` is block (default), entry, exit, combo, path or path_one_way.",
		"op=station is claimed only when `station_type` is rail, airport or dock, or when `rail_type`/`airport_type` is given; road stops stay with the road module.",
		"op=depot is claimed for rail, air and ship depots; road and tram depots stay with the road module, and an air depot is reported as a gap because hangars come with an airport.",
		"op=bridge and op=tunnel take `transport_type` rail (default), road or water; `bridge_type` is an index below 13 or the bridge's own name.",
		"op=terraform takes `terraform` raise, lower or level for a whole tile, or `terraform` raise/lower plus a `slope` corner mask such as `ne`.",
		"op=tram is claimed only for `demolish`, the one tram case the road module reports as a gap.",
		"Every tile, direction, type id and width is validated before the first command of a batch runs; a batch reports partial receipts and the money it actually spent.",
	});
	return result;
}

std::optional<Json> DispatchTransportTool(std::string_view method, const Json &params, CompanyID company)
{
	if (method != "world.construct") return std::nullopt;
	if (!params.is_object()) return MakeError("invalid_arguments", "params must be an object");

	std::string op;
	if (!ReadString(params, "op", op)) return MakeError("invalid_arguments", "op is required");

	bool demolish = false;
	if (!ReadOptionalBool(params, "demolish", demolish)) return MakeError("invalid_arguments", "demolish must be a boolean");
	bool dry_run = false;
	if (!ReadOptionalBool(params, "dry_run", dry_run)) return MakeError("invalid_arguments", "dry_run must be a boolean");

	/* Ops another module owns: returning nullopt lets the bridge fall through to it. */
	if (op == "road" || op == "demolish") return std::nullopt;
	if (op == "tram" && !demolish) return std::nullopt;
	if (op == "station" && !IsStationOp(params)) return std::nullopt;
	if (op == "depot" && !IsDepotOp(params)) return std::nullopt;

	/* An op this module does not implement at all is not its either, whatever the company is. */
	static const std::array<std::string_view, 10> owned_ops = {"rail", "signal", "station", "airport", "dock", "depot", "bridge", "tunnel", "canal", "terraform"};
	if (op != "tram" && std::find(owned_ops.begin(), owned_ops.end(), std::string_view{op}) == owned_ops.end()) return std::nullopt;

	if (const std::optional<Json> error = CheckWriteContext(company, dry_run); error.has_value()) return error;

	CompanyContext context(company);

	if (op == "rail") return ConstructRail(params, company, demolish, dry_run);
	if (op == "signal") return ConstructSignal(params, demolish, dry_run);
	if (op == "station") return ConstructStation(params, company, dry_run);
	if (op == "airport") return ConstructAirport(params, dry_run);
	if (op == "dock") return ConstructDock(params, dry_run);
	if (op == "depot") return ConstructDepot(params, company, dry_run);
	if (op == "bridge") return ConstructBridge(params, company, dry_run);
	if (op == "tunnel") return ConstructTunnel(params, company, dry_run);
	if (op == "canal") return ConstructCanal(params, dry_run);
	if (op == "terraform") return ConstructTerraform(params, dry_run);
	if (op == "tram") return DemolishTram(params, dry_run);

	return std::nullopt;
}

} // namespace InGameAgent
