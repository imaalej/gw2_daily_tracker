#include "DataTypes.h"
#include <cmath>
#include <algorithm>
#include <climits>

namespace DailyTracker
{

// ---------------------------------------------------------------------------
// World-boss static schedule table
// Times are seconds-from-midnight UTC.  Values from the community-maintained
// GW2 wiki timer page.  A trailing 0 marks the end of each boss's list.
// ---------------------------------------------------------------------------
static const WorldBossScheduleEntry s_worldBossSchedule[] =
{
    // id                           display name                               UTC times (sec)
    { "shadow_behemoth",            "Shadow Behemoth",                        { 0*3600+15*60,  2*3600+15*60,  4*3600+15*60,
                                                                                6*3600+15*60,  8*3600+15*60, 10*3600+15*60,
                                                                               12*3600+15*60, 0 } },
    { "the_shatterer",              "The Shatterer",                          { 1*3600,         3*3600,        5*3600,
                                                                                7*3600,         9*3600,       11*3600,
                                                                               13*3600, 0 } },
    { "great_jungle_wurm",          "Great Jungle Wurm",                      { 1*3600+30*60,   3*3600+30*60,  5*3600+30*60,
                                                                                7*3600+30*60,   9*3600+30*60, 11*3600+30*60,
                                                                               13*3600+30*60, 0 } },
    { "megadestroyer",              "Megadestroyer",                          { 0*3600+30*60,   2*3600+30*60,  4*3600+30*60,
                                                                                6*3600+30*60,   8*3600+30*60, 10*3600+30*60,
                                                                               12*3600+30*60, 0 } },
    { "fire_elemental",             "Fire Elemental",                         { 0*3600+45*60,   2*3600+45*60,  4*3600+45*60,
                                                                                6*3600+45*60,   8*3600+45*60, 10*3600+45*60,
                                                                               12*3600+45*60, 0 } },
    { "tequatl_the_sunless",        "Tequatl the Sunless",                    { 0*3600,         3*3600,        7*3600,
                                                                               11*3600+30*60,  19*3600,       23*3600, 0 } },
    { "triple_trouble_wurm",        "Triple Trouble Wurm",                    { 1*3600,         4*3600,        8*3600,
                                                                               12*3600+30*60,  20*3600, 0 } },
    { "karka_queen",                "Karka Queen",                            { 2*3600,         6*3600,       10*3600,
                                                                               15*3600,        23*3600+30*60, 0 } },
    { "inquest_golem_mark_ii",      "Inquest Golem Mark II",                  { 1*3600,         5*3600,        9*3600,
                                                                               14*3600,        19*3600,       23*3600, 0 } },
    { "modniir_ulgoth",             "Modniir Ulgoth",                         { 1*3600+30*60,   5*3600+30*60,  9*3600+30*60,
                                                                               14*3600+30*60,  19*3600+30*60, 23*3600+30*60, 0 } },
    { "claw_of_jormag",             "Claw of Jormag",                         { 2*3600+30*60,   4*3600+30*60,  6*3600+30*60,
                                                                                8*3600+30*60,  10*3600+30*60, 12*3600+30*60,
                                                                               14*3600+30*60, 0 } },
    { "svanir_shaman_chief",        "Svanir Shaman Chief",                   { 0,               2*3600,        4*3600,
                                                                                6*3600,          8*3600,       10*3600,
                                                                               12*3600, 0 } },
    { "admiral_taidha_covington",   "Admiral Taidha Covington",               { 0,               3*3600,        6*3600,
                                                                                9*3600,         12*3600,       15*3600,
                                                                               18*3600,         21*3600, 0 } },
    { nullptr, nullptr, { 0 } } // sentinel
};

const WorldBossScheduleEntry* GetWorldBossScheduleTable()
{
    return s_worldBossSchedule;
}

// ---------------------------------------------------------------------------
// Ley-Line Anomaly static schedule table
// Hardcoded per the design doc; the anomaly has no API endpoint of its own.
// Spawn times were given in UTC-4 (Eastern, no DST adjustment per spec) and
// are converted to UTC-seconds-from-midnight here. It spawns every 2 hours,
// rotating through three maps, returning to the same map every 6 hours:
//   Timberline Falls : 20:20, 02:20, 08:20, 14:20 (UTC-4)
//   Iron Marches     : 22:20, 04:20, 10:20, 16:20 (UTC-4)
//   Gendarran Fields : 00:20, 06:20, 12:20, 18:20 (UTC-4)
// UTC-4 -> UTC: add 4 hours (mod 24).
// ---------------------------------------------------------------------------
static constexpr int kMapIdTimberlineFalls = 29;
static constexpr int kMapIdIronMarches     = 25;
static constexpr int kMapIdGendarranFields = 24;

// Converts an "HH:MM UTC-4" wall time into seconds-from-midnight UTC.
static constexpr int UtcMinus4ToUtcSec(int hourUtcMinus4, int minuteUtcMinus4)
{
    int totalMin = hourUtcMinus4 * 60 + minuteUtcMinus4 + 4 * 60; // shift +4h to UTC
    totalMin %= (24 * 60);
    return totalMin * 60;
}

static const std::vector<LeyLineAnomalySpot>& BuildLeyLineAnomalySchedule()
{
    static const std::vector<LeyLineAnomalySpot> s_schedule = []
    {
        std::vector<LeyLineAnomalySpot> spots = {
            { kMapIdTimberlineFalls, "Timberline Falls", UtcMinus4ToUtcSec(20, 20) },
            { kMapIdTimberlineFalls, "Timberline Falls", UtcMinus4ToUtcSec(2,  20) },
            { kMapIdTimberlineFalls, "Timberline Falls", UtcMinus4ToUtcSec(8,  20) },
            { kMapIdTimberlineFalls, "Timberline Falls", UtcMinus4ToUtcSec(14, 20) },

            { kMapIdIronMarches,     "Iron Marches",     UtcMinus4ToUtcSec(22, 20) },
            { kMapIdIronMarches,     "Iron Marches",     UtcMinus4ToUtcSec(4,  20) },
            { kMapIdIronMarches,     "Iron Marches",     UtcMinus4ToUtcSec(10, 20) },
            { kMapIdIronMarches,     "Iron Marches",     UtcMinus4ToUtcSec(16, 20) },

            { kMapIdGendarranFields, "Gendarran Fields",  UtcMinus4ToUtcSec(0,  20) },
            { kMapIdGendarranFields, "Gendarran Fields",  UtcMinus4ToUtcSec(6,  20) },
            { kMapIdGendarranFields, "Gendarran Fields",  UtcMinus4ToUtcSec(12, 20) },
            { kMapIdGendarranFields, "Gendarran Fields",  UtcMinus4ToUtcSec(18, 20) },
        };

        std::sort(spots.begin(), spots.end(),
            [](const LeyLineAnomalySpot& a, const LeyLineAnomalySpot& b)
            { return a.spawnUtcSec < b.spawnUtcSec; });

        return spots;
    }();

    return s_schedule;
}

const std::vector<LeyLineAnomalySpot>& GetLeyLineAnomalySchedule()
{
    return BuildLeyLineAnomalySchedule();
}

// ---------------------------------------------------------------------------
// WorldBossEntry::SecondsUntilNextSpawn
// ---------------------------------------------------------------------------
int WorldBossEntry::SecondsUntilNextSpawn(std::time_t nowUtc) const
{
    if (spawnTimesUtcSec.empty())
        return -1;

    // Seconds elapsed since start of today (UTC midnight)
    int secondsToday = static_cast<int>(nowUtc % 86400);

    int minPositiveDiff = INT_MAX;
    for (int spawnSec : spawnTimesUtcSec)
    {
        int diff = spawnSec - secondsToday;
        if (diff < 0)
            diff += 86400; // wrap to next occurrence tomorrow
        if (diff < minPositiveDiff)
            minPositiveDiff = diff;
    }
    return (minPositiveDiff == INT_MAX) ? -1 : minPositiveDiff;
}

} // namespace DailyTracker
