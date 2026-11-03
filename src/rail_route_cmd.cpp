/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/**
 * @file rail_route_cmd.cpp The command that prepares the corridor of a rail trunk line: clear areas and level corners.
 *
 * The trunk line tool is a big construction of its own, so this command lifts the landscaping limits of the
 * company while it runs, and demolishes town houses, industries and town roads like the "magic bulldozer"
 * (without the town rating penalty) when the player asked for it. Everything else follows the normal rules:
 * the property of other companies, own rail and stations are never removed.
 */

#include "stdafx.h"
#include "rail_route_cmd.h"
#include "cheat_type.h"
#include "command_func.h"
#include "company_base.h"
#include "company_func.h"
#include "landscape_cmd.h"
#include "map_func.h"
#include "road_map.h"
#include "slope_type.h"
#include "terraform_cmd.h"
#include "tile_map.h"
#include "debug.h"
#include "strings_func.h"

#include "table/strings.h"

#include "safeguards.h"

/** Reader of the items of a chunk; reading past the end sets #ok to false. */
struct RouteChunkReader {
	std::span<const uint8_t> in; ///< The chunk data.
	size_t pos = 0; ///< Read position.
	bool ok = true; ///< No read went past the end.

	bool AtEnd() const { return this->pos >= this->in.size(); }
	uint8_t U8()
	{
		if (this->pos >= this->in.size()) {
			this->ok = false;
			return 0;
		}
		return this->in[this->pos++];
	}
	uint16_t U16() { uint16_t lo = this->U8(); return lo | static_cast<uint16_t>(this->U8()) << 8; }
};

/**
 * Should a tile of the corridor be cleared before levelling?
 * Grass, rocks, fields and trees are cleared by the levelling and the track themselves.
 * @param tile The tile.
 * @param water Clear water.
 * @param demolish Demolish town houses, industries, town roads and unowned objects.
 * @return True iff the tile is to be cleared.
 */
static bool IsRouteTileToClear(TileIndex tile, bool water, bool demolish)
{
	switch (GetTileType(tile)) {
		case TileType::Water: return water;
		case TileType::House:
		case TileType::Industry: return demolish;
		case TileType::Road: {
			if (!demolish || !IsNormalRoadTile(tile)) return false;
			Owner owner = GetRoadOwner(tile, RoadTramType::Road);
			return owner == OWNER_TOWN || owner == OWNER_NONE;
		}
		case TileType::Object: return demolish && GetTileOwner(tile) == OWNER_NONE;
		default: return false;
	}
}

/**
 * Is one of the four tiles around a tile corner a water tile?
 * @param x X of the corner (the north corner of tile x, y).
 * @param y Y of the corner.
 * @return True iff a tile touching the corner is water.
 */
static bool IsCornerNextToWater(uint x, uint y)
{
	for (uint dy = 0; dy < 2; dy++) {
		for (uint dx = 0; dx < 2; dx++) {
			if (x < dx || y < dy) continue;
			TileIndex t = TileXY(x - dx, y - dy);
			if (IsTileType(t, TileType::Water)) return true;
		}
	}
	return false;
}

/**
 * Prepare a part of the corridor of a rail trunk line.
 * @param flags Operation to perform.
 * @param origin Tile the item offsets are relative to.
 * @param chunk The items, see #RailRouteItemKind.
 * @return The cost (or the error when nothing was done), the money missing for the next step, the number of
 *         tiles cleared and corners levelled, the number that failed, and the last error.
 */
