#include "NoMoreAltF4.h"
#include "EventBodyRewriter.h"

#include <Logging.h>
#include <IconsMaterialDesign.h>
#include <imgui.h>
#include <Hooks.h>
#include <Functions.h>
#include <Globals.h>
#include <Glacier/ZHttp.h>
#include <Glacier/ZHitman5.h>
#include <Glacier/ZContract.h>
#include <Glacier/SOnlineEvent.h>

#include <Windows.h>
#include <shellapi.h>
#pragma comment(lib, "winhttp.lib")

#include <zlib.h>

#include <algorithm>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <cwctype>
#include <string>
#include <string_view>
#include <vector>

// =============================================================================
// SDK Plugin Registration
// =============================================================================
// DEFINE_ZHM_PLUGIN creates the singleton instance and exports:
//   GetPluginInterface(), CompiledSdkVersion(), CompiledSdkAbiVersion()
DEFINE_ZHM_PLUGIN(NoMoreAltF4);

// Settings INI section name.
// MUST be all-lowercase, as must every setting key: SDK v4.0.2's ModSettings
// does case-SENSITIVE lookups against keys that mINI lowercases when the INI
// file is written, so mixed-case names save fine but silently fail to load
// on the next launch (every session would start with defaults).
static const ZString S_SETTINGS_SECTION = "nomorealtf4";

// Shown in the menu and logged at load. Keep in step with the release tag.
static constexpr const char* k_Version = "1.0.0";

// Freelancer Seamless (experimental): set once a Freelancer mission failure has
// been contained. From then on the game's own campaign data may be the failed one,
// so any campaign upload (CpdSet) other than a display page flip, the start of the
// next Freelancer mission, and disabling the plugin all close the game first.
//
// Released when the game has reloaded the server's campaign: the server has
// closed the contained attempt as an orphaned session in this run, AND a safehouse
// session has started after the containment. (Test of 2026-11-13: the safehouse
// GetForPlay2 then served the pre-mission campaign, the game showed it, and its
// next ExitInventory carried the kept items.)
static std::atomic<bool> s_FreelancerGuard{ false };

// The contained Freelancer attempt the guard waits for (written on the game
// thread at arming, read on the HTTP thread when the server's close arrives).
// Release needs all of: the server closed it as OrphanedSession, no unexpected
// close of it, the game went back to the Hub (the route through the main menu is
// the one verified to reload the campaign), and a safehouse started after that.
static SRWLOCK s_FreelancerLock = SRWLOCK_INIT;
static std::string s_FreelancerContainedCsid;
static std::atomic<bool> s_FreelancerClosedOk{ false };       // server closed it as OrphanedSession
static std::atomic<bool> s_FreelancerClosedBad{ false };      // server closed it in another way (ALARM)
static std::atomic<bool> s_FreelancerHubSeen{ false };        // the Hub page was requested after the containment
static std::atomic<bool> s_FreelancerSafehouseSeen{ false };  // a safehouse started after the Hub

// After a release, the first safehouse visit is watched: a campaign upload that
// looks like the game processing the failure itself (a reset) closes the game.
// Cleared at the next Freelancer mission start. Game thread only.
static bool s_FreelancerWatch = false;

// Backstop for a Freelancer mission that starts while the guard is on (the
// contract-load check in SeamlessFilter normally closes the game before the
// mission is even requested). The game is closed a few seconds AFTER the request
// carrying the mission's ContractStart was sent, never before it. Closing it at the
// ContractStart itself (2026-11-13) left the server a session that had events but
// no ContractStart. From the next run on, the server closed no session as
// completed. A session whose start was uploaded and then abandoned is the ordinary
// Alt-F4 case. The guard's campaign-upload checks keep working during the wait.
// The clock starts when the request is handed to WinHTTP, not when the server has
// stored it, so the wait is long and restarts whenever the game re-sends the start.
static std::string s_GuardKillCsid;                    // under s_FreelancerLock
static std::atomic<bool> s_GuardKillPending{ false };  // waiting for that ContractStart to be sent
static std::atomic<uint64_t> s_GuardKillAt{ 0 };       // GetTickCount64() deadline; 0 = not sent yet
constexpr uint64_t k_GuardKillDelayMs = 30000;

// Game thread: the mission's ContractStart is about to be queued for upload.
static void ScheduleGuardKill(const char* p_Csid, size_t p_Len)
{
    if (s_GuardKillPending.load())
        return;
    AcquireSRWLockExclusive(&s_FreelancerLock);
    s_GuardKillCsid.assign(p_Csid, p_Len);
    ReleaseSRWLockExclusive(&s_FreelancerLock);
    s_GuardKillPending = true;
}

// HTTP thread, just before a request is sent: (re)starts the countdown each time
// a body carries the pending mission's ContractStart (a re-send restarts it).
static void NoteGuardKillBody(const char* p_Body, size_t p_Len)
{
    if (!s_GuardKillPending.load())
        return;
    const std::string_view s_Body(p_Body, p_Len);
    if (s_Body.find("\"Name\":\"ContractStart\"") == std::string_view::npos)
        return;
    AcquireSRWLockShared(&s_FreelancerLock);
    const bool s_Hit = !s_GuardKillCsid.empty() && s_Body.find(s_GuardKillCsid) != std::string_view::npos;
    ReleaseSRWLockShared(&s_FreelancerLock);
    if (!s_Hit)
        return;
    const uint64_t s_Previous = s_GuardKillAt.exchange(GetTickCount64() + k_GuardKillDelayMs);
    Logger::Warn("[NoMoreAltF4] SEAMLESS FREELANCER GUARD: the mission's ContractStart is being {}; "
                 "the game closes {} s after its last upload, as after an Alt-F4.",
                 s_Previous ? "re-sent" : "uploaded", k_GuardKillDelayMs / 1000);
}

// Any thread.
static bool GuardKillDue()
{
    const uint64_t s_At = s_GuardKillAt.load();
    return s_At != 0 && GetTickCount64() >= s_At;
}

// A contract session is starting: from the request that opens it (GetForPlay2,
// or ContractSessionsService/Load for a save) until the body carrying its
// ContractStart or ContractLoad has been handed to WinHTTP and k_StartAckMs have
// passed. A kill in that window leaves the server a session with no events, or
// with events but no start (the 2026-11-13 incident shape). Measured windows:
// up to about 31 s from the request to the first upload, and up to 16 s from the
// first upload to the start in Arcade and ETs. The abort key waits for it to
// pass. If the start is never seen, the wait ends k_StartMaxWaitMs after the
// request while nothing of the session was uploaded (a failed load), or
// k_StartHardCapMs after it once events were uploaded. Any thread.
static std::atomic<uint64_t> s_StartOpenedAt{ 0 };  // tick of the opening request; 0 = none
static std::atomic<uint64_t> s_StartUploadAt{ 0 };  // tick of the first events upload after it; 0 = none
static std::atomic<uint64_t> s_StartSentAt{ 0 };    // tick of the last hand-off of its start; 0 = not yet
constexpr uint64_t k_StartAckMs = 5000;
constexpr uint64_t k_StartMaxWaitMs = 60000;   // longest logged request-to-start: 32 s
constexpr uint64_t k_StartHardCapMs = 180000;

static void NoteSessionStartTraffic(HINTERNET p_Request, const char* p_Body, size_t p_Len)
{
    if (p_Request)
    {
        wchar_t s_UrlBuf[512] = {};
        DWORD s_UrlLen = sizeof(s_UrlBuf);
        if (WinHttpQueryOption(p_Request, WINHTTP_OPTION_URL, s_UrlBuf, &s_UrlLen))
        {
            const std::wstring_view s_Url(s_UrlBuf);
            if (s_Url == L"https://hm3-service.hitman.io/authentication/api/userchannel/ContractsService/GetForPlay2"
                || s_Url == L"https://hm3-service.hitman.io/authentication/api/userchannel/ContractSessionsService/Load")
            {
                s_StartSentAt = 0;
                s_StartUploadAt = 0;
                s_StartOpenedAt = GetTickCount64();
                return;
            }
        }
    }

    if (s_StartOpenedAt.load() == 0 || !p_Body || p_Len == 0)
        return;
    const std::string_view s_Body(p_Body, p_Len);
    if (s_StartUploadAt.load() == 0 && s_Body.starts_with("{\"userId\":")
        && s_Body.find("\"values\":[{") != std::string_view::npos)
        s_StartUploadAt = GetTickCount64();
    if (s_Body.find("\"Name\":\"ContractStart\"") != std::string_view::npos
        || s_Body.find("\"Name\":\"ContractLoad\"") != std::string_view::npos)
        s_StartSentAt = GetTickCount64();
}

// Any thread: true when no contract session is in the middle of starting.
static bool SessionStartSettled()
{
    const uint64_t s_Opened = s_StartOpenedAt.load();
    if (s_Opened == 0)
        return true;
    const uint64_t s_Sent = s_StartSentAt.load();
    const uint64_t s_Upload = s_StartUploadAt.load();
    if (s_StartOpenedAt.load() != s_Opened)
        return false;  // a new session opened meanwhile: look again next time

    // The clock is read after the loads, so it is never behind a tick stored before them.
    const uint64_t s_Now = GetTickCount64();
    if (s_Sent != 0 && s_Sent >= s_Opened)
        return s_Now >= s_Sent && s_Now - s_Sent >= k_StartAckMs;  // start seen: always the full wait
    if (s_Upload != 0 && s_Upload >= s_Opened)
        return s_Now - s_Opened >= k_StartHardCapMs;  // events out, no start: closing now is the bad shape
    return s_Now - s_Opened >= k_StartMaxWaitMs;      // nothing uploaded: probably a failed load
}

// Drops a backstop kill that never got its deadline (its ContractStart was never
// seen going out), so a stale one cannot block a later schedule. Caller holds no lock.
static void ClearUnarmedGuardKill()
{
    if (s_GuardKillAt.load() != 0)
        return;
    s_GuardKillPending = false;
    AcquireSRWLockExclusive(&s_FreelancerLock);
    s_GuardKillCsid.clear();
    ReleaseSRWLockExclusive(&s_FreelancerLock);
}

static void NoteFreelancerContained(const char* p_Csid, size_t p_Len)
{
    ClearUnarmedGuardKill();
    AcquireSRWLockExclusive(&s_FreelancerLock);
    s_FreelancerContainedCsid.assign(p_Csid, p_Len);
    s_FreelancerClosedOk = false;
    s_FreelancerClosedBad = false;
    s_FreelancerHubSeen = false;
    s_FreelancerSafehouseSeen = false;
    ReleaseSRWLockExclusive(&s_FreelancerLock);
}

// HTTP thread: the server closed an attempt of this run (p_Orphaned: as an
// orphaned session). The flag is stored under the lock, so a new arming cannot
// be overtaken by a late record of an earlier attempt.
static void NoteFreelancerClosed(const char* p_Csid, bool p_Orphaned)
{
    AcquireSRWLockExclusive(&s_FreelancerLock);
    if (!s_FreelancerContainedCsid.empty() && s_FreelancerContainedCsid == p_Csid)
    {
        if (p_Orphaned)
            s_FreelancerClosedOk = true;
        else
            s_FreelancerClosedBad = true;
    }
    ReleaseSRWLockExclusive(&s_FreelancerLock);
}

// Any thread (atomics only): every release condition holds, so the game thread
// releases the guard at its next event.
static bool FreelancerReleaseDue()
{
    return s_FreelancerClosedOk.load() && !s_FreelancerClosedBad.load()
        && s_FreelancerHubSeen.load() && s_FreelancerSafehouseSeen.load();
}

// Game thread: releases the guard once every condition holds. Returns true if released.
static bool TryReleaseFreelancerGuard()
{
    if (!s_FreelancerGuard.load() || !FreelancerReleaseDue())
        return false;
    s_FreelancerGuard = false;
    s_FreelancerWatch = true;
    ClearUnarmedGuardKill();
    return true;
}

// Saving in no-save story missions and escalations. The server's GetForPlay2 reply
// carries Contract.Data.EnableSaving; the game greys the Save button out when it is
// false. For contracts of type "flashback" (Holiday Hoarders and similar) and
// "escalation" the flag is set to true in place, before the game reads the reply.
// Freelancer, Elusive Targets and Arcade are never touched. Anything unexpected
// leaves the reply as it is. Only header-inline accessors are used: the SDK does
// not export ZDynamicObject's own Get/Set/operator[].
static const ZDynamicObject* FindKey(const ZDynamicObject& p_Object, std::string_view p_Key)
{
    const auto* s_Pairs = p_Object.As<TArray<SDynamicObjectKeyValuePair>>();
    if (!s_Pairs)
        return nullptr;
    for (auto& s_Pair : *s_Pairs)
        if (std::string_view(s_Pair.sKey.c_str(), s_Pair.sKey.size()) == p_Key)
            return &s_Pair.value;
    return nullptr;
}

static void PatchEnableSaving(const ZDynamicObject& p_Response)
{
    const ZDynamicObject* s_Contract = FindKey(p_Response, "Contract");
    const ZDynamicObject* s_Metadata = s_Contract ? FindKey(*s_Contract, "Metadata") : nullptr;
    const ZDynamicObject* s_Data = s_Contract ? FindKey(*s_Contract, "Data") : nullptr;
    const ZDynamicObject* s_TypeValue = s_Metadata ? FindKey(*s_Metadata, "Type") : nullptr;
    const ZString* s_Type = s_TypeValue ? s_TypeValue->As<ZString>() : nullptr;
    if (!s_Data || !s_Type)
    {
        Logger::Warn("[NoMoreAltF4] SAVE-ENABLE: unexpected contract reply shape, left unchanged.");
        return;
    }

    const std::string_view s_TypeName(s_Type->c_str(), s_Type->size());
    if (s_TypeName != "flashback" && s_TypeName != "escalation")
        return;

    const ZDynamicObject* s_Flag = FindKey(*s_Data, "EnableSaving");
    bool* s_Value = s_Flag ? s_Flag->As<bool>() : nullptr;
    if (!s_Value)
    {
        Logger::Warn("[NoMoreAltF4] SAVE-ENABLE type={}: no boolean EnableSaving in the reply, left unchanged.", s_TypeName);
        return;
    }
    if (*s_Value)
        return;

    *s_Value = true;
    Logger::Info("[NoMoreAltF4] SAVE-ENABLE type={}: EnableSaving false -> true (saving allowed in this contract).", s_TypeName);
}

// Second step, on the game's own stored contract data (what its "is saving
// enabled" reader looks at: ZContractsManager::m_contractContext.m_contractData,
// ["Data"]["EnableSaving"]). Test of 2026-11-13: patching the reply alone left the
// Save button grey. Called about once a second from the render thread while the
// setting is on; each distinct state is logged once, as a diagnostic.
static void PatchLiveContractData()
{
    static std::string s_LastState;
    std::string s_State;

    auto* s_Manager = Globals::ContractsManager;
    if (!s_Manager)
        return;

    const ZDynamicObject& s_Contract = s_Manager->m_contractContext.m_contractData;
    const ZDynamicObject* s_Metadata = FindKey(s_Contract, "Metadata");
    const ZDynamicObject* s_Data = FindKey(s_Contract, "Data");
    const ZDynamicObject* s_TypeValue = s_Metadata ? FindKey(*s_Metadata, "Type") : nullptr;
    const ZString* s_Type = s_TypeValue ? s_TypeValue->As<ZString>() : nullptr;
    const ZDynamicObject* s_Flag = s_Data ? FindKey(*s_Data, "EnableSaving") : nullptr;
    bool* s_Value = s_Flag ? s_Flag->As<bool>() : nullptr;

    // Undeclared byte the game's reader checks first (set = saving refused).
    const uint8_t s_Block = *(reinterpret_cast<const uint8_t*>(s_Manager) + 0x9B1);

    const std::string s_TypeName = s_Type ? std::string(s_Type->c_str(), s_Type->size()) : std::string("?");
    bool s_Patched = false;
    if (s_Value && !*s_Value && (s_TypeName == "flashback" || s_TypeName == "escalation"))
    {
        *s_Value = true;
        s_Patched = true;
    }

    s_State = "type=" + s_TypeName + " data=" + (s_Data ? "yes" : "no")
        + " flag=" + (s_Value ? (*s_Value ? "true" : "false") : (s_Flag ? "not-bool" : "missing"))
        + " block9B1=" + std::to_string(s_Block) + (s_Patched ? " PATCHED" : "");
    if (s_State != s_LastState)
    {
        s_LastState = s_State;
        Logger::Info("[NoMoreAltF4] SAVE-ENABLE live contract data: {}", s_State);
    }
}

