/**
 * @file wirt_wishlist.cpp
 *
 * /wirt: Wirt rerolls his item until it fits a wishlist, written like the seed search's (drops.ps1), with the
 * same meaning for names, minimums and --min-roll.
 */
#include "qol/wirt_wishlist.h"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <limits>
#include <map>
#include <optional>
#include <vector>

#include <SDL.h>
#include <fmt/format.h>

#include "engine/random.hpp"
#include "itemdat.h"
#include "items.h"
#include "levels/gendung.h"
#include "multi.h"
#include "player.h"
#include "plrmsg.h"
#include "spelldat.h"
#include "spells.h"
#include "stores.h"
#include "utils/format_int.hpp"
#include "utils/language.h"
#include "utils/str_case.hpp"
#include "utils/str_cat.hpp"

namespace devilution {

namespace {

/** A wanted prefix or suffix: its name (lower case) and, optionally, the least its first shown number may be. */
struct WantedAffix {
	std::string name;
	std::optional<int> minimum;
};

struct Wishlist {
	std::vector<std::string> types;
	std::vector<std::string> bases;
	std::vector<WantedAffix> prefixes;
	std::vector<WantedAffix> suffixes;
	bool either = false;
	std::optional<int> minRoll;
	/** --ac: the least base armor an armor, helm or shield has to have rolled. */
	std::optional<int> minAc;
	/** The wishlist as given, with names filled in, to show back. */
	std::string text;
	std::string lastShown;
};

std::optional<Wishlist> Wish;
/** The level Wirt's hunt rolls at, when your own can't roll the wishlist: the nearest one that can. */
std::optional<int> RolledAt;

constexpr std::array<const char *, 13> TypeNames = { "ring", "amulet", "sword", "axe", "mace", "bow", "staff", "helm", "shield",
	"light_armor", "medium_armor", "heavy_armor", "book" };

string_view ItemTypeName(ItemType type)
{
	switch (type) {
	case ItemType::Sword: return "sword";
	case ItemType::Axe: return "axe";
	case ItemType::Bow: return "bow";
	case ItemType::Mace: return "mace";
	case ItemType::Shield: return "shield";
	case ItemType::LightArmor: return "light_armor";
	case ItemType::Helm: return "helm";
	case ItemType::MediumArmor: return "medium_armor";
	case ItemType::HeavyArmor: return "heavy_armor";
	case ItemType::Staff: return "staff";
	case ItemType::Ring: return "ring";
	case ItemType::Amulet: return "amulet";
	default: return "";
	}
}

/** An item's type name, with spell books as "book". */
std::string KindName(const ItemData &data)
{
	return data.iMiscId == IMISC_BOOK ? "book" : std::string(ItemTypeName(data.itype));
}

// The roll of an affix, the same way the seed search works it out (test/drop_stats_test.cpp).

/** The first two numbers of an affix as the item panel shows it, e.g. "to hit: +20%, +150% damage" -> 20, 150. */
std::array<std::string, 2> PowerNumbers(string_view text)
{
	std::array<std::string, 2> numbers;
	size_t found = 0;
	for (size_t i = 0; i < text.size() && found < numbers.size(); i++) {
		if (text[i] < '0' || text[i] > '9')
			continue;
		size_t start = i;
		if (start > 0 && text[start - 1] == '-' && (start == 1 || text[start - 2] == ' ' || text[start - 2] == ':'))
			start--;
		while (i < text.size() && text[i] >= '0' && text[i] <= '9')
			i++;
		numbers[found++] = std::string(text.substr(start, i - start));
	}
	return numbers;
}

/**
 * The quantities an affix's roll can show up in: the first or second number the item panel shows for it, and the
 * durability change, which isn't shown and is a percentage of the base item's durability.
 */
std::array<int, 3> RollQuantities(const Item &item, const std::array<std::string, 2> &numbers)
{
	std::array<int, 3> quantities {};
	for (size_t i = 0; i < numbers.size(); i++)
		quantities[i] = numbers[i].empty() ? 0 : std::abs(std::atoi(numbers[i].c_str()));
	if (item.IDidx == IDI_NONE)
		return quantities;
	const int base = AllItemsList[item.IDidx].iDurability;
	if (base != 0 && item._iMaxDur != DUR_INDESTRUCTIBLE)
		quantities[2] = std::abs(item._iMaxDur - base) * 100 / base;
	return quantities;
}

struct RollRange {
	size_t quantity;
	int lowest;
	int highest;
};

/**
 * How an affix's roll shows on an item, learned from the game: apply the affix with SaveItemPower under many seeds
 * and see which quantities change and between which values. One that changes nothing has no roll.
 */
std::vector<RollRange> MeasureRollRanges(const PLStruct &affix)
{
	static const _item_indexes Weapon = []() {
		for (int i = IDI_GOLD; i <= IDI_LAST; i++) {
			if (AllItemsList[i].itype == ItemType::Sword && AllItemsList[i].iDurability > 0)
				return static_cast<_item_indexes>(i);
		}
		return IDI_NONE;
	}();
	std::vector<RollRange> ranges;
	if (Weapon == IDI_NONE)
		return ranges;

	const uint32_t rngState = GetLCGEngineState();
	std::array<int, 3> lowest;
	std::array<int, 3> highest;
	lowest.fill(std::numeric_limits<int>::max());
	highest.fill(std::numeric_limits<int>::min());
	for (uint32_t seed = 0; seed < 3000; seed++) {
		Item item {};
		InitializeItem(item, Weapon);
		ItemPower power = affix.power;
		SetRndSeed(seed);
		SaveItemPower(*MyPlayer, item, power);
		const std::array<int, 3> quantities = RollQuantities(item, PowerNumbers(PrintItemPower(affix.power.type, item).str()));
		for (size_t i = 0; i < quantities.size(); i++) {
			lowest[i] = std::min(lowest[i], quantities[i]);
			highest[i] = std::max(highest[i], quantities[i]);
		}
	}
	SetRndSeed(rngState);

	for (size_t i = 0; i < lowest.size(); i++) {
		if (lowest[i] != highest[i])
			ranges.push_back({ i, lowest[i], highest[i] });
	}
	return ranges;
}

const std::vector<RollRange> &GetRollRanges(const PLStruct &affix)
{
	static std::map<const PLStruct *, std::vector<RollRange>> ranges;
	auto found = ranges.find(&affix);
	if (found == ranges.end())
		found = ranges.emplace(&affix, MeasureRollRanges(affix)).first;
	return found->second;
}

/**
 * How good an affix's roll is, 0 to 100; of more than one number, the one rolled from its own table range: King's
 * damage (151-175), not the to-hit that comes with it (76-100), which also leaves the price alone. Affixes without a
 * roll count as 100. Nothing when a shown number doesn't fit this table entry's ranges.
 */
std::optional<int> RollPercent(const PLStruct &affix, const Item &item, const std::array<std::string, 2> &numbers)
{
	const std::array<int, 3> quantities = RollQuantities(item, numbers);
	const std::vector<RollRange> &ranges = GetRollRanges(affix);
	const auto own = std::find_if(ranges.begin(), ranges.end(), [&affix](const RollRange &range) {
		return range.lowest == std::abs(affix.power.param1) && range.highest == std::abs(affix.power.param2);
	});
	int weakest = 100;
	for (const RollRange &range : ranges) {
		int percent = (quantities[range.quantity] - range.lowest) * 100 / (range.highest - range.lowest);
		if (range.quantity == 2)
			percent = std::clamp(percent, 0, 100);
		// Every number still has to be in its range: that tells apart table entries with the same name.
		if (percent < 0 || percent > 100)
			return std::nullopt;
		if (own == ranges.end() || &range == &*own)
			weakest = std::min(weakest, percent);
	}
	return weakest;
}

/** What an item has in a prefix or suffix place: the affix's name, its first shown number and its roll. */
struct ItemAffix {
	const PLStruct *affix = nullptr;
	std::string name;
	std::optional<int> value;
	std::optional<int> roll;
};

/** Items don't keep which table entry they rolled; some names appear twice with different ranges, so the entry the shown value fits wins. */
ItemAffix FindAffix(const PLStruct *table, item_effect_type power, bool isPrefix, const Item &item)
{
	ItemAffix found;
	if (power == IPL_INVALID)
		return found;
	const std::array<std::string, 2> numbers = PowerNumbers(PrintItemPower(power, item).str());
	const string_view name = item._iIName;
	for (int j = 0; table[j].power.type != IPL_INVALID; j++) {
		const PLStruct &affix = table[j];
		if (affix.power.type != power)
			continue;
		const std::string part = isPrefix ? StrCat(affix.PLName, " ") : StrCat(" of ", affix.PLName);
		if (name.size() <= part.size())
			continue;
		if ((isPrefix ? name.substr(0, part.size()) : name.substr(name.size() - part.size())) != part)
			continue;
		const std::optional<int> roll = RollPercent(affix, item, numbers);
		if (found.affix == nullptr || (roll && !found.roll)) {
			found.affix = &affix;
			found.roll = roll;
		}
	}
	if (found.affix != nullptr) {
		found.name = AsciiStrToLower(found.affix->PLName);
		if (!numbers[0].empty())
			found.value = std::atoi(numbers[0].c_str());
	}
	return found;
}

/**
 * The suffix place: a staff's spell takes it (such a staff never has a suffix), with its charges as the value and
 * where they fell in the spell's range, before a Plentiful or Bountiful prefix multiplied them, as the roll. A book's
 * spell stands there too ("Book of Teleport"), with nothing that rolls.
 */
ItemAffix FindSuffix(const Item &item, const ItemAffix &prefix)
{
	if (item._iMiscId == IMISC_BOOK) {
		ItemAffix book;
		if (item._iSpell != SpellID::Null) {
			book.name = AsciiStrToLower(GetSpellData(item._iSpell).sNameText);
			book.roll = 100;
		}
		return book;
	}
	ItemAffix suffix = FindAffix(ItemSuffixes, item._iSufPower, false, item);
	if (suffix.affix != nullptr || item._iMiscId != IMISC_STAFF || item._iSpell == SpellID::Null)
		return suffix;
	const SpellData &spell = GetSpellData(item._iSpell);
	const int multiplier = prefix.affix != nullptr && prefix.affix->power.type == IPL_CHARGES ? std::max(prefix.affix->power.param1, 1) : 1;
	const int charges = item._iMaxCharges / multiplier;
	const int range = spell.sStaffMax - spell.sStaffMin;
	suffix.name = AsciiStrToLower(spell.sNameText);
	suffix.value = item._iMaxCharges;
	suffix.roll = range > 0 ? std::clamp((charges - spell.sStaffMin) * 100 / range, 0, 100) : 100;
	return suffix;
}

bool AffixWanted(const std::vector<WantedAffix> &wanted, const ItemAffix &affix, std::optional<int> minRoll)
{
	if (affix.name.empty())
		return false;
	for (const WantedAffix &want : wanted) {
		if (want.name != affix.name)
			continue;
		if (want.minimum && (!affix.value || *affix.value < *want.minimum))
			continue;
		if (minRoll && (!affix.roll || *affix.roll < *minRoll))
			continue;
		return true;
	}
	return false;
}

// Reading a wishlist.

/** Splits on spaces, keeping "quoted names" whole. */
std::vector<std::string> Tokens(string_view text)
{
	std::vector<std::string> tokens;
	std::string current;
	bool quoted = false;
	bool any = false;
	for (char c : text) {
		if (c == '"') {
			quoted = !quoted;
			any = true;
		} else if (c == ' ' && !quoted) {
			if (any)
				tokens.push_back(current);
			current.clear();
			any = false;
		} else {
			current += c;
			any = true;
		}
	}
	if (any)
		tokens.push_back(current);
	return tokens;
}

/** "Obsidian:38" -> ("Obsidian", 38). */
std::pair<std::string, std::optional<int>> SplitMinimum(const std::string &spec)
{
	const size_t colon = spec.rfind(':');
	if (colon != std::string::npos && colon + 1 < spec.size() && std::all_of(spec.begin() + colon + 1, spec.end(), [](char c) { return c >= '0' && c <= '9'; }))
		return { spec.substr(0, colon), std::atoi(spec.c_str() + colon + 1) };
	return { spec, std::nullopt };
}

/**
 * The game's name a typed one means: the exact name (any case), or the only name that starts with it, or failing
 * that, the only one with a word that does ("staff" for the staves). Otherwise an error naming what it could be, or
 * saying there's none.
 */
std::optional<std::string> Resolve(const std::string &typed, const std::vector<std::string> &known, string_view kind, std::string &error)
{
	std::string name = AsciiStrToLower(typed);
	if (kind == "suffix" && name.rfind("of ", 0) == 0)
		name = name.substr(3);
	std::vector<std::string> starting;
	for (const std::string &candidate : known) {
		if (candidate == name)
			return candidate;
		const bool starts = candidate.rfind(name, 0) == 0 || candidate.rfind(StrCat("the ", name), 0) == 0;
		if (starts && std::find(starting.begin(), starting.end(), candidate) == starting.end())
			starting.push_back(candidate);
	}
	if (starting.empty()) {
		for (const std::string &candidate : known) {
			if (StrCat(" ", candidate).find(StrCat(" ", name)) != std::string::npos && std::find(starting.begin(), starting.end(), candidate) == starting.end())
				starting.push_back(candidate);
		}
	}
	if (starting.size() == 1)
		return starting[0];
	if (starting.empty()) {
		error = fmt::format(fmt::runtime(_("There is no {:s} called \"{:s}\".")), kind, typed);
	} else {
		std::string names;
		for (size_t i = 0; i < starting.size() && i < 6; i++)
			names += (i == 0 ? "" : ", ") + starting[i];
		error = fmt::format(fmt::runtime(_("{:s} \"{:s}\" could be {:s}{:s}")), kind, typed, names, starting.size() > 6 ? "..." : "");
	}
	return std::nullopt;
}

std::vector<std::string> KnownNames(string_view kind)
{
	std::vector<std::string> names;
	if (kind == "prefix") {
		for (int j = 0; ItemPrefixes[j].power.type != IPL_INVALID; j++)
			names.push_back(AsciiStrToLower(ItemPrefixes[j].PLName));
	} else if (kind == "suffix") {
		for (int j = 0; ItemSuffixes[j].power.type != IPL_INVALID; j++)
			names.push_back(AsciiStrToLower(ItemSuffixes[j].PLName));
		for (int8_t j = static_cast<int8_t>(SpellID::Firebolt); j <= static_cast<int8_t>(SpellID::LAST); j++)
			names.push_back(AsciiStrToLower(GetSpellData(static_cast<SpellID>(j)).sNameText));
	} else if (kind == "base") {
		// Not quest items (the Staff of Lazarus): no vendor sells them.
		for (int j = IDI_GOLD; j <= IDI_LAST; j++) {
			if (AllItemsList[j].iRnd != IDROP_NEVER)
				names.push_back(AsciiStrToLower(AllItemsList[j].iName));
		}
	} else if (kind == "type") {
		for (const char *type : TypeNames)
			names.emplace_back(type);
	}
	return names;
}

/** Reads a wishlist, or says what's wrong with it. */
std::optional<Wishlist> ParseWishlist(string_view text, std::string &error)
{
	Wishlist wish;
	std::string option;
	for (const std::string &token : Tokens(text)) {
		// Options, long or short to save chat space: --type -t, --base -b, --prefix -p, --suffix -s, --either -e, --min-roll -m, --ac -a.
		const bool isShort = token.size() == 2 && token[0] == '-' && token[1] != '-';
		if (token.rfind("--", 0) == 0 || isShort) {
			option = AsciiStrToLower(token.substr(isShort ? 1 : 2));
			if (isShort) {
				constexpr std::array<std::pair<char, const char *>, 7> Shorts = { { { 't', "type" }, { 'b', "base" }, { 'p', "prefix" }, { 's', "suffix" }, { 'e', "either" }, { 'm', "min-roll" }, { 'a', "ac" } } };
				const auto found = std::find_if(Shorts.begin(), Shorts.end(), [&option](const auto &entry) { return option[0] == entry.first; });
				option = found != Shorts.end() ? found->second : option;
			}
			if (option == "either") {
				wish.either = true;
				option.clear();
			} else if (!IsAnyOf(option, "type", "base", "prefix", "suffix", "min-roll", "ac")) {
				error = fmt::format(fmt::runtime(_("Unknown option {:s}. Use -t, -b, -p, -s, -e, -m, -a (or --type, --base, --prefix, --suffix, --either, --min-roll, --ac).")), token);
				return std::nullopt;
			}
			continue;
		}
		if (option.empty()) {
			error = fmt::format(fmt::runtime(_("\"{:s}\" needs an option before it, such as --prefix.")), token);
			return std::nullopt;
		}
		if (IsAnyOf(option, "min-roll", "ac")) {
			const bool isNumber = !token.empty() && std::all_of(token.begin(), token.end(), [](char c) { return c >= '0' && c <= '9'; });
			const int number = std::atoi(token.c_str());
			if (option == "min-roll") {
				if (!isNumber || number > 100) {
					error = std::string(_("--min-roll takes a percentage from 0 to 100."));
					return std::nullopt;
				}
				wish.minRoll = number;
			} else {
				if (!isNumber) {
					error = std::string(_("--ac takes the least base armor, such as 20."));
					return std::nullopt;
				}
				wish.minAc = number;
			}
			option.clear();
			continue;
		}
		const auto [typed, minimum] = IsAnyOf(option, "prefix", "suffix") ? SplitMinimum(token) : std::make_pair(token, std::optional<int> {});
		const std::optional<std::string> name = Resolve(typed, KnownNames(option), option, error);
		if (!name)
			return std::nullopt;
		if (option == "type")
			wish.types.push_back(*name);
		else if (option == "base")
			wish.bases.push_back(*name);
		else if (option == "prefix")
			wish.prefixes.push_back({ *name, minimum });
		else
			wish.suffixes.push_back({ *name, minimum });
		const std::string shown = StrCat(*name, minimum ? fmt::format(":{:d}", *minimum) : "");
		if (wish.lastShown == option)
			wish.text += StrCat(" ", shown);
		else
			wish.text += StrCat(wish.text.empty() ? " " : ", ", option, " ", shown);
		wish.lastShown = option;
	}
	if (wish.types.empty() && wish.bases.empty() && wish.prefixes.empty() && wish.suffixes.empty()) {
		error = std::string(_("Give at least one of --type, --base, --prefix, --suffix."));
		return std::nullopt;
	}
	if (wish.either)
		wish.text += ", either";
	if (wish.minRoll)
		wish.text += fmt::format(", min-roll {:d}", *wish.minRoll);
	if (wish.minAc)
		wish.text += fmt::format(", base armor {:d}+", *wish.minAc);
	return wish;
}

/**
 * Has Wirt roll a new item now, as for a character of this level; he keeps it until you've gained two levels or bought
 * it, wherever you are.
 */
void RerollWirt(int level)
{
	boyitem = {};
	if (MyPlayer == nullptr)
		return;
	// A fresh start each time, as the stores get one (SetupTownStores), so asking again doesn't replay the same items.
	const uint32_t rngState = GetLCGEngineState();
	SetRndSeed(SDL_GetTicks());
	SpawnBoy(level);
	SetRndSeed(rngState);
	// SpawnBoy marks the item as rolled for that level; it's yours, so he keeps it as long as one rolled at your own.
	boylevel = MyPlayer->_pLevel / 2;
}

/** The affix item kinds an item type takes affixes for. */
AffixItemType AffixKindsFor(string_view type)
{
	if (IsAnyOf(type, "ring", "amulet"))
		return AffixItemType::Misc;
	if (IsAnyOf(type, "sword", "axe", "mace"))
		return AffixItemType::Weapon;
	if (type == "bow")
		return AffixItemType::Bow;
	if (type == "staff")
		return AffixItemType::Staff;
	if (type == "shield")
		return AffixItemType::Shield;
	return AffixItemType::Armor;
}

/** Whether Wirt sells this type of item at all: never staves (in Diablo) or books, and in multiplayer no rings or amulets. */
bool WirtSells(string_view type)
{
	if (IsAnyOf(type, "staff", "book"))
		return false;
	if (gbIsMultiplayer && IsAnyOf(type, "ring", "amulet"))
		return false;
	return true;
}

/**
 * Whether Wirt rolls an affix of this level for a character of this level: from the character level, but never above
 * 25 (GetItemBonus caps the lowest affix level there), to twice the character level (SpawnBoy).
 */
bool WirtAffixLevelFits(int affixLevel, int level)
{
	return affixLevel >= std::min(level, 25) && affixLevel <= 2 * level;
}

/**
 * The least an affix table entry adds to an item's price when its value is the lowest the wishlist allows: the game
 * prices an affix by where the value its power rolled sits in the power's range (PLVal in SaveItemAffix), the same
 * whole percentage --min-roll is judged by. (King's prices only its damage; its to-hit is free.)
 */
int CheapestAffixPrice(const PLStruct &affix, std::optional<int> minRoll, std::optional<int> minimum)
{
	const int p1 = affix.power.param1;
	const int p2 = affix.power.param2;
	if (p1 >= p2 || affix.minVal == affix.maxVal)
		return affix.minVal;
	for (int value = p1; value <= p2; value++) {
		const int percent = 100 * (value - p1) / (p2 - p1);
		if ((minRoll && percent < *minRoll) || (minimum && value < *minimum))
			continue;
		return affix.minVal + (affix.maxVal - affix.minVal) * percent / 100;
	}
	return affix.maxVal;
}

/**
 * The cheapest an item on the wishlist can cost on each base Wirt can roll for you: the price is the affixes' prices plus
 * their multipliers times the base item's value (CalcItemValue), so it's each wanted affix at the lowest value allowed.
 * Bases the wanted affixes can't go on are left out; a name the game has more than once (Ring) is listed once. A level
 * of -1 prices them as at a level where Wirt rolls the affixes, to see ahead.
 */
std::vector<std::pair<std::string, int>> BasePrices(const Wishlist &wish, const std::vector<std::string> &sold, int level)
{
	// The least a place adds for a base: its cheapest wanted entry Wirt can roll on it; nothing when none can.
	const auto slotPrice = [&](const std::vector<WantedAffix> &wanted, const PLStruct *table, AffixItemType kinds, int baseValue) -> std::optional<int> {
		std::optional<int> best;
		for (const WantedAffix &want : wanted) {
			for (int j = 0; table[j].power.type != IPL_INVALID; j++) {
				const PLStruct &affix = table[j];
				if (AsciiStrToLower(affix.PLName) != want.name || !HasAnyOf(affix.PLIType, kinds) || !affix.PLOk
				    || (level >= 0 && !WirtAffixLevelFits(affix.PLMinLvl, level)))
					continue;
				const int price = CheapestAffixPrice(affix, wish.minRoll, want.minimum) + std::max(affix.multVal, 0) * baseValue;
				if (!best || price < *best)
					best = price;
			}
		}
		return best;
	};

	std::vector<std::pair<std::string, int>> prices;
	for (int j = IDI_GOLD; j <= IDI_LAST; j++) {
		const ItemData &base = AllItemsList[j];
		const std::string type(ItemTypeName(base.itype));
		if (type.empty() || base.iRnd == IDROP_NEVER || (level >= 0 && base.iMinMLvl > level) || std::find(sold.begin(), sold.end(), type) == sold.end()
		    || (wish.minAc && base.iMaxAC < *wish.minAc))
			continue;
		if (!wish.bases.empty() && std::find(wish.bases.begin(), wish.bases.end(), AsciiStrToLower(base.iName)) == wish.bases.end())
			continue;
		const AffixItemType kinds = AffixKindsFor(type);
		const std::optional<int> prefix = wish.prefixes.empty() ? std::optional<int>(0) : slotPrice(wish.prefixes, ItemPrefixes, kinds, base.iValue);
		const std::optional<int> suffix = wish.suffixes.empty() ? std::optional<int>(0) : slotPrice(wish.suffixes, ItemSuffixes, kinds, base.iValue);
		std::optional<int> price;
		if (wish.either && !wish.prefixes.empty() && !wish.suffixes.empty()) {
			if (prefix && suffix)
				price = std::min(*prefix, *suffix);
			else
				price = prefix ? prefix : suffix;
		} else if (prefix && suffix) {
			price = *prefix + *suffix;
		}
		if (!price)
			continue;
		const auto same = std::find_if(prices.begin(), prices.end(), [&base](const auto &entry) { return entry.first == base.iName; });
		if (same == prices.end())
			prices.emplace_back(base.iName, *price);
		else
			same->second = std::min(same->second, *price);
	}
	std::sort(prices.begin(), prices.end(), [](const auto &a, const auto &b) { return a.second < b.second; });
	return prices;
}

/** The types on the wishlist (from --type and --base, else every type) that Wirt sells. */
std::vector<std::string> SoldTypes(const Wishlist &wish)
{
	std::vector<std::string> types = wish.types;
	for (const std::string &base : wish.bases) {
		for (int j = IDI_GOLD; j <= IDI_LAST; j++) {
			if (AsciiStrToLower(AllItemsList[j].iName) == base)
				types.emplace_back(ItemTypeName(AllItemsList[j].itype));
		}
	}
	if (types.empty())
		types.assign(TypeNames.begin(), TypeNames.end());
	std::vector<std::string> sold;
	for (const std::string &type : types) {
		if (!type.empty() && WirtSells(type) && std::find(sold.begin(), sold.end(), type) == sold.end())
			sold.push_back(type);
	}
	return sold;
}

constexpr int MaxCharacterLevel = 50;


/**
 * A base's own stats, which is what makes one better than another (not its gold value: a Tower Shield rolls 12-20
 * armor, a Gothic Shield 14-18, and is worth more): "armor 12-20" or "damage 6-20".
 */
std::string BaseStats(const ItemData &base)
{
	if (base.iMaxAC > 0)
		return fmt::format(fmt::runtime(_("armor {:d}-{:d}")), base.iMinAC, base.iMaxAC);
	if (base.iMaxDam > 0)
		return fmt::format(fmt::runtime(_("damage {:d}-{:d}")), base.iMinDam, base.iMaxDam);
	return "";
}

/** The best a base can roll: its most armor or most damage, then its least. */
std::pair<int, int> BaseBest(const ItemData &base)
{
	return base.iMaxAC > 0 ? std::make_pair<int, int>(base.iMaxAC, base.iMinAC) : std::make_pair<int, int>(base.iMaxDam, base.iMinDam);
}

const ItemData *BaseNamed(const std::string &name)
{
	for (int j = IDI_GOLD; j <= IDI_LAST; j++) {
		if (AllItemsList[j].iName == name && AllItemsList[j].iRnd != IDROP_NEVER)
			return &AllItemsList[j];
	}
	return nullptr;
}

/** What Wirt asks for an item worth this much: half again its value (StoreBoy, BoyBuyItem). His limit is on the value. */
int WirtAskingPrice(int value)
{
	return value + value / 2;
}

/** Whether Wirt rolls one of the wanted affixes for a place at a character level: its level is from that level to twice it. */
bool SlotRollable(const std::vector<WantedAffix> &wanted, const PLStruct *table, AffixItemType kinds, int level)
{
	for (const WantedAffix &want : wanted) {
		for (int j = 0; table[j].power.type != IPL_INVALID; j++) {
			const PLStruct &affix = table[j];
			if (AsciiStrToLower(affix.PLName) == want.name && HasAnyOf(affix.PLIType, kinds) && affix.PLOk && WirtAffixLevelFits(affix.PLMinLvl, level))
				return true;
		}
	}
	return false;
}

/**
 * The character levels at which Wirt can roll an item on the wishlist: a base of a wanted type is within the level (its
 * level at most yours), and every wanted affix, or one of them with --either, is within its window.
 */
std::vector<int> WishLevels(const Wishlist &wish, const std::vector<std::string> &sold)
{
	std::vector<int> levels;
	for (int level = 1; level <= MaxCharacterLevel; level++) {
		bool fits = false;
		for (int j = IDI_GOLD; j <= IDI_LAST && !fits; j++) {
			const ItemData &base = AllItemsList[j];
			const std::string type(ItemTypeName(base.itype));
			if (type.empty() || base.iRnd == IDROP_NEVER || base.iMinMLvl > level || std::find(sold.begin(), sold.end(), type) == sold.end()
			    || (wish.minAc && base.iMaxAC < *wish.minAc))
				continue;
			if (!wish.bases.empty() && std::find(wish.bases.begin(), wish.bases.end(), AsciiStrToLower(base.iName)) == wish.bases.end())
				continue;
			const AffixItemType kinds = AffixKindsFor(type);
			const bool prefix = wish.prefixes.empty() || SlotRollable(wish.prefixes, ItemPrefixes, kinds, level);
			const bool suffix = wish.suffixes.empty() || SlotRollable(wish.suffixes, ItemSuffixes, kinds, level);
			if (wish.either && !wish.prefixes.empty() && !wish.suffixes.empty())
				fits = SlotRollable(wish.prefixes, ItemPrefixes, kinds, level) || SlotRollable(wish.suffixes, ItemSuffixes, kinds, level);
			else
				fits = prefix && suffix;
		}
		if (fits)
			levels.push_back(level);
	}
	return levels;
}

/** Levels as ranges: "14-19", or "8-12, 20-24". */
std::string RangesText(const std::vector<int> &levels)
{
	std::string text;
	for (size_t i = 0; i < levels.size();) {
		size_t end = i;
		while (end + 1 < levels.size() && levels[end + 1] == levels[end] + 1)
			end++;
		text += StrCat(text.empty() ? "" : ", ", levels[i], levels[end] != levels[i] ? StrCat("-", levels[end]) : "");
		i = end + 1;
	}
	return text;
}

/** The wanted affixes with their levels: "King's (level 28) + blood (level 19)", alternatives joined with "or". */
std::string AffixLevelsText(const Wishlist &wish, AffixItemType kinds)
{
	const auto slotText = [kinds](const std::vector<WantedAffix> &wanted, const PLStruct *table) {
		std::string text;
		for (const WantedAffix &want : wanted) {
			for (int j = 0; table[j].power.type != IPL_INVALID; j++) {
				const PLStruct &affix = table[j];
				if (AsciiStrToLower(affix.PLName) == want.name && HasAnyOf(affix.PLIType, kinds) && affix.PLOk) {
					text += StrCat(text.empty() ? "" : " or ", affix.PLName, " (level ", affix.PLMinLvl, ")");
					break;
				}
			}
		}
		return text;
	};
	const std::string prefixes = slotText(wish.prefixes, ItemPrefixes);
	const std::string suffixes = slotText(wish.suffixes, ItemSuffixes);
	if (prefixes.empty() || suffixes.empty())
		return prefixes.empty() ? suffixes : prefixes;
	return StrCat(prefixes, wish.either ? " or " : " + ", suffixes);
}

/**
 * Why Wirt could never roll an item on this wishlist for you, or nothing if he can. He sells no staves, and no rings or
 * amulets in multiplayer; he only rolls affixes whose level is from your character level (at most 25) to twice it, and
 * only ones that aren't bad (onlygood).
 */
std::string WhyImpossible(const Wishlist &wish, bool checkPrice = true, bool checkLevel = true, int *huntLevel = nullptr)
{
	const std::vector<std::string> sold = SoldTypes(wish);
	if (sold.empty())
		return gbIsMultiplayer ? std::string(_("Wirt doesn't sell those: no staves, and no rings or amulets in multiplayer.")) : std::string(_("Wirt doesn't sell staves."));
	if (wish.minAc) {
		int most = 0;
		for (int j = IDI_GOLD; j <= IDI_LAST; j++) {
			const ItemData &base = AllItemsList[j];
			const std::string type(ItemTypeName(base.itype));
			if (base.iRnd == IDROP_NEVER || std::find(sold.begin(), sold.end(), type) == sold.end())
				continue;
			if (!wish.bases.empty() && std::find(wish.bases.begin(), wish.bases.end(), AsciiStrToLower(base.iName)) == wish.bases.end())
				continue;
			most = std::max(most, static_cast<int>(base.iMaxAC));
		}
		if (most < *wish.minAc)
			return most == 0 ? std::string(_("--ac is for armor, helms and shields."))
			                 : fmt::format(fmt::runtime(_("No base on this list rolls {:d} armor; the most is {:d}.")), *wish.minAc, most);
	}
	AffixItemType kinds = AffixItemType::None;
	for (const std::string &type : sold)
		kinds |= AffixKindsFor(type);

	int level = MyPlayer->_pLevel;
	// The reason the first wanted affix in a place can't come up, or nothing if one of them can.
	const auto slotReason = [&](const std::vector<WantedAffix> &wanted, const PLStruct *table) -> std::string {
		std::string reason;
		for (const WantedAffix &want : wanted) {
			std::string why = fmt::format(fmt::runtime(_("Wirt doesn't sell staves, so no {:s} spell.")), want.name);
			for (int j = 0; table[j].power.type != IPL_INVALID; j++) {
				const PLStruct &affix = table[j];
				if (AsciiStrToLower(affix.PLName) != want.name)
					continue;
				if (!HasAnyOf(affix.PLIType, kinds)) {
					why = fmt::format(fmt::runtime(_("{:s} can't be on those items.")), affix.PLName);
				} else if (!affix.PLOk) {
					why = fmt::format(fmt::runtime(_("Wirt never sells {:s}: it's a bad affix.")), affix.PLName);
				} else {
					return "";
				}
			}
			if (reason.empty())
				reason = why;
		}
		return reason;
	};
	const std::string prefixReason = wish.prefixes.empty() ? "" : slotReason(wish.prefixes, ItemPrefixes);
	const std::string suffixReason = wish.suffixes.empty() ? "" : slotReason(wish.suffixes, ItemSuffixes);
	if (wish.either && !wish.prefixes.empty() && !wish.suffixes.empty()) {
		if (!prefixReason.empty() && !suffixReason.empty())
			return prefixReason;
	} else if (!prefixReason.empty() || !suffixReason.empty()) {
		return !prefixReason.empty() ? prefixReason : suffixReason;
	}

	if (checkLevel) {
		const std::vector<int> levels = WishLevels(wish, sold);
		if (levels.empty())
			return fmt::format(fmt::runtime(_("{:s}: Wirt never rolls that at any one character level.")), AffixLevelsText(wish, kinds));
		// When yours can't, he rolls as for the nearest level that can: the highest below yours, else the lowest above.
		if (std::find(levels.begin(), levels.end(), level) == levels.end())
			level = levels.front() > level ? levels.front() : *std::prev(std::upper_bound(levels.begin(), levels.end(), level));
	}
	if (huntLevel != nullptr)
		*huntLevel = level;

	if (!checkPrice)
		return "";
	const std::vector<std::pair<std::string, int>> prices = BasePrices(wish, sold, level);
	if (!prices.empty() && prices.front().second > MaxBoyValue) {
		return fmt::format(fmt::runtime(_("The cheapest item on this wishlist would cost {:s} gold; Wirt asks at most {:s}. See /wirt bases.")),
		    FormatInteger(WirtAskingPrice(prices.front().second)), FormatInteger(WirtAskingPrice(MaxBoyValue)));
	}
	return "";
}

/**
 * /wirt bases: each base Wirt could put the wishlist's affixes on, with the cheapest such an item costs, in the chat log.
 * The dearest base still under his limit is the best one a wishlist like this can get.
 */
std::string ListBases(string_view text)
{
	std::string error;
	const std::optional<Wishlist> wish = ParseWishlist(text, error);
	if (!wish)
		return error;
	// Without affixes there's no price to give (it comes from them), so just the bases, best (dearest base) first, with
	// the character level the ones above yours open at.
	if (wish->prefixes.empty() && wish->suffixes.empty()) {
		const std::vector<std::string> sold = SoldTypes(*wish);
		if (sold.empty())
			return gbIsMultiplayer ? std::string(_("Wirt doesn't sell those: no staves, and no rings or amulets in multiplayer.")) : std::string(_("Wirt doesn't sell staves."));
		std::vector<const ItemData *> bases;
		for (int j = IDI_GOLD; j <= IDI_LAST; j++) {
			const ItemData &data = AllItemsList[j];
			const std::string type(ItemTypeName(data.itype));
			if (type.empty() || data.iRnd == IDROP_NEVER || std::find(sold.begin(), sold.end(), type) == sold.end() || (wish->minAc && data.iMaxAC < *wish->minAc))
				continue;
			if (!wish->bases.empty() && std::find(wish->bases.begin(), wish->bases.end(), AsciiStrToLower(data.iName)) == wish->bases.end())
				continue;
			const auto same = std::find_if(bases.begin(), bases.end(), [&data](const ItemData *base) { return std::string_view(base->iName) == data.iName; });
			if (same == bases.end())
				bases.push_back(&data);
			else if (data.iMinMLvl < (*same)->iMinMLvl)
				*same = &data;
		}
		std::sort(bases.begin(), bases.end(), [](const ItemData *a, const ItemData *b) { return BaseBest(*a) > BaseBest(*b); });
		EventPlrMsg(fmt::format(fmt::runtime(_("Wirt's bases for{:s}:")), wish->text));
		for (const ItemData *base : bases)
			EventPlrMsg(fmt::format(fmt::runtime(_("  {:s}  {:s}  level {:d}+")), base->iName, BaseStats(*base), base->iMinMLvl));
		return "";
	}
	// Only what can never happen stops the list; outside the level where Wirt rolls the affixes it's priced as within
	// it, with a note, so you can see ahead.
	if (const std::string reason = WhyImpossible(*wish, false, false); !reason.empty())
		return reason;
	// Each base with the character levels at which Wirt can roll it with the affixes (the base's own level and the
	// affixes' windows); bases that never come with them at one level are left out.
	const std::vector<std::string> sold = SoldTypes(*wish);
	std::vector<std::pair<std::string, int>> under;
	std::vector<std::pair<std::string, int>> over;
	std::vector<std::string> levelsOf;
	for (const auto &entry : BasePrices(*wish, sold, -1)) {
		Wishlist narrowed = *wish;
		narrowed.bases = { AsciiStrToLower(entry.first) };
		const std::vector<int> levels = WishLevels(narrowed, sold);
		if (levels.empty())
			continue;
		(entry.second <= MaxBoyValue ? under : over).push_back(entry);
		if (entry.second <= MaxBoyValue)
			levelsOf.push_back(RangesText(levels));
	}
	if (under.empty() && over.empty())
		return fmt::format(fmt::runtime(_("Wirt never rolls that on any base at one character level: {:s}.")), AffixLevelsText(*wish, AffixItemType::Misc | AffixItemType::Bow | AffixItemType::Weapon | AffixItemType::Shield | AffixItemType::Armor));

	EventPlrMsg(fmt::format(fmt::runtime(_("Wirt's starting prices for{:s}:")), wish->text));
	constexpr size_t Shown = 8;
	for (size_t i = 0; i < under.size() && i < Shown; i++) {
		const size_t index = under.size() - 1 - i;
		const ItemData *base = BaseNamed(under[index].first);
		EventPlrMsg(fmt::format(fmt::runtime(_("  {:s}  {:s}  {:s}  levels {:s}")), under[index].first, base != nullptr ? BaseStats(*base) : "",
		    FormatInteger(WirtAskingPrice(under[index].second)), levelsOf[index]));
	}
	if (under.size() > Shown)
		EventPlrMsg(fmt::format(fmt::runtime(_("  and {:d} cheaper")), under.size() - Shown));
	if (under.empty())
		EventPlrMsg(fmt::format(fmt::runtime(_("  none under {:s}")), FormatInteger(WirtAskingPrice(MaxBoyValue))));
	if (!over.empty()) {
		std::string names;
		for (size_t i = 0; i < over.size() && i < 4; i++)
			names += StrCat(i == 0 ? "" : ", ", over[i].first, " ", FormatInteger(WirtAskingPrice(over[i].second)));
		EventPlrMsg(fmt::format(fmt::runtime(_("  over {:s}: {:s}{:s}")), FormatInteger(WirtAskingPrice(MaxBoyValue)), names, over.size() > 4 ? ", ..." : ""));
	}
	return "";
}

/** Whether an item fits a wishlist: its type and base, its base armor (--ac), and its wanted affixes (a staff's or book's spell as its suffix). */
bool MatchesWish(const Wishlist &wish, const Item &item)
{
	if (item.isEmpty())
		return false;
	if (!wish.types.empty() && std::find(wish.types.begin(), wish.types.end(), KindName(AllItemsList[item.IDidx])) == wish.types.end())
		return false;
	if (!wish.bases.empty() && std::find(wish.bases.begin(), wish.bases.end(), AsciiStrToLower(AllItemsList[item.IDidx].iName)) == wish.bases.end())
		return false;
	// An armor, helm or shield rolls its own armor class (GetItemAttrs), apart from any affix's armor percentage.
	if (wish.minAc && (AllItemsList[item.IDidx].iMaxAC == 0 || item._iAC < *wish.minAc))
		return false;
	if (wish.prefixes.empty() && wish.suffixes.empty())
		return true;
	const ItemAffix prefix = FindAffix(ItemPrefixes, item._iPrePower, true, item);
	const ItemAffix suffix = FindSuffix(item, prefix);
	const bool hasPrefix = AffixWanted(wish.prefixes, prefix, wish.minRoll);
	const bool hasSuffix = AffixWanted(wish.suffixes, suffix, wish.minRoll);
	if (!wish.prefixes.empty() && !wish.suffixes.empty())
		return wish.either ? (hasPrefix || hasSuffix) : (hasPrefix && hasSuffix);
	return wish.prefixes.empty() ? hasSuffix : hasPrefix;
}

// Adria: she sells staves and books among her potions and scrolls, and restocks each time you come to town.

std::optional<Wishlist> AdriaWish;

/** How many items Adria rolls at most in one hunt, over all her restocks and slot rerolls, so it can't hang the game. */
constexpr int AdriaRollBudget = 200000;

/** What's left of the budget in the hunt going on; 0 when none is. */
int AdriaRollsLeft = 0;

/** The kinds the hunt rerolls slots of: those the wishlist names, or that it could fit at the stock level. */
bool AdriaWantsStaff = false;
bool AdriaWantsBook = false;

/** Her stock level, as SetupTownStores works it out: half your character level in multiplayer, your deepest dungeon level in single player, plus 2, from 6 to 16. */
int AdriaStockLevel()
{
	int level = MyPlayer->_pLevel / 2;
	if (!gbIsMultiplayer) {
		level = 0;
		for (int i = 0; i < NUMLEVELS; i++) {
			if (MyPlayer->_pLvlVisited[i])
				level = i;
		}
	}
	return std::clamp(level + 2, 6, 16);
}

std::optional<SpellID> SpellNamed(const std::string &name)
{
	for (int8_t j = static_cast<int8_t>(SpellID::Firebolt); j <= static_cast<int8_t>(SpellID::LAST); j++) {
		if (AsciiStrToLower(GetSpellData(static_cast<SpellID>(j)).sNameText) == name)
			return static_cast<SpellID>(j);
	}
	return std::nullopt;
}

/** Whether one of the wanted affixes is a good one for staves with a level from lowest to highest. */
bool AnyStaffAffix(const std::vector<WantedAffix> &wanted, const PLStruct *table, int lowest, int highest)
{
	for (const WantedAffix &want : wanted) {
		for (int j = 0; table[j].power.type != IPL_INVALID; j++) {
			const PLStruct &affix = table[j];
			if (AsciiStrToLower(affix.PLName) == want.name && HasAnyOf(affix.PLIType, AffixItemType::Staff) && affix.PLOk
			    && affix.PLMinLvl >= lowest && affix.PLMinLvl <= highest)
				return true;
		}
	}
	return false;
}

/** Whether one of the wanted suffixes is a spell whose staff or book level (as asked) is at most the given level. */
bool AnySpell(const std::vector<WantedAffix> &wanted, bool book, int level)
{
	for (const WantedAffix &want : wanted) {
		const std::optional<SpellID> spell = SpellNamed(want.name);
		if (!spell)
			continue;
		const int spellLevel = book ? GetSpellBookLevel(*spell) : GetSpellStaffLevel(*spell);
		if (spellLevel != -1 && spellLevel <= level)
			return true;
	}
	return false;
}

/**
 * Whether Adria can have an item on the wishlist at a stock level: a staff or book base of at most that level; a book's
 * spell of at most that book level; a staff either with a spell of at most that staff level and a prefix of at most twice
 * the level (GetStaffPower), or without a spell and with affixes from the level to twice it (SpawnWitch, GetItemBonus).
 */
bool AdriaCanSell(const Wishlist &wish, int level)
{
	const bool wantsPrefix = !wish.prefixes.empty();
	const bool wantsSuffix = !wish.suffixes.empty();
	const auto fits = [&](bool prefix, bool suffix) {
		if (wantsPrefix && wantsSuffix)
			return wish.either ? (prefix || suffix) : (prefix && suffix);
		return wantsPrefix ? prefix : (wantsSuffix ? suffix : true);
	};
	for (int j = IDI_GOLD; j <= IDI_LAST; j++) {
		const ItemData &base = AllItemsList[j];
		const std::string kind = KindName(base);
		if (!IsAnyOf(kind, "staff", "book") || base.iRnd == IDROP_NEVER || base.iMinMLvl > level)
			continue;
		if (!wish.types.empty() && std::find(wish.types.begin(), wish.types.end(), kind) == wish.types.end())
			continue;
		if (!wish.bases.empty() && std::find(wish.bases.begin(), wish.bases.end(), AsciiStrToLower(base.iName)) == wish.bases.end())
			continue;
		if (kind == "book") {
			if (fits(false, AnySpell(wish.suffixes, true, level)))
				return true;
			continue;
		}
		if (fits(AnyStaffAffix(wish.prefixes, ItemPrefixes, 0, 2 * level), AnySpell(wish.suffixes, false, level)))
			return true;
		if (fits(AnyStaffAffix(wish.prefixes, ItemPrefixes, level, 2 * level), AnyStaffAffix(wish.suffixes, ItemSuffixes, level, 2 * level)))
			return true;
	}
	return false;
}

/** Why Adria could never have an item on this wishlist for you now, or nothing if she can. */
std::string WhyAdriaCant(const Wishlist &wish)
{
	if (!wish.types.empty() && std::none_of(wish.types.begin(), wish.types.end(), [](const std::string &type) { return IsAnyOf(type, "staff", "book"); }))
		return std::string(_("Adria sells staves and books (and potions and scrolls), nothing else on this list."));
	std::vector<int> levels;
	for (int level = 6; level <= 16; level++) {
		if (AdriaCanSell(wish, level))
			levels.push_back(level);
	}
	if (levels.empty()) {
		// Say it's her stock level only when a deeper stock would have it (a Book of Apocalypse); otherwise the items
		// themselves can't be like that (a book has no prefix).
		for (int level = 17; level <= 2 * MaxCharacterLevel; level++) {
			if (AdriaCanSell(wish, level))
				return fmt::format(fmt::runtime(_("Adria never sells that: it needs stock level {:d}, and hers is at most 16.")), level);
		}
		if (!wish.prefixes.empty() && !wish.either && !wish.types.empty() && std::all_of(wish.types.begin(), wish.types.end(), [](const std::string &type) { return type == "book"; }))
			return std::string(_("Adria never sells that: books have no prefixes."));
		return std::string(_("Adria never sells that: no staff or book can be like that."));
	}
	const int level = AdriaStockLevel();
	if (std::find(levels.begin(), levels.end(), level) != levels.end())
		return "";
	return fmt::format(fmt::runtime(_("Adria sells that at stock levels {:s}; her stock is level {:d} ({:s} + 2).")), RangesText(levels), level,
	    gbIsMultiplayer ? _("half your character level") : _("your deepest dungeon level"));
}

/** An item's name for a report, with a staff's charges. */
std::string ItemReport(const Item &item)
{
	if (item._iMiscId == IMISC_STAFF && item._iSpell != SpellID::Null)
		return fmt::format(fmt::runtime(_("{:s} ({:d} charges)")), item._iIName, item._iMaxCharges);
	return item._iIName;
}

} // namespace

bool IsWishlistHuntRunning()
{
	return WirtWishlistActive() || AdriaRollsLeft > 0;
}

uint32_t NextHuntSeed()
{
	// splitmix64, started once from the clock.
	static uint64_t state = SDL_GetPerformanceCounter();
	uint64_t z = (state += 0x9E3779B97F4A7C15ULL);
	z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
	z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
	return static_cast<uint32_t>((z ^ (z >> 31)) & 0x7FFFFFFF);
}

bool WirtWishlistActive()
{
	return Wish.has_value();
}

bool WirtWishlistMatches(const Item &item)
{
	return !Wish || MatchesWish(*Wish, item);
}

bool IsAdriaWishlistSlot(const Item &item)
{
	if (AdriaRollsLeft <= 0 || !AdriaWish)
		return false;
	const std::string kind = KindName(AllItemsList[item.IDidx]);
	return (kind == "staff" && AdriaWantsStaff) || (kind == "book" && AdriaWantsBook);
}

bool RerollAdriaWishlistSlot(const Item &item)
{
	if (AdriaRollsLeft <= 0 || MatchesWish(*AdriaWish, item))
		return false;
	AdriaRollsLeft--;
	return true;
}

void HuntAdria(int lvl)
{
	if (!AdriaWish) {
		SpawnWitch(lvl);
		return;
	}
	// Reroll the slots of a kind the wishlist can be, judged with the wishlist narrowed to that kind.
	const auto wants = [lvl](const char *kind) {
		Wishlist narrowed = *AdriaWish;
		if (!narrowed.types.empty() && std::find(narrowed.types.begin(), narrowed.types.end(), kind) == narrowed.types.end())
			return false;
		narrowed.types = { kind };
		return AdriaCanSell(narrowed, lvl);
	};
	AdriaWantsStaff = wants("staff");
	AdriaWantsBook = wants("book");
	AdriaRollsLeft = AdriaRollBudget;
	// Tries are rolls, as for Wirt: every slot rerolled and every restock, not the restocks alone.
	const auto triesSoFar = []() { return FormatInteger(std::min(AdriaRollBudget, AdriaRollBudget - AdriaRollsLeft)); };
	while (true) {
		// Each restock from a seed of its own, as each roll is (NextHuntSeed).
		SetRndSeed(NextHuntSeed());
		SpawnWitch(lvl);
		// Name everything she has that fits, alike ones together: "Adria has Book of Elemental x2, Book of Blood Star."
		std::vector<std::pair<std::string, int>> found;
		for (const Item &item : witchitem) {
			if (!MatchesWish(*AdriaWish, item))
				continue;
			const std::string name = ItemReport(item);
			const auto same = std::find_if(found.begin(), found.end(), [&name](const auto &entry) { return entry.first == name; });
			if (same == found.end())
				found.emplace_back(name, 1);
			else
				same->second++;
		}
		if (!found.empty()) {
			std::string what;
			for (const auto &[name, count] : found)
				what += StrCat(what.empty() ? "" : ", ", name, count > 1 ? StrCat(" x", count) : "");
			// The restocks happen out of sight, at once; only the stock she ends up with is seen, so they aren't counted out.
			EventPlrMsg(fmt::format(fmt::runtime(_("Adria has {:s}, after {:s} tries.")), what, triesSoFar()));
			break;
		}
		if (AdriaRollsLeft <= 0) {
			EventPlrMsg(fmt::format(fmt::runtime(_("Adria found nothing on your wishlist in {:s} tries.")), triesSoFar()));
			break;
		}
		AdriaRollsLeft -= 9;
	}
	AdriaRollsLeft = 0;
}

namespace {

/**
 * /adria bases: the staff and book bases Adria can stock with the wishlist on them, best first (dearest base; price is
 * no object with her), each with the stock level it first comes at if that's above hers now.
 */
std::string ListAdriaBases(string_view text)
{
	std::string error;
	const std::optional<Wishlist> wish = ParseWishlist(text, error);
	if (!wish)
		return error;
	if (!wish->types.empty() && std::none_of(wish->types.begin(), wish->types.end(), [](const std::string &type) { return IsAnyOf(type, "staff", "book"); }))
		return std::string(_("Adria sells staves and books (and potions and scrolls), nothing else on this list."));
	struct Base {
		std::string name;
		int value;
		int firstLevel;
	};
	std::vector<Base> bases;
	for (int j = IDI_GOLD; j <= IDI_LAST; j++) {
		const ItemData &data = AllItemsList[j];
		const std::string kind = KindName(data);
		if (!IsAnyOf(kind, "staff", "book") || data.iRnd == IDROP_NEVER)
			continue;
		if (std::any_of(bases.begin(), bases.end(), [&](const Base &base) { return base.name == (kind == "book" ? std::string(_("Book")) : std::string(data.iName)); }))
			continue;
		Wishlist narrowed = *wish;
		narrowed.bases = { AsciiStrToLower(data.iName) };
		for (int level = 6; level <= 16; level++) {
			if (AdriaCanSell(narrowed, level)) {
				// A book's base name is "Book of ", which the spell completes.
				bases.push_back({ kind == "book" ? std::string(_("Book")) : std::string(data.iName), data.iValue, level });
				break;
			}
		}
	}
	if (bases.empty())
		return std::string(_("No staff or book Adria stocks can be like that."));
	std::sort(bases.begin(), bases.end(), [](const Base &a, const Base &b) { return a.value > b.value; });
	EventPlrMsg(fmt::format(fmt::runtime(_("Adria's bases for{:s}:")), wish->text));
	for (const Base &base : bases)
		EventPlrMsg(fmt::format(fmt::runtime(_("  {:s}  {:s}  stock level {:d}+")), base.name, FormatInteger(base.value), base.firstLevel));
	return "";
}

} // namespace

std::string TextCmdAdria(string_view parameter)
{
	if (parameter.empty())
		return std::string(_("Use /adria --type staff book --prefix ... --suffix ... --min-roll N, or /adria bases ..."));
	// "/adria bases ..." (or "base") lists the bases instead of hunting.
	for (const string_view word : { string_view("bases"), string_view("base") }) {
		const std::string lower = AsciiStrToLower(parameter);
		if (lower == word || lower.rfind(StrCat(word, " "), 0) == 0)
			return ListAdriaBases(parameter.substr(std::min(parameter.size(), word.size() + 1)));
	}
	std::string error;
	std::optional<Wishlist> wish = ParseWishlist(parameter, error);
	if (!wish)
		return error;
	if (const std::string reason = WhyAdriaCant(*wish); !reason.empty())
		return reason;
	// She restocks each time you come to town, which would undo a hunt done elsewhere.
	if (leveltype != DTYPE_TOWN || MyPlayer == nullptr)
		return std::string(_("Go to town first: Adria restocks when you arrive."));
	// One hunt, now; the wishlist is only kept while it runs. A fresh start, as the stores get one in town.
	AdriaWish = std::move(wish);
	const uint32_t rngState = GetLCGEngineState();
	SetRndSeed(SDL_GetTicks());
	HuntAdria(AdriaStockLevel());
	SetRndSeed(rngState);
	AdriaWish = std::nullopt;
	return "";
}

void ReportWirtWishlist(const Item &item, bool found, int tries, int tooDear)
{
	if (found) {
		EventPlrMsg(fmt::format(fmt::runtime(_("Wirt found {:s} after {:d} tries{:s}.")), item._iIName, tries + 1,
		    RolledAt ? fmt::format(fmt::runtime(_(", rolling as for level {:d}")), *RolledAt) : ""));
	} else if (tooDear > 0) {
		EventPlrMsg(fmt::format(fmt::runtime(_("Wirt found nothing in {:d} tries: the {:d} he rolled that fit would cost over {:s} gold, more than he asks.")),
		    WirtWishlistTries, tooDear, FormatInteger(WirtAskingPrice(MaxBoyValue))));
	} else {
		EventPlrMsg(fmt::format(fmt::runtime(_("Wirt found nothing on your wishlist in {:d} tries.")), WirtWishlistTries));
	}
}

std::string TextCmdWirt(string_view parameter)
{
	if (parameter.empty())
		return std::string(_("Use /wirt --type ... --base ... --prefix ... --suffix ... --min-roll N, or /wirt bases ..."));
	// "/wirt bases ..." (or "base") lists the bases instead of hunting.
	for (const string_view word : { string_view("bases"), string_view("base") }) {
		const std::string lower = AsciiStrToLower(parameter);
		if (lower == word || lower.rfind(StrCat(word, " "), 0) == 0)
			return ListBases(parameter.substr(std::min(parameter.size(), word.size() + 1)));
	}
	std::string error;
	std::optional<Wishlist> wish = ParseWishlist(parameter, error);
	if (!wish)
		return error;
	int level;
	if (const std::string reason = WhyImpossible(*wish, true, true, &level); !reason.empty())
		return reason;
	// One hunt, now; the wishlist is only kept while it runs.
	Wish = std::move(wish);
	if (level != MyPlayer->_pLevel)
		RolledAt = level;
	RerollWirt(level);
	Wish = std::nullopt;
	RolledAt = std::nullopt;
	return "";
}

} // namespace devilution
