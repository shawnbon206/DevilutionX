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
#include "stores.h"
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
	/** The wishlist as given, with names filled in, to show back. */
	std::string text;
	std::string lastShown;
};

std::optional<Wishlist> Wish;

constexpr std::array<const char *, 12> TypeNames = { "ring", "amulet", "sword", "axe", "mace", "bow", "staff", "helm", "shield",
	"light_armor", "medium_armor", "heavy_armor" };

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
 * How good an affix's roll is, 0 to 100, taking the weakest part when it rolls more than one value. Affixes without
 * a roll count as 100. Nothing when a shown number doesn't fit this table entry's ranges.
 */
std::optional<int> RollPercent(const PLStruct &affix, const Item &item, const std::array<std::string, 2> &numbers)
{
	const std::array<int, 3> quantities = RollQuantities(item, numbers);
	int weakest = 100;
	for (const RollRange &range : GetRollRanges(affix)) {
		int percent = (quantities[range.quantity] - range.lowest) * 100 / (range.highest - range.lowest);
		if (range.quantity == 2)
			percent = std::clamp(percent, 0, 100);
		if (percent < 0 || percent > 100)
			return std::nullopt;
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
 * where they fell in the spell's range, before a Plentiful or Bountiful prefix multiplied them, as the roll.
 */
ItemAffix FindSuffix(const Item &item, const ItemAffix &prefix)
{
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
 * The game's name a typed one means: the exact name (any case), or the only name that starts with it. Otherwise an
 * error naming what it could be, or saying there's none.
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
		for (int j = IDI_GOLD; j <= IDI_LAST; j++)
			names.push_back(AsciiStrToLower(AllItemsList[j].iName));
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
		if (token.rfind("--", 0) == 0) {
			option = AsciiStrToLower(token.substr(2));
			if (option == "either") {
				wish.either = true;
				option.clear();
			} else if (!IsAnyOf(option, "type", "base", "prefix", "suffix", "min-roll")) {
				error = fmt::format(fmt::runtime(_("Unknown option {:s}. Use --type, --base, --prefix, --suffix, --either, --min-roll.")), token);
				return std::nullopt;
			}
			continue;
		}
		if (option.empty()) {
			error = fmt::format(fmt::runtime(_("\"{:s}\" needs an option before it, such as --prefix.")), token);
			return std::nullopt;
		}
		if (option == "min-roll") {
			const int roll = std::atoi(token.c_str());
			if (token.empty() || !std::all_of(token.begin(), token.end(), [](char c) { return c >= '0' && c <= '9'; }) || roll > 100) {
				error = std::string(_("--min-roll takes a percentage from 0 to 100."));
				return std::nullopt;
			}
			wish.minRoll = roll;
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
	return wish;
}

/** Has Wirt roll a new item now if you're in town, or when you next come to town. */
void RerollWirt()
{
	boyitem = {};
	if (leveltype != DTYPE_TOWN || MyPlayer == nullptr)
		return;
	// A fresh start each time, as the stores get one (SetupTownStores), so asking again doesn't replay the same items.
	const uint32_t rngState = GetLCGEngineState();
	SetRndSeed(SDL_GetTicks());
	SpawnBoy(MyPlayer->_pLevel);
	SetRndSeed(rngState);
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

/** Whether Wirt sells this type of item at all: never staves (in Diablo), and in multiplayer no rings or amulets. */
bool WirtSells(string_view type)
{
	if (type == "staff")
		return false;
	if (gbIsMultiplayer && IsAnyOf(type, "ring", "amulet"))
		return false;
	return true;
}

/**
 * Why Wirt could never roll an item on this wishlist for you, or nothing if he can. He sells no staves, and no rings or
 * amulets in multiplayer; he only rolls affixes whose level is from your character level to twice it (SpawnBoy), and
 * only ones that aren't bad (onlygood).
 */
std::string WhyImpossible(const Wishlist &wish)
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
		if (!type.empty() && WirtSells(type))
			sold.push_back(type);
	}
	if (sold.empty())
		return gbIsMultiplayer ? std::string(_("Wirt doesn't sell those: no staves, and no rings or amulets in multiplayer.")) : std::string(_("Wirt doesn't sell staves."));
	AffixItemType kinds = AffixItemType::None;
	for (const std::string &type : sold)
		kinds |= AffixKindsFor(type);

	const int level = MyPlayer->_pLevel;
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
				} else if (affix.PLMinLvl < level || affix.PLMinLvl > 2 * level) {
					why = fmt::format(fmt::runtime(_("{:s} is a level {:d} affix: Wirt rolls it at character levels {:d}-{:d}, you're {:d}.")),
					    affix.PLName, affix.PLMinLvl, (affix.PLMinLvl + 1) / 2, affix.PLMinLvl, level);
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
	if (wish.either && !wish.prefixes.empty() && !wish.suffixes.empty())
		return !prefixReason.empty() && !suffixReason.empty() ? prefixReason : "";
	return !prefixReason.empty() ? prefixReason : suffixReason;
}

} // namespace

bool WirtWishlistActive()
{
	return Wish.has_value();
}

bool WirtWishlistMatches(const Item &item)
{
	if (!Wish)
		return true;
	if (!Wish->types.empty() && std::find(Wish->types.begin(), Wish->types.end(), ItemTypeName(item._itype)) == Wish->types.end())
		return false;
	if (!Wish->bases.empty() && std::find(Wish->bases.begin(), Wish->bases.end(), AsciiStrToLower(AllItemsList[item.IDidx].iName)) == Wish->bases.end())
		return false;
	if (Wish->prefixes.empty() && Wish->suffixes.empty())
		return true;
	const ItemAffix prefix = FindAffix(ItemPrefixes, item._iPrePower, true, item);
	const ItemAffix suffix = FindSuffix(item, prefix);
	const bool hasPrefix = AffixWanted(Wish->prefixes, prefix, Wish->minRoll);
	const bool hasSuffix = AffixWanted(Wish->suffixes, suffix, Wish->minRoll);
	if (!Wish->prefixes.empty() && !Wish->suffixes.empty())
		return Wish->either ? (hasPrefix || hasSuffix) : (hasPrefix && hasSuffix);
	return Wish->prefixes.empty() ? hasSuffix : hasPrefix;
}

void ReportWirtWishlist(const Item &item, bool found, int tries, int tooDear)
{
	if (found) {
		EventPlrMsg(fmt::format(fmt::runtime(_("Wirt found {:s} after {:d} tries.")), item._iIName, tries + 1));
	} else if (tooDear > 0) {
		EventPlrMsg(fmt::format(fmt::runtime(_("Wirt found nothing in {:d} tries: the {:d} he rolled that fit cost over {:d} gold, more than he may sell.")),
		    WirtWishlistTries, tooDear, MaxBoyValue));
	} else {
		EventPlrMsg(fmt::format(fmt::runtime(_("Wirt found nothing on your wishlist in {:d} tries.")), WirtWishlistTries));
	}
}

std::string TextCmdWirt(string_view parameter)
{
	if (parameter.empty()) {
		if (!Wish)
			return std::string(_("No Wirt wishlist. Use /wirt --type ... --prefix ... --suffix ... --min-roll N, or /wirt off."));
		return fmt::format(fmt::runtime(_("Wirt is looking for:{:s}")), Wish->text);
	}
	if (AsciiStrToLower(parameter) == "off") {
		Wish = std::nullopt;
		return std::string(_("Wirt's wishlist cleared."));
	}
	std::string error;
	std::optional<Wishlist> wish = ParseWishlist(parameter, error);
	if (!wish)
		return error;
	if (const std::string reason = WhyImpossible(*wish); !reason.empty())
		return reason;
	Wish = std::move(wish);
	RerollWirt();
	if (leveltype != DTYPE_TOWN)
		return fmt::format(fmt::runtime(_("Wirt will look for:{:s} (when you're next in town).")), Wish->text);
	return fmt::format(fmt::runtime(_("Wirt is looking for:{:s}")), Wish->text);
}

} // namespace devilution
