#include "NoMoreAltF4.h"

#include <Logging.h>
#include <IconsMaterialDesign.h>
#include <imgui.h>
#include <Hooks.h>
#include <Globals.h>
#include <Glacier/ZHttp.h>
#include <Glacier/ZHitman5.h>
#include <Glacier/ZContract.h>
#include <Glacier/SOnlineEvent.h>

#include <Windows.h>
#pragma comment(lib, "winhttp.lib")

// =============================================================================
// SDK Plugin Registration
// =============================================================================
// DEFINE_ZHM_PLUGIN creates the singleton instance and exports:
//   GetPluginInterface(), CompiledSdkVersion(), CompiledSdkAbiVersion()
DEFINE_ZHM_PLUGIN(NoMoreAltF4);

// Settings INI section name
static const ZString S_SETTINGS_SECTION = "NoMoreAltF4";

// Static storage for the original WinHttpSendRequest pointer (pre-hook).
NoMoreAltF4::FnWinHttpSendRequest NoMoreAltF4::s_OriginalSendRequest = nullptr;
std::atomic<bool> NoMoreAltF4::s_HookPassthrough{ false };

// =============================================================================
// Initialization
// =============================================================================

void NoMoreAltF4::Init()
{
    // Load persisted settings from the plugin's INI file.
    // Settings API: GetSettingBool(section, name, defaultValue)
    m_AutoKillEnabled      = GetSettingBool(S_SETTINGS_SECTION, "AutoKillEnabled",      true);
    m_FreelancerOnly       = GetSettingBool(S_SETTINGS_SECTION, "FreelancerOnly",       false);
    m_ManualKillKey        = static_cast<int>(GetSettingInt(S_SETTINGS_SECTION, "ManualKillKey", 0));
    m_BlockNetworkOnDeath  = GetSettingBool(S_SETTINGS_SECTION, "BlockNetworkOnDeath",  true);
    m_LogHttpRequests      = GetSettingBool(S_SETTINGS_SECTION, "LogHttpRequests",      true);
    m_AllowExitToMenu      = GetSettingBool(S_SETTINGS_SECTION, "AllowExitToMenu",      false);

    Logger::Info("[NoMoreAltF4] Plugin loaded. Auto-kill: {}, Freelancer-only: {}, Hotkey: 0x{:X}, BlockNet: {}, LogHTTP: {}",
        m_AutoKillEnabled, m_FreelancerOnly, m_ManualKillKey, m_BlockNetworkOnDeath, m_LogHttpRequests);
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
    InstallSendRequestHook();
}

