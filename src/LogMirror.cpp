#include "LogMirror.h"

#include <Logging.h>

#include <Windows.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cwchar>
#include <iterator>
#include <mutex>
#include <string_view>
#include <vector>

namespace fs = std::filesystem;

namespace
{
    constexpr DWORD  k_PollMs        = 500;            // worker mirror interval
    constexpr size_t k_PollMaxBytes  = 64u << 20;      // per-poll read cap (64 MB)
    constexpr size_t k_KillMaxBytes  = 128u << 10;     // kill-path read cap (128 KB)
    constexpr size_t k_KeepSessions  = 30;             // archives kept in the folder
    constexpr DWORD  k_StopWaitMs    = 2000;           // shutdown wait for the worker
    constexpr auto   k_PollLockWait  = std::chrono::milliseconds(200);
    constexpr auto   k_KillLockWait  = std::chrono::milliseconds(20);
    constexpr auto   k_StopLockWait  = std::chrono::milliseconds(500);

    constexpr std::string_view k_Redacted    = "<redacted>";
    constexpr std::string_view k_RedactedJwt = "<redacted-jwt>";

    std::string ToUtf8(const fs::path& p_Path)
    {
        const std::wstring& s_Wide = p_Path.native();
        if (s_Wide.empty())
            return {};
        const int s_Len = WideCharToMultiByte(CP_UTF8, 0, s_Wide.data(), static_cast<int>(s_Wide.size()),
                                              nullptr, 0, nullptr, nullptr);
        std::string s_Out(static_cast<size_t>(s_Len), '\0');
        WideCharToMultiByte(CP_UTF8, 0, s_Wide.data(), static_cast<int>(s_Wide.size()),
                            s_Out.data(), s_Len, nullptr, nullptr);
        return s_Out;
    }

    bool IsBase64Url(char c)
    {
        return std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_';
    }

    bool WriteAll(HANDLE p_File, std::string_view p_Data)
    {
        while (!p_Data.empty())
        {
            const DWORD s_Chunk = static_cast<DWORD>(std::min<size_t>(p_Data.size(), 1u << 30));
            DWORD s_Written = 0;
            if (!WriteFile(p_File, p_Data.data(), s_Chunk, &s_Written, nullptr) || s_Written == 0)
                return false;
            p_Data.remove_prefix(s_Written);
        }
        return true;
    }
}

// =============================================================================
// Shared state (owned jointly by LogMirror and the worker thread)
// =============================================================================

struct LogMirror::State
{
    std::atomic<bool> Enabled{ true };
    std::atomic<bool> StopRequested{ false };

    HANDLE  Thread    = nullptr;
    HANDLE  WakeEvent = nullptr;
    HMODULE Module    = nullptr;   // our DLL, ref-counted for the worker

    // Immutable after Start().
    fs::path Source;
    fs::path Folder;
    fs::path SessionFile;

    std::timed_mutex Io;           // guards Out and Offset
    HANDLE   Out    = INVALID_HANDLE_VALUE;
    uint64_t Offset = 0;           // bytes of Source already mirrored

    ~State()
    {
        if (Out != INVALID_HANDLE_VALUE)
            CloseHandle(Out);
        if (WakeEvent)
            CloseHandle(WakeEvent);
        if (Thread)
            CloseHandle(Thread);
    }
};

namespace
{
    // Locates the live SDK log. The SDK writes it relative to the working
    // directory (observed in the game root when launched from Visual Studio /
    // Steam, and in Retail\ for some launch setups), so check the likely spots
    // and take whichever was written most recently — the active one.
    fs::path FindLiveLog()
    {
        std::vector<fs::path> s_Candidates;
        std::error_code s_Ec;

        const fs::path s_Cwd = fs::current_path(s_Ec);
        if (!s_Ec)
            s_Candidates.push_back(s_Cwd / L"ZHMModLoader.log");

        wchar_t s_Exe[MAX_PATH * 4] = {};
        const DWORD s_Len = GetModuleFileNameW(nullptr, s_Exe, static_cast<DWORD>(std::size(s_Exe)));
        if (s_Len > 0 && s_Len < std::size(s_Exe))
        {
            const fs::path s_ExeDir = fs::path(s_Exe).parent_path();
            s_Candidates.push_back(s_ExeDir / L"ZHMModLoader.log");
            s_Candidates.push_back(s_ExeDir.parent_path() / L"ZHMModLoader.log");
        }

        fs::path s_Best;
        fs::file_time_type s_BestTime{};
        for (const auto& s_Candidate : s_Candidates)
        {
            s_Ec.clear();
            const auto s_Time = fs::last_write_time(s_Candidate, s_Ec);
            if (s_Ec)
                continue;
            if (s_Best.empty() || s_Time > s_BestTime)
            {
                s_Best = s_Candidate;
                s_BestTime = s_Time;
            }
        }
        return s_Best;
    }

