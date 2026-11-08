/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file rail_route.cpp Test the track geometry of the rail trunk line tool. */

#include "../stdafx.h"

#include "../3rdparty/catch2/catch.hpp"

#include "../clear_map.h"
#include "../company_func.h"
#include "../map_func.h"
#include "../rail.h"
#include "../rail_map.h"
#include "../settings_type.h"
#include "../station_map.h"
#include "../void_map.h"
#include "../rail_route.h"
#include "../track_func.h"

#include "../safeguards.h"

/** Offset to the neighbouring tile across an edge, as if u were x and v were y. */
static std::pair<int, int> EdgeDelta(DiagDirection dir)
{
	switch (dir) {
		case DiagDirection::NE: return {-1, 0};
		case DiagDirection::SE: return {0, 1};
		case DiagDirection::SW: return {1, 0};
		case DiagDirection::NW: return {0, -1};
		default: NOT_REACHED();
	}
}

/** Edges a track connects. */
static std::set<DiagDirection> TrackEdges(Track t)
{
	Trackdir td = TrackToTrackdir(t);
	return {TrackdirToExitdir(td), TrackdirToExitdir(ReverseTrackdir(td))};
}

/** Make steps from a pattern: 'S' straight, 'D' diagonal. */
static std::vector<RailRouteStep> Steps(std::string_view pattern)
{
	std::vector<RailRouteStep> steps;
	for (char c : pattern) steps.push_back({c == 'D', 0});
	return steps;
}

TEST_CASE("RailRoutePiecesConnect")
{
	for (std::string_view pattern : {"SSSS", "DDDD", "SSDDSS", "DSDSDS", "SDDDDDDSSSSDDS"}) {
		std::vector<RailRoutePiece> pieces = GetRailRoutePieces(Steps(pattern));
		REQUIRE(pieces.front().u == 0);
		REQUIRE(pieces.front().v == 0);
		int diagonal = static_cast<int>(std::count(pattern.begin(), pattern.end(), 'D'));
		CHECK(pieces.back().u == static_cast<int>(pattern.size()));
		CHECK(pieces.back().v == diagonal);

		for (size_t i = 0; i + 1 < pieces.size(); i++) {
			const RailRoutePiece &a = pieces[i];
			const RailRoutePiece &b = pieces[i + 1];
			CHECK(TrackdirToTrack(a.trackdir) == a.track);
			/* Leaving a through its exit edge enters b through the opposite edge. */
			DiagDirection exit = TrackdirToExitdir(a.trackdir);
			auto [du, dv] = EdgeDelta(exit);
			CHECK(b.u == a.u + du);
			CHECK(b.v == a.v + dv);
			CHECK(TrackEdges(b.track).contains(ReverseDiagDir(exit)));
			/* The train only ever moves towards the end: +u or +v. */
			CHECK((exit == DiagDirection::SW || exit == DiagDirection::SE));
		}
	}
}

TEST_CASE("RailRouteParallelTracksStayApart")
{
	/* Thirty tracks, each the same polyline moved by one tile in v: pieces of different tracks on one tile must not share an edge. */
	for (std::string_view pattern : {"SSSSDDDDSSSS", "DDDDSSSSDDDD", "SDSDSDSD", "DDDDDDDD"}) {
		std::vector<RailRoutePiece> pieces = GetRailRoutePieces(Steps(pattern));
		std::map<std::pair<int, int>, std::vector<std::pair<int, Track>>> tiles;
		for (int k = 0; k < 30; k++) {
			for (const RailRoutePiece &p : pieces) tiles[{p.u, p.v + k}].push_back({k, p.track});
		}
		for (const auto &[pos, list] : tiles) {
			for (size_t i = 0; i < list.size(); i++) {
				for (size_t j = i + 1; j < list.size(); j++) {
					if (list[i].first == list[j].first) continue;
					CHECK(list[i].second != list[j].second);
					std::set<DiagDirection> a = TrackEdges(list[i].second);
					for (DiagDirection e : TrackEdges(list[j].second)) CHECK_FALSE(a.contains(e));
				}
			}
		}
	}
}

/** Small terrain fixture; no running game or company pool is needed by the read-only planner. */
struct RouteMap {
	CompanyID old_company = _current_company;
	uint8_t old_height_limit = _settings_game.construction.map_height_limit;
	bool old_slopes = _settings_game.construction.build_on_slopes;

