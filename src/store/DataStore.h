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

    // -----------------------------------------------------------------------
    // Ley-Line Anomaly tracking.
    // Must be called every frame from the main thread (where Mumble link
    // data is available) so the DataStore can observe which map the player
    // is in during the ~1-minute pre-window before each occurrence.
    // currentCharacterName is needed to query that character's inventory.
    // -----------------------------------------------------------------------
    void TickLeyLineAnomaly(int currentMapId, const std::string& currentCharacterName);

private:
    // Fetches all data categories asynchronously.
    void BeginFetch();

    // Individual fetch steps – called sequentially on the worker thread
    // via chained callbacks so we don't fire all requests simultaneously.
    void FetchStep_WorldBossCompletion();
    void FetchStep_WorldBossMeta(std::set<std::string> completedIds);
    void FetchStep_MapChestsMeta();
    void FetchStep_MapChestsCompletion(nlohmann::json metaJson);
    void FinalizeFetch();

    // Checks if the daily reset has occurred since the last fetch
    // and invalidates the cache if so.
    void CheckDailyReset();

    // -----------------------------------------------------------------------
    // Ley-Line Anomaly internals
    // -----------------------------------------------------------------------
    // Begins sampling item counts (bank + materials + character inventory)
    // at the start of an event window so we have a "before" baseline.
    void LeyLine_BeginWindowSample(const std::string& characterName);
    // Re-samples item counts at the end of the window and compares against
    // the baseline + whether the player was seen in the right map.
    void LeyLine_EndWindowSample(const std::string& characterName);
    int  LeyLine_SumTrackedItems(const nlohmann::json& bank,
                                 const nlohmann::json& materials,
                                 const nlohmann::json& charInventory) const;
    void LeyLine_RefreshNextOccurrence();

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
        std::vector<WorldBossEntry>        worldBosses;
        std::vector<MapChest>              mapChests;
        std::mutex                         mtx;
    };
    PendingData m_pending;

    // Tracks how many async steps are still outstanding so we know when done.
    static constexpr int kTotalFetchSteps = 2; // WorldBosses, MapChests
    std::atomic<int>  m_pendingSteps { 0 };
    std::atomic<bool> m_anyStepFailed{ false };

    void SetStatus(FetchStatus s, const std::string& msg = "");
    void OnFetchError(const std::string& msg, FetchStatus s);
    void DecrementAndFinalize(const char* stepName);

    // -----------------------------------------------------------------------
    // Ley-Line Anomaly state (main-thread only; no locking needed since
    // TickLeyLineAnomaly/Tick are both only ever called from AddonRender).
    // -----------------------------------------------------------------------
    enum class LeyLineWindowPhase
    {
        Idle,           // outside any event window
        Sampling,       // inside the ~20-min window, baseline captured
    };
    LeyLineWindowPhase m_leyLinePhase = LeyLineWindowPhase::Idle;
    int                m_leyLineActiveOccurrenceSec = -1; // which occurrence we're tracking
    std::atomic<int>   m_leyLineBaselineCount       { -1 }; // -1 = not yet sampled
    bool               m_leyLineSeenCorrectMap      = false;
    std::atomic<bool>  m_leyLineSampleInFlight      { false };
};

} // namespace DailyTracker
