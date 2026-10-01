#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <iostream>
#include <map>
#include <optional>
#include <thread>
#include <tuple>
#include <vector>

#include <fmt/format.h>
#include <gtest/gtest.h>

#include "diablo.h"
#include "engine/load_file.hpp"
#include "engine/random.hpp"
#include "init.h"
#include "items.h"
#include "levels/gendung.h"
#include "levels/setmaps.h"
#include "levels/themes.h"
#include "levels/trigs.h"
#include "lighting.h"
#include "loadsave.h"
#include "monster.h"
#include "multi.h"
#include "objects.h"
#include "options.h"
#include "player.h"
#include "quests.h"
#include "spelldat.h"
#include "storm/storm_net.hpp"
#include "utils/paths.h"
#include "utils/str_cat.hpp"

namespace devilution {
namespace {

// Must match "Randomize Quests" in the game creator's diablo.ini, given as DROPSTATS_RANDOMIZE_QUESTS=0 or 1
// (default 1); joiners take the creator's quest states via DeltaSyncJunk.
bool RandomizeQuests()
{
	const char *value = std::getenv("DROPSTATS_RANDOMIZE_QUESTS");
	return value == nullptr || string_view(value) != "0";
}

// "Theo Quest" and "Cow Quest" from the creator's diablo.ini, as DROPSTATS_THEO_QUEST / DROPSTATS_COW_QUEST.
uint8_t QuestSetting(const char *name)
{
	const char *value = std::getenv(name);
	return value != nullptr && string_view(value) == "1" ? 1 : 0;
}

std::string MpqDir()
{
	const char *dir = std::getenv("DROPSTATS_MPQ_DIR");
	if (dir != nullptr)
		return dir;
	const char *appData = std::getenv("APPDATA");
	return std::string(appData != nullptr ? appData : ".") + "/diasurgical/devilution/";
}

void LoadArchivesOnce()
{
	static bool loaded = false;
	if (loaded)
		return;
	paths::SetPrefPath(MpqDir());
	LoadCoreArchives();
	LoadGameArchives();
	// LoadGameArchives turns Hellfire on whenever hellfire.mpq exists; DiabloInit turns it back off for Diablo mode.
	gbIsHellfire = false;
	gbIsHellfireSaveGame = false;
	// Some quest kills send network messages even with sendmsg false; loopback just queues them.
	SNetInitializeProvider(SELCONN_LOOPBACK, &sgGameInitInfo);
	loaded = true;
}

void StartMultiplayerGame(uint32_t gameSeed, _difficulty difficulty)
{
	// Players[1] opens containers in the verification test, standing in for another player.
	Players.resize(2);
	MyPlayer = &Players[0];

	gbIsMultiplayer = true;
	sgGameInitInfo.dwSeed = gameSeed;
	sgGameInitInfo.nDifficulty = difficulty;
	sgGameInitInfo.fullQuests = 1;
	sgOptions.Gameplay.randomizeQuests.SetValue(RandomizeQuests());
	sgGameInitInfo.bTheoQuest = QuestSetting("DROPSTATS_THEO_QUEST");
	sgGameInitInfo.bCowQuest = QuestSetting("DROPSTATS_COW_QUEST");

	// Same order as NetInit then StartGame.
	SetRndSeed(gameSeed);
	for (uint32_t &seed : glSeedTbl)
		seed = AdvanceRndSeed();
	InitLevels();
	InitQuests();
}

struct LevelId {
	uint8_t dlvl;
	_setlevels setLevel = SL_NONE;
};

struct SetLevelQuest {
	_setlevels setLevel;
	quest_id quest;
	const char *name;
};

constexpr SetLevelQuest SetLevelQuests[] = {
	{ SL_SKELKING, Q_SKELKING, "King Leoric's Tomb" },
	{ SL_BONECHAMB, Q_SCHAMB, "Chamber of Bone" },
	{ SL_POISONWATER, Q_PWATER, "Poisoned Water Supply" },
	{ SL_VILEBETRAYER, Q_BETRAYER, "Lazarus' Lair" },
};

const SetLevelQuest &GetSetLevelQuest(_setlevels setLevel)
{
	for (const SetLevelQuest &entry : SetLevelQuests) {
		if (entry.setLevel == setLevel)
			return entry;
	}
	app_fatal("GetSetLevelQuest");
}

std::string LevelName(LevelId level)
{
	if (level.setLevel == SL_NONE)
		return fmt::format("dlvl {}", level.dlvl);
	return GetSetLevelQuest(level.setLevel).name;
}

// Dungeon levels 1-16 plus the quest set levels this game's quest roll made reachable.
std::vector<LevelId> ReachableLevels()
{
	std::vector<LevelId> levels;
	for (uint8_t dlvl = 1; dlvl <= 16; dlvl++)
		levels.push_back({ dlvl });
	for (const SetLevelQuest &entry : SetLevelQuests) {
		const Quest &quest = Quests[entry.quest];
		if (quest._qactive != QUEST_NOTAVAIL)
			levels.push_back({ quest._qlevel, entry.setLevel });
	}
	return levels;
}

void LoadLevelTiles()
{
	switch (leveltype) {
	case DTYPE_CATHEDRAL:
		pMegaTiles = LoadFileInMem<MegaTile>("levels\\l1data\\l1.til");
		break;
	case DTYPE_CATACOMBS:
		pMegaTiles = LoadFileInMem<MegaTile>("levels\\l2data\\l2.til");
		break;
	case DTYPE_CAVES:
		pMegaTiles = LoadFileInMem<MegaTile>("levels\\l3data\\l3.til");
		break;
	case DTYPE_HELL:
		pMegaTiles = LoadFileInMem<MegaTile>("levels\\l4data\\l4.til");
		break;
	default:
		app_fatal("LoadLevelTiles");
	}
}

void InitLevelTriggers()
{
	switch (leveltype) {
	case DTYPE_CATHEDRAL:
		InitL1Triggers();
		break;
	case DTYPE_CATACOMBS:
		InitL2Triggers();
		break;
	case DTYPE_CAVES:
		InitL3Triggers();
		break;
	case DTYPE_HELL:
		InitL4Triggers();
		break;
	default:
		app_fatal("InitLevelTriggers");
	}
	Freeupstairs();
}

// Mirrors the multiplayer first-visit path of LoadGameLevel for dungeon levels 1-16,
// minus graphics, players, sound and network. The RNG reseeds must stay in the same places.
void GenerateDungeonLevel(uint8_t dlvl)
{
	currlevel = dlvl;
	leveltype = GetLevelType(dlvl);
	setlevel = false;
	MyPlayer->setLevel(dlvl);

	LoadLevelTiles();
	InitLighting();
	InitLevelMonsters();

	CreateDungeon(glSeedTbl[currlevel], ENTRY_MAIN);
	InitLevelTriggers();
	LoadLevelSOLData();

	SetRndSeed(glSeedTbl[currlevel]);
	GetLevelMTypes();
	InitThemes();

	SetRndSeed(glSeedTbl[currlevel]);
	HoldThemeRooms();
	InitGolems();
	InitObjects();
	InitMonsters();
	InitItems();
	CreateThemeRooms();
}

// Mirrors the set level path of LoadGameLevel as reached through the quest entrance
// (quests.cpp CheckQuests, interfac.cpp WM_DIABSETLVL).
void GenerateSetLevel(_setlevels setLevel)
{
	Quest &quest = Quests[GetSetLevelQuest(setLevel).quest];

	// The player walks in from the quest's dungeon level, whose SOL data is still loaded while the set map is built.
	currlevel = quest._qlevel;
	leveltype = GetLevelType(currlevel);
	setlevel = false;
	LoadLevelSOLData();

	// Lazarus' portal only opens after the book on dlvl 15 is read.
	if (setLevel == SL_VILEBETRAYER && quest._qactive == QUEST_INIT) {
		quest._qactive = QUEST_ACTIVE;
		quest._qvar1 = 3;
	}

	setlevel = true;
	setlvlnum = setLevel;
	setlvltype = quest._qlvltype;
	leveltype = setlvltype;
	currlevel = static_cast<uint8_t>(setLevel);
	MyPlayer->setLevel(setLevel);

	LoadLevelTiles();
	SetRndSeed(glSeedTbl[static_cast<size_t>(setLevel)]);
	InitLighting();
	InitLevelMonsters();

	LoadSetMap();
	GetLevelMTypes();
	InitGolems();
	InitMonsters();
	LoadLevelSOLData();
	InitItems();
}

void GenerateLevel(LevelId level)
{
	if (level.setLevel == SL_NONE)
		GenerateDungeonLevel(level.dlvl);
	else
		GenerateSetLevel(level.setLevel);
}

enum class SourceKind : uint8_t {
	Monster,
	UniqueMonster,
	Chest,
	Barrel,
	Sarcophagus,
	Rack,
	Other,
};

string_view SourceKindName(SourceKind kind)
{
	switch (kind) {
	case SourceKind::Monster:
		return "monster";
	case SourceKind::UniqueMonster:
		return "unique_monster";
	case SourceKind::Chest:
		return "chest";
	case SourceKind::Barrel:
		return "barrel";
	case SourceKind::Sarcophagus:
		return "sarcophagus";
	case SourceKind::Rack:
		return "rack";
	default:
		return "other";
	}
}

// Items lying on the floor when the level is created have no source object or monster.
constexpr int FloorSource = -1;

struct Drop {
	SourceKind kind;
	int sourceIndex;
	std::string sourceName;
	Item item;
};

std::optional<SourceKind> ContainerKind(const Object &object)
{
	switch (object._otype) {
	case OBJ_CHEST1:
	case OBJ_CHEST2:
	case OBJ_CHEST3:
	case OBJ_TCHEST1:
	case OBJ_TCHEST2:
	case OBJ_TCHEST3:
		return SourceKind::Chest;
	case OBJ_BARREL:
		return SourceKind::Barrel;
	case OBJ_SARC:
		return SourceKind::Sarcophagus;
	case OBJ_ARMORSTAND:
	case OBJ_WARARMOR:
	case OBJ_WEAPONRACK:
	case OBJ_WARWEAP:
		return SourceKind::Rack;
	case OBJ_DECAP:
		return SourceKind::Other;
	default:
		// Book stands and bookcases only hold books. The slain hero is handled per class.
		return std::nullopt;
	}
}

std::string ContainerName(const Object &object)
{
	switch (object._otype) {
	case OBJ_CHEST1:
		return "Small Chest";
	case OBJ_CHEST2:
		return "Chest";
	case OBJ_CHEST3:
		return "Large Chest";
	case OBJ_TCHEST1:
		return "Small Chest (trapped)";
	case OBJ_TCHEST2:
		return "Chest (trapped)";
	case OBJ_TCHEST3:
		return "Large Chest (trapped)";
	case OBJ_BARREL:
		return "Barrel";
	case OBJ_SARC:
		return "Sarcophagus";
	case OBJ_ARMORSTAND:
		return "Armor Stand";
	case OBJ_WARARMOR:
		return "Warlord's Armor Stand";
	case OBJ_WEAPONRACK:
		return "Weapon Rack";
	case OBJ_WARWEAP:
		return "Warlord's Weapon Rack";
	case OBJ_DECAP:
		return "Decapitated Body";
	default:
		return "Object";
	}
}

// The slain hero on dlvl 9 gives an item for the class of whoever opens it. The sorcerer's is a book.
constexpr HeroClass SlainHeroClasses[] = { HeroClass::Warrior, HeroClass::Rogue };

std::string SlainHeroName(HeroClass heroClass)
{
	return heroClass == HeroClass::Warrior ? "Slain Hero (Warrior)" : "Slain Hero (Rogue)";
}

// Mirrors SpawnLoot (monster.cpp) after MonsterDeath reseeds, for Diablo mode.
void SpawnMonsterLoot(Monster &monster)
{
	SetRndSeed(monster.rndItemSeed);
	if (Quests[Q_GARBUD].IsAvailable() && monster.uniqueType == UniqueMonsterType::Garbud)
		CreateTypeItem(monster.position.tile + Displacement { 1, 1 }, true, ItemType::Mace, IMISC_NONE, false, false);
	else
		SpawnItem(monster, monster.position.tile, false);
}

// Mirrors the loot part of OperateChest, BreakBarrel, OperateSarcophagus, OperateArmorStand,
// OperateWeaponRack and OperateDecapitatedBody (objects.cpp).
void SpawnContainerLoot(Object &object)
{
	SetRndSeed(object._oRndSeed);
	switch (object._otype) {
	case OBJ_CHEST1:
	case OBJ_CHEST2:
	case OBJ_CHEST3:
	case OBJ_TCHEST1:
	case OBJ_TCHEST2:
	case OBJ_TCHEST3:
		for (int j = 0; j < object._oVar1; j++) {
			if (setlevel)
				CreateRndItem(object.position, true, false, false);
			else if (object._oVar2 != 0)
				CreateRndItem(object.position, false, false, false);
			else
				CreateRndUseful(object.position, false);
		}
		break;
	case OBJ_BARREL:
		if (object._oVar2 <= 1) {
			if (object._oVar3 == 0)
				CreateRndUseful(object.position, false);
			else
				CreateRndItem(object.position, false, false, false);
		}
		break;
	case OBJ_SARC:
		if (object._oVar1 <= 2)
			CreateRndItem(object.position, false, false, false);
		break;
	case OBJ_ARMORSTAND:
	case OBJ_WARARMOR: {
		bool uniqueRnd = !FlipCoin();
		if (currlevel <= 5)
			CreateTypeItem(object.position, true, ItemType::LightArmor, IMISC_NONE, false, false);
		else if (currlevel >= 6 && currlevel <= 9)
			CreateTypeItem(object.position, uniqueRnd, ItemType::MediumArmor, IMISC_NONE, false, false);
		else if (currlevel >= 10 && currlevel <= 12)
			CreateTypeItem(object.position, false, ItemType::HeavyArmor, IMISC_NONE, false, false);
		else if (currlevel >= 13)
			CreateTypeItem(object.position, true, ItemType::HeavyArmor, IMISC_NONE, false, false);
	} break;
	case OBJ_WEAPONRACK:
	case OBJ_WARWEAP: {
		ItemType weaponType { PickRandomlyAmong({ ItemType::Sword, ItemType::Axe, ItemType::Bow, ItemType::Mace }) };
		CreateTypeItem(object.position, leveltype != DTYPE_CATHEDRAL, weaponType, IMISC_NONE, false, false);
	} break;
	case OBJ_DECAP:
		CreateRndItem(object.position, false, false, false);
		break;
	default:
		break;
	}
}

// Mirrors OperateSlainHero (objects.cpp) for the classes whose reward isn't a book.
void SpawnSlainHeroLoot(Object &corpse, HeroClass heroClass)
{
	SetRndSeed(corpse._oRndSeed);
	if (heroClass == HeroClass::Warrior)
		CreateMagicArmor(corpse.position, ItemType::HeavyArmor, ICURS_BREAST_PLATE, false, false);
	else
		CreateMagicWeapon(corpse.position, ItemType::Bow, ICURS_LONG_BATTLE_BOW, false, false);
}

std::vector<Item> TakeItemsSince(uint8_t firstActive)
{
	std::vector<Item> taken;
	for (uint8_t k = firstActive; k < ActiveItemCount; k++) {
		Item &item = Items[ActiveItems[k]];
		taken.push_back(item);
		if (InDungeonBounds(item.position) && dItem[item.position.x][item.position.y] == ActiveItems[k] + 1)
			dItem[item.position.x][item.position.y] = 0;
		item.clear();
	}
	ActiveItemCount = firstActive;
	return taken;
}

// Runs a real spawn function, takes the items it made and restores every state it touched.
template <typename Spawn>
std::vector<Item> DryRun(Spawn &&spawn)
{
	const uint32_t rngState = GetLCGEngineState();
	std::array<bool, 128> uniqueFlags;
	std::copy(std::begin(UniqueItemFlags), std::end(UniqueItemFlags), uniqueFlags.begin());
	const uint8_t firstActive = ActiveItemCount;

	spawn();

	std::vector<Item> items = TakeItemsSince(firstActive);
	std::copy(uniqueFlags.begin(), uniqueFlags.end(), std::begin(UniqueItemFlags));
	SetRndSeed(rngState);
	return items;
}

std::vector<Drop> DryRunLevelDrops()
{
	std::vector<Drop> drops;
	for (uint8_t k = 0; k < ActiveItemCount; k++)
		drops.push_back({ SourceKind::Other, FloorSource, "Floor", Items[ActiveItems[k]] });
	for (size_t i = 0; i < ActiveMonsterCount; i++) {
		Monster &monster = Monsters[ActiveMonsters[i]];
		if (monster.isPlayerMinion())
			continue;
		const SourceKind kind = monster.isUnique() ? SourceKind::UniqueMonster : SourceKind::Monster;
		for (Item &item : DryRun([&]() { SpawnMonsterLoot(monster); }))
			drops.push_back({ kind, ActiveMonsters[i], std::string(monster.name()), std::move(item) });
	}
	for (int i = 0; i < ActiveObjectCount; i++) {
		Object &object = Objects[ActiveObjects[i]];
		if (object._otype == OBJ_SLAINHERO) {
			for (HeroClass heroClass : SlainHeroClasses) {
				for (Item &item : DryRun([&]() { SpawnSlainHeroLoot(object, heroClass); }))
					drops.push_back({ SourceKind::Other, ActiveObjects[i], SlainHeroName(heroClass), std::move(item) });
			}
			continue;
		}
		const std::optional<SourceKind> kind = ContainerKind(object);
		if (!kind)
			continue;
		for (Item &item : DryRun([&]() { SpawnContainerLoot(object); }))
			drops.push_back({ *kind, ActiveObjects[i], ContainerName(object), std::move(item) });
	}
	return drops;
}

bool HasTwoAffixes(const Item &item)
{
	return item._iMagical == ITEM_QUALITY_MAGIC && item._iPrePower != IPL_INVALID && item._iSufPower != IPL_INVALID;
}

bool IsWanted(const Item &item)
{
	return HasTwoAffixes(item) || item._iMagical == ITEM_QUALITY_UNIQUE;
}

// Kills every monster and opens every container through the game's own entry points, in reverse
// order and with earlier drops left on the floor, without restoring any state in between.
std::vector<Drop> RealLevelDrops()
{
	Player &opener = Players[1];
	std::vector<Drop> drops;
	auto take = [&](SourceKind kind, int index, std::string name, uint8_t firstActive) {
		for (uint8_t k = firstActive; k < ActiveItemCount; k++)
			drops.push_back({ kind, index, name, Items[ActiveItems[k]] });
		if (ActiveItemCount > MAXITEMS - 20)
			TakeItemsSince(firstActive);
	};

	for (int i = ActiveObjectCount - 1; i >= 0; i--) {
		Object &object = Objects[ActiveObjects[i]];
		if (object._otype == OBJ_SLAINHERO) {
			const HeroClass openerClass = opener._pClass;
			const uint8_t selFlag = object._oSelFlag;
			for (HeroClass heroClass : SlainHeroClasses) {
				opener._pClass = heroClass;
				object._oSelFlag = selFlag;
				const uint8_t firstActive = ActiveItemCount;
				OperateObject(opener, object);
				take(SourceKind::Other, ActiveObjects[i], SlainHeroName(heroClass), firstActive);
			}
			opener._pClass = openerClass;
			continue;
		}
		const std::optional<SourceKind> kind = ContainerKind(object);
		if (!kind)
			continue;
		const uint8_t firstActive = ActiveItemCount;
		if (object._otype == OBJ_BARREL)
			SyncBreakObj(opener, object);
		else
			OperateObject(opener, object);
		take(*kind, ActiveObjects[i], ContainerName(object), firstActive);
	}

	std::vector<int> monsterIds;
	std::vector<int> diabloIds;
	for (size_t i = ActiveMonsterCount; i-- > 0;)
		(Monsters[ActiveMonsters[i]].type().type == MT_DIABLO ? diabloIds : monsterIds).push_back(ActiveMonsters[i]);
	// DiabloDeath changes the other monsters, so Diablo goes last.
	monsterIds.insert(monsterIds.end(), diabloIds.begin(), diabloIds.end());
	for (int id : monsterIds) {
		Monster &monster = Monsters[id];
		if (monster.isPlayerMinion())
			continue;
		const uint8_t firstActive = ActiveItemCount;
		MonsterDeath(monster, Direction::South, false);
		take(monster.isUnique() ? SourceKind::UniqueMonster : SourceKind::Monster, id, std::string(monster.name()), firstActive);
	}
	return drops;
}

std::string DescribeItem(const Item &item)
{
	return fmt::format("{} idx={} seed={} ci={} q={} pre={} suf={} value={}", item._iIName, static_cast<int>(item.IDidx), item._iSeed, item._iCreateInfo,
	    static_cast<int>(item._iMagical), static_cast<int>(item._iPrePower), static_cast<int>(item._iSufPower), item._iIvalue);
}

std::map<std::pair<int, int>, std::vector<std::string>> GroupBySource(const std::vector<Drop> &drops)
{
	std::map<std::pair<int, int>, std::vector<std::string>> grouped;
	for (const Drop &drop : drops) {
		if (drop.sourceIndex == FloorSource)
			continue;
		const int group = drop.kind == SourceKind::Monster || drop.kind == SourceKind::UniqueMonster ? 0 : 1;
		grouped[{ group, drop.sourceIndex }].push_back(fmt::format("{}: {}", drop.sourceName, DescribeItem(drop.item)));
	}
	return grouped;
}

std::string CsvField(string_view text)
{
	if (text.find_first_of(",\"\r\n") == string_view::npos)
		return std::string(text);
	std::string quoted = "\"";
	for (char c : text) {
		if (c == '"')
			quoted += '"';
		quoted += c;
	}
	quoted += '"';
	return quoted;
}

string_view ItemTypeName(ItemType type)
{
	switch (type) {
	case ItemType::Sword:
		return "sword";
	case ItemType::Axe:
		return "axe";
	case ItemType::Bow:
		return "bow";
	case ItemType::Mace:
		return "mace";
	case ItemType::Shield:
		return "shield";
	case ItemType::LightArmor:
		return "light_armor";
	case ItemType::Helm:
		return "helm";
	case ItemType::MediumArmor:
		return "medium_armor";
	case ItemType::HeavyArmor:
		return "heavy_armor";
	case ItemType::Staff:
		return "staff";
	case ItemType::Ring:
		return "ring";
	case ItemType::Amulet:
		return "amulet";
	case ItemType::Gold:
		return "gold";
	case ItemType::Misc:
		return "misc";
	default:
		return "none";
	}
}

// The first two numbers of an affix as the item panel shows it, e.g. "to hit: +20%, +150% damage" -> 20, 150.
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

// Where a rolled affix value sits in the affix's range: 0 is the lowest possible roll, 100 the highest.
// Affixes with a single possible value count as 100. Nothing when the item doesn't fit this entry.
std::optional<int> RollPercent(const PLStruct &affix, const Item &item, const std::array<std::string, 2> &numbers)
{
	const int low = std::min(std::abs(affix.power.param1), std::abs(affix.power.param2));
	const int high = std::max(std::abs(affix.power.param1), std::abs(affix.power.param2));
	if (low == high)
		return 100;
	if (affix.power.type == IPL_DUR) {
		// The durability bonus isn't shown; SaveItemPower adds r% of the base item's durability.
		const int base = AllItemsList[item.IDidx].iDurability;
		if (base == 0)
			return std::nullopt;
		const int rolled = (item._iMaxDur - base) * 100 / base;
		return std::clamp((rolled - low) * 100 / (high - low), 0, 100);
	}
	// The shown number that falls inside the range is the rolled one; King's, for example, shows to-hit
	// first and then the rolled damage.
	for (const std::string &number : numbers) {
		if (number.empty())
			continue;
		const int value = std::abs(std::stoi(number));
		if (value >= low && value <= high)
			return (value - low) * 100 / (high - low);
	}
	return std::nullopt;
}

struct AffixRoll {
	const PLStruct *affix = nullptr;
	std::optional<int> roll;
};

// Items don't keep which affix table entry they rolled, only its power type and the name it produced.
// Some names appear twice with different ranges (Crimson), so the entry the shown value fits wins.
AffixRoll FindAffix(const PLStruct *table, item_effect_type power, bool isPrefix, const Item &item, const std::array<std::string, 2> &numbers)
{
	AffixRoll found;
	if (power == IPL_INVALID)
		return found;
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
		if (found.affix == nullptr || (roll && !found.roll))
			found = { &affix, roll };
	}
	return found;
}

std::string RollText(std::optional<int> roll)
{
	return roll ? std::to_string(*roll) : "";
}

// Columns in the order of ITEM_HEADER in test/drop_stats/search_drop_stats.py.
std::string ItemCsvRow(uint32_t gameSeed, _difficulty difficulty, LevelId level, const Drop &drop)
{
	const Item &item = drop.item;
	const std::string prefixPower = item._iPrePower != IPL_INVALID ? std::string(PrintItemPower(item._iPrePower, item).str()) : "";
	const std::string suffixPower = item._iSufPower != IPL_INVALID ? std::string(PrintItemPower(item._iSufPower, item).str()) : "";
	const auto prefixNumbers = PowerNumbers(prefixPower);
	const auto suffixNumbers = PowerNumbers(suffixPower);
	const AffixRoll prefix = FindAffix(ItemPrefixes, item._iPrePower, true, item, prefixNumbers);
	const AffixRoll suffix = FindAffix(ItemSuffixes, item._iSufPower, false, item, suffixNumbers);
	const std::string prefixText = prefix.affix != nullptr ? prefixPower : "";
	const std::string suffixText = suffix.affix != nullptr ? suffixPower : "";
	const bool isUnique = item._iMagical == ITEM_QUALITY_UNIQUE;
	const bool hasSpell = item._iSpell != SpellID::Null && item._iMiscId == IMISC_STAFF;

	return fmt::format("{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{}\n",
	    gameSeed, static_cast<int>(difficulty), level.dlvl, level.setLevel == SL_NONE ? "" : CsvField(GetSetLevelQuest(level.setLevel).name),
	    SourceKindName(drop.kind), CsvField(drop.sourceName), drop.sourceIndex,
	    ItemTypeName(item._itype), CsvField(AllItemsList[item.IDidx].iName), item._iCreateInfo & CF_LEVEL, isUnique ? "unique" : "magic",
	    prefix.affix != nullptr ? CsvField(prefix.affix->PLName) : "", CsvField(prefixText), prefix.affix != nullptr ? prefixNumbers[0] : "", prefix.affix != nullptr ? prefixNumbers[1] : "",
	    suffix.affix != nullptr ? CsvField(suffix.affix->PLName) : "", CsvField(suffixText), suffix.affix != nullptr ? suffixNumbers[0] : "", suffix.affix != nullptr ? suffixNumbers[1] : "",
	    isUnique ? CsvField(UniqueItems[item._iUid].UIName) : "", hasSpell ? CsvField(GetSpellData(item._iSpell).sNameText) : "", hasSpell ? std::to_string(item._iMaxCharges) : "",
	    item._iMinDam, item._iMaxDam, item._iAC, item._iMaxDur, item._iMinStr, item._iMinMag, item._iMinDex,
	    item._iIvalue, CsvField(item._iIName), static_cast<int>(item.IDidx), item._iSeed, item._iCreateInfo,
	    RollText(prefix.roll), RollText(suffix.roll));
}

std::optional<uint64_t> EnvNumber(const char *name)
{
	const char *value = std::getenv(name);
	if (value == nullptr || *value == '\0')
		return std::nullopt;
	return std::strtoull(value, nullptr, 10);
}

class DropStats : public ::testing::Test {
protected:
	void SetUp() override
	{
		LoadArchivesOnce();
		ASSERT_TRUE(HaveDiabdat()) << "DIABDAT.MPQ not found in " << MpqDir();
	}
};

TEST_F(DropStats, GenerateLevelIsDeterministic)
{
	StartMultiplayerGame(123456789, DIFF_NORMAL);
	for (LevelId level : ReachableLevels()) {
		StartMultiplayerGame(123456789, DIFF_NORMAL);
		GenerateLevel(level);
		const size_t monsters = ActiveMonsterCount;
		const int objects = ActiveObjectCount;
		const uint32_t monsterSeed = Monsters[ActiveMonsters[ActiveMonsterCount - 1]].rndItemSeed;

		StartMultiplayerGame(123456789, DIFF_NORMAL);
		GenerateLevel(level);
		EXPECT_EQ(ActiveMonsterCount, monsters) << LevelName(level);
		EXPECT_EQ(ActiveObjectCount, objects) << LevelName(level);
		EXPECT_EQ(Monsters[ActiveMonsters[ActiveMonsterCount - 1]].rndItemSeed, monsterSeed) << LevelName(level);
	}
}

TEST_F(DropStats, DryRunMatchesRealDrops)
{
	const uint32_t gameSeeds[] = { 1, 42, 123456789, 987654321 };
	const _difficulty difficulties[] = { DIFF_NORMAL, DIFF_NIGHTMARE, DIFF_HELL };
	size_t comparedItems = 0;
	size_t wantedItems = 0;
	size_t setLevels = 0;
	for (uint32_t gameSeed : gameSeeds) {
		for (_difficulty difficulty : difficulties) {
			StartMultiplayerGame(gameSeed, difficulty);
			for (LevelId level : ReachableLevels()) {
				StartMultiplayerGame(gameSeed, difficulty);
				GenerateLevel(level);
				const std::vector<Drop> predicted = DryRunLevelDrops();
				const std::vector<Drop> actual = RealLevelDrops();
				ASSERT_EQ(GroupBySource(predicted), GroupBySource(actual)) << "seed " << gameSeed << " difficulty " << difficulty << " " << LevelName(level);
				comparedItems += actual.size();
				wantedItems += std::count_if(actual.begin(), actual.end(), [](const Drop &drop) { return IsWanted(drop.item); });
				if (level.setLevel != SL_NONE)
					setLevels++;
			}
		}
	}
	std::cout << "compared " << comparedItems << " drops (" << setLevels << " set levels included), " << wantedItems << " two-affix or unique\n";
}

TEST_F(DropStats, DryRunTimingAndSample)
{
	constexpr int Games = 20;
	struct Stats {
		int levels = 0;
		double genMs = 0;
		double dryMs = 0;
		size_t drops = 0;
		size_t wanted = 0;
	};
	std::vector<std::pair<std::string, Stats>> stats;
	auto statsFor = [&](const std::string &name) -> Stats & {
		for (auto &entry : stats) {
			if (entry.first == name)
				return entry.second;
		}
		return stats.emplace_back(name, Stats {}).second;
	};

	double totalMs = 0;
	for (int game = 0; game < Games; game++) {
		StartMultiplayerGame(1000 + game, DIFF_HELL);
		for (LevelId level : ReachableLevels()) {
			StartMultiplayerGame(1000 + game, DIFF_HELL);
			auto start = std::chrono::steady_clock::now();
			GenerateLevel(level);
			auto generated = std::chrono::steady_clock::now();
			const std::vector<Drop> drops = DryRunLevelDrops();
			auto done = std::chrono::steady_clock::now();

			Stats &levelStats = statsFor(LevelName(level));
			levelStats.levels++;
			levelStats.genMs += std::chrono::duration<double, std::milli>(generated - start).count();
			levelStats.dryMs += std::chrono::duration<double, std::milli>(done - generated).count();
			totalMs += std::chrono::duration<double, std::milli>(done - start).count();
			levelStats.drops += drops.size();
			for (const Drop &drop : drops) {
				if (!IsWanted(drop.item))
					continue;
				levelStats.wanted++;
				if (game == 0)
					std::cout << "  game 1000 " << LevelName(level) << " " << SourceKindName(drop.kind) << " " << drop.sourceName << ": " << drop.item._iIName << "\n";
			}
		}
	}

	std::printf("%-22s %5s %7s %7s %6s %6s\n", "level", "games", "gen_ms", "dry_ms", "drops", "wanted");
	for (const auto &[name, s] : stats)
		std::printf("%-22s %5d %7.2f %7.2f %6.1f %6.2f\n", name.c_str(), s.levels, s.genMs / s.levels, s.dryMs / s.levels, static_cast<double>(s.drops) / s.levels, static_cast<double>(s.wanted) / s.levels);
	std::printf("per game on Hell (all reachable levels): %.1f ms\n", totalMs / Games);
}

// Writes every prefix, suffix, unique and base item name to DROPSTATS_NAMES_FILE, so searches can
// reject a misspelled name before simulating anything.
TEST_F(DropStats, DumpNames)
{
	const char *path = std::getenv("DROPSTATS_NAMES_FILE");
	if (path == nullptr)
		GTEST_SKIP() << "DROPSTATS_NAMES_FILE not set";
	std::ofstream names(path, std::ios::binary | std::ios::trunc);
	names << "kind,name\n";
	for (int j = 0; ItemPrefixes[j].power.type != IPL_INVALID; j++)
		names << "prefix," << CsvField(ItemPrefixes[j].PLName) << "\n";
	for (int j = 0; ItemSuffixes[j].power.type != IPL_INVALID; j++)
		names << "suffix," << CsvField(ItemSuffixes[j].PLName) << "\n";
	for (int j = 0; UniqueItems[j].UIItemId != UITYPE_INVALID; j++)
		names << "unique," << CsvField(UniqueItems[j].UIName) << "\n";
	for (int j = IDI_GOLD; j <= IDI_LAST; j++)
		names << "base," << CsvField(AllItemsList[j].iName) << "\n";
	ASSERT_TRUE(names) << "could not write " << path;
}

// Prints each step for one game seed, to find where a seed that stalls a worker gets stuck.
TEST_F(DropStats, TraceSeed)
{
	const std::optional<uint64_t> traceSeed = EnvNumber("DROPSTATS_TRACE_SEED");
	if (!traceSeed)
		GTEST_SKIP() << "DROPSTATS_TRACE_SEED not set";
	const std::optional<uint64_t> onlyDlvl = EnvNumber("DROPSTATS_TRACE_DLVL");
	const auto gameSeed = static_cast<uint32_t>(*traceSeed);
	for (_difficulty difficulty : { DIFF_NORMAL, DIFF_NIGHTMARE, DIFF_HELL }) {
		StartMultiplayerGame(gameSeed, difficulty);
		for (LevelId level : ReachableLevels()) {
			if (onlyDlvl && (level.setLevel != SL_NONE || level.dlvl != *onlyDlvl))
				continue;
			StartMultiplayerGame(gameSeed, difficulty);
			std::printf("difficulty %d %s: generate\n", static_cast<int>(difficulty), LevelName(level).c_str());
			std::fflush(stdout);
			GenerateLevel(level);
			std::printf("difficulty %d %s: dry run\n", static_cast<int>(difficulty), LevelName(level).c_str());
			std::fflush(stdout);
			DryRunLevelDrops();
		}
	}
}

// One worker of drops.ps1 search (test/drop_stats/search_drop_stats.py), driven by environment variables:
// DROPSTATS_FIRST_SEED, DROPSTATS_SEED_STEP (every n-th seed, so workers interleave), and optionally
// DROPSTATS_STOP_AT (unix time), DROPSTATS_SKIP_LEVELS and DROPSTATS_WORKER. Rows go to stdout: an "I," row per
// magic or unique item (columns as ItemCsvRow), then "G,<seed>,<difficulty>,<levels left out>" per game.
TEST_F(DropStats, SearchWorker)
{
	const std::optional<uint64_t> firstSeed = EnvNumber("DROPSTATS_FIRST_SEED");
	if (!firstSeed)
		GTEST_SKIP() << "DROPSTATS_FIRST_SEED not set";
	const uint64_t seedStep = std::max<uint64_t>(EnvNumber("DROPSTATS_SEED_STEP").value_or(1), 1);
	const std::optional<uint64_t> stopAt = EnvNumber("DROPSTATS_STOP_AT");
	const auto worker = static_cast<unsigned long long>(EnvNumber("DROPSTATS_WORKER").value_or(0));

	// For some seeds the game's own level generator loops forever on one level (seen in the catacombs). The
	// watchdog below reports the level and exits, and the search restarts the worker with that level in
	// DROPSTATS_SKIP_LEVELS ("seed:dlvl:setlevel;..."). The layout doesn't depend on difficulty or entrance,
	// so the level is left out on every difficulty.
	std::vector<std::tuple<uint64_t, int, int>> skipLevels;
	if (const char *skip = std::getenv("DROPSTATS_SKIP_LEVELS")) {
		for (const char *entry = skip; *entry != '\0';) {
			unsigned long long skipSeed = 0;
			int dlvl = 0;
			int setLevel = 0;
			if (std::sscanf(entry, "%llu:%d:%d", &skipSeed, &dlvl, &setLevel) == 3)
				skipLevels.emplace_back(skipSeed, dlvl, setLevel);
			const char *next = std::strchr(entry, ';');
			if (next == nullptr)
				break;
			entry = next + 1;
		}
	}
	auto isSkipped = [&](uint64_t seed, LevelId level) {
		const std::tuple<uint64_t, int, int> key { seed, level.dlvl, level.setLevel };
		return std::find(skipLevels.begin(), skipLevels.end(), key) != skipLevels.end();
	};

	// A normal seed takes about half a second for all three difficulties.
	const auto hangLimit = std::chrono::seconds(EnvNumber("DROPSTATS_HANG_SECONDS").value_or(10));
	std::atomic<uint64_t> currentSeed { *firstSeed };
	std::atomic<int> currentDlvl { 0 };
	std::atomic<int> currentSetLevel { SL_NONE };
	std::atomic<int64_t> seedStarted { std::chrono::steady_clock::now().time_since_epoch().count() };
	std::atomic<bool> searching { true };
	std::thread watchdog([&]() {
		while (searching) {
			std::this_thread::sleep_for(std::chrono::seconds(1));
			const std::chrono::steady_clock::time_point since { std::chrono::steady_clock::duration(seedStarted.load()) };
			if (!searching || std::chrono::steady_clock::now() - since < hangLimit)
				continue;
			const LevelId level { static_cast<uint8_t>(currentDlvl.load()), static_cast<_setlevels>(currentSetLevel.load()) };
			std::printf("HUNG,%llu,%d,%d,%s\n", static_cast<unsigned long long>(currentSeed.load()), static_cast<int>(level.dlvl), static_cast<int>(level.setLevel), CsvField(LevelName(level)).c_str());
			std::fflush(stdout);
			std::_Exit(3);
		}
	});

	uint64_t seed = *firstSeed;
	for (; seed < (uint64_t { 1 } << 32); seed += seedStep) {
		if (stopAt && static_cast<uint64_t>(std::time(nullptr)) >= *stopAt)
			break;
		currentSeed = seed;
		seedStarted = std::chrono::steady_clock::now().time_since_epoch().count();
		const auto gameSeed = static_cast<uint32_t>(seed);
		std::string rows;
		for (_difficulty difficulty : { DIFF_NORMAL, DIFF_NIGHTMARE, DIFF_HELL }) {
			StartMultiplayerGame(gameSeed, difficulty);
			const std::vector<LevelId> levels = ReachableLevels();
			std::string skipped;
			for (LevelId level : levels) {
				if (isSkipped(seed, level)) {
					skipped += (skipped.empty() ? "" : ";") + LevelName(level);
					continue;
				}
				currentDlvl = level.dlvl;
				currentSetLevel = level.setLevel;
				StartMultiplayerGame(gameSeed, difficulty);
				GenerateLevel(level);
				for (const Drop &drop : DryRunLevelDrops()) {
					if (drop.item._iMagical == ITEM_QUALITY_NORMAL)
						continue;
					rows += "I,";
					rows += ItemCsvRow(gameSeed, difficulty, level, drop);
				}
			}
			rows += fmt::format("G,{},{},{}\n", gameSeed, static_cast<int>(difficulty), CsvField(skipped));
		}
		std::fwrite(rows.data(), 1, rows.size(), stdout);
		std::fflush(stdout);
		// The search reading the pipe is gone (closed window, killed process), so stop instead of running on unseen.
		if (std::ferror(stdout))
			break;
	}
	searching = false;
	watchdog.join();
	std::printf("worker %llu finished at seed %llu\n", worker, static_cast<unsigned long long>(seed));
}

} // namespace
} // namespace devilution
