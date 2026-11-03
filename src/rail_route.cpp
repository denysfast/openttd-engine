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
 * Shape: the route is as straight as possible - one straight run and one diagonal run (at most one bend), in
 * the order that needs less earthworks. Demolition and filling are not avoided.
 *
 * Height: diagonal track is always level, so heights change only on straight steps, by at most one level per
 * #ROUTE_RAMP_SPACING steps. A dynamic programme over blocks of steps chooses the height at every block border,
 * minimising the earthworks of the corridor plus a cost for what has to be demolished.
 */

#include "stdafx.h"
#include <map>
#include <set>
#include "rail_route.h"
#include "bridge_map.h"
#include "tunnelbridge_map.h"
#include "house.h"
#include "town_map.h"
#include "economy_func.h"
#include "industry.h"
#include "industry_map.h"
#include "map_func.h"
#include "rail.h"
#include "road_map.h"
#include "settings_type.h"
#include "signal_func.h"
#include "tile_map.h"
#include "track_func.h"
#include "water_map.h"

#include "safeguards.h"

/** Steps of a block of the height programme, at least; long routes use longer blocks (at most #ROUTE_MAX_BLOCKS blocks). */
static constexpr uint ROUTE_MIN_BLOCK = 16;
/** Maximum number of blocks of the height programme. */
static constexpr uint ROUTE_MAX_BLOCKS = 256;
/** A ramp tile is followed by at least this many steps before the next one (gentle gradients). */
static constexpr uint ROUTE_RAMP_SPACING = 4;
/** Cost of a tile that cannot be cleared, in units of earthworks (one corner one level). */
static constexpr int64_t ROUTE_BLOCKED_COST = 2000;
/** Width of the cleared strip on both sides of the corridor, so levelling has room; two, because a corner on the stepped edge of a diagonal run touches the tile two rows out. */
static constexpr int ROUTE_CLEAR_MARGIN = 2;
/** Widest cleared strip: levelling a corner by n levels reshapes the land up to n - 1 tiles beside it. */
static constexpr int ROUTE_MAX_CLEAR_MARGIN = 16;

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
 * a Right piece and a Left piece one tile further in v. The end tile gets a final X piece.
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
	pieces.push_back({u, v, Track::X, TrackExitdirToTrackdir(Track::X, DiagDirection::SW), static_cast<int>(steps.size())});
	return pieces;
}