std::tuple<CommandCost, Money, uint32_t, uint32_t, StringID> CmdPrepareRailRoute(DoCommandFlags flags, TileIndex origin, const BlueprintChunkData &chunk)
{
	if (origin >= Map::Size() || chunk.data.empty() || chunk.data.size() > BlueprintChunkData::MAX_SIZE) return {CMD_ERROR, 0, 0, 0, INVALID_STRING_ID};

	/* Validate the whole chunk first. */
	bool demolish_any = false;
	{
		RouteChunkReader r{chunk.data};
		while (!r.AtEnd()) {
			uint8_t kind = r.U8();
			if (kind >= to_underlying(RailRouteItemKind::End)) return {CMD_ERROR, 0, 0, 0, INVALID_STRING_ID};
			r.U16(); r.U16();
			uint16_t w = r.U16(), h = r.U16();
			r.U8();
			uint8_t item_flags = r.U8();
			if (!r.ok || w == 0 || h == 0) return {CMD_ERROR, 0, 0, 0, INVALID_STRING_ID};
			if (HasBit(item_flags, to_underlying(RailRouteClearFlag::Demolish))) demolish_any = true;
		}
	}

	const bool exec = flags.Test(DoCommandFlag::Execute);
	const DoCommandFlags test_flags = DoCommandFlags{flags}.Reset(DoCommandFlag::Execute);
	Money money = GetAvailableMoneyForCommand();
	CommandCost total(ExpensesType::Construction);
	uint32_t done = 0;
	uint32_t failed = 0;
	StringID last_error = INVALID_STRING_ID;
	Money needed = 0;

	/* The trunk line is one big construction: no landscaping limits while it runs, and demolishing as asked. */
	Company *c = Company::GetIfValid(_current_company);
	uint32_t old_terraform_limit = c != nullptr ? c->terraform_limit : 0;
	uint32_t old_clear_limit = c != nullptr ? c->clear_limit : 0;
	if (c != nullptr) {
		c->terraform_limit = UINT32_MAX;
		c->clear_limit = UINT32_MAX;
	}
	bool old_bulldozer = _cheats.magic_bulldozer.value;
	if (demolish_any) _cheats.magic_bulldozer.value = true;

	/* The landscaping limits are lifted again before every step: one chunk may terraform more than the limit can hold. */
	auto lift_limits = [&]() {
		if (c == nullptr) return;
		c->terraform_limit = UINT32_MAX;
		c->clear_limit = UINT32_MAX;
	};

	/* Do one step: test, check the money, execute. Returns whether it was done. */
	auto attempt = [&](TileIndex tile, auto &&action) -> bool {
		lift_limits();
		CommandCost ret = action(test_flags);
		if (ret.Failed()) {
			failed++;
			last_error = ret.GetErrorMessage();
			if (exec) Debug(Facility::Misc, Severity::Info, "[route] prepare failed at {}x{}: {}", TileX(tile), TileY(tile), last_error == INVALID_STRING_ID ? std::string("generic error") : GetString(last_error));
			return false;
		}
		if (exec) {
			if (ret.GetCost() > 0 && money < ret.GetCost()) {
				needed = ret.GetCost();
				return false;
			}
			ret = action(flags);
			if (ret.Failed()) {
				failed++;
				last_error = ret.GetErrorMessage();
				return false;
			}
			money -= ret.GetCost();
		}
		total.AddCost(ret.GetCost());
		return true;
	};

	RouteChunkReader r{chunk.data};
	while (!r.AtEnd() && needed == 0) {
		uint8_t kind = r.U8();
		uint16_t x0 = r.U16(), y0 = r.U16(), w = r.U16(), h = r.U16();
		uint8_t value = r.U8();
		uint8_t item_flags = r.U8();
		uint ox = TileX(origin), oy = TileY(origin);
		bool water = HasBit(item_flags, to_underlying(RailRouteClearFlag::Water));
		bool demolish = HasBit(item_flags, to_underlying(RailRouteClearFlag::Demolish));

		if (kind == to_underlying(RailRouteItemKind::Clear)) {
			for (uint y = oy + y0; y < oy + y0 + h && needed == 0; y++) {
				for (uint x = ox + x0; x < ox + x0 + w && needed == 0; x++) {
					if (x >= Map::MaxX() || y >= Map::MaxY()) continue;
					TileIndex tile = TileXY(x, y);
					if (!IsRouteTileToClear(tile, water, demolish)) continue;
					if (attempt(tile, [&](DoCommandFlags f) { return Command<Commands::LandscapeClear>::Do(f, tile); })) done++;
				}
			}
		} else {
			/* Level: every corner of the rectangle to the height. A corner is the north corner of the tile with its coordinates. */
			int target = value;
			for (uint y = oy + y0; y < oy + y0 + h && needed == 0; y++) {
				for (uint x = ox + x0; x < ox + x0 + w && needed == 0; x++) {
					if (x >= Map::MaxX() || y >= Map::MaxY()) continue;
					TileIndex tile = TileXY(x, y);
					if (static_cast<int>(TileHeight(tile)) == target) continue;
					/* Keeping the water: leave the corners of water tiles alone. */
					if (!water && IsCornerNextToWater(x, y)) continue;
					bool ok = true;
					/* In a test run nothing changes, so test a single level. */
					int steps = exec ? std::abs(static_cast<int>(TileHeight(tile)) - target) : 1;
					int clears = 0;
					for (int i = 0; i < steps && ok && needed == 0; i++) {
						bool up = static_cast<int>(TileHeight(tile)) < target;
						/* Something beside the corridor may be in the way of the reshaped land: clear it when the options allow and retry. */
						lift_limits();
						auto [test, extra, err_tile] = Command<Commands::TerraformLand>::Do(test_flags, tile, SLOPE_N, up);
						if (exec && test.Failed() && clears < 8 && err_tile != INVALID_TILE && err_tile < Map::Size() && IsRouteTileToClear(err_tile, water, demolish) &&
								attempt(err_tile, [&](DoCommandFlags f) { return Command<Commands::LandscapeClear>::Do(f, err_tile); })) {
							clears++;
							i--;
							continue;
						}
						ok = attempt(tile, [&](DoCommandFlags f) { return std::get<0>(Command<Commands::TerraformLand>::Do(f, tile, SLOPE_N, up)); });
					}
					if (ok && needed == 0) done++;
				}
			}
		}
	}

	_cheats.magic_bulldozer.value = old_bulldozer;
	if (c != nullptr) {
		c->terraform_limit = old_terraform_limit;
		c->clear_limit = old_clear_limit;
	}

	if (needed != 0) return {total, needed, done, failed, last_error};
	if (exec && done == 0 && failed != 0) return {CommandCost(last_error), 0, done, failed, last_error};
	return {total, 0, done, failed, last_error};
}
