#pragma once
#include <string>
#include <functional>
#include <queue>
#include <mutex>
#include <thread>
#include <atomic>
#include <condition_variable>
#include <chrono>
#include "json.hpp"

namespace DailyTracker
{

// Result of an API call
struct ApiResult
{
    bool        success     = false;
    int         httpStatus  = 0;
    std::string errorMessage;
    nlohmann::json body;
};

using ApiCallback = std::function<void(ApiResult)>;

// Enqueues an HTTP GET and calls back on a worker thread when done.
// IMPORTANT: All ImGui / UI mutations must be marshalled to the main
// thread. The callbacks here run on the worker thread.
class APIManager
{
public:
    static constexpr const char* kBaseUrl = "https://api.guildwars2.com";

    explicit APIManager();
    ~APIManager();

    // Must be called once before any requests.
    bool Init();

    // Drains the queue and joins worker thread.
    void Shutdown();

    // Update the stored API key (used for authenticated requests).
    void SetApiKey(const std::string& key);
    const std::string& GetApiKey() const { return m_apiKey; }
    bool HasApiKey() const { return !m_apiKey.empty(); }

    // -----------------------------------------------------------------------
    // Authenticated endpoints
    // -----------------------------------------------------------------------
    void FetchWorldBossCompletion(ApiCallback cb);
    void FetchMapChests(ApiCallback cb);
    void FetchAccountBank(ApiCallback cb);
    void FetchAccountMaterials(ApiCallback cb);
    void FetchCharacterInventory(const std::string& characterName, ApiCallback cb);

    // -----------------------------------------------------------------------
    // Unauthenticated / metadata endpoints
    // -----------------------------------------------------------------------
    void FetchWorldBossMeta(const std::string& lang, ApiCallback cb);
    void FetchMapChestMeta(const std::string& lang, ApiCallback cb);

    // Generic GET (public)
    void Get(const std::string& endpoint,
             bool authenticated,
             const std::string& lang,
             ApiCallback cb);

private:
    struct Request
    {
        std::string url;
        std::string apiKey;     // empty for unauthenticated
        ApiCallback callback;
    };

    void WorkerLoop();
    ApiResult PerformGet(const std::string& url, const std::string& apiKey);

    // Respects X-Rate-Limit-Limit / X-Rate-Limit-Remaining response headers.
    // Sleeps the worker thread if we're close to the limit.
    void MaybeThrottle(int remaining, int limit);

    std::string             m_apiKey;
    std::string             m_language = "en";

    std::thread             m_worker;
    std::atomic<bool>       m_running{ false };
    std::queue<Request>     m_queue;
    std::mutex              m_queueMtx;
    std::condition_variable m_queueCv;

    // Rate-limit state (updated by worker from response headers)
    int  m_rlRemaining  = 300;
    int  m_rlLimit      = 300;
};

} // namespace DailyTracker
