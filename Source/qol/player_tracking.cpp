/**
 * @file player_tracking.cpp
 *
 * Lists which level every other player is on.
 */
#include "qol/player_tracking.h"

#include <algorithm>
#include <string>

#include <fmt/format.h>

#include "DiabloUI/ui_flags.hpp"
#include "engine/render/text_render.hpp"
#include "player.h"

namespace devilution {

namespace {

/** "16" for a dungeon level, "s5" for a quest level (Lazarus' Lair), "t" for town. */
std::string ShortLevelName(const Player &player)
{
	if (player.plrIsOnSetLevel)
		return fmt::format("s{:d}", player.plrlevel);
	if (player.plrlevel == 0)
		return "t";
	return fmt::format("{:d}", player.plrlevel);
}

} // namespace

void DrawPlayerTracking(const Surface &out)
{
	constexpr int Left = 8;
	const int nameLeft = Left + GetLineWidth(">") + 4;
	int nameWidth = 0;
	for (const Player &player : Players) {
		if (player.plractive && &player != MyPlayer)
			nameWidth = std::max(nameWidth, GetLineWidth(player._pName));
	}
	int y = 82;
	for (const Player &player : Players) {
		if (!player.plractive || &player == MyPlayer)
			continue;
		const UiFlags color = player.friendlyMode ? UiFlags::ColorWhitegold : UiFlags::ColorRed;
		if (player.isOnActiveLevel())
			DrawString(out, ">", Point { Left, y }, { color });
		DrawString(out, player._pName, Point { nameLeft, y }, { color });
		DrawString(out, ShortLevelName(player), Point { nameLeft + nameWidth + 8, y }, { color });
		y += 12;
	}
}

} // namespace devilution
