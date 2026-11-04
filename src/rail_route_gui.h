/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file rail_route_gui.h GUI of the rail trunk line tool: its window and the route preview in the viewport. */

#ifndef RAIL_ROUTE_GUI_H
#define RAIL_ROUTE_GUI_H

#include "tile_type.h"
#include "track_type.h"
#include "window_type.h"

/** What the trunk line preview shows on one tile. */
struct RailRoutePreviewTile {
	TrackBits tracks{}; ///< Planned track on the tile.
	bool start = false; ///< The tile is the start tile of the route.
	bool end = false; ///< The tile is the end tile of the route (where the line really ends, which may differ from the cursor).
	bool red = false; ///< Draw in red: the route cannot be built here or the company cannot afford it.
};

Window *ShowRailRouteWindow();

bool IsRailRoutePreviewActive();
RailRoutePreviewTile GetRailRoutePreviewTile(TileIndex tile);

#endif /* RAIL_ROUTE_GUI_H */