    // Deletes the oldest session-*.log files beyond p_Keep. Names embed a
    // sortable timestamp, so lexical order is chronological.
    void PruneOldArchives(const fs::path& p_Folder, const fs::path& p_Current, size_t p_Keep)
    {
        std::error_code s_Ec;
        std::vector<fs::path> s_Files;
        for (const auto& s_Entry : fs::directory_iterator(p_Folder, s_Ec))
        {
            const std::wstring s_Name = s_Entry.path().filename().native();
            if (s_Entry.is_regular_file(s_Ec) && s_Name.starts_with(L"session-") && s_Name.ends_with(L".log")
                && s_Entry.path() != p_Current)
                s_Files.push_back(s_Entry.path());
        }
        if (s_Files.size() < p_Keep)
            return;

        std::sort(s_Files.begin(), s_Files.end());
        // Keep (p_Keep - 1) old files plus the current session.
        const size_t s_Remove = s_Files.size() - (p_Keep - 1);
        for (size_t i = 0; i < s_Remove; ++i)
            fs::remove(s_Files[i], s_Ec);
    }

    // Appends source bytes [Offset, size) to the archive. Caller holds State::Io.
    // p_MaxBytes caps how much is read; a larger delta is noted and only the
    // newest p_MaxBytes (from a line boundary) are written.
    // p_CompleteLinesOnly holds back a trailing partial line (normal polling).
    void AppendDelta(LogMirror::State& p_State, size_t p_MaxBytes, bool p_CompleteLinesOnly)
    {
        if (p_State.Out == INVALID_HANDLE_VALUE || p_State.Source.empty())
            return;

        HANDLE s_In = CreateFileW(p_State.Source.c_str(), GENERIC_READ,
                                  FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                  nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (s_In == INVALID_HANDLE_VALUE)
            return;

        LARGE_INTEGER s_SizeLi{};
        if (!GetFileSizeEx(s_In, &s_SizeLi))
        {
            CloseHandle(s_In);
            return;
        }
        const uint64_t s_Size = static_cast<uint64_t>(s_SizeLi.QuadPart);

        std::string s_Prefix;
        if (s_Size < p_State.Offset)
        {
            // The live log shrank (truncated/replaced) — restart from its top.
            s_Prefix = "\n[NoMoreAltF4] ---- live log was truncated; mirroring restarts here ----\n";
            p_State.Offset = 0;
        }
        if (s_Size == p_State.Offset)
        {
            CloseHandle(s_In);
            if (!s_Prefix.empty())
                WriteAll(p_State.Out, s_Prefix);
            return;
        }

        uint64_t s_Start = p_State.Offset;
        bool s_Skipped = false;
        if (s_Size - s_Start > p_MaxBytes)
        {
            s_Start = s_Size - p_MaxBytes;
            s_Skipped = true;
        }

        std::string s_Buf(static_cast<size_t>(s_Size - s_Start), '\0');
        LARGE_INTEGER s_Pos{};
        s_Pos.QuadPart = static_cast<LONGLONG>(s_Start);
        size_t s_Read = 0;
        if (SetFilePointerEx(s_In, s_Pos, nullptr, FILE_BEGIN))
        {
            while (s_Read < s_Buf.size())
            {
                DWORD s_Got = 0;
                const DWORD s_Want = static_cast<DWORD>(std::min<size_t>(s_Buf.size() - s_Read, 1u << 30));
                if (!ReadFile(s_In, s_Buf.data() + s_Read, s_Want, &s_Got, nullptr) || s_Got == 0)
                    break;
                s_Read += s_Got;
            }
        }
        CloseHandle(s_In);
        s_Buf.resize(s_Read);
        if (s_Buf.empty())
            return;

        // When skipping, drop the partial first line so the archive stays readable.
        size_t s_Lead = 0;
        if (s_Skipped)
        {
            const size_t s_Nl = s_Buf.find('\n');
            s_Lead = (s_Nl == std::string::npos) ? 0 : s_Nl + 1;
            s_Prefix += "\n[NoMoreAltF4] ---- "
                + std::to_string(s_Start + s_Lead - p_State.Offset)
                + " bytes of the live log were skipped in this archive ----\n";
        }

        size_t s_End = s_Buf.size();
        if (p_CompleteLinesOnly)
        {
            const size_t s_LastNl = s_Buf.rfind('\n');
            s_End = (s_LastNl == std::string::npos || s_LastNl + 1 <= s_Lead) ? s_Lead : s_LastNl + 1;
            // A single giant line with no newline would stall forever — take it.
            if (s_End == s_Lead && s_Buf.size() >= p_MaxBytes)
                s_End = s_Buf.size();
        }
        if (s_End <= s_Lead && s_Prefix.empty())
            return;

        std::string s_Chunk = s_Buf.substr(s_Lead, s_End - s_Lead);
        LogMirror::Scrub(s_Chunk);

        if (WriteAll(p_State.Out, s_Prefix) && WriteAll(p_State.Out, s_Chunk))
            p_State.Offset = s_Start + s_End;
    }

    struct WorkerArgs
    {
        std::shared_ptr<LogMirror::State> State;
    };

    DWORD WINAPI WorkerMain(LPVOID p_Param)
    {
        auto* s_Args = static_cast<WorkerArgs*>(p_Param);
        HMODULE s_Module = s_Args->State->Module;
        {
            const std::shared_ptr<LogMirror::State> s_State = std::move(s_Args->State);
            delete s_Args;

            while (!s_State->StopRequested)
            {
                WaitForSingleObject(s_State->WakeEvent, k_PollMs);
                if (s_State->StopRequested)
                    break;
                if (!s_State->Enabled)
                    continue;

                std::unique_lock s_Lock(s_State->Io, std::defer_lock);
                if (!s_Lock.try_lock_for(k_PollLockWait))
                    continue;
                AppendDelta(*s_State, k_PollMaxBytes, true);
            }
        } // drop our State reference before releasing the module

        FreeLibraryAndExitThread(s_Module, 0);
    }
}

// =============================================================================
// Public API
// =============================================================================

LogMirror::~LogMirror()
{
    Stop();
}

void LogMirror::Start(bool p_Enabled)
{
    if (m_State)
        return;

    auto s_State = std::make_shared<State>();
    s_State->Enabled = p_Enabled;
    m_State = s_State;

    s_State->Source = FindLiveLog();
    if (s_State->Source.empty())
    {
        Logger::Warn("[NoMoreAltF4] Log archive: ZHMModLoader.log not found — this session's log will not be preserved.");
        return;
    }

    std::error_code s_Ec;
    const fs::path s_Folder = s_State->Source.parent_path() / L"NoMoreAltF4Logs";
    fs::create_directories(s_Folder, s_Ec);
    if (s_Ec)
    {
        Logger::Warn("[NoMoreAltF4] Log archive: cannot create '{}' ({}).", ToUtf8(s_Folder), s_Ec.message());
        return;
    }

    SYSTEMTIME s_Now{};
    GetLocalTime(&s_Now);
    wchar_t s_Name[64] = {};
    swprintf_s(s_Name, L"session-%04u%02u%02u-%02u%02u%02u.log",
               s_Now.wYear, s_Now.wMonth, s_Now.wDay, s_Now.wHour, s_Now.wMinute, s_Now.wSecond);
    const fs::path s_Session = s_Folder / s_Name;

    HANDLE s_Out = CreateFileW(s_Session.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_DELETE,
                               nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (s_Out == INVALID_HANDLE_VALUE)
    {
        Logger::Warn("[NoMoreAltF4] Log archive: cannot open '{}' (error {}).", ToUtf8(s_Session), GetLastError());
        return;
    }

    s_State->Folder = s_Folder;
    s_State->SessionFile = s_Session;
    s_State->Out = s_Out;

    PruneOldArchives(s_Folder, s_Session, k_KeepSessions);

    // Copy what the log already holds (SDK startup, other mods) right away.
    {
        std::lock_guard s_Lock(s_State->Io);
        AppendDelta(*s_State, k_PollMaxBytes, true);
    }

    // Pin our DLL for the worker's lifetime (released by FreeLibraryAndExitThread).
    HMODULE s_Module = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                            reinterpret_cast<LPCWSTR>(&WorkerMain), &s_Module))
    {
        Logger::Warn("[NoMoreAltF4] Log archive: cannot pin module (error {}); live mirroring disabled.", GetLastError());
        return;
    }
    s_State->Module = s_Module;
    s_State->WakeEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);

    auto* s_Args = new WorkerArgs{ s_State };
    s_State->Thread = s_State->WakeEvent
        ? CreateThread(nullptr, 0, &WorkerMain, s_Args, 0, nullptr)
        : nullptr;
    if (!s_State->Thread)
    {
        delete s_Args;
        FreeLibrary(s_Module);
        s_State->Module = nullptr;
        Logger::Warn("[NoMoreAltF4] Log archive: cannot start worker (error {}); live mirroring disabled.", GetLastError());
        return;
    }

    Logger::Info("[NoMoreAltF4] Session log archive: {}", ToUtf8(s_Session));
}

