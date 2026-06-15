#include "DataStore.h"
#include "../api/ResponseParser.h"
#include <set>

namespace DailyTracker
{

// Cache keys
static constexpr const char* k_cacheWV        = "wizards_vault_daily";
static constexpr const char* k_cacheBosses     = "world_bosses";
static constexpr const char* k_cacheBossesMeta = "world_bosses_meta";
static constexpr const char* k_cacheHomeNodes  = "home_nodes";
static constexpr const char* k_cacheHomeNodesMeta = "home_nodes_meta";
static constexpr const char* k_cacheMapChests  = "map_chests";
static constexpr const char* k_cacheMapChestsMeta = "map_chests_meta";

// Wizard's Vault progress can change in <5 min, so use a short TTL
static constexpr auto k_wvTTL = std::chrono::seconds(120);

// ---------------------------------------------------------------------------
DataStore::DataStore(Cache& cache, Settings& settings, APIManager& api)
    : m_cache(cache), m_settings(settings), m_api(api)
{
    m_nextDailyReset = Cache::NextDailyReset();
}

void DataStore::Initialize()
{
    // Try to populate from cache first for instant UI on startup
    DailySnapshot snap;
    nlohmann::json cached;

    if (m_cache.Get(k_cacheWV, cached))
        snap.wizardsVault = ResponseParser::ParseWizardsVaultDaily(cached);

    if (m_cache.Get(k_cacheBosses, cached))
    {
        auto completedIds = ResponseParser::ParseWorldBossCompletion(cached);
        nlohmann::json meta;
        if (m_cache.Get(k_cacheBossesMeta, meta))
            snap.worldBosses = ResponseParser::ParseWorldBossMeta(meta, completedIds);
    }

    if (m_cache.Get(k_cacheHomeNodesMeta, cached))
    {
        nlohmann::json completion;
        m_cache.Get(k_cacheHomeNodes, completion);
        snap.homeNodes = ResponseParser::ParseHomeNodes(cached, completion);
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

    // If no API key yet, don't kick off a network fetch
    if (!m_settings.HasApiKey())
    {
        SetStatus(FetchStatus::NoApiKey, "Enter your GW2 API key in Settings");
        return;
    }

    BeginFetch();
}

// ---------------------------------------------------------------------------
void DataStore::Tick()
{
    // Check for daily reset
    CheckDailyReset();

    // Auto-refresh
    auto now = std::chrono::system_clock::now();
    auto interval = std::chrono::seconds(m_settings.Get().refreshIntervalSec);
    bool overdue  = (now - m_lastFetchTime) >= interval;

    if (overdue && !m_fetchInFlight && m_settings.HasApiKey())
        BeginFetch();

    // Fire data-updated callback if flagged
    if (m_dataUpdated.exchange(false) && m_onDataUpdated)
        m_onDataUpdated();
}

void DataStore::ForceRefresh()
{
    if (!m_fetchInFlight)
        BeginFetch();
}

void DataStore::OnApiKeyChanged()
{
    m_api.SetApiKey(m_settings.ApiKey());
    if (m_settings.HasApiKey())
        BeginFetch();
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

FetchStatus DataStore::GetStatus() const { return m_status; }

std::string DataStore::GetStatusMessage() const
{
    std::lock_guard<std::mutex> lk(m_statusMsgMtx);
    return m_statusMessage;
}

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------
void DataStore::SetStatus(FetchStatus s, const std::string& msg)
{
    m_status = s;
    std::lock_guard<std::mutex> lk(m_statusMsgMtx);
    m_statusMessage = msg;
}

void DataStore::OnFetchError(const std::string& msg, FetchStatus s)
{
    m_anyStepFailed = true;
    SetStatus(s, msg);
    DecrementAndFinalize();
}

void DataStore::DecrementAndFinalize()
{
    if (--m_pendingSteps == 0)
        FinalizeFetch();
}

// ---------------------------------------------------------------------------
void DataStore::CheckDailyReset()
{
    auto now = std::chrono::system_clock::now();
    if (now >= m_nextDailyReset)
    {
        m_cache.InvalidateAll();
        m_cache.Save();
        m_nextDailyReset = Cache::NextDailyReset(now);

        // Force immediate refresh after reset
        if (!m_fetchInFlight && m_settings.HasApiKey())
            BeginFetch();
    }
}

// ---------------------------------------------------------------------------
// Fetch orchestration
// ---------------------------------------------------------------------------
void DataStore::BeginFetch()
{
    if (m_fetchInFlight.exchange(true))
        return;

    m_anyStepFailed = false;
    m_pendingSteps  = 4;
    m_lastFetchTime = std::chrono::system_clock::now();
    SetStatus(FetchStatus::Fetching, "Refreshing data...");

    // Clear pending data (mutex stays unchanged)
    {
        std::lock_guard<std::mutex> lk(m_pending.mtx);
        m_pending.wizardsVault.clear();
        m_pending.worldBosses.clear();
        m_pending.homeNodes.clear();
        m_pending.mapChests.clear();
    }

    FetchStep_WizardsVault();
    FetchStep_WorldBossCompletion();
    FetchStep_HomeNodesMeta();
    FetchStep_MapChestsMeta();
}

// ---------------------------------------------------------------------------
void DataStore::FetchStep_WizardsVault()
{
    // Check cache first (short TTL since progress changes often)
    nlohmann::json cached;
    if (m_cache.Get(k_cacheWV, cached))
    {
        std::lock_guard<std::mutex> lk(m_pending.mtx);
        m_pending.wizardsVault = ResponseParser::ParseWizardsVaultDaily(cached);
        DecrementAndFinalize();
        return;
    }

    m_api.FetchWizardsVaultDaily([this](ApiResult res)
    {
        if (!res.success)
        {
            OnFetchError(res.errorMessage,
                res.httpStatus == 401 || res.httpStatus == 403
                    ? FetchStatus::AuthError
                    : FetchStatus::NetworkError);
            return;
        }

        m_cache.SetWithTTL(k_cacheWV, res.body, k_wvTTL);

        std::lock_guard<std::mutex> lk(m_pending.mtx);
        m_pending.wizardsVault = ResponseParser::ParseWizardsVaultDaily(res.body);
        DecrementAndFinalize();
    });
}

// ---------------------------------------------------------------------------
void DataStore::FetchStep_WorldBossCompletion()
{
    auto doMeta = [this](std::set<std::string> completedIds)
    {
        FetchStep_WorldBossMeta(std::move(completedIds));
    };

    nlohmann::json cachedCompletion;
    if (m_cache.Get(k_cacheBosses, cachedCompletion))
    {
        auto completedIds = ResponseParser::ParseWorldBossCompletion(cachedCompletion);
        doMeta(std::move(completedIds));
        return;
    }

    m_api.FetchWorldBossCompletion([this, doMeta](ApiResult res)
    {
        if (!res.success)
        {
            OnFetchError(res.errorMessage,
                res.httpStatus == 401 || res.httpStatus == 403
                    ? FetchStatus::AuthError
                    : FetchStatus::NetworkError);
            return;
        }
        m_cache.SetUntilDailyReset(k_cacheBosses, res.body);
        auto ids = ResponseParser::ParseWorldBossCompletion(res.body);
        doMeta(std::move(ids));
    });
}

void DataStore::FetchStep_WorldBossMeta(std::set<std::string> completedIds)
{
    nlohmann::json cachedMeta;
    if (m_cache.Get(k_cacheBossesMeta, cachedMeta))
    {
        auto bosses = ResponseParser::ParseWorldBossMeta(cachedMeta, completedIds);
        std::lock_guard<std::mutex> lk(m_pending.mtx);
        m_pending.worldBosses = std::move(bosses);
        DecrementAndFinalize();
        return;
    }

    const auto& lang = m_settings.Get().language;
    m_api.FetchWorldBossMeta(lang, [this, completedIds = std::move(completedIds)](ApiResult res)
    {
        if (!res.success)
        {
            // Non-fatal: we can still show bosses with fallback names
            std::lock_guard<std::mutex> lk(m_pending.mtx);
            m_pending.worldBosses = ResponseParser::ParseWorldBossMeta(
                nlohmann::json::array(), completedIds);
            DecrementAndFinalize();
            return;
        }
        // Meta doesn't change daily, use a 24-hour TTL
        m_cache.SetWithTTL(k_cacheBossesMeta, res.body, std::chrono::hours(24));
        auto bosses = ResponseParser::ParseWorldBossMeta(res.body, completedIds);
        std::lock_guard<std::mutex> lk(m_pending.mtx);
        m_pending.worldBosses = std::move(bosses);
        DecrementAndFinalize();
    });
}

// ---------------------------------------------------------------------------
void DataStore::FetchStep_HomeNodesMeta()
{
    auto doCompletion = [this](nlohmann::json metaJson)
    {
        FetchStep_HomeNodesCompletion(std::move(metaJson));
    };

    nlohmann::json cachedMeta;
    if (m_cache.Get(k_cacheHomeNodesMeta, cachedMeta))
    {
        doCompletion(std::move(cachedMeta));
        return;
    }

    const auto& lang = m_settings.Get().language;
    m_api.FetchHomeNodeMeta(lang, [this, doCompletion](ApiResult res)
    {
        if (!res.success)
        {
            // Non-fatal
            doCompletion(nlohmann::json::array());
            return;
        }
        m_cache.SetWithTTL(k_cacheHomeNodesMeta, res.body, std::chrono::hours(24));
        doCompletion(res.body);
    });
}

void DataStore::FetchStep_HomeNodesCompletion(nlohmann::json metaJson)
{
    nlohmann::json cachedCompletion;
    if (m_cache.Get(k_cacheHomeNodes, cachedCompletion))
    {
        auto nodes = ResponseParser::ParseHomeNodes(metaJson, cachedCompletion);
        std::lock_guard<std::mutex> lk(m_pending.mtx);
        m_pending.homeNodes = std::move(nodes);
        DecrementAndFinalize();
        return;
    }

    m_api.FetchHomeNodes([this, metaJson = std::move(metaJson)](ApiResult res)
    {
        if (!res.success)
        {
            OnFetchError(res.errorMessage,
                res.httpStatus == 401 || res.httpStatus == 403
                    ? FetchStatus::AuthError
                    : FetchStatus::NetworkError);
            return;
        }
        m_cache.SetUntilDailyReset(k_cacheHomeNodes, res.body);
        auto nodes = ResponseParser::ParseHomeNodes(metaJson, res.body);
        std::lock_guard<std::mutex> lk(m_pending.mtx);
        m_pending.homeNodes = std::move(nodes);
        DecrementAndFinalize();
    });
}

// ---------------------------------------------------------------------------
void DataStore::FetchStep_MapChestsMeta()
{
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
            doCompletion(nlohmann::json::array());
            return;
        }
        m_cache.SetWithTTL(k_cacheMapChestsMeta, res.body, std::chrono::hours(24));
        doCompletion(res.body);
    });
}

