/**
 * @file wirt_wishlist.h
 *
 * /wirt: Wirt rerolls his item until it fits a wishlist, written like the seed search's (drops.ps1).
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
 * @brief Says in the chat log how Wirt's hunt went: what he found and after how many tries, or that he found nothing,
 * and how many wanted items he rolled that cost more than he may sell (90,000 gold).
 */
void ReportWirtWishlist(const Item &item, bool found, int tries, int tooDear);

/**
 * @brief The /wirt command. "/wirt --type ring --prefix obsidian --suffix zodiac --min-roll 90" sets a wishlist
 * and has Wirt reroll his item now (in town) or when you next come to town; "/wirt" shows it, "/wirt off" clears it.
 */
std::string TextCmdWirt(string_view parameter);

} // namespace devilution
