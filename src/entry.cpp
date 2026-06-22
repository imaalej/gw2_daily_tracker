#include "nexus/Nexus.h"
#include "mumble/Mumble.h"
#include "imgui/imgui.h"
#include "core/DataTypes.h"
#include "core/Cache.h"
#include "core/Settings.h"
#include "api/APIManager.h"
#include "DataStore.h"

#include <memory>
#include <string>
#include <cstring>
#include <chrono>
#include <ctime>
#include <filesystem>
#include <atomic>
#include <mutex>

namespace DailyTracker
{

// ---------------------------------------------------------------------------
// Globals
// ---------------------------------------------------------------------------
static AddonAPI_t*                g_nexus  = nullptr;
// Change ELogLevel to int here so it matches APIManager.cpp precisely
void LogMessage(int level, const std::string& channel, const std::string& message)
{
    if (g_nexus)
    {
        // static_cast turns our plain number back into Nexus's official ELogLevel format
        g_nexus->Log(static_cast<ELogLevel>(level), const_cast<char*>(channel.c_str()), const_cast<char*>(message.c_str()));
    }
}
static std::unique_ptr<Cache>     g_cache;
static std::unique_ptr<Settings>  g_settings;
static std::unique_ptr<APIManager> g_api;
static std::unique_ptr<DataStore> g_dataStore;

static char g_apiKeyBuffer[256] = {};
static std::string g_apiKeyTestMsg;
static std::atomic<bool> g_apiKeyTestInFlight{ false };
static std::mutex g_testMsgMutex;

static constexpr const char* kAddonName       = "Daily Tracker";
static constexpr const char* kKeybindToggle   = "DAILYTRACKER_TOGGLE";

// ---------------------------------------------------------------------------
// Helpers (unchanged)
// ---------------------------------------------------------------------------
static std::string FormatTimeOfDay(std::chrono::system_clock::time_point tp)
{
    if (tp.time_since_epoch().count() == 0)
        return "never";

    std::time_t t = std::chrono::system_clock::to_time_t(tp);
    std::tm localTm{};
#ifdef _WIN32
    localtime_s(&localTm, &t);
#else
    localtime_r(&t, &localTm);
#endif
    char buf[32];
    std::strftime(buf, sizeof(buf), "%H:%M:%S", &localTm);
    return std::string(buf);
}

static std::string FormatCountdown(int seconds)
{
    if (seconds < 0)
        return "unknown";

    int h = seconds / 3600;
    int m = (seconds % 3600) / 60;
    int s = seconds % 60;

    char buf[32];
    if (h > 0)
        std::snprintf(buf, sizeof(buf), "%dh %02dm", h, m);
    else
        std::snprintf(buf, sizeof(buf), "%dm %02ds", m, s);
    return std::string(buf);
}

static const char* StatusToString(FetchStatus s)
{
    switch (s)
    {
        case FetchStatus::Idle:          return "Idle";
        case FetchStatus::Fetching:      return "Refreshing...";
        case FetchStatus::Ok:            return "OK";
        case FetchStatus::NoApiKey:      return "No API key set";
        case FetchStatus::AuthError:     return "Authentication error";
        case FetchStatus::NetworkError:  return "Network error";
        case FetchStatus::ApiKeyMissing: return "API key missing";
    }
    return "Unknown";
}

static void DrawCompletionIcon(CompletionState state)
{
    switch (state)
    {
        case CompletionState::Complete:
            ImGui::TextColored(ImVec4(0.2f, 0.9f, 0.2f, 1.0f), "[x]");
            break;
        case CompletionState::Incomplete:
            ImGui::TextColored(ImVec4(0.9f, 0.3f, 0.3f, 1.0f), "[ ]");
            break;
        case CompletionState::Unknown:
        default:
            ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.2f, 1.0f), "[?]");
            break;
    }
}

