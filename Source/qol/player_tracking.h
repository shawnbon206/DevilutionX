/**
 * @file player_tracking.h
 *
 * Says when another player enters your level, and with /players lists which level every other player is on.
 */
#pragma once

#include <string>

#include "utils/stdcompat/string_view.hpp"

namespace devilution {

struct Surface;

/** @brief Forgets who was on your level; called when a game starts. */
void ResetPlayerTracking();

/** @brief Says in the chat log when another player enters your level. Runs every game tick. */
void UpdatePlayerTracking();

/**
 * @brief While /players is on, a compact list under the FPS counter: each other player's name and level, the levels
 * in a column ("16", "s5" for a quest level, "t" for town), in red for anyone on your level.
 */
void DrawPlayerTracking(const Surface &out);

/** @brief The /players command, which turns the list on and off. */
std::string TextCmdPlayers(string_view parameter);

} // namespace devilution
