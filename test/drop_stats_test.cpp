#include <algorithm>
#include <atomic>
#include <chrono>
#include <cctype>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <optional>
#include <thread>
#include <vector>

#include <fmt/format.h>
#include <gtest/gtest.h>

#include "config.h"
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

// Must match "Randomize Quests" in the game creator's diablo.ini; joiners take the creator's quest states via DeltaSyncJunk.
constexpr bool RandomizeQuests = true;

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
	sgOptions.Gameplay.randomizeQuests.SetValue(RandomizeQuests);
	sgGameInitInfo.bTheoQuest = 0;
	sgGameInitInfo.bCowQuest = 0;

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

// Items don't keep which affix table entry they rolled, only its power type and the name it produced.
const PLStruct *FindPrefix(const Item &item)
{
	if (item._iPrePower == IPL_INVALID)
		return nullptr;
	const string_view name = item._iIName;
	for (int j = 0; ItemPrefixes[j].power.type != IPL_INVALID; j++) {
		const PLStruct &prefix = ItemPrefixes[j];
		const string_view prefixName = prefix.PLName;
		if (prefix.power.type == item._iPrePower && name.size() > prefixName.size() && name.substr(0, prefixName.size()) == prefixName && name[prefixName.size()] == ' ')
			return &prefix;
	}
	return nullptr;
}