void DataStore::FetchStep_MapChestsCompletion(nlohmann::json metaJson)
{
    nlohmann::json cachedCompletion;
    if (m_cache.Get(k_cacheMapChests, cachedCompletion))
    {
        auto chests = ResponseParser::ParseMapChests(metaJson, cachedCompletion);
        std::lock_guard<std::mutex> lk(m_pending.mtx);
        m_pending.mapChests = std::move(chests);
        DecrementAndFinalize();
        return;
    }

    m_api.FetchMapChests([this, metaJson = std::move(metaJson)](ApiResult res)
    {
        if (!res.success)
        {
            OnFetchError(res.errorMessage,
                res.httpStatus == 401 || res.httpStatus == 403
                    ? FetchStatus::AuthError
                    : FetchStatus::NetworkError);
            return;
        }
        m_cache.SetUntilDailyReset(k_cacheMapChests, res.body);
        auto chests = ResponseParser::ParseMapChests(metaJson, res.body);
        std::lock_guard<std::mutex> lk(m_pending.mtx);
        m_pending.mapChests = std::move(chests);
        DecrementAndFinalize();
    });
}

// ---------------------------------------------------------------------------
// Called when all async steps finish (pendingSteps reaches 0)
// ---------------------------------------------------------------------------
void DataStore::FinalizeFetch()
{
    DailySnapshot snap;
    {
        std::lock_guard<std::mutex> lk(m_pending.mtx);
        snap.wizardsVault = std::move(m_pending.wizardsVault);
        snap.worldBosses  = std::move(m_pending.worldBosses);
        snap.homeNodes    = std::move(m_pending.homeNodes);
        snap.mapChests    = std::move(m_pending.mapChests);
    }
    snap.lastRefreshed = std::chrono::system_clock::now();

    {
        std::lock_guard<std::mutex> lk(m_snapshotMtx);
        m_snapshot = std::move(snap);
    }

    if (!m_anyStepFailed)
        SetStatus(FetchStatus::Ok, "");

    m_cache.Save();
    m_fetchInFlight = false;
    m_dataUpdated   = true;  // signals Tick() to fire callback
}

} // namespace DailyTracker
