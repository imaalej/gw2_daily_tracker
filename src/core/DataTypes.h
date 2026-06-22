#pragma once
#include <string>
#include <vector>
#include <chrono>
#include <ctime>

namespace DailyTracker
{

// -------------------------------------------------------------------------
// Completion states
// -------------------------------------------------------------------------
enum class CompletionState : uint8_t
{
    Unknown     = 0,
    Incomplete  = 1,
    Complete    = 2,
};

// -------------------------------------------------------------------------
// World boss entry
// -------------------------------------------------------------------------
struct WorldBossEntry
{
    std::string id;             // e.g. "shadow_behemoth"
    std::string name;           // e.g. "Shadow Behemoth"
    CompletionState completion  = CompletionState::Unknown;

    // Scheduled spawn times (in seconds from midnight UTC)
    std::vector<int> spawnTimesUtcSec;

    // Returns seconds until next spawn, or -1 if unknown
    int SecondsUntilNextSpawn(std::time_t nowUtc) const;
};

// -------------------------------------------------------------------------
// Ley-Line Anomaly
// -------------------------------------------------------------------------
// The Ley-Line Anomaly has no API endpoint of its own. It rotates through
// three maps on a fixed, hardcoded schedule (sourced from the GW2 wiki):
// it appears in a different map every 2 hours, and returns to the same map
// every 6 hours. We detect completion heuristically: if the player was in
// the correct map during the ~20-minute event window, and their count of
// Mystic Coins (19976) or Crystallized Ley-Energy (79047) increased over
// that window, we consider the event completed for that occurrence.
struct LeyLineAnomalySpot
{
    int         mapId = 0;
    std::string mapName;
    int         spawnUtcSec = 0;   // seconds from midnight UTC
};

// Returns the built-in Ley-Line Anomaly rotation (sorted by spawnUtcSec).
const std::vector<LeyLineAnomalySpot>& GetLeyLineAnomalySchedule();

// Item IDs whose count increasing (while in the correct map, during the
// event window) we treat as a signal that the Ley-Line Anomaly was
// completed. See ResponseParser / DataStore for the detection logic.
inline constexpr int kItemIdMysticCoin             = 19976;
inline constexpr int kItemIdCrystallizedLeyEnergy  = 79047;

struct LeyLineAnomalyState
{
    // Whether today's "current cycle" occurrence has been detected as done.
    CompletionState completion = CompletionState::Unknown;

    // The next upcoming occurrence (earliest), as seconds-from-midnight UTC.
    int nextSpawnUtcSec = -1;
    std::string nextMapName;
    int nextMapId = 0;
};

// -------------------------------------------------------------------------
// Hero's Choice Chest (map chest)
// -------------------------------------------------------------------------
struct MapChest
{
    std::string id;             // e.g. "verdant_brink_heros_choice_chest"
    std::string name;           // e.g. "Hero's Choice Chest: Verdant Brink"
    CompletionState completion  = CompletionState::Unknown;
};

// -------------------------------------------------------------------------
// Full aggregated daily data snapshot
// -------------------------------------------------------------------------
struct DailySnapshot
{
    std::vector<WorldBossEntry>         worldBosses;
    std::vector<MapChest>               mapChests;
    LeyLineAnomalyState                 leyLineAnomaly;

    // Timestamp when data was last refreshed
    std::chrono::system_clock::time_point lastRefreshed;

    bool IsEmpty() const
    {
        return worldBosses.empty()
            && mapChests.empty();
    }
};

// -------------------------------------------------------------------------
// Settings
// -------------------------------------------------------------------------
struct AddonSettings
{
    // API key (stored obfuscated on disk via simple XOR; not cryptographically
    // secure, but prevents casual shoulder-surfing in settings files)
    std::string apiKey;

    // Refresh interval in seconds (default 300 = 5 min)
    int refreshIntervalSec = 300;

    // Category visibility
    bool showWorldBosses    = true;
    bool showMapChests      = true;
    bool showLeyLineAnomaly = true;

    // Window state
    bool windowVisible      = false;
    float windowPosX        = 100.f;
    float windowPosY        = 100.f;
    float windowWidth       = 420.f;
    float windowHeight      = 560.f;

    // Language for API requests
    std::string language    = "en";
};

// -------------------------------------------------------------------------
// Small helper: world-boss spawn schedule (UTC seconds from midnight)
// Sourced from the GW2 wiki / community data; kept in the data type file
// so it can also be referenced by tests without pulling in API logic.
// -------------------------------------------------------------------------
struct WorldBossScheduleEntry
{
    const char* id;
    const char* displayName;
    int         spawnTimesUtcSec[9];   // up to 8 spawns; 0-terminated
};

// Returns the built-in schedule table (null-terminated array)
const WorldBossScheduleEntry* GetWorldBossScheduleTable();

} // namespace DailyTracker
