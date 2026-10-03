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
 * @brief Says in the chat log how Wirt's hunt went: what he found and after how many tries, or that he found nothing,
 * and how many wanted items he rolled that cost more than he may sell (90,000 gold).
 */
void ReportWirtWishlist(const Item &item, bool found, int tries, int tooDear);

/**
 * @brief With an /adria wishlist, Adria restocks (SpawnWitch at her stock level) until she has an item that fits it, a
 * limited number of times, and the chat log says how it went. Called when she stocks up as you come to town.
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
