#include "Cache.h"
#include <fstream>
#include <iomanip>

namespace DailyTracker
{

Cache::Cache(const std::filesystem::path& addonDir)
    : m_cachePath(addonDir / "cache.json")
{}

void Cache::Load()
{
    if (!std::filesystem::exists(m_cachePath))
        return;

    std::ifstream file(m_cachePath);
    if (!file.is_open())
        return;

    try { file >> m_data; }
    catch (...) { m_data = nlohmann::json::object(); }
}

void Cache::Save() const
{
    std::filesystem::create_directories(m_cachePath.parent_path());
    std::ofstream file(m_cachePath);
    if (file.is_open())
        file << std::setw(2) << m_data << "\n";
}

void Cache::Set(const std::string& key, const nlohmann::json& value,
                std::chrono::system_clock::time_point expiresAt)
{
    auto expiresUnix = std::chrono::duration_cast<std::chrono::seconds>(
        expiresAt.time_since_epoch()).count();

    m_data[key] = {
        { "value",        value       },
        { "expires_unix", expiresUnix }
    };
}

void Cache::SetUntilDailyReset(const std::string& key, const nlohmann::json& value)
{
    Set(key, value, NextDailyReset());
}

void Cache::SetWithTTL(const std::string& key, const nlohmann::json& value,
                       std::chrono::seconds ttl)
{
    Set(key, value, std::chrono::system_clock::now() + ttl);
}

bool Cache::Get(const std::string& key, nlohmann::json& outValue, bool ignoreTTL) const
{
    auto it = m_data.find(key);
    if (it == m_data.end())
        return false;

    const auto& entry = *it;
    if (!entry.contains("expires_unix") || !entry.contains("value"))
        return false;

    if (!ignoreTTL)
    {
        auto expiresUnix = entry["expires_unix"].get<int64_t>();
        auto nowUnix = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();

        if (nowUnix >= expiresUnix)
            return false; // expired
    }

    outValue = entry["value"];
    return true;
}

void Cache::Invalidate(const std::string& key)
{
    m_data.erase(key);
}

void Cache::InvalidateAll()
{
    m_data = nlohmann::json::object();
}

std::chrono::system_clock::time_point
Cache::NextDailyReset(std::chrono::system_clock::time_point from)
{
    // GW2 daily reset is 00:00 UTC
    auto fromTime = std::chrono::system_clock::to_time_t(from);
    std::tm utcTm{};
#ifdef _WIN32
    gmtime_s(&utcTm, &fromTime);
#else
    gmtime_r(&fromTime, &utcTm);
#endif
    // Advance to next midnight UTC
    utcTm.tm_hour = 0;
    utcTm.tm_min  = 0;
    utcTm.tm_sec  = 0;
    utcTm.tm_mday += 1;

#ifdef _WIN32
    std::time_t resetTime = _mkgmtime(&utcTm);
#else
    std::time_t resetTime = timegm(&utcTm);
#endif
    return std::chrono::system_clock::from_time_t(resetTime);
}

} // namespace DailyTracker
