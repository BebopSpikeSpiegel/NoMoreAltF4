#pragma once

#include <filesystem>
#include <memory>
#include <string>

// =============================================================================
// LogMirror — preserves ZHMModLoader.log across game launches
//
// The SDK truncates ZHMModLoader.log every time the game starts, so the log of
// a session that ended in an auto-terminate, a crash or a freeze is lost on the
// next launch. LogMirror tails the live log from a background thread and
// appends every new chunk to a per-session archive:
//
//     <log dir>\NoMoreAltF4Logs\session-YYYYMMDD-HHMMSS.log
//
// Design constraints:
//  - The KILL PATH must stay fast. A detected failure is racing the game's
//    SaveEvents2 POSTs (the WinHTTP hook is observe-only — it cannot block
//    them), so KillProcess must never do a full multi-MB copy. Instead the
//    worker mirrors incrementally every 500 ms, and KillProcess calls
//    SyncTail(), which only appends the few KB written since the last poll
//    (size-capped, and it gives up rather than wait long for the lock).
//    Written data sits in the OS file cache, so it survives TerminateProcess.
//  - Archives are SCRUBBED: OAuth access/refresh tokens (JWTs) and bearer
//    header values are redacted so a session log is safe to share. Numeric
//    event tokens (the SaveEvents2 "SavedTokens" forensics) are kept.
//  - Worker state is heap-owned and shared with the thread, and the thread
//    holds a reference on this DLL (exits via FreeLibraryAndExitThread), so a
//    timed-out shutdown wait can never cause a use-after-free or a jump into
//    unmapped code.
// =============================================================================

class LogMirror
{
public:
    ~LogMirror();

    // Locates the live log, creates the session archive, prunes old archives
    // and starts the worker. Only the first call has an effect.
    void Start(bool p_Enabled);

    // Stops the worker (bounded wait) and appends the final tail.
    void Stop();

    // Fast, bounded tail append for the termination path.
    void SyncTail();

    void SetEnabled(bool p_Enabled);

    // Archive folder / current session file (empty if Start failed).
    std::filesystem::path Folder() const;
    std::filesystem::path SessionFile() const;

    // Redacts secrets in place. Public so it can be unit-tested.
    static void Scrub(std::string& p_Text);

    struct State;

private:
    std::shared_ptr<State> m_State;
};
