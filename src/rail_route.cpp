/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/**
 * @file rail_route.cpp Planning of the rail trunk line tool.
 *
 * Canonical frame: u runs along the major axis of the route from the start (u = 0) to the end (u = L), v runs
 * along the minor axis and increases towards the end (v = 0 .. M). In this frame the route is a list of L steps:
 * a straight step moves +1 u, a diagonal step +1 u and +1 v. Track pieces are named as if u were x and v were y;
 * the frame is mapped to the map by flipping and/or swapping the axes.
 *
 * Shape: compare a bounded set of straight/diagonal routes with up to two bends. Reuse existing
 * connections, avoid protected obstacles and prefer continuous tracks on the original terrain.
 * When earthworks are necessary, a dynamic programme chooses every corner column separately;
 * diagonal columns stay level and straight columns have gentle ramps.
 */

#include "stdafx.h"
#include <map>
#include <set>
#include "rail_route.h"
#include "debug.h"
#include "fileio_func.h"
#include "bridge_map.h"
#include "clear_map.h"
#include "tree_map.h"
#include "tunnelbridge_map.h"
#include "house.h"
#include "town_map.h"
#include "economy_func.h"
#include "industry.h"
#include "industry_map.h"
#include "map_func.h"
#include "rail.h"
#include "company_func.h"
#include "landscape.h"
#include "station_map.h"
#include "road_map.h"
#include "settings_type.h"
#include "signal_func.h"
#include "tile_map.h"
#include "track_func.h"
#include "water_map.h"

#include "safeguards.h"

/** A ramp tile is followed by at least this many steps before the next one. */
static constexpr int ROUTE_RAMP_SPACING = 4;
/** Cost of an obstacle which the selected options cannot remove. */
static constexpr int64_t ROUTE_BLOCKED_COST = 1'000'000;

/** Item size limit of the chunks, see #BlueprintChunkData::MAX_SIZE. */
static constexpr size_t CHUNK_LIMIT = BlueprintChunkData::MAX_SIZE;

/** Mapping of the canonical frame to the map. */
struct RouteFrame {
	int ax; ///< X of the start tile.
	int ay; ///< Y of the start tile.
	int sx; ///< Direction of the map x axis that the route follows: +1 or -1.
	int sy; ///< Direction of the map y axis: +1 or -1.
	bool swap; ///< The major axis is the map y axis.

	/** Map x and y of the tile at canonical u, v. */
	std::pair<int, int> Tile(int u, int v) const
	{
		if (this->swap) return {this->ax + this->sx * v, this->ay + this->sy * u};
		return {this->ax + this->sx * u, this->ay + this->sy * v};
	}

	/** Map x and y of the tile corner at canonical corner u, v (the corner with the lowest u and v of tile u, v). */
	std::pair<int, int> Corner(int u, int v) const
	{
		auto axis = [](int a, int s, int c) { return s > 0 ? a + c : a - c + 1; };
		if (this->swap) return {axis(this->ax, this->sx, v), axis(this->ay, this->sy, u)};
		return {axis(this->ax, this->sx, u), axis(this->ay, this->sy, v)};
	}

	/** Map direction of a canonical edge (as if u were x and v were y). */
	DiagDirection Edge(DiagDirection canonical) const
	{
		/* Canonical: NE = -u, SW = +u, NW = -v, SE = +v. */
		bool along_u = canonical == DiagDirection::NE || canonical == DiagDirection::SW;
		bool positive = canonical == DiagDirection::SW || canonical == DiagDirection::SE;
		bool map_x = along_u != this->swap;
		int sign = (map_x ? this->sx : this->sy) * (positive ? 1 : -1);
		if (map_x) return sign > 0 ? DiagDirection::SW : DiagDirection::NE;
		return sign > 0 ? DiagDirection::SE : DiagDirection::NW;
	}
};

/** The two tile edges every track connects. */
static constexpr std::array<std::pair<DiagDirection, DiagDirection>, to_underlying(Track::End)> _route_track_edges = {{
	{DiagDirection::NE, DiagDirection::SW}, // X
	{DiagDirection::SE, DiagDirection::NW}, // Y
	{DiagDirection::NW, DiagDirection::NE}, // Upper
	{DiagDirection::SE, DiagDirection::SW}, // Lower
	{DiagDirection::SW, DiagDirection::NW}, // Left
	{DiagDirection::NE, DiagDirection::SE}, // Right
}};

/**
 * Get the track that connects two tile edges.
 * @param a One edge.
 * @param b The other edge.
 * @return The track.
 */
static Track TrackFromEdges(DiagDirection a, DiagDirection b)
{
	for (Track t : EnumRange(Track::End)) {
		auto [c, d] = _route_track_edges[to_underlying(t)];
		if ((c == a && d == b) || (c == b && d == a)) return t;
	}
	NOT_REACHED();
}

/**
 * Get the pieces of one track of a route in the canonical frame: a straight step is an X piece, a diagonal step
 * a Right piece and a Left piece one tile further in v. The end continues the last run.
 * @param steps The steps.
 * @return The pieces, in the order a train passes them.
 */
std::vector<RailRoutePiece> GetRailRoutePieces(const std::vector<RailRouteStep> &steps)
{
	std::vector<RailRoutePiece> pieces;
	int u = 0;
	int v = 0;
	for (size_t s = 0; s < steps.size(); s++) {
		if (steps[s].diagonal) {
			pieces.push_back({u, v, Track::Right, TrackExitdirToTrackdir(Track::Right, DiagDirection::SE), static_cast<int>(s)});
			pieces.push_back({u, v + 1, Track::Left, TrackExitdirToTrackdir(Track::Left, DiagDirection::SW), static_cast<int>(s)});
			v++;
		} else {
			pieces.push_back({u, v, Track::X, TrackExitdirToTrackdir(Track::X, DiagDirection::SW), static_cast<int>(s)});
		}
		u++;
	}
	Track last = !steps.empty() && steps.back().diagonal ? Track::Right : Track::X;
	pieces.push_back({u, v, last, TrackExitdirToTrackdir(last, last == Track::Right ? DiagDirection::SE : DiagDirection::SW), static_cast<int>(steps.size())});
	return pieces;
}

