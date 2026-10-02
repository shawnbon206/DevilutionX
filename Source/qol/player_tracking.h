/**
 * @file player_tracking.h
 *
 * Says when another player enters your level, and follows one player's level with /track.
 */
#pragma once

#include <string>

#include "utils/stdcompat/string_view.hpp"

namespace devilution {

struct Surface;

/** @brief Forgets the tracked player and who was on your level; called when a game starts. */
void ResetPlayerTracking();

/** @brief Says in the chat log when another player enters your level. Runs every game tick. */
void UpdatePlayerTracking();

/**
 * @brief While a player is tracked, a line under the FPS counter with which level they're on, red once they're on
 * yours. Nothing while they're out of the game; it comes back if they rejoin.
 */
void DrawPlayerTracking(const Surface &out);

/** @brief The /track command: "/track <player name>" starts tracking, "/track off" stops. */
std::string TextCmdTrack(string_view parameter);

} // namespace devilution
