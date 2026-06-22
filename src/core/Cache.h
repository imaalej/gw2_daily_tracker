#pragma once
#include <string>
#include <chrono>
#include <filesystem>
#include <mutex>
#include "json.hpp"

namespace DailyTracker
{

class Cache
{
public:
    static constexpr int kDailyResetHourUtc = 0;

    explicit Cache(const std::filesystem::path& addonDir);

    void Load();
    void Save() const;

    void Set(const std::string& key, const nlohmann::json& value,
             std::chrono::system_clock::time_point expiresAt);
    void SetUntilDailyReset(const std::string& key, const nlohmann::json& value);
    void SetWithTTL(const std::string& key, const nlohmann::json& value,
                    std::chrono::seconds ttl);

    bool Get(const std::string& key, nlohmann::json& outValue,
             bool ignoreTTL = false) const;

    void Invalidate(const std::string& key);
    void InvalidateAll();

    static std::chrono::system_clock::time_point
        NextDailyReset(std::chrono::system_clock::time_point from
                       = std::chrono::system_clock::now());

private:
    std::filesystem::path   m_cachePath;
    nlohmann::json          m_data;
    mutable std::mutex      m_mtx;   // protects m_data and file I/O
};

} // namespace DailyTracker