/** Existing tracks that can be reused without changing their owner or rail type. */
static TrackBits ExistingRouteTracks(TileIndex tile, RailType railtype)
{
	if (!IsPlainRailTile(tile) && !IsRailStationTile(tile)) return {};
	if (GetTileOwner(tile) != _current_company) return {};
	if (IsPlainRailTile(tile) && GetRailType(tile) == railtype) return GetTrackBits(tile);
	if (IsRailStationTile(tile) && GetRailType(tile) == railtype) return TrackBits{AxisToTrack(GetRailStationAxis(tile))};
	return {};
}

/** Height at the middle of an edge after applying its rail foundation. */
static int RouteEdgeHeight(TileIndex tile, TrackBits bits, DiagDirection edge)
{
	auto [slope, z] = GetTileSlopeZ(tile);
	Foundation foundation = GetRailFoundation(slope, bits);
	if (foundation == Foundation::Invalid) return -1;
	z = (z + ApplyFoundationToSlope(foundation, slope)) * TILE_HEIGHT;
	auto [near, far] = GetSlopePixelZOnEdge(slope, edge, z);
	return (near + far) / 2;
}

/** Context of planning one route. */
class RoutePlanner {
	const RailRouteOptions &options;
	RouteFrame frame;
	int length; ///< L: number of steps.
	int minor; ///< M: number of diagonal steps.
	int k0; ///< Lowest offset (canonical v) of a track.
	int k1; ///< Highest offset of a track.
	std::vector<std::pair<int, int>> tracks; ///< Every track: its offset, and how many leading pieces it skips to start on the fixed start pieces.
	std::vector<int> column_height; ///< Selected corner heights; empty when the original terrain is kept.
	std::map<TileIndex, TrackBits> track_tiles; ///< Selected route, including terminal pieces.
	std::vector<RailRoutePiece> pieces; ///< Selected canonical track pieces.
	std::string failure_detail; ///< Rejection counts for an unsuccessful search.

public:
	RoutePlanner(const RailRouteOptions &options, RouteFrame frame, int length, int minor) :
		options(options), frame(frame), length(length), minor(minor)
	{
		this->k0 = -static_cast<int>((options.tracks - 1) / 2);
		this->k1 = this->k0 + options.tracks - 1;
		for (int k = this->k0; k <= this->k1; k++) this->tracks.emplace_back(k, 0);
	}

	/**
	 * Use these tracks instead of the ones centred on the line from the start to the end.
	 * @param tracks Every track: offset and leading pieces to skip.
	 */
	void SetTracks(std::vector<std::pair<int, int>> &&tracks)
	{
		this->tracks = std::move(tracks);
		this->k0 = INT_MAX;
		this->k1 = INT_MIN;
		for (auto [k, skip] : this->tracks) {
			this->k0 = std::min(this->k0, k);
			this->k1 = std::max(this->k1, k);
		}
	}

	/** Height of a canonical corner, or -1 when it is off the map. */
	int CornerHeight(int u, int v) const
	{
		auto [x, y] = this->frame.Corner(u, v);
		if (x < 0 || y < 0 || x > static_cast<int>(Map::MaxX()) || y > static_cast<int>(Map::MaxY())) return -1;
		return TileHeight(TileXY(x, y));
	}

	/** Map tile at canonical u, v; INVALID_TILE when it is off the map. */
	TileIndex MapTile(int u, int v) const
	{
		auto [x, y] = this->frame.Tile(u, v);
		if (x < 1 || y < 1 || x + 1 >= static_cast<int>(Map::MaxX()) || y + 1 >= static_cast<int>(Map::MaxY())) return INVALID_TILE;
		return TileXY(x, y);
	}

	/**
	 * Cost of a tile of the corridor besides the earthworks: what must be demolished, and a high cost for what cannot be.
	 * @param tile The tile.
	 * @return The cost in units of earthworks.
	 */
	int64_t ObstacleCost(TileIndex tile) const
	{
		if (tile == INVALID_TILE) return ROUTE_BLOCKED_COST * 10;
		int64_t cost = 0;
		if (IsBridgeAbove(tile)) {
			TileIndex head = GetSouthernBridgeEnd(tile);
			bool removable = GetTunnelBridgeTransportType(head) == TransportType::Road &&
					(GetRoadOwner(head, RoadTramType::Road) == OWNER_TOWN || GetRoadOwner(head, RoadTramType::Road) == OWNER_NONE);
			cost = this->options.demolish && removable ? 20 : ROUTE_BLOCKED_COST;
		}
		switch (GetTileType(tile)) {
			case TileType::Railway:
			case TileType::Station:
				return cost + (ExistingRouteTracks(tile, this->options.railtype).Any() ? 0 : ROUTE_BLOCKED_COST);
			case TileType::Clear:
			case TileType::Trees:
				return cost;
			case TileType::Water:
				return cost + (this->options.remove_water ? 3 : ROUTE_BLOCKED_COST);
			case TileType::House:
				return cost + (this->options.demolish ? 30 : ROUTE_BLOCKED_COST);
			case TileType::Industry:
				return cost + (this->options.demolish ? 60 : ROUTE_BLOCKED_COST);
			case TileType::Road:
				if (this->options.demolish && IsNormalRoadTile(tile) && !IsLevelCrossing(tile)) {
					Owner owner = GetRoadOwner(tile, RoadTramType::Road);
					if (owner == OWNER_TOWN || owner == OWNER_NONE) return cost + 5;
				}
				return cost + ROUTE_BLOCKED_COST;
			case TileType::Object:
				return cost + (this->options.demolish && GetTileOwner(tile) == OWNER_NONE ? 30 : ROUTE_BLOCKED_COST);
			case TileType::TunnelBridge:
				if (this->options.demolish && GetTunnelBridgeTransportType(tile) == TransportType::Road) {
					Owner owner = GetRoadOwner(tile, RoadTramType::Road);
					if (owner == OWNER_TOWN || owner == OWNER_NONE) return cost + 20;
				}
				return cost + ROUTE_BLOCKED_COST;
			default:
				return cost + ROUTE_BLOCKED_COST;
		}
	}

