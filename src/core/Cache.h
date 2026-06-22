#pragma once
#include <string>
#include <chrono>
#include <filesystem>
#include <mutex>
#include "json.hpp"

namespace DailyTracker
{

// Manages a single JSON cache file with per-key TTL support.
// Each stored value is written alongside a timestamp; Get() returns
// the value only if it has not expired.
class Cache
{
public:
    // The "daily" TTL aligns to the server daily reset (18:00 UTC) rather
    // than a fixed wall-clock duration. Use kDailyTTL for data that resets
    // with the server day (boss kills, map chests, etc.).
    static constexpr int kDailyResetHourUtc = 0;  // GW2 daily reset 00:00 UTC

    explicit Cache(const std::filesystem::path& addonDir);

    // Loads the cache file from disk. Safe to call even if file doesn't exist.
    void Load();

    // Flushes in-memory cache to disk.
    void Save() const;

    // Store a value with an explicit expiry time point.
    void Set(const std::string& key, const nlohmann::json& value,
             std::chrono::system_clock::time_point expiresAt);

    // Store a value that expires at the next daily reset (00:00 UTC).
    void SetUntilDailyReset(const std::string& key, const nlohmann::json& value);

    // Store a value that expires after a fixed duration.
    void SetWithTTL(const std::string& key, const nlohmann::json& value,
                    std::chrono::seconds ttl);

    // Retrieve a cached value.  Returns true and populates `outValue` if the
    // entry exists and has not expired (unless ignoreTTL is true, in which
    // case expiry is not checked — used as a stale-data fallback when a
    // live API call fails and we'd rather show old data than none at all).
    bool Get(const std::string& key, nlohmann::json& outValue,
             bool ignoreTTL = false) const;

    // Explicitly invalidate a key.
    void Invalidate(const std::string& key);

    // Wipe all cached entries (e.g., on daily reset detection).
    void InvalidateAll();

    // Returns the time_point of the next daily reset following `from`.
    static std::chrono::system_clock::time_point
        NextDailyReset(std::chrono::system_clock::time_point from
                       = std::chrono::system_clock::now());

private:
    std::filesystem::path   m_cachePath;
    nlohmann::json          m_data;  // { key: { "value": ..., "expires_unix": ... } }
    mutable std::mutex      m_mtx;   // protects m_data and file I/O
};

} // namespace DailyTracker