const PLStruct *FindSuffix(const Item &item)
{
	if (item._iSufPower == IPL_INVALID)
		return nullptr;
	const string_view name = item._iIName;
	for (int j = 0; ItemSuffixes[j].power.type != IPL_INVALID; j++) {
		const PLStruct &suffix = ItemSuffixes[j];
		const std::string ending = StrCat(" of ", suffix.PLName);
		if (suffix.power.type == item._iSufPower && name.size() > ending.size() && name.substr(name.size() - ending.size()) == ending)
			return &suffix;
	}
	return nullptr;
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

constexpr string_view ItemCsvHeader = "game_seed,difficulty,dlvl,set_level,source_kind,source_name,source_index,"
                                      "item_type,base_item,item_level,quality,"
                                      "prefix,prefix_text,prefix_value,prefix_value2,"
                                      "suffix,suffix_text,suffix_value,suffix_value2,"
                                      "unique_name,spell,charges,min_dam,max_dam,ac,max_dur,req_str,req_mag,req_dex,"
                                      "item_value,name,idx,iseed,create_info\n";

std::string ItemCsvRow(uint32_t gameSeed, _difficulty difficulty, LevelId level, const Drop &drop)
{
	const Item &item = drop.item;
	const PLStruct *prefix = FindPrefix(item);
	const PLStruct *suffix = FindSuffix(item);
	const std::string prefixText = prefix != nullptr ? std::string(PrintItemPower(item._iPrePower, item).str()) : "";
	const std::string suffixText = suffix != nullptr ? std::string(PrintItemPower(item._iSufPower, item).str()) : "";
	const auto prefixNumbers = PowerNumbers(prefixText);
	const auto suffixNumbers = PowerNumbers(suffixText);
	const bool isUnique = item._iMagical == ITEM_QUALITY_UNIQUE;
	const bool hasSpell = item._iSpell != SpellID::Null && item._iMiscId == IMISC_STAFF;

	return fmt::format("{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{}\n",
	    gameSeed, static_cast<int>(difficulty), level.dlvl, level.setLevel == SL_NONE ? "" : CsvField(GetSetLevelQuest(level.setLevel).name),
	    SourceKindName(drop.kind), CsvField(drop.sourceName), drop.sourceIndex,
	    ItemTypeName(item._itype), CsvField(AllItemsList[item.IDidx].iName), item._iCreateInfo & CF_LEVEL, isUnique ? "unique" : "magic",
	    prefix != nullptr ? CsvField(prefix->PLName) : "", CsvField(prefixText), prefixNumbers[0], prefixNumbers[1],
	    suffix != nullptr ? CsvField(suffix->PLName) : "", CsvField(suffixText), suffixNumbers[0], suffixNumbers[1],
	    isUnique ? CsvField(UniqueItems[item._iUid].UIName) : "", hasSpell ? CsvField(GetSpellData(item._iSpell).sNameText) : "", hasSpell ? std::to_string(item._iMaxCharges) : "",
	    item._iMinDam, item._iMaxDam, item._iAC, item._iMaxDur, item._iMinStr, item._iMinMag, item._iMinDex,
	    item._iIvalue, CsvField(item._iIName), static_cast<int>(item.IDidx), item._iSeed, item._iCreateInfo);
}

constexpr string_view GameCsvHeader = "game_seed,difficulty,quests,set_levels,hung_levels,item_rows,items_end\n";

std::string AvailableQuests()
{
	std::string names;
	for (int q = Q_ROCK; q <= Q_BETRAYER; q++) {
		if (Quests[q]._qactive == QUEST_NOTAVAIL)
			continue;
		if (!names.empty())
			names += ';';
		names += QuestsData[q]._qlstr;
	}
	return names;
}

std::string ReachableSetLevels(const std::vector<LevelId> &levels)
{
	std::string names;
	for (LevelId level : levels) {
		if (level.setLevel == SL_NONE)
			continue;
		if (!names.empty())
			names += ';';
		names += GetSetLevelQuest(level.setLevel).name;
	}
	return names;
}

std::optional<uint64_t> EnvNumber(const char *name)
{
	const char *value = std::getenv(name);
	if (value == nullptr || *value == '\0')
		return std::nullopt;
	return std::strtoull(value, nullptr, 10);
}

struct ResumePoint {
	uint64_t lastSeed;
	uint64_t itemsEnd;
	uint64_t gamesEnd;
};

// A seed's rows are appended to the items file first, then its three games rows, each carrying the items
// file size after it. The last complete Hell row marks where both files can be cut back to after a crash.
std::optional<ResumePoint> FindResumePoint(const std::string &gamesPath)
{
	std::ifstream games(gamesPath, std::ios::binary);
	if (!games)
		return std::nullopt;
	std::optional<ResumePoint> resume;
	std::string line;
	uint64_t lineStart = 0;
	while (std::getline(games, line)) {
		const uint64_t lineEnd = lineStart + line.size() + 1;
		const bool complete = !games.eof();
		std::vector<std::string> fields;
		std::string field;
		bool quoted = false;
		for (char c : line) {
			if (c == '"')
				quoted = !quoted;
			else if (c == ',' && !quoted) {
				fields.push_back(field);
				field.clear();
			} else
				field += c;
		}
		fields.push_back(field);
		if (complete && fields.size() == 7 && fields[1] == "2" && !fields[6].empty() && std::isdigit(static_cast<unsigned char>(fields[0][0])))
			resume = ResumePoint { std::stoull(fields[0]), std::stoull(fields[6]), lineEnd };
		lineStart = lineEnd;
	}
	return resume;
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

// Prints each step for one game seed, to find where a seed that stalls a worker gets stuck.
TEST_F(DropStats, TraceSeed)
{
	const std::optional<uint64_t> traceSeed = EnvNumber("DROPSTATS_TRACE_SEED");
	if (!traceSeed)
		GTEST_SKIP() << "DROPSTATS_TRACE_SEED not set";
	const std::optional<uint64_t> onlyDlvl = EnvNumber("DROPSTATS_TRACE_DLVL");
	const auto gameSeed = static_cast<uint32_t>(*traceSeed);
	StartMultiplayerGame(gameSeed, DIFF_NORMAL);
	std::printf("storybook %u\n", glSeedTbl[16]);
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

// One worker of the long simulation run, driven by environment variables (see test/drop_stats/drops.ps1):
// DROPSTATS_OUT_DIR, DROPSTATS_WORKER, DROPSTATS_FIRST_SEED, DROPSTATS_SEED_COUNT, optional DROPSTATS_SEED_STEP
// (every n-th seed, so workers can interleave), DROPSTATS_STOP_AT (unix time)
// and DROPSTATS_GIT_REV. Rerunning with the same settings resumes after the last complete seed.
TEST_F(DropStats, Record)
{
	const char *outDir = std::getenv("DROPSTATS_OUT_DIR");
	if (outDir == nullptr)
		GTEST_SKIP() << "DROPSTATS_OUT_DIR not set";
	const char *workerEnv = std::getenv("DROPSTATS_WORKER");
	const std::string worker = workerEnv != nullptr ? workerEnv : "0";
	const uint64_t firstSeed = EnvNumber("DROPSTATS_FIRST_SEED").value_or(0);
	const uint64_t seedCount = EnvNumber("DROPSTATS_SEED_COUNT").value_or(1000);
	const uint64_t seedStep = std::max<uint64_t>(EnvNumber("DROPSTATS_SEED_STEP").value_or(1), 1);
	const std::optional<uint64_t> stopAt = EnvNumber("DROPSTATS_STOP_AT");
	const char *gitRev = std::getenv("DROPSTATS_GIT_REV");

	const std::string base = StrCat(outDir, "/");
	const std::string itemsPath = StrCat(base, "items_", worker, ".csv");
	const std::string gamesPath = StrCat(base, "games_", worker, ".csv");

	uint64_t seed = firstSeed;
	if (const std::optional<ResumePoint> resume = FindResumePoint(gamesPath)) {
		seed = resume->lastSeed + seedStep;
		std::filesystem::resize_file(itemsPath, resume->itemsEnd);
		std::filesystem::resize_file(gamesPath, resume->gamesEnd);
	} else {
		std::ofstream(itemsPath, std::ios::binary | std::ios::trunc) << ItemCsvHeader;
		std::ofstream(gamesPath, std::ios::binary | std::ios::trunc) << GameCsvHeader;
	}

	{
		std::ofstream info(StrCat(base, "run_info_", worker, ".txt"), std::ios::binary | std::ios::trunc);
		info << "version=" << PROJECT_VERSION << "\n"
		     << "git_rev=" << (gitRev != nullptr ? gitRev : "") << "\n"
		     << "game_mode=" << (gbIsHellfire ? "hellfire" : "diablo") << "\n"
		     << "multiplayer=" << 1 << "\n"
		     << "full_quests=" << 1 << "\n"
		     << "randomize_quests=" << RandomizeQuests << "\n"
		     << "theo_quest=" << 0 << "\n"
		     << "cow_quest=" << 0 << "\n"
		     << "worker=" << worker << "\n"
		     << "first_seed=" << firstSeed << "\n"
		     << "seed_count=" << seedCount << "\n"
		     << "seed_step=" << seedStep << "\n";
	}

	// For some seeds the game's own level generator loops forever on one level (seen in the catacombs).
	// The layout doesn't depend on difficulty or entrance, so that level is listed in hung_<worker>.csv and
	// left out of the seed on every difficulty; the launcher restarts the worker after the exit below.
	const std::string hungPath = StrCat(base, "hung_", worker, ".csv");
	std::map<uint64_t, std::vector<std::pair<int, int>>> hungLevels;
	{
		std::ifstream hung(hungPath);
		std::string line;
		while (std::getline(hung, line)) {
			unsigned long long hungSeed = 0;
			int dlvl = 0;
			int setLevel = 0;
			if (std::sscanf(line.c_str(), "%llu,%d,%d", &hungSeed, &dlvl, &setLevel) == 3)
				hungLevels[hungSeed].emplace_back(dlvl, setLevel);
		}
	}
	auto isHung = [&](uint64_t hungSeed, LevelId level) {
		const auto found = hungLevels.find(hungSeed);
		if (found == hungLevels.end())
			return false;
		const std::pair<int, int> key { level.dlvl, level.setLevel };
		return std::find(found->second.begin(), found->second.end(), key) != found->second.end();
	};
	const auto hangLimit = std::chrono::seconds(EnvNumber("DROPSTATS_HANG_SECONDS").value_or(60));
	std::atomic<uint64_t> currentSeed { seed };
	std::atomic<int> currentDlvl { 0 };
	std::atomic<int> currentSetLevel { SL_NONE };
	std::atomic<int64_t> seedStarted { std::chrono::steady_clock::now().time_since_epoch().count() };
	std::atomic<bool> recording { true };
	std::thread watchdog([&]() {
		while (recording) {
			std::this_thread::sleep_for(std::chrono::seconds(1));
			const std::chrono::steady_clock::time_point since { std::chrono::steady_clock::duration(seedStarted.load()) };
			if (!recording || std::chrono::steady_clock::now() - since < hangLimit)
				continue;
			const LevelId level { static_cast<uint8_t>(currentDlvl.load()), static_cast<_setlevels>(currentSetLevel.load()) };
			std::ofstream hung(hungPath, std::ios::binary | std::ios::app);
			hung << currentSeed.load() << "," << static_cast<int>(level.dlvl) << "," << static_cast<int>(level.setLevel) << "," << CsvField(LevelName(level)) << "\n";
			hung.flush();
			std::printf("worker %s: seed %llu hung in %s, exiting so the level can be skipped\n", worker.c_str(), static_cast<unsigned long long>(currentSeed.load()), LevelName(level).c_str());
			std::fflush(stdout);
			std::_Exit(3);
		}
	});

	uint64_t itemsSize = std::filesystem::file_size(itemsPath);
	std::ofstream items(itemsPath, std::ios::binary | std::ios::app);
	std::ofstream games(gamesPath, std::ios::binary | std::ios::app);

	const auto started = std::chrono::steady_clock::now();
	auto lastReport = started;
	uint64_t seedsDone = 0;
	const uint64_t endSeed = std::min<uint64_t>(firstSeed + seedCount * seedStep, uint64_t { 1 } << 32);
	for (; seed < endSeed; seed += seedStep) {
		if (stopAt && static_cast<uint64_t>(std::time(nullptr)) >= *stopAt)
			break;
		currentSeed = seed;
		seedStarted = std::chrono::steady_clock::now().time_since_epoch().count();
		const auto gameSeed = static_cast<uint32_t>(seed);
		std::string itemRows;
		std::string gameRows;
		for (_difficulty difficulty : { DIFF_NORMAL, DIFF_NIGHTMARE, DIFF_HELL }) {
			StartMultiplayerGame(gameSeed, difficulty);
			const std::vector<LevelId> levels = ReachableLevels();
			const std::string quests = AvailableQuests();
			size_t rows = 0;
			std::string hungNames;
			for (LevelId level : levels) {
				if (isHung(seed, level)) {
					hungNames += (hungNames.empty() ? "" : ";") + LevelName(level);
					continue;
				}
				currentDlvl = level.dlvl;
				currentSetLevel = level.setLevel;
				StartMultiplayerGame(gameSeed, difficulty);
				GenerateLevel(level);
				for (const Drop &drop : DryRunLevelDrops()) {
					if (drop.item._iMagical == ITEM_QUALITY_NORMAL)
						continue;
					itemRows += ItemCsvRow(gameSeed, difficulty, level, drop);
					rows++;
				}
			}
			gameRows += fmt::format("{},{},{},{},{},{},{}\n", gameSeed, static_cast<int>(difficulty), CsvField(quests), CsvField(ReachableSetLevels(levels)), CsvField(hungNames), rows, itemsSize + itemRows.size());
		}
		items << itemRows;
		items.flush();
		itemsSize += itemRows.size();
		games << gameRows;
		games.flush();
		ASSERT_TRUE(items && games) << "write failed in " << outDir;
		seedsDone++;

		const auto now = std::chrono::steady_clock::now();
		if (now - lastReport >= std::chrono::seconds(60)) {
			const double seconds = std::chrono::duration<double>(now - started).count();
			std::printf("worker %s: seed %llu, %llu seeds this session, %.2f seeds/s\n", worker.c_str(), static_cast<unsigned long long>(seed), static_cast<unsigned long long>(seedsDone), seedsDone / seconds);
			std::fflush(stdout);
			lastReport = now;
		}
	}
	recording = false;
	watchdog.join();
	std::printf("worker %s finished at seed %llu after %llu seeds\n", worker.c_str(), static_cast<unsigned long long>(seed), static_cast<unsigned long long>(seedsDone));
}

} // namespace
} // namespace devilution