	/** Build the exact footprint, without the unused lead-in of staggered diagonal starts. */
	std::map<TileIndex, TrackBits> MakeTrackTiles(const std::vector<RailRouteStep> &steps, std::vector<RailRoutePiece> &pieces) const
	{
		pieces = GetRailRoutePieces(steps);
		std::map<TileIndex, TrackBits> tiles;
		for (auto [k, skip] : this->tracks) {
			for (size_t pi = skip; pi < pieces.size(); pi++) {
				const auto &p = pieces[pi];
				TileIndex tile = this->MapTile(p.u, p.v + k);
				if (tile == INVALID_TILE) return {};
				auto [a, b] = _route_track_edges[to_underlying(p.track)];
				Track track = TrackFromEdges(this->frame.Edge(a), this->frame.Edge(b));
				/* Keep the end in the direction of the last run. At existing rail, use the piece
				 * which accepts our incoming edge instead of adding a straight stub over a bend. */
				if (pi + 1 == pieces.size()) {
					TrackBits existing = ExistingRouteTracks(tile, this->options.railtype);
					DiagDirection incoming = this->frame.Edge(DiagDirection::NE);
					bool matched = false;
					for (Track t : existing) {
						auto [e1, e2] = _route_track_edges[to_underlying(t)];
						if (e1 == incoming || e2 == incoming) { track = t; matched = true; break; }
					}
					if (!matched && existing.Any() && IsPlainRailTile(tile)) {
						/* A perpendicular straight needs a turnout, not an unconnected crossing. */
						Track t = *existing.begin();
						auto [e1, e2] = _route_track_edges[to_underlying(t)];
						DiagDirection outward = e1 == this->frame.Edge(DiagDirection::SW) || e1 == this->frame.Edge(DiagDirection::SE) ? e1 : e2;
						track = TrackFromEdges(incoming, outward);
					}
				}
				tiles[tile].Set(track);
			}
		}
		return tiles;
	}

	/** Can the entire route use the original terrain and normal rail foundations? */
	bool KeepTerrain(const std::map<TileIndex, TrackBits> &tiles) const
	{
		for (const auto &[tile, bits] : tiles) {
			if (IsTileType(tile, TileType::Water)) return false;
			if (this->options.raise_sea_level && TileHeight(tile) == 0) return false;
			TrackBits combined = bits | ExistingRouteTracks(tile, this->options.railtype);
			Foundation f = GetRailFoundation(GetTileSlope(tile), combined);
			if (f == Foundation::Invalid) return false;
			if (f != Foundation::None && !_settings_game.construction.build_on_slopes) return false;
			TrackBits existing = ExistingRouteTracks(tile, this->options.railtype);
			for (Track old : existing) {
				for (DiagDirection edge : {_route_track_edges[to_underlying(old)].first, _route_track_edges[to_underlying(old)].second}) {
					if (RouteEdgeHeight(tile, combined, edge) != RouteEdgeHeight(tile, existing, edge)) return false;
				}
			}
			/* Both pieces on a shared diagonal tile need one compatible foundation. Each
			 * internal connection must meet at the same height, including on steep land. */
			for (Track t : bits) {
				for (DiagDirection edge : {_route_track_edges[to_underlying(t)].first, _route_track_edges[to_underlying(t)].second}) {
					TileIndex next = TileAddByDiagDir(tile, edge);
					if (next >= Map::Size()) return false;
					auto it = tiles.find(next);
					TrackBits other = it == tiles.end() ? ExistingRouteTracks(next, this->options.railtype) : it->second | ExistingRouteTracks(next, this->options.railtype);
					for (Track nt : other) {
						auto [a, b] = _route_track_edges[to_underlying(nt)];
						if (a != ReverseDiagDir(edge) && b != ReverseDiagDir(edge)) continue;
						if (RouteEdgeHeight(tile, combined, edge) != RouteEdgeHeight(next, other, ReverseDiagDir(edge))) return false;
					}
				}
			}
		}
		return true;
	}

	/** Actual corners touched by the track, indexed by canonical column. */
	std::vector<std::set<int>> CornerRows(const std::vector<RailRoutePiece> &pieces) const
	{
		std::vector<std::set<int>> rows(this->length + 2);
		for (auto [k, skip] : this->tracks) {
			for (size_t pi = skip; pi < pieces.size(); pi++) {
				const auto &p = pieces[pi];
				for (int c : {p.u, p.u + 1}) {
					rows[c].insert(p.v + k);
					rows[c].insert(p.v + k + 1);
				}
			}
		}
		return rows;
	}

