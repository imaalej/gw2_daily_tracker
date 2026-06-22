#pragma once
#include "../core/DataTypes.h"
#include "json.hpp"
#include <set>
#include <string>

namespace DailyTracker
{

// Parses raw GW2 API JSON responses into the addon's typed structures.
// All functions accept the parsed nlohmann::json object directly.
// They never throw; on malformed input they return empty/default values.
class ResponseParser
{
public:
    // -----------------------------------------------------------------------
    // /v2/account/worldbosses
    // Returns a set of boss IDs completed since the last daily reset.
    // -----------------------------------------------------------------------
    static std::set<std::string>
        ParseWorldBossCompletion(const nlohmann::json& j);

    // -----------------------------------------------------------------------
    // /v2/worldbosses?ids=all
    // Returns a list of WorldBossEntry with names populated.
    // spawnTimesUtcSec is filled from the built-in schedule table.
    // completion is set by cross-referencing with ParseWorldBossCompletion().
    // Any boss ID not found in the built-in schedule table is logged as a
    // warning so missing/mismatched IDs can be identified and added.
    // -----------------------------------------------------------------------
    static std::vector<WorldBossEntry>
        ParseWorldBossMeta(const nlohmann::json& j,
                           const std::set<std::string>& completed);

    // -----------------------------------------------------------------------
    // /v2/account/mapchests  +  /v2/mapchests?ids=all
    // Returns a list of MapChest with names and completion status.
    // -----------------------------------------------------------------------
    static std::vector<MapChest>
        ParseMapChests(const nlohmann::json& metaJson,
                       const nlohmann::json& completionJson);

    // -----------------------------------------------------------------------
    // /v2/account/bank, /v2/account/materials, /v2/characters/:id/inventory
    // Each returns an array of (possibly null) { "id": int, "count": int }
    // entries (materials/bank may omit "count" semantics differently, but
    // all three share this minimal shape). Sums the count for a given item
    // id across one such array. Safe on malformed/empty input (returns 0).
    // -----------------------------------------------------------------------
    static int SumItemCount(const nlohmann::json& itemArray, int itemId);

    // Same as above, but for the nested bags->inventory shape returned by
    // /v2/characters/:id/inventory.
    static int SumCharacterInventoryItemCount(const nlohmann::json& inventoryJson,
                                              int itemId);

private:
    // Safely retrieves a value; returns defaultValue if missing or wrong type.
    template<typename T>
    static T SafeGet(const nlohmann::json& obj, const std::string& key,
                     T defaultValue = T{})
    {
        try
        {
            if (obj.contains(key) && !obj[key].is_null())
                return obj[key].get<T>();
        }
        catch (...) {}
        return defaultValue;
    }
};

} // namespace DailyTracker