// ---------------------------------------------------------------------------
// Mumble link access (current map ID + active character name), needed for
// Ley-Line Anomaly detection. DL_MUMBLE_LINK gives us Mumble::Data, whose
// Context.MapID is the player's current map, and whose Identity (delivered
// separately via DL_MUMBLE_LINK_IDENTITY, or embedded depending on Nexus
// version) gives the active character's name. Nexus exposes the parsed
// identity directly via DataLink_Get(DL_MUMBLE_LINK_IDENTITY).
// ---------------------------------------------------------------------------
static bool GetCurrentMapAndCharacter(int& outMapId, std::string& outCharacterName)
{
    outMapId = -1;
    outCharacterName.clear();

    if (!g_nexus || !g_nexus->DataLink_Get)
        return false;

    void* mumbleData = g_nexus->DataLink_Get(DL_MUMBLE_LINK);
    if (mumbleData)
    {
        auto* data = static_cast<Mumble::Data*>(mumbleData);
        outMapId = static_cast<int>(data->Context.MapID);
    }

    void* identityData = g_nexus->DataLink_Get(DL_MUMBLE_LINK_IDENTITY);
    if (identityData)
    {
        auto* identity = static_cast<Mumble::Identity*>(identityData);
        // Name is a fixed-size char buffer; guard against missing
        // null-termination from the shared-memory link.
        char nameBuf[sizeof(identity->Name) + 1] = {};
        std::memcpy(nameBuf, identity->Name, sizeof(identity->Name));
        outCharacterName = nameBuf;
    }

    return outMapId >= 0 && !outCharacterName.empty();
}

// ---------------------------------------------------------------------------
// Forward declarations for render callbacks
// ---------------------------------------------------------------------------
static void AddonRender();
static void AddonOptionsRender();
static void OnKeybindToggle(const char* aIdentifier, bool aIsRelease);

// ---------------------------------------------------------------------------
// Main window rendering (unchanged logic, only format tweaks)
// ---------------------------------------------------------------------------
static void RenderStatusBar()
{
    FetchStatus status = g_dataStore->GetStatus();
    std::string msg    = g_dataStore->GetStatusMessage();

    ImGui::Separator();

    switch (status)
    {
        case FetchStatus::AuthError:
            ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f),
                "Authentication error: %s", msg.c_str());
            ImGui::TextWrapped("Your API key may be invalid or missing required permissions.");
            break;

        case FetchStatus::NetworkError:
            ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.2f, 1.0f),
                "Network error: %s", msg.c_str());
            break;

        case FetchStatus::NoApiKey:
        case FetchStatus::ApiKeyMissing:
            ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.2f, 1.0f),
                "%s", msg.empty() ? StatusToString(status) : msg.c_str());
            ImGui::TextWrapped("Set your GW2 API key in the addon options to enable data fetching.");
            break;

        case FetchStatus::Fetching:
            ImGui::TextColored(ImVec4(0.6f, 0.8f, 1.0f, 1.0f), "Refreshing...");
            break;

        case FetchStatus::Ok:
        default:
            if (!msg.empty())
                ImGui::TextUnformatted(msg.c_str());
            break;
    }

    DailySnapshot snap = g_dataStore->GetSnapshot();
    ImGui::Text("Last refreshed: %s", FormatTimeOfDay(snap.lastRefreshed).c_str());

    if (ImGui::Button("Refresh Now"))
        g_dataStore->ForceRefresh();
}