DEFINE_PLUGIN_DETOUR(NoMoreAltF4, void, ZUserChannelContractsProxyBase_GetForPlay2,
    const ZString& id, const ZString& locationId, const ZDynamicObject& extraGameChangedIds, int difficulty,
    const std::function<void(const ZDynamicObject&)>& onOk, const std::function<void(int)>& onError,
    ZAsyncContext* ctx, const SHttpRequestBehavior& behavior)
{
    // Setting off: do nothing at all, the next detour (the SDK's own) carries on.
    if (!m_EnableSavingStory.load())
        return HookAction::Continue();

    // The game keeps the callback until the reply arrives. It captures no plugin
    // state, and the DLL is pinned, so it stays valid if the plugin is disabled.
    const std::function<void(const ZDynamicObject&)> s_Wrapped = [s_Ok = onOk](const ZDynamicObject& p_Response)
    {
        try
        {
            PatchEnableSaving(p_Response);
        }
        catch (...)
        {
            Logger::Warn("[NoMoreAltF4] SAVE-ENABLE: exception while reading the contract reply, left unchanged.");
        }
        s_Ok(p_Response);
    };

    // The SDK's own detour for this hook does exactly this with the original callback.
    p_Hook->CallOriginal(id, locationId, extraGameChangedIds, difficulty, s_Wrapped, onError, ctx, behavior);
    return HookAction::Return();
}

// The campaign keys a safehouse upload only carries when it processes a failed
// mission (seen once: the reset after the 2026-11-12 showdown exit). Legitimate
// safehouse uploads in all logs carry none of them.
static bool LooksLikeFailureProcessing(std::string_view p_Text)
{
    return p_Text.find("\"AnyActiveCampaign\":false") != std::string_view::npos
        || p_Text.find("_MissionFailed\":") != std::string_view::npos
        || p_Text.find("\"CPD_CampaignStep\":") != std::string_view::npos
        || p_Text.find("\"CPD_CampaignProgress\":") != std::string_view::npos
        || p_Text.find("\"HeroWounded\":") != std::string_view::npos;
}


// Safety release valve: the maximum time a network block may stay active without
// the process having terminated.  This bounds the "drop outgoing online events"
// state so an unexpected event path can never leave the game stuck.
static constexpr uint64_t k_NetworkBlockTimeoutMs = 8000;

// Max bytes of an HTTP body/response written to the log. Set high (1 MB) so full
// payloads are visible for diagnostics — only a pathologically huge body is
// clipped. Logging is gated behind the "Log HTTP requests" toggle anyway.
static constexpr size_t k_MaxLogBytes = 1u << 20;

// IMPORTANT — why we NEVER short-circuit WinHttpSendRequest:
//
// The game drives WinHTTP asynchronously (it registers Http_WinHttpCallback).
// If our IAT hook returns from WinHttpSendRequest WITHOUT calling the original:
//   - returning TRUE  → no completion callback ever fires → the game's save/sync
//                       state machine waits forever (the "game froze" report).
//   - returning FALSE → the game's send-failed path dereferences a half-built
//                       async context → access violation crash (0xC0000005).
// Both are confirmed from user logs.  Therefore the WinHTTP hook is OBSERVE-ONLY:
// it logs, detects, and may TerminateProcess (which is clean), but it always
// otherwise calls the original.  Actual event suppression for block-only mode is
// done at the game's own event layer (ZAchievementManagerSimple_OnEventSent),
// which is the SDK-sanctioned, crash-safe interception point.

// Static storage for the original WinHttpSendRequest pointer (pre-hook).
NoMoreAltF4::FnWinHttpSendRequest NoMoreAltF4::s_OriginalSendRequest = nullptr;
std::atomic<bool> NoMoreAltF4::s_HookPassthrough{ false };
std::atomic<NoMoreAltF4*> NoMoreAltF4::s_Instance{ nullptr };
std::atomic<int> NoMoreAltF4::s_InFlight{ 0 };

// Pin this DLL so it is never unmapped while the game runs.
//
// Workaround for ZHMModSDK <= 4.2.1 (OrfeasZ/ZHMModSDK#280): every mod links imgui
// statically, and the patched imgui's stack-layout code registers context hooks
// (NewFramePre / EndFramePre / Shutdown) into the SDK's *shared* ImGui context with
// callbacks pointing into this DLL the first time our menu draws anything. The SDK
// never removes them, so when the mod is disabled at runtime and FreeLibrary'd, the
// next ImGui::EndFrame() calls StackLayout_EndFramePreCallback in unmapped memory.
// Pinned, the SDK still deletes the plugin object (all our logic stops), but the code
// those stale callbacks point at stays valid. It also makes any WinHTTP call that is
// mid-flight inside HookedSendRequest during unload safe. Cost: the DLL file stays
// locked until the game exits (no hot-swapping a rebuilt DLL in the same session).
static void PinThisModule()
{
    HMODULE s_Module = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                            reinterpret_cast<LPCWSTR>(&PinThisModule), &s_Module))
        Logger::Warn("[NoMoreAltF4] Could not pin module (error {}); disabling the mod at runtime may crash the game.",
                     GetLastError());
}

// =============================================================================
// Initialization
// =============================================================================

void NoMoreAltF4::Init()
{
    PinThisModule();

    // Load persisted settings from the plugin's INI file.
    // Settings API: GetSettingBool(section, name, defaultValue)
    m_AutoKillEnabled      = GetSettingBool(S_SETTINGS_SECTION, "autokillenabled",      true);
    m_FreelancerOnly       = GetSettingBool(S_SETTINGS_SECTION, "freelanceronly",       false);
    m_ManualKillKey        = static_cast<int>(GetSettingInt(S_SETTINGS_SECTION, "manualkillkey", 0));
    m_BlockNetworkOnDeath  = GetSettingBool(S_SETTINGS_SECTION, "blocknetworkondeath",  true);
    m_LogHttpRequests      = GetSettingBool(S_SETTINGS_SECTION, "loghttprequests",      true);
    m_AllowExitToMenu      = GetSettingBool(S_SETTINGS_SECTION, "allowexittomenu",      false);
    m_ArchiveLogs          = GetSettingBool(S_SETTINGS_SECTION, "archivesessionlogs",   true);
    // One switch since v1.0.0. Before that there was one setting per contract
    // type; if any of those was on, the switch starts on.
    const bool s_OldSeamless = GetSettingBool(S_SETTINGS_SECTION, "seamlessother",   false)
                            || GetSettingBool(S_SETTINGS_SECTION, "seamlessarcade",  false)
                            || GetSettingBool(S_SETTINGS_SECTION, "seamlesselusive", false);
    m_Seamless             = GetSettingBool(S_SETTINGS_SECTION, "seamless",             s_OldSeamless);
    m_SeamlessFreelancer   = GetSettingBool(S_SETTINGS_SECTION, "seamlessfreelancer",   false);
    m_EnableSavingStory    = GetSettingBool(S_SETTINGS_SECTION, "enablesavingstory",    false);

    // Must be added here, not in OnEngineInitialized: the SDK adds its own detour
    // for this hook right after the mods' Init(), and that one ends the chain.
    Hooks::ZUserChannelContractsProxyBase_GetForPlay2->AddDetour(this, &NoMoreAltF4::ZUserChannelContractsProxyBase_GetForPlay2);

    // Start mirroring ZHMModLoader.log into NoMoreAltF4Logs\ as early as
    // possible (the SDK truncates the live log on every launch).
    m_LogMirror.Start(m_ArchiveLogs);

    // Attempts contained in earlier runs: the server closes them at this login,
    // and CheckServerCloses reports how.
    LoadPastQuarantine();

    Logger::Info("[NoMoreAltF4] v{} loaded. Auto-kill: {}, Freelancer-only: {}, Hotkey: 0x{:X}, BlockNet: {}, LogHTTP: {}, "
                 "Seamless: {}, Freelancer Seamless: {}, Allow saving: {}",
        k_Version, m_AutoKillEnabled, m_FreelancerOnly, m_ManualKillKey, m_BlockNetworkOnDeath, m_LogHttpRequests,
        m_Seamless.load(), m_SeamlessFreelancer.load(), m_EnableSavingStory.load());
}

void NoMoreAltF4::OnEngineInitialized()
{
    Logger::Info("[NoMoreAltF4] Engine initialized. Death detection active.");
    m_Initialized = true;
    m_WasAlive = false;
    s_HookPassthrough = false;

    // Register SDK event/network hooks.
    Hooks::ZAchievementManagerSimple_OnEventReceived->AddDetour(this, &NoMoreAltF4::ZAchievementManagerSimple_OnEventReceived);
    Hooks::ZAchievementManagerSimple_OnEventSent->AddDetour(this, &NoMoreAltF4::ZAchievementManagerSimple_OnEventSent);
    Hooks::Http_WinHttpCallback->AddDetour(this, &NoMoreAltF4::Http_WinHttpCallback);
    Hooks::ZHttpResultDynamicObject_OnBufferReady->AddDetour(this, &NoMoreAltF4::ZHttpResultDynamicObject_OnBufferReady);

    // Install manual IAT hook for WinHttpSendRequest so we can read POST bodies.
    s_Instance = this;
    InstallSendRequestHook();
}

NoMoreAltF4::~NoMoreAltF4()
{
    // The Freelancer guard lives in the event detour, which goes away with the
    // plugin; after that the game's (possibly failed) campaign could be uploaded.
    // Not while the guard's deferred close waits for a new mission's start to be
    // uploaded: closing now could leave that session without its start. The
    // request hook stays installed (a quarantine exists) and closes the game when
    // the wait is over; its wire-level campaign check holds meanwhile.
    if (s_FreelancerGuard.load())
    {
        if (s_GuardKillPending.load() && !GuardKillDue())
            Logger::Warn("[NoMoreAltF4] Plugin disabled while the Freelancer guard waits for a new mission's start "
                         "to be uploaded; the request hook closes the game when the wait is over.");
        else
        {
            Logger::Warn("[NoMoreAltF4] Plugin disabled while the Freelancer guard is on — closing the game.");
            KillProcess();
        }
    }

    if (m_AbortQueued)
        Logger::Warn("[NoMoreAltF4] Plugin disabled with a queued manual abort: the abort is cancelled.");

    // Make IAT hook a passthrough, then swap IAT to persistent JMP stub
    // so in-flight WinHttpSendRequest calls don't land in freed DLL memory.
    s_HookPassthrough = true;
    s_Instance = nullptr;

    // While an attempt is contained, the hook must stay in the import table:
    // its Seamless step runs on static state only, and removing it would let
    // the contained failure out. The DLL is pinned, so the code stays mapped.
    if (ebr::HasQuarantine())
        Logger::Warn("[NoMoreAltF4] Plugin disabled while {} attempt(s) are contained — "
                     "leaving the request hook installed so they stay contained.", ebr::QuarantineCount());
    else
        RemoveSendRequestHook();

    // Drain WinHTTP calls already inside HookedSendRequest (they may have loaded
    // s_Instance before it was cleared) so none touches this object after it's freed.
    // Bounded: never hang the game's render thread on a stuck network call.
    const ULONGLONG s_DrainStart = GetTickCount64();
    while (s_InFlight.load() > 0 && GetTickCount64() - s_DrainStart < 2000)
        Sleep(1);

    // Final archive append + bounded worker shutdown.
    m_LogMirror.Stop();

    // The DLL is pinned (see PinThisModule), so its statics survive a disable/enable
    // cycle. Reset the singleton DEFINE_ZHM_PLUGIN keeps, or re-enabling the mod would
    // hand the SDK this deleted object.
    g_NoMoreAltF4_Instance = nullptr;

    // SDK detour cleanup is left to the framework (sample mods don't
    // call RemoveDetoursWithContext — the SDK handles it on plugin unload).
    // See: https://github.com/OrfeasZ/ZHMModSDK/issues/XXX
}

// =============================================================================
// Per-Frame Logic (runs inside OnDrawUI, called every frame)
// =============================================================================

void NoMoreAltF4::OnDrawUI(bool p_HasFocus)
{
    if (!m_Initialized)
        return;

    if (GuardKillDue())
        SeamlessFallback("a new Freelancer mission before the campaign was reloaded (closed after its start was uploaded)");

    DrawSeamlessNotice();

    // --- A manual abort pressed while a contract session was starting ---
    if (m_AbortQueued)
    {
        if (SessionStartSettled())
        {
            Logger::Warn("[NoMoreAltF4] Queued manual abort: the session has started. Terminating.");
            KillProcess();
            return;
        }

        // Below the Seamless notice (y 48), which can be showing at the same time.
        ImGui::SetNextWindowPos(ImVec2(ImGui::GetIO().DisplaySize.x * 0.5f, 130.0f), ImGuiCond_Always, ImVec2(0.5f, 0.0f));
        ImGui::SetNextWindowBgAlpha(0.8f);
        if (ImGui::Begin("##NoMoreAltF4AbortQueued", nullptr,
                         ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings
                         | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoInputs))
            ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.3f, 1.0f),
                ICON_MD_HOURGLASS_EMPTY " NoMoreAltF4: abort queued. The game closes a few seconds after the "
                "mission's start has reached the server.");
        ImGui::End();
    }

    // --- Manual hotkey check (edge-triggered: fires once on key down) ---
    if (m_ManualKillKey != 0)
    {
        bool keyDown = (GetAsyncKeyState(m_ManualKillKey) & 0x8000) != 0;
        if (keyDown && !m_ManualKillKeyPrevState)
        {
            // Ignore presses that are part of an Alt/Win combination (e.g. a
            // screen recorder's Alt+F9) — those belong to other tools, not an
            // abort request.  Shift/Ctrl are deliberately NOT excluded: they
            // are sprint/crouch, which a panicking player may be holding while
            // pressing the abort key.
            const bool s_AltOrWinHeld =
                (GetAsyncKeyState(VK_MENU) & 0x8000) != 0
                || (GetAsyncKeyState(VK_LWIN) & 0x8000) != 0
                || (GetAsyncKeyState(VK_RWIN) & 0x8000) != 0;

            if (s_AltOrWinHeld)
            {
                Logger::Info("[NoMoreAltF4] Hotkey 0x{:X} pressed with Alt/Win held — ignoring (combination belongs to another tool).",
                    m_ManualKillKey);
            }
            else if (ShouldProtect() && !SessionStartSettled())
            {
                // Closing during a load can leave the server a half-started
                // session; nothing can be failed before the start anyway.
                if (!m_AbortQueued)
                    Logger::Warn("[NoMoreAltF4] Manual abort (hotkey 0x{:X}) queued: a contract session is still "
                                 "starting. The game closes once its start has been uploaded.", m_ManualKillKey);
                m_AbortQueued = true;
            }
            else if (ShouldProtect())
            {
                Logger::Warn("[NoMoreAltF4] Manual abort triggered (hotkey 0x{:X}). Terminating.", m_ManualKillKey);
                KillProcess();
                return;
            }
            else
            {
                Logger::Info("[NoMoreAltF4] Hotkey 0x{:X} ignored: 'Only Enable for Freelancer' is ON and this is not Freelancer.",
                    m_ManualKillKey);
                m_AbortIgnoredUntilMs = GetTickCount64() + 3000;
            }
        }
        m_ManualKillKeyPrevState = keyDown;
    }

    // Saving in no-save missions: keep the game's stored contract data patched.
    if (m_EnableSavingStory.load())
    {
        static uint64_t s_NextCheck = 0;
        const uint64_t s_Now = GetTickCount64();
        if (s_Now >= s_NextCheck)
        {
            s_NextCheck = s_Now + 1000;
            PatchLiveContractData();
        }
    }

    if (GetTickCount64() < m_AbortIgnoredUntilMs)
    {
        ImGui::SetNextWindowPos(ImVec2(ImGui::GetIO().DisplaySize.x * 0.5f, 130.0f), ImGuiCond_Always, ImVec2(0.5f, 0.0f));
        ImGui::SetNextWindowBgAlpha(0.8f);
        if (ImGui::Begin("##NoMoreAltF4AbortIgnored", nullptr,
                         ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings
                         | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoInputs))
            ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.3f, 1.0f),
                ICON_MD_WARNING " NoMoreAltF4: abort key ignored. 'Only Enable for Freelancer' is ON\n"
                "and this is not Freelancer. Nothing is protected here.");
        ImGui::End();
    }

    // --- Reset transient state on mission boundaries ---
    {
        auto s_LocalPlayer = SDK()->GetLocalPlayer();
        bool s_InMission = static_cast<bool>(s_LocalPlayer);

        if (s_InMission && !m_PlayerWasInMission)
        {
            // Just entered a mission — clear stale flags.  Crucially, clear any
            // network block left over from a previous contract: a block raised in
            // an earlier mission must never leak into this one (e.g. dropping the
            // legitimate success/sync traffic of the next mission and freezing it).
            m_DeathDetected = false;
            m_NetworkBlocked = false;
            m_NetworkBlockedAtMs = 0;
            // Note: m_FreelancerDetected is NOT reset here — it persists across
            // missions within the same Freelancer campaign. It's only reset when
            // the player leaves a mission (below), so it re-detects on next entry.
        }
        if (!s_InMission && m_PlayerWasInMission)
        {
            // Left a mission — reset all per-mission state so nothing leaks into
            // the next one and the mode flags re-detect next time.
            m_FreelancerDetected = false;
            m_ElusiveOrArcadeDetected = false;
            m_DeathDetected = false;
            m_NetworkBlocked = false;
            m_NetworkBlockedAtMs = 0;
        }
        m_PlayerWasInMission = s_InMission;
    }

    // --- Safety release valve ---
    // A network block must never persist indefinitely (it would freeze the game).
    // In the normal flow the process is terminated within milliseconds of a block
    // being raised, so this never fires; it only protects block-only mode and any
    // unexpected event path from leaving the game permanently stuck.
    if (m_NetworkBlocked)
    {
        const uint64_t s_Now = GetTickCount64();
        const uint64_t s_Since = m_NetworkBlockedAtMs.load();
        if (s_Since == 0)
        {
            m_NetworkBlockedAtMs = s_Now; // first frame we observed the block
        }
        else if (s_Now - s_Since > k_NetworkBlockTimeoutMs)
        {
            Logger::Warn("[NoMoreAltF4] Network block release valve fired after {} ms — clearing block to avoid a hang.",
                s_Now - s_Since);
            m_NetworkBlocked = false;
            m_NetworkBlockedAtMs = 0;
        }
    }

    // --- Auto-kill on death ---
    if (!m_AutoKillEnabled)
        return;

    if (!ShouldProtect())
        return;

    bool alive = IsPlayerAlive();

    // Detect transition: was alive last frame, now dead
    // (Backup path — primary kill is triggered directly from OnEventReceived)
    if (m_WasAlive && !alive)
    {
        Logger::Warn("[NoMoreAltF4] Player death detected (frame poll)! Terminating process.");
        KillProcess();
        return;
    }

    m_WasAlive = alive;
}

