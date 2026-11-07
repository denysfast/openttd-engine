/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file agent_chat_gui.h Interface of the native in-game agent chat window. */

#ifndef AGENT_CHAT_GUI_H
#define AGENT_CHAT_GUI_H

#include "3rdparty/nlohmann/json.hpp"

/**
 * Open the in-game agent chat window, or bring it to the front when it is already open.
 * The window is also reachable with the "agent_chat" hotkey (Ctrl+Shift+A by default).
 */
void ShowAgentChatWindow();

/**
 * Drive the agent chat: refresh the chat list, poll messages and events of the chats
 * with running tasks, and keep the status indicator in sync. The bridge itself is
 * ticked by the game loop, so this only polls the UI state.
 * Called from a realtime timer, so it runs whether or not the chat window is open.
 */
void AgentChatGuiTick();

/**
 * Read-only description of the agent chat UI, meant for GUI tests.
 *
 * The returned object contains the actual screen rectangles of the window widgets and of
 * the individual targets inside them (chat rows, attachment chips and their remove areas,
 * object disambiguation rows), so a test can locate a target and then inject real input.
 * Nothing in here executes an action or changes any state.
 *
 * Layout of the returned object:
 * - "hotkey": string, the hotkey that opens the window.
 * - "window_open": bool, whether the chat window is open.
 * - "connected", "world_id", "epoch", "connection_id": transport state.
 * - "active_chat": string id of the selected chat, empty when there is none.
 * - "input": current text of the message input.
 * - "widgets": object mapping widget names to rectangles, only present when the window is open.
 * - "chats": array of {id, title, rect, selected, task_active}.
 * - "messages": array of {seq, role, text, rect}.
 * - "tray": array of {type, id, tile, name, rect, remove_rect}.
 * - "candidates": array of {type, id, name, rect}.
 * - "status_window": object with the status indicator rectangles, or null.
 *
 * A rectangle is an object {left, top, right, bottom} in screen pixels.
 *
 * @return The description of the agent chat UI.
 */
nlohmann::json DescribeAgentChatUi();

#endif /* AGENT_CHAT_GUI_H */
