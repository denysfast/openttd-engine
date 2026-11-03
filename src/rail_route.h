/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/**
 * @file rail_route.h Rail trunk line tool: plan and build many parallel one-way tracks from one tile to another.
 *
 * The route goes from the start tile to the end tile along the major axis, made of straight runs and 45 degree
 * diagonal runs (a diagonal step moves one tile along both axes). Every track of the corridor is the same
 * polyline moved sideways, so parallel tracks never cross. Diagonal track cannot climb in OpenTTD, so the
 * height only changes on straight runs; a dynamic programme chooses where the diagonal runs go and the
 * height of the corridor, minimising earthworks and demolition. The corridor is then cleared, levelled
 * (sloped along the track on ramps), and the tracks and one-way signals are built.
 *
 * Planning runs on the client and only reads the map. Building is a list of commands sent by the
 * construction job of blueprint.cpp: #Commands::PrepareRailRoute (clear and level areas) followed by
 * #Commands::PasteBlueprint (track and signals).
 */

#ifndef RAIL_ROUTE_H
#define RAIL_ROUTE_H

#include "blueprint.h"
#include "economy_type.h"
#include "rail_type.h"
#include "signal_type.h"
#include "tile_type.h"

/** Options of a trunk line. */
struct RailRouteOptions {
	static constexpr uint MAX_TRACKS = 30; ///< Maximum number of parallel tracks.

	uint8_t tracks = 2; ///< Number of parallel tracks, 1..#MAX_TRACKS.
	RailType railtype = RAILTYPE_RAIL; ///< Rail type to build.
	bool signals = true; ///< Build one-way signals.
	uint8_t signal_interval = 6; ///< Distance between signals, in steps along the route.
	SignalType signal_type = SignalType::PathOneWay; ///< Type of the signals.
	SignalVariant signal_variant = SignalVariant::Electric; ///< Variant of the signals.
	bool remove_water = true; ///< Clear water (sea, lakes, rivers, canals) in the corridor and fill it.
	bool demolish = true; ///< Demolish town houses, industries and town roads in the corridor.
	bool raise_sea_level = true; ///< Keep the corridor at least one level above the sea.
};

/** Item kinds of a #Commands::PrepareRailRoute chunk. */
enum class RailRouteItemKind : uint8_t {
	Clear = 0, ///< x, y, width, height (u16, tiles relative to the origin), flags (u8, #RailRouteClearFlag).
	Level = 1, ///< x, y, width, height (u16, tile corners relative to the origin), height (u8): set every corner to the height.
	End, ///< End marker.
};

/** What a clear item may remove besides trees, rocks and fields. */
enum class RailRouteClearFlag : uint8_t {
	Water = 0, ///< Water tiles.
	Demolish = 1, ///< Town houses, industries, town roads and unowned objects.
};

/** Estimated costs of a trunk line. */
struct RailRouteEstimate {
	Money clearing = 0; ///< Clearing tiles, demolishing and removing water.
	Money terraform = 0; ///< Levelling the corridor (without the knock-on effects outside it).
	Money track = 0; ///< Track and signals.
	uint houses = 0; ///< House tiles to demolish.
	uint industries = 0; ///< Industries to demolish.
	uint water = 0; ///< Water tiles to fill.
	uint blocked = 0; ///< Tiles that cannot be cleared (other companies, own rail, stations...).

	Money Total() const { return this->clearing + this->terraform + this->track; }
};

/** A planned trunk line. */
struct RailRoutePlan {
	std::string error{}; ///< Why the route cannot be built; empty when it can.
	TileIndex origin = INVALID_TILE; ///< Tile the offsets of all chunks are relative to.
	std::vector<BlueprintChunkData> prepare{}; ///< #Commands::PrepareRailRoute chunks: clearing first, then levelling.
	std::vector<BlueprintChunkData> build{}; ///< #Commands::PasteBlueprint chunks: track, then signals.
	std::vector<std::pair<TileIndex, TrackBits>> tracks{}; ///< Track of the route per tile, for the preview; sorted by tile.
	RailRouteEstimate estimate{}; ///< Estimated costs.
	uint length = 0; ///< Length of the route in steps along the major axis.
	uint bends = 0; ///< Number of 45 degree bends.
	int min_height = 0; ///< Lowest height of the track.
	int max_height = 0; ///< Highest height of the track.
};

RailRoutePlan PlanRailRoute(TileIndex from, TileIndex to, const RailRouteOptions &options);

/* Exposed for the tests: the steps of a route and the pieces of its tracks in the canonical frame. */

/** One step of a route in the canonical frame (u along the major axis, v along the minor axis, both increasing). */
struct RailRouteStep {
	bool diagonal = false; ///< A diagonal step (moves +1 u and +1 v), or a straight one (+1 u).
	int height = 0; ///< Height of the track at the start of the step.
};

/** A track piece of one track in the canonical frame. */
struct RailRoutePiece {
	int u = 0; ///< Tile u.
	int v = 0; ///< Tile v.
	Track track = Track::Invalid; ///< The piece, as if u were x and v were y.
	Trackdir trackdir = Trackdir::Invalid; ///< Direction of travel (from the start to the end).
	int step = 0; ///< Index of the step the piece belongs to.
};

std::vector<RailRoutePiece> GetRailRoutePieces(const std::vector<RailRouteStep> &steps);

#endif /* RAIL_ROUTE_H */
