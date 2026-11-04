/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file rail_route_widget.h Types related to the rail trunk line widgets. */

#ifndef WIDGETS_RAIL_ROUTE_WIDGET_H
#define WIDGETS_RAIL_ROUTE_WIDGET_H

/** Widgets of the #RailRouteWindow class. */
enum RailRouteWidgets : WidgetID {
	WID_RR_CAPTION, ///< Caption of the window.
	WID_RR_START_NS, ///< Start orientation: diagonal, north-south on the screen.
	WID_RR_START_X, ///< Start orientation: straight along the x axis.
	WID_RR_START_EW, ///< Start orientation: diagonal, east-west on the screen.
	WID_RR_START_Y, ///< Start orientation: straight along the y axis.
	WID_RR_TRACKS_DECREASE, ///< One track less.
	WID_RR_TRACKS, ///< Number of tracks.
	WID_RR_TRACKS_INCREASE, ///< One track more.
	WID_RR_SIGNALS, ///< Toggle: build signals.
	WID_RR_INTERVAL_DECREASE, ///< Signals closer together.
	WID_RR_INTERVAL, ///< Distance between signals.
	WID_RR_INTERVAL_INCREASE, ///< Signals further apart.
	WID_RR_WATER, ///< Toggle: remove water.
	WID_RR_DEMOLISH, ///< Toggle: demolish town houses, industries and town roads.
	WID_RR_SEA, ///< Toggle: raise land at sea level.
	WID_RR_BUILD, ///< Start placing a trunk line.
	WID_RR_INFO, ///< Placement state and the planned route.
	WID_RR_STATUS, ///< Progress of the construction job.
	WID_RR_ABORT, ///< Abort the construction job.
};

#endif /* WIDGETS_RAIL_ROUTE_WIDGET_H */
