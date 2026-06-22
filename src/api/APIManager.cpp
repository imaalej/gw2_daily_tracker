#include "APIManager.h"
#include <curl/curl.h>
#include <sstream>
#include <thread>
#include <chrono>

namespace DailyTracker
{
    // Clean declaration matching across compilation units
    void LogMessage(int level, const std::string& channel, const std::string& message);
}

namespace DailyTracker
{

// ---------------------------------------------------------------------------
// libcurl write callback: appends received data to a std::string*
// ---------------------------------------------------------------------------
static size_t WriteCallback(char* ptr, size_t size, size_t nmemb, void* userdata)
{
    auto* buf = static_cast<std::string*>(userdata);
    buf->append(ptr, size * nmemb);
    return size * nmemb;
}

// ---------------------------------------------------------------------------
// libcurl header callback: parses X-Rate-Limit-* response headers
// ---------------------------------------------------------------------------
struct RateLimitHeaders
{
    int remaining = -1;
    int limit     = -1;
};

static size_t HeaderCallback(char* buffer, size_t size, size_t nitems, void* userdata)
{
    auto* rl = static_cast<RateLimitHeaders*>(userdata);
    std::string line(buffer, size * nitems);

    auto parseHeader = [&](const char* name, int& out)
    {
        if (line.substr(0, strlen(name)) == name)
        {
            try { out = std::stoi(line.substr(strlen(name))); }
            catch (...) {}
        }
    };
    parseHeader("X-Rate-Limit-Limit: ",     rl->limit);
    parseHeader("X-Rate-Limit-Remaining: ", rl->remaining);

    return size * nitems;
}

// ---------------------------------------------------------------------------
APIManager::APIManager() = default;

APIManager::~APIManager()
{
    Shutdown();
}

bool APIManager::Init()
{
    CURLcode rc = curl_global_init(CURL_GLOBAL_DEFAULT);
    if (rc != CURLE_OK)
        return false;

    m_running = true;
    m_worker  = std::thread(&APIManager::WorkerLoop, this);
    return true;
}

void APIManager::Shutdown()
{
    if (!m_running.exchange(false))
        return;

    m_queueCv.notify_one();
    if (m_worker.joinable())
        m_worker.join();

    curl_global_cleanup();
}

void APIManager::SetApiKey(const std::string& key)
{
    m_apiKey = key;
}

// ---------------------------------------------------------------------------
// Endpoint helpers
// ---------------------------------------------------------------------------
void APIManager::FetchWorldBossCompletion(ApiCallback cb)
{
    Get("/v2/account/worldbosses", true, m_language, std::move(cb));
}

void APIManager::FetchMapChests(ApiCallback cb)
{
    Get("/v2/account/mapchests", true, m_language, std::move(cb));
}

void APIManager::FetchAccountBank(ApiCallback cb)
{
    Get("/v2/account/bank", true, m_language, std::move(cb));
}

void APIManager::FetchAccountMaterials(ApiCallback cb)
{
    Get("/v2/account/materials", true, m_language, std::move(cb));
}

void APIManager::FetchCharacterInventory(const std::string& characterName, ApiCallback cb)
{
    // Character names can contain spaces and other characters that need
    // percent-encoding in the URL path.
    CURL* escapeHandle = curl_easy_init();
    std::string encodedName = characterName;
    if (escapeHandle)
    {
        char* escaped = curl_easy_escape(escapeHandle, characterName.c_str(),
                                          static_cast<int>(characterName.size()));
        if (escaped)
        {
            encodedName = escaped;
            curl_free(escaped);
        }
        curl_easy_cleanup(escapeHandle);
    }

    std::string ep = "/v2/characters/" + encodedName + "/inventory";
    Get(ep, true, m_language, std::move(cb));
}

void APIManager::FetchWorldBossMeta(const std::string& lang, ApiCallback cb)
{
    std::string ep = "/v2/worldbosses?ids=all&lang=" + lang;
    Get(ep, false, lang, std::move(cb));
}

void APIManager::FetchMapChestMeta(const std::string& lang, ApiCallback cb)
{
    std::string ep = "/v2/mapchests?ids=all&lang=" + lang;
    Get(ep, false, lang, std::move(cb));
}

void APIManager::Get(const std::string& endpoint,
                     bool authenticated,
                     const std::string& /*lang*/,
                     ApiCallback cb)
{
    Request req;
    req.url      = std::string(kBaseUrl) + endpoint;
    req.apiKey   = authenticated ? m_apiKey : "";
    req.callback = std::move(cb);

    {
        std::lock_guard<std::mutex> lk(m_queueMtx);
        m_queue.push(std::move(req));
    }
    m_queueCv.notify_one();
}

// ---------------------------------------------------------------------------
// Worker loop – runs on dedicated thread
// ---------------------------------------------------------------------------
void APIManager::WorkerLoop()
{
    while (true)
    {
        Request req;
        {
            std::unique_lock<std::mutex> lk(m_queueMtx);
            m_queueCv.wait(lk, [this]
            {
                return !m_running || !m_queue.empty();
            });

            if (!m_running && m_queue.empty())
                break;
            if (m_queue.empty())
                continue;

            req = std::move(m_queue.front());
            m_queue.pop();
        }

        // Throttle before sending if rate limit is exhausted
        MaybeThrottle(m_rlRemaining, m_rlLimit);

        ApiResult result = PerformGet(req.url, req.apiKey);

        // The callback (owned by DataStore) does JSON parsing via
        // ResponseParser. A malformed cache file or unexpected API shape
        // could throw there; without this guard, an uncaught exception
        // would terminate the worker thread and leave m_fetchInFlight
        // stuck `true` forever (see DataStore::Tick() heartbeat for the
        // complementary main-thread-side safety net).
        if (req.callback)
        {
            try
            {
                req.callback(std::move(result));
            }
            catch (const std::exception& e)
            {
                LogMessage(5, "DailyTracker",
                    std::string("CRITICAL: Unhandled exception in API callback: ") + e.what());
            }
            catch (...)
            {
                LogMessage(5, "DailyTracker",
                    "CRITICAL: Unknown unhandled exception in API callback.");
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Actual curl call
// ---------------------------------------------------------------------------
ApiResult APIManager::PerformGet(const std::string& url, const std::string& apiKey)
{
    ApiResult result;
    
    // Log tracking target (2 = LOGL_INFO)
    LogMessage(2, "DailyTracker", "Starting API Request to: " + url);

    CURL* curl = curl_easy_init();
    if (!curl)
    {
        result.errorMessage = "curl_easy_init() failed";
        LogMessage(5, "DailyTracker", "CRITICAL: curl_easy_init failed!"); // 5 = LOGL_CRITICAL
        return result;
    }

    std::string responseBody;
    RateLimitHeaders rlHeaders;
    char curlErrorBuf[CURL_ERROR_SIZE] = {0};

    curl_easy_setopt(curl, CURLOPT_URL,           url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION,  WriteCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA,      &responseBody);
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, HeaderCallback);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA,     &rlHeaders);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    
    // Multi-threading safety flag
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL,       1L);
    
    // Enforce strict timeouts so it cannot stay stuck forever
    curl_easy_setopt(curl, CURLOPT_TIMEOUT,        10L); // Max 10 seconds total
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 5L);  // Max 5 seconds connecting
    
    // Bind error buffer to catch detailed underlying SSL errors
    curl_easy_setopt(curl, CURLOPT_ERRORBUFFER,    curlErrorBuf);
    
    // SSL Verification settings
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_OPTIONS, (long)CURLSSLOPT_NATIVE_CA);

