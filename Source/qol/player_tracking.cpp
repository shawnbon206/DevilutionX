/**
 * @file player_tracking.cpp
 *
 * Says when another player enters your level, and with /players lists which level every other player is on.
 */
#include "qol/player_tracking.h"

#include <algorithm>
#include <array>

#include <fmt/format.h>

#include "DiabloUI/ui_flags.hpp"
#include "engine/render/text_render.hpp"
#include "levels/gendung.h"
#include "player.h"
#include "plrmsg.h"
#include "utils/language.h"

namespace devilution {

namespace {

bool ShowPlayerLevels = false;

/** Which players were on your level last tick, and the level you were on then, to notice someone arriving. */
std::array<bool, MAX_PLRS> WasOnYourLevel {};
int SeenFromLevel = -1;
bool SeenFromSetLevel = false;

bool IsOnYourLevel(const Player &player)
{
	return player.plractive && &player != MyPlayer && player.isOnActiveLevel();
}

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

void ResetPlayerTracking()
{
	WasOnYourLevel = {};
	SeenFromLevel = -1;
}

void UpdatePlayerTracking()
{
	if (MyPlayer == nullptr)
		return;
	// Who is already there when you arrive somewhere isn't news; only someone arriving after you is.
	const bool youMoved = SeenFromLevel != currlevel || SeenFromSetLevel != setlevel;
	for (size_t id = 0; id < MAX_PLRS; id++) {
		const bool here = id < Players.size() && IsOnYourLevel(Players[id]);
		if (here && !WasOnYourLevel[id] && !youMoved)
			EventPlrMsg(fmt::format(fmt::runtime(_("{:s} entered your level.")), Players[id]._pName));
		WasOnYourLevel[id] = here;
	}
	SeenFromLevel = currlevel;
	SeenFromSetLevel = setlevel;
}

void DrawPlayerTracking(const Surface &out)
{
	if (!ShowPlayerLevels)
		return;
	int nameWidth = 0;
	for (const Player &player : Players) {
		if (player.plractive && &player != MyPlayer)
			nameWidth = std::max(nameWidth, GetLineWidth(player._pName));
	}
	int y = 82;
	for (const Player &player : Players) {
		if (!player.plractive || &player == MyPlayer)
			continue;
		const UiFlags color = IsOnYourLevel(player) ? UiFlags::ColorRed : UiFlags::ColorWhitegold;
		DrawString(out, player._pName, Point { 8, y }, { color });
		DrawString(out, ShortLevelName(player), Point { 8 + nameWidth + 8, y }, { color });
		y += 12;
	}
}

std::string TextCmdPlayers(string_view /*parameter*/)
{
	ShowPlayerLevels = !ShowPlayerLevels;
	return std::string(ShowPlayerLevels ? _("Showing which level each player is on.") : _("Player levels hidden."));
}

} // namespace devilution