	/**
	 * Choose each corner column separately. Height changes are allowed only under straight
	 * rail, with four tiles between ramps. Costs use the actual columns (including both
	 * ends), rather than the average height of a sixteen-tile block. Existing infrastructure
	 * pins its corners, so a new route cannot excavate its connections.
	 */
	int64_t PlanHeights(const std::vector<RailRouteStep> &steps, const std::vector<RailRoutePiece> &pieces,
			const std::map<TileIndex, TrackBits> &tiles, std::vector<int> &heights) const
	{
		const auto rows = this->CornerRows(pieces);
		int lo = INT_MAX, hi = 0;
		std::map<TileIndex, int> pinned;
		for (const auto &[tile, bits] : tiles) {
			if (ExistingRouteTracks(tile, this->options.railtype).None()) continue;
			for (uint dx : {0, 1}) for (uint dy : {0, 1}) {
				TileIndex corner = TileXY(TileX(tile) + dx, TileY(tile) + dy);
				pinned[corner] = TileHeight(corner);
			}
		}
		for (int c = 0; c <= this->length + 1; c++) {
			for (int v : rows[c]) {
				int h = this->CornerHeight(c, v);
				if (h < 0) return INT64_MAX;
				lo = std::min(lo, h);
				hi = std::max(hi, h);
			}
		}
		lo = std::max(lo, this->options.raise_sea_level ? 1 : 0);
		hi = std::max(lo, hi);
		if (hi > _settings_game.construction.map_height_limit) return INT64_MAX;
		const int range = hi - lo + 1;
		const int states = range * ROUTE_RAMP_SPACING;
		constexpr int64_t INF = INT64_MAX / 4;
		std::vector<int64_t> cur(states, INF), next(states, INF);
		std::vector<std::vector<int16_t>> back(this->length + 2, std::vector<int16_t>(states, -1));
		for (int h = lo; h <= hi; h++) cur[(h - lo) * ROUTE_RAMP_SPACING] = 0;
		for (int c = 0; c <= this->length + 1; c++) {
			std::vector<int64_t> cost(range, 0);
			for (int v : rows[c]) {
				auto [x, y] = this->frame.Corner(c, v);
				TileIndex corner = TileXY(x, y);
				int old = TileHeight(corner);
				/* Protect infrastructure beside the route as well as underneath it. */
				bool protect = pinned.contains(corner);
				for (int dx : {-1, 0}) for (int dy : {-1, 0}) {
					TileIndex neighbour = TileXY(x + dx, y + dy);
					if (ExistingRouteTracks(neighbour, this->options.railtype).Any() || this->ObstacleCost(neighbour) >= ROUTE_BLOCKED_COST) protect = true;
				}
				for (int h = lo; h <= hi; h++) {
					int d = std::abs(old - h);
					cost[h - lo] = protect && d != 0 ? INF : std::min(INF, cost[h - lo] + 8 * d + 2 * d * d);
				}
			}
			/* Score column c, then advance across tile column c. A bend and the end
			 * tile stay level; unused lead-in columns are left unconstrained. */
			std::fill(next.begin(), next.end(), INF);
			for (int state = 0; state < states; state++) {
				int h = state / ROUTE_RAMP_SPACING + lo;
				int cooldown = state % ROUTE_RAMP_SPACING;
				int64_t base = cur[state] + cost[h - lo];
				if (base >= INF) continue;
				bool ramp = c < this->length && !steps[c].diagonal;
				for (int dh : {0, -1, 1}) {
					if (dh != 0 && (!ramp || cooldown != 0)) continue;
					int nh = h + dh;
					if (nh < lo || nh > hi) continue;
					int ns = (nh - lo) * ROUTE_RAMP_SPACING + (dh == 0 ? std::max(0, cooldown - 1) : ROUTE_RAMP_SPACING - 1);
					int64_t value = base + (dh == 0 ? 0 : 2);
					if (value < next[ns]) { next[ns] = value; back[c][ns] = state; }
				}
			}
			cur.swap(next);
		}
		auto best = std::min_element(cur.begin(), cur.end());
		if (*best >= INF) return INT64_MAX;
		heights.resize(this->length + 2);
		int state = best - cur.begin();
		for (int c = this->length + 1; c >= 0; c--) {
			state = back[c][state];
			heights[c] = lo + state / ROUTE_RAMP_SPACING;
		}
		return *best;
	}