	RouteMap()
	{
		Map::Allocate(128, 128);
		for (Tile t : Map::Iterate()) {
			MakeClear(t, ClearGround::Grass, 3);
			SetTileHeight(t, 3);
			if (TileX(t) == Map::MaxX() || TileY(t) == Map::MaxY()) MakeVoid(t);
		}
		_current_company = CompanyID{0};
		_settings_game.construction.map_height_limit = 30;
		_settings_game.construction.build_on_slopes = true;
		ResetRailTypes();
	}

	~RouteMap()
	{
		_current_company = old_company;
		_settings_game.construction.map_height_limit = old_height_limit;
		_settings_game.construction.build_on_slopes = old_slopes;
	}
};

/** Planned tracks at a given tile. */
static TrackBits PlannedBits(const RailRoutePlan &plan, uint x, uint y)
{
	for (auto [tile, bits] : plan.tracks) if (tile == TileXY(x, y)) return bits;
	return {};
}

TEST_CASE("RailRouteFlatGroundNeedsNoPreparation")
{
	RouteMap map;
	RailRouteOptions o;
	for (uint8_t lanes : {1, 2, 8, 30}) {
		o.tracks = lanes;
		for (RailRouteStart start : {RailRouteStart::X, RailRouteStart::Y, RailRouteStart::NS, RailRouteStart::EW}) {
			o.start = start;
			for (auto [x, y] : {std::pair{85, 72}, {40, 45}, {85, 45}, {40, 72}}) {
				CAPTURE(lanes, start, x, y);
				auto plan = PlanRailRoute(TileXY(64, 64), TileXY(x, y), o);
				/* A diagonal orientation only reaches its corresponding quadrants. */
				if ((start == RailRouteStart::NS && (x > 64) != (y > 64)) ||
						(start == RailRouteStart::EW && (x > 64) == (y > 64))) {
					CHECK_FALSE(plan.error.empty());
					continue;
				}
				REQUIRE(plan.error.empty());
				CHECK(plan.prepare.empty());
				CHECK(plan.estimate.terraform == 0);
				for (auto [tile, bits] : GetRailRouteStartPieces(TileXY(64, 64), start, lanes)) {
					CHECK((PlannedBits(plan, TileX(tile), TileY(tile)) & bits) == bits);
				}
			}
		}
	}
}

TEST_CASE("RailRouteNaturalRampAndNearbyRailArePreserved")
{
	RouteMap map;
	for (Tile t : Map::Iterate()) SetTileHeight(t, 3 + std::clamp((static_cast<int>(TileX(t)) - 30) / 5, 0, 4));
	RailRouteOptions o;
	o.tracks = 2;
	o.start = RailRouteStart::X;
	MakeRailNormal(TileXY(30, 50), _current_company, Track::X, o.railtype);
	MakeRailNormal(TileXY(70, 50), _current_company, Track::X, o.railtype);
	MakeRailNormal(TileXY(45, 53), CompanyID{1}, Track::X, o.railtype);
	auto plan = PlanRailRoute(TileXY(30, 50), TileXY(70, 50), o);
	REQUIRE(plan.error.empty());
	CHECK(plan.prepare.empty());
	CHECK(plan.estimate.blocked == 0);
	CHECK(plan.min_height == 3);
	CHECK(plan.max_height == 7);
}

TEST_CASE("RailRouteReusesExistingRailAndDiagonalTerminal")
{
	RouteMap map;
	RailRouteOptions o;
	o.tracks = 1;
	o.signals = false;
	o.start = RailRouteStart::X;
	MakeRailNormal(TileXY(30, 30), _current_company, Track::X, o.railtype);
	MakeRailNormal(TileXY(60, 45), _current_company, Track::Right, o.railtype);
	auto plan = PlanRailRoute(TileXY(30, 30), TileXY(60, 45), o);
	REQUIRE(plan.error.empty());
	CHECK(plan.prepare.empty());
	CHECK(PlannedBits(plan, 60, 45) == Track::Right);
	CHECK(plan.estimate.blocked == 0);

	/* The exact same route is a no-op, including its signal configuration. */
	for (auto [tile, bits] : plan.tracks) MakeRailNormal(tile, _current_company, bits, o.railtype);
	auto repeated = PlanRailRoute(TileXY(30, 30), TileXY(60, 45), o);
	REQUIRE(repeated.error.empty());
	CHECK(repeated.prepare.empty());
	CHECK(repeated.build.empty());
}

