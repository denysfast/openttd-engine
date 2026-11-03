/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file rail_route_cmd.h Command definitions for preparing the corridor of a rail trunk line. */

#ifndef RAIL_ROUTE_CMD_H
#define RAIL_ROUTE_CMD_H

#include "blueprint_cmd.h"
#include "rail_route.h"

std::tuple<CommandCost, Money, uint32_t, uint32_t, StringID> CmdPrepareRailRoute(DoCommandFlags flags, TileIndex origin, const BlueprintChunkData &chunk);

/* Test and execution differ: levelling depends on the clearing before it. */
DEF_CMD_TRAIT(Commands::PrepareRailRoute, CmdPrepareRailRoute, CommandFlags({CommandFlag::NoTest, CommandFlag::NoEst}), CommandType::LandscapeConstruction)

void CcPrepareRailRoute(Commands cmd, const CommandCost &result, Money extra_money, uint32_t done, uint32_t failed, StringID last_error);

#endif /* RAIL_ROUTE_CMD_H */
