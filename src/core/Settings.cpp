#include "Settings.h"
#include <fstream>
#include <sstream>
#include <iomanip>
#include "json.hpp"

using json = nlohmann::json;

namespace DailyTracker
{

// XOR key for API key obfuscation (32 bytes cycling)
static const uint8_t k_xorKey[] = {
    0xA3, 0x5F, 0x2C, 0x71, 0x8E, 0x4B, 0xD9, 0x16,
    0x37, 0xC2, 0x8A, 0x5E, 0x1F, 0x73, 0xB4, 0x29,
    0x6D, 0x0C, 0xE7, 0x4A, 0x93, 0x2F, 0xB8, 0x51,
    0x7C, 0x3D, 0xA6, 0x59, 0xE2, 0x14, 0x68, 0x0B
};
static const size_t k_xorKeyLen = sizeof(k_xorKey);

// ---------------------------------------------------------------------------
Settings::Settings(const std::filesystem::path& addonDir)
    : m_settingsPath(addonDir / "settings.json")
{}

// ---------------------------------------------------------------------------
bool Settings::Load()
{
    if (!std::filesystem::exists(m_settingsPath))
        return true; // first run, use defaults

    std::ifstream file(m_settingsPath);
    if (!file.is_open())
        return false;

    try
    {
        json j;
        file >> j;

        if (j.contains("api_key_hex"))
        {
            std::string hex  = j["api_key_hex"].get<std::string>();
            std::string xord = FromHex(hex);
            m_settings.apiKey = XorDeobfuscate(xord);
        }
        if (j.contains("refresh_interval_sec"))
            m_settings.refreshIntervalSec = j["refresh_interval_sec"].get<int>();
        if (j.contains("show_wizards_vault"))
            m_settings.showWizardsVault  = j["show_wizards_vault"].get<bool>();
        if (j.contains("show_world_bosses"))
            m_settings.showWorldBosses   = j["show_world_bosses"].get<bool>();
        if (j.contains("show_home_nodes"))
            m_settings.showHomeNodes     = j["show_home_nodes"].get<bool>();
        if (j.contains("show_map_chests"))
            m_settings.showMapChests     = j["show_map_chests"].get<bool>();
        if (j.contains("window_visible"))
            m_settings.windowVisible     = j["window_visible"].get<bool>();
        if (j.contains("window_pos_x"))
            m_settings.windowPosX        = j["window_pos_x"].get<float>();
        if (j.contains("window_pos_y"))
            m_settings.windowPosY        = j["window_pos_y"].get<float>();
        if (j.contains("window_width"))
            m_settings.windowWidth       = j["window_width"].get<float>();
        if (j.contains("window_height"))
            m_settings.windowHeight      = j["window_height"].get<float>();
        if (j.contains("language"))
            m_settings.language          = j["language"].get<std::string>();
    }
    catch (const std::exception&)
    {
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
bool Settings::Save() const
{
    // Ensure directory exists
    std::filesystem::create_directories(m_settingsPath.parent_path());

    std::ofstream file(m_settingsPath);
    if (!file.is_open())
        return false;

    try
    {
        std::string obfuscated = ToHex(XorObfuscate(m_settings.apiKey));

        json j;
        j["api_key_hex"]         = obfuscated;
        j["refresh_interval_sec"] = m_settings.refreshIntervalSec;
        j["show_wizards_vault"]  = m_settings.showWizardsVault;
        j["show_world_bosses"]   = m_settings.showWorldBosses;
        j["show_home_nodes"]     = m_settings.showHomeNodes;
        j["show_map_chests"]     = m_settings.showMapChests;
        j["window_visible"]      = m_settings.windowVisible;
        j["window_pos_x"]        = m_settings.windowPosX;
        j["window_pos_y"]        = m_settings.windowPosY;
        j["window_width"]        = m_settings.windowWidth;
        j["window_height"]       = m_settings.windowHeight;
        j["language"]            = m_settings.language;

        file << std::setw(4) << j << "\n";
    }
    catch (const std::exception&)
    {
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
std::string Settings::XorObfuscate(const std::string& input)
{
    std::string out(input.size(), '\0');
    for (size_t i = 0; i < input.size(); ++i)
        out[i] = static_cast<char>(static_cast<uint8_t>(input[i])
                                   ^ k_xorKey[i % k_xorKeyLen]);
    return out;
}

std::string Settings::XorDeobfuscate(const std::string& input)
{
    return XorObfuscate(input); // XOR is symmetric
}

std::string Settings::ToHex(const std::string& bytes)
{
    std::ostringstream oss;
    for (unsigned char c : bytes)
        oss << std::hex << std::setw(2) << std::setfill('0')
            << static_cast<int>(c);
    return oss.str();
}

std::string Settings::FromHex(const std::string& hex)
{
    std::string result;
    for (size_t i = 0; i + 1 < hex.size(); i += 2)
    {
        uint8_t byte = static_cast<uint8_t>(
            std::stoul(hex.substr(i, 2), nullptr, 16));
        result.push_back(static_cast<char>(byte));
    }
    return result;
}

} // namespace DailyTracker