NoMoreAltF4::~NoMoreAltF4()
{
    // Make IAT hook a passthrough, then swap IAT to persistent JMP stub
    // so in-flight WinHttpSendRequest calls don't land in freed DLL memory.
    s_HookPassthrough = true;
    RemoveSendRequestHook();

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

    // --- Manual hotkey check (edge-triggered: fires once on key down) ---
    if (m_ManualKillKey != 0)
    {
        bool keyDown = (GetAsyncKeyState(m_ManualKillKey) & 0x8000) != 0;
        if (keyDown && !m_ManualKillKeyPrevState)
        {
            if (ShouldProtect())
            {
                Logger::Warn("[NoMoreAltF4] Manual abort triggered (hotkey 0x{:X}). Terminating.", m_ManualKillKey);
                KillProcess();
                return;
            }
        }
        m_ManualKillKeyPrevState = keyDown;
    }

    // --- Reset death detection on new mission entry ---
    {
        auto s_LocalPlayer = SDK()->GetLocalPlayer();
        bool s_InMission = static_cast<bool>(s_LocalPlayer);

        if (s_InMission && !m_PlayerWasInMission)
        {
            // Just entered a mission — clear stale flags
            m_DeathDetected = false;
            // Note: m_FreelancerDetected is NOT reset here — it persists across
            // missions within the same Freelancer campaign. It's only reset when
            // the player leaves a mission (below), so it re-detects on next entry.
        }
        if (!s_InMission && m_PlayerWasInMission)
        {
            // Left a mission — reset Freelancer flag so it re-detects next time
            m_FreelancerDetected = false;
        }
        m_PlayerWasInMission = s_InMission;
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
        m_NetworkBlocked = true;

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
    s_OriginalSendRequest = PatchIAT(&NoMoreAltF4::HookedSendRequest);
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
    if (s_PersistentStub && s_OriginalSendRequest)
    {
        PatchIAT(reinterpret_cast<FnWinHttpSendRequest>(s_PersistentStub));
        s_OriginalSendRequest = nullptr;
    }
}

// The replacement function: inspect, log, and optionally block POST requests.
// The body in lpOptional is plain UTF-8 JSON — IOI does not encrypt it.
//
// Blocking strategy (learned from packet captures):
//   - On actual death, the game sends MissionFailed_Event, MissionWounded_Event,
//     and MildChess_MissionFailed via SaveEvents2 POST bodies.
//   - ContractFailed is sent outgoing only for manual actions (exit-to-menu,
//     restart, replan). On death, ContractFailed only appears as a server→client
//     response event, not in outgoing POST bodies.
//   - SaveAndSynchronizeEvents4 is then called to sync — the server derives
//     SegmentClosing from the previously-sent failure events.
//   - Therefore: block SaveEvents2 bodies containing any failure event,
//     and block SaveAndSynchronizeEvents4 entirely while network kill is active.
//   - We return TRUE (success) without calling the original, so the game
//     believes the request succeeded — no retry loops.
BOOL WINAPI NoMoreAltF4::HookedSendRequest(
    HINTERNET hRequest, LPCWSTR pwszHeaders, DWORD dwHeadersLength,
    LPVOID lpOptional, DWORD dwOptionalLength,
    DWORD dwTotalLength, DWORD_PTR dwContext)
{
    // During shutdown, skip all logic and call the original directly.
    // This prevents accessing freed plugin memory while in-flight calls drain.
    if (s_HookPassthrough)
        return s_OriginalSendRequest(hRequest, pwszHeaders, dwHeadersLength,
            lpOptional, dwOptionalLength, dwTotalLength, dwContext);

    auto* s_Plugin = Plugin();

    // Build body string once for both logging and blocking checks.
    std::string s_Body;
    if (s_Plugin && lpOptional && dwOptionalLength > 0)
        s_Body.assign(reinterpret_cast<const char*>(lpOptional), dwOptionalLength);

    // --- Diagnostic logging ---
    if (s_Plugin && s_Plugin->m_LogHttpRequests && !s_Body.empty())
    {
        std::string s_LogBody = s_Body;
        constexpr size_t k_MaxLog = 1024;
        if (s_LogBody.size() > k_MaxLog)
        {
            s_LogBody.resize(k_MaxLog);
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
    if (s_Plugin && !s_Plugin->m_FreelancerDetected && !s_Body.empty()
        && (s_Body.find("\"Evergreen_") != std::string::npos
            || s_Body.find("\"EvergreenMission") != std::string::npos
            || s_Body.find("\"ContractType\":\"evergreen\"") != std::string::npos))
    {
        Logger::Info("[NoMoreAltF4] Freelancer mode detected via Evergreen event in HTTP traffic.");
        s_Plugin->m_FreelancerDetected = true;
    }

    // --- Death detection + network blocking ---
    // Detect failure events in outgoing SaveEvents2 POST bodies.
    // The game sends different events depending on the type of failure:
    //   - MissionFailed_Event / MissionWounded_Event — actual player death
    //   - MildChess_MissionFailed — Freelancer-specific failure event
    //   - ContractFailed — manual actions (exit-to-menu, restart, replan, load)
    //     Note: ContractFailed does NOT appear in outgoing bodies on actual death;
    //     on death the game sends MissionFailed_Event etc. instead.
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

            // If auto-kill is enabled, kill immediately (fastest path)
            if (s_Plugin->m_AutoKillEnabled)
            {
                Logger::Warn("[NoMoreAltF4] TERMINATED — auto-kill from HTTP hook.");
                s_Plugin->KillProcess();
                return TRUE; // unreachable after TerminateProcess, but keeps compiler happy
            }

            // Even without auto-kill, block the request if network blocking is enabled
            if (s_Plugin->m_BlockNetworkOnDeath)
            {
                Logger::Warn("[NoMoreAltF4] BLOCKED outgoing failure events.");
                s_Plugin->m_NetworkBlocked = true;
                return TRUE; // fake success
            }
        }
    }

    // --- Network blocking (after KillProcess or manual block) ---
    if (s_Plugin && s_Plugin->m_NetworkBlocked && hRequest)
    {
        wchar_t s_UrlBuf[2048] = {};
        DWORD s_UrlLen = sizeof(s_UrlBuf);
        if (WinHttpQueryOption(hRequest, WINHTTP_OPTION_URL, s_UrlBuf, &s_UrlLen))
        {
            std::wstring s_Url(s_UrlBuf);

            // Block SaveAndSynchronizeEvents4 entirely — it triggers SegmentClosing.
            if (s_Url.find(L"SaveAndSynchronizeEvents4") != std::wstring::npos)
            {
                Logger::Warn("[NoMoreAltF4] BLOCKED SaveAndSynchronizeEvents4 (network kill active).");
                return TRUE;
            }

            // Block any remaining SaveEvents2 with failure data.
            // Match "Name":"EventName" to avoid false positives from CpdSet field names.
            if (s_Url.find(L"SaveEvents2") != std::wstring::npos && !s_Body.empty())
            {
                if (s_Body.find("\"Name\":\"ContractFailed\"") != std::string::npos
                    || s_Body.find("\"Name\":\"MissionFailed_Event\"") != std::string::npos
                    || s_Body.find("\"Name\":\"MissionWounded_Event\"") != std::string::npos
                    || s_Body.find("\"Name\":\"MildChess_MissionFailed\"") != std::string::npos)
                {
                    Logger::Warn("[NoMoreAltF4] BLOCKED SaveEvents2 containing failure data.");
                    return TRUE;
                }
            }
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

    // Block server-side events when network kill is active
    if (m_NetworkBlocked)
    {
        Logger::Warn("[NoMoreAltF4] Blocking server event '{}' (network kill active).", s_Name);
        return HookAction::Return();
    }

    // Block SegmentClosing even if network kill hasn't been set yet —
    // this is the server's confirmation of mission failure
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
// Fires when the game dispatches an online event to IOI's servers (kills,
// mission outcomes, Elusive Target results, etc.).  This is the highest-level
// interception point — returning early drops the event before any HTTP call is
// ever initiated, which is earlier and safer than WinHTTP-level cancellation.
//
// When m_NetworkBlocked is true (set in KillProcess) we swallow the event so
// the ET/mission failure is never recorded server-side.
// -----------------------------------------------------------------------------
DEFINE_PLUGIN_DETOUR(NoMoreAltF4, void, ZAchievementManagerSimple_OnEventSent,
    ZAchievementManagerSimple* th, uint32_t eventIndex, const ZDynamicObject& event)
{
    if (m_NetworkBlocked)
    {
        Logger::Warn("[NoMoreAltF4] Blocking online event #{} (network kill active).", eventIndex);
        return HookAction::Return();
    }

    if (m_LogHttpRequests)
        Logger::Info("[NoMoreAltF4] Online event sent: index={}", eventIndex);

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
DEFINE_PLUGIN_DETOUR(NoMoreAltF4, void, ZHttpResultDynamicObject_OnBufferReady,
    ZHttpResultDynamicObject* th)
{
    if (m_LogHttpRequests && th)
    {
        const auto* s_Data = static_cast<const char*>(th->m_buffer.data());
        const auto  s_Size = th->m_buffer.size();

        if (s_Data && s_Size > 0)
        {
            // Truncate very large responses in the log.
            constexpr uint32_t k_MaxLog = 512;
            std::string s_Body(s_Data, s_Data + (s_Size < k_MaxLog ? s_Size : k_MaxLog));
            Logger::Info("[NoMoreAltF4] HTTP Response ({} bytes): {}{}", s_Size, s_Body,
                s_Size > k_MaxLog ? "..." : "");
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
        // --- Protection settings ---
        if (ImGui::Checkbox("Terminate Game Process on Death/Failure", &m_AutoKillEnabled))
        {
            SetSettingBool(S_SETTINGS_SECTION, "AutoKillEnabled", m_AutoKillEnabled);
            Logger::Info("[NoMoreAltF4] Terminate on failure: {}", m_AutoKillEnabled ? "ON" : "OFF");
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Kill the game process when a mission fails.\n"
                              "Prevents failure from being saved to IOI servers.\n"
                              "Protects Freelancer items and Elusive Target retries.");

        if (ImGui::Checkbox("Block Outgoing Mission Failure Messages", &m_BlockNetworkOnDeath))
        {
            SetSettingBool(S_SETTINGS_SECTION, "BlockNetworkOnDeath", m_BlockNetworkOnDeath);
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Intercept ContractFailed network events before they\n"
                              "reach IOI servers. Active even if auto-terminate is off.");

        ImGui::Separator();

        // --- Scope ---
        if (ImGui::Checkbox("Only Enable for Freelancer", &m_FreelancerOnly))
        {
            SetSettingBool(S_SETTINGS_SECTION, "FreelancerOnly", m_FreelancerOnly);
            Logger::Info("[NoMoreAltF4] Freelancer only: {}", m_FreelancerOnly ? "ON" : "OFF");
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("When enabled, all protection is limited to Freelancer mode.\n"
                              "Other game modes (ETs, contracts, etc.) are unaffected.\n"
                              "When disabled, protection is active in all modes.");

        if (ImGui::Checkbox("Allow Exit to Main Menu", &m_AllowExitToMenu))
        {
            SetSettingBool(S_SETTINGS_SECTION, "AllowExitToMenu", m_AllowExitToMenu);
            Logger::Info("[NoMoreAltF4] Allow exit to menu: {}", m_AllowExitToMenu ? "ON" : "OFF");
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("When enabled, exiting to main menu during a mission is allowed\n"
                              "without being blocked. When disabled, exit-to-menu is treated\n"
                              "as a potential failure event and blocked.\n"
                              "WARNING: In Freelancer/ET, exiting to menu counts as a failure!");

        ImGui::Separator();

        // --- Abort hotkey ---
        const char* keyName = "None";
        switch (m_ManualKillKey)
        {
        case 0:      keyName = "None"; break;
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
            ImGui::SetTooltip("Press this key to instantly abort the mission.\nUse when spotted or about to fail.");

        if (ImGui::SmallButton("None")) { m_ManualKillKey = 0;     SetSettingInt(S_SETTINGS_SECTION, "ManualKillKey", m_ManualKillKey); }
        ImGui::SameLine();
        if (ImGui::SmallButton("F9")) { m_ManualKillKey = VK_F9;  SetSettingInt(S_SETTINGS_SECTION, "ManualKillKey", m_ManualKillKey); }
        ImGui::SameLine();
        if (ImGui::SmallButton("F10")) { m_ManualKillKey = VK_F10; SetSettingInt(S_SETTINGS_SECTION, "ManualKillKey", m_ManualKillKey); }
        ImGui::SameLine();
        if (ImGui::SmallButton("F11")) { m_ManualKillKey = VK_F11; SetSettingInt(S_SETTINGS_SECTION, "ManualKillKey", m_ManualKillKey); }
        ImGui::SameLine();
        if (ImGui::SmallButton("F12")) { m_ManualKillKey = VK_F12; SetSettingInt(S_SETTINGS_SECTION, "ManualKillKey", m_ManualKillKey); }

        ImGui::Separator();

        // --- Debug ---
        if (ImGui::Checkbox("Log HTTP requests", &m_LogHttpRequests))
        {
            SetSettingBool(S_SETTINGS_SECTION, "LogHttpRequests", m_LogHttpRequests);
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Log all outgoing IOI HTTP request URLs and response bodies\n"
                              "to the ZHMModSDK log. Use this to identify which endpoints\n"
                              "handle Elusive Target / mission results.");

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
