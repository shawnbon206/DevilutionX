/**
 * @file player_tracking.cpp
 *
 * Says when another player enters your level, and follows one player's level with /track.
 */
#include "qol/player_tracking.h"

#include <array>
#include <optional>

#include <fmt/format.h>

#include "DiabloUI/ui_flags.hpp"
#include "engine/render/text_render.hpp"
#include "levels/gendung.h"
#include "player.h"
#include "plrmsg.h"
#include "quests.h"
#include "utils/language.h"
#include "utils/str_case.hpp"

namespace devilution {

namespace {

/** Who /track follows, by name, so they're found again if they leave and rejoin; empty when nobody is tracked. */
std::string TrackedName;

/** Which players were on your level last tick, and the level you were on then, to notice someone arriving. */
std::array<bool, MAX_PLRS> WasOnYourLevel {};
int SeenFromLevel = -1;
bool SeenFromSetLevel = false;

bool IsOnYourLevel(const Player &player)
{
	return player.plractive && &player != MyPlayer && player.isOnActiveLevel();
}

std::optional<size_t> FindPlayerByName(string_view name)
{
	const std::string wanted = AsciiStrToLower(name);
	for (size_t id = 0; id < Players.size(); id++) {
		if (Players[id].plractive && &Players[id] != MyPlayer && AsciiStrToLower(Players[id]._pName) == wanted)
			return id;
	}
	return std::nullopt;
}

/** "town", "dungeon level 9", or a quest level's name. */
std::string LevelName(const Player &player)
{
	if (player.plrIsOnSetLevel) {
		for (const Quest &quest : Quests) {
			if (quest._qslvl == player.plrlevel)
				return std::string(_(QuestsData[quest._qidx]._qlstr));
		}
	}
	if (player.plrlevel == 0)
		return std::string(_("town"));
	return fmt::format(fmt::runtime(_("dungeon level {:d}")), player.plrlevel);
}

} // namespace

void ResetPlayerTracking()
{
	TrackedName.clear();
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
	if (TrackedName.empty())
		return;
	const std::optional<size_t> id = FindPlayerByName(TrackedName);
	if (!id)
		return;
	const Player &player = Players[*id];
	if (IsOnYourLevel(player)) {
		DrawString(out, fmt::format(fmt::runtime(_("{:s} is on your level")), player._pName), Point { 8, 82 }, { UiFlags::ColorRed });
		return;
	}
	DrawString(out, fmt::format(fmt::runtime(_("Tracking {:s}: {:s}")), player._pName, LevelName(player)), Point { 8, 82 }, { UiFlags::ColorWhitegold });
}

std::string TextCmdTrack(string_view parameter)
{
	if (parameter.empty()) {
		if (TrackedName.empty())
			return std::string(_("Use /track <player name> or /track off."));
		return fmt::format(fmt::runtime(_("Tracking {:s}.")), TrackedName);
	}
	if (parameter == "off") {
		TrackedName.clear();
		return std::string(_("Stopped tracking."));
	}
	const std::optional<size_t> id = FindPlayerByName(parameter);
	if (!id)
		return fmt::format(fmt::runtime(_("There is no other player called {:s} in this game.")), parameter);
	TrackedName = Players[*id]._pName;
	return fmt::format(fmt::runtime(_("Tracking {:s}.")), TrackedName);
}

} // namespace devilution
