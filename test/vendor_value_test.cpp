#include <algorithm>
#include <cstdio>
#include <vector>

#include <gtest/gtest.h>

#include "engine/random.hpp"
#include "items.h"
#include "player.h"
#include "stores.h"

namespace devilution {
namespace {

void Setup()
{
	Players.resize(1);
	// MyPlayer must be set after CreatePlayer, or equipping the starting gear sends network messages.
	CreatePlayer(Players[0], HeroClass::Warrior);
	MyPlayer = &Players[0];
	gbIsHellfire = false;
	gbIsMultiplayer = true;
}

void PrintHeader(const char *title, const char *levelName)
{
	std::printf("\n%s\n%5s  median     p90     max   >=80000\n", title, levelName);
}

void PrintRow(int level, std::vector<int> &v)
{
	std::sort(v.begin(), v.end());
	size_t n = v.size();
	size_t hits = std::count_if(v.begin(), v.end(), [](int x) { return x >= 80000; });
	std::printf("%5d %7d %7d %7d  %5.2f%%\n", level, v[n / 2], v[n * 9 / 10], v.back(), 100.0 * hits / n);
}

} // namespace

TEST(VendorValue, Wirt)
{
	Setup();
	PrintHeader("Wirt, by character level", "clvl");
	for (int lvl = 10; lvl <= 50; lvl += 2) {
		MyPlayer->_pLevel = lvl;
		std::vector<int> v;
		for (int i = 0; i < 4000; i++) {
			SetRndSeed(i * 7919 + lvl);
			boyitem.clear();
			boylevel = 0;
			SpawnBoy(lvl);
			v.push_back(boyitem._iIvalue);
		}
		PrintRow(lvl, v);
	}
}

TEST(VendorValue, GriswoldPremium)
{
	Setup();
	PrintHeader("Griswold premium, all 6 slots pooled, by character level", "clvl");
	for (int lvl = 10; lvl <= 40; lvl += 2) {
		MyPlayer->_pLevel = lvl;
		std::vector<int> v;
		for (int i = 0; i < 1000; i++) {
			SetRndSeed(i * 7919 + lvl);
			for (Item &item : premiumitems)
				item.clear();
			numpremium = 0;
			premiumlevel = lvl;
			SpawnPremium(*MyPlayer);
			for (int j = 0; j < 6; j++)
				v.push_back(premiumitems[j]._iIvalue);
		}
		PrintRow(lvl, v);
	}
}

TEST(VendorValue, GriswoldRegular)
{
	Setup();
	PrintHeader("Griswold regular stock, by store level (clvl / 2 + 2, 6 to 16)", "slvl");
	for (int lvl = 6; lvl <= 16; lvl++) {
		std::vector<int> v;
		for (int i = 0; i < 1000; i++) {
			SetRndSeed(i * 7919 + lvl);
			SpawnSmith(lvl);
			for (const Item &item : smithitem)
				if (!item.isEmpty())
					v.push_back(item._iIvalue);
		}
		PrintRow(lvl, v);
	}
}

TEST(VendorValue, AdriaStaves)
{
	Setup();
	PrintHeader("Adria staves only, by store level (clvl / 2 + 2, 6 to 16)", "slvl");
	for (int lvl = 6; lvl <= 16; lvl++) {
		std::vector<int> v;
		for (int i = 0; i < 2000; i++) {
			SetRndSeed(i * 7919 + lvl);
			SpawnWitch(lvl);
			for (const Item &item : witchitem)
				if (!item.isEmpty() && item._itype == ItemType::Staff)
					v.push_back(item._iIvalue);
		}
		PrintRow(lvl, v);
	}
}

} // namespace devilution
