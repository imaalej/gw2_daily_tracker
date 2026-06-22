#include "DataStore.h"
#include "../api/ResponseParser.h"
#include <set>
#include <array>
#include <memory>
#include <ctime>
#include <climits>
#include <future>
#include <algorithm>

namespace DailyTracker
{

// Forward declaration for logging
void LogMessage(int level, const std::string& channel, const std::string& message);

// Cache keys – defined only here, not in header
static constexpr const char* k_cacheBosses        = "world_bosses";
static constexpr const char* k_cacheBossesMeta    = "world_bosses_meta";
static constexpr const char* k_cacheMapChests     = "map_chests";
static constexpr const char* k_cacheMapChestsMeta = "map_chests_meta";

static constexpr auto k_fetchStallTimeout = std::chrono::seconds(60);
static constexpr int k_leyLinePreWindowSec  = 60;
static constexpr int k_leyLineEventLenSec   = 20 * 60;

// ---------------------------------------------------------------------------
DataStore::DataStore(Cache& cache, Settings& settings, APIManager& api)
    : m_cache(cache), m_settings(settings), m_api(api)
{
    m_nextDailyReset = Cache::NextDailyReset();
}

DataStore::~DataStore()
{
    m_stopBackground = true;
    m_queueCv.notify_all();
    if (m_backgroundThread.joinable())
        m_backgroundThread.join();
}

void DataStore::Initialize()
{
    // Load cache on main thread (quick, just reads a few JSON entries without heavy I/O)
    DailySnapshot snap;
    nlohmann::json cached;

    if (m_cache.Get(k_cacheBosses, cached))
    {
        auto completedIds = ResponseParser::ParseWorldBossCompletion(cached);
        nlohmann::json meta;
        if (m_cache.Get(k_cacheBossesMeta, meta))
            snap.worldBosses = ResponseParser::ParseWorldBossMeta(meta, completedIds);
    }

    if (m_cache.Get(k_cacheMapChestsMeta, cached))
    {
        nlohmann::json completion;
        m_cache.Get(k_cacheMapChests, completion);
        snap.mapChests = ResponseParser::ParseMapChests(cached, completion);
    }

    if (!snap.IsEmpty())
    {
        snap.lastRefreshed = std::chrono::system_clock::now();
        std::lock_guard<std::mutex> lk(m_snapshotMtx);
        m_snapshot = std::move(snap);
        SetStatus(FetchStatus::Ok, "Loaded from cache");
    }

    // Start the background thread
    m_backgroundThread = std::thread(&DataStore::BackgroundLoop, this);

    // Post initial fetch if key is present
    if (m_settings.HasApiKey())
        PostTask([this]() { BeginFetch(); });
    else
        SetStatus(FetchStatus::NoApiKey, "Enter your GW2 API key in Settings");
}

void DataStore::Tick()
{
    // Check for data update flag and fire callback
    if (m_dataUpdated.exchange(false) && m_onDataUpdated)
        m_onDataUpdated();
}

void DataStore::ForceRefresh()
{
    if (m_settings.HasApiKey())
        PostTask([this]() { BeginFetch(); });
}

void DataStore::OnApiKeyChanged()
{
    m_api.SetApiKey(m_settings.ApiKey());
    if (m_settings.HasApiKey())
        PostTask([this]() { BeginFetch(); });
    else
        SetStatus(FetchStatus::NoApiKey, "Enter your GW2 API key in Settings");
}

void DataStore::SetOnDataUpdated(std::function<void()> cb)
{
    m_onDataUpdated = std::move(cb);
}

DailySnapshot DataStore::GetSnapshot() const
{
    std::lock_guard<std::mutex> lk(m_snapshotMtx);
    return m_snapshot;
}

FetchStatus DataStore::GetStatus() const { return m_status.load(); }

std::string DataStore::GetStatusMessage() const
{
    std::lock_guard<std::mutex> lk(m_statusMsgMtx);
    return m_statusMessage;
}

// ---------------------------------------------------------------------------
// Background thread loop
// ---------------------------------------------------------------------------
void DataStore::BackgroundLoop()
{
    while (!m_stopBackground)
    {
        std::function<void()> task;
        {
            std::unique_lock<std::mutex> lk(m_queueMtx);
            m_queueCv.wait(lk, [this] { return !m_taskQueue.empty() || m_stopBackground; });
            if (m_stopBackground && m_taskQueue.empty())
                break;
            task = std::move(m_taskQueue.front());
            m_taskQueue.pop();
        }
        if (task)
            task();
    }
}

void DataStore::PostTask(std::function<void()> task)
{
    {
        std::lock_guard<std::mutex> lk(m_queueMtx);
        m_taskQueue.push(std::move(task));
    }
    m_queueCv.notify_one();
}

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------
void DataStore::SetStatus(FetchStatus s, const std::string& msg)
{
    m_status.store(s);
    std::lock_guard<std::mutex> lk(m_statusMsgMtx);
    m_statusMessage = msg;
}

void DataStore::OnFetchError(const std::string& msg, FetchStatus s)
{
    m_anyStepFailed.store(true);
    SetStatus(s, msg);
    DecrementAndFinalize("error-path");
}

void DataStore::DecrementAndFinalize(const char* stepName)
{
    int newVal = --m_pendingSteps;
    LogMessage(2, "DailyTracker",
        std::string("Fetch step finished: ") + (stepName ? stepName : "?")
        + " (pendingSteps now " + std::to_string(newVal) + ")");

    if (newVal == 0)
    {
        FinalizeFetch();
    }
    else if (newVal < 0)
    {
        LogMessage(3, "DailyTracker", "Warning: pendingSteps went negative! Resetting.");
        m_pendingSteps = 0;
        FinalizeFetch();
    }
}

void DataStore::CheckDailyReset()
{
    auto now = std::chrono::system_clock::now();
    if (now >= m_nextDailyReset)
    {
        LogMessage(2, "DailyTracker", "Daily reset detected – invalidating cache.");
        m_cache.InvalidateAll();
        m_cache.Save();
        m_nextDailyReset = Cache::NextDailyReset(now);
        m_pendingReset = false;
    }
}

// ---------------------------------------------------------------------------
// Fetch orchestration (all run on background thread)
// ---------------------------------------------------------------------------
void DataStore::BeginFetch()
{
    if (m_fetchInFlight.exchange(true))
        return;

    // Check for daily reset before fetching
    CheckDailyReset();

    m_anyStepFailed.store(false);
    m_pendingSteps  = kTotalFetchSteps;
    m_lastFetchTime = std::chrono::system_clock::now();
    SetStatus(FetchStatus::Fetching, "Refreshing data...");

    LogMessage(2, "DailyTracker", "BeginFetch: starting fetch cycle");

    {
        std::lock_guard<std::mutex> lk(m_pending.mtx);
        m_pending.worldBosses.clear();
        m_pending.mapChests.clear();
    }

    FetchStep_WorldBossCompletion();
    FetchStep_MapChestsMeta();
}

// ---------------------------------------------------------------------------
// World Boss Completion
// ---------------------------------------------------------------------------
void DataStore::FetchStep_WorldBossCompletion()
{
    LogMessage(2, "DailyTracker", "FetchStep_WorldBossCompletion: starting");

    auto doMeta = [this](std::set<std::string> completedIds)
    {
        FetchStep_WorldBossMeta(std::move(completedIds));
    };

    nlohmann::json cachedCompletion;
    if (m_cache.Get(k_cacheBosses, cachedCompletion))
    {
        try
        {
            auto completedIds = ResponseParser::ParseWorldBossCompletion(cachedCompletion);
            doMeta(std::move(completedIds));
        }
        catch (const std::exception& e)
        {
            OnFetchError(std::string("Cache parse error (world bosses): ") + e.what(),
                         FetchStatus::NetworkError);
        }
        return;
    }

    m_api.FetchWorldBossCompletion([this, doMeta](ApiResult res)
    {
        if (!res.success)
        {
            nlohmann::json staleCompletion;
            if (m_cache.Get(k_cacheBosses, staleCompletion, true))
            {
                LogMessage(3, "DailyTracker", "Using stale completion cache for world bosses");
                try
                {
                    auto ids = ResponseParser::ParseWorldBossCompletion(staleCompletion);
                    doMeta(std::move(ids));
                    return;
                }
                catch (const std::exception& e)
                {
                    OnFetchError(std::string("Stale cache parse error (world bosses): ") + e.what(),
                                 FetchStatus::NetworkError);
                    return;
                }
            }

            OnFetchError(res.errorMessage,
                res.httpStatus == 401 || res.httpStatus == 403
                    ? FetchStatus::AuthError
                    : FetchStatus::NetworkError);
            return;
        }

        try
        {
            m_cache.SetUntilDailyReset(k_cacheBosses, res.body);
            auto ids = ResponseParser::ParseWorldBossCompletion(res.body);
            doMeta(std::move(ids));
        }
        catch (const std::exception& e)
        {
            OnFetchError(std::string("Parse error (world bosses): ") + e.what(),
                         FetchStatus::NetworkError);
        }
    });
}

// ---------------------------------------------------------------------------
// World Boss Meta
// ---------------------------------------------------------------------------
void DataStore::FetchStep_WorldBossMeta(std::set<std::string> completedIds)
{
    LogMessage(2, "DailyTracker", "FetchStep_WorldBossMeta: starting");

    nlohmann::json cachedMeta;
    if (m_cache.Get(k_cacheBossesMeta, cachedMeta))
    {
        try
        {
            auto bosses = ResponseParser::ParseWorldBossMeta(cachedMeta, completedIds);
            std::lock_guard<std::mutex> lk(m_pending.mtx);
            m_pending.worldBosses = std::move(bosses);
            DecrementAndFinalize("WorldBossMeta(cache)");
        }
        catch (const std::exception& e)
        {
            OnFetchError(std::string("Cache parse error (world boss meta): ") + e.what(),
                         FetchStatus::NetworkError);
        }
        return;
    }

    const auto& lang = m_settings.Get().language;
    m_api.FetchWorldBossMeta(lang, [this, completedIds = std::move(completedIds)](ApiResult res)
    {
        if (!res.success)
        {
            nlohmann::json staleMeta;
            if (m_cache.Get(k_cacheBossesMeta, staleMeta, true))
            {
                LogMessage(3, "DailyTracker", "Using stale metadata for world bosses");
                try
                {
                    auto bosses = ResponseParser::ParseWorldBossMeta(staleMeta, completedIds);
                    std::lock_guard<std::mutex> lk(m_pending.mtx);
                    m_pending.worldBosses = std::move(bosses);
                    DecrementAndFinalize("WorldBossMeta(stale-fallback)");
                    return;
                }
                catch (const std::exception& e)
                {
                    LogMessage(3, "DailyTracker",
                        std::string("Stale metadata parse error (world bosses): ") + e.what());
                }
            }

            try
            {
                std::lock_guard<std::mutex> lk(m_pending.mtx);
                m_pending.worldBosses = ResponseParser::ParseWorldBossMeta(
                    nlohmann::json::array(), completedIds);
            }
            catch (const std::exception&) { }
            DecrementAndFinalize("WorldBossMeta(empty-fallback)");
            return;
        }

        try
        {
            m_cache.SetWithTTL(k_cacheBossesMeta, res.body, std::chrono::hours(24));
            auto bosses = ResponseParser::ParseWorldBossMeta(res.body, completedIds);
            std::lock_guard<std::mutex> lk(m_pending.mtx);
            m_pending.worldBosses = std::move(bosses);
            DecrementAndFinalize("WorldBossMeta(network)");
        }
        catch (const std::exception& e)
        {
            OnFetchError(std::string("Parse error (world boss meta): ") + e.what(),
                         FetchStatus::NetworkError);
        }
    });
}

// ---------------------------------------------------------------------------
// Map Chests Meta
// ---------------------------------------------------------------------------
void DataStore::FetchStep_MapChestsMeta()
{
    LogMessage(2, "DailyTracker", "FetchStep_MapChestsMeta: starting");

    auto doCompletion = [this](nlohmann::json metaJson)
    {
        FetchStep_MapChestsCompletion(std::move(metaJson));
    };

    nlohmann::json cachedMeta;
    if (m_cache.Get(k_cacheMapChestsMeta, cachedMeta))
    {
        doCompletion(std::move(cachedMeta));
        return;
    }

    const auto& lang = m_settings.Get().language;
    m_api.FetchMapChestMeta(lang, [this, doCompletion](ApiResult res)
    {
        if (!res.success)
        {
            nlohmann::json staleMeta;
            if (m_cache.Get(k_cacheMapChestsMeta, staleMeta, true))
            {
                LogMessage(3, "DailyTracker", "Using stale metadata for map chests");
                doCompletion(std::move(staleMeta));
                return;
            }
            doCompletion(nlohmann::json::array());
            return;
        }
        m_cache.SetWithTTL(k_cacheMapChestsMeta, res.body, std::chrono::hours(24));
        doCompletion(res.body);
    });
}

// ---------------------------------------------------------------------------
// Map Chests Completion
// ---------------------------------------------------------------------------
void DataStore::FetchStep_MapChestsCompletion(nlohmann::json metaJson)
{
    LogMessage(2, "DailyTracker", "FetchStep_MapChestsCompletion: starting");

    nlohmann::json cachedCompletion;
    if (m_cache.Get(k_cacheMapChests, cachedCompletion))
    {
        try
        {
            auto chests = ResponseParser::ParseMapChests(metaJson, cachedCompletion);
            std::lock_guard<std::mutex> lk(m_pending.mtx);
            m_pending.mapChests = std::move(chests);
            DecrementAndFinalize("MapChestsCompletion(cache)");
        }
        catch (const std::exception& e)
        {
            OnFetchError(std::string("Cache parse error (map chests): ") + e.what(),
                         FetchStatus::NetworkError);
        }
        return;
    }

    m_api.FetchMapChests([this, metaJson = std::move(metaJson)](ApiResult res)
    {
        if (!res.success)
        {
            nlohmann::json staleCompletion;
            if (m_cache.Get(k_cacheMapChests, staleCompletion, true))
            {
                LogMessage(3, "DailyTracker", "Using stale completion cache for map chests");
                try
                {
                    auto chests = ResponseParser::ParseMapChests(metaJson, staleCompletion);
                    std::lock_guard<std::mutex> lk(m_pending.mtx);
                    m_pending.mapChests = std::move(chests);
                    DecrementAndFinalize("MapChestsCompletion(stale-fallback)");
                    return;
                }
                catch (const std::exception& e)
                {
                    OnFetchError(std::string("Stale cache parse error (map chests): ") + e.what(),
                                 FetchStatus::NetworkError);
                    return;
                }
            }

            OnFetchError(res.errorMessage,
                res.httpStatus == 401 || res.httpStatus == 403
                    ? FetchStatus::AuthError
                    : FetchStatus::NetworkError);
            return;
        }

        try
        {
            m_cache.SetUntilDailyReset(k_cacheMapChests, res.body);
            auto chests = ResponseParser::ParseMapChests(metaJson, res.body);
            std::lock_guard<std::mutex> lk(m_pending.mtx);
            m_pending.mapChests = std::move(chests);
            DecrementAndFinalize("MapChestsCompletion(network)");
        }
        catch (const std::exception& e)
        {
            OnFetchError(std::string("Parse error (map chests): ") + e.what(),
                         FetchStatus::NetworkError);
        }
    });
}

// ---------------------------------------------------------------------------
// Finalize Fetch – called when all steps complete
// ---------------------------------------------------------------------------
void DataStore::FinalizeFetch()
{
    m_fetchInFlight = false;
    m_dataUpdated   = true;

    DailySnapshot snap;
    {
        std::lock_guard<std::mutex> lk(m_pending.mtx);
        snap.worldBosses = std::move(m_pending.worldBosses);
        snap.mapChests   = std::move(m_pending.mapChests);
    }
    snap.lastRefreshed = std::chrono::system_clock::now();
    {
        std::lock_guard<std::mutex> lk(m_snapshotMtx);
        snap.leyLineAnomaly = m_snapshot.leyLineAnomaly;
        m_snapshot = std::move(snap);
    }

    if (!m_anyStepFailed.load())
        SetStatus(FetchStatus::Ok, "");

    LogMessage(2, "DailyTracker", "FinalizeFetch: fetch cycle complete");

    try {
        m_cache.Save();
    } catch (const std::exception& e) {
        LogMessage(3, "DailyTracker", std::string("Failed to save cache: ") + e.what());
    } catch (...) {
        LogMessage(3, "DailyTracker", "Failed to save cache: unknown error");
    }
}

// ---------------------------------------------------------------------------
// Ley-Line Anomaly helpers (run on background thread)
// ---------------------------------------------------------------------------
void DataStore::LeyLine_RefreshNextOccurrence()
{
    const auto& schedule = GetLeyLineAnomalySchedule();
    if (schedule.empty())
        return;

    std::time_t nowUtc = std::time(nullptr);
    int secondsToday = static_cast<int>(nowUtc % 86400);

    int bestDiff = INT_MAX;
    const LeyLineAnomalySpot* best = nullptr;
    for (const auto& spot : schedule)
    {
        int diff = spot.spawnUtcSec - secondsToday;
        if (diff < 0)
            diff += 86400;
        if (diff < bestDiff)
        {
            bestDiff = diff;
            best = &spot;
        }
    }

    if (!best)
        return;

    std::lock_guard<std::mutex> lk(m_snapshotMtx);
    m_snapshot.leyLineAnomaly.nextSpawnUtcSec = best->spawnUtcSec;
    m_snapshot.leyLineAnomaly.nextMapName     = best->mapName;
    m_snapshot.leyLineAnomaly.nextMapId       = best->mapId;
}

int DataStore::LeyLine_SumTrackedItems(const nlohmann::json& bank,
                                       const nlohmann::json& materials,
                                       const nlohmann::json& charInventory) const
{
    int total = 0;
    total += ResponseParser::SumItemCount(bank,      kItemIdMysticCoin);
    total += ResponseParser::SumItemCount(bank,      kItemIdCrystallizedLeyEnergy);
    total += ResponseParser::SumItemCount(materials, kItemIdMysticCoin);
    total += ResponseParser::SumItemCount(materials, kItemIdCrystallizedLeyEnergy);
    total += ResponseParser::SumCharacterInventoryItemCount(charInventory, kItemIdMysticCoin);
    total += ResponseParser::SumCharacterInventoryItemCount(charInventory, kItemIdCrystallizedLeyEnergy);
    return total;
}

void DataStore::LeyLine_BeginWindowSample(const std::string& characterName)
{
    if (m_leyLineSampleInFlight.exchange(true))
        return;

    auto results = std::make_shared<std::array<nlohmann::json, 3>>();
    auto remaining = std::make_shared<std::atomic<int>>(3);

    auto onOneDone = [this, results, remaining]()
    {
        if (--(*remaining) != 0)
            return;

        try
        {
            int total = LeyLine_SumTrackedItems((*results)[0], (*results)[1], (*results)[2]);
            m_leyLineBaselineCount.store(total);
            LogMessage(2, "DailyTracker",
                "Ley-Line Anomaly: captured baseline item count = " + std::to_string(total));
        }
        catch (const std::exception& e)
        {
            LogMessage(3, "DailyTracker",
                std::string("Ley-Line Anomaly: baseline sample failed: ") + e.what());
            m_leyLineBaselineCount.store(-1);
        }
        m_leyLineSampleInFlight = false;
    };

    m_api.FetchAccountBank([results, onOneDone](ApiResult res)
    {
        if (res.success) (*results)[0] = res.body;
        onOneDone();
    });
    m_api.FetchAccountMaterials([results, onOneDone](ApiResult res)
    {
        if (res.success) (*results)[1] = res.body;
        onOneDone();
    });
    m_api.FetchCharacterInventory(characterName, [results, onOneDone](ApiResult res)
    {
        if (res.success) (*results)[2] = res.body;
        onOneDone();
    });
}

void DataStore::LeyLine_EndWindowSample(const std::string& characterName)
{
    auto results = std::make_shared<std::array<nlohmann::json, 3>>();
    auto remaining = std::make_shared<std::atomic<int>>(3);
    bool sawCorrectMap = m_leyLineSeenCorrectMap;
    int  baseline       = m_leyLineBaselineCount.load();

    auto onOneDone = [this, results, remaining, sawCorrectMap, baseline]()
    {
        if (--(*remaining) != 0)
            return;

        try
        {
            int finalCount = LeyLine_SumTrackedItems((*results)[0], (*results)[1], (*results)[2]);

            CompletionState state = CompletionState::Unknown;
            if (baseline >= 0)
            {
                bool countIncreased = finalCount > baseline;
                state = (sawCorrectMap && countIncreased)
                    ? CompletionState::Complete
                    : CompletionState::Incomplete;
            }

            LogMessage(2, "DailyTracker",
                "Ley-Line Anomaly: window ended. sawCorrectMap=" + std::string(sawCorrectMap ? "true" : "false")
                + " baseline=" + std::to_string(baseline) + " final=" + std::to_string(finalCount));

            std::lock_guard<std::mutex> lk(m_snapshotMtx);
            m_snapshot.leyLineAnomaly.completion = state;
        }
        catch (const std::exception& e)
        {
            LogMessage(3, "DailyTracker",
                std::string("Ley-Line Anomaly: end-of-window sample failed: ") + e.what());
        }

        m_leyLineSampleInFlight = false;
    };

    m_api.FetchAccountBank([results, onOneDone](ApiResult res)
    {
        if (res.success) (*results)[0] = res.body;
        onOneDone();
    });
    m_api.FetchAccountMaterials([results, onOneDone](ApiResult res)
    {
        if (res.success) (*results)[1] = res.body;
        onOneDone();
    });
    m_api.FetchCharacterInventory(characterName, [results, onOneDone](ApiResult res)
    {
        if (res.success) (*results)[2] = res.body;
        onOneDone();
    });
}

// ---------------------------------------------------------------------------
// Called from main thread – posts Ley-Line detection to background.
// ---------------------------------------------------------------------------
void DataStore::TickLeyLineAnomaly(int currentMapId, const std::string& currentCharacterName)
{
    if (!m_settings.HasApiKey() || currentCharacterName.empty())
        return;

    PostTask([this, currentMapId, currentCharacterName]()
    {
        const auto& schedule = GetLeyLineAnomalySchedule();
        if (schedule.empty())
            return;

        std::time_t nowUtc = std::time(nullptr);
        int secondsToday = static_cast<int>(nowUtc % 86400);

        const LeyLineAnomalySpot* activeSpot = nullptr;
        for (const auto& spot : schedule)
        {
            int delta = secondsToday - spot.spawnUtcSec;
            if (delta < -43200) delta += 86400;
            if (delta > 43200)  delta -= 86400;

            if (delta >= -k_leyLinePreWindowSec && delta <= k_leyLineEventLenSec)
            {
                activeSpot = &spot;
                break;
            }
        }

        if (activeSpot)
        {
            bool isNewOccurrence = (m_leyLinePhase == LeyLineWindowPhase::Idle)
                || (m_leyLineActiveOccurrenceSec != activeSpot->spawnUtcSec);

            if (isNewOccurrence)
            {
                m_leyLinePhase                = LeyLineWindowPhase::Sampling;
                m_leyLineActiveOccurrenceSec  = activeSpot->spawnUtcSec;
                m_leyLineBaselineCount.store(-1);
                m_leyLineSeenCorrectMap       = (currentMapId == activeSpot->mapId);

                {
                    std::lock_guard<std::mutex> lk(m_snapshotMtx);
                    m_snapshot.leyLineAnomaly.completion = CompletionState::Unknown;
                }

                LeyLine_BeginWindowSample(currentCharacterName);
            }
            else if (currentMapId == activeSpot->mapId)
            {
                m_leyLineSeenCorrectMap = true;
            }
        }
        else if (m_leyLinePhase == LeyLineWindowPhase::Sampling)
        {
            m_leyLinePhase = LeyLineWindowPhase::Idle;
            LeyLine_EndWindowSample(currentCharacterName);
        }

        LeyLine_RefreshNextOccurrence();
    });
}

} // namespace DailyTracker
