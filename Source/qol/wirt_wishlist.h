/**
 * @file wirt_wishlist.h
 *
 * /wirt and /adria: Wirt rerolls his item, or Adria her stock, until it fits a wishlist, written like the seed search's
 * (drops.ps1).
 */
#pragma once

#include <string>

#include "utils/stdcompat/string_view.hpp"

namespace devilution {

struct Item;

/** How many items Wirt rolls at most looking for a wanted one, so an impossible wishlist can't hang the game. */
constexpr int WirtWishlistTries = 100000;

/** @brief Whether a wishlist is set with /wirt. */
bool WirtWishlistActive();

/** @brief Whether an item Wirt rolled fits the wishlist. */
bool WirtWishlistMatches(const Item &item);

/**
 * @brief Before Wirt restocks: if his wishlist can't come up any more (you've levelled past its affixes), says why
 * once and clears it.
 */
void RecheckWirtWishlist();

/** @brief Whether a slot of Adria's stock whose first roll is this item is one to reroll: a kind the /adria wishlist wants. */
bool IsAdriaWishlistSlot(const Item &item);

/** @brief Whether to roll an Adria wishlist slot again: the item doesn't fit, and the hunt's rolls aren't used up. */
bool RerollAdriaWishlistSlot(const Item &item);

/**
 * @brief Says in the chat log how Wirt's hunt went: what he found and after how many tries, or that he found nothing,
 * and how many wanted items he rolled that cost more than he may sell (90,000 gold).
 */
void ReportWirtWishlist(const Item &item, bool found, int tries, int tooDear);

/**
 * @brief Adria stocks up (SpawnWitch at her stock level). With an /adria wishlist, every slot she'd stock with a kind on
 * it rerolls until it fits, and she restocks until she has at least one, within a limited number of rolls; the chat log
 * says what she has. Called when she stocks up as you come to town.
 */
void HuntAdria(int lvl);

/**
 * @brief The /adria command, like /wirt for Adria's staves and books: "/adria --type book --suffix teleport" or
 * "/adria --prefix bountiful --suffix firebolt --min-roll 90".
 */
std::string TextCmdAdria(string_view parameter);

/**
 * @brief The /wirt command. "/wirt --type ring --prefix obsidian --suffix zodiac --min-roll 90" sets a wishlist
 * and has Wirt reroll his item now (in town) or when you next come to town; "/wirt" shows it, "/wirt off" clears it.
 */
std::string TextCmdWirt(string_view parameter);

} // namespace devilution
