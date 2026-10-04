/**
 * @file disguise.cpp
 *
 * /disguise: the other players are shown your weapons, armor and jewelry with the same bases and affixes but poor
 * rolls. Items go to other games as the seed they were rolled from, and those games roll them again, so a disguised
 * item is the same base and creation info rolled from another seed: the first one found that comes out with the same
 * name (base, prefix and suffix). Its rolls are another draw, so on average a high roll shows as a lower one.
 */
#include "qol/disguise.h"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstring>
#include <map>
#include <tuple>

#include "engine/random.hpp"
#include "inv.h"
#include "items.h"
#include "msg.h"
#include "multi.h"
#include "options.h"
#include "pack.h"
#include "player.h"
#include "utils/language.h"

namespace devilution {

namespace {

/**
 * How many seeds are tried for a disguise at most. Nothing is stored: an item's disguise is worked out again each
 * session from its own seed, so changing PassesFor or the seed walk changes every disguise at once, which players who
 * inspect you would notice.
 */
constexpr int MaxSeedTries = 200000;

/** The disguise seed found for each real item, by the item's seed, base and creation info. */
std::map<std::tuple<uint32_t, int, uint16_t>, uint32_t> DisguiseSeeds;

bool DisguiseOn()
{
	return gbIsMultiplayer && *sgOptions.Gameplay.disguiseGear;
}

/** Weapons, armor and jewelry other than uniques, whose stats depend on their rolls. */
bool CanDisguise(const Item &item)
{
	if (item.isEmpty() || item.IDidx == IDI_EAR || item.IDidx == IDI_GOLD || item._iMagical == ITEM_QUALITY_UNIQUE)
		return false;
	switch (item._itype) {
	case ItemType::Sword:
	case ItemType::Axe:
	case ItemType::Bow:
	case ItemType::Mace:
	case ItemType::Shield:
	case ItemType::LightArmor:
	case ItemType::Helm:
	case ItemType::MediumArmor:
	case ItemType::HeavyArmor:
	case ItemType::Staff:
	case ItemType::Ring:
	case ItemType::Amulet:
		return true;
	default:
		return false;
	}
}

/** The item rolled again from another seed, as another game rolls it from what it's sent (RecreateItem). */
Item RollFromSeed(const Item &item, uint32_t seed)
{
	Item rolled;
	RecreateItem(*MyPlayer, rolled, item.IDidx, item._iCreateInfo, seed, item._ivalue, (item.dwBuff & CF_HELLFIRE) != 0);
	return rolled;
}

/**
 * Whether an item rolled from another seed passes for this one: the same base (a store item's base comes from its seed
 * too, and bases share names: "Plate" is more than one), the same name (so the same prefix and suffix), the same light
 * (it changes the light radius), and no curse rolled worse, so the disguise never takes more life or mana than the
 * real items' curses do.
 */
bool PassesFor(const Item &rolled, const Item &item)
{
	return rolled.IDidx == item.IDidx && rolled._iMagical == item._iMagical && std::strcmp(rolled._iIName, item._iIName) == 0
	    && rolled._iPLLight == item._iPLLight
	    && rolled._iPLHP >= std::min<int>(item._iPLHP, 0) && rolled._iPLVit >= std::min<int>(item._iPLVit, 0)
	    && rolled._iPLMana >= std::min<int>(item._iPLMana, 0) && rolled._iPLMag >= std::min<int>(item._iPLMag, 0);
}

/**
 * The seed of the first lookalike, walking seeds from one that comes from the item's own, so the same item always gets
 * the same disguise; the item's own seed if none turns up.
 */
uint32_t FindDisguiseSeed(const Item &item)
{
	const uint32_t rngState = GetLCGEngineState();
	uint32_t found = item._iSeed;
	uint32_t seed = item._iSeed;
	for (int tries = 0; tries < MaxSeedTries; tries++) {
		seed = seed * 1664525 + 1013904223;
		if (PassesFor(RollFromSeed(item, seed), item)) {
			found = seed;
			break;
		}
	}
	SetRndSeed(rngState);
	return found;
}

/** The local player's gear, life and mana put aside while the disguise is worn. */
struct RealGear {
	std::array<Item, NUM_INVLOC> body;
	std::array<Item, InventoryGridCells> inventory;
	std::array<Item, MaxBeltItems> belt;
	int32_t hpBase;
	int32_t maxHPBase;
	int32_t manaBase;
	int32_t maxManaBase;
};

/** Far above any real life or mana, so that worn in a disguise the player never comes out at 0 life (and dies). */
constexpr int32_t Headroom = 1 << 24;

/**
 * Puts on the disguised items and works out the player's stats with them (CalcPlrInv). Life and mana are raised by
 * Headroom meanwhile; what the items give is then the maximum less the raised base.
 */
RealGear PutOnDisguise(Player &player)
{
	RealGear real;
	std::copy(std::begin(player.InvBody), std::end(player.InvBody), real.body.begin());
	std::copy(std::begin(player.InvList), std::end(player.InvList), real.inventory.begin());
	std::copy(std::begin(player.SpdList), std::end(player.SpdList), real.belt.begin());
	real.hpBase = player._pHPBase;
	real.maxHPBase = player._pMaxHPBase;
	real.manaBase = player._pManaBase;
	real.maxManaBase = player._pMaxManaBase;

	for (Item &item : player.InvBody)
		item = DisguisedItem(item);
	for (Item &item : player.InvList)
		item = DisguisedItem(item);
	for (Item &item : player.SpdList)
		item = DisguisedItem(item);
	player._pMaxHPBase += Headroom;
	player._pHPBase = player._pMaxHPBase;
	player._pMaxManaBase += Headroom;
	player._pManaBase = player._pMaxManaBase;
	CalcPlrInv(player, false);
	return real;
}

void TakeOffDisguise(Player &player, const RealGear &real)
{
	std::copy(real.body.begin(), real.body.end(), std::begin(player.InvBody));
	std::copy(real.inventory.begin(), real.inventory.end(), std::begin(player.InvList));
	std::copy(real.belt.begin(), real.belt.end(), std::begin(player.SpdList));
	player._pHPBase = real.hpBase;
	player._pMaxHPBase = real.maxHPBase;
	player._pManaBase = real.manaBase;
	player._pMaxManaBase = real.maxManaBase;
	CalcPlrInv(player, false);
}

/** What the items give in life and mana: the maximum less the base. */
struct ItemLifeAndMana {
	int32_t life;
	int32_t mana;
};

ItemLifeAndMana ItemsGive(const Player &player)
{
	return { player._pMaxHP - player._pMaxHPBase, player._pMaxMana - player._pMaxManaBase };
}

/** How much more life and mana the real items give than the disguised ones, for the gear last worked out. */
struct {
	bool known = false;
	uint32_t gear = 0;
	int32_t life = 0;
	int32_t mana = 0;
} Shortfall;

/** Anything that changes what the items give: the equipped items, the character's level and base stats. */
uint32_t GearSignature(const Player &player)
{
	uint32_t hash = 2166136261U;
	const auto mix = [&hash](uint32_t value) { hash = (hash ^ value) * 16777619U; };
	for (const Item &item : player.InvBody) {
		mix(item._iSeed);
		mix(static_cast<uint32_t>(item.IDidx));
		mix(item._iCreateInfo);
		mix(item._iStatFlag ? 1 : 0);
	}
	mix(player._pLevel);
	mix(player._pBaseStr);
	mix(player._pBaseMag);
	mix(player._pBaseDex);
	mix(player._pBaseVit);
	mix(static_cast<uint32_t>(player._pMaxHPBase));
	mix(static_cast<uint32_t>(player._pMaxManaBase));
	return hash;
}

/** Sends the other players every equipped and carried item again, disguised or not as the disguise now is. */
void ResendGear()
{
	Player &player = *MyPlayer;
	for (int bodyLocation = 0; bodyLocation < NUM_INVLOC; bodyLocation++) {
		if (!player.InvBody[bodyLocation].isEmpty())
			NetSendCmdChItem(false, static_cast<uint8_t>(bodyLocation));
	}
	// By its top-left cell, where the other games put an item down from (CheckInvSwap).
	for (int i = 0; i < player._pNumInv; i++) {
		for (int cell = 0; cell < InventoryGridCells; cell++) {
			if (std::abs(player.InvGrid[cell]) == i + 1) {
				NetSendCmdChInvItem(false, cell);
				break;
			}
		}
	}
	for (int i = 0; i < MaxBeltItems; i++) {
		if (!player.SpdList[i].isEmpty())
			NetSendCmdChBeltItem(false, i);
	}
	Shortfall.known = false;
}

} // namespace

Item DisguisedItem(const Item &item)
{
	if (!DisguiseOn() || !CanDisguise(item))
		return item;
	const auto key = std::make_tuple(item._iSeed, static_cast<int>(item.IDidx), item._iCreateInfo);
	auto found = DisguiseSeeds.find(key);
	if (found == DisguiseSeeds.end())
		found = DisguiseSeeds.emplace(key, FindDisguiseSeed(item)).first;
	if (found->second == item._iSeed)
		return item;

	const uint32_t rngState = GetLCGEngineState();
	Item disguised = RollFromSeed(item, found->second);
	SetRndSeed(rngState);
	disguised._iIdentified = item._iIdentified;
	disguised._iDurability = std::min(item._iDurability, disguised._iMaxDur);
	disguised._iCharges = std::min(item._iCharges, disguised._iMaxCharges);
	return disguised;
}

void PackDisguisedNetPlayer(PlayerNetPack &packed, Player &player)
{
	PackNetPlayer(packed, player);
	if (!DisguiseOn())
		return;

	const RealGear real = PutOnDisguise(player);
	PlayerNetPack disguised {};
	PackNetPlayer(disguised, player);
	const ItemLifeAndMana items = ItemsGive(player);
	TakeOffDisguise(player, real);

	// The life and mana the other games work out from the real bases and the disguised items (CalcPlrItemVals).
	const int32_t maxHP = items.life + real.maxHPBase;
	const int32_t hitPoints = std::min(items.life + real.hpBase, maxHP);
	const int32_t maxMana = items.mana + real.maxManaBase;
	const int32_t mana = std::min(items.mana + real.manaBase, maxMana);
	// Hurt badly enough, the disguise's lower life could leave none, or fail the other games' check on the life base
	// (UnPackNetPlayer); then the real gear goes rather than being dropped from the game.
	if ((player._pHitPoints > 0 && hitPoints <= 0) || real.hpBase < real.maxHPBase - maxHP)
		return;
	disguised.pHPBase = SDL_SwapLE32(real.hpBase);
	disguised.pMaxHPBase = SDL_SwapLE32(real.maxHPBase);
	disguised.pHitPoints = SDL_SwapLE32(hitPoints);
	disguised.pMaxHP = SDL_SwapLE32(maxHP);
	disguised.pManaBase = SDL_SwapLE32(real.manaBase);
	disguised.pMaxManaBase = SDL_SwapLE32(real.maxManaBase);
	disguised.pMana = SDL_SwapLE32(mana);
	disguised.pMaxMana = SDL_SwapLE32(maxMana);
	packed = disguised;
}

void DisguisedLifeAndMana(Player &player, int32_t &hitPoints, int32_t &maxHitPoints, int32_t &mana, int32_t &maxMana)
{
	hitPoints = player._pHitPoints;
	maxHitPoints = player._pMaxHP;
	mana = player._pMana;
	maxMana = player._pMaxMana;
	if (!DisguiseOn())
		return;

	const uint32_t gear = GearSignature(player);
	if (!Shortfall.known || Shortfall.gear != gear) {
		const ItemLifeAndMana realItems = ItemsGive(player);
		const RealGear real = PutOnDisguise(player);
		const ItemLifeAndMana items = ItemsGive(player);
		TakeOffDisguise(player, real);
		Shortfall.known = true;
		Shortfall.gear = gear;
		Shortfall.life = std::max(0, realItems.life - items.life);
		Shortfall.mana = std::max(0, realItems.mana - items.mana);
	}
	maxHitPoints -= Shortfall.life;
	// Still alive, however little life the disguise leaves.
	if (hitPoints > 0)
		hitPoints = std::max(hitPoints - Shortfall.life, std::min(hitPoints, 1 << 6));
	maxMana -= Shortfall.mana;
	mana = std::max(0, mana - Shortfall.mana);
}

std::string TextCmdDisguise(string_view /*parameter*/)
{
	const bool on = !*sgOptions.Gameplay.disguiseGear;
	sgOptions.Gameplay.disguiseGear.SetValue(on);
	SaveOptions();
	if (gbIsMultiplayer && MyPlayer != nullptr)
		ResendGear();
	return on ? std::string(_("Disguise on: the other players see your gear with poor rolls."))
	          : std::string(_("Disguise off: the other players see your real gear."));
}

} // namespace devilution