	/** Try a bounded set of routes with up to two bends, favouring terrain that needs no changes. */
	bool PlanSteps(std::vector<RailRouteStep> &steps)
	{
		int64_t best = INT64_MAX;
		uint tried = 0, bounds = 0, obstacles_rejected = 0, terrain_rejected = 0;
		TileIndex first_obstacle = INVALID_TILE;
		std::set<std::vector<bool>> shapes;
		const bool straight_start = this->options.start == RailRouteStart::X || this->options.start == RailRouteStart::Y;
		const bool diagonal_start = this->options.start == RailRouteStart::NS || this->options.start == RailRouteStart::EW;
		for (bool diagonal_first : {false, true}) {
			if ((straight_start && diagonal_first) || (diagonal_start && !diagonal_first)) continue;
			int outside = diagonal_first ? this->minor : this->length - this->minor;
			int inside = this->length - outside;
			for (int split : {outside, 0, outside / 4, outside / 2, outside * 3 / 4}) {
				if (this->options.start != RailRouteStart::Auto && split == 0) continue;
				int required = 1;
				for (auto [k, skip] : this->tracks) required = std::max(required, diagonal_first ? skip / 2 + 1 : skip + 1);
				if (split < required && this->options.start != RailRouteStart::Auto) continue;
				std::vector<bool> shape(this->length, diagonal_first);
				std::fill(shape.begin() + split, shape.begin() + split + inside, !diagonal_first);
				shapes.insert(std::move(shape));
			}
		}
		for (int pass = 0; pass < 2; pass++) {
			for (const auto &shape : shapes) {
				tried++;
				std::vector<RailRouteStep> candidate(this->length);
				int bends = 0;
				for (int c = 0; c < this->length; c++) {
					candidate[c].diagonal = shape[c];
					if (c > 0 && shape[c] != shape[c - 1]) bends++;
				}
				std::vector<RailRoutePiece> candidate_pieces;
				auto tiles = this->MakeTrackTiles(candidate, candidate_pieces);
				if (tiles.empty()) { bounds++; continue; }
				int64_t obstacles = 0;
				bool blocked = false;
				for (const auto &[tile, bits] : tiles) {
					int64_t cost = this->ObstacleCost(tile);
					if (cost >= ROUTE_BLOCKED_COST) {
						if (first_obstacle == INVALID_TILE) first_obstacle = tile;
						blocked = true;
						break;
					}
					TrackBits existing = ExistingRouteTracks(tile, this->options.railtype);
					if (existing.Any() && bits != (bits & existing)) {
						if (IsRailStationTile(tile) || (HasSignals(tile) && TracksOverlap(bits | existing))) {
							if (first_obstacle == INVALID_TILE) first_obstacle = tile;
							blocked = true;
							break;
						}
					}
					obstacles += cost;
				}
				if (blocked) { obstacles_rejected++; continue; }
				std::vector<int> heights;
				int64_t cost = this->KeepTerrain(tiles) ? 0 : this->options.terraform ? this->PlanHeights(candidate, candidate_pieces, tiles, heights) : INT64_MAX;
				if (cost == INT64_MAX) { terrain_rejected++; continue; }
				cost += obstacles * 8 + bends;
				if (cost >= best) continue;
				best = cost;
				steps = std::move(candidate);
				this->pieces = std::move(candidate_pieces);
				this->track_tiles = std::move(tiles);
				this->column_height = std::move(heights);
				/* With no obstacles or earthworks and the minimum number of bends, no later
				 * candidate can improve this route. Keep previews of long flat lines cheap. */
				if (best == (this->minor == 0 || this->minor == this->length ? 0 : 1)) break;
			}
			if (best != INT64_MAX) break;
			/* A few sampled bends can all hit obstacles while a bend between them is clear.
			 * Retry only a failed plan. Bound the work for long, wide corridors so preview
			 * failures do not turn into an exhaustive search over the map. */
			if (pass != 0) break;
			shapes.clear();
			int samples = std::clamp(1'000'000 / std::max(1, this->length * static_cast<int>(this->tracks.size())), 8, 128);
			for (bool diagonal_first : {false, true}) {
				if ((straight_start && diagonal_first) || (diagonal_start && !diagonal_first)) continue;
				int outside = diagonal_first ? this->minor : this->length - this->minor;
				int inside = this->length - outside;
				int required = this->options.start == RailRouteStart::Auto ? 0 : 1;
				if (this->options.start != RailRouteStart::Auto) {
					for (auto [k, skip] : this->tracks) required = std::max(required, diagonal_first ? skip / 2 + 1 : skip + 1);
				}
				if (required > outside) continue;
				int count = std::min(samples, outside - required + 1);
				for (int i = 0; i < count; i++) {
					int split = count == 1 ? required : required + static_cast<int>(static_cast<int64_t>(outside - required) * i / (count - 1));
					std::vector<bool> shape(this->length, diagonal_first);
					std::fill(shape.begin() + split, shape.begin() + split + inside, !diagonal_first);
					shapes.insert(std::move(shape));
				}
			}
		}
		if (best == INT64_MAX) this->failure_detail = fmt::format("candidates={} bounds={} protected={} terrain={}", tried, bounds, obstacles_rejected, terrain_rejected);
		if (best == INT64_MAX && first_obstacle != INVALID_TILE && first_obstacle < Map::Size()) {
			format_append(this->failure_detail, " first_protected={},{} tiletype={} owner={}", TileX(first_obstacle), TileY(first_obstacle),
					to_underlying(GetTileType(first_obstacle)), GetTileOwner(first_obstacle).base());
		}
		return best != INT64_MAX;
	}

