/**
 * @file wirt_wishlist.h
 *
 * /wirt and /adria: Wirt rerolls his item, or Adria her stock, until it fits a wishlist, written like the seed search's
 * (drops.ps1). Each command is one hunt, then; nothing is kept for later restocks.
 */
#pragma once

#include <string>

#include "utils/stdcompat/string_view.hpp"

namespace devilution {

struct Item;

/** How many items Wirt rolls at most looking for a wanted one, so an impossible wishlist can't hang the game. */
constexpr int WirtWishlistTries = 100000;

/** @brief Whether a /wirt hunt is running. */
bool WirtWishlistActive();

/** @brief Whether an item Wirt rolled fits the wishlist of the hunt running. */
bool WirtWishlistMatches(const Item &item);

/** @brief Whether a slot of Adria's stock whose first roll is this item is one to reroll: a kind the /adria hunt wants. */
bool IsAdriaWishlistSlot(const Item &item);

/** @brief Whether to roll an Adria wishlist slot again: the item doesn't fit, and the hunt's rolls aren't used up. */
bool RerollAdriaWishlistSlot(const Item &item);

/**
 * @brief Says in the chat log how Wirt's hunt went: what he found and after how many tries, or that he found nothing,
 * and how many wanted items he rolled that cost more than he may ask.
 */
void ReportWirtWishlist(const Item &item, bool found, int tries, int tooDear);

/**
 * @brief Adria stocks up (SpawnWitch at her stock level). During an /adria hunt, every slot she'd stock with a kind on the
 * wishlist rerolls until it fits, and she restocks until she has at least one, within a limited number of rolls; the
 * chat log says what she has.
 */
void HuntAdria(int lvl);

/**
 * @brief The /adria command, like /wirt for Adria's staves and books, in town: "/adria --type book --suffix teleport" or
 * "/adria --prefix bountiful --suffix firebolt --min-roll 90"; "/adria bases ..." lists the bases.
 */
std::string TextCmdAdria(string_view parameter);

/**
 * @brief The /wirt command: "/wirt --type ring --prefix obsidian --suffix zodiac --min-roll 90" has Wirt reroll his item
 * until it fits, once, now; "/wirt bases ..." lists the bases with their prices.
 */
std::string TextCmdWirt(string_view parameter);

} // namespace devilution
