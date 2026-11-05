/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/**
 * @file town_growth.h Supply-driven town growth.
 *
 * A town grows from a growth budget measured in residents. Every year it earns
 * 2 * a * M * sqrt(P) residents, where P is the population, a is the base growth
 * constant (setting economy.town_growth_base, in thousandths) and M is the
 * product of the multipliers of the four needs (passengers, mail, goods,
 * electricity) and of the valuables bonus. Each need has a level 0..15 derived
 * from its coverage of a monthly norm; level 0 of an unlocked need blocks
 * growth. Valuables are a bonus: level 0 is neutral, level 5 adds 10%. The builder spends
 * the budget on houses laid out in 3x3 blocks surrounded by roads.
 *
 * Module ownership:
 *  - town_growth.cpp: needs, levels, multiplier and the budget (monthly + per tick).
 *  - town_power.cpp: power plants, their coal stock, town links and electricity allocation.
 *  - town_blocks.cpp: the 3x3 block builder that turns budget into roads and houses.
 */

#ifndef TOWN_GROWTH_H
#define TOWN_GROWTH_H

#include "town_growth_type.h"
#include "town_type.h"
#include "industry_type.h"
#include "cargo_type.h"
#include "strings_type.h"

struct Town;
struct Industry;

/** Balance constants of the growth model that are not exposed as settings. */
namespace TownGrowthBalance {
	/** Below this population a missing supply (level 0) never blocks growth. */
	static constexpr uint32_t GRACE_POPULATION = 1000;
	/** Population from which a need is unlocked; a locked need counts as at least neutral. */
	static constexpr std::array<uint32_t, to_underlying(TownNeed::End)> UNLOCK_POPULATION = {0, 1000, 2500, 5000, 0};
	/** Growth multiplier per need level 0..15, per mille: +0.3 for every level above the neutral 3 (level 5: x1.6, level 15: x4.6). */
	static constexpr std::array<uint16_t, TOWN_NEED_LEVEL_MAX + 1> LEVEL_MULTIPLIER = {0, 250, 700, 1000, 1300, 1600, 1900, 2200, 2500, 2800, 3100, 3400, 3700, 4000, 4300, 4600};
	/** Growth multiplier per level 0..15 of a bonus need (valuables), per mille: it only speeds growth up, 2% per level, up to 30%. */
	static constexpr std::array<uint16_t, TOWN_NEED_LEVEL_MAX + 1> BONUS_LEVEL_MULTIPLIER = {1000, 1020, 1040, 1060, 1080, 1100, 1120, 1140, 1160, 1180, 1200, 1220, 1240, 1260, 1280, 1300};
	/** Valuables a town wants per month for full coverage of the bonus: one unit per this many residents (at least one unit). */
	static constexpr uint32_t VALUABLES_RESIDENTS = 2000;
	/** Minimal coverage in percent for need levels 1..15 (index 0 is level 1); levels above 5 need more than the norm. */
	static constexpr std::array<uint16_t, TOWN_NEED_LEVEL_MAX> LEVEL_COVERAGE = {5, 25, 50, 75, 100, 120, 140, 160, 180, 200, 220, 240, 260, 280, 300};
	/** The norms of a need grow slower than the population: with population^0.9, per mille of the linear norm for 1000 * 2^i residents. */
	static constexpr std::array<uint16_t, 14> DEMAND_SCALE = {1000, 933, 871, 812, 758, 707, 660, 616, 574, 536, 500, 467, 435, 406};
	/** The budget is capped at this many years of growth. */
	static constexpr uint32_t BUDGET_CAP_YEARS = 2;
	/** A house is replaced by a denser one only when it is at least this many years old (not while the town has investments). */
	static constexpr uint8_t DENSIFY_MIN_AGE = 3;
	/** A house that suits its zone is still replaced by one with at least this many times its residents, per mille. */
	static constexpr uint32_t DENSIFY_SAME_ZONE_GAIN = 1500;
	/** Chance in percent that the builder first tries to replace a house by a denser one, before building on free plots. */
	static constexpr uint32_t DENSIFY_FIRST_CHANCE = 35;
	/** Highest chance in percent to densify first, reached with large investments. */
	static constexpr uint32_t DENSIFY_FIRST_CHANCE_MAX = 90;
	/** Amounts a company can invest into a town at once (base currency). */
	static constexpr std::array<int64_t, 3> INVESTMENT_AMOUNTS = {50'000'000, 1'000'000'000, 5'000'000'000};
	/** Investment unit of the bonus: the bonus grows by #INVESTMENT_BONUS_PER_DOUBLING for every doubling of 1 + investment / unit. */
	static constexpr int64_t INVESTMENT_BONUS_UNIT = 50'000'000;
	/** Growth bonus per doubling of 1 + investment / #INVESTMENT_BONUS_UNIT, per mille (50M: x1.6, 1G: x3.6, 5G: x5.0, 100G: x7.6). */
	static constexpr uint32_t INVESTMENT_BONUS_PER_DOUBLING = 600;
	/** While a town has investments, an unlocked need below this level counts as this level (it does not block growth). */
	static constexpr uint8_t INVESTMENT_MIN_LEVEL = 2;
	/** Every month 1/this of the invested money runs out (about two years for most of it). */
	static constexpr int64_t INVESTMENT_MONTHS = 24;
	/** The last of an investment runs out at once when less than this is left. */
	static constexpr int64_t INVESTMENT_MIN_REMAINDER = 1'000'000;
	/** Ticks to wait before trying the builder again after it failed. */
	static constexpr uint16_t BUILDER_RETRY_TICKS = 74 * 10;
	/** Number of power plants a town links to: one plus one per this many residents (from 50000 on). */
	static constexpr uint32_t POWER_LINK_RESIDENTS = 100000;
	/** Maximum number of power plants linked to one town. */
	static constexpr uint32_t POWER_LINK_MAX = 8;
	/** Electricity units produced from one unit of fuel (coal). */
	static constexpr uint32_t POWER_PER_FUEL = 1;
}