	/** Plan clearing, necessary earthworks, new pieces and new signals, in that order. */
	void Plan(RailRoutePlan &plan)
	{
		std::vector<RailRouteStep> steps;
		if (!this->PlanSteps(steps)) {
			plan.error = "no route found";
			plan.diagnostics = this->failure_detail;
			return;
		}
		plan.length = this->length;
		for (size_t s = 1; s < steps.size(); s++) if (steps[s].diagonal != steps[s - 1].diagonal) plan.bends++;
		plan.tracks.assign(this->track_tiles.begin(), this->track_tiles.end());
		int minx = INT_MAX, miny = INT_MAX;
		for (const auto &[tile, bits] : this->track_tiles) {
			if (TileX(tile) < 1 || TileY(tile) < 1 || TileX(tile) + 1 >= Map::MaxX() || TileY(tile) + 1 >= Map::MaxY()) {
				plan.error = "the corridor does not fit on the map";
				return;
			}
			minx = std::min<int>(minx, TileX(tile));
			miny = std::min<int>(miny, TileY(tile));
		}
		plan.origin = TileXY(minx, miny);
		plan.min_height = INT_MAX;
		std::set<IndustryID> industries;
		ChunkWriter prepare(plan.prepare), build(plan.build);
		const uint8_t clear_flags = (this->options.remove_water ? 1 << to_underlying(RailRouteClearFlag::Water) : 0) |
				(this->options.demolish ? 1 << to_underlying(RailRouteClearFlag::Demolish) : 0);
		auto emit_prepare = [&](RailRouteItemKind kind, TileIndex tile, int height) {
			auto &item = prepare.Begin();
			item.push_back(to_underlying(kind));
			auto u16 = [&](uint val) { item.push_back(GB(val, 0, 8)); item.push_back(GB(val, 8, 8)); };
			u16(TileX(tile) - minx); u16(TileY(tile) - miny); u16(1); u16(1);
			item.push_back(height); item.push_back(clear_flags);
			prepare.Commit();
		};
		/* Clear only obstacles on actual track tiles. Terraforming clears a neighbour only
		 * when a dry run identifies that specific neighbour as blocking the operation. */
		for (const auto &[tile, bits] : this->track_tiles) {
			int h = GetTileZ(tile);
			plan.min_height = std::min(plan.min_height, h);
			plan.max_height = std::max<int>(plan.max_height, GetTileMaxZ(tile));
			if (this->ObstacleCost(tile) == 0) continue;
			emit_prepare(RailRouteItemKind::Clear, tile, 0);
			switch (GetTileType(tile)) {
				case TileType::Water: plan.estimate.water++; plan.estimate.clearing += _price[Price::ClearWater]; break;
				case TileType::House: plan.estimate.houses++; plan.estimate.clearing += HouseSpec::Get(GetHouseType(tile))->GetRemovalCost(); break;
				case TileType::Industry:
					if (industries.insert(GetIndustryIndex(tile)).second) plan.estimate.clearing += GetIndustrySpec(Industry::GetByTile(tile)->type)->GetRemovalCost();
					break;
				default: plan.estimate.clearing += _price[Price::ClearRoad]; break;
			}
		}
		plan.estimate.industries = industries.size();
		/* Lower first, then raise. Emit each changed corner once; no eager margin clearing
		 * or repeated levelling of already correct columns. */
		if (!this->column_height.empty()) {
			auto rows = this->CornerRows(this->pieces);
			plan.min_height = *std::min_element(this->column_height.begin(), this->column_height.end());
			plan.max_height = *std::max_element(this->column_height.begin(), this->column_height.end());
			for (bool raising : {false, true}) {
				for (int c = 0; c <= this->length + 1; c++) {
					for (int v : rows[c]) {
						auto [x, y] = this->frame.Corner(c, v);
						TileIndex tile = TileXY(x, y);
						int old = TileHeight(tile), h = this->column_height[c];
						if (old == h || (h > old) != raising) continue;
						emit_prepare(RailRouteItemKind::Level, tile, h);
						int d = std::abs(h - old);
						plan.estimate.terraform += _price[Price::Terraform] * d * (d + 1);
					}
				}
			}
		}
		for (const auto &[tile, bits] : this->track_tiles) {
			TrackBits missing = TrackBits{bits}.Reset(ExistingRouteTracks(tile, this->options.railtype));
			if (missing.None()) continue;
			auto &item = build.Begin();
			item.push_back(to_underlying(BlueprintItemKind::Track));
			auto u16 = [&](uint val) { item.push_back(GB(val, 0, 8)); item.push_back(GB(val, 8, 8)); };
			u16(TileX(tile) - minx); u16(TileY(tile) - miny);
			item.push_back(this->options.railtype); item.push_back(missing.base());
			build.Commit();
			for ([[maybe_unused]] Track t : missing) plan.estimate.track += RailBuildCost(this->options.railtype);
			if (IsTileType(tile, TileType::Clear)) {
				/* Normal rail construction also pays for the grass, rocks, fields or snow. */
				static constexpr std::array<Price, 6> prices = {Price::ClearGrass, Price::ClearRough, Price::ClearRocks,
					Price::ClearFields, Price::ClearRough, Price::ClearRough};
				ClearGround ground = GetClearGround(tile);
				if (IsSnowTile(tile) || ground != ClearGround::Grass || GetClearDensity(tile) != 0) plan.estimate.clearing += _price[prices[to_underlying(ground)]];
				if (IsSnowTile(tile)) plan.estimate.clearing += std::abs(_price[Price::ClearRough] - _price[Price::ClearGrass]);
			} else if (IsTileType(tile, TileType::Trees)) {
				uint count = GetTreeCount(tile);
				if (IsInsideMM(GetTreeType(tile), TREE_RAINFOREST, TREE_CACTUS)) count *= 4;
				plan.estimate.clearing += _price[Price::ClearTrees] * count;
			}
			if (GetRailFoundation(GetTileSlope(tile), bits) != Foundation::None) plan.estimate.track += _price[Price::BuildFoundation];
		}
		if (!this->options.signals) return;
		for (auto [k, skip] : this->tracks) {
			for (size_t pi = skip; pi < this->pieces.size(); pi++) {
				const auto &p = this->pieces[pi];
				int distance = p.step - this->pieces[skip].step;
				if (pi != static_cast<size_t>(skip) && (p.track == Track::Left || distance <= 0 || distance % std::max<int>(this->options.signal_interval, 1) != 0)) continue;
				TileIndex tile = this->MapTile(p.u, p.v + k);
				/* Existing signals and junctions belong to the player: never reverse them. */
				if (ExistingRouteTracks(tile, this->options.railtype).Any() || TracksOverlap(this->track_tiles.at(tile))) continue;
				auto [a, b] = _route_track_edges[to_underlying(p.track)];
				Track track = TrackFromEdges(this->frame.Edge(a), this->frame.Edge(b));
				if (!this->track_tiles.at(tile).Test(track)) continue;
				Trackdir td = TrackExitdirToTrackdir(track, this->frame.Edge(TrackdirToExitdir(p.trackdir)));
				auto &item = build.Begin();
				item.push_back(to_underlying(BlueprintItemKind::Signal));
				auto u16 = [&](uint val) { item.push_back(GB(val, 0, 8)); item.push_back(GB(val, 8, 8)); };
				u16(TileX(tile) - minx); u16(TileY(tile) - miny);
				item.push_back(to_underlying(track)); item.push_back(to_underlying(this->options.signal_type));
				item.push_back(to_underlying(this->options.signal_variant)); item.push_back(SignalAlongTrackdir(td));
				build.Commit();
				plan.estimate.track += _price[Price::BuildSignals];
			}
		}
	}

private:
	/** Writer of items into chunks of at most #CHUNK_LIMIT bytes. */
	class ChunkWriter {
		std::vector<BlueprintChunkData> &chunks;
		std::vector<uint8_t> item{};

	public:
		explicit ChunkWriter(std::vector<BlueprintChunkData> &chunks) : chunks(chunks) {}

		std::vector<uint8_t> &Begin()
		{
			this->item.clear();
			return this->item;
		}

		void Commit()
		{
			if (this->chunks.empty() || this->chunks.back().data.size() + this->item.size() > CHUNK_LIMIT) this->chunks.emplace_back();
			auto &data = this->chunks.back().data;
			data.insert(data.end(), this->item.begin(), this->item.end());
		}
	};
};

/**
 * Get a track piece near the start of a track of a route in a frame, on the map.
 * @param frame The frame.
 * @param k Offset of the track.
 * @param diagonal The route starts with diagonal steps (otherwise with straight ones).
 * @param index Index of the piece along the track, within the first run.
 * @return The tile (INVALID_TILE when off the map) and the track.
 */