// =============================================================================
// Player Alive Detection
// =============================================================================
//
// Uses a hybrid approach:
//   1. SDK()->GetLocalPlayer() — returns TEntityRef<ZHitman5>, null when not
//      in a mission (main menu, loading, etc.). Null = safe, don't trigger.
//   2. m_DeathDetected flag — set by HookedSendRequest when it detects failure
//      events (MissionFailed_Event, MissionWounded_Event, MildChess_MissionFailed)
//      in outgoing HTTP POST bodies, or by OnEventReceived when the server sends
//      back a ContractFailed confirmation.
//
// ZHitman5 does not inherit IActor (which has IsDead()/IsAlive()), and does
// not expose an alive/dead field directly in the SDK headers.

bool NoMoreAltF4::IsPlayerAlive() const
{
    // Not in a mission — treat as alive (safe: won't trigger false kills)
    auto s_LocalPlayer = SDK()->GetLocalPlayer();
    if (!s_LocalPlayer)
        return true;

    // Death detected via the game's own event system
    if (m_DeathDetected)
        return false;

    return true;
}

// =============================================================================
// Freelancer Mode Detection
// =============================================================================
//
// Freelancer mode uses the internal codename "Evergreen" throughout Glacier 2.
// Detection uses two methods:
//   1. Scene path check — some Freelancer scenes contain "Evergreen" in the path
//   2. HTTP event detection — Freelancer missions send events with "Evergreen" in
//      the name (Evergreen_MissionPayout, EvergreenMissionStarted, etc.) and use
//      ContractType "evergreen". This is set by HookedSendRequest.
// Method 2 is needed because actual Freelancer mission scenes use regular map
// paths (e.g. miami/scene_flamingo_hot_pinochle.entity), not "Evergreen" paths.

bool NoMoreAltF4::IsFreelancerMode() const
{
    // Method 1: HTTP event detection (most reliable — "Evergreen" appears in
    // event names and contract types in every Freelancer session)
    if (m_FreelancerDetected)
        return true;

    // Method 2: Scene path check (fallback)
    auto* s_ContractsManager = Globals::ContractsManager;
    if (!s_ContractsManager)
        return false;

    const auto& s_Scene = s_ContractsManager->m_contractContext.m_sScene;
    if (s_Scene.size() == 0)
        return false;

    return strstr(s_Scene.c_str(), "Evergreen") != nullptr;
}

// Returns true when protection features should be active.
// When FreelancerOnly is enabled and we're not in Freelancer, everything is off.
bool NoMoreAltF4::ShouldProtect() const
{
    return !m_FreelancerOnly || IsFreelancerMode();
}

// =============================================================================
// Process Termination
// =============================================================================

void NoMoreAltF4::KillProcess()
{
    // Set the network block flag FIRST so any concurrent threads that are about
    // to dispatch achievement events or HTTP requests get intercepted during the
    // brief window between this flag set and the OS actually killing the process.
    if (m_BlockNetworkOnDeath)
    {
        m_NetworkBlocked = true;
        m_NetworkBlockedAtMs = GetTickCount64();
    }

    // Flush all SDK loggers so the detection/TERMINATED lines logged just
    // before this call actually reach ZHMModLoader.log — spdlog's file sink
    // is buffered, and TerminateProcess discards anything unflushed.
    const auto s_Loggers = GetLoggers();
    for (size_t i = 0; i < s_Loggers.Count; ++i)
        s_Loggers.Loggers[i]->flush();

    // Append the just-flushed tail (detection + TERMINATED lines) to the
    // session archive. Bounded and lock-timeout'd: only the few KB written
    // since the mirror's last 500 ms poll, so it doesn't delay the kill.
    m_LogMirror.SyncTail();

    // TerminateProcess is used instead of ExitProcess because:
    // - ExitProcess runs DLL detach routines and atexit handlers, which could
    //   trigger save/cleanup code that writes the death state to IOI servers
    // - TerminateProcess immediately kills the process with no cleanup
    // - This replicates the exact effect of Alt-F4 -> OS force kill
    TerminateProcess(GetCurrentProcess(), 0);

    // Unreachable safety fallback
    ExitProcess(0);
}

// =============================================================================
// WinHttpSendRequest IAT Hook
// =============================================================================
//
// The game's import table has a slot for WinHttpSendRequest. We patch that slot
// to point to our function. This is lighter-weight than an inline hook and does
// not require any external library — just standard PE header walking.
//
// Thread safety: the patch is done under VirtualProtect PAGE_READWRITE, which
// is a single pointer write (atomic on x64). No lock needed.
// =============================================================================

// Helper: walk the game's import table and swap WinHttpSendRequest's slot.
// Returns the old function pointer, or nullptr if the import was not found.
static NoMoreAltF4::FnWinHttpSendRequest PatchIAT(NoMoreAltF4::FnWinHttpSendRequest p_New)
{
    auto* s_Base = reinterpret_cast<uint8_t*>(GetModuleHandleW(nullptr));
    if (!s_Base)
        return nullptr;

    auto& s_DosHdr = *reinterpret_cast<IMAGE_DOS_HEADER*>(s_Base);
    auto& s_NtHdr  = *reinterpret_cast<IMAGE_NT_HEADERS*>(s_Base + s_DosHdr.e_lfanew);

    auto& s_ImpDir = s_NtHdr.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!s_ImpDir.VirtualAddress)
        return nullptr;

    auto* s_Desc = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(s_Base + s_ImpDir.VirtualAddress);
    for (; s_Desc->Name; ++s_Desc)
    {
        const char* s_DllName = reinterpret_cast<const char*>(s_Base + s_Desc->Name);
        if (_stricmp(s_DllName, "winhttp.dll") != 0)
            continue;

        auto* s_Thunk = reinterpret_cast<IMAGE_THUNK_DATA*>(s_Base + s_Desc->FirstThunk);
        auto* s_Orig  = reinterpret_cast<IMAGE_THUNK_DATA*>(s_Base + s_Desc->OriginalFirstThunk);

        for (size_t i = 0; s_Thunk[i].u1.Function; ++i)
        {
            // Match by name (skip ordinal imports)
            if (s_Orig[i].u1.Ordinal & IMAGE_ORDINAL_FLAG)
                continue;

            auto* s_Import = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(s_Base + s_Orig[i].u1.AddressOfData);
            if (strcmp(s_Import->Name, "WinHttpSendRequest") != 0)
                continue;

            // Found the slot — swap it.
            void** s_Slot = reinterpret_cast<void**>(&s_Thunk[i].u1.Function);
            DWORD s_OldProtect;
            VirtualProtect(s_Slot, sizeof(void*), PAGE_READWRITE, &s_OldProtect);
            auto* s_Old = reinterpret_cast<NoMoreAltF4::FnWinHttpSendRequest>(*s_Slot);
            *s_Slot = reinterpret_cast<void*>(p_New);
            VirtualProtect(s_Slot, sizeof(void*), s_OldProtect, &s_OldProtect);
            return s_Old;
        }
    }
    return nullptr;
}

// Persistent trampoline stub allocated via VirtualAlloc.
// This tiny block of executable memory survives DLL unload and simply
// forwards all WinHttpSendRequest calls to the original function.
// When our DLL IS loaded and s_HookPassthrough is false, HookedSendRequest
// does the real work. When the DLL unloads, s_HookPassthrough is set to
// true by the destructor, and HookedSendRequest (still reachable because
// the IAT points here) calls the original immediately.
//
// Layout of persistent data (heap-allocated, never freed):
//   PersistentHookData { original_fn, passthrough_flag, hook_fn }
// The IAT is patched to point at a VirtualAlloc'd thunk that reads these.
//
// HOWEVER — the simplest safe approach for x64 where we can't easily
// write position-independent thunks: we leave our static HookedSendRequest
// in the IAT while loaded, and on unload we restore the IAT to the
// original function. The crash happens because of in-flight calls.
//
// REAL FIX: On unload, patch the IAT to a VirtualAlloc'd stub that
// just does `jmp [original]`. This stub is 14 bytes of machine code
// and a pointer, allocated once and never freed.

// Persistent stub — allocated once, never freed, survives DLL unload.
static void* s_PersistentStub = nullptr;
static NoMoreAltF4::FnWinHttpSendRequest* s_PersistentOrigPtr = nullptr;