/* --- Needs and growth budget (town_growth.cpp) --- */

void UpdateTownNeeds(Town *t);
void TownGrowthTick(Town *t);
uint32_t GetTownNeedDemand(const Town *t, TownNeed need);
uint32_t ScaleTownNeedDemand(uint64_t linear_demand, uint32_t population);
bool IsTownNeedUnlocked(const Town *t, TownNeed need);
uint32_t GetTownNeedUnlockPopulation(TownNeed need);
uint8_t GetTownNeedEffectiveLevel(const Town *t, TownNeed need);
uint8_t GetTownNeedLevelForCoverage(uint32_t coverage);
uint16_t GetTownNeedLevelMultiplier(uint8_t level);
uint16_t GetTownNeedMultiplier(TownNeed need, uint8_t level);
bool IsTownNeedBonus(TownNeed need);
bool IsTownGrowthCargo(CargoType cargo);
bool IsValuablesCargo(CargoType cargo);
uint32_t GetTownGrowthMultiplier(const Town *t);
uint32_t GetTownYearlyGrowth(const Town *t, uint32_t multiplier);
void SetTownNeedForced(Town *t, TownNeed need, uint8_t level);
StringID GetTownNeedName(TownNeed need);
uint32_t ScaleHousePopulation(uint32_t house_population);
uint32_t CalcTownInvestmentBonus(int64_t investment);
uint32_t GetTownInvestmentBonus(const Town *t);
uint32_t GetTownDensifyFirstChance(const Town *t);
void AddTownInvestment(Town *t, int64_t amount);

/* --- Power grid (town_power.cpp) --- */

bool IsElectricityAvailable();
bool IsPowerPlant(const Industry *i);
CargoType GetPowerPlantFuelCargo(const Industry *i);
uint32_t GetPowerPlantFuelStock(const Industry *i);
uint32_t GetTownWantedPowerLinks(const Town *t);
uint32_t GetTownPowerReceived(const Town *t);
void UpdatePowerGrid();
void RebuildTownPowerLinks();
void OnPowerPlantRemoved(IndustryID plant);

/* --- Block builder (town_blocks.cpp) --- */

/** Outcome of one step of the town builder. */
struct TownBuildResult {
	bool success = false; ///< Did the builder build anything (road or house)?
	int32_t residents = 0; ///< Population the step will add once its houses are completed (density applied).
};

TownBuildResult TownBuilderStep(Town *t, TownExpandModes modes);

/* --- Commands and the simulation log (town_growth_cmd.cpp) --- */

void LogTownGrowthSimulation(bool force);

#endif /* TOWN_GROWTH_H */