/** Context of planning one route. */
class RoutePlanner {
	const RailRouteOptions &options;
	RouteFrame frame;
	int length; ///< L: number of steps.
	int minor; ///< M: number of diagonal steps.
	int k0; ///< Offset of the first track from the centre line.
	int k1; ///< Offset of the last track.
	int hlo = 0; ///< Lowest height the corridor may get.
	int hhi = 0; ///< Highest height.
	int end_height = 0; ///< Height at the end of the last step.

public:
	RoutePlanner(const RailRouteOptions &options, RouteFrame frame, int length, int minor) :
		options(options), frame(frame), length(length), minor(minor)
	{
		this->k0 = -static_cast<int>((options.tracks - 1) / 2);
		this->k1 = this->k0 + options.tracks - 1;
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
		if (x < 0 || y < 0 || x >= static_cast<int>(Map::MaxX()) || y >= static_cast<int>(Map::MaxY())) return INVALID_TILE;
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

	/** Earthworks and obstacles of one block for every corridor height. */
	struct BlockCost {
		std::vector<int64_t> by_height; ///< Cost per height hlo..hhi.
	};

	/**
	 * Compute the cost of a block for every height.
	 * @param u0 First step of the block.
	 * @param len Number of steps.
	 * @param v0 Number of diagonal steps before the block.
	 * @param diagonal Type of every step of the route.
	 * @return The costs.
	 */
	BlockCost ComputeBlockCost(int u0, int len, int v0, const std::vector<bool> &diagonal) const
	{
		std::array<int64_t, 256> count{};
		int64_t obstacles = 0;
		int v = v0;
		for (int s = 0; s < len; s++) {
			int u = u0 + s;
			int rows_hi = this->k1 + (diagonal[u] ? 1 : 0);
			for (int k = this->k0; k <= rows_hi; k++) obstacles += this->ObstacleCost(this->MapTile(u, v + k));
			for (int k = this->k0; k <= rows_hi + 1; k++) {
				int h = this->CornerHeight(u, v + k);
				if (h >= 0) count[h]++;
			}
			if (diagonal[u]) v++;
		}

		BlockCost bc;
		bc.by_height.resize(this->hhi - this->hlo + 1);
		/* cost(h) = sum count[l] * |l - h|, with running sums over the levels. */
		int64_t below_n = 0, below_sum = 0, above_n = 0, above_sum = 0;
		for (int l = 0; l < 256; l++) {
			above_n += count[l];
			above_sum += count[l] * l;
		}
		int next = 0;
		for (int h = this->hlo; h <= this->hhi; h++) {
			while (next <= h && next < 256) {
				below_n += count[next];
				below_sum += count[next] * next;
				above_n -= count[next];
				above_sum -= count[next] * next;
				next++;
			}
			bc.by_height[h - this->hlo] = obstacles + (below_n * h - below_sum) + (above_sum - above_n * h);
		}
		return bc;
	}

	/**
	 * Choose the heights of a route with known step types: diagonal steps keep the height, straight steps may
	 * ramp (at most one level per #ROUTE_RAMP_SPACING steps); the heights minimise earthworks and demolition.
	 * @param diagonal Type of every step.
	 * @param[out] steps The steps with their heights.
	 * @return The cost of the route, or INT64_MAX when there is none.
	 */
	int64_t PlanHeights(const std::vector<bool> &diagonal, std::vector<RailRouteStep> &steps)
	{
		const int range = this->hhi - this->hlo + 1;
		const int block = std::max<int>(ROUTE_MIN_BLOCK, CeilDiv(this->length, ROUTE_MAX_BLOCKS));
		const int blocks = CeilDiv(this->length, block);
		constexpr int64_t INF = INT64_MAX / 4;

		std::vector<int64_t> cur(range, 0), nxt(range);
		std::vector<std::vector<int16_t>> back(blocks, std::vector<int16_t>(range, -1));
		int v0 = 0;
		for (int j = 0; j < blocks; j++) {
			int u0 = j * block;
			int len = std::min(block, this->length - u0);
			int diag = 0;
			for (int s = 0; s < len; s++) diag += diagonal[u0 + s] ? 1 : 0;
			/* A block with diagonal steps keeps one height; a straight block may ramp. */
			int max_ramp = diag > 0 ? 0 : len / static_cast<int>(ROUTE_RAMP_SPACING);
			BlockCost bc = this->ComputeBlockCost(u0, len, v0, diagonal);
			std::fill(nxt.begin(), nxt.end(), INF);
			for (int h = this->hlo; h <= this->hhi; h++) {
				int64_t c = cur[h - this->hlo];
				if (c >= INF) continue;
				for (int nh = std::max(this->hlo, h - max_ramp); nh <= std::min(this->hhi, h + max_ramp); nh++) {
					int64_t total = c + bc.by_height[(h + nh) / 2 - this->hlo] + std::abs(nh - h);
					if (total < nxt[nh - this->hlo]) {
						nxt[nh - this->hlo] = total;
						back[j][nh - this->hlo] = static_cast<int16_t>(h);
					}
				}
			}
			std::swap(cur, nxt);
			v0 += diag;
		}

		int best_h = -1;
		int64_t best = INF;
		for (int h = this->hlo; h <= this->hhi; h++) {
			if (cur[h - this->hlo] < best) {
				best = cur[h - this->hlo];
				best_h = h;
			}
		}
		if (best_h < 0) return INT64_MAX;

		std::vector<int> border(blocks + 1);
		border[blocks] = best_h;
		for (int j = blocks - 1; j >= 0; j--) border[j] = back[j][border[j + 1] - this->hlo];

		steps.assign(this->length, {});
		for (int j = 0; j < blocks; j++) {
			int u0 = j * block;
			int len = std::min(block, this->length - u0);
			int dh = border[j + 1] - border[j];
			for (int s = 0; s < len; s++) {
				RailRouteStep &step = steps[u0 + s];
				step.diagonal = diagonal[u0 + s];
				/* Ramps spread evenly over the block (only straight blocks ramp). */
				step.height = border[j] + dh * s / len;
			}
		}
		this->end_height = best_h;
		return best;
	}

	/**
	 * Find the steps of the route. The route is as straight as possible: at most one bend, a straight run and
	 * a diagonal run in the order that needs less earthworks; demolition and filling do not matter.
	 * @param[out] steps The steps.
	 * @return False when no route was found.
	 */
	bool PlanSteps(std::vector<RailRouteStep> &steps)
	{
		/* Height range: the terrain under the corridor, at least one level above the sea when asked. */
		int tmin = INT_MAX, tmax = 0;
		for (int u = 0; u <= this->length + 1; u += std::max(1, this->length / 512)) {
			for (int v = this->k0; v <= this->minor + this->k1 + 2; v += std::max(1, (this->minor + this->options.tracks) / 64)) {
				int h = this->CornerHeight(u, v);
				if (h < 0) continue;
				tmin = std::min(tmin, h);
				tmax = std::max(tmax, h);
			}
		}
		if (tmin == INT_MAX) return false;
		int floor = this->options.raise_sea_level ? 1 : 0;
		this->hlo = std::max(tmin, floor);
		this->hhi = std::clamp(tmax, this->hlo, static_cast<int>(_settings_game.construction.map_height_limit));

		int64_t best = INT64_MAX;
		int best_end = 0;
		for (bool diagonal_first : {false, true}) {
			/* Without a straight or without a diagonal part both orders are the same route. */
			if (diagonal_first && (this->minor == 0 || this->minor == this->length)) break;
			std::vector<bool> diagonal(this->length);
			for (int u = 0; u < this->length; u++) diagonal[u] = diagonal_first ? u < this->minor : u >= this->length - this->minor;
			std::vector<RailRouteStep> candidate;
			int64_t cost = this->PlanHeights(diagonal, candidate);
			if (cost < best) {
				best = cost;
				steps = std::move(candidate);
				best_end = this->end_height;
			}
		}
		this->end_height = best_end;
		return best != INT64_MAX;
	}

	/**
	 * Plan the route.
	 * @param plan The plan to fill.
	 */
	void Plan(RailRoutePlan &plan)
	{
		std::vector<RailRouteStep> steps;
		if (!this->PlanSteps(steps)) {
			plan.error = "no route found";
			return;
		}
		plan.length = this->length;
		for (size_t s = 1; s < steps.size(); s++) {
			if (steps[s].diagonal != steps[s - 1].diagonal) plan.bends++;
		}
		/* Height of corner column c: the height at the start of step c; after the last step the end height. */
		std::vector<int> column_height(this->length + 2);
		for (int c = 0; c <= this->length + 1; c++) column_height[c] = c < this->length ? steps[c].height : this->end_height;
		plan.min_height = *std::min_element(column_height.begin(), column_height.end());
		plan.max_height = *std::max_element(column_height.begin(), column_height.end());

		/* Rows of tiles of every column: column c holds the tiles of step c (two rows more for a diagonal step). */
		std::vector<std::pair<int, int>> tile_rows(this->length + 1);
		int v = 0;
		for (int c = 0; c <= this->length; c++) {
			bool diagonal = c < this->length && steps[c].diagonal;
			tile_rows[c] = {v + this->k0, v + this->k1 + (diagonal ? 1 : 0)};
			if (diagonal) v++;
		}

		/* Cleared strip beside every column: wider where the corridor cuts deep or fills high. */
		std::vector<int> margin(this->length + 1);
		for (int c = 0; c <= this->length; c++) {
			int depth = 0;
			for (int cc : {c, c + 1}) {
				for (int k = tile_rows[c].first; k <= tile_rows[c].second + 1; k++) {
					int h = this->CornerHeight(cc, k);
					if (h >= 0) depth = std::max(depth, std::abs(h - column_height[cc]));
				}
			}
			margin[c] = std::min(ROUTE_CLEAR_MARGIN + std::max(depth - 1, 0), ROUTE_MAX_CLEAR_MARGIN);
		}

		/* Bounding box on the map (with the cleared strip) and the origin of all chunks. */
		int minx = INT_MAX, miny = INT_MAX, maxx = INT_MIN, maxy = INT_MIN;
		for (int c = -margin[0]; c <= this->length + margin[this->length]; c++) {
			int t = std::clamp(c, 0, this->length);
			for (int k : {tile_rows[t].first - margin[t], tile_rows[t].second + margin[t]}) {
				auto [x, y] = this->frame.Tile(c, k);
				minx = std::min(minx, x);
				maxx = std::max(maxx, x);
				miny = std::min(miny, y);
				maxy = std::max(maxy, y);
			}
		}
		if (minx < 1 || miny < 1 || maxx >= static_cast<int>(Map::MaxX()) - 1 || maxy >= static_cast<int>(Map::MaxY()) - 1) {
			plan.error = "the corridor does not fit on the map";
			return;
		}
		plan.origin = TileXY(minx, miny);

		/* Track pieces of all tracks. */
		std::vector<RailRoutePiece> pieces = GetRailRoutePieces(steps);
		std::map<TileIndex, TrackBits> track_tiles;
		struct SignalPos { TileIndex tile; Track track; Trackdir trackdir; };
		std::vector<SignalPos> signals;
		for (int k = this->k0; k <= this->k1; k++) {
			for (const RailRoutePiece &piece : pieces) {
				TileIndex tile = this->MapTile(piece.u, piece.v + k);
				if (tile == INVALID_TILE) continue;
				auto [e1, e2] = _route_track_edges[to_underlying(piece.track)];
				Track track = TrackFromEdges(this->frame.Edge(e1), this->frame.Edge(e2));
				Trackdir trackdir = TrackExitdirToTrackdir(track, this->frame.Edge(TrackdirToExitdir(piece.trackdir)));
				track_tiles[tile].Set(track);
				/* One signal per track every interval steps, on the first piece of the step. */
				bool first_piece = piece.track != Track::Left;
				if (this->options.signals && first_piece && piece.step % std::max<int>(this->options.signal_interval, 1) == 0) {
					signals.push_back({tile, track, trackdir});
				}
			}
		}
		plan.tracks.assign(track_tiles.begin(), track_tiles.end());

		/* --- Prepare chunks: clearing, then levelling. --- */
		ChunkWriter prepare(plan.prepare);
		uint8_t clear_flags = (this->options.remove_water ? 1 << to_underlying(RailRouteClearFlag::Water) : 0) |
				(this->options.demolish ? 1 << to_underlying(RailRouteClearFlag::Demolish) : 0);
		auto emit_rect = [&](RailRouteItemKind kind, int c_from, int c_to, int row_lo, int row_hi, bool corners, int height) {
			/* Canonical columns c_from..c_to, rows row_lo..row_hi -> map rectangle. */
			auto [x1, y1] = corners ? this->frame.Corner(c_from, row_lo) : this->frame.Tile(c_from, row_lo);
			auto [x2, y2] = corners ? this->frame.Corner(c_to, row_hi) : this->frame.Tile(c_to, row_hi);
			int x = std::min(x1, x2), y = std::min(y1, y2);
			int w = std::abs(x2 - x1) + 1, h = std::abs(y2 - y1) + 1;
			std::vector<uint8_t> &item = prepare.Begin();
			item.push_back(to_underlying(kind));
			auto u16 = [&](int val) { item.push_back(GB(val, 0, 8)); item.push_back(GB(val, 8, 8)); };
			u16(x - minx); u16(y - miny); u16(w); u16(h);
			item.push_back(static_cast<uint8_t>(kind == RailRouteItemKind::Clear ? 0 : height));
			item.push_back(clear_flags);
			prepare.Commit();
		};

		/* Clear: runs of columns with the same rows. */
		for (int c = 0; c <= this->length;) {
			int e = c;
			while (e + 1 <= this->length && tile_rows[e + 1] == tile_rows[c] && margin[e + 1] == margin[c]) e++;
			emit_rect(RailRouteItemKind::Clear, c, e, tile_rows[c].first - margin[c], tile_rows[c].second + margin[c], false, 0);
			c = e + 1;
		}

		/* Clear beyond both ends too: the corners of the first and the last column touch the tiles outside. */
		emit_rect(RailRouteItemKind::Clear, -margin[0], -1, tile_rows[0].first - margin[0], tile_rows[0].second + margin[0], false, 0);
		emit_rect(RailRouteItemKind::Clear, this->length + 1, this->length + margin[this->length],
				tile_rows[this->length].first - margin[this->length], tile_rows[this->length].second + margin[this->length], false, 0);

		/* Level: corner column c spans the corners of the tiles of columns c - 1 and c. */
		std::vector<std::pair<int, int>> corner_rows(this->length + 2);
		for (int c = 0; c <= this->length + 1; c++) {
			int lo = INT_MAX, hi = INT_MIN;
			for (int t : {c - 1, c}) {
				if (t < 0 || t > this->length) continue;
				lo = std::min(lo, tile_rows[t].first);
				hi = std::max(hi, tile_rows[t].second + 1);
			}
			corner_rows[c] = {lo, hi};
		}
		for (int c = 0; c <= this->length + 1;) {
			int e = c;
			while (e + 1 <= this->length + 1 && corner_rows[e + 1] == corner_rows[c] && column_height[e + 1] == column_height[c]) e++;
			emit_rect(RailRouteItemKind::Level, c, e, corner_rows[c].first, corner_rows[c].second, true, column_height[c]);
			c = e + 1;
		}

		/* --- Build chunks: track, then signals (blueprint item format, see blueprint.h). --- */
		ChunkWriter build(plan.build);
		for (const auto &[tile, bits] : track_tiles) {
			std::vector<uint8_t> &item = build.Begin();
			item.push_back(to_underlying(BlueprintItemKind::Track));
			auto u16 = [&](int val) { item.push_back(GB(val, 0, 8)); item.push_back(GB(val, 8, 8)); };
			u16(TileX(tile) - minx); u16(TileY(tile) - miny);
			item.push_back(this->options.railtype);
			item.push_back(bits.base());
			build.Commit();
		}
		for (const SignalPos &s : signals) {
			std::vector<uint8_t> &item = build.Begin();
			item.push_back(to_underlying(BlueprintItemKind::Signal));
			auto u16 = [&](int val) { item.push_back(GB(val, 0, 8)); item.push_back(GB(val, 8, 8)); };
			u16(TileX(s.tile) - minx); u16(TileY(s.tile) - miny);
			item.push_back(to_underlying(s.track));
			item.push_back(to_underlying(this->options.signal_type));
			item.push_back(to_underlying(this->options.signal_variant));
			item.push_back(SignalAlongTrackdir(s.trackdir));
			build.Commit();
		}

		this->Estimate(plan, tile_rows, margin, corner_rows, column_height, pieces.size() * this->options.tracks, signals.size());
	}

	/** Estimate the costs of the route. */
	void Estimate(RailRoutePlan &plan, const std::vector<std::pair<int, int>> &tile_rows, const std::vector<int> &margin, const std::vector<std::pair<int, int>> &corner_rows,
			const std::vector<int> &column_height, size_t pieces, size_t signals)
	{
		RailRouteEstimate &est = plan.estimate;
		std::set<IndustryID> industries;
		for (int c = 0; c <= this->length; c++) {
			for (int k = tile_rows[c].first - margin[c]; k <= tile_rows[c].second + margin[c]; k++) {
				TileIndex tile = this->MapTile(c, k);
				if (tile == INVALID_TILE) continue;
				/* Beside the corridor only what is in the way is cleared; the land itself is left. */
				bool beside = k < tile_rows[c].first || k > tile_rows[c].second;
				if (beside && (GetTileType(tile) == TileType::Clear || GetTileType(tile) == TileType::Trees || this->ObstacleCost(tile) >= ROUTE_BLOCKED_COST)) continue;
				switch (GetTileType(tile)) {
					case TileType::Clear: est.clearing += _price[Price::ClearGrass]; break;
					case TileType::Trees: est.clearing += _price[Price::ClearTrees]; break;
					case TileType::Water:
						if (this->options.remove_water) {
							est.clearing += _price[Price::ClearWater];
							est.water++;
						} else {
							est.blocked++;
						}
						break;
					case TileType::House:
						if (this->options.demolish) {
							est.clearing += HouseSpec::Get(GetHouseType(tile))->GetRemovalCost();
							est.houses++;
						} else {
							est.blocked++;
						}
						break;
					case TileType::Industry:
						if (this->options.demolish) {
							if (industries.insert(GetIndustryIndex(tile)).second) est.clearing += GetIndustrySpec(Industry::GetByTile(tile)->type)->GetRemovalCost();
						} else {
							est.blocked++;
						}
						break;
					default:
						if (this->ObstacleCost(tile) >= ROUTE_BLOCKED_COST) {
							est.blocked++;
						} else {
							est.clearing += _price[Price::ClearRoad];
						}
						break;
				}
			}
		}
		est.industries = static_cast<uint>(industries.size());
		/* Demolition in towns costs more than the removal prices: the knock-on terraforming hits houses and roads beside. */
		est.clearing = est.clearing * 6 / 5;

		int64_t levels = 0;
		for (int c = 0; c <= this->length + 1; c++) {
			for (int k = corner_rows[c].first; k <= corner_rows[c].second; k++) {
				int h = this->CornerHeight(c, k);
				if (h < 0) continue;
				int d = std::abs(h - column_height[c]);
				levels += d;
				/* A corner on the edge drags the land beside it along: a slope of d - 1 + d - 2 + ... levels. */
				if (k == corner_rows[c].first || k == corner_rows[c].second) levels += static_cast<int64_t>(d) * (d - 1) / 2;
			}
		}
		/* Plus a margin for the knock-on effects at the ends and on diagonal edges. */
		est.terraform = _price[Price::Terraform] * levels * 2;
		est.track = RailBuildCost(this->options.railtype) * static_cast<int64_t>(pieces) + _price[Price::BuildSignals] * static_cast<int64_t>(signals);
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
 * Plan a trunk line from one tile to another.
 * @param from The start tile: the first tile of the centre track.
 * @param to The end tile: the last tile of the centre track.
 * @param options The options.
 * @return The plan; see RailRoutePlan::error.
 */
RailRoutePlan PlanRailRoute(TileIndex from, TileIndex to, const RailRouteOptions &options)
{
	RailRoutePlan plan;
	if (from >= Map::Size() || to >= Map::Size()) {
		plan.error = "invalid tile";
		return plan;
	}
	if (options.tracks < 1 || options.tracks > RailRouteOptions::MAX_TRACKS) {
		plan.error = "invalid number of tracks";
		return plan;
	}

	int dx = static_cast<int>(TileX(to)) - static_cast<int>(TileX(from));
	int dy = static_cast<int>(TileY(to)) - static_cast<int>(TileY(from));
	RouteFrame frame{static_cast<int>(TileX(from)), static_cast<int>(TileY(from)), dx < 0 ? -1 : 1, dy < 0 ? -1 : 1, std::abs(dy) > std::abs(dx)};
	int length = std::max(std::abs(dx), std::abs(dy));
	int minor = std::min(std::abs(dx), std::abs(dy));
	if (length < 2) {
		plan.error = "the route is too short";
		return plan;
	}

	RoutePlanner planner(options, frame, length, minor);
	planner.Plan(plan);
	return plan;
}
