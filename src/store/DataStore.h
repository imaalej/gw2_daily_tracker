#pragma once
#include "../core/DataTypes.h"
#include "../core/Cache.h"
#include "../core/Settings.h"
#include "../api/APIManager.h"
#include <atomic>
#include <mutex>
#include <functional>
#include <chrono>
#include <string>
#include <set>
#include <string>

namespace DailyTracker
{

// Error / status enum shown in the UI status bar
enum class FetchStatus
{
    Idle,
    Fetching,
    Ok,
    NoApiKey,
    AuthError,      // 401 / 403
    NetworkError,
    ApiKeyMissing,
};

class DataStore
{
public:
    DataStore(Cache& cache, Settings& settings, APIManager& api);

    // Called on addon load to hydrate data from cache or kick off first fetch.
    void Initialize();

    // Called from AddonRender to tick refresh logic (main thread).
    // Will schedule background refreshes when the refresh interval elapses or
    // when the daily reset passes.
    void Tick();

    // Manually trigger a full data refresh (e.g., keybind or button press).
    void ForceRefresh();

    // Thread-safe snapshot of the latest data.
    DailySnapshot GetSnapshot() const;

    // Current fetch/error status for the UI to display.
    FetchStatus GetStatus() const;
    std::string GetStatusMessage() const;

    // Called whenever the API key changes in settings.
    void OnApiKeyChanged();

    // Register a callback to be invoked on the main thread after data updates.
    // The DataStore posts a notification flag; the UI polls it via Tick().
    void SetOnDataUpdated(std::function<void()> cb);

private:
    // Fetches all data categories asynchronously.
    void BeginFetch();

    // Individual fetch steps – called sequentially on the worker thread
    // via chained callbacks so we don't fire all requests simultaneously.
    void FetchStep_WorldBossCompletion();
    void FetchStep_WorldBossMeta(std::set<std::string> completedIds);
    void FetchStep_WizardsVault();
    void FetchStep_HomeNodesMeta();
    void FetchStep_HomeNodesCompletion(nlohmann::json metaJson);
    void FetchStep_MapChestsMeta();
    void FetchStep_MapChestsCompletion(nlohmann::json metaJson);
    void FinalizeFetch();

    // Checks if the daily reset has occurred since the last fetch
    // and invalidates the cache if so.
    void CheckDailyReset();

    Cache&      m_cache;
    Settings&   m_settings;
    APIManager& m_api;

    mutable std::mutex m_snapshotMtx;
    DailySnapshot      m_snapshot;

    std::atomic<FetchStatus> m_status { FetchStatus::Idle };
    mutable std::mutex m_statusMsgMtx;
    std::string        m_statusMessage;

    std::atomic<bool>  m_fetchInFlight { false };
    std::atomic<bool>  m_dataUpdated   { false };

    std::chrono::system_clock::time_point m_lastFetchTime;
    std::chrono::system_clock::time_point m_nextDailyReset;

    std::function<void()> m_onDataUpdated;

    // Partial results accumulated across async steps
    struct PendingData
    {
        std::vector<WizardsVaultObjective> wizardsVault;
        std::vector<WorldBossEntry>        worldBosses;
        std::vector<HomeNode>              homeNodes;
        std::vector<MapChest>              mapChests;
        std::mutex                         mtx;
    };
    PendingData m_pending;

    // Tracks how many async steps are still outstanding so we know when done.
    std::atomic<int>  m_pendingSteps { 0 };
    std::atomic<bool> m_anyStepFailed{ false };

    void SetStatus(FetchStatus s, const std::string& msg = "");
    void OnFetchError(const std::string& msg, FetchStatus s);
    void DecrementAndFinalize();
};

} // namespace DailyTracker
