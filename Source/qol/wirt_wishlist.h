/**
 * @file wirt_wishlist.h
 *
 * /wirt and /adria: Wirt rerolls his item, or Adria her stock, until it fits a wishlist, written like the seed search's
 * (drops.ps1). A hunt runs in the background, a slice of tries each game tick, wherever you are in the game.
 */
#pragma once

#include <cstdint>
#include <string>

#include "utils/stdcompat/string_view.hpp"

namespace devilution {

struct Item;

/** How many items Wirt rolls at most in one go (SpawnBoy): a slice of a background hunt. */
extern int WirtWishlistTries;

/** @brief Whether a /wirt hunt is running. */
bool WirtWishlistActive();

/** @brief Whether a /wirt or /adria hunt is rolling items right now. */
bool IsWishlistHuntRunning();

/**
 * @brief An item seed for a hunt's next roll, independent of every roll before it. The game seeds each item from the
 * state the last one left (AdvanceRndSeed), a chain that soon runs in a loop, the same loop from almost anywhere, so a
 * long hunt kept seeing the same items and some it could never reach. In the game's own range, 0 to 2^31 - 1.
 */
uint32_t NextHuntSeed();

/** @brief Whether an item Wirt rolled fits the wishlist of the hunt running. */
bool WirtWishlistMatches(const Item &item);

/** @brief Whether a slot of Adria's stock whose first roll is this item is one to reroll: a kind the /adria hunt wants. */
bool IsAdriaWishlistSlot(const Item &item);

/** @brief Whether to roll an Adria wishlist slot again: the item doesn't fit, and the hunt's rolls aren't used up. */
bool RerollAdriaWishlistSlot(const Item &item);

/**
 * @brief Notes how a slice of Wirt's hunt went (SpawnBoy): whether he rolled a fit, after how many tries, and how many
 * wanted items he rolled that cost more than he may ask.
 */
void ReportWirtWishlist(const Item &item, bool found, int tries, int tooDear);

/** @brief Runs the /wirt and /adria hunts on, a slice each, for half a millisecond; called each game tick. */
void UpdateWishlistHunts();

/** @brief After Adria restocks on your arrival in town: what her hunt found while you were away, in its place. */
void TakePendingAdriaStock();

/**
 * @brief The /adria command, like /wirt for Adria's staves and books, in town: "/adria --type book --suffix teleport" or
 * "/adria --prefix bountiful --suffix firebolt --min-roll 90"; "/adria bases ..." lists the bases, "/adria off" stops.
 */
std::string TextCmdAdria(string_view parameter);

/**
 * @brief The /wirt command, in town: "/wirt --type ring --prefix obsidian --suffix zodiac --min-roll 90" has Wirt reroll
 * his item until it fits; "/wirt bases ..." lists the bases with their prices, "/wirt off" stops.
 */
std::string TextCmdWirt(string_view parameter);

} // namespace devilution
