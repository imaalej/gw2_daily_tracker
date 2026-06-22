#include "ResponseParser.h"
#include <algorithm>
#include <cctype>

namespace DailyTracker
{

// Forward declaration matching the one in APIManager.cpp / entry.cpp, so we
// can log from here without introducing a circular header dependency.
void LogMessage(int level, const std::string& channel, const std::string& message);

// ---------------------------------------------------------------------------
// /v2/account/worldbosses  →  ["shadow_behemoth", "tequatl_the_sunless", ...]
// ---------------------------------------------------------------------------
std::set<std::string>
ResponseParser::ParseWorldBossCompletion(const nlohmann::json& j)
{
    std::set<std::string> completed;
    if (!j.is_array())
        return completed;

    for (const auto& item : j)
    {
        if (item.is_string())
            completed.insert(item.get<std::string>());
    }
    return completed;
}

// ---------------------------------------------------------------------------
// /v2/worldbosses?ids=all  (array of boss objects)
// Example element: { "id": "shadow_behemoth" }  (minimal – name is the id
// pretty-printed; GW2 API doesn't return a display name for world bosses in
// this endpoint, so we capitalise the ID)
// ---------------------------------------------------------------------------
static std::string PrettifyId(const std::string& id)
{
    std::string pretty = id;
    // Replace underscores with spaces and capitalise each word
    bool capitalise = true;
    for (char& c : pretty)
    {
        if (c == '_') { c = ' '; capitalise = true; }
        else if (capitalise) { c = static_cast<char>(std::toupper(c)); capitalise = false; }
    }
    return pretty;
}

std::vector<WorldBossEntry>
ResponseParser::ParseWorldBossMeta(const nlohmann::json& j,
                                   const std::set<std::string>& completed)
{
    std::vector<WorldBossEntry> result;
    if (!j.is_array())
        return result;

    // Build a lookup from the schedule table
    const WorldBossScheduleEntry* schedule = GetWorldBossScheduleTable();

    for (const auto& item : j)
    {
        if (!item.is_object())
            continue;

        WorldBossEntry e;
        e.id   = SafeGet<std::string>(item, "id", "");
        e.name = PrettifyId(e.id);   // fallback; override if API gives name
        if (item.contains("name") && item["name"].is_string())
            e.name = item["name"].get<std::string>();

        e.completion = completed.count(e.id)
            ? CompletionState::Complete
            : CompletionState::Incomplete;

        // Populate spawn times from the built-in schedule table
        bool foundInSchedule = false;
        for (int i = 0; schedule[i].id != nullptr; ++i)
        {
            if (e.id == schedule[i].id)
            {
                for (int k = 0; schedule[i].spawnTimesUtcSec[k] != -1; ++k)
                    e.spawnTimesUtcSec.push_back(schedule[i].spawnTimesUtcSec[k]);
                foundInSchedule = true;
                break;
            }
        }

        if (!foundInSchedule)
        {
            // LOGL_WARNING (3): lets us identify exact missing/mismatched
            // IDs from the Nexus log so the schedule table can be updated.
            LogMessage(3, "DailyTracker", "World boss ID not in schedule: " + e.id);
        }

        result.push_back(std::move(e));
    }

    // Sort alphabetically for consistent display
    std::sort(result.begin(), result.end(),
              [](const WorldBossEntry& a, const WorldBossEntry& b)
              { return a.name < b.name; });

    return result;
}

// ---------------------------------------------------------------------------
// Item-count helpers for Ley-Line Anomaly completion detection.
// Shared shape across /v2/account/bank, /v2/account/materials, and the
// per-bag entries inside /v2/characters/:id/inventory:
//   [ { "id": 19976, "count": 3, ... }, null, { "id": 79047, "count": 1 } ]
// Null entries represent empty slots and are skipped.
// ---------------------------------------------------------------------------
int ResponseParser::SumItemCount(const nlohmann::json& itemArray, int itemId)
{
    int total = 0;
    if (!itemArray.is_array())
        return total;

    for (const auto& entry : itemArray)
    {
        if (!entry.is_object())
            continue; // null slot or malformed entry

        if (SafeGet<int>(entry, "id", -1) == itemId)
            total += SafeGet<int>(entry, "count", 0);
    }
    return total;
}

// /v2/characters/:id/inventory  →  { "bags": [ { "id":..., "size":...,
//   "inventory": [ {"id":19976,"count":3}, null, ... ] }, null, ... ] }
int ResponseParser::SumCharacterInventoryItemCount(const nlohmann::json& inventoryJson,
                                                   int itemId)
{
    int total = 0;
    if (!inventoryJson.is_object())
        return total;

    const auto& bags = inventoryJson.value("bags", nlohmann::json::array());
    if (!bags.is_array())
        return total;

    for (const auto& bag : bags)
    {
        if (!bag.is_object())
            continue; // empty bag slot

        const auto& slots = bag.value("inventory", nlohmann::json::array());
        total += SumItemCount(slots, itemId);
    }
    return total;
}

// ---------------------------------------------------------------------------
// Map chests (Hero's Choice Chests)
// /v2/mapchests?ids=all  →  [{ "id": "...", "name": "..." }, ...]
// /v2/account/mapchests  →  ["verdant_brink_heros_choice_chest", ...]
// ---------------------------------------------------------------------------
std::vector<MapChest>
ResponseParser::ParseMapChests(const nlohmann::json& metaJson,
                               const nlohmann::json& completionJson)
{
    std::set<std::string> earnedIds;
    if (completionJson.is_array())
    {
        for (const auto& item : completionJson)
            if (item.is_string())
                earnedIds.insert(item.get<std::string>());
    }

    std::vector<MapChest> result;
    if (!metaJson.is_array())
        return result;

    for (const auto& item : metaJson)
    {
        if (!item.is_object())
            continue;

        MapChest c;
        c.id         = SafeGet<std::string>(item, "id",   "");
        c.name       = SafeGet<std::string>(item, "name", PrettifyId(c.id));
        c.completion = earnedIds.count(c.id)
            ? CompletionState::Complete
            : CompletionState::Incomplete;
        result.push_back(std::move(c));
    }

    std::sort(result.begin(), result.end(),
              [](const MapChest& a, const MapChest& b)
              { return a.name < b.name; });

    return result;
}

} // namespace DailyTracker