static std::pair<TileIndex, Track> GetStartPiece(const RouteFrame &frame, int k, bool diagonal, int index)
{
	/* Canonical pieces: straight step s is X at (s, k); diagonal step s is Right at (s, s + k), then Left at (s, s + k + 1). */
	int step = diagonal ? index / 2 : index;
	int u = step;
	int v = k + (diagonal ? step + (index % 2) : 0);
	Track canonical = diagonal ? (index % 2 == 0 ? Track::Right : Track::Left) : Track::X;
	auto [x, y] = frame.Tile(u, v);
	auto [e1, e2] = _route_track_edges[to_underlying(canonical)];
	Track track = TrackFromEdges(frame.Edge(e1), frame.Edge(e2));
	if (x < 0 || y < 0 || x >= static_cast<int>(Map::SizeX()) || y >= static_cast<int>(Map::SizeY())) return {INVALID_TILE, track};
	return {TileXY(x, y), track};
}

/**
 * Get the reference frame of a start orientation: the start pieces are the first pieces of its tracks.
 * @param from The start tile.
 * @param start The orientation.
 * @return The frame.
 */
static RouteFrame GetStartReferenceFrame(TileIndex from, RailRouteStart start)
{
	int ax = TileX(from), ay = TileY(from);
	switch (start) {
		case RailRouteStart::Y: return {ax, ay, 1, 1, true};
		case RailRouteStart::EW: return {ax, ay, 1, -1, false};
		default: return {ax, ay, 1, 1, false};
	}
}

/** Detect an unambiguous existing start, leaving an empty tile or a junction to the selected orientation. */
RailRouteStart GetRailRouteStartOrientation(TileIndex tile, RailType railtype)
{
	if (tile >= Map::Size()) return RailRouteStart::Auto;
	TrackBits bits = ExistingRouteTracks(tile, railtype);
	if (bits == Track::X) return RailRouteStart::X;
	if (bits == Track::Y) return RailRouteStart::Y;
	if (bits.Any() && TrackBits{bits}.Reset(TRACK_BIT_VERT).None()) return RailRouteStart::NS;
	if (bits.Any() && TrackBits{bits}.Reset(TRACK_BIT_HORZ).None()) return RailRouteStart::EW;
	return RailRouteStart::Auto;
}

/**
 * Get the fixed start of a trunk line: the first piece of every track. It only depends on the start tile,
 * the orientation and the number of tracks, never on the end.
 * @param from The start tile: the first tile of the centre track.
 * @param start The orientation (not Auto).
 * @param tracks Number of tracks.
 * @return Tile and track piece of every track, from one side of the corridor to the other.
 */
static std::vector<std::pair<TileIndex, Track>> GetStartTemplate(TileIndex from, RailRouteStart start, uint8_t tracks)
{
	RouteFrame ref = GetStartReferenceFrame(from, start);
	bool diagonal = start == RailRouteStart::NS || start == RailRouteStart::EW;
	std::vector<std::pair<TileIndex, Track>> result;
	int lo = -static_cast<int>((tracks - 1) / 2);
	int phase = 0;
	if (diagonal && IsPlainRailTile(from) && GetTileOwner(from) == _current_company) {
		TrackBits bits = GetTrackBits(from);
		auto [tile, track] = GetStartPiece(ref, 0, true, 0);
		if (!bits.Test(track)) {
			RouteFrame shifted = ref;
			shifted.ay -= ref.sy;
			auto [other_tile, other_track] = GetStartPiece(shifted, 0, true, 1);
			if (bits.Test(other_track)) { ref = shifted; phase = 1; }
		}
	}
	for (int k = lo; k < lo + tracks; k++) result.push_back(GetStartPiece(ref, k, diagonal, phase));
	return result;
}

/**
 * Get the track pieces at the start of a trunk line with a fixed start orientation, for the preview before the start is placed.
 * @param from The start tile.
 * @param start The orientation; Auto gives nothing.
 * @param tracks Number of tracks.
 * @return Tiles with their track pieces.
 */
std::vector<std::pair<TileIndex, TrackBits>> GetRailRouteStartPieces(TileIndex from, RailRouteStart start, uint8_t tracks)
{
	std::vector<std::pair<TileIndex, TrackBits>> result;
	if (start == RailRouteStart::Auto || start >= RailRouteStart::End || from >= Map::Size() || tracks < 1 || tracks > RailRouteOptions::MAX_TRACKS) return result;
	std::map<TileIndex, TrackBits> tiles;
	for (auto [tile, track] : GetStartTemplate(from, start, tracks)) {
		if (tile != INVALID_TILE) tiles[tile].Set(track);
	}
	result.assign(tiles.begin(), tiles.end());
	return result;
}

/**
 * Plan a trunk line from one tile to another.
 * With a fixed start orientation the route begins with a run in that orientation, the tracks at the start tile are
 * always the same, and an end out of reach is moved: a straight start reaches at most 45 degrees sideways, a diagonal
 * start the quarter between its diagonal and the axes.
 * @param from The start tile: the first tile of the centre track.
 * @param to The end tile: the last tile of the centre track.
 * @param options The options.
 * @return The plan; see RailRoutePlan::error.
 */
