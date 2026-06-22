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
#include <thread>
#include <condition_variable>
#include <queue>

namespace DailyTracker
{

enum class FetchStatus
{
    Idle,
    Fetching,
    Ok,
    NoApiKey,
    AuthError,
    NetworkError,
    ApiKeyMissing,
};

class DataStore
{
public:
    DataStore(Cache& cache, Settings& settings, APIManager& api);
    ~DataStore();

    void Initialize();
    void Tick();
    void ForceRefresh();
    void OnApiKeyChanged();
    void SetOnDataUpdated(std::function<void()> cb);

    DailySnapshot GetSnapshot() const;
    FetchStatus GetStatus() const;
    std::string GetStatusMessage() const;

    void TickLeyLineAnomaly(int currentMapId, const std::string& currentCharacterName);

private:
    // Background thread
    void BackgroundLoop();
    void PostTask(std::function<void()> task);

    // Fetch steps (run on background thread)
    void BeginFetch();
    void FetchStep_WorldBossCompletion();
    void FetchStep_WorldBossMeta(std::set<std::string> completedIds);
    void FetchStep_MapChestsMeta();
    void FetchStep_MapChestsCompletion(nlohmann::json metaJson);
    void FinalizeFetch();

    // Ley-Line anomaly helpers (run on background thread)
    void LeyLine_RefreshNextOccurrence();
    int  LeyLine_SumTrackedItems(const nlohmann::json& bank,
                                 const nlohmann::json& materials,
                                 const nlohmann::json& charInventory) const;
    void LeyLine_BeginWindowSample(const std::string& characterName);
    void LeyLine_EndWindowSample(const std::string& characterName);

    // Internal helpers
    void SetStatus(FetchStatus s, const std::string& msg = "");
    void OnFetchError(const std::string& msg, FetchStatus s);
    void DecrementAndFinalize(const char* stepName);
    void CheckDailyReset();  // called from background thread

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
    std::atomic<bool>  m_pendingReset{ false };

    std::function<void()> m_onDataUpdated;

    struct PendingData
    {
        std::vector<WorldBossEntry>        worldBosses;
        std::vector<MapChest>              mapChests;
        std::mutex                         mtx;
    };
    PendingData m_pending;

    static constexpr int kTotalFetchSteps = 2;
    std::atomic<int>  m_pendingSteps { 0 };
    std::atomic<bool> m_anyStepFailed{ false };

    // Background thread and task queue
    std::thread                m_backgroundThread;
    std::atomic<bool>          m_stopBackground{ false };
    std::queue<std::function<void()>> m_taskQueue;
    std::mutex                m_queueMtx;
    std::condition_variable   m_queueCv;

    // Ley-Line state (only accessed on background thread)
    enum class LeyLineWindowPhase { Idle, Sampling };
    LeyLineWindowPhase m_leyLinePhase = LeyLineWindowPhase::Idle;
    int                m_leyLineActiveOccurrenceSec = -1;
    std::atomic<int>   m_leyLineBaselineCount { -1 };
    bool               m_leyLineSeenCorrectMap = false;
    std::atomic<bool>  m_leyLineSampleInFlight{ false };

    // Throttle for TickLeyLineAnomaly: accessed on the main (render) thread only.
    // Prevents flooding the task queue at 60fps when the player is in-game.
    std::chrono::steady_clock::time_point m_lastLeyLineTick{};
};

} // namespace DailyTracker
