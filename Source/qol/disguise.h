/**
 * @file disguise.h
 *
 * /disguise: the other players are shown your weapons, armor and jewelry with the same bases and affixes rolled
 * again, in /inspect and everywhere else their game uses your gear. Your own game keeps the real items. Whether it's on
 * is saved with the options, so it carries over to the next game.
 */
#pragma once

#include <cstdint>
#include <string>

#include "utils/stdcompat/string_view.hpp"

namespace devilution {

struct Item;
struct Player;
struct PlayerNetPack;

/**
 * @brief The item as the other players are sent it: while the disguise is on, a weapon, armor or jewelry rolled again
 * from another seed to the same base and affixes, always the same one for the same item. Uniques
 * and everything else are sent as they are.
 */
Item DisguisedItem(const Item &item);

/**
 * @brief Packs the local player for the other players (SendPlayerInfo). While the disguise is on, that's the player
 * wearing the disguised items, with the totals (armor, damage, life, ...) worked out from them, as the other games
 * check those against the items and drop a player whose don't match.
 */
void PackDisguisedNetPlayer(PlayerNetPack &packed, Player &player);

/**
 * @brief The life and mana every packet tells the other players the local player has (the packet header): while the
 * disguise is on, less whatever the real items give over the disguised ones.
 */
void DisguisedLifeAndMana(Player &player, int32_t &hitPoints, int32_t &maxHitPoints, int32_t &mana, int32_t &maxMana);

/** @brief The /disguise command: turns the disguise on or off, and sends the other players the gear to match. */
std::string TextCmdDisguise(string_view parameter);

} // namespace devilution
