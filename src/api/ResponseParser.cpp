#include "ResponseParser.h"
#include <algorithm>
#include <cctype>

namespace DailyTracker
{

// ---------------------------------------------------------------------------
// /v2/account/wizardsvault/daily
// Example response:
// {
//   "meta_progress_current": 2, "meta_progress_complete": 8,
//   "objectives": [
//     { "id": 1, "title": "Daily Login", "track": "PvE",
//       "acclaim": 10, "progress_current": 1, "progress_complete": 1,
//       "claimed": false }
//   ]
// }
// ---------------------------------------------------------------------------
std::vector<WizardsVaultObjective>
ResponseParser::ParseWizardsVaultDaily(const nlohmann::json& j)
{
    std::vector<WizardsVaultObjective> result;
    if (!j.is_object())
        return result;

    const auto& objs = j.value("objectives", nlohmann::json::array());
    if (!objs.is_array())
        return result;

    for (const auto& obj : objs)
    {
        if (!obj.is_object())
            continue;

        WizardsVaultObjective o;
        o.id              = SafeGet<int>(obj,  "id",                0);
        o.title           = SafeGet<std::string>(obj, "title",      "");
        o.progressCurrent = SafeGet<int>(obj,  "progress_current",  0);
        o.progressComplete= SafeGet<int>(obj,  "progress_complete", 1);
        o.claimed         = SafeGet<bool>(obj, "claimed",           false);
        result.push_back(std::move(o));
    }
    return result;
}

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
        for (int i = 0; schedule[i].id != nullptr; ++i)
        {
            if (e.id == schedule[i].id)
            {
                for (int k = 0; schedule[i].spawnTimesUtcSec[k] != 0; ++k)
                    e.spawnTimesUtcSec.push_back(schedule[i].spawnTimesUtcSec[k]);
                break;
            }
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
// Home nodes
// /v2/home/nodes?ids=all  →  [{ "id": "quartz_crystal", "name": "..." }, ...]
// /v2/account/home/nodes  →  ["quartz_crystal", "black_lion_statuette", ...]
//   (only IDs of nodes *unlocked* on the account; the API does NOT indicate
//    harvest status directly - if the node is in the account list it is
//    available to harvest, and considered "incomplete" unless the user has
//    harvested it.  The GW2 API does not expose per-day harvest status, so
//    we default to Incomplete and note this limitation in the UI.)
// ---------------------------------------------------------------------------
std::vector<HomeNode>
ResponseParser::ParseHomeNodes(const nlohmann::json& metaJson,
                               const nlohmann::json& completionJson)
{
    // Build set of unlocked node IDs
    std::set<std::string> unlockedIds;
    if (completionJson.is_array())
    {
        for (const auto& item : completionJson)
            if (item.is_string())
                unlockedIds.insert(item.get<std::string>());
    }

    std::vector<HomeNode> result;
    if (!metaJson.is_array())
        return result;

    for (const auto& item : metaJson)
    {
        if (!item.is_object())
            continue;

        std::string id = SafeGet<std::string>(item, "id", "");
        if (unlockedIds.empty() || unlockedIds.count(id))
        {
            HomeNode n;
            n.id         = id;
            n.name       = SafeGet<std::string>(item, "name", PrettifyId(id));
            // The API has no per-character harvest status; we mark Unknown
            // so the UI can show "?" rather than a false green/red.
            n.completion = CompletionState::Unknown;
            result.push_back(std::move(n));
        }
    }

    std::sort(result.begin(), result.end(),
              [](const HomeNode& a, const HomeNode& b)
              { return a.name < b.name; });

    return result;
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
