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
    // /v2/account/wizardsvault/daily
    // Returns an array of WizardsVaultObjective populated from the JSON.
    // -----------------------------------------------------------------------
    static std::vector<WizardsVaultObjective>
        ParseWizardsVaultDaily(const nlohmann::json& j);

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
    // -----------------------------------------------------------------------
    static std::vector<WorldBossEntry>
        ParseWorldBossMeta(const nlohmann::json& j,
                           const std::set<std::string>& completed);

    // -----------------------------------------------------------------------
    // /v2/account/home/nodes  +  /v2/home/nodes?ids=all
    // Returns a list of HomeNode with names and completion status.
    // -----------------------------------------------------------------------
    static std::vector<HomeNode>
        ParseHomeNodes(const nlohmann::json& metaJson,
                       const nlohmann::json& completionJson);

    // -----------------------------------------------------------------------
    // /v2/account/mapchests  +  /v2/mapchests?ids=all
    // Returns a list of MapChest with names and completion status.
    // -----------------------------------------------------------------------
    static std::vector<MapChest>
        ParseMapChests(const nlohmann::json& metaJson,
                       const nlohmann::json& completionJson);

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