RailRoutePlan PlanRailRoute(TileIndex from, TileIndex to, const RailRouteOptions &requested_options)
{
	RailRouteOptions options = requested_options;
	RailRoutePlan plan;
	if (from >= Map::Size() || to >= Map::Size()) {
		plan.error = "invalid tile";
		return plan;
	}
	if (options.tracks < 1 || options.tracks > RailRouteOptions::MAX_TRACKS) {
		plan.error = "invalid number of tracks";
		return plan;
	}

	if (options.start == RailRouteStart::Auto) options.start = GetRailRouteStartOrientation(from, options.railtype);
	if (options.railtype >= RAILTYPE_END || GetRailTypeInfo(options.railtype)->label.Empty()) {
		plan.error = "invalid rail type";
		return plan;
	}

	int dx = static_cast<int>(TileX(to)) - static_cast<int>(TileX(from));
	int dy = static_cast<int>(TileY(to)) - static_cast<int>(TileY(from));
	bool swap = std::abs(dy) > std::abs(dx);

	/* A fixed start: keep the end in reach of the start orientation. */
	switch (options.start) {
		case RailRouteStart::Auto: break;
		case RailRouteStart::X:
		case RailRouteStart::Y: {
			int &major = options.start == RailRouteStart::X ? dx : dy;
			int &side = options.start == RailRouteStart::X ? dy : dx;
			if (major == 0) {
				plan.error = "the end must lie in the direction of the start";
				return plan;
			}
			/* At least one straight step: the start tile holds straight track. */
			int reach = std::abs(major) - 1;
			if (std::abs(side) > reach) side = side < 0 ? -reach : reach;
			swap = options.start == RailRouteStart::Y;
			break;
		}
		case RailRouteStart::NS:
		case RailRouteStart::EW: {
			bool same_sign = options.start == RailRouteStart::NS;
			if (dx == 0 || dy == 0 || ((dx > 0) == (dy > 0)) != same_sign) {
				plan.error = "the end must lie in the direction of the start";
				return plan;
			}
			break;
		}
		default:
			plan.error = "invalid start orientation";
			return plan;
	}
	plan.end = TileXY(TileX(from) + dx, TileY(from) + dy);

	RouteFrame frame{static_cast<int>(TileX(from)), static_cast<int>(TileY(from)), dx < 0 ? -1 : 1, dy < 0 ? -1 : 1, swap};
	int length = std::max(std::abs(dx), std::abs(dy));
	int minor = std::min(std::abs(dx), std::abs(dy));
	if (length < 1) {
		plan.error = "the route is too short";
		return plan;
	}

	if (options.start == RailRouteStart::Auto) {
		RoutePlanner planner(options, frame, length, minor);
		planner.Plan(plan);
		return plan;
	}

	/* Find, for every piece of the fixed start, the track of this route that runs through it. In a frame turned
	 * against the reference frame the tracks cross the start pieces at different steps: they start staggered.
	 * When some of them would cross their start piece before the start tile, the route is planned from a few
	 * diagonal steps further back; the pieces before the start pieces are never built. */
	bool diagonal = options.start == RailRouteStart::NS || options.start == RailRouteStart::EW;
	int k_range = 2 * static_cast<int>(options.tracks) + 6;
	auto templ = GetStartTemplate(from, options.start, options.tracks);
	for (int back = 0; back <= (diagonal ? static_cast<int>(options.tracks) + 3 : 0); back++) {
		RouteFrame f = frame;
		f.ax -= back * frame.sx;
		f.ay -= back * frame.sy;
		if (f.ax < 1 || f.ay < 1 || f.ax >= static_cast<int>(Map::MaxX()) || f.ay >= static_cast<int>(Map::MaxY())) break;
		int len = length + back;
		int mnr = minor + back;
		/* Only pieces of the first run (diagonal for a diagonal start, straight for a straight one) can be start pieces. */
		int first_run = diagonal ? mnr : len - mnr;
		int search_limit = 2 * static_cast<int>(options.tracks) + 2 * back + 6;
		/* First identify the fixed starts on an unbounded diagonal run. A short requested
		 * end may otherwise hide outer start pieces before reach can be adjusted. */
		int max_index = diagonal ? search_limit : std::min(first_run, search_limit);
		std::vector<std::pair<int, int>> tracks;
		for (auto [tile, track] : templ) {
			bool found = false;
			for (int k = -k_range; k <= k_range && !found; k++) {
				for (int index = 0; index < max_index && !found; index++) {
					if (GetStartPiece(f, k, diagonal, index) == std::make_pair(tile, track)) {
						tracks.emplace_back(k, index);
						found = true;
					}
				}
			}
			if (!found) break;
		}
		if (tracks.size() != templ.size()) continue;

		if (diagonal) {
			int required = 1;
			for (auto [offset, skip] : tracks) required = std::max(required, skip / 2 + 1);
			/* All staggered outer starts must fit before the first bend. Project an end
			 * that is too close to the start fan into reach, as for a straight start. */
			mnr = std::max(mnr, required);
			len = std::max(len, mnr);
			auto [end_x, end_y] = f.Tile(len, mnr);
			if (end_x < 1 || end_y < 1 || end_x + 1 >= static_cast<int>(Map::MaxX()) || end_y + 1 >= static_cast<int>(Map::MaxY())) {
				plan.error = "the corridor does not fit on the map";
				return plan;
			}
			plan.end = TileXY(end_x, end_y);
		}

		RoutePlanner planner(options, f, len, mnr);
		planner.SetTracks(std::move(tracks));
		planner.Plan(plan);
		return plan;
	}
	plan.error = "the route does not fit the start";
	return plan;
}

/**
 * Persist an explicitly requested failed route, without logging mouse-hover previews.
 * Records only route coordinates, options and rejection counts; the file is bounded to 64 KiB.
 */
void LogRailRouteFailure(TileIndex from, TileIndex to, const RailRouteOptions &options, const RailRoutePlan &plan)
{
	if (plan.error.empty() || from >= Map::Size() || to >= Map::Size()) return;
	bool truncate = false;
	if (auto previous = FioFOpenFile("rail-route.log", "rb", Subdirectory::Save); previous.has_value()) {
		if (fseek(*previous, 0, SEEK_END) == 0) truncate = ftell(*previous) > 63 * 1024;
	}
	if (auto log = FioFOpenFile("rail-route.log", truncate ? "wb" : "ab", Subdirectory::Save); log.has_value()) {
		fmt::print(*log, "{}from={},{} to={},{} company={} tracks={} start={} railtype={} terraform={} water={} demolish={} sea={} error={} {}\n",
				GetLogPrefix(true), TileX(from), TileY(from), TileX(to), TileY(to), _current_company.base(), options.tracks,
				to_underlying(options.start), to_underlying(options.railtype), options.terraform, options.remove_water, options.demolish,
				options.raise_sea_level, plan.error, plan.diagnostics);
	}
}
