#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <iostream>
#include <limits>
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
#include "missiles.h"
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

// Must match "Full quests in Multiplayer" and "Randomize Quests" in the game creator's settings; joiners take the
// creator's quest states via DeltaSyncJunk. The search worker sets them from DROPSTATS_FULL_QUESTS and
// DROPSTATS_RANDOMIZE_QUESTS.
struct QuestSettings {
	bool fullQuests = true;
	bool randomizeQuests = true;
};
QuestSettings Settings;

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
	// The host is in the game (NetInit). ClrAllMonsters draws GenerateRnd(gbActivePlayers) for every monster
	// slot, and with 0 players that draws nothing: set levels, which are built straight after their seed is
	// set, would come out 200 random numbers early.
	gbActivePlayers = 1;
	sgGameInitInfo.dwSeed = gameSeed;
	sgGameInitInfo.nDifficulty = difficulty;
	sgGameInitInfo.fullQuests = Settings.fullQuests ? 1 : 0;
	sgOptions.Gameplay.randomizeQuests.SetValue(Settings.randomizeQuests);
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

// Dungeon levels 1-16 plus the quest set levels this game's quest roll made reachable. Without full quests
// there are none: the Skeleton King and Lazarus are placed on dlvl 3 and 15 instead (PlaceQuestMonsters).
std::vector<LevelId> ReachableLevels()
{
	std::vector<LevelId> levels;
	for (uint8_t dlvl = 1; dlvl <= 16; dlvl++)
		levels.push_back({ dlvl });
	if (UseMultiplayerQuests())
		return levels;
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
	/** Where the source is when the level is created: the monster's starting tile, the object's, or the floor item's. */
	Point position {};
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

// Each player's golem waits off the map until it is cast and then never drops loot, so neither the parked
// slots (no golem flag yet) nor summoned golems are drop sources.
bool IsGolemSlot(const Monster &monster)
{
	return monster.isPlayerMinion() || monster.position.tile == GolemHoldingCell;
}

std::vector<Drop> DryRunLevelDrops()
{
	std::vector<Drop> drops;
	for (uint8_t k = 0; k < ActiveItemCount; k++)
		drops.push_back({ SourceKind::Other, FloorSource, "Floor", Items[ActiveItems[k]], Items[ActiveItems[k]].position });
	for (size_t i = 0; i < ActiveMonsterCount; i++) {
		Monster &monster = Monsters[ActiveMonsters[i]];
		if (IsGolemSlot(monster))
			continue;
		const SourceKind kind = monster.isUnique() ? SourceKind::UniqueMonster : SourceKind::Monster;
		for (Item &item : DryRun([&]() { SpawnMonsterLoot(monster); }))
			drops.push_back({ kind, ActiveMonsters[i], std::string(monster.name()), std::move(item), monster.position.tile });
	}
	for (int i = 0; i < ActiveObjectCount; i++) {
		Object &object = Objects[ActiveObjects[i]];
		if (object._otype == OBJ_SLAINHERO) {
			for (HeroClass heroClass : SlainHeroClasses) {
				for (Item &item : DryRun([&]() { SpawnSlainHeroLoot(object, heroClass); }))
					drops.push_back({ SourceKind::Other, ActiveObjects[i], SlainHeroName(heroClass), std::move(item), object.position });
			}
			continue;
		}
		const std::optional<SourceKind> kind = ContainerKind(object);
		if (!kind)
			continue;
		for (Item &item : DryRun([&]() { SpawnContainerLoot(object); }))
			drops.push_back({ *kind, ActiveObjects[i], ContainerName(object), std::move(item), object.position });
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
		if (IsGolemSlot(monster))
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

// The quantities an affix's roll can show up in: the first or second number the item panel shows for it,
// and the durability change, which isn't shown and is a percentage of the base item's durability.
std::array<int, 3> RollQuantities(const Item &item, const std::array<std::string, 2> &numbers)
{
	std::array<int, 3> quantities {};
	for (size_t i = 0; i < numbers.size(); i++)
		quantities[i] = numbers[i].empty() ? 0 : std::abs(std::stoi(numbers[i]));
	if (item.IDidx == IDI_NONE)
		return quantities;
	const int base = AllItemsList[item.IDidx].iDurability;
	if (base != 0 && item._iMaxDur != DUR_INDESTRUCTIBLE)
		quantities[2] = std::abs(item._iMaxDur - base) * 100 / base;
	return quantities;
}

// How an affix's roll shows on an item, learned from the game: apply the affix with SaveItemPower under many
// seeds and see which quantities change and between which values. An affix can roll more than one (King's
// rolls its damage and, separately, its to-hit); one that changes nothing has no roll (fire damage, attack
// speed and the like copy fixed values from their table entries).
struct RollRange {
	size_t quantity;
	int lowest;
	int highest;
};

std::vector<RollRange> MeasureRollRanges(const PLStruct &affix)
{
	static const _item_indexes Weapon = []() {
		for (int i = IDI_GOLD; i <= IDI_LAST; i++) {
			if (AllItemsList[i].itype == ItemType::Sword && AllItemsList[i].iDurability > 0)
				return static_cast<_item_indexes>(i);
		}
		app_fatal("No sword to measure affix rolls on");
	}();

	const uint32_t rngState = GetLCGEngineState();
	std::array<int, 3> lowest;
	std::array<int, 3> highest;
	lowest.fill(std::numeric_limits<int>::max());
	highest.fill(std::numeric_limits<int>::min());
	// Each roll has at most about a hundred values, so a few thousand seeds reach both ends of every one.
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

	std::vector<RollRange> ranges;
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

// How good an affix's roll is, 0 to 100: where it sits between the lowest and highest the game can roll,
// taking the weakest part when the affix rolls more than one value. Affixes without a roll count as 100, so
// --min-roll never rules them out. Nothing when a shown number doesn't fit this table entry's ranges.
std::optional<int> RollPercent(const PLStruct &affix, const Item &item, const std::array<std::string, 2> &numbers)
{
	const std::array<int, 3> quantities = RollQuantities(item, numbers);
	int weakest = 100;
	for (const RollRange &range : GetRollRanges(affix)) {
		int percent = (quantities[range.quantity] - range.lowest) * 100 / (range.highest - range.lowest);
		// The durability change is rounded down from a percentage of the base item, so it can land just outside.
		if (range.quantity == 2)
			percent = std::clamp(percent, 0, 100);
		if (percent < 0 || percent > 100)
			return std::nullopt;
		weakest = std::min(weakest, percent);
	}
	return weakest;
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

// Columns in the order of ITEM_HEADER in test/drop_stats/search_drop_stats.py; near the end, the tile the source starts
// on, then how well an armor rolled its own armor class.
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
	const bool isBook = item._iMiscId == IMISC_BOOK;
	const bool hasSpell = item._iSpell != SpellID::Null && item._iMiscId == IMISC_STAFF;

	// A staff's spell takes the suffix's place (such a staff never has a suffix: GetStaffSpell), so it's written
	// as the suffix, its charges as the value, and the roll is where the charges fell in the spell's range,
	// before a Plentiful or Bountiful prefix multiplied them.
	std::string suffixName = suffix.affix != nullptr ? CsvField(suffix.affix->PLName) : "";
	std::string suffixShown = CsvField(suffixText);
	std::string suffixValue = suffix.affix != nullptr ? suffixNumbers[0] : "";
	std::string suffixRoll = RollText(suffix.roll);
	if (hasSpell && suffix.affix == nullptr && !isUnique) {
		const SpellData &spell = GetSpellData(item._iSpell);
		const int multiplier = prefix.affix != nullptr && prefix.affix->power.type == IPL_CHARGES ? std::max(prefix.affix->power.param1, 1) : 1;
		const int charges = item._iMaxCharges / multiplier;
		const int range = spell.sStaffMax - spell.sStaffMin;
		suffixName = CsvField(spell.sNameText);
		suffixShown = fmt::format("{} charges", item._iMaxCharges);
		suffixValue = std::to_string(item._iMaxCharges);
		suffixRoll = std::to_string(range > 0 ? std::clamp((charges - spell.sStaffMin) * 100 / range, 0, 100) : 100);
	}
	// A book's spell is in its name the same way ("Book of Apocalypse"), and has nothing that rolls.
	if (isBook && item._iSpell != SpellID::Null) {
		suffixName = CsvField(GetSpellData(item._iSpell).sNameText);
		suffixShown = "";
		suffixValue = "";
		suffixRoll = "100";
	}

	// An armor, helm or shield rolls its own armor class between its base's least and most (GetItemAttrs), apart from
	// any affix's armor-class percentage; a unique's is set by the unique, so it has none.
	const ItemData &baseData = AllItemsList[item.IDidx];
	const std::string baseRoll = !isUnique && baseData.iMaxAC > baseData.iMinAC
	    ? std::to_string(std::clamp((item._iAC - baseData.iMinAC) * 100 / (baseData.iMaxAC - baseData.iMinAC), 0, 100))
	    : "";

	return fmt::format("{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{}\n",
	    gameSeed, static_cast<int>(difficulty), level.dlvl, level.setLevel == SL_NONE ? "" : CsvField(GetSetLevelQuest(level.setLevel).name),
	    SourceKindName(drop.kind), CsvField(drop.sourceName), drop.sourceIndex,
	    isBook ? "book" : ItemTypeName(item._itype), CsvField(AllItemsList[item.IDidx].iName), item._iCreateInfo & CF_LEVEL,
	    isUnique ? "unique" : (item._iMagical == ITEM_QUALITY_MAGIC ? "magic" : "normal"),
	    prefix.affix != nullptr ? CsvField(prefix.affix->PLName) : "", CsvField(prefixText), prefix.affix != nullptr ? prefixNumbers[0] : "", prefix.affix != nullptr ? prefixNumbers[1] : "",
	    suffixName, suffixShown, suffixValue, suffix.affix != nullptr ? suffixNumbers[1] : "",
	    isUnique ? CsvField(UniqueItems[item._iUid].UIName) : "", hasSpell ? CsvField(GetSpellData(item._iSpell).sNameText) : "", hasSpell ? std::to_string(item._iMaxCharges) : "",
	    item._iMinDam, item._iMaxDam, item._iAC, item._iMaxDur, item._iMinStr, item._iMinMag, item._iMinDex,
	    item._iIvalue, CsvField(item._iIName), static_cast<int>(item.IDidx), item._iSeed, item._iCreateInfo,
	    RollText(prefix.roll), suffixRoll,
	    drop.position.x, drop.position.y, baseRoll);
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

// Everything a level holds before anything is killed or opened, as text to compare.
std::string LevelContents()
{
	std::string contents;
	for (size_t i = 0; i < ActiveMonsterCount; i++) {
		const Monster &monster = Monsters[ActiveMonsters[i]];
		contents += fmt::format("m{} {} {},{} {}\n", ActiveMonsters[i], static_cast<int>(monster.type().type), monster.position.tile.x, monster.position.tile.y, monster.rndItemSeed);
	}
	for (int i = 0; i < ActiveObjectCount; i++) {
		const Object &object = Objects[ActiveObjects[i]];
		contents += fmt::format("o{} {} {},{} {} {} {} {}\n", ActiveObjects[i], static_cast<int>(object._otype), object.position.x, object.position.y, object._oRndSeed, object._oVar1, object._oVar2, object._oVar3);
	}
	for (uint8_t k = 0; k < ActiveItemCount; k++) {
		const Item &item = Items[ActiveItems[k]];
		contents += fmt::format("i {} {},{} {}\n", static_cast<int>(item.IDidx), item.position.x, item.position.y, item._iSeed);
	}
	return contents;
}

// If levels come out the same on every difficulty, each level only has to be generated once.
// The game's own LoadGameLevel, entered the way interfac.cpp does for stairs down (WM_DIABNEXTLVL) and for
// a quest entrance or Lazarus' red portal (WM_DIABSETLVL), with the quest's dungeon level loaded first.
void LoadLevelLikeTheGame(LevelId level)
{
	const bool headless = HeadlessMode;
	const bool music = gbMusicOn;
	HeadlessMode = true;
	gbMusicOn = false;
	if (level.setLevel == SL_NONE) {
		setlevel = false;
		currlevel = level.dlvl;
		leveltype = GetLevelType(currlevel);
		MyPlayer->setLevel(currlevel);
		LoadGameLevel(false, ENTRY_MAIN);
	} else {
		Quest &quest = Quests[GetSetLevelQuest(level.setLevel).quest];
		setlevel = false;
		currlevel = quest._qlevel;
		leveltype = GetLevelType(currlevel);
		MyPlayer->setLevel(currlevel);
		LoadGameLevel(false, ENTRY_MAIN);
		if (level.setLevel == SL_VILEBETRAYER && quest._qactive == QUEST_INIT) {
			quest._qactive = QUEST_ACTIVE;
			quest._qvar1 = 3;
		}
		setlvlnum = level.setLevel;
		setlvltype = quest._qlvltype;
		setlevel = true;
		leveltype = setlvltype;
		currlevel = static_cast<uint8_t>(setlvlnum);
		MyPlayer->setLevel(level.setLevel);
		LoadGameLevel(false, ENTRY_SETLVL);
	}
	HeadlessMode = headless;
	gbMusicOn = music;
}

// Drops seen in a real game (seed 1791094559, Normal, Full quests and Randomize Quests on). The level tests
// compare the simulator with the game's own code inside this test, so a setting both get wrong goes unnoticed;
// these come from outside it.
TEST_F(DropStats, MatchesDropsSeenInTheGame)
{
	struct Seen {
		LevelId level;
		int sourceIndex;
		const char *name;
	};
	const Seen seen[] = {
		{ { 1 }, 32, "Bronze Bow of weakness" },
		{ { 14 }, 48, "Breast Plate of vigor" },
		{ { 15, SL_VILEBETRAYER }, 40, "Blessed Mail of the ages" },
		{ { 15, SL_VILEBETRAYER }, 41, "Master's Short Sword" },
		{ { 15, SL_VILEBETRAYER }, 42, "Field Plate of the ages" },
	};
	for (const Seen &expected : seen) {
		StartMultiplayerGame(1791094559u, DIFF_NORMAL);
		GenerateLevel(expected.level);
		std::vector<std::string> names;
		for (const Drop &drop : DryRunLevelDrops()) {
			if (drop.sourceIndex == expected.sourceIndex)
				names.emplace_back(drop.item._iIName);
		}
		EXPECT_NE(std::find(names.begin(), names.end(), expected.name), names.end()) << LevelName(expected.level) << " source " << expected.sourceIndex << " should drop " << expected.name;
	}
}

TEST_F(DropStats, GenerationMatchesTheGame)
{
	size_t levels = 0;
	for (uint32_t gameSeed : { 42u, 1791094559u }) {
		StartMultiplayerGame(gameSeed, DIFF_NORMAL);
		for (LevelId level : ReachableLevels()) {
			StartMultiplayerGame(gameSeed, DIFF_NORMAL);
			GenerateLevel(level);
			const std::string simulated = LevelContents();
			StartMultiplayerGame(gameSeed, DIFF_NORMAL);
			LoadLevelLikeTheGame(level);
			EXPECT_EQ(LevelContents(), simulated) << "seed " << gameSeed << " " << LevelName(level);
			levels++;
		}
	}
	std::cout << "compared " << levels << " levels with the game's LoadGameLevel\n";
}

TEST_F(DropStats, LevelsAreTheSameOnEveryDifficulty)
{
	const QuestSettings settingsToCheck[] = { { true, true }, { true, false }, { false, true } };
	size_t levels = 0;
	for (const QuestSettings &settings : settingsToCheck) {
		Settings = settings;
		for (uint32_t gameSeed : { 1u, 42u, 123456789u, 987654321u, 1790867798u }) {
			StartMultiplayerGame(gameSeed, DIFF_NORMAL);
			for (LevelId level : ReachableLevels()) {
				StartMultiplayerGame(gameSeed, DIFF_NORMAL);
				GenerateLevel(level);
				const std::string normal = LevelContents();
				for (_difficulty difficulty : { DIFF_NIGHTMARE, DIFF_HELL }) {
					StartMultiplayerGame(gameSeed, difficulty);
					GenerateLevel(level);
					EXPECT_EQ(LevelContents(), normal) << "seed " << gameSeed << " " << LevelName(level) << " difficulty " << difficulty;
				}
				levels++;
			}
		}
	}
	Settings = {};
	std::cout << "compared " << levels << " levels across the three difficulties\n";
}

// The search generates a level on Normal and switches the difficulty only to work out the drops; that has to
// give the same drops as generating the level on that difficulty.
TEST_F(DropStats, SwitchingDifficultyAfterGenerationGivesTheSameDrops)
{
	size_t drops = 0;
	for (uint32_t gameSeed : { 1u, 42u, 1790867798u }) {
		StartMultiplayerGame(gameSeed, DIFF_NORMAL);
		for (LevelId level : ReachableLevels()) {
			for (_difficulty difficulty : { DIFF_NIGHTMARE, DIFF_HELL }) {
				StartMultiplayerGame(gameSeed, difficulty);
				GenerateLevel(level);
				const std::vector<Drop> generatedOnDifficulty = DryRunLevelDrops();

				StartMultiplayerGame(gameSeed, DIFF_NORMAL);
				GenerateLevel(level);
				sgGameInitInfo.nDifficulty = difficulty;
				const std::vector<Drop> switched = DryRunLevelDrops();

				EXPECT_EQ(GroupBySource(switched), GroupBySource(generatedOnDifficulty)) << "seed " << gameSeed << " " << LevelName(level) << " difficulty " << difficulty;
				drops += switched.size();
			}
		}
	}
	std::cout << "compared " << drops << " drops\n";
}

TEST_F(DropStats, DryRunMatchesRealDrops)
{
	const uint32_t gameSeeds[] = { 1, 42, 123456789, 987654321 };
	const _difficulty difficulties[] = { DIFF_NORMAL, DIFF_NIGHTMARE, DIFF_HELL };
	// Randomize Quests only matters with full quests.
	const QuestSettings settingsToCheck[] = { { true, true }, { true, false }, { false, true } };
	for (const QuestSettings &settings : settingsToCheck) {
		Settings = settings;
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
					ASSERT_EQ(GroupBySource(predicted), GroupBySource(actual)) << "full quests " << settings.fullQuests << " randomized " << settings.randomizeQuests
					                                                           << " seed " << gameSeed << " difficulty " << difficulty << " " << LevelName(level);
					comparedItems += actual.size();
					wantedItems += std::count_if(actual.begin(), actual.end(), [](const Drop &drop) { return IsWanted(drop.item); });
					if (level.setLevel != SL_NONE)
						setLevels++;
				}
			}
		}
		std::cout << "full quests " << settings.fullQuests << ", randomized quests " << settings.randomizeQuests << ": compared " << comparedItems
		          << " drops (" << setLevels << " set levels included), " << wantedItems << " two-affix or unique\n";
	}
	Settings = {};
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

/** The search's item types an affix can go on: "sword axe mace", "helm light_armor medium_armor heavy_armor". */
std::string AffixTypeNames(AffixItemType kinds)
{
	std::string names;
	const auto add = [&](AffixItemType kind, const char *types) {
		if (HasAnyOf(kinds, kind))
			names += StrCat(names.empty() ? "" : " ", types);
	};
	add(AffixItemType::Weapon, "sword axe mace");
	add(AffixItemType::Bow, "bow");
	add(AffixItemType::Staff, "staff");
	add(AffixItemType::Armor, "helm light_armor medium_armor heavy_armor");
	add(AffixItemType::Shield, "shield");
	add(AffixItemType::Misc, "ring amulet");
	return names;
}

/**
 * The highest level a dungeon item is made at in Diablo mode (dungeon levels 1-16). A monster's drop is made at its
 * kind's level (monster.data().level, the same on every difficulty), a unique monster's 4 higher (GetItemBLevel), and a
 * chest's or other object's at twice the dungeon level.
 */
int MostDropLevel()
{
	const auto inDiablo = [](const MonsterData &data) { return data.availability != MonsterAvailability::Never && data.minDunLvl <= 16; };
	int most = 2 * 16;
	for (int j = 0; j < NUM_MTYPES; j++) {
		if (inDiablo(MonstersData[j]))
			most = std::max<int>(most, MonstersData[j].level);
	}
	for (int j = 0; UniqueMonstersData[j].mtype != MT_INVALID; j++) {
		const MonsterData &data = MonstersData[UniqueMonstersData[j].mtype];
		if (inDiablo(data))
			most = std::max(most, data.level + 4);
	}
	return most;
}

// Writes every prefix, suffix, unique, base and spell name to DROPSTATS_NAMES_FILE, so searches can reject a
// misspelled name before simulating anything; and with them what a search checks a wishlist against before it starts:
// an affix's level and the item types it goes on, a base's type and most armor, and the highest level a drop is made at.
TEST_F(DropStats, DumpNames)
{
	const char *path = std::getenv("DROPSTATS_NAMES_FILE");
	if (path == nullptr)
		GTEST_SKIP() << "DROPSTATS_NAMES_FILE not set";
	std::ofstream names(path, std::ios::binary | std::ios::trunc);
	names << "kind,name,level,types,max_ac\n";
	for (int j = 0; ItemPrefixes[j].power.type != IPL_INVALID; j++)
		names << "prefix," << CsvField(ItemPrefixes[j].PLName) << "," << static_cast<int>(ItemPrefixes[j].PLMinLvl) << "," << AffixTypeNames(ItemPrefixes[j].PLIType) << ",\n";
	for (int j = 0; ItemSuffixes[j].power.type != IPL_INVALID; j++)
		names << "suffix," << CsvField(ItemSuffixes[j].PLName) << "," << static_cast<int>(ItemSuffixes[j].PLMinLvl) << "," << AffixTypeNames(ItemSuffixes[j].PLIType) << ",\n";
	for (int j = 0; UniqueItems[j].UIItemId != UITYPE_INVALID; j++)
		names << "unique," << CsvField(UniqueItems[j].UIName) << ",,,\n";
	for (int j = IDI_GOLD; j <= IDI_LAST; j++) {
		const ItemData &base = AllItemsList[j];
		names << "base," << CsvField(base.iName) << "," << static_cast<int>(base.iMinMLvl) << ","
		      << (base.iMiscId == IMISC_BOOK ? string_view("book") : ItemTypeName(base.itype)) << "," << static_cast<int>(base.iMaxAC) << "\n";
	}
	for (int8_t j = static_cast<int8_t>(SpellID::Firebolt); j <= static_cast<int8_t>(SpellID::LAST); j++)
		names << "spell," << CsvField(GetSpellData(static_cast<SpellID>(j)).sNameText) << ",,staff book,\n";
	names << "droplevel,most," << MostDropLevel() << ",,\n";
	ASSERT_TRUE(names) << "could not write " << path;
}

// The roll ranges come from the game, so check them against what the affixes are known to do: King's rolls
// its to-hit and its damage, Obsidian its resistance, structure the durability, and fire damage and Haste
// don't roll at all. Every rolled value must read 0 and 100 at the ends of its range.
TEST_F(DropStats, AffixRollRanges)
{
	StartMultiplayerGame(1, DIFF_NORMAL);
	auto find = [](const PLStruct *table, string_view name) -> const PLStruct & {
		for (int j = 0; table[j].power.type != IPL_INVALID; j++) {
			if (table[j].PLName == name)
				return table[j];
		}
		app_fatal(StrCat("No affix ", name));
	};
	auto quantities = [](const PLStruct &affix) {
		std::vector<size_t> found;
		for (const RollRange &range : GetRollRanges(affix))
			found.push_back(range.quantity);
		return found;
	};
	EXPECT_EQ(quantities(find(ItemPrefixes, "King's")), (std::vector<size_t> { 0, 1 }));
	EXPECT_EQ(quantities(find(ItemPrefixes, "Obsidian")), std::vector<size_t> { 0 });
	EXPECT_EQ(quantities(find(ItemSuffixes, "structure")), std::vector<size_t> { 2 });
	EXPECT_TRUE(quantities(find(ItemSuffixes, "flame")).empty());
	EXPECT_TRUE(quantities(find(ItemSuffixes, "haste")).empty());

	// Every affix Diablo mode can roll, on any kind of item.
	constexpr AffixItemType AnyItem = AffixItemType::Misc | AffixItemType::Bow | AffixItemType::Staff | AffixItemType::Weapon | AffixItemType::Shield | AffixItemType::Armor;
	int rolling = 0;
	for (const bool prefixes : { true, false }) {
		const PLStruct *table = prefixes ? ItemPrefixes : ItemSuffixes;
		for (int j = 0; table[j].power.type != IPL_INVALID; j++) {
			if (!(prefixes ? IsPrefixValidForItemType(j, AnyItem, false) : IsSuffixValidForItemType(j, AnyItem, false)))
				continue;
			const std::vector<RollRange> &ranges = GetRollRanges(table[j]);
			if (ranges.empty() || ranges[0].quantity == 2)
				continue;
			rolling++;
			std::array<std::string, 2> lowest;
			std::array<std::string, 2> highest;
			for (const RollRange &range : ranges) {
				lowest[range.quantity] = std::to_string(range.lowest);
				highest[range.quantity] = std::to_string(range.highest);
			}
			const Item item {};
			EXPECT_EQ(RollPercent(table[j], item, lowest), 0) << table[j].PLName;
			EXPECT_EQ(RollPercent(table[j], item, highest), 100) << table[j].PLName;
		}
	}
	EXPECT_GT(rolling, 100);
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
// DROPSTATS_STOP_AT (unix time), DROPSTATS_SKIP_LEVELS, DROPSTATS_WORKER, DROPSTATS_FULL_QUESTS and
// DROPSTATS_RANDOMIZE_QUESTS (0 or 1, default 1). Rows go to stdout: an "I," row per
// magic or unique item (columns as ItemCsvRow), then "G,<seed>,<difficulty>,<levels left out>" per game.
TEST_F(DropStats, SearchWorker)
{
	const std::optional<uint64_t> firstSeed = EnvNumber("DROPSTATS_FIRST_SEED");
	if (!firstSeed)
		GTEST_SKIP() << "DROPSTATS_FIRST_SEED not set";
	const uint64_t seedStep = std::max<uint64_t>(EnvNumber("DROPSTATS_SEED_STEP").value_or(1), 1);
	const std::optional<uint64_t> stopAt = EnvNumber("DROPSTATS_STOP_AT");
	const auto worker = static_cast<unsigned long long>(EnvNumber("DROPSTATS_WORKER").value_or(0));
	Settings.fullQuests = EnvNumber("DROPSTATS_FULL_QUESTS").value_or(1) != 0;
	Settings.randomizeQuests = EnvNumber("DROPSTATS_RANDOMIZE_QUESTS").value_or(1) != 0;

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
		std::string skipped;
		StartMultiplayerGame(gameSeed, DIFF_NORMAL);
		for (LevelId level : ReachableLevels()) {
			if (isSkipped(seed, level)) {
				skipped += (skipped.empty() ? "" : ";") + LevelName(level);
				continue;
			}
			currentDlvl = level.dlvl;
			currentSetLevel = level.setLevel;
			// A level is the same on every difficulty (LevelsAreTheSameOnEveryDifficulty); only the drops differ,
			// so it is generated once and its drops worked out for each difficulty.
			StartMultiplayerGame(gameSeed, DIFF_NORMAL);
			GenerateLevel(level);
			for (_difficulty difficulty : { DIFF_NORMAL, DIFF_NIGHTMARE, DIFF_HELL }) {
				sgGameInitInfo.nDifficulty = difficulty;
				for (const Drop &drop : DryRunLevelDrops()) {
					// Plain items aren't worth searching for, except books (normal quality, but their spell matters).
					if (drop.item._iMagical == ITEM_QUALITY_NORMAL && drop.item._iMiscId != IMISC_BOOK)
						continue;
					rows += "I,";
					rows += ItemCsvRow(gameSeed, difficulty, level, drop);
				}
			}
		}
		for (_difficulty difficulty : { DIFF_NORMAL, DIFF_NIGHTMARE, DIFF_HELL })
			rows += fmt::format("G,{},{},{}\n", gameSeed, static_cast<int>(difficulty), CsvField(skipped));
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
