/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file rail_route.cpp Test the track geometry of the rail trunk line tool. */

#include "../stdafx.h"

#include "../3rdparty/catch2/catch.hpp"

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
