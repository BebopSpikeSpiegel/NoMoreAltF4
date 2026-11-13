#pragma once

#include <cstddef>
#include <cstdint>

// =============================================================================
// EventBodyRewriter ("ebr") — Seamless failure protection
//
// The game uploads online events as plain JSON:
//     {"userId":"<uid>","values":[ <event>, ... ] ...}
// Each event carries the ContractSessionId (CSID) of the attempt it belongs to.
// When an attempt fails, its CSID is quarantined, and every event of that CSID
// is rewritten in the outgoing body into a harmless Hero_Health event of the
// SAME BYTE LENGTH. The server acknowledges it, never learns the attempt ended,
// and later closes it as an orphaned session, exactly as after an Alt-F4.
//
// This module has no SDK dependency and never allocates, so it can be replayed
// offline against captured bodies (tests/ebr_replay.cpp). All state is static:
// the DLL is pinned, so the quarantine outlives the plugin object.
// =============================================================================

namespace ebr
{
    constexpr size_t k_MaxToken = 96;   // longest id / token we accept (bytes)
    constexpr size_t k_Slots = 64;      // tracked attempts (contained ones are never evicted)
    constexpr size_t k_MaxEvents = 64;  // events per body (observed: 0-3)

    enum class Kind : uint8_t { Unknown, Other, Arcade, Elusive, Evergreen };
    enum class Result : uint8_t { NotEvents, Clean, Rewritten, Error };

    struct Report
    {
        Result result = Result::NotEvents;
        uint32_t events = 0;       // events in the body
        uint32_t neutralized = 0;  // events rewritten
        uint32_t ladder = 0;       // highest fit level used (0 = full health value)
        const char* error = "";    // static text when result == Error
        char names[200] = {};      // original names of the rewritten events
        char ids[200] = {};        // their Ids
    };

    // ---- attempt table (thread-safe) ----------------------------------------

    void NoteContractStart(const char* p_Csid, size_t p_Len, Kind p_Kind);
    Kind KindOf(const char* p_Csid, size_t p_Len);
    void NoteHealth(const char* p_Csid, size_t p_Len, const char* p_Token, size_t p_TokenLen);

    // Quarantines an attempt. p_Staked = a real penalty is at risk (Arcade/ET).
    // Returns false if the id is unusable or every slot is already quarantined.
    bool Arm(const char* p_Csid, size_t p_Len, bool p_Staked);
    bool IsQuarantined(const char* p_Csid, size_t p_Len, bool* p_Staked = nullptr);
    bool HasQuarantine();        // lock-free
    bool HasStakedQuarantine();  // lock-free: at least one contained attempt has a real penalty at risk
    uint32_t QuarantineCount();

    // Ids quarantined in an earlier run (loaded from disk; server-close check only).
    void AddPast(const char* p_Csid, size_t p_Len);
    bool HasPast();
    // Marks a past id as answered by the server. Returns true the first time
    // only. The id stays watched for the rest of this run, because the server
    // reports one close as two records that can arrive in separate responses.
    bool RetirePast(const char* p_Csid, size_t p_Len);

    // 0 = no quarantined id in the bytes, 1 = a test-only one, 2 = a staked one.
    int MentionsQuarantined(const char* p_Data, size_t p_Len);

    // ---- event-layer text (ZDynamicObject_ToString output, not valid JSON) ---

    bool ExtractCsid(const char* p_Text, size_t p_Len, const char** p_Csid, size_t* p_CsidLen);
    bool NameIs(const char* p_Text, size_t p_Len, const char* p_Name);
    Kind KindFromContractStart(const char* p_Text, size_t p_Len);
    // True when the event reports a failed attempt: a death event, or a
    // ContractFailed that is not a manual restart / replan / load (and not an
    // exit to the menu when p_AllowExitToMenu is set).
    bool IsFailureText(const char* p_Text, size_t p_Len, bool p_AllowExitToMenu);
    bool ExtractNumberAfter(const char* p_Text, size_t p_Len, const char* p_Key,
                            const char** p_Tok, size_t* p_TokLen);

    // ---- wire bodies --------------------------------------------------------

    bool LooksLikeEventsBody(const char* p_Data, size_t p_Len);

    // Rewrites, inside p_Buf (a PRIVATE copy of the body), every event of a
    // quarantined attempt. Never changes p_Len. On Error the buffer contents are
    // undefined and must not be sent.
    // Also an Error: a quarantined id that appears anywhere other than as the
    // ContractSessionId of an event (it could not be neutralized there).
    Result Neutralize(char* p_Buf, size_t p_Len, Report& p_Report);

    // Freelancer guard at the wire: true when the events body holds a CpdSet that
    // is not an event of a quarantined attempt and is not one of the allowed
    // 1-key display page flips ({"GearWall_ThematicIndex":<n>} / {"MyValue01":<n>}).
    // A body that does not parse counts as true when it mentions CpdSet at all.
    bool HasUnsafeCpdSet(const char* p_Data, size_t p_Len);

    // ---- server responses ---------------------------------------------------

    struct ServerClose
    {
        char csid[k_MaxToken] = {};
        char closeType[k_MaxToken] = {};
        bool orphaned = false;  // closed as an orphaned session (the Alt-F4 outcome)
        bool past = false;      // id came from an earlier run
    };

    // Finds server-made SegmentClosing / ContractFailed events for quarantined
    // (or past) attempts in a decoded response. Returns the number written.
    // The two records the server sends for one close count once when they agree.
    // A return value equal to p_MaxOut means the list may be incomplete.
    size_t FindServerCloses(const char* p_Text, size_t p_Len, ServerClose* p_Out, size_t p_MaxOut);

    // Test support: forgets everything.
    void ResetForTests();
}
