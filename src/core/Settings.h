#pragma once
#include "DataTypes.h"
#include <string>
#include <filesystem>

namespace DailyTracker
{

class Settings
{
public:
    explicit Settings(const std::filesystem::path& addonDir);

    // Loads settings from disk.  Returns true on success.
    bool Load();

    // Saves current settings to disk.  Returns true on success.
    bool Save() const;

    // Accessor
    AddonSettings&       Get()       { return m_settings; }
    const AddonSettings& Get() const { return m_settings; }

    // Convenience wrappers ------------------------------------------------
    const std::string& ApiKey() const           { return m_settings.apiKey; }
    void SetApiKey(const std::string& key)      { m_settings.apiKey = key; }

    bool HasApiKey() const { return !m_settings.apiKey.empty(); }

private:
    std::filesystem::path m_settingsPath;
    AddonSettings         m_settings;

    // Very simple XOR "obfuscation" for the stored API key.
    // This is NOT strong encryption - it merely prevents the key from being
    // trivially read by shoulder-surfing the JSON file in a text editor.
    static std::string XorObfuscate(const std::string& input);
    static std::string XorDeobfuscate(const std::string& input);

    // Encodes/decodes the XOR result as printable hex
    static std::string ToHex(const std::string& bytes);
    static std::string FromHex(const std::string& hex);
};

} // namespace DailyTracker
