#pragma once

#include <IPluginInterface.h>

#include <random>
#include <Windows.h>
#include <winhttp.h>
#include <atomic>

// Forward declarations for network hook types
class ZAchievementManagerSimple;
class ZDynamicObject;
class ZHttpResultDynamicObject;

// =============================================================================
// NoMoreAltF4 - Automatic death prevention for Hitman WoA Freelancer mode
//
// ZHMModSDK plugin that monitors player alive state during Freelancer missions
// and terminates the game process on death before the failure is saved.
// Also provides a manual hotkey for aborting missions without penalty.
//
// BUILD: This plugin builds within a CMake project that fetches ZHMModSDK
// via FetchContent. Place Src/ files in your project's Src/ directory.
// =============================================================================

class NoMoreAltF4 : public IPluginInterface
{
public:
    // --- IPluginInterface overrides ---
    void Init() override;
    void OnEngineInitialized() override;

    // Called every frame � we use this for death detection and hotkey polling.
    // (IPluginInterface has no OnFrameUpdate; OnDrawUI runs per-frame.)
    void OnDrawUI(bool p_HasFocus) override;

    // SDK overlay menu for settings.
    void OnDrawMenu() override;

    ~NoMoreAltF4() override;

    // --- Plugin macro (header declaration) ---
    // Creates the Plugin() accessor function and declares DLL exports.
    // The matching DEFINE_ZHM_PLUGIN goes in the .cpp file.

private:
    // --- Core Logic ---
    void KillProcess();
    bool IsFreelancerMode() const;
    bool IsPlayerAlive() const;
    bool ShouldProtect() const;  // true when protection features should be active

    // --- State ---
    std::atomic<bool> m_WasAlive{ false };
    std::atomic<bool> m_DeathDetected{ false };  // Set by OnEventReceived on ContractFailed
    bool m_AutoKillEnabled = true;
    bool m_FreelancerOnly = false;
    bool m_Initialized = false;
    bool m_PlayerWasInMission = false;            // For resetting death flag on mission entry
    bool m_AllowExitToMenu = false;               // Allow "Exit to Main Menu" ContractFailed events through

    // --- Hotkey ---
    int m_ManualKillKey = 0;              // Default: None (0 = disabled)
    bool m_ManualKillKeyPrevState = false; // Edge detection

    // --- Network blocking ---
    // Set to true in KillProcess() before TerminateProcess() so that any
    // in-flight achievement/HTTP sends on other threads are also intercepted.
    bool m_BlockNetworkOnDeath = true;
    bool m_LogHttpRequests = true;        // Diagnostic: log all IOI HTTP traffic
    std::atomic<bool> m_NetworkBlocked{ false };

    // --- WinHttpSendRequest manual hook ---
    void InstallSendRequestHook();
    void RemoveSendRequestHook();
    static std::atomic<bool> s_HookPassthrough;  // When true, HookedSendRequest just calls original

public:
    // Public so the free-function PatchIAT helper can use the typedef.
    using FnWinHttpSendRequest = BOOL(WINAPI*)(
        HINTERNET hRequest, LPCWSTR pwszHeaders, DWORD dwHeadersLength,
        LPVOID lpOptional, DWORD dwOptionalLength,
        DWORD dwTotalLength, DWORD_PTR dwContext);

    static FnWinHttpSendRequest s_OriginalSendRequest;

    static BOOL WINAPI HookedSendRequest(
        HINTERNET hRequest, LPCWSTR pwszHeaders, DWORD dwHeadersLength,
        LPVOID lpOptional, DWORD dwOptionalLength,
        DWORD dwTotalLength, DWORD_PTR dwContext);

private:

    // --- Event hooks ---
    // ZAchievementManagerSimple_OnEventReceived: fires for SERVER→CLIENT events
    //   (e.g. SegmentClosing).  Does NOT fire for client-side events like
    //   ContractFailed.  Used to block SegmentClosing when death is detected.
    DECLARE_PLUGIN_DETOUR(NoMoreAltF4, void, ZAchievementManagerSimple_OnEventReceived,
        ZAchievementManagerSimple* th, const SOnlineEvent& event);

    // --- Network hooks ---
    // Http_WinHttpCallback: fires on every WinHTTP status event.
    //   Used for diagnostic URL logging (SENDING_REQUEST status).
    DECLARE_PLUGIN_DETOUR(NoMoreAltF4, void, Http_WinHttpCallback,
        void* dwContext, void* hInternet, void* param_3, int dwInternetStatus,
        void* param_5, int param_6);

    // ZHttpResultDynamicObject_OnBufferReady: fires when an HTTP response body
    //   is fully received. Used to log response content in diagnostic mode.
    DECLARE_PLUGIN_DETOUR(NoMoreAltF4, void, ZHttpResultDynamicObject_OnBufferReady,
        ZHttpResultDynamicObject* th);

    // ZAchievementManagerSimple_OnEventSent: fires when the game dispatches an
    //   online event (kills, mission outcomes, ET results, etc.) to IOI servers.
    //   Primary blocking point — returning early drops the event before any HTTP
    //   call is made, which is earlier than WinHTTP-level interception.
    DECLARE_PLUGIN_DETOUR(NoMoreAltF4, void, ZAchievementManagerSimple_OnEventSent,
        ZAchievementManagerSimple* th, uint32_t eventIndex, const ZDynamicObject& event);
};

// Declare DLL exports + Plugin() accessor
DECLARE_ZHM_PLUGIN(NoMoreAltF4);