    curl_easy_setopt(curl, CURLOPT_USERAGENT,
        "GW2-DailyTracker/1.0 (Nexus addon; https://github.com/gumibo/GW2-DailyTracker)");

    curl_slist* headers = nullptr;
    if (!apiKey.empty())
    {
        std::string authHeader = "Authorization: Bearer " + apiKey;
        headers = curl_slist_append(headers, authHeader.c_str());
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    }

    // Execute network block
    CURLcode rc = curl_easy_perform(curl);

    long httpCode = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &httpCode);

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (rlHeaders.remaining >= 0) m_rlRemaining = rlHeaders.remaining;
    if (rlHeaders.limit     >= 0) m_rlLimit     = rlHeaders.limit;

    result.httpStatus = static_cast<int>(httpCode);

    // LOG UNEXPECTED CURL ERRORS (3 = LOGL_WARNING)
    if (rc != CURLE_OK)
    {
        std::string detailedError = std::string(curl_easy_strerror(rc)) + " | Details: " + curlErrorBuf;
        result.errorMessage = detailedError;
        LogMessage(3, "DailyTracker", "Network Error (" + std::to_string(rc) + "): " + detailedError);
        return result;
    }

    LogMessage(2, "DailyTracker", "HTTP Response received: " + std::to_string(httpCode));

    if (httpCode == 401)
    {
        result.errorMessage = "Invalid or missing API key (HTTP 401)";
        LogMessage(3, "DailyTracker", result.errorMessage);
        return result;
    }
    if (httpCode == 403)
    {
        result.errorMessage = "API key lacks required permissions (HTTP 403)";
        LogMessage(3, "DailyTracker", result.errorMessage);
        return result;
    }
    if (httpCode >= 500)
    {
        result.errorMessage = "GW2 API server error (HTTP " + std::to_string(httpCode) + ")";
        LogMessage(3, "DailyTracker", result.errorMessage);
        return result;
    }
    if (httpCode != 200)
    {
        result.errorMessage = "Unexpected HTTP " + std::to_string(httpCode);
        LogMessage(3, "DailyTracker", result.errorMessage);
        return result;
    }

    try
    {
        result.body    = nlohmann::json::parse(responseBody);
        result.success = true;
        LogMessage(2, "DailyTracker", "Successfully parsed API payload json.");
    }
    catch (const nlohmann::json::exception& e)
    {
        result.errorMessage = std::string("JSON parse error: ") + e.what();
        LogMessage(3, "DailyTracker", result.errorMessage);
    }
    return result;
}

// ---------------------------------------------------------------------------
// Rate-limit throttle – sleep if we're nearly exhausted
// ---------------------------------------------------------------------------
void APIManager::MaybeThrottle(int remaining, int limit)
{
    if (limit <= 0 || remaining < 0)
        return;

    // If below 10% headroom, sleep proportionally
    float ratio = static_cast<float>(remaining) / static_cast<float>(limit);
    if (ratio < 0.10f)
    {
        // Sleep up to 2 seconds; more aggressive the lower we are
        int sleepMs = static_cast<int>((1.0f - ratio * 10.f) * 2000.f);
        std::this_thread::sleep_for(std::chrono::milliseconds(sleepMs));
    }
}

} // namespace DailyTracker