static void RenderWorldBosses(const DailySnapshot& snap)
{
    if (!ImGui::CollapsingHeader("World Bosses", ImGuiTreeNodeFlags_DefaultOpen))
        return;

    if (snap.worldBosses.empty())
    {
        ImGui::TextDisabled("No data available.");
        return;
    }

    std::time_t nowUtc = std::time(nullptr);

    if (ImGui::BeginTable("##worldbosses", 3,
            ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp))
    {
        ImGui::TableSetupColumn("##icon", ImGuiTableColumnFlags_WidthFixed, 28.0f);
        ImGui::TableSetupColumn("Boss");
        ImGui::TableSetupColumn("Next Spawn", ImGuiTableColumnFlags_WidthFixed, 90.0f);

        for (const auto& boss : snap.worldBosses)
        {
            ImGui::TableNextRow();

            ImGui::TableSetColumnIndex(0);
            DrawCompletionIcon(boss.completion);

            ImGui::TableSetColumnIndex(1);
            ImGui::TextUnformatted(boss.name.c_str());

            ImGui::TableSetColumnIndex(2);
            int secs = boss.SecondsUntilNextSpawn(nowUtc);
            ImGui::TextUnformatted(FormatCountdown(secs).c_str());
        }

        ImGui::EndTable();
    }
}

static void RenderLeyLineAnomaly(const DailySnapshot& snap)
{
    if (!ImGui::CollapsingHeader("Ley-Line Anomaly", ImGuiTreeNodeFlags_DefaultOpen))
        return;

    const auto& ll = snap.leyLineAnomaly;

    if (ll.nextSpawnUtcSec < 0)
    {
        ImGui::TextDisabled("No data available.");
        return;
    }

    DrawCompletionIcon(ll.completion);
    ImGui::SameLine();
    ImGui::Text("Next: %s", ll.nextMapName.c_str());

    std::time_t nowUtc = std::time(nullptr);
    int secondsToday = static_cast<int>(nowUtc % 86400);
    int diff = ll.nextSpawnUtcSec - secondsToday;
    if (diff < 0)
        diff += 86400;

    ImGui::Text("Spawns in: %s", FormatCountdown(diff).c_str());
    ImGui::TextDisabled("Detected heuristically via map + item-count tracking.");
}

static void RenderMapChests(const DailySnapshot& snap)
{
    if (!ImGui::CollapsingHeader("Hero's Choice Chests", ImGuiTreeNodeFlags_DefaultOpen))
        return;

    if (snap.mapChests.empty())
    {
        ImGui::TextDisabled("No data available.");
        return;
    }

    for (const auto& chest : snap.mapChests)
    {
        DrawCompletionIcon(chest.completion);
        ImGui::SameLine();
        ImGui::TextUnformatted(chest.name.c_str());
    }
}

static void AddonRender()
{
    // Drive refresh / daily-reset logic and data-updated callback on main thread.
    g_dataStore->Tick();

    // Ley-Line Anomaly detection needs the player's current map + active
    // character every frame, regardless of whether the window is visible.
    int currentMapId = -1;
    std::string currentCharacterName;
    GetCurrentMapAndCharacter(currentMapId, currentCharacterName);
    g_dataStore->TickLeyLineAnomaly(currentMapId, currentCharacterName);

    AddonSettings& settings = g_settings->Get();
    if (!settings.windowVisible)
        return;

    ImGui::SetNextWindowPos(ImVec2(settings.windowPosX, settings.windowPosY), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(settings.windowWidth, settings.windowHeight), ImGuiCond_FirstUseEver);

    bool open = settings.windowVisible;
    if (ImGui::Begin(kAddonName, &open))
    {
        DailySnapshot snap = g_dataStore->GetSnapshot();

        if (settings.showLeyLineAnomaly) RenderLeyLineAnomaly(snap);
        if (settings.showWorldBosses)    RenderWorldBosses(snap);
        if (settings.showMapChests)      RenderMapChests(snap);

        RenderStatusBar();

        // Persist window position/size for next session
        ImVec2 pos  = ImGui::GetWindowPos();
        ImVec2 size = ImGui::GetWindowSize();
        settings.windowPosX  = pos.x;
        settings.windowPosY  = pos.y;
        settings.windowWidth = size.x;
        settings.windowHeight = size.y;
    }
    ImGui::End();

    if (open != settings.windowVisible)
    {
        settings.windowVisible = open;
        g_settings->Save();
    }
}