static NoMoreAltF4::FnWinHttpSendRequest CreatePersistentJmpStub(NoMoreAltF4::FnWinHttpSendRequest p_Original)
{
    // Allocate executable memory: 14 bytes for `jmp [rip+0]` + 8 bytes for the pointer
    // This memory is intentionally never freed — it must outlive the DLL.
    constexpr size_t k_StubSize = 32;
    auto* s_Mem = static_cast<uint8_t*>(
        VirtualAlloc(nullptr, k_StubSize, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    if (!s_Mem)
        return nullptr;

    // x64: FF 25 00 00 00 00 = jmp qword ptr [rip+0]  (jumps to the address stored right after)
    s_Mem[0] = 0xFF;
    s_Mem[1] = 0x25;
    s_Mem[2] = 0x00;
    s_Mem[3] = 0x00;
    s_Mem[4] = 0x00;
    s_Mem[5] = 0x00;
    // The 8-byte target address immediately follows
    memcpy(s_Mem + 6, &p_Original, sizeof(p_Original));

    // Store pointer to the address slot so we can update it if needed
    s_PersistentOrigPtr = reinterpret_cast<NoMoreAltF4::FnWinHttpSendRequest*>(s_Mem + 6);

    DWORD s_OldProtect;
    VirtualProtect(s_Mem, k_StubSize, PAGE_EXECUTE_READ, &s_OldProtect);

    return reinterpret_cast<NoMoreAltF4::FnWinHttpSendRequest>(s_Mem);
}

void NoMoreAltF4::InstallSendRequestHook()
{
    // Create the persistent stub first — this will be what the IAT points to
    // after DLL unload, so it must exist before we install anything.
    const auto s_Previous = PatchIAT(&NoMoreAltF4::HookedSendRequest);
    if (s_Previous == &NoMoreAltF4::HookedSendRequest && s_OriginalSendRequest)
    {
        // The hook was left installed across a disable (contained attempt).
        // Keep the saved original: taking the hook as "original" would recurse.
        Logger::Info("[NoMoreAltF4] WinHttpSendRequest IAT hook was still installed; reusing it.");
        return;
    }

    s_OriginalSendRequest = s_Previous;
    if (s_OriginalSendRequest)
    {
        s_PersistentStub = reinterpret_cast<void*>(
            CreatePersistentJmpStub(s_OriginalSendRequest));
        Logger::Info("[NoMoreAltF4] WinHttpSendRequest IAT hook installed.");
    }
    else
    {
        Logger::Warn("[NoMoreAltF4] WinHttpSendRequest not found in IAT — POST body logging unavailable.");
    }
}

void NoMoreAltF4::RemoveSendRequestHook()
{
    // Instead of restoring the original function pointer (which races with
    // in-flight calls), patch the IAT to point at the persistent JMP stub.
    // The stub lives in VirtualAlloc'd memory that survives DLL unload and
    // simply jumps to the real WinHttpSendRequest.
    // s_OriginalSendRequest is deliberately NOT cleared: a call already inside
    // HookedSendRequest still forwards through it after this returns.
    if (s_PersistentStub && s_OriginalSendRequest)
        PatchIAT(reinterpret_cast<FnWinHttpSendRequest>(s_PersistentStub));
}

// =============================================================================
// Seamless protection (experimental)
// =============================================================================
//
// Terminate mode keeps a failure off the wire by killing the process. Seamless
// keeps the process alive and instead rewrites, in the outgoing request body,
// every event of the failed attempt into a harmless Hero_Health event of the
// same byte length (EventBodyRewriter). The request itself is sent normally —
// no return value is faked — so WinHTTP's async state machine is untouched.
// The server acknowledges every event, never learns the attempt ended, and
// closes it later as an orphaned session, as it does after an Alt-F4.
//
// Anything unexpected falls back to KillProcess, so Seamless can never protect
// less than Terminate.

// Copies of rewritten bodies are handed to WinHTTP, which reads them
// asynchronously (and again on a redirect or auth challenge), so they are never
// freed. A failure costs a few KB; this caps the total.
static constexpr size_t k_SeamlessCopyBudget = 4u << 20;
static std::atomic<size_t> s_SeamlessCopyBytes{ 0 };

// quarantine.log (in the session-log folder) remembers contained attempts across
// launches, so the next login can report how the server closed them:
//     <csid> <unix time>     written when an attempt is contained
//     closed <csid>          written once the server's close has been reported
// Answered and stale entries are dropped the next time the file is loaded.
static constexpr size_t k_MaxPastWatched = 48;
static constexpr long long k_PastMaxAgeSeconds = 30ll * 24 * 3600;
static SRWLOCK s_QuarantineFileLock = SRWLOCK_INIT;  // appends come from two threads

// Past ids whose close was just reported on the HTTP thread; the render thread
// writes their "closed" lines, so no file IO happens while a response is handled.
static SRWLOCK s_ClosedLock = SRWLOCK_INIT;
static std::vector<std::string> s_ClosedPending;
static std::vector<std::string> s_ClosedReported;  // this run's attempts already queued (at most ebr::k_Slots)
static std::atomic<bool> s_ClosedPendingFlag{ false };

static void AppendQuarantineLine(const std::filesystem::path& p_Folder, const std::string& p_Line)
{
    if (p_Folder.empty())
        return;

    AcquireSRWLockExclusive(&s_QuarantineFileLock);
    {
        std::ofstream s_Out(p_Folder / "quarantine.log", std::ios::app | std::ios::binary);
        if (s_Out)
            s_Out << p_Line << '\n';
    }
    ReleaseSRWLockExclusive(&s_QuarantineFileLock);
}

static bool GuardedCopy(void* p_Dst, const void* p_Src, size_t p_Len)
{
    __try
    {
        memcpy(p_Dst, p_Src, p_Len);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

// Session id of the Freelancer safehouse visit in progress (ContractStart with
// LocationId LOCATION_SNUG). Static, like the ebr kind table, so a disable /
// re-enable of the plugin keeps it. Game thread (OnEventSent) only.
static std::string s_SafehouseCsid;

// The only CpdSets allowed through the guard: the 1-key numeric uploads a
// safehouse display sends when paged. These two keys are the only 1-key CpdSets
// in all logs (the themed weapon wall and one other display).
static bool IsSingleNumberObject(std::string_view p_Value)
{
    if (p_Value.size() < 6 || p_Value.front() != '{' || p_Value.back() != '}' || p_Value[1] != '"')
        return false;
    const size_t s_KeyEnd = p_Value.find('"', 2);
    if (s_KeyEnd == std::string_view::npos || s_KeyEnd + 2 >= p_Value.size() - 1 || p_Value[s_KeyEnd + 1] != ':')
        return false;
    const std::string_view s_Key = p_Value.substr(2, s_KeyEnd - 2);
    if (s_Key != "GearWall_ThematicIndex" && s_Key != "MyValue01")
        return false;
    bool s_Digit = false;
    for (size_t i = s_KeyEnd + 2; i < p_Value.size() - 1; ++i)
    {
        const char c = p_Value[i];
        if (c >= '0' && c <= '9')
            s_Digit = true;
        else if (c != '.' && c != '-')
            return false;
    }
    return s_Digit;
}

// The Value object of an event-layer CpdSet text, up to its first '}' (enough to
// recognise the 1-key numeric form; anything nested fails that test).
static std::string_view CpdSetValueHead(std::string_view p_Text)
{
    constexpr std::string_view k_Key = "\"Value\":{";
    const size_t s_At = p_Text.find(k_Key);
    if (s_At == std::string_view::npos)
        return {};
    const size_t s_Open = s_At + k_Key.size() - 1;
    const size_t s_Close = p_Text.find('}', s_Open);
    if (s_Close == std::string_view::npos)
        return {};
    return p_Text.substr(s_Open, s_Close - s_Open + 1);
}

// The mission-end screen requested for a contained Freelancer attempt, in the two
// exact forms all 101 logged requests use:
//   https://hm3-service.hitman.io/profiles/page/MissionEndReady?contractSessionId=<id>&retryCount=<1-3 digits>
//   https://hm3-service.hitman.io/profiles/page/MissionEnd?contractSessionId=<id>&masteryUnlockableId=
// Static state only (HTTP thread): only while the Freelancer guard is on, and the
// attempt must be quarantined and of kind Evergreen.
static bool IsFreelancerEndScreenUrl(const std::string& p_Url)
{
    if (!s_FreelancerGuard.load())
        return false;

    constexpr std::string_view k_Host = "https://hm3-service.hitman.io/profiles/page/";
    if (p_Url.compare(0, k_Host.size(), k_Host) != 0)
        return false;

    const std::string_view s_Rest = std::string_view(p_Url).substr(k_Host.size());
    constexpr std::string_view k_Ready = "MissionEndReady?contractSessionId=";
    constexpr std::string_view k_End = "MissionEnd?contractSessionId=";
    const bool s_IsReady = s_Rest.substr(0, k_Ready.size()) == k_Ready;
    const bool s_IsEnd = !s_IsReady && s_Rest.substr(0, k_End.size()) == k_End;
    if (!s_IsReady && !s_IsEnd)
        return false;

    const std::string_view s_Query = s_Rest.substr(s_IsReady ? k_Ready.size() : k_End.size());
    const size_t s_Amp = s_Query.find('&');
    if (s_Amp == std::string_view::npos || s_Amp == 0 || s_Amp >= ebr::k_MaxToken)
        return false;
    const std::string_view s_Csid = s_Query.substr(0, s_Amp);
    const std::string_view s_Tail = s_Query.substr(s_Amp);

    if (s_IsReady)
    {
        constexpr std::string_view k_Retry = "&retryCount=";
        if (s_Tail.substr(0, k_Retry.size()) != k_Retry)
            return false;
        const std::string_view s_Digits = s_Tail.substr(k_Retry.size());
        if (s_Digits.empty() || s_Digits.size() > 3)
            return false;
        for (const char c : s_Digits)
            if (c < '0' || c > '9')
                return false;
    }
    else if (s_Tail != "&masteryUnlockableId=")
    {
        return false;
    }

    return ebr::IsQuarantined(s_Csid.data(), s_Csid.size())
        && ebr::KindOf(s_Csid.data(), s_Csid.size()) == ebr::Kind::Evergreen;
}

// The Freelancer mission in progress (game thread only) and whether it has
// already ended well: a successful mission sends MissionCompleted_Event and
// passes an exit (EvergreenExitTriggered, exit_gate) BEFORE its campaign upload;
// the logged pause-menu abandon sent none of them.
static std::string s_MissionCsid;
static bool s_MissionEndedWell = false;

// A Freelancer campaign upload (CpdSet) that reports a failed mission. Game thread.
// - Any "..._MissionFailed":true flag: the showdown sends Hot_MissionFailed; a
//   normal mission presumably sends its own Mild<Type>_MissionFailed.
// - Fail closed: in every log a Freelancer mission sends exactly one CpdSet, at
//   its end. A CpdSet of a mission session that has not reported a completion or
//   an exit, and carries no "..._MissionCompleted":true flag, is treated as a
//   failure. (The flag alone is not enough: CpdSet carries only changed values,
//   so a repeat completion can omit it.) The safehouse session is never a mission.
static bool IsFreelancerFailureCpd(std::string_view p_Text, const char* p_Csid, size_t p_CsidLen)
{
    if (p_Text.find("\"Name\":\"CpdSet\"") == std::string_view::npos)
        return false;
    if (p_Text.find("_MissionFailed\":true") != std::string_view::npos)
        return true;

    if (!p_Csid || ebr::KindOf(p_Csid, p_CsidLen) != ebr::Kind::Evergreen)
        return false;
    const std::string_view s_Csid(p_Csid, p_CsidLen);
    if (!s_SafehouseCsid.empty() && s_Csid == std::string_view(s_SafehouseCsid))
        return false;
    if (s_MissionEndedWell && s_Csid == std::string_view(s_MissionCsid))
        return false;
    return p_Text.find("_MissionCompleted\":true") == std::string_view::npos;
}

// Appends the NAME of every header in a CRLF-separated block. Values are never
// read into the result: they include the Authorization token.
static void AppendHeaderNames(const wchar_t* p_Block, size_t p_Len, std::string& p_Names, bool& p_Suspicious)
{
    size_t s_Line = 0;
    while (s_Line < p_Len)
    {
        size_t s_End = s_Line;
        while (s_End < p_Len && p_Block[s_End] != L'\r' && p_Block[s_End] != L'\n' && p_Block[s_End] != L'\0')
            ++s_End;

        size_t s_Colon = s_Line;
        while (s_Colon < s_End && p_Block[s_Colon] != L':' && p_Block[s_Colon] != L' ')
            ++s_Colon;

        // A header line is "Name: value". The request line ("POST /x HTTP/1.1")
        // hits a space first and is skipped.
        if (s_Colon < s_End && p_Block[s_Colon] == L':' && s_Colon > s_Line && s_Colon - s_Line <= 64)
        {
            std::string s_Name;
            for (size_t i = s_Line; i < s_Colon; ++i)
                s_Name.push_back(static_cast<char>(towlower(p_Block[i]) & 0x7F));

            for (const char* s_Marker : { "digest", "md5", "signature", "hmac", "hash", "content-encoding" })
                if (s_Name.find(s_Marker) != std::string::npos)
                    p_Suspicious = true;

            if (!p_Names.empty())
                p_Names += ",";
            p_Names += s_Name;
        }

        s_Line = s_End + 1;
    }
}

static std::string RequestHeaderNames(HINTERNET hRequest, LPCWSTR pwszHeaders, DWORD dwHeadersLength,
                                      bool& p_Suspicious)
{
    std::string s_Names;

    // Headers the game attached to the request handle earlier.
    std::vector<wchar_t> s_Buffer(8192);
    DWORD s_Size = static_cast<DWORD>(s_Buffer.size() * sizeof(wchar_t));
    if (hRequest && WinHttpQueryHeaders(hRequest, WINHTTP_QUERY_RAW_HEADERS_CRLF | WINHTTP_QUERY_FLAG_REQUEST_HEADERS,
                                        WINHTTP_HEADER_NAME_BY_INDEX, s_Buffer.data(), &s_Size, WINHTTP_NO_HEADER_INDEX))
        AppendHeaderNames(s_Buffer.data(), s_Size / sizeof(wchar_t), s_Names, p_Suspicious);
    else
        s_Names = "(handle headers unavailable)";

    // Headers passed with this send call.
    if (pwszHeaders)
    {
        const size_t s_Len = dwHeadersLength == static_cast<DWORD>(-1) ? wcslen(pwszHeaders) : dwHeadersLength;
        AppendHeaderNames(pwszHeaders, s_Len, s_Names, p_Suspicious);
    }

    return s_Names;
}

void NoMoreAltF4::SeamlessFallback(const char* p_Reason)
{
    Logger::Error("[NoMoreAltF4] SEAMLESS FALLBACK reason={} — terminating instead, as Terminate mode would.",
                  p_Reason);

    if (auto* s_Plugin = s_Instance.load())
        s_Plugin->KillProcess();

    // No live plugin object (the mod was disabled mid-attempt). Flush first, or
    // the reason logged above is lost with the buffered file sink.
    const auto s_Loggers = GetLoggers();
    for (size_t i = 0; i < s_Loggers.Count; ++i)
        s_Loggers.Loggers[i]->flush();

    TerminateProcess(GetCurrentProcess(), 0);
}

LPVOID NoMoreAltF4::SeamlessFilter(HINTERNET hRequest, LPCWSTR pwszHeaders, DWORD dwHeadersLength,
                                   LPVOID lpOptional, DWORD dwOptionalLength, DWORD dwTotalLength)
{
    if (!ebr::HasQuarantine())
        return lpOptional;

    // The Freelancer guard's deferred backstop kill (see ScheduleGuardKill), if the
    // render thread has not done it yet.
    if (GuardKillDue())
        SeamlessFallback("a new Freelancer mission before the campaign was reloaded (closed after its start was uploaded)");

    // --- Requests that name a contained attempt in their URL (no body to rewrite) ---
    std::string s_Url;
    if (hRequest)
    {
        bool s_UrlKnown = false;
        DWORD s_Error = 0;

        wchar_t s_UrlBuf[2048] = {};
        DWORD s_UrlLen = sizeof(s_UrlBuf);
        if (WinHttpQueryOption(hRequest, WINHTTP_OPTION_URL, s_UrlBuf, &s_UrlLen))
        {
            s_Url.assign(s_UrlBuf, s_UrlBuf + wcslen(s_UrlBuf));
            s_UrlKnown = true;
        }
        else if ((s_Error = GetLastError()) == ERROR_INSUFFICIENT_BUFFER && s_UrlLen > 0 && s_UrlLen <= (256u << 10))
        {
            // Longer than the stack buffer: retry once with the size WinHTTP asked
            // for (sized generously, whether it reported bytes or characters).
            std::vector<wchar_t> s_Big(static_cast<size_t>(s_UrlLen) + 2, L'\0');
            DWORD s_BigLen = static_cast<DWORD>((s_Big.size() - 1) * sizeof(wchar_t));
            if (WinHttpQueryOption(hRequest, WINHTTP_OPTION_URL, s_Big.data(), &s_BigLen))
            {
                s_Url.assign(s_Big.data(), s_Big.data() + wcslen(s_Big.data()));
                s_UrlKnown = true;
            }
            else
            {
                s_Error = GetLastError();
            }
        }

        // An unreadable URL cannot be checked. With a real penalty at risk that
        // is doubt, so terminate; for test-only attempts just report it.
        if (!s_UrlKnown)
        {
            static std::atomic<uint32_t> s_Reported{ 0 };
            if (s_Reported.fetch_add(1) < 5)
                Logger::Warn("[NoMoreAltF4] SEAMLESS URL unreadable error={} — this request's URL was not checked.",
                             s_Error);
            if (ebr::HasStakedQuarantine())
                SeamlessFallback("a request URL could not be read");
        }
    }

    // Freelancer guard release: the game went back to the Hub (main menu) after
    // the containment, the route verified to reload the campaign.
    if (s_FreelancerGuard.load()
        && (s_Url == "https://hm3-service.hitman.io/profiles/page/Hub"
            || s_Url.rfind("https://hm3-service.hitman.io/profiles/page/Hub?", 0) == 0)
        && !s_FreelancerHubSeen.exchange(true))
        Logger::Info("[NoMoreAltF4] SEAMLESS FREELANCER: back at the Hub after the contained failure.");

    if (const int s_Hit = ebr::MentionsQuarantined(s_Url.data(), s_Url.size()))
    {
        // Freelancer test: the mission-end screen is always requested by the
        // session id, after a death as after a success. These are page reads; the
        // campaign itself changes only through CpdSet uploads, which the Freelancer
        // guard keeps from the server. Let them through to see what follows.
        if (IsFreelancerEndScreenUrl(s_Url))
        {
            // The game polls this page up to ~46 times; log the first few and every 10th.
            static std::atomic<uint32_t> s_EndScreenSeen{ 0 };
            const uint32_t s_Seen = s_EndScreenSeen.fetch_add(1);
            if (s_Seen < 3 || s_Seen % 10 == 0)
                Logger::Warn("[NoMoreAltF4] SEAMLESS END-SCREEN allowed (Freelancer test, #{}): {}", s_Seen + 1, s_Url);
        }
        else
        {
            Logger::Warn("[NoMoreAltF4] SEAMLESS URL-HIT stakes={} url={}", s_Hit == 2 ? "staked" : "test", s_Url);
            if (s_Hit == 2)
                SeamlessFallback("a request URL names a contained attempt");
        }
    }

    // Freelancer guard: loading any contract other than the safehouse before the
    // campaign was reloaded closes the game before the request goes out, so the
    // server never creates that session. (The old kill at the mission's
    // ContractStart let the mission's first event out without its ContractStart.
    // That was seen once, on 2026-11-13; from the next run on the server closed no
    // session as completed and the end screen had no playstyle. That point is now
    // only a deferred backstop, see ScheduleGuardKill.)
    // A story mission or ET loaded in this window also closes the game; nothing
    // of it reaches the server.
    const bool s_GuardedContractLoad = s_FreelancerGuard.load() && !FreelancerReleaseDue()
        && (s_Url == "https://hm3-service.hitman.io/authentication/api/userchannel/ContractsService/GetForPlay2"
            || s_Url.rfind("https://hm3-service.hitman.io/authentication/api/userchannel/ContractsService/GetForPlay2?", 0) == 0);

    if (!lpOptional || dwOptionalLength == 0)
    {
        if (s_GuardedContractLoad)
        {
            Logger::Warn("[NoMoreAltF4] SEAMLESS FREELANCER GUARD (contract load) without a body: not sent.");
            SeamlessFallback("a contract load after a contained Freelancer failure, before the campaign was reloaded");
        }
        return lpOptional;
    }

    // --- Work on a private copy; game memory is never written ---
    char* s_Copy = static_cast<char*>(HeapAlloc(GetProcessHeap(), 0, dwOptionalLength));
    if (!s_Copy || !GuardedCopy(s_Copy, lpOptional, dwOptionalLength))
    {
        if (s_Copy)
            HeapFree(GetProcessHeap(), 0, s_Copy);
        SeamlessFallback("the request body could not be copied");
        return lpOptional;
    }

    NoteGuardKillBody(s_Copy, dwOptionalLength);

    if (s_GuardedContractLoad)
    {
        // Observed body: {"id":"<contract id>","locationId":"",...}. Only the safehouse passes.
        static constexpr std::string_view k_SafehouseLoad = "{\"id\":\"f8ec92c2-4fa2-471e-ae08-545480c746ee\"";
        const std::string_view s_Body(s_Copy, dwOptionalLength);
        const bool s_IsSafehouse = s_Body.size() > k_SafehouseLoad.size()
            && s_Body.substr(0, k_SafehouseLoad.size()) == k_SafehouseLoad
            && (s_Body[k_SafehouseLoad.size()] == ',' || s_Body[k_SafehouseLoad.size()] == '}');
        if (!s_IsSafehouse)
        {
            Logger::Warn("[NoMoreAltF4] SEAMLESS FREELANCER GUARD (contract load): {} before the campaign was "
                         "reloaded, not sent.", s_Body.substr(0, 50));
            HeapFree(GetProcessHeap(), 0, s_Copy);
            SeamlessFallback("a contract load after a contained Freelancer failure, before the campaign was reloaded");
            return lpOptional;
        }
    }

    // Freelancer guard at the wire: the backstop for the event-layer guard in
    // OnEventSent, on static state only (so it also holds while the plugin is
    // disabled). Any campaign upload that the rewrite below would not neutralize
    // and that is not a display page flip closes the game before it is sent.
    if (s_FreelancerGuard.load() && ebr::HasUnsafeCpdSet(s_Copy, dwOptionalLength))
    {
        HeapFree(GetProcessHeap(), 0, s_Copy);
        Logger::Warn("[NoMoreAltF4] SEAMLESS FREELANCER GUARD (request) url={}: campaign upload after a contained "
                     "failure, not sent.", s_Url);
        SeamlessFallback("a campaign upload after a contained Freelancer failure (request)");
        return lpOptional;
    }

    const int s_Mention = ebr::MentionsQuarantined(s_Copy, dwOptionalLength);

    if (!ebr::LooksLikeEventsBody(s_Copy, dwOptionalLength))
    {
        HeapFree(GetProcessHeap(), 0, s_Copy);
        if (s_Mention)
        {
            // Telemetry is not rewritten: log only. In the session logs a "Died"
            // record was already being sent before the kill in 10 of 11 death
            // kills (4 of them Arcade, none an Elusive Target) and every one of
            // those attempts was later closed as an orphaned session. In a death
            // that is not killed the record follows the failure event instead.
            const bool s_Metrics = s_Url.find("hm3-metrics") != std::string::npos;
            Logger::Warn("[NoMoreAltF4] SEAMLESS BODY-HIT stakes={} metrics={} url={}",
                         s_Mention == 2 ? "staked" : "test", s_Metrics, s_Url);
            if (s_Mention == 2 && !s_Metrics)
                SeamlessFallback("a non-event request names a contained attempt");
        }
        return lpOptional;
    }

    ebr::Report s_Report;
    const ebr::Result s_Result = ebr::Neutralize(s_Copy, dwOptionalLength, s_Report);

    if (s_Result != ebr::Result::Rewritten)
    {
        HeapFree(GetProcessHeap(), 0, s_Copy);
        if (s_Result == ebr::Result::Error && s_Mention)
            SeamlessFallback(s_Report.error);
        return lpOptional;
    }

    // --- The copy will be sent: last checks ---
    const char* s_Problem = nullptr;
    if (dwTotalLength != dwOptionalLength)
        s_Problem = "the request body is sent in several parts";

    bool s_Suspicious = false;
    const std::string s_HeaderNames = RequestHeaderNames(hRequest, pwszHeaders, dwHeadersLength, s_Suspicious);
    Logger::Info("[NoMoreAltF4] SEAMLESS HEADERS names=[{}]", s_HeaderNames);
    if (!s_Problem && s_Suspicious)
        s_Problem = "the request carries a body digest or encoding header";

    if (!s_Problem && s_SeamlessCopyBytes.fetch_add(dwOptionalLength) + dwOptionalLength > k_SeamlessCopyBudget)
        s_Problem = "the copy budget is exhausted";

    if (s_Problem)
    {
        HeapFree(GetProcessHeap(), 0, s_Copy);
        SeamlessFallback(s_Problem);
        return lpOptional;
    }

    Logger::Warn("[NoMoreAltF4] SEAMLESS REWRITE url={} len={} events={} neutralized={} names=[{}] ids=[{}] ladder={}",
                 s_Url, dwOptionalLength, s_Report.events, s_Report.neutralized, s_Report.names, s_Report.ids,
                 s_Report.ladder);

    return s_Copy;
}

bool NoMoreAltF4::AnySeamlessEnabled() const
{
    return m_Seamless || m_SeamlessFreelancer;
}

// Decides how a failure of this attempt is handled. The contract type comes
// from the attempt's own ContractStart event, not from the live mode flags.
NoMoreAltF4::SeamlessMode NoMoreAltF4::SeamlessFor(const char* p_Csid, size_t p_Len) const
{
    if (!m_AutoKillEnabled)
        return SeamlessMode::Off;

    const ebr::Kind s_Kind = ebr::KindOf(p_Csid, p_Len);

    // Freelancer is protected whatever "Only Enable for Freelancer" says, and its
    // live mode flag resets while a mission unloads, so it is not consulted.
    if (s_Kind != ebr::Kind::Evergreen && !ShouldProtect())
        return SeamlessMode::Off;

    switch (s_Kind)
    {
    case ebr::Kind::Arcade:
        return m_Seamless ? SeamlessMode::Staked : SeamlessMode::Off;
    case ebr::Kind::Elusive:
        return m_Seamless ? SeamlessMode::Staked : SeamlessMode::Off;
    case ebr::Kind::Other:
        // An ordinary contract (nothing at stake), and never when the live flags
        // say this could still be Freelancer, an ET or Arcade.
        if (IsFreelancerMode() || m_ElusiveOrArcadeDetected)
            return SeamlessMode::Off;
        return m_Seamless ? SeamlessMode::Test : SeamlessMode::Off;
    case ebr::Kind::Evergreen:
        // Experimental. A Freelancer mission is contained like an Arcade level;
        // the guard in OnEventSent then closes the game before any later
        // campaign upload, so the campaign itself can never be changed. The
        // safehouse session has nothing to contain.
        if (!m_SeamlessFreelancer)
            return SeamlessMode::Off;
        if (!s_SafehouseCsid.empty() && std::string_view(p_Csid, p_Len) == std::string_view(s_SafehouseCsid))
            return SeamlessMode::Off;
        return SeamlessMode::Staked;
    case ebr::Kind::Unknown:    // its start was never seen (mod loaded mid-mission, table full)
    default:
        return SeamlessMode::Off;
    }
}

void NoMoreAltF4::OnSeamlessArmed(const char* p_Csid, size_t p_Len, bool p_ExitToMenu)
{
    m_SeamlessNoticeExit = p_ExitToMenu;
    m_SeamlessNoticeUntilMs = GetTickCount64() + 12000;

    // Remember the attempt across launches so the next login can report how
    // the server closed it.
    std::string s_Line(p_Csid, p_Len);
    s_Line += ' ';
    s_Line += std::to_string(static_cast<long long>(std::time(nullptr)));
    AppendQuarantineLine(m_LogMirror.Folder(), s_Line);
}

void NoMoreAltF4::LoadPastQuarantine()
{
    const auto s_Folder = m_LogMirror.Folder();
    if (s_Folder.empty())
        return;

    const auto s_Path = s_Folder / "quarantine.log";

    struct Entry
    {
        std::string csid;
        long long when = 0;  // 0 = unknown
    };

    std::vector<Entry> s_Entries;
    std::vector<std::string> s_Closed;
    size_t s_LineCount = 0;
    {
        std::ifstream s_In(s_Path, std::ios::binary);
        if (!s_In)
            return;

        for (std::string s_Line; std::getline(s_In, s_Line);)
        {
            if (!s_Line.empty() && s_Line.back() == '\r')
                s_Line.pop_back();
            if (s_Line.empty())
                continue;
            ++s_LineCount;

            const size_t s_Space = s_Line.find(' ');
            const std::string s_Head = s_Line.substr(0, s_Space);
            const std::string s_Rest = s_Space == std::string::npos ? std::string() : s_Line.substr(s_Space + 1);

            if (s_Head == "closed")
                s_Closed.push_back(s_Rest);
            else
                s_Entries.push_back({ s_Head, std::strtoll(s_Rest.c_str(), nullptr, 10) });
        }
    }

    // Keep the attempts the server has not answered for yet.
    const long long s_Now = static_cast<long long>(std::time(nullptr));
    std::vector<Entry> s_Keep;
    for (auto& s_Entry : s_Entries)
    {
        if (std::find(s_Closed.begin(), s_Closed.end(), s_Entry.csid) != s_Closed.end())
            continue;  // its close was reported in an earlier run

        if (s_Entry.when > 0 && s_Now - s_Entry.when > k_PastMaxAgeSeconds)
        {
            Logger::Info("[NoMoreAltF4] SEAMLESS-CHECK csid={} dropped: no server close seen within 30 days.",
                         s_Entry.csid);
            continue;
        }

        const bool s_Known = std::any_of(s_Keep.begin(), s_Keep.end(),
            [&](const Entry& p_Other) { return p_Other.csid == s_Entry.csid; });
        if (!s_Known)
            s_Keep.push_back(std::move(s_Entry));
    }

    if (s_Keep.size() > k_MaxPastWatched)
        s_Keep.erase(s_Keep.begin(), s_Keep.end() - k_MaxPastWatched);

    for (const auto& s_Entry : s_Keep)
        ebr::AddPast(s_Entry.csid.data(), s_Entry.csid.size());

    // Compact the file whenever something was dropped, so it cannot grow and an
    // answered attempt stops being watched in later runs.
    if (s_Keep.size() != s_LineCount)
    {
        AcquireSRWLockExclusive(&s_QuarantineFileLock);
        std::error_code s_Ec;
        if (s_Keep.empty())
        {
            std::filesystem::remove(s_Path, s_Ec);
        }
        else
        {
            const auto s_Tmp = s_Folder / "quarantine.tmp";
            bool s_Written = false;
            {
                std::ofstream s_Out(s_Tmp, std::ios::trunc | std::ios::binary);
                for (const auto& s_Entry : s_Keep)
                    s_Out << s_Entry.csid << ' ' << s_Entry.when << '\n';
                s_Out.flush();
                s_Written = s_Out.good();
            }

            if (s_Written)
                std::filesystem::rename(s_Tmp, s_Path, s_Ec);
            else
                std::filesystem::remove(s_Tmp, s_Ec);
        }
        ReleaseSRWLockExclusive(&s_QuarantineFileLock);
    }

    if (!s_Keep.empty())
        Logger::Info("[NoMoreAltF4] SEAMLESS-CHECK watching {} attempt(s) contained in earlier runs.",
                     s_Keep.size());
}

// Looks for the server's own close of a contained attempt in a decoded response.
void NoMoreAltF4::CheckServerCloses(const char* p_Text, size_t p_Len)
{
    // Room for every watched past attempt plus this run's contained ones; the
    // two records the server sends for one close are merged.
    static constexpr size_t k_MaxCloses = 64;
    ebr::ServerClose s_Closes[k_MaxCloses];
    const size_t s_Count = ebr::FindServerCloses(p_Text, p_Len, s_Closes, k_MaxCloses);

    if (s_Count == k_MaxCloses)
        Logger::Warn("[NoMoreAltF4] SEAMLESS-CHECK incomplete: this response closes more contained attempts "
                     "than can be checked at once ({}).", k_MaxCloses);

    for (size_t i = 0; i < s_Count; ++i)
    {
        const auto& s_Close = s_Closes[i];
        const char* s_When = s_Close.past ? "at login, attempt from an earlier run" : "in this run";

        // The verdict is now on record, so later launches need not watch for
        // this attempt: queue its "closed" line, once per attempt. It stays
        // watched (and, if contained in this run, quarantined) until the game
        // exits, because the second record of the pair can still arrive.
        bool s_Queue = s_Close.past && ebr::RetirePast(s_Close.csid, strlen(s_Close.csid));

        AcquireSRWLockExclusive(&s_ClosedLock);
        if (!s_Close.past
            && std::find(s_ClosedReported.begin(), s_ClosedReported.end(), s_Close.csid) == s_ClosedReported.end())
        {
            s_ClosedReported.emplace_back(s_Close.csid);
            s_Queue = true;
        }
        if (s_Queue)
            s_ClosedPending.emplace_back(s_Close.csid);
        ReleaseSRWLockExclusive(&s_ClosedLock);

        if (s_Queue)
            s_ClosedPendingFlag = true;

        if (s_Close.orphaned)
        {
            Logger::Info("[NoMoreAltF4] SEAMLESS-CHECK csid={} CloseType={} verdict=OK ({}) — "
                         "closed as an orphaned session, the same outcome as an Alt-F4.",
                         s_Close.csid, s_Close.closeType, s_When);
            if (!s_Close.past)
                NoteFreelancerClosed(s_Close.csid, true);
            continue;
        }

        if (!s_Close.past)
            NoteFreelancerClosed(s_Close.csid, false);

        Logger::Error("[NoMoreAltF4] SEAMLESS-CHECK csid={} CloseType={} verdict=ALARM ({}) — "
                      "the server closed a contained attempt in an unexpected way. Seamless is now OFF.",
                      s_Close.csid, s_Close.closeType[0] ? s_Close.closeType : "(none)", s_When);

        m_Seamless = false;
        m_SeamlessFreelancer = false;
        m_SeamlessAlarm = true;
        m_SeamlessPersistPending = true;  // settings are written from the render thread
        m_SeamlessNoticeUntilMs = GetTickCount64() + 60000;
    }
}

void NoMoreAltF4::DrawSeamlessNotice()
{
    if (m_SeamlessPersistPending.exchange(false))
    {
        SetSettingBool(S_SETTINGS_SECTION, "seamless", m_Seamless);
        SetSettingBool(S_SETTINGS_SECTION, "seamlessfreelancer", m_SeamlessFreelancer);
    }

    if (s_ClosedPendingFlag.exchange(false))
    {
        std::vector<std::string> s_Closed;
        AcquireSRWLockExclusive(&s_ClosedLock);
        s_Closed.swap(s_ClosedPending);
        ReleaseSRWLockExclusive(&s_ClosedLock);

        for (const auto& s_Csid : s_Closed)
            AppendQuarantineLine(m_LogMirror.Folder(), "closed " + s_Csid);
    }

    if (GetTickCount64() >= m_SeamlessNoticeUntilMs.load())
        return;

    ImGui::SetNextWindowPos(ImVec2(ImGui::GetIO().DisplaySize.x * 0.5f, 48.0f), ImGuiCond_Always, ImVec2(0.5f, 0.0f));
    ImGui::SetNextWindowBgAlpha(0.8f);

    const ImGuiWindowFlags s_Flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize
        | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav
        | ImGuiWindowFlags_NoInputs;

    if (ImGui::Begin("##NoMoreAltF4SeamlessNotice", nullptr, s_Flags))
    {
        if (m_SeamlessAlarm)
            ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.3f, 1.0f),
                ICON_MD_WARNING " NoMoreAltF4: the server closed a contained attempt unexpectedly.\n"
                "Seamless has been switched off. See the session log.");
        else if (m_SeamlessNoticeFreelancer)
        {
            ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.5f, 1.0f),
                ICON_MD_SHIELD " NoMoreAltF4: Freelancer failure hidden from the server (test).");
            ImGui::TextUnformatted("If the end screen keeps loading, wait for the error dialog and choose\n"
                                   "'Go to menu', then enter Freelancer again: the safehouse reloads your campaign.");
        }
        else if (m_SeamlessNoticeExit)
            // A pause-menu exit counts as a failure while "Allow Exit to Main
            // Menu" is off, but the player chose it: do not call it a failure.
            ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.5f, 1.0f),
                ICON_MD_SHIELD " NoMoreAltF4: exit to menu contained. The server was not told that you left.");
        else
            ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.5f, 1.0f),
                ICON_MD_SHIELD " NoMoreAltF4: failure contained. Exit to the menu and start again.");
    }
    ImGui::End();
}

