#pragma once

#include <IPluginInterface.h>

#include <random>
#include <Windows.h>
#include <winhttp.h>
#include <atomic>
#include <functional>
#include <string>

#include "LogMirror.h"

// Forward declarations for network hook types
class ZAchievementManagerSimple;
class ZDynamicObject;
class ZHttpResultDynamicObject;
class ZString;
class ZAsyncContext;
struct SHttpRequestBehavior;

// =============================================================================
// NoMoreAltF4 - failure protection for HITMAN World of Assassination
//
// ZHMModSDK plugin. When a Freelancer, Elusive Target or Arcade attempt fails,
// it either terminates the game before the failure is saved (the default), or,
// with Seamless on, rewrites the failed attempt's outgoing events so the server
// never learns of the failure and the game keeps running. It also provides a
// manual abort key and can re-enable saving in no-save story missions.
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
    std::atomic<bool> m_DeathDetected{ false };  // Set by HookedSendRequest on death events
    std::atomic<bool> m_FreelancerDetected{ false }; // Set when "Evergreen" events seen in HTTP traffic
    std::atomic<bool> m_ElusiveOrArcadeDetected{ false }; // Set when ContractType "arcade"/"elusive" seen in HTTP traffic
    bool m_AutoKillEnabled = true;
    bool m_FreelancerOnly = false;
    bool m_Initialized = false;
    bool m_PlayerWasInMission = false;            // For resetting death flag on mission entry
    bool m_AllowExitToMenu = false;               // Allow "Exit to Main Menu" ContractFailed events through

    // --- Hotkey ---
    int m_ManualKillKey = 0;              // Default: None (0 = disabled)
    bool m_ManualKillKeyPrevState = false; // Edge detection
    bool m_AbortQueued = false;            // abort key pressed while a session was starting (render thread)
    uint64_t m_AbortIgnoredUntilMs = 0;    // show "abort key ignored" until this tick (render thread)

    // --- Saving in no-save story missions and escalations ---
    std::atomic<bool> m_EnableSavingStory{ false };

    // --- Network blocking ---
    // Set to true in KillProcess() before TerminateProcess() so that any
    // in-flight achievement/HTTP sends on other threads are also intercepted.
    bool m_BlockNetworkOnDeath = true;
    bool m_LogHttpRequests = true;        // Diagnostic: log all IOI HTTP traffic

    // --- Session log archive ---
    // ZHMModLoader.log is truncated on every launch; this mirrors it (scrubbed)
    // into <log dir>\NoMoreAltF4Logs\ so a session survives the next start.
    bool m_ArchiveLogs = true;
    LogMirror m_LogMirror;
    std::atomic<bool> m_NetworkBlocked{ false };
    // Tick (GetTickCount64) at which the network block was first observed active.
    // Drives the safety release valve in OnDrawUI so a block can never persist
    // indefinitely and freeze the game. 0 = not currently timing a block.
    std::atomic<uint64_t> m_NetworkBlockedAtMs{ 0 };

    // --- Seamless protection (experimental) ---
    // Instead of terminating, a failed attempt is quarantined and its events are
    // rewritten in outgoing bodies (see EventBodyRewriter.h). Opt-in per
    // contract type. Freelancer is experimental and fails closed (see OnEventSent).
    enum class SeamlessMode { Off, Test, Staked };
    std::atomic<bool> m_Seamless{ false };           // story/other contracts, Arcade and Elusive Targets
    std::atomic<bool> m_SeamlessFreelancer{ false }; // experimental: see the guard in OnEventSent
    std::atomic<bool> m_SeamlessAlarm{ false };          // the server closed a contained attempt unexpectedly
    std::atomic<bool> m_SeamlessPersistPending{ false }; // settings changed off the render thread
    std::atomic<uint64_t> m_SeamlessNoticeUntilMs{ 0 };
    std::atomic<bool> m_SeamlessNoticeExit{ false };     // the last contained event was a pause-menu exit
    std::atomic<bool> m_SeamlessNoticeFreelancer{ false }; // the last contained attempt was a Freelancer mission

    bool AnySeamlessEnabled() const;
    SeamlessMode SeamlessFor(const char* p_Csid, size_t p_Len) const;
    void OnSeamlessArmed(const char* p_Csid, size_t p_Len, bool p_ExitToMenu);
    void LoadPastQuarantine();
    void CheckServerCloses(const char* p_Text, size_t p_Len);
    void DrawSeamlessNotice();

    // Runs first in HookedSendRequest, on static state only (so it keeps working
    // while the plugin is disabled). Returns the body pointer to send.
    static LPVOID SeamlessFilter(HINTERNET hRequest, LPCWSTR pwszHeaders, DWORD dwHeadersLength,
                                 LPVOID lpOptional, DWORD dwOptionalLength, DWORD dwTotalLength);
    static void SeamlessFallback(const char* p_Reason);

    // --- WinHttpSendRequest manual hook ---
    void InstallSendRequestHook();
    void RemoveSendRequestHook();
    static std::atomic<bool> s_HookPassthrough;  // When true, HookedSendRequest just calls original
    // Live plugin instance for HookedSendRequest (runs on WinHTTP threads). Never use Plugin()
    // there: after unload it would *create* a new instance via GetPluginInterface().
    static std::atomic<NoMoreAltF4*> s_Instance;
    // Calls currently inside HookedSendRequest; the destructor drains this before returning.
    static std::atomic<int> s_InFlight;

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

    // ZUserChannelContractsProxyBase_GetForPlay2: the request that loads a contract.
    //   Its reply carries Contract.Data.EnableSaving; see PatchEnableSaving.
    DECLARE_PLUGIN_DETOUR(NoMoreAltF4, void, ZUserChannelContractsProxyBase_GetForPlay2,
        const ZString& id, const ZString& locationId, const ZDynamicObject& extraGameChangedIds, int difficulty,
        const std::function<void(const ZDynamicObject&)>& onOk, const std::function<void(int)>& onError,
        ZAsyncContext* ctx, const SHttpRequestBehavior& behavior);
};

// Declare DLL exports + Plugin() accessor
DECLARE_ZHM_PLUGIN(NoMoreAltF4);