void LogMirror::Stop()
{
    const auto s_State = m_State;
    if (!s_State)
        return;
    m_State.reset();

    s_State->StopRequested = true;
    if (s_State->WakeEvent)
        SetEvent(s_State->WakeEvent);

    if (s_State->Thread)
        WaitForSingleObject(s_State->Thread, k_StopWaitMs); // bounded: never hang shutdown

    std::unique_lock s_Lock(s_State->Io, std::defer_lock);
    if (s_Lock.try_lock_for(k_StopLockWait))
    {
        if (s_State->Enabled)
            AppendDelta(*s_State, k_PollMaxBytes, false);
        if (s_State->Out != INVALID_HANDLE_VALUE)
        {
            CloseHandle(s_State->Out);
            s_State->Out = INVALID_HANDLE_VALUE;
        }
    }
    // Remaining handles close when the last State reference goes away (the
    // worker holds one until it exits, so a timed-out wait stays safe).
}

void LogMirror::SyncTail()
{
    const auto s_State = m_State;
    if (!s_State || !s_State->Enabled)
        return;

    std::unique_lock s_Lock(s_State->Io, std::defer_lock);
    if (!s_Lock.try_lock_for(k_KillLockWait))
        return; // the worker is mid-append; don't delay termination for it
    AppendDelta(*s_State, k_KillMaxBytes, false);
}