// The replacement function: inspect, log, and optionally block POST requests.
// The body in lpOptional is plain UTF-8 JSON — IOI does not encrypt it.
//
// Role of this hook (learned from packet captures + crash/hang logs):
//   - It is OBSERVE-ONLY for the request itself — it NEVER fakes the return value
//     (see the note at the top of this file: faking TRUE hangs the game, faking
//     FALSE crashes it).  It always either calls the original or TerminateProcess.
//   - On death the game sends MissionFailed_Event / MissionWounded_Event /
//     MildChess_MissionFailed; ET/Arcade/Freelancer failures send ContractFailed,
//     all via SaveEvents2 POST bodies (ContractFailed also fires for manual
//     restart/replan/load in normal contracts — filtered out below).
//   - When a failure is seen here and auto-kill is on, we TerminateProcess before
//     the original send, so the failure never reaches IOI.  In block-only mode the
//     actual suppression is done earlier, at the event layer (OnEventSent).
BOOL WINAPI NoMoreAltF4::HookedSendRequest(
    HINTERNET hRequest, LPCWSTR pwszHeaders, DWORD dwHeadersLength,
    LPVOID lpOptional, DWORD dwOptionalLength,
    DWORD dwTotalLength, DWORD_PTR dwContext)
{
    // Counted before touching any plugin state so the destructor can drain us.
    struct InFlightGuard
    {
        InFlightGuard() { ++s_InFlight; }
        ~InFlightGuard() { --s_InFlight; }
    } s_InFlightGuard;

    // Seamless: events of a contained attempt are rewritten before anything else
    // looks at the body. From here on lpOptional is what will really be sent, so
    // the logging and the Terminate logic below only ever see the rewritten bytes.
    lpOptional = SeamlessFilter(hRequest, pwszHeaders, dwHeadersLength,
                                lpOptional, dwOptionalLength, dwTotalLength);


    // During shutdown, skip all logic and call the original directly.
    // This prevents accessing freed plugin memory while in-flight calls drain.
    if (s_HookPassthrough)
        return s_OriginalSendRequest(hRequest, pwszHeaders, dwHeadersLength,
            lpOptional, dwOptionalLength, dwTotalLength, dwContext);

    // Not Plugin(): after unload that would construct a brand-new instance on this thread.
    auto* s_Plugin = s_Instance.load();

    // Session-start window, for the abort key (see SessionStartSettled).
    NoteSessionStartTraffic(hRequest, static_cast<const char*>(lpOptional), lpOptional ? dwOptionalLength : 0);

    // Build body string once for both logging and blocking checks.
    std::string s_Body;
    if (s_Plugin && lpOptional && dwOptionalLength > 0)
        s_Body.assign(reinterpret_cast<const char*>(lpOptional), dwOptionalLength);

    // --- Diagnostic logging ---
    if (s_Plugin && s_Plugin->m_LogHttpRequests && !s_Body.empty())
    {
        std::string s_LogBody = s_Body;
        if (s_LogBody.size() > k_MaxLogBytes)
        {
            s_LogBody.resize(k_MaxLogBytes);
            s_LogBody += "...";
        }
        Logger::Info("[NoMoreAltF4] HTTP POST body: {}", s_LogBody);
    }

    // --- Freelancer mode detection ---
    // Freelancer events contain "Evergreen" in event names (e.g. Evergreen_MissionPayout,
    // EvergreenMissionStarted) and ContractType "evergreen". The scene path does NOT
    // contain "Evergreen" — actual missions use regular map paths.
    // IMPORTANT: Match "Evergreen_" (with underscore) or "EvergreenMission" specifically,
    // NOT just "Evergreen" — because Actor_Kill events in ALL modes contain
    // "EvergreenRarity" which would cause false Freelancer detection.
    if (s_Plugin && !s_Plugin->m_FreelancerDetected && !s_Body.empty())
    {
        // "Evergreen_"-prefixed events are Freelancer-specific EXCEPT
        // Evergreen_SecurityCameraDestroyed, which the game emits in EVERY
        // mode (observed live on an Elusive Target, game 3.280) — skip it.
        static constexpr char k_CrossModeEvent[] = "\"Evergreen_SecurityCameraDestroyed";
        bool s_HasEvergreenEvent = false;
        for (size_t s_Pos = s_Body.find("\"Evergreen_"); s_Pos != std::string::npos;
             s_Pos = s_Body.find("\"Evergreen_", s_Pos + 1))
        {
            if (s_Body.compare(s_Pos, sizeof(k_CrossModeEvent) - 1, k_CrossModeEvent) != 0)
            {
                s_HasEvergreenEvent = true;
                break;
            }
        }

        if (s_HasEvergreenEvent
            || s_Body.find("\"EvergreenMission") != std::string::npos
            || s_Body.find("\"ContractType\":\"evergreen\"") != std::string::npos)
        {
            Logger::Info("[NoMoreAltF4] Freelancer mode detected via Evergreen event in HTTP traffic.");
            s_Plugin->m_FreelancerDetected = true;
        }
    }

    // --- Elusive Target / Arcade detection ---
    // ETs and Elusive Target Arcade are single-attempt modes (failing has a
    // permanent/locking consequence), so a ContractFailed there is always a real
    // failure we want to suppress — unlike normal contracts where ContractFailed
    // also fires for legitimate restart/replan/load.  The contract metadata carries
    // "ContractType":"arcade" (Arcade) or "elusive" (live ETs).
    if (s_Plugin && !s_Plugin->m_ElusiveOrArcadeDetected && !s_Body.empty()
        && (s_Body.find("\"ContractType\":\"arcade\"") != std::string::npos
            || s_Body.find("\"ContractType\":\"elusive\"") != std::string::npos))
    {
        Logger::Info("[NoMoreAltF4] Elusive Target / Arcade mode detected via ContractType in HTTP traffic.");
        s_Plugin->m_ElusiveOrArcadeDetected = true;
    }

    // --- Death detection + network blocking ---
    // Detect failure events in outgoing SaveEvents2 POST bodies.
    // The game sends different events depending on the type of failure:
    //   - ContractFailed with Value {"FailType":"HitmanDead"|"PrimaryObjective",..}
    //     — a death or an objective failure on game 3.290 (every mode logged)
    //   - ContractFailed with Value "Contract ended manually: ..." — manual
    //     actions (exit-to-menu, restart, replan, load)
    //   - MissionWounded_Event — a Freelancer death (still so on 3.290, where it
    //     is what caught a real death); MissionFailed_Event — older builds
    //   - MildChess_MissionFailed — Freelancer-specific failure event
    //
    // IMPORTANT: Match "Name":"EventName" (the JSON key-value pair), NOT just
    // the bare event name.  CpdSet events include field names like
    // "MildChess_MissionFailed" inside their Value object — matching the bare
    // string causes false positives on normal mission completion.
    if (s_Plugin && s_Plugin->ShouldProtect() && hRequest && !s_Body.empty())
    {
        // Check for actual death/failure events (these ONLY fire on real death)
        // Must match as event Name, not as a field name inside CpdSet values.
        bool s_HasDeathEvent =
            s_Body.find("\"Name\":\"MissionFailed_Event\"") != std::string::npos
            || s_Body.find("\"Name\":\"MissionWounded_Event\"") != std::string::npos
            || s_Body.find("\"Name\":\"MildChess_MissionFailed\"") != std::string::npos;

        // Check for ContractFailed (manual exit, restart, replan, etc.)
        bool s_HasContractFailed =
            s_Body.find("\"Name\":\"ContractFailed\"") != std::string::npos;

        if (s_HasDeathEvent || s_HasContractFailed)
        {
            // ContractFailed without death events = player-initiated action.
            // Check if it should be allowed through.
            if (!s_HasDeathEvent && s_HasContractFailed)
            {
                if (s_Body.find("OnRestartLevel") != std::string::npos
                    || s_Body.find("OnReplanLevel") != std::string::npos
                    || s_Body.find("OnLoadGame") != std::string::npos
                    || (s_Plugin->m_AllowExitToMenu && s_Body.find("exit to Main menu") != std::string::npos))
                {
                    Logger::Info("[NoMoreAltF4] ContractFailed is a manual action (restart/replan/load/exit) — allowing.");
                    return s_OriginalSendRequest(hRequest, pwszHeaders, dwHeadersLength,
                        lpOptional, dwOptionalLength, dwTotalLength, dwContext);
                }
            }

            Logger::Warn("[NoMoreAltF4] Failure detected in HTTP body! Death events: {}, ContractFailed: {}",
                s_HasDeathEvent, s_HasContractFailed);
            s_Plugin->m_DeathDetected = true;

            // Auto-kill is the only safe action: TerminateProcess runs BEFORE the
            // original send, so the failure never reaches IOI and there is no
            // WinHTTP state left to corrupt. This is a fallback to the earlier
            // OnEventSent detection. We never fake this request's return value
            // (faking TRUE hangs the game, FALSE crashes it — see the file header).
            if (s_Plugin->m_AutoKillEnabled)
            {
                Logger::Warn("[NoMoreAltF4] TERMINATED — auto-kill from HTTP hook.");
                s_Plugin->KillProcess();
                return TRUE; // unreachable after TerminateProcess, but keeps compiler happy
            }

            Logger::Warn("[NoMoreAltF4] Failure seen at HTTP layer but 'Terminate on Death/Failure' is OFF — "
                         "the failure WILL be saved (it cannot be blocked safely). Enable auto-terminate to protect.");
        }
    }

    return s_OriginalSendRequest(
        hRequest, pwszHeaders, dwHeadersLength,
        lpOptional, dwOptionalLength, dwTotalLength, dwContext);
}