TEST_CASE("RailRoutePerpendicularJoinUsesTurnout")
{
	RouteMap map;
	RailRouteOptions o;
	o.tracks = 1;
	o.start = RailRouteStart::X;
	MakeRailNormal(TileXY(60, 30), _current_company, Track::Y, o.railtype);
	auto plan = PlanRailRoute(TileXY(30, 30), TileXY(60, 30), o);
	REQUIRE(plan.error.empty());
	CHECK(PlannedBits(plan, 60, 30) == Track::Right);
	CHECK(plan.prepare.empty());
}

TEST_CASE("RailRouteBothHalvesOfDiagonalStartStayFixed")
{
	RouteMap map;
	RailRouteOptions o;
	o.tracks = 4;
	for (Track piece : {Track::Left, Track::Right, Track::Upper, Track::Lower}) {
		MakeRailNormal(TileXY(64, 64), _current_company, piece, o.railtype);
		o.start = GetRailRouteStartOrientation(TileXY(64, 64), o.railtype);
		for (auto [dx, dy] : {std::pair{20, 10}, {-20, -10}, {10, 20}, {-10, -20}}) {
			if (o.start == RailRouteStart::EW) dy = -dy;
			CAPTURE(piece, dx, dy);
			auto plan = PlanRailRoute(TileXY(64, 64), TileXY(64 + dx, 64 + dy), o);
			REQUIRE(plan.error.empty());
			CHECK(PlannedBits(plan, 64, 64).Test(piece));
			CHECK(plan.prepare.empty());
		}
	}
}

TEST_CASE("RailRouteMovesBendsAroundProtectedObstacles")
{
	RouteMap map;
	RailRouteOptions o;
	o.tracks = 1;
	o.start = RailRouteStart::X;
	/* Original one-bend route goes straight to x=50, then diagonally to (70,50). */
	MakeRailNormal(TileXY(45, 30), CompanyID{1}, Track::Y, o.railtype);
	auto plan = PlanRailRoute(TileXY(30, 30), TileXY(70, 50), o);
	REQUIRE(plan.error.empty());
	CHECK(PlannedBits(plan, 45, 30).None());
	CHECK(plan.prepare.empty());
	CHECK(plan.end == TileXY(70, 50));
}

TEST_CASE("RailRouteRejectsBlockedRouteBeforeConstruction")
{
	RouteMap map;
	RailRouteOptions o;
	o.tracks = 1;
	o.start = RailRouteStart::X;
	MakeRailNormal(TileXY(45, 30), CompanyID{1}, Track::X, o.railtype);
	auto plan = PlanRailRoute(TileXY(30, 30), TileXY(60, 30), o);
	CHECK_FALSE(plan.error.empty());
	CHECK(plan.prepare.empty());
	CHECK(plan.build.empty());
}

TEST_CASE("RailRouteAllowsShortExtensionsAndMapEdges")
{
	RouteMap map;
	RailRouteOptions o;
	o.tracks = 1;
	o.start = RailRouteStart::X;
	for (auto [from, to] : {std::pair{TileXY(20, 20), TileXY(21, 20)}, {TileXY(1, 1), TileXY(5, 1)}}) {
		auto plan = PlanRailRoute(from, to, o);
		CHECK(plan.error.empty());
		CHECK(plan.prepare.empty());
	}
}

TEST_CASE("RailRouteEarthworksOnlyTouchNecessaryCorners")
{
	RouteMap map;
	for (Tile t : Map::Iterate()) {
		int radius = std::max(std::abs(static_cast<int>(TileX(t)) - 50), std::abs(static_cast<int>(TileY(t)) - 50));
		SetTileHeight(t, 3 + std::max(0, 4 - radius));
	}
	RailRouteOptions o;
	o.tracks = 1;
	o.start = RailRouteStart::NS;
	o.terraform = false;
	CHECK_FALSE(PlanRailRoute(TileXY(40, 40), TileXY(60, 60), o).error.empty());
	o.terraform = true;
	auto plan = PlanRailRoute(TileXY(40, 40), TileXY(60, 60), o);
	REQUIRE(plan.error.empty());
	REQUIRE_FALSE(plan.prepare.empty());
	std::set<TileIndex> changed;
	for (const auto &chunk : plan.prepare) {
		for (size_t i = 0; i < chunk.data.size(); i += 11) {
			const auto &data = chunk.data;
			REQUIRE(i + 11 <= data.size());
			CHECK(data[i] == to_underlying(RailRouteItemKind::Level));
			auto u16 = [&](size_t off) { return data[i + off] | data[i + off + 1] << 8; };
			CHECK(u16(5) == 1);
			CHECK(u16(7) == 1);
			TileIndex corner = TileXY(TileX(plan.origin) + u16(1), TileY(plan.origin) + u16(3));
			CHECK(TileHeight(corner) != data[i + 9]);
			CHECK(changed.insert(corner).second);
			CHECK(data[i + 9] == 3);
		}
	}
	CHECK(changed.size() < 40);
}