void LogMirror::SetEnabled(bool p_Enabled)
{
    if (m_State)
        m_State->Enabled = p_Enabled;
}

fs::path LogMirror::Folder() const
{
    return m_State ? m_State->Folder : fs::path{};
}

fs::path LogMirror::SessionFile() const
{
    return m_State ? m_State->SessionFile : fs::path{};
}

// =============================================================================
// Secret scrubbing
// =============================================================================

void LogMirror::Scrub(std::string& p_Text)
{
    std::string& t = p_Text;

    // 1. JWTs (OAuth access/refresh tokens): eyJ<header>.<payload>.<signature>
    for (size_t s_Pos = t.find("eyJ"); s_Pos != std::string::npos; s_Pos = t.find("eyJ", s_Pos))
    {
        if (s_Pos > 0 && IsBase64Url(t[s_Pos - 1]))
        {
            s_Pos += 3;
            continue;
        }
        size_t s_End = s_Pos;
        int s_Dots = 0;
        while (s_End < t.size() && (IsBase64Url(t[s_End]) || t[s_End] == '.'))
        {
            if (t[s_End] == '.')
                ++s_Dots;
            ++s_End;
        }
        if (s_Dots >= 2 && s_End - s_Pos >= 32)
        {
            t.replace(s_Pos, s_End - s_Pos, k_RedactedJwt);
            s_Pos += k_RedactedJwt.size();
        }
        else
        {
            s_Pos = s_End;
        }
    }

    // 2. Values of secret JSON keys (covers non-JWT tokens; handles escaped JSON).
    for (const std::string_view s_Key : { std::string_view("access_token"), std::string_view("refresh_token"),
                                          std::string_view("id_token") })
    {
        for (size_t s_Pos = t.find(s_Key); s_Pos != std::string::npos; s_Pos = t.find(s_Key, s_Pos))
        {
            size_t i = s_Pos + s_Key.size();
            auto Skip = [&](auto p_Pred) { while (i < t.size() && p_Pred(t[i])) ++i; };
            const auto IsQuote = [](char c) { return c == '"' || c == '\\'; };
            const auto IsSpace = [](char c) { return c == ' '; };

            Skip(IsQuote);
            Skip(IsSpace);
            if (i >= t.size() || t[i] != ':')
            {
                s_Pos = i;
                continue;
            }
            ++i;
            Skip(IsSpace);
            Skip(IsQuote);

            const size_t s_Value = i;
            while (i < t.size() && t[i] != '"' && t[i] != '\\' && t[i] != ',' && t[i] != '}' && t[i] != '\n')
                ++i;
            if (i > s_Value && t[s_Value] != '<')
            {
                t.replace(s_Value, i - s_Value, k_Redacted);
                i = s_Value + k_Redacted.size();
            }
            s_Pos = i;
        }
    }

    // 3. Authorization header values ("Bearer <token>"). "token_type":"bearer"
    //    has no trailing space, so it is not matched.
    for (const std::string_view s_Bearer : { std::string_view("Bearer "), std::string_view("bearer "),
                                             std::string_view("BEARER ") })
    {
        for (size_t s_Pos = t.find(s_Bearer); s_Pos != std::string::npos; s_Pos = t.find(s_Bearer, s_Pos))
        {
            const size_t s_Value = s_Pos + s_Bearer.size();
            size_t i = s_Value;
            while (i < t.size() && !std::isspace(static_cast<unsigned char>(t[i])) && t[i] != '"' && t[i] != '\\'
                   && t[i] != ',')
                ++i;
            if (i > s_Value && t[s_Value] != '<')
            {
                t.replace(s_Value, i - s_Value, k_Redacted);
                i = s_Value + k_Redacted.size();
            }
            s_Pos = i;
        }
    }
}