// =============================================================================
// Network Interception
// =============================================================================

// -----------------------------------------------------------------------------
// ZAchievementManagerSimple_OnEventReceived
//
// Fires when the game receives an event FROM THE SERVER (e.g. SegmentClosing,
// ContractFailed).  When network kill is active, we block all server events
// to prevent the game from processing the failure confirmation.
// -----------------------------------------------------------------------------
DEFINE_PLUGIN_DETOUR(NoMoreAltF4, void, ZAchievementManagerSimple_OnEventReceived,
    ZAchievementManagerSimple* th, const SOnlineEvent& event)
{
    const char* s_Name = event.sName.c_str();

    if (m_LogHttpRequests)
        Logger::Info("[NoMoreAltF4] Event received (server→client): {}", s_Name);

    // NOTE: Server-side ContractFailed is just a confirmation of what we already
    // catch on the outgoing side (HTTP body / OnEventSent).  We do NOT trigger
    // protection from it because SaveAndSynchronizeEvents4 replays the previous
    // session's ContractFailed at startup, which would crash the game.

    // When a network kill/block is active, drop the server's failure-confirmation
    // events.  We deliberately only block the KNOWN failure/closing events rather
    // than every server event: swallowing unrelated events (matchmaking, presence,
    // config, etc.) can stall the game's own state machine, and the actual failure
    // is already prevented on the outgoing side.  Letting unrelated events through
    // is what keeps the game responsive instead of frozen.
    const bool s_IsFailureEvent =
        strcmp(s_Name, "SegmentClosing") == 0
        || strcmp(s_Name, "ContractFailed") == 0
        || strcmp(s_Name, "MissionFailed_Event") == 0
        || strcmp(s_Name, "MissionWounded_Event") == 0
        || strcmp(s_Name, "MildChess_MissionFailed") == 0;

    if (m_NetworkBlocked && s_IsFailureEvent)
    {
        Logger::Warn("[NoMoreAltF4] Blocking server failure event '{}' (network kill active).", s_Name);
        return HookAction::Return();
    }

    // Block SegmentClosing even if network kill hasn't been set yet —
    // this is the server's confirmation of mission failure.
    if (m_DeathDetected && strcmp(s_Name, "SegmentClosing") == 0)
    {
        Logger::Warn("[NoMoreAltF4] Blocking SegmentClosing (death detected, protecting state).");
        return HookAction::Return();
    }

    return HookAction::Continue();
}