// ---------------------------------------------------------------------------
// Options window rendering
// ---------------------------------------------------------------------------
static void AddonOptionsRender()
{
    AddonSettings& settings = g_settings->Get();

    ImGui::TextUnformatted("Daily Tracker Settings");
    ImGui::Separator();

    // ---- API key ----
    ImGui::TextUnformatted("GW2 API Key");
    ImGui::TextDisabled("Requires scopes: account, progression, unlocks, inventories");

    if (ImGui::InputText("##apikey", g_apiKeyBuffer, sizeof(g_apiKeyBuffer),
                          ImGuiInputTextFlags_Password))
    {
        // buffer updated live; saved on button press below
    }

    ImGui::SameLine();
    if (ImGui::Button("Save Key"))
    {
        std::string newKey(g_apiKeyBuffer);
        settings.apiKey = newKey;
        g_settings->Save();
        g_dataStore->OnApiKeyChanged();
        {
            std::lock_guard<std::mutex> lock(g_testMsgMutex);
            g_apiKeyTestMsg.clear();
        }
    }

    if (ImGui::Button("Test Connection") && !g_apiKeyTestInFlight)
    {
        g_apiKeyTestInFlight = true;
        {
            std::lock_guard<std::mutex> lock(g_testMsgMutex);
            g_apiKeyTestMsg = "Testing...";
        }

        // Use a lightweight authenticated endpoint to validate the key.
        g_api->Get("/v2/account", true, settings.language,
            [](ApiResult res)
            {
                std::lock_guard<std::mutex> lock(g_testMsgMutex);
                if (res.success)
                    g_apiKeyTestMsg = "Key is valid.";
                else if (res.httpStatus == 401 || res.httpStatus == 403)
                    g_apiKeyTestMsg = "Invalid key or insufficient permissions.";
                else
                    g_apiKeyTestMsg = "Error: " + res.errorMessage;

                g_apiKeyTestInFlight = false;
            });
    }

    {
        std::lock_guard<std::mutex> lock(g_testMsgMutex);
        if (!g_apiKeyTestMsg.empty())
        {
            ImGui::SameLine();
            ImGui::TextUnformatted(g_apiKeyTestMsg.c_str());
        }
    }

    ImGui::Spacing();
    ImGui::Separator();

    // ---- Refresh interval ----
    int refreshSec = settings.refreshIntervalSec;
    if (ImGui::SliderInt("Refresh interval (seconds)", &refreshSec, 30, 600))
    {
        settings.refreshIntervalSec = refreshSec;
        g_settings->Save();
    }

    ImGui::Spacing();
    ImGui::Separator();

    // ---- Category toggles ----
    ImGui::TextUnformatted("Visible Categories");

    bool changed = false;
    changed |= ImGui::Checkbox("Ley-Line Anomaly",    &settings.showLeyLineAnomaly);
    changed |= ImGui::Checkbox("World Bosses",         &settings.showWorldBosses);
    changed |= ImGui::Checkbox("Hero's Choice Chests", &settings.showMapChests);

    if (changed)
        g_settings->Save();

    ImGui::Spacing();
    ImGui::Separator();

    // ---- Window visibility ----
    if (ImGui::Checkbox("Show main window", &settings.windowVisible))
        g_settings->Save();
}

// ---------------------------------------------------------------------------
// Keybind callback
// ---------------------------------------------------------------------------
static void OnKeybindToggle(const char* /*aIdentifier*/, bool aIsRelease)
{
    if (aIsRelease)
        return;

    AddonSettings& settings = g_settings->Get();
    settings.windowVisible = !settings.windowVisible;
    g_settings->Save();
}

