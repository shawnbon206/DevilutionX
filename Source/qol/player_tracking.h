/**
 * @file player_tracking.h
 *
 * Lists which level every other player is on, and says when one enters your level.
 */
#pragma once

namespace devilution {

struct Surface;

/** @brief Forgets who was on your level; called when a game starts. */
void ResetPlayerTracking();

/** @brief Says in the chat log when another player enters your level. Runs every game tick. */
void UpdatePlayerTracking();

/**
 * @brief A compact list under the FPS counter: each other player's name and level, the levels in a column ("16",
 * "s5" for a quest level, "t" for town), in red for anyone on your level. Empty when you're alone.
 */
void DrawPlayerTracking(const Surface &out);

} // namespace devilution