// -----------------------------------------------------------------------------
// ZAchievementManagerSimple_OnEventSent
//
// Fires when the game dispatches an online event (kills, mission outcomes,
// Elusive Target results, etc.) BEFORE it is batched into an HTTP request.  This
// is the SDK-sanctioned, crash-safe place to suppress an event: returning
// HookAction::Return() simply stops the game from dispatching it, with none of
// the WinHTTP-corruption problems of short-circuiting WinHttpSendRequest.
//
// This is the PRIMARY protection point for block-only mode and an early,
// reliable trigger for auto-kill.
// -----------------------------------------------------------------------------
DEFINE_PLUGIN_DETOUR(NoMoreAltF4, void, ZAchievementManagerSimple_OnEventSent,
    ZAchievementManagerSimple* th, uint32_t eventIndex, const ZDynamicObject& event)
{
    // Protection already engaged — drop every outgoing online event so nothing
    // about the failed session reaches IOI. Bounded by the release valve.
    if (m_NetworkBlocked)
    {
        if (m_LogHttpRequests)
            Logger::Warn("[NoMoreAltF4] Dropping outgoing online event #{} (protection active).", eventIndex);
        return HookAction::Return();
    }

    // Serialize the event with the game's OWN serializer (the same text the game
    // would POST). Used both for the diagnostic log below and for failure matching.
    // ZDynamicObject's accessors aren't exported by the SDK, but
    // ZDynamicObject_ToString is resolved at runtime; if it's unavailable we skip
    // rather than crash. The HTTP-body hook is a fallback for death events and
    // ContractFailed only, not for the Freelancer exit check below (a warning is
    // logged once in that case).
    // Also whenever termination is on: the Freelancer exit check below must not
    // depend on the live mode flags, which reset while a mission unloads.
    std::string s_EventStr;
    if ((m_LogHttpRequests || ShouldProtect() || m_AutoKillEnabled || s_FreelancerGuard.load() || ebr::HasQuarantine())
        && Functions::ZDynamicObject_ToString && Functions::ZDynamicObject_ToString->Exists())
    {
        ZString s_Json;
        Functions::ZDynamicObject_ToString->Call(const_cast<ZDynamicObject*>(&event), s_Json);
        s_EventStr.assign(s_Json.c_str(), s_Json.size());
    }

    if (m_LogHttpRequests)
    {
        if (s_EventStr.empty())
            Logger::Info("[NoMoreAltF4] Online event sent: index={} (payload unavailable)", eventIndex);
        else
        {
            std::string s_LogEvent = s_EventStr;
            if (s_LogEvent.size() > k_MaxLogBytes)
            {
                s_LogEvent.resize(k_MaxLogBytes);
                s_LogEvent += "...";
            }
            Logger::Info("[NoMoreAltF4] Online event sent: index={} payload={}", eventIndex, s_LogEvent);
        }
    }

    // The Freelancer guard and watch below need the event text; without it, fail closed.
    if ((s_FreelancerGuard.load() || s_FreelancerWatch) && s_EventStr.empty())
    {
        Logger::Error("[NoMoreAltF4] SEAMLESS FREELANCER GUARD index={}: event text unavailable.", eventIndex);
        SeamlessFallback("event text unavailable while the Freelancer guard is on");
        return HookAction::Return();
    }

    // --- Seamless (experimental) ---
    // This hook sees an event before it is serialized into a request, so the
    // attempt is quarantined here and the actual rewrite happens in
    // SeamlessFilter when the body goes out. The text below is only searched,
    // never measured: ToString output is not what goes on the wire.
    const char* s_Csid = nullptr;
    size_t s_CsidLen = 0;
    const bool s_HasCsid = !s_EventStr.empty()
        && ebr::ExtractCsid(s_EventStr.data(), s_EventStr.size(), &s_Csid, &s_CsidLen);
    const bool s_SeamlessActive = AnySeamlessEnabled() || ebr::HasQuarantine();

    // The contract type of every attempt is recorded whatever the settings are,
    // so a toggle switched on mid-mission applies to the attempt in progress. A
    // session resumed from a save opens with ContractLoad, not ContractStart.
    if (s_HasCsid && (ebr::NameIs(s_EventStr.data(), s_EventStr.size(), "ContractStart")
                      || ebr::NameIs(s_EventStr.data(), s_EventStr.size(), "ContractLoad")))
    {
        const ebr::Kind s_Kind = ebr::KindFromContractStart(s_EventStr.data(), s_EventStr.size());
        ebr::NoteContractStart(s_Csid, s_CsidLen, s_Kind);

        // The Freelancer safehouse is its own contract session.
        const bool s_IsSafehouse = s_EventStr.find("\"LocationId\":\"LOCATION_SNUG\"") != std::string::npos;
        if (s_IsSafehouse)
            s_SafehouseCsid.assign(s_Csid, s_CsidLen);
        else if (s_Kind == ebr::Kind::Evergreen)
        {
            s_MissionCsid.assign(s_Csid, s_CsidLen);
            s_MissionEndedWell = false;
        }

        // Freelancer guard: a new Freelancer mission after a contained failure
        // ends the run before any new progress (leaving the safehouse often sends
        // no campaign upload the guard below could catch). Backstop only: the
        // contract-load check in SeamlessFilter closes the game before the mission
        // is even requested. The mission's first event has already gone out here,
        // so this start is let through and the game closes a few seconds after it
        // was sent (ScheduleGuardKill), never before.
        // A safehouse start after a containment and a return to the Hub: the game
        // loads the server's campaign for it. Once the server has also closed the
        // contained attempt, the guard is no longer needed.
        if (s_IsSafehouse && s_FreelancerGuard.load() && s_FreelancerHubSeen.load())
            s_FreelancerSafehouseSeen = true;

        // The watched first safehouse visit after a release ends at the next mission.
        if (!s_IsSafehouse && s_Kind == ebr::Kind::Evergreen)
            s_FreelancerWatch = false;

        if (TryReleaseFreelancerGuard())
            Logger::Warn("[NoMoreAltF4] SEAMLESS FREELANCER GUARD off: the server closed the hidden attempt and the "
                         "safehouse reloaded the campaign from the server.");

        if (s_FreelancerGuard.load() && s_Kind == ebr::Kind::Evergreen && !s_IsSafehouse)
        {
            Logger::Warn("[NoMoreAltF4] SEAMLESS FREELANCER GUARD index={} csid={}: new Freelancer mission after a "
                         "contained failure, before the safehouse reloaded the campaign. Its start is let through; "
                         "the game closes once it has been uploaded.",
                         eventIndex, std::string_view(s_Csid, s_CsidLen));
            ScheduleGuardKill(s_Csid, s_CsidLen);
        }

        if (s_SeamlessActive)
        {
            static constexpr const char* k_KindNames[] = { "unknown", "other", "arcade", "elusive", "evergreen" };
            static constexpr const char* k_ModeNames[] = { "terminate", "seamless-test", "seamless" };

            const char* s_ModeName = k_ModeNames[static_cast<size_t>(SeamlessFor(s_Csid, s_CsidLen))];
            if (!m_AutoKillEnabled)
                s_ModeName = "none (termination is off: a failure is not prevented)";
            else if (m_FreelancerOnly && s_Kind != ebr::Kind::Evergreen)
                s_ModeName = "none ('Only Enable for Freelancer' is on: a failure is not prevented)";
            else if (s_Kind != ebr::Kind::Unknown && ebr::KindOf(s_Csid, s_CsidLen) != s_Kind)
                s_ModeName = "terminate (the attempt table is full)";
            else if (!s_OriginalSendRequest)
                s_ModeName = "terminate (the request hook is not installed)";

            Logger::Info("[NoMoreAltF4] SEAMLESS READY csid={} kind={} mode={}",
                         std::string_view(s_Csid, s_CsidLen), k_KindNames[static_cast<size_t>(s_Kind)], s_ModeName);
        }
    }

    // Freelancer: remember when the mission in progress has ended well (see
    // IsFreelancerFailureCpd). These events come before its campaign upload.
    if (s_HasCsid && !s_MissionCsid.empty() && std::string_view(s_Csid, s_CsidLen) == std::string_view(s_MissionCsid)
        && (ebr::NameIs(s_EventStr.data(), s_EventStr.size(), "MissionCompleted_Event")
            || ebr::NameIs(s_EventStr.data(), s_EventStr.size(), "EvergreenExitTriggered")
            || ebr::NameIs(s_EventStr.data(), s_EventStr.size(), "exit_gate")))
        s_MissionEndedWell = true;

    // The server's close can arrive after the safehouse started: release then.
    if (TryReleaseFreelancerGuard())
        Logger::Warn("[NoMoreAltF4] SEAMLESS FREELANCER GUARD off: the server closed the hidden attempt and the "
                     "safehouse reloaded the campaign from the server.");

    // --- Freelancer watch (first safehouse visit after a release) ---
    // Should the game still process the hidden failure itself, its safehouse
    // upload would carry a campaign reset: close the game before it is sent.
    if (s_FreelancerWatch && !s_EventStr.empty()
        && ebr::NameIs(s_EventStr.data(), s_EventStr.size(), "CpdSet")
        && !(s_HasCsid && ebr::IsQuarantined(s_Csid, s_CsidLen))
        && LooksLikeFailureProcessing(s_EventStr))
    {
        Logger::Warn("[NoMoreAltF4] SEAMLESS FREELANCER WATCH index={}: the safehouse uploads what looks like a "
                     "processed failure, not sent: {}", eventIndex, std::string_view(s_EventStr).substr(0, 800));
        SeamlessFallback("a campaign reset after a contained Freelancer failure");
        return HookAction::Return();
    }

    // --- Freelancer Seamless guard (experimental) ---
    // After a contained Freelancer failure the game's own campaign data may be
    // the failed one (items gone, campaign reset). Any campaign upload that is not
    // an event of the contained attempt (those are rewritten), including one with
    // no usable session id, closes the game first. Only a display page flip
    // (GearWall_ThematicIndex / MyValue01) goes through.
    if (s_FreelancerGuard.load() && !s_EventStr.empty()
        && ebr::NameIs(s_EventStr.data(), s_EventStr.size(), "CpdSet")
        && !(s_HasCsid && ebr::IsQuarantined(s_Csid, s_CsidLen))
        && !IsSingleNumberObject(CpdSetValueHead(s_EventStr)))
    {
        Logger::Warn("[NoMoreAltF4] SEAMLESS FREELANCER GUARD index={} csid={}: campaign upload after a contained "
                     "failure, not sent: {}", eventIndex,
                     s_HasCsid ? std::string_view(s_Csid, s_CsidLen) : std::string_view("(none)"),
                     std::string_view(s_EventStr).substr(0, 800));
        SeamlessFallback("a campaign upload after a contained Freelancer failure");
        return HookAction::Return();
    }

    if (s_HasCsid && s_SeamlessActive)
    {
        const char* s_Text = s_EventStr.data();
        const size_t s_Len = s_EventStr.size();

        const std::string_view s_CsidView(s_Csid, s_CsidLen);

        // Leaving a Freelancer mission through the pause menu reports no death:
        // its first failure upload is this campaign data (see the Freelancer
        // block below), so it arms the attempt as a failure would.
        const bool s_CampaignFailedData = IsFreelancerFailureCpd(s_EventStr, s_Csid, s_CsidLen);

        if (ebr::NameIs(s_Text, s_Len, "Hero_Health"))
        {
            const char* s_Tok = nullptr;
            size_t s_TokLen = 0;
            if (ebr::ExtractNumberAfter(s_Text, s_Len, "\"Value\":", &s_Tok, &s_TokLen))
                ebr::NoteHealth(s_Csid, s_CsidLen, s_Tok, s_TokLen);
        }

        // Anything else the game emits for a contained attempt is
        // neutralized on send; the Terminate logic must not act on it.
        if (ebr::IsQuarantined(s_Csid, s_CsidLen))
        {
            Logger::Info("[NoMoreAltF4] SEAMLESS TAIL index={} csid={} (event of a contained attempt)",
                         eventIndex, s_CsidView);
            return HookAction::Continue();
        }

        if (ebr::IsFailureText(s_Text, s_Len, m_AllowExitToMenu) || s_CampaignFailedData)
        {
            const SeamlessMode s_Mode = SeamlessFor(s_Csid, s_CsidLen);
            if (s_Mode != SeamlessMode::Off)
            {
                // Nothing would rewrite the failure without the request hook.
                const char* s_Unavailable = nullptr;
                if (!s_OriginalSendRequest)
                    s_Unavailable = "the request hook is not installed";
                else if (!ebr::Arm(s_Csid, s_CsidLen, s_Mode == SeamlessMode::Staked))
                    s_Unavailable = "the attempt table is full";

                if (!s_Unavailable)
                {
                    // Deliberately no m_DeathDetected / m_NetworkBlocked here:
                    // those drive the frame-poll kill and the event drops.
                    // The same test the Terminate logic below uses for its
                    // exit-to-menu opt-out. Only the wording depends on it.
                    const bool s_ExitToMenu = ebr::NameIs(s_Text, s_Len, "ContractFailed")
                        && s_EventStr.find("exit to Main menu") != std::string::npos;

                    Logger::Warn("[NoMoreAltF4] SEAMLESS ARMED csid={} stakes={} index={} trigger={} — {} contained, "
                                 "the game keeps running.",
                                 s_CsidView, s_Mode == SeamlessMode::Staked ? "staked" : "test", eventIndex,
                                 s_ExitToMenu ? "exit-to-menu" : (s_CampaignFailedData ? "campaign-data" : "failure"),
                                 s_ExitToMenu ? "exit" : "failure");

                    const bool s_Freelancer = ebr::KindOf(s_Csid, s_CsidLen) == ebr::Kind::Evergreen;
                    m_SeamlessNoticeFreelancer = s_Freelancer;
                    if (s_Freelancer)
                    {
                        // Record the attempt before the guard goes on, so a close of
                        // an earlier attempt cannot release it.
                        NoteFreelancerContained(s_Csid, s_CsidLen);
                        s_FreelancerGuard = true;
                        Logger::Warn("[NoMoreAltF4] SEAMLESS FREELANCER GUARD on: until the server has closed this "
                                     "attempt and the safehouse has reloaded the campaign, any campaign upload other "
                                     "than a display page flip closes the game before it is sent.");
                    }

                    OnSeamlessArmed(s_Csid, s_CsidLen, s_ExitToMenu);
                    return HookAction::Continue();
                }

                Logger::Error("[NoMoreAltF4] SEAMLESS UNAVAILABLE csid={} reason={}", s_CsidView, s_Unavailable);

                // A penalty is at risk and the attempt cannot be contained:
                // terminate here, as Terminate mode does. (The Terminate logic
                // below depends on mode flags that only the request hook sets.)
                if (s_Mode == SeamlessMode::Staked)
                {
                    m_DeathDetected = true;
                    Logger::Warn("[NoMoreAltF4] TERMINATED — auto-kill from OnEventSent (Seamless unavailable).");
                    KillProcess();
                    return HookAction::Return();
                }
                // Test stakes: fall through to the Terminate logic below.
            }
        }
    }

    // --- Freelancer: a mission that ends without a death event ---
    // Leaving a Freelancer mission through the pause menu sends no death event and
    // not the "exit to Main menu" text. The game first uploads the campaign data
    // with "Hot_MissionFailed":true and an empty inventory (that upload IS the
    // loss), then ContractFailed "Contract session stopped by event". The scene is
    // unloading by then and the live Freelancer flag may already be reset, so this
    // is decided from the event alone (CpdSet exists only in Freelancer), whatever
    // "Only Enable for Freelancer" and "Allow Exit to Main Menu" say: in Freelancer
    // an exit always costs the carried gear.
    // - Campaign data reporting a failed mission (IsFreelancerFailureCpd: any
    //   ..._MissionFailed flag, or a mission's end upload without a
    //   ..._MissionCompleted flag): terminate.
    // - ContractFailed of a Freelancer mission: terminate (Freelancer has no
    //   restart, replan or save load). The safehouse is excluded: quitting the
    //   game there ends its session the same way and costs nothing.
    // Seen on game 3.290, 2026-11-12: a showdown exit lost the whole campaign.
    if (s_HasCsid && !s_EventStr.empty())
    {
        const char* s_Text = s_EventStr.data();
        const size_t s_Len = s_EventStr.size();

        const bool s_CampaignFailed = IsFreelancerFailureCpd(s_EventStr, s_Csid, s_CsidLen);
        const bool s_FreelancerFailed = ebr::NameIs(s_Text, s_Len, "ContractFailed")
            && ebr::KindOf(s_Csid, s_CsidLen) == ebr::Kind::Evergreen;
        const bool s_InSafehouse = !s_SafehouseCsid.empty()
            && std::string_view(s_Csid, s_CsidLen) == std::string_view(s_SafehouseCsid);

        if (s_FreelancerFailed && !s_CampaignFailed && s_InSafehouse)
        {
            Logger::Info("[NoMoreAltF4] Freelancer safehouse session end (index={}): not a mission failure.", eventIndex);
            return HookAction::Continue();
        }

        if (s_CampaignFailed || s_FreelancerFailed)
        {
            Logger::Warn("[NoMoreAltF4] Freelancer failure detected at OnEventSent (index={}, {}{}).",
                         eventIndex, s_CampaignFailed ? "campaign data of a failed mission" : "ContractFailed of a mission",
                         s_InSafehouse ? ", in the safehouse" : "");

            if (m_AutoKillEnabled)
            {
                m_DeathDetected = true;
                Logger::Warn("[NoMoreAltF4] TERMINATED — auto-kill from OnEventSent (Freelancer failure).");
                KillProcess();
                return HookAction::Return();
            }

            Logger::Warn("[NoMoreAltF4] Freelancer failure detected but 'Terminate on Death/Failure' is OFF — "
                         "the failure will be saved and is NOT prevented.");
        }
    }
    else if (s_EventStr.empty() && m_AutoKillEnabled)
    {
        // Without the event text the event-layer checks (this one included) are
        // blind; only the death-event names in the HTTP hook remain.
        static std::atomic<bool> s_Warned{ false };
        if (!s_Warned.exchange(true))
            Logger::Error("[NoMoreAltF4] Online event text is unavailable (ZDynamicObject_ToString): failures "
                          "detected only from event text, such as a Freelancer pause-menu exit, are NOT caught.");
    }

    if (ShouldProtect() && !s_EventStr.empty())
    {
        // Unambiguous player-death / failure events — always a real failure.
        const bool s_DeathEvent =
            s_EventStr.find("\"Name\":\"MissionFailed_Event\"") != std::string::npos
            || s_EventStr.find("\"Name\":\"MissionWounded_Event\"") != std::string::npos
            || s_EventStr.find("\"Name\":\"MildChess_MissionFailed\"") != std::string::npos;

        // ContractFailed is also used by legitimate restart/replan/load in normal
        // contracts (where the manual-action markers live in sibling events we
        // can't see here), so only treat it as a protectable failure in the
        // single-attempt modes this mod targets (Freelancer / Elusive Target /
        // Arcade), where there is no legitimate mid-mission restart.
        const bool s_ContractFailed =
            s_EventStr.find("\"Name\":\"ContractFailed\"") != std::string::npos
            && (IsFreelancerMode() || m_ElusiveOrArcadeDetected);

        if (s_DeathEvent || s_ContractFailed)
        {
            // Manual player actions (Restart Mission, Replan Mission, load)
            // also emit ContractFailed, with the reason embedded in the event
            // itself: "Value":"Contract ended manually: OnRestartLevel" (or
            // OnReplanLevel / OnLoadGame).  ETs allow restart/replan before
            // the target is engaged, so these are legitimate in every mode —
            // let them through.  Both markers observed live on the Herbalist
            // ET (game 3.280); a real failure never carries them.
            if (!s_DeathEvent && s_ContractFailed
                && (s_EventStr.find("OnRestartLevel") != std::string::npos
                    || s_EventStr.find("OnReplanLevel") != std::string::npos
                    || s_EventStr.find("OnLoadGame") != std::string::npos))
            {
                Logger::Info("[NoMoreAltF4] ContractFailed is a manual restart/replan/load — allowing.");
                return HookAction::Continue();
            }

            // Honor the exit-to-menu opt-out.
            if (!s_DeathEvent && s_ContractFailed && m_AllowExitToMenu
                && s_EventStr.find("exit to Main menu") != std::string::npos)
            {
                Logger::Info("[NoMoreAltF4] ContractFailed is an allowed exit-to-menu — passing through.");
                return HookAction::Continue();
            }

            Logger::Warn("[NoMoreAltF4] Failure event detected at OnEventSent (index={}, death={}, contractFailed={}).",
                eventIndex, s_DeathEvent, s_ContractFailed);
            m_DeathDetected = true;

            // Termination is the ONLY reliable, crash-safe protection. It fires
            // here — before the failure is dispatched or batched into a SaveEvents2
            // POST — so the failure never leaves the machine. (The failure is saved
            // via plain WinHTTP POSTs that cannot be suppressed without crashing or
            // hanging the game, so there is no safe "don't close the game" path.)
            if (m_AutoKillEnabled)
            {
                Logger::Warn("[NoMoreAltF4] TERMINATED — auto-kill from OnEventSent.");
                KillProcess();
                return HookAction::Return();
            }

            Logger::Warn("[NoMoreAltF4] Failure detected but 'Terminate on Death/Failure' is OFF — "
                         "the failure will be saved and is NOT prevented. Enable auto-terminate to protect.");
        }
    }

    return HookAction::Continue(); // framework calls original
}

// -----------------------------------------------------------------------------
// Http_WinHttpCallback
//
// Fires on every WinHTTP status change for the game's HTTP requests.
// At WINHTTP_CALLBACK_STATUS_SENDING_REQUEST (0x10) the request is about to be
// sent — this is where we log the full URL when diagnostic mode is on.
//
// We cannot cancel a WinHTTP request from within its own callback (Microsoft
// docs advise against calling WinHttp functions from callbacks), so this hook
// is logging-only.  The ZAchievementManagerSimple hook above is the actual
// blocking mechanism.
// -----------------------------------------------------------------------------
DEFINE_PLUGIN_DETOUR(NoMoreAltF4, void, Http_WinHttpCallback,
    void* dwContext, void* hInternet, void* param_3, int dwInternetStatus,
    void* param_5, int param_6)
{
    // WINHTTP_CALLBACK_STATUS_SENDING_REQUEST = 0x00000010
    // At this status the request handle is valid and WinHttpQueryOption(URL) works.
    // We inspect the URL when either logging is enabled OR network kill is active
    // (to detect any gap in ZAchievementManagerSimple coverage).
    if (dwInternetStatus == WINHTTP_CALLBACK_STATUS_SENDING_REQUEST && hInternet
        && (m_LogHttpRequests || m_NetworkBlocked))
    {
        wchar_t s_UrlBuf[2048] = {};
        DWORD s_UrlLen = sizeof(s_UrlBuf);
        if (WinHttpQueryOption(static_cast<HINTERNET>(hInternet), WINHTTP_OPTION_URL, s_UrlBuf, &s_UrlLen))
        {
            // Narrow-convert: IOI API URLs are ASCII-safe.
            std::string s_Url(s_UrlBuf, s_UrlBuf + wcslen(s_UrlBuf));

            if (m_LogHttpRequests)
                Logger::Info("[NoMoreAltF4] HTTP Sending: {}", s_Url);

            // SaveAndSynchronizeEvents4 is the session-close packet that records
            // contract outcomes including ET failures. If it fires while we're in
            // network-kill mode, our ZAchievementManagerSimple block didn't catch
            // the SegmentClosing event in time. Log a warning so this gap is visible.
            if (m_NetworkBlocked && s_Url.find("SaveAndSynchronizeEvents4") != std::string::npos)
                Logger::Error("[NoMoreAltF4] WARNING: SaveAndSynchronizeEvents4 reached HTTP layer "
                              "despite network kill — event block may not have fired in time.");
        }
    }

    return HookAction::Continue(); // framework calls original
}

