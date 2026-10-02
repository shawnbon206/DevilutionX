/**
 * @file player_tracking.h
 *
 * Lists which level every other player is on.
 */
#pragma once

namespace devilution {

struct Surface;

/**
 * @brief A compact list under the FPS counter: each other player's name and level, the levels in a column ("16",
 * "s5" for a quest level, "t" for town). A ">" marks anyone on your level, and hostile players are red. Empty when
 * you're alone.
 */
void DrawPlayerTracking(const Surface &out);

} // namespace devilution
