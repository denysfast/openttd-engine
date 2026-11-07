/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file agent_bridge.h Native bridge between the running game and the in-game agent backend. */

#ifndef AGENT_BRIDGE_H
#define AGENT_BRIDGE_H

#include "3rdparty/nlohmann/json.hpp"
#include "tile_type.h"

#include <functional>
#include <string>

/**
 * Native bridge between the running game and the in-game agent backend.
 *
 * The bridge connects outward to the backend over authenticated HTTP, polls for
 * requests and executes them on the main thread. It is shared by the engine
 * (which owns the polling) and by the native chat UI (which uses Request()).
 */
namespace InGameAgent {

using Json = nlohmann::json;

/**
 * Run one step of the bridge. Must be called from the main thread once per
 * game loop iteration; it also runs while the game is paused.
 */
void Tick();

/**
 * Forget everything about the current world and start a fresh epoch. Called
 * whenever a new game is started or a savegame is loaded.
 */
void ResetWorld();

/**
 * Identity of the world the bridge is currently bound to.
 * @return {world_id, epoch, connection_id, connected}.
 */
Json Context();

/**
 * Describe the objects attached to a tile, as typed attachment candidates.
 *
 * Every entry carries the generation of the object and the world/epoch it was
 * read in, so a later request can name the exact object it was planned for: the
 * object the tile carries (station, waypoint or buoy, industry, the town of a
 * house, rail/road/ship depot), then every vehicle and every sign standing on it,
 * then the tile itself. The tile is named in the canonical `{"x": .., "y": ..}`
 * shape the protocol schema uses.
 * @param tile The tile to inspect.
 * @return Array of {type, id?, generation?, name?, what?, tile, world_id, epoch} entries, most specific first.
 */
Json PickObjects(TileIndex tile);

/**
 * Send a request to the backend on behalf of the native UI. The UI token is
 * used for authentication, and the callback is always invoked on the main
 * thread.
 * @param method HTTP method, e.g. "GET" or "POST".
 * @param path Path below the backend base URL, e.g. "/v1/chats".
 * @param body Request body; ignored for GET.
 * @param callback Called with the parsed response and whether it succeeded.
 */
void Request(std::string method, std::string path, Json body, std::function<void(Json, bool)> callback);

} // namespace InGameAgent

#endif /* AGENT_BRIDGE_H */