// -----------------------------------------------------------------------------
// ZHttpResultDynamicObject_OnBufferReady
//
// Fires when an HTTP response body is fully received.  Used in diagnostic mode
// to log response content so you can identify exactly which API endpoints IOI
// uses for mission/ET outcomes.
// -----------------------------------------------------------------------------

// Inflate a gzip/zlib-compressed buffer into p_Out for LOG DISPLAY ONLY — the
// game's own buffer is never modified.  Returns false if the data doesn't
// decompress cleanly (caller falls back to raw logging).
static bool TryInflateForLog(const uint8_t* p_Data, size_t p_Size, std::string& p_Out)
{
    z_stream s_Strm {};
    // 15 = max window size; +32 auto-detects gzip or zlib headers.
    if (inflateInit2(&s_Strm, 15 + 32) != Z_OK)
        return false;

    s_Strm.next_in = const_cast<Bytef*>(p_Data);
    s_Strm.avail_in = static_cast<uInt>(p_Size);

    char s_Chunk[16384];
    int s_Ret = Z_OK;

    while (s_Ret == Z_OK && p_Out.size() < k_MaxLogBytes)
    {
        s_Strm.next_out = reinterpret_cast<Bytef*>(s_Chunk);
        s_Strm.avail_out = sizeof(s_Chunk);
        s_Ret = inflate(&s_Strm, Z_NO_FLUSH);

        if (s_Ret != Z_OK && s_Ret != Z_STREAM_END)
            break;

        p_Out.append(s_Chunk, sizeof(s_Chunk) - s_Strm.avail_out);
    }

    inflateEnd(&s_Strm);

    // Success = full stream inflated, or output hit the log cap mid-stream.
    return s_Ret == Z_STREAM_END || (s_Ret == Z_OK && p_Out.size() >= k_MaxLogBytes);
}

DEFINE_PLUGIN_DETOUR(NoMoreAltF4, void, ZHttpResultDynamicObject_OnBufferReady,
    ZHttpResultDynamicObject* th)
{
    // The Seamless self-check reads responses whether or not logging is on.
    const bool s_SeamlessCheck = ebr::HasQuarantine() || ebr::HasPast();

    if ((m_LogHttpRequests || s_SeamlessCheck) && th)
    {
        // SDK v4.2 renamed ZBuffer::data()/size() to Get()/Size().  Access by
        // reference only — never copy a ZBuffer: since v4.2 its destructor
        // really frees the game's buffer when the refcount drops.
        const auto* s_Data = static_cast<const char*>(th->m_buffer.Get());
        const auto  s_Size = th->m_buffer.Size();

        if (s_Data && s_Size > 0)
        {
            // IOI compresses some endpoints; this hook sees the raw buffer
            // before the game inflates it (gzip magic 1F 8B), so those bodies
            // would log as binary garbage.  Decompress a copy for display.
            bool s_Logged = false;
            const bool s_Gzip = s_Size >= 2 && static_cast<uint8_t>(s_Data[0]) == 0x1F
                && static_cast<uint8_t>(s_Data[1]) == 0x8B;

            if (s_Gzip)
            {
                std::string s_Inflated;
                if (TryInflateForLog(reinterpret_cast<const uint8_t*>(s_Data), s_Size, s_Inflated))
                {
                    if (s_SeamlessCheck)
                        CheckServerCloses(s_Inflated.data(), s_Inflated.size());

                    if (m_LogHttpRequests)
                    {
                        const bool s_Clipped = s_Inflated.size() > k_MaxLogBytes;
                        if (s_Clipped)
                            s_Inflated.resize(k_MaxLogBytes);
                        Logger::Info("[NoMoreAltF4] HTTP Response (gzip {} -> {} bytes): {}{}",
                            s_Size, s_Inflated.size(), s_Inflated, s_Clipped ? "..." : "");
                    }
                    s_Logged = true;
                }
                else if (ebr::HasQuarantine())
                {
                    Logger::Warn("[NoMoreAltF4] SEAMLESS-CHECK blind: a compressed response of {} bytes could not be decoded.", s_Size);
                }
            }
            else if (s_SeamlessCheck)
            {
                CheckServerCloses(s_Data, s_Size);
            }

            if (!s_Logged && m_LogHttpRequests)
            {
                // Clip only pathologically large responses (see k_MaxLogBytes).
                const size_t s_LogSize = s_Size < k_MaxLogBytes ? s_Size : k_MaxLogBytes;
                std::string s_Body(s_Data, s_Data + s_LogSize);
                Logger::Info("[NoMoreAltF4] HTTP Response ({} bytes): {}{}", s_Size, s_Body,
                    s_Size > k_MaxLogBytes ? "..." : "");
            }
        }
    }

    return HookAction::Continue(); // framework calls original
}

// =============================================================================
// ImGui Settings Panel (ZHMModSDK Overlay Menu)
// =============================================================================

void NoMoreAltF4::OnDrawMenu()
{
    if (ImGui::BeginMenu(ICON_MD_SHIELD " NoMoreAltF4"))
    {
        ImGui::TextDisabled("NoMoreAltF4 v%s", k_Version);

        // --- Protection settings ---
        if (ImGui::Checkbox("Terminate Game Process on Death/Failure", &m_AutoKillEnabled))
        {
            SetSettingBool(S_SETTINGS_SECTION, "autokillenabled", m_AutoKillEnabled);
            Logger::Info("[NoMoreAltF4] Terminate on failure: {}", m_AutoKillEnabled ? "ON" : "OFF");
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Kill the game process the instant a mission failure is detected,\n"
                              "before it can be saved to IOI servers. This is the ONLY reliable,\n"
                              "crash-safe protection — the failure is recorded via network requests\n"
                              "that cannot be blocked without crashing or hanging the game.\n"
                              "Protects Freelancer items and Elusive Target / Arcade attempts.");

        if (!m_AutoKillEnabled)
            ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.0f, 1.0f),
                ICON_MD_WARNING " Protection OFF — failures will be saved.");

        ImGui::Separator();

        // --- Scope ---
        if (ImGui::Checkbox("Only Enable for Freelancer", &m_FreelancerOnly))
        {
            SetSettingBool(S_SETTINGS_SECTION, "freelanceronly", m_FreelancerOnly);
            Logger::Info("[NoMoreAltF4] Freelancer only: {}", m_FreelancerOnly ? "ON" : "OFF");
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("When enabled, all protection is limited to Freelancer mode.\n"
                              "Other game modes (ETs, contracts, etc.) are unaffected.\n"
                              "When disabled, protection is active in all modes.");

        if (ImGui::Checkbox("Allow Exit to Main Menu", &m_AllowExitToMenu))
        {
            SetSettingBool(S_SETTINGS_SECTION, "allowexittomenu", m_AllowExitToMenu);
            Logger::Info("[NoMoreAltF4] Allow exit to menu: {}", m_AllowExitToMenu ? "ON" : "OFF");
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("When enabled, exiting to the main menu from the pause menu is\n"
                              "reported to the server as usual.\n"
                              "When disabled, that exit is treated as a failure: Terminate closes\n"
                              "the game, Seamless contains the exit (the server is not told).\n"
                              "WARNING: In Freelancer/ET, exiting to menu counts as a failure!\n"
                              "Freelancer ignores this option: leaving a mission always closes the game\n"
                              "(or is hidden, with the experimental Freelancer Seamless box), because the\n"
                              "exit would cost the gear you carried.");

        bool s_EnableSaving = m_EnableSavingStory.load();
        if (ImGui::Checkbox("Allow saving in no-save story missions and escalations", &s_EnableSaving))
        {
            m_EnableSavingStory = s_EnableSaving;
            SetSettingBool(S_SETTINGS_SECTION, "enablesavingstory", s_EnableSaving);
            Logger::Info("[NoMoreAltF4] Allow saving in no-save story missions and escalations: {}", s_EnableSaving ? "ON" : "OFF");
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Un-greys the Save button in missions where the server switches saving off,\n"
                              "such as Holiday Hoarders, and in escalations. Applies from the next mission load.\n"
                              "Freelancer, Elusive Targets and Arcade are never changed.\n"
                              "In an escalation, only load saves made in the level you are playing:\n"
                              "the server's escalation level does not roll back.\n"
                              "If the mod was disabled and re-enabled in this run, restart the game first.");

        ImGui::Separator();

        // --- Seamless (experimental) ---
        ImGui::TextUnformatted("Seamless: contain a failure without closing the game");
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Instead of terminating, the failed attempt's events are rewritten\n"
                              "into harmless ones before they are sent. You see the normal failure\n"
                              "screen, go back to the menu and start again.\n"
                              "If anything looks wrong, the mod terminates the game as usual.\n"
                              "Needs 'Terminate Game Process on Death/Failure' to stay ON.");

        struct SeamlessToggle
        {
            const char* label;
            const char* key;
            std::atomic<bool>* value;
            const char* tooltip;
        };

        const SeamlessToggle s_Toggles[] = {
            { "Seamless protection (story, Arcade, Elusive Targets)", "seamless", &m_Seamless,
              "A death, a failed objective or a pause-menu exit is hidden from the server.\n"
              "You see the normal failure screen: choose exit to menu, then start again.\n"
              "Restart, Replan and Load are your own choice and are sent as usual.\n"
              "Tested on game 3.290.1.0: story missions, Elusive Target Arcade and a live\n"
              "Elusive Target (deaths and exits contained, then completed normally).\n"
              "After a game update, try it in a story mission first: nothing is at stake there." },
            { "Freelancer (experimental, use cheap gear)", "seamlessfreelancer", &m_SeamlessFreelancer,
              "A failed or abandoned Freelancer mission is hidden from the server like an\n"
              "Arcade level. The end screen then cannot load: wait for the error dialog,\n"
              "choose 'Go to menu' and enter Freelancer again. Until the safehouse has\n"
              "reloaded your campaign from the server, any campaign upload or mission start\n"
              "closes the game first, so the campaign on the server never changes.\n"
              "Covers showdowns too, but test it in normal missions first." },
        };

        for (const auto& s_Toggle : s_Toggles)
        {
            bool s_Value = s_Toggle.value->load();
            if (ImGui::Checkbox(s_Toggle.label, &s_Value))
            {
                s_Toggle.value->store(s_Value);
                SetSettingBool(S_SETTINGS_SECTION, ZString(std::string_view(s_Toggle.key)), s_Value);
                Logger::Info("[NoMoreAltF4] Seamless '{}': {}", s_Toggle.key, s_Value ? "ON" : "OFF");
                if (s_Value)
                    m_SeamlessAlarm = false;
            }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("%s", s_Toggle.tooltip);
        }

        if (!m_SeamlessFreelancer)
            ImGui::TextDisabled("Freelancer terminates unless its experimental box is ticked.");

        if (AnySeamlessEnabled() && !m_AutoKillEnabled)
            ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.0f, 1.0f),
                ICON_MD_WARNING " Seamless is inactive while termination is OFF.");

        // That option switches every protection off outside Freelancer, so the
        // story, Arcade and Elusive Target boxes then protect nothing.
        if (m_Seamless && m_FreelancerOnly)
            ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.0f, 1.0f),
                ICON_MD_WARNING " Seamless is inactive while 'Only Enable for Freelancer' is ON:\n"
                "story, Arcade and Elusive Target failures are NOT protected at all.");

        if (AnySeamlessEnabled() && !s_OriginalSendRequest)
            ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.0f, 1.0f),
                ICON_MD_WARNING " Seamless is unavailable (request hook not installed): failures terminate.");

        if (const uint32_t s_Contained = ebr::QuarantineCount())
        {
            ImGui::Text("Contained attempts this run: %u", s_Contained);
            if (s_Contained >= ebr::k_Slots)
                ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.0f, 1.0f),
                    ICON_MD_WARNING " Seamless capacity reached: further failures terminate until the game is restarted.");
        }

        if (m_SeamlessAlarm)
            ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.3f, 1.0f),
                ICON_MD_WARNING " The server closed a contained attempt unexpectedly.\n"
                "Seamless was switched off. See the session log (SEAMLESS-CHECK).");

        ImGui::Separator();

        // --- Abort hotkey ---
        const char* keyName = "None";
        switch (m_ManualKillKey)
        {
        case 0:      keyName = "None"; break;
        case VK_F8:  keyName = "F8"; break;
        case VK_F9:  keyName = "F9"; break;
        case VK_F10: keyName = "F10"; break;
        case VK_F11: keyName = "F11"; break;
        case VK_F12: keyName = "F12"; break;
        default:     keyName = "Custom"; break;
        }
        if (m_ManualKillKey != 0)
            ImGui::Text("Manual abort hotkey: %s (0x%X)", keyName, m_ManualKillKey);
        else
            ImGui::Text("Manual abort hotkey: None (disabled)");
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Press this key to instantly abort the mission.\nUse when spotted or about to fail.\n"
                              "Ignored while Alt or Win is held, so other tools'\ncombos (e.g. Alt+F9 recording) can't trigger it.");

        if (ImGui::SmallButton("None")) { m_ManualKillKey = 0;     SetSettingInt(S_SETTINGS_SECTION, "manualkillkey", m_ManualKillKey); }
        ImGui::SameLine();
        if (ImGui::SmallButton("F8")) { m_ManualKillKey = VK_F8;  SetSettingInt(S_SETTINGS_SECTION, "manualkillkey", m_ManualKillKey); }
        ImGui::SameLine();
        if (ImGui::SmallButton("F9")) { m_ManualKillKey = VK_F9;  SetSettingInt(S_SETTINGS_SECTION, "manualkillkey", m_ManualKillKey); }
        ImGui::SameLine();
        if (ImGui::SmallButton("F10")) { m_ManualKillKey = VK_F10; SetSettingInt(S_SETTINGS_SECTION, "manualkillkey", m_ManualKillKey); }
        ImGui::SameLine();
        if (ImGui::SmallButton("F11")) { m_ManualKillKey = VK_F11; SetSettingInt(S_SETTINGS_SECTION, "manualkillkey", m_ManualKillKey); }
        ImGui::SameLine();
        if (ImGui::SmallButton("F12")) { m_ManualKillKey = VK_F12; SetSettingInt(S_SETTINGS_SECTION, "manualkillkey", m_ManualKillKey); }

        // SDK v4.2+ bundles a QuickSave mod that binds F9 to quick-load (F5 to
        // quick-save). With both on F9, a quick-load would close the game.
        if (m_ManualKillKey == VK_F9)
            ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.2f, 1.0f),
                "Note: F9 is also the SDK QuickSave mod's quick-load key.\n"
                "If you use QuickSave, pick a different abort key.");

        ImGui::Separator();

        // --- Debug ---
        if (ImGui::Checkbox("Log HTTP requests", &m_LogHttpRequests))
        {
            SetSettingBool(S_SETTINGS_SECTION, "loghttprequests", m_LogHttpRequests);
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Log all outgoing IOI HTTP request URLs and response bodies\n"
                              "to the ZHMModSDK log. Use this to identify which endpoints\n"
                              "handle Elusive Target / mission results.");

        if (ImGui::Checkbox("Archive session logs", &m_ArchiveLogs))
        {
            SetSettingBool(S_SETTINGS_SECTION, "archivesessionlogs", m_ArchiveLogs);
            m_LogMirror.SetEnabled(m_ArchiveLogs);
            Logger::Info("[NoMoreAltF4] Archive session logs: {}", m_ArchiveLogs ? "ON" : "OFF");
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("ZHMModLoader.log is wiped every time the game starts. When enabled,\n"
                              "each session is copied live into NoMoreAltF4Logs\\ (last 30 kept),\n"
                              "including the final lines before an auto-terminate or crash.\n"
                              "Login tokens are redacted, so these files are safe to share.");

        const auto s_ArchiveFolder = m_LogMirror.Folder();
        if (!s_ArchiveFolder.empty())
        {
            ImGui::SameLine();
            if (ImGui::SmallButton(ICON_MD_FOLDER_OPEN " Open log folder"))
                ShellExecuteW(nullptr, L"open", s_ArchiveFolder.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Current session: %ls", m_LogMirror.SessionFile().filename().c_str());
        }

        ImGui::Separator();

        // --- Status ---
        if (m_Initialized)
        {
            bool s_Protected = ShouldProtect();
            if (s_Protected)
                ImGui::TextColored(ImVec4(0.0f, 1.0f, 0.0f, 1.0f), ICON_MD_SHIELD " Protection active");
            else
                ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1.0f), ICON_MD_SHIELD " Protection inactive (not Freelancer)");

            const char* s_Mode = "---";
            if (IsFreelancerMode())
                s_Mode = "Freelancer";
            else if (SDK()->GetLocalPlayer())
                s_Mode = "Standard";
            ImGui::Text("Mode: %s", s_Mode);
        }
        else
        {
            ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.0f, 1.0f), ICON_MD_HOURGLASS_EMPTY " Waiting for engine...");
        }

        ImGui::EndMenu();
    }
}
