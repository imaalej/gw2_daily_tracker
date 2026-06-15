#include "DataTypes.h"
#include <cmath>

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
    { "evolved_jungle_wurm",        "Evolved Jungle Wurm",                   { 2*3600,         6*3600,       10*3600+30*60,
                                                                               17*3600+30*60,  21*3600+30*60, 0 } },
    { "golem_mark_ii",              "Golem Mark II",                          { 1*3600,         5*3600,        9*3600,
                                                                               14*3600,        19*3600,       23*3600, 0 } },
    { "ulgoth_the_modniir",         "Ulgoth the Modniir",                     { 1*3600+30*60,   5*3600+30*60,  9*3600+30*60,
                                                                               14*3600+30*60,  19*3600+30*60, 23*3600+30*60, 0 } },
    { "claw_of_jormag",             "Claw of Jormag",                         { 2*3600+30*60,   4*3600+30*60,  6*3600+30*60,
                                                                                8*3600+30*60,  10*3600+30*60, 12*3600+30*60,
                                                                               14*3600+30*60, 0 } },
    { "svanir_shaman_chief",        "Svanir Shaman Chief",                   { 0,               2*3600,        4*3600,
                                                                                6*3600,          8*3600,       10*3600,
                                                                               12*3600, 0 } },
    { nullptr, nullptr, { 0 } } // sentinel
};

const WorldBossScheduleEntry* GetWorldBossScheduleTable()
{
    return s_worldBossSchedule;
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