TEST_CASE("RailRouteProtectsDifferentHeightConnections")
{
	RouteMap map;
	for (Tile t : Map::Iterate()) SetTileHeight(t, 3 + std::clamp((static_cast<int>(TileX(t)) - 40) / 6, 0, 2));
	RailRouteOptions o;
	o.tracks = 1;
	o.start = RailRouteStart::NS;
	MakeRailNormal(TileXY(35, 35), _current_company, Track::Right, o.railtype);
	MakeRailNormal(TileXY(60, 60), _current_company, Track::Right, o.railtype);
	/* A pure diagonal cannot connect two different levels. No cost overflow may override the pins. */
	auto plan = PlanRailRoute(TileXY(35, 35), TileXY(60, 60), o);
	CHECK_FALSE(plan.error.empty());
	CHECK(plan.prepare.empty());
	CHECK(plan.build.empty());
}

TEST_CASE("RailRouteLeavesHouseBeyondEndpointAlone")
{
	RouteMap map;
	SetTileType(TileXY(61, 30), TileType::House);
	RailRouteOptions o;
	o.tracks = 1;
	o.start = RailRouteStart::X;
	o.demolish = false;
	auto plan = PlanRailRoute(TileXY(30, 30), TileXY(60, 30), o);
	CHECK(plan.error.empty());
	CHECK(plan.prepare.empty());
}

TEST_CASE("RailRouteConnectsToStationWithoutRebuildingIt")
{
	RouteMap map;
	RailRouteOptions o;
	o.tracks = 1;
	o.start = RailRouteStart::X;
	MakeRailStation(TileXY(60, 30), _current_company, StationID{0}, Axis::X, 0, o.railtype);
	auto plan = PlanRailRoute(TileXY(30, 30), TileXY(60, 30), o);
	REQUIRE(plan.error.empty());
	CHECK(plan.prepare.empty());
	CHECK(PlannedBits(plan, 60, 30) == Track::X);
}

TEST_CASE("RailRouteDoesNotRemoveSignalsForTurnouts")
{
	RouteMap map;
	RailRouteOptions o;
	o.tracks = 1;
	o.start = RailRouteStart::X;
	TileIndex end = TileXY(60, 30);
	MakeRailNormal(end, _current_company, Track::Y, o.railtype);
	SetHasSignals(end, true);
	auto plan = PlanRailRoute(TileXY(30, 30), end, o);
	CHECK_FALSE(plan.error.empty());
	CHECK(plan.prepare.empty());
	CHECK(plan.build.empty());
}

TEST_CASE("RailRouteFindsBendsBetweenBlockedSamples")
{
	RouteMap map;
	RailRouteOptions o;
	o.tracks = 1;
	o.start = RailRouteStart::X;
	o.terraform = false;
	/* Block all four distinct fixed-X samples, leaving other bend positions free. */
	for (auto [x, y] : {std::pair{45, 30}, {40, 35}, {45, 35}, {50, 35}}) {
		MakeRailNormal(TileXY(x, y), CompanyID{1}, Track::Y, o.railtype);
	}
	auto plan = PlanRailRoute(TileXY(30, 30), TileXY(70, 50), o);
	REQUIRE(plan.error.empty());
	CHECK(plan.end == TileXY(70, 50));
	CHECK(plan.prepare.empty());
	CHECK(PlannedBits(plan, 30, 30) == Track::X);
	for (auto [x, y] : {std::pair{45, 30}, {40, 35}, {45, 35}, {50, 35}}) {
		CHECK(PlannedBits(plan, x, y).None());
		CHECK(GetTileOwner(TileXY(x, y)) == CompanyID{1});
		CHECK(GetTrackBits(TileXY(x, y)) == Track::Y);
	}
}