// ---------------------------------------------------------------------------
// Addon load / unload
// ---------------------------------------------------------------------------
static void AddonLoad(AddonAPI_t* aApi)
{
    g_nexus = aApi;

    // ImGui context/allocator
    ImGui::SetCurrentContext((ImGuiContext*)aApi->ImguiContext);
    if (aApi->ImguiMalloc && aApi->ImguiFree)
    {
        ImGui::SetAllocatorFunctions(
            reinterpret_cast<void* (*)(size_t, void*)>(aApi->ImguiMalloc),
            reinterpret_cast<void  (*)(void*, void*)>(aApi->ImguiFree));
    }

    std::filesystem::path addonDir = aApi->Paths_GetAddonDirectory("DailyTracker");

    g_cache    = std::make_unique<Cache>(addonDir);
    g_settings = std::make_unique<Settings>(addonDir);
    g_api      = std::make_unique<APIManager>();
    g_dataStore = std::make_unique<DataStore>(*g_cache, *g_settings, *g_api);

    g_cache->Load();
    g_settings->Load();

    if (!g_api->Init())
    {
        aApi->Log(LOGL_CRITICAL, "DailyTracker", "Failed to initialize APIManager (curl_global_init failed)");
    }

    // Seed API key + UI buffer from loaded settings
    g_api->SetApiKey(g_settings->ApiKey());
    std::strncpy(g_apiKeyBuffer, g_settings->ApiKey().c_str(), sizeof(g_apiKeyBuffer) - 1);
    g_apiKeyBuffer[sizeof(g_apiKeyBuffer) - 1] = '\0';

    // Register render callbacks
    aApi->GUI_Register(RT_Render, AddonRender);
    aApi->GUI_Register(RT_OptionsRender, AddonOptionsRender);

    // Register keybind (default unbound, user can rebind via Nexus UI)
    aApi->InputBinds_RegisterWithString(kKeybindToggle, OnKeybindToggle, "");

    // Kick off initial data load (from cache and/or network)
    g_dataStore->Initialize();

    aApi->Log(LOGL_INFO, "DailyTracker", "Addon loaded");
}

static void AddonUnload()
{
    if (g_nexus)
    {
        g_nexus->GUI_Deregister(AddonRender);
        g_nexus->GUI_Deregister(AddonOptionsRender);
        g_nexus->InputBinds_Deregister(kKeybindToggle);
    }

    if (g_settings)
        g_settings->Save();
    if (g_cache)
        g_cache->Save();

    if (g_api)
        g_api->Shutdown();

    g_dataStore.reset();
    g_api.reset();
    g_settings.reset();
    g_cache.reset();

    if (g_nexus)
        g_nexus->Log(LOGL_INFO, "DailyTracker", "Addon unloaded");

    g_nexus = nullptr;
}

// ---------------------------------------------------------------------------
// Addon definition
// ---------------------------------------------------------------------------
static AddonDefinition_t g_addonDef = {
    /* Signature   */ 0x44544B52,
    /* APIVersion  */ NEXUS_API_VERSION,
    /* Name        */ kAddonName,
    /* Version     */ { 1, 0, 0, 0 },
    /* Author      */ "gumibo.1643",
    /* Description */ "Tracks your dailies that aren't tracked by your achievements.",
    /* Load        */ AddonLoad,
    /* Unload      */ AddonUnload,
    /* Flags       */ AF_None,
    /* Provider    */ UP_None,
    /* UpdateLink  */ nullptr,
};

} // namespace DailyTracker

// ---------------------------------------------------------------------------
// Exported entry point
// ---------------------------------------------------------------------------
extern "C" __declspec(dllexport) AddonDefinition_t* GetAddonDef()
{
    return &DailyTracker::g_addonDef;
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID /*lpReserved*/)
{
    switch (reason)
    {
        case DLL_PROCESS_ATTACH:
        case DLL_THREAD_ATTACH:
        case DLL_THREAD_DETACH:
        case DLL_PROCESS_DETACH:
            break;
    }
    return TRUE;
}
