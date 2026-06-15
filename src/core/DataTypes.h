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
// Wizard's Vault daily objective
// -------------------------------------------------------------------------
struct WizardsVaultObjective
{
    int         id          = 0;
    std::string title;
    int         progressCurrent = 0;
    int         progressComplete = 0;    // total required
    bool        claimed     = false;

    CompletionState GetState() const
    {
        if (progressCurrent >= progressComplete && progressComplete > 0)
            return CompletionState::Complete;
        return CompletionState::Incomplete;
    }
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
// Home instance node
// -------------------------------------------------------------------------
struct HomeNode
{
    std::string id;             // e.g. "quartz_crystal"
    std::string name;           // e.g. "Quartz Crystal Formation"
    CompletionState completion  = CompletionState::Unknown;
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
    std::vector<WizardsVaultObjective>  wizardsVault;
    std::vector<WorldBossEntry>         worldBosses;
    std::vector<HomeNode>               homeNodes;
    std::vector<MapChest>               mapChests;

    // Timestamp when data was last refreshed
    std::chrono::system_clock::time_point lastRefreshed;

    bool IsEmpty() const
    {
        return wizardsVault.empty()
            && worldBosses.empty()
            && homeNodes.empty()
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
    bool showWizardsVault   = true;
    bool showWorldBosses    = true;
    bool showHomeNodes      = true;
    bool showMapChests      = true;

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
    int         spawnTimesUtcSec[8];   // up to 8 spawns; 0-terminated
};

// Returns the built-in schedule table (null-terminated array)
const WorldBossScheduleEntry* GetWorldBossScheduleTable();

} // namespace DailyTracker
