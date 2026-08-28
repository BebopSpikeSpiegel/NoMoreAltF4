#include "NoMoreAltF4.h"

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
#pragma comment(lib, "winhttp.lib")

#include <zlib.h>

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

// =============================================================================
// Initialization
// =============================================================================

void NoMoreAltF4::Init()
{
    // Load persisted settings from the plugin's INI file.
    // Settings API: GetSettingBool(section, name, defaultValue)
    m_AutoKillEnabled      = GetSettingBool(S_SETTINGS_SECTION, "autokillenabled",      true);
    m_FreelancerOnly       = GetSettingBool(S_SETTINGS_SECTION, "freelanceronly",       false);
    m_ManualKillKey        = static_cast<int>(GetSettingInt(S_SETTINGS_SECTION, "manualkillkey", 0));
    m_BlockNetworkOnDeath  = GetSettingBool(S_SETTINGS_SECTION, "blocknetworkondeath",  true);
    m_LogHttpRequests      = GetSettingBool(S_SETTINGS_SECTION, "loghttprequests",      true);
    m_AllowExitToMenu      = GetSettingBool(S_SETTINGS_SECTION, "allowexittomenu",      false);

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
            else if (ShouldProtect())
            {
                Logger::Warn("[NoMoreAltF4] Manual abort triggered (hotkey 0x{:X}). Terminating.", m_ManualKillKey);
                KillProcess();
                return;
            }
        }
        m_ManualKillKeyPrevState = keyDown;
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
    // (the HTTP-body hook is the fallback) rather than crash.
    std::string s_EventStr;
    if ((m_LogHttpRequests || ShouldProtect())
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
    if (m_LogHttpRequests && th)
    {
        const auto* s_Data = static_cast<const char*>(th->m_buffer.data());
        const auto  s_Size = th->m_buffer.size();

        if (s_Data && s_Size > 0)
        {
            // IOI compresses some endpoints; this hook sees the raw buffer
            // before the game inflates it (gzip magic 1F 8B), so those bodies
            // would log as binary garbage.  Decompress a copy for display.
            bool s_Logged = false;

            if (s_Size >= 2 && static_cast<uint8_t>(s_Data[0]) == 0x1F
                && static_cast<uint8_t>(s_Data[1]) == 0x8B)
            {
                std::string s_Inflated;
                if (TryInflateForLog(reinterpret_cast<const uint8_t*>(s_Data), s_Size, s_Inflated))
                {
                    const bool s_Clipped = s_Inflated.size() > k_MaxLogBytes;
                    if (s_Clipped)
                        s_Inflated.resize(k_MaxLogBytes);
                    Logger::Info("[NoMoreAltF4] HTTP Response (gzip {} -> {} bytes): {}{}",
                        s_Size, s_Inflated.size(), s_Inflated, s_Clipped ? "..." : "");
                    s_Logged = true;
                }
            }

            if (!s_Logged)
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
            ImGui::SetTooltip("Press this key to instantly abort the mission.\nUse when spotted or about to fail.\n"
                              "Ignored while Alt or Win is held, so other tools'\ncombos (e.g. Alt+F9 recording) can't trigger it.");

        if (ImGui::SmallButton("None")) { m_ManualKillKey = 0;     SetSettingInt(S_SETTINGS_SECTION, "manualkillkey", m_ManualKillKey); }
        ImGui::SameLine();
        if (ImGui::SmallButton("F9")) { m_ManualKillKey = VK_F9;  SetSettingInt(S_SETTINGS_SECTION, "manualkillkey", m_ManualKillKey); }
        ImGui::SameLine();
        if (ImGui::SmallButton("F10")) { m_ManualKillKey = VK_F10; SetSettingInt(S_SETTINGS_SECTION, "manualkillkey", m_ManualKillKey); }
        ImGui::SameLine();
        if (ImGui::SmallButton("F11")) { m_ManualKillKey = VK_F11; SetSettingInt(S_SETTINGS_SECTION, "manualkillkey", m_ManualKillKey); }
        ImGui::SameLine();
        if (ImGui::SmallButton("F12")) { m_ManualKillKey = VK_F12; SetSettingInt(S_SETTINGS_SECTION, "manualkillkey", m_ManualKillKey); }

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
