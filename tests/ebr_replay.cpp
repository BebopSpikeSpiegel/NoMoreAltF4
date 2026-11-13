// Offline replay test for src/EventBodyRewriter.
//
// Build (standalone, not part of the plugin target):
//   cl /std:c++20 /EHsc /utf-8 /W4 /O2 tests\ebr_replay.cpp src\EventBodyRewriter.cpp
// Run:
//   ebr_replay.exe <session log or folder> [more...]
//
// It feeds every "HTTP POST body:" line of the captured session logs through
// the rewriter and checks the invariants the plugin relies on.

#include "../src/EventBodyRewriter.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace
{
    int g_Failures = 0;

    void Fail(const std::string& p_What, const std::string& p_Detail = {})
    {
        if (++g_Failures <= 25)
            std::printf("FAIL: %s %s\n", p_What.c_str(), p_Detail.substr(0, 300).c_str());
    }

#define CHECK(cond, what) \
    do { if (!(cond)) Fail(what, #cond); } while (0)

    // ---- independent strict JSON validator (shares no code with the scanner) ----

    struct Json
    {
        const char* p;
        const char* end;
        int depth = 0;

        void Ws() { while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) ++p; }

        bool String()
        {
            if (p >= end || *p != '"') return false;
            ++p;
            while (p < end)
            {
                const unsigned char c = static_cast<unsigned char>(*p);
                if (c == '"') { ++p; return true; }
                if (c < 0x20) return false;
                if (c == '\\')
                {
                    if (++p >= end) return false;
                    if (!std::strchr("\"\\/bfnrtu", *p)) return false;
                    if (*p == 'u')
                    {
                        for (int i = 0; i < 4; ++i)
                            if (++p >= end || !std::isxdigit(static_cast<unsigned char>(*p))) return false;
                    }
                }
                ++p;
            }
            return false;
        }

        bool Number()
        {
            const char* s = p;
            if (p < end && *p == '-') ++p;
            if (p >= end || !std::isdigit(static_cast<unsigned char>(*p))) return false;
            while (p < end && std::isdigit(static_cast<unsigned char>(*p))) ++p;
            if (p < end && *p == '.')
            {
                ++p;
                if (p >= end || !std::isdigit(static_cast<unsigned char>(*p))) return false;
                while (p < end && std::isdigit(static_cast<unsigned char>(*p))) ++p;
            }
            if (p < end && (*p == 'e' || *p == 'E'))
            {
                ++p;
                if (p < end && (*p == '+' || *p == '-')) ++p;
                if (p >= end || !std::isdigit(static_cast<unsigned char>(*p))) return false;
                while (p < end && std::isdigit(static_cast<unsigned char>(*p))) ++p;
            }
            return p > s;
        }

        bool Literal(const char* p_Lit)
        {
            const size_t n = std::strlen(p_Lit);
            if (static_cast<size_t>(end - p) < n || std::memcmp(p, p_Lit, n) != 0) return false;
            p += n;
            return true;
        }

        bool Value()
        {
            if (++depth > 200) return false;
            Ws();
            bool ok = false;
            if (p >= end) ok = false;
            else if (*p == '"') ok = String();
            else if (*p == '{')
            {
                ++p; Ws();
                if (p < end && *p == '}') { ++p; ok = true; }
                else for (;;)
                {
                    Ws();
                    if (!String()) break;
                    Ws();
                    if (p >= end || *p != ':') break;
                    ++p;
                    if (!Value()) break;
                    Ws();
                    if (p < end && *p == ',') { ++p; continue; }
                    if (p < end && *p == '}') { ++p; ok = true; }
                    break;
                }
            }
            else if (*p == '[')
            {
                ++p; Ws();
                if (p < end && *p == ']') { ++p; ok = true; }
                else for (;;)
                {
                    if (!Value()) break;
                    Ws();
                    if (p < end && *p == ',') { ++p; continue; }
                    if (p < end && *p == ']') { ++p; ok = true; }
                    break;
                }
            }
            else if (*p == 't') ok = Literal("true");
            else if (*p == 'f') ok = Literal("false");
            else if (*p == 'n') ok = Literal("null");
            else ok = Number();
            --depth;
            return ok;
        }
    };

    bool IsValidJson(const std::string& p_Text)
    {
        Json j{ p_Text.data(), p_Text.data() + p_Text.size() };
        if (!j.Value()) return false;
        j.Ws();
        return j.p == j.end;
    }

    // Every string that follows p_Key in p_Text.
    std::vector<std::string> ValuesOf(const std::string& p_Text, const char* p_Key)
    {
        std::vector<std::string> s_Out;
        const size_t s_KeyLen = std::strlen(p_Key);
        for (size_t s_Pos = p_Text.find(p_Key); s_Pos != std::string::npos;
             s_Pos = p_Text.find(p_Key, s_Pos + 1))
        {
            const size_t s_Start = s_Pos + s_KeyLen;
            const size_t s_End = p_Text.find('"', s_Start);
            if (s_End == std::string::npos) break;
            s_Out.push_back(p_Text.substr(s_Start, s_End - s_Start));
        }
        return s_Out;
    }

    size_t Count(const std::string& p_Text, const char* p_Needle)
    {
        size_t n = 0;
        for (size_t p = p_Text.find(p_Needle); p != std::string::npos; p = p_Text.find(p_Needle, p + 1))
            ++n;
        return n;
    }

    struct Stats
    {
        size_t bodies = 0, eventBodies = 0, otherBodies = 0, events = 0;
        size_t rewrittenBodies = 0, rewrittenEvents = 0, failureBodies = 0;
        size_t ladder[4] = {};
        size_t eventLines = 0, failureTexts = 0, arcadeStarts = 0, elusiveStarts = 0, evergreenStarts = 0, loads = 0;
        size_t responses = 0, orphanCloses = 0, mergedCloses = 0;
        size_t cumulativeBodies = 0, cumulativeRewritten = 0;
        bool sampleShown = false;
    };

    void ReplayBody(const std::string& p_Body, Stats& p_Stats)
    {
        ++p_Stats.bodies;
        const bool s_Looks = ebr::LooksLikeEventsBody(p_Body.data(), p_Body.size());

        // --- A: nothing quarantined -> the body must come back untouched ---
        ebr::ResetForTests();
        {
            std::string s_Copy = p_Body;
            ebr::Report s_Report;
            const ebr::Result s_Result = ebr::Neutralize(s_Copy.data(), s_Copy.size(), s_Report);
            CHECK(s_Copy == p_Body, "A: body changed with an empty quarantine");
            if (s_Looks)
            {
                if (s_Result != ebr::Result::Clean) Fail("A: events body not Clean", p_Body);
            }
            else
            {
                CHECK(s_Result == ebr::Result::NotEvents, "A: non-events body not NotEvents");
                CHECK(Count(p_Body, "\"FailType\"") == 0, "A: FailType outside an events body");
            }
            if (!s_Looks)
            {
                ++p_Stats.otherBodies;
                return;
            }
            ++p_Stats.eventBodies;
            p_Stats.events += s_Report.events;
            CHECK(IsValidJson(p_Body), "A: original events body is not valid JSON");
        }

        // --- B: every attempt in the body quarantined -> all its events neutralized ---
        const auto s_Csids = ValuesOf(p_Body, "\"ContractSessionId\":\"");
        const auto s_Ids = ValuesOf(p_Body, "\"Id\":\"");
        if (s_Csids.empty())
            return;

        for (const auto& s_Csid : s_Csids)
            ebr::Arm(s_Csid.data(), s_Csid.size(), true);
        ebr::NoteHealth(s_Csids[0].data(), s_Csids[0].size(), "28.744013", 9);

        const bool s_IsFailure = Count(p_Body, "\"FailType\"") > 0;
        if (s_IsFailure)
            ++p_Stats.failureBodies;

        std::string s_Copy = p_Body;
        ebr::Report s_Report;
        const ebr::Result s_Result = ebr::Neutralize(s_Copy.data(), s_Copy.size(), s_Report);

        if (s_Result == ebr::Result::Error)
        {
            Fail(std::string("B: Error: ") + s_Report.error, p_Body);
            return;
        }

        CHECK(s_Copy.size() == p_Body.size(), "B: length changed");
        CHECK(ebr::MentionsQuarantined(s_Copy.data(), s_Copy.size()) == 2, "B: staked id not reported");

        if (s_Result == ebr::Result::Clean)
        {
            CHECK(s_Copy == p_Body, "B: Clean but bytes changed");
            CHECK(Count(p_Body, "\"Name\":\"Hero_Health\"") == s_Report.events,
                  "B: Clean although a quarantined non-health event exists");
            return;
        }

        ++p_Stats.rewrittenBodies;
        p_Stats.rewrittenEvents += s_Report.neutralized;
        ++p_Stats.ladder[s_Report.ladder < 4 ? s_Report.ladder : 3];

        if (!IsValidJson(s_Copy)) Fail("B: rewritten body is not valid JSON", s_Copy);
        CHECK(Count(s_Copy, "\"Name\":\"ContractFailed\"") == 0, "B: ContractFailed survived");
        CHECK(Count(s_Copy, "\"FailType\"") == 0, "B: FailType survived");
        // Nameless, valueless events are left as they are (see IsInertEvent).
        CHECK(Count(s_Copy, "\"Name\":\"Hero_Health\"") + Count(s_Copy, "\"Name\":\"\"") == s_Report.events,
              "B: not every event is Hero_Health");
        CHECK(ValuesOf(s_Copy, "\"ContractSessionId\":\"") == s_Csids, "B: session ids changed");
        CHECK(ValuesOf(s_Copy, "\"Id\":\"").size() >= s_Report.events, "B: an Id went missing");
        for (const auto& s_Id : s_Ids)
            if (s_Id.size() == 36 && s_Copy.find(s_Id) == std::string::npos && p_Body.find("\"Id\":\"" + s_Id + "\"}") != std::string::npos)
                Fail("B: event Id lost", s_Id);

        // The tail of the root object (lastEventTicks etc.) must be untouched.
        const size_t s_Tail = p_Body.rfind(']');
        CHECK(s_Tail != std::string::npos && s_Copy.compare(s_Tail, std::string::npos, p_Body, s_Tail, std::string::npos) == 0,
              "B: bytes after the values array changed");

        // A second pass over already-rewritten bytes must be a no-op (re-sends).
        std::string s_Again = s_Copy;
        ebr::Report s_Report2;
        CHECK(ebr::Neutralize(s_Again.data(), s_Again.size(), s_Report2) == ebr::Result::Clean && s_Again == s_Copy,
              "B: second pass changed the body");

        if (s_IsFailure && !p_Stats.sampleShown)
        {
            p_Stats.sampleShown = true;
            const size_t s_At = p_Body.find("\"Name\":\"ContractFailed\"");
            const size_t s_From = p_Body.rfind('{', s_At);
            std::printf("\nSample failure event (trimmed to 330 bytes)\n  before: %s\n  after : %s\n\n",
                        p_Body.substr(s_From, 330).c_str(), s_Copy.substr(s_From, 330).c_str());
        }
    }

    // In live play an attempt stays quarantined until the game exits, so every
    // later body is checked against it. Replays the body with the most recent
    // attempts of the same log all contained: nothing the game really sent may
    // be refused (a refusal terminates the game).
    void ReplayCumulative(const std::string& p_Body, std::vector<std::string>& p_Recent, Stats& p_Stats)
    {
        if (!ebr::LooksLikeEventsBody(p_Body.data(), p_Body.size()))
            return;

        ebr::ResetForTests();
        const size_t s_First = p_Recent.size() > ebr::k_Slots ? p_Recent.size() - ebr::k_Slots : 0;
        for (size_t i = s_First; i < p_Recent.size(); ++i)
            ebr::Arm(p_Recent[i].data(), p_Recent[i].size(), true);

        std::string s_Copy = p_Body;
        ebr::Report s_Report;
        const ebr::Result s_Result = ebr::Neutralize(s_Copy.data(), s_Copy.size(), s_Report);
        ++p_Stats.cumulativeBodies;
        if (s_Result == ebr::Result::Error)
            Fail(std::string("cumulative: Error: ") + s_Report.error, p_Body);
        else if (s_Result == ebr::Result::Rewritten)
            ++p_Stats.cumulativeRewritten;

        for (const auto& s_Csid : ValuesOf(p_Body, "\"ContractSessionId\":\""))
            if (!s_Csid.empty() && std::find(p_Recent.begin(), p_Recent.end(), s_Csid) == p_Recent.end())
                p_Recent.push_back(s_Csid);
    }

    void ReplayEventText(const std::string& p_Text, Stats& p_Stats)
    {
        ++p_Stats.eventLines;
        const char* s_Csid = nullptr;
        size_t s_CsidLen = 0;
        const bool s_HasCsid = ebr::ExtractCsid(p_Text.data(), p_Text.size(), &s_Csid, &s_CsidLen);

        if (ebr::IsFailureText(p_Text.data(), p_Text.size(), false))
        {
            ++p_Stats.failureTexts;
            CHECK(s_HasCsid, "event layer: failure event without a session id");
        }

        const bool s_Start = ebr::NameIs(p_Text.data(), p_Text.size(), "ContractStart");
        const bool s_Load = ebr::NameIs(p_Text.data(), p_Text.size(), "ContractLoad");
        if (s_Start || s_Load)
        {
            // The plugin learns an attempt's contract type from these two events.
            const ebr::Kind s_Kind = ebr::KindFromContractStart(p_Text.data(), p_Text.size());
            CHECK(s_HasCsid, "event layer: ContractStart/ContractLoad without a session id");
            CHECK(s_Kind != ebr::Kind::Unknown, "event layer: ContractStart/ContractLoad without a contract type");
            if (s_Load)
                ++p_Stats.loads;

            if (s_HasCsid)
            {
                ebr::ResetForTests();
                ebr::NoteContractStart(s_Csid, s_CsidLen, s_Kind);
                CHECK(ebr::KindOf(s_Csid, s_CsidLen) == s_Kind, "event layer: contract type not stored");
            }
        }

        if (s_Start)
        {
            switch (ebr::KindFromContractStart(p_Text.data(), p_Text.size()))
            {
                case ebr::Kind::Arcade: ++p_Stats.arcadeStarts; break;
                case ebr::Kind::Elusive: ++p_Stats.elusiveStarts; break;
                case ebr::Kind::Evergreen: ++p_Stats.evergreenStarts; break;
                default: break;
            }
        }
    }

    void ReplayResponse(const std::string& p_Text, Stats& p_Stats)
    {
        if (p_Text.find("OrphanedSession") == std::string::npos)
            return;
        ++p_Stats.responses;

        ebr::ResetForTests();
        for (const auto& s_Csid : ValuesOf(p_Text, "\"ContractSessionId\":\""))
            ebr::AddPast(s_Csid.data(), s_Csid.size());

        ebr::ServerClose s_Closes[64];
        const size_t s_Found = ebr::FindServerCloses(p_Text.data(), p_Text.size(), s_Closes, 64);
        CHECK(s_Found < 64, "response: more closes than the plugin checks at once");
        bool s_Orphan = false;
        for (size_t i = 0; i < s_Found; ++i)
        {
            CHECK(s_Closes[i].past, "response: close not marked past");
            s_Orphan = s_Orphan || s_Closes[i].orphaned;

            // One verdict per attempt: agreeing records are merged, and in the
            // captured logs the two records of a close never disagree (a
            // disagreement would raise a false ALARM in the plugin).
            for (size_t j = 0; j < i; ++j)
                CHECK(std::strcmp(s_Closes[i].csid, s_Closes[j].csid) != 0, "response: two verdicts for one attempt");
        }
        CHECK(s_Orphan, "response: orphan close not found");
        if (s_Orphan)
            ++p_Stats.orphanCloses;

        // Raw records naming an orphaned close, against the verdicts reported.
        const size_t s_Raw = Count(p_Text, "\"CloseType\":\"ContractFailed:OrphanedSession\"")
            + Count(p_Text, "\"FailType\":\"OrphanedSession\"");
        size_t s_OrphanVerdicts = 0;
        for (size_t i = 0; i < s_Found; ++i)
            s_OrphanVerdicts += s_Closes[i].orphaned ? 1 : 0;
        if (s_Raw > s_OrphanVerdicts)
            ++p_Stats.mergedCloses;
    }

    void ReplayFile(const std::filesystem::path& p_Path, Stats& p_Stats)
    {
        std::ifstream s_In(p_Path, std::ios::binary);
        if (!s_In)
        {
            std::printf("cannot open %s\n", p_Path.string().c_str());
            return;
        }

        constexpr char s_BodyTag[] = "[NoMoreAltF4] HTTP POST body: ";
        constexpr char s_EventTag[] = "payload=";
        constexpr char s_ResponseTag[] = "[NoMoreAltF4] HTTP Response (";

        std::vector<std::string> s_Recent;  // attempts seen so far in this log, oldest first
        std::string s_Line;
        while (std::getline(s_In, s_Line))
        {
            // A last line without its newline was cut off when the game closed
            // (raw loader-log copies end mid-write after a kill): not a real record.
            if (s_In.eof())
                break;

            if (!s_Line.empty() && s_Line.back() == '\r')
                s_Line.pop_back();

            if (const size_t s_Pos = s_Line.find(s_BodyTag); s_Pos != std::string::npos)
            {
                const std::string s_Body = s_Line.substr(s_Pos + sizeof(s_BodyTag) - 1);
                ReplayBody(s_Body, p_Stats);
                ReplayCumulative(s_Body, s_Recent, p_Stats);
            }
            else if (s_Line.find("[NoMoreAltF4] Online event sent: index=") != std::string::npos)
            {
                if (const size_t s_Pos = s_Line.find(s_EventTag); s_Pos != std::string::npos)
                    ReplayEventText(s_Line.substr(s_Pos + sizeof(s_EventTag) - 1), p_Stats);
            }
            else if (const size_t s_Pos = s_Line.find(s_ResponseTag); s_Pos != std::string::npos)
            {
                if (const size_t s_Colon = s_Line.find("): ", s_Pos); s_Colon != std::string::npos)
                    ReplayResponse(s_Line.substr(s_Colon + 3), p_Stats);
            }
        }
    }

    // ---- hand-made cases ----

    const std::string k_Csid = "2516080689509182607-aaaaaaaa-bbbb-4ccc-8ddd-eeeeeeeeeeee";
    const std::string k_Common =
        "\"ContractSessionId\":\"" + k_Csid + "\",\"ContractId\":\"a562b31c-7b80-4cbb-88e1-6980ed6824ea\"";
    const std::string k_Trailer =
        "\"UserId\":\"00000000-0000-4000-8000-000000000001\",\"SessionId\":\"76561190000000000-4172822371\","
        "\"Origin\":\"gameclient\",\"Id\":\"dfcccb6e-e6dc-45fb-96eb-46cc0c3cf87b\"}";

    std::string Wrap(const std::string& p_Events)
    {
        return "{\"userId\":\"00000000-0000-4000-8000-000000000001\",\"values\":[" + p_Events
            + "],\"lastEventTicks\":\"639298286490817449\",\"clienttype\":\"gameclient\",\"lastPushDt\":\"0\"}";
    }

    ebr::Result Run(std::string& p_Body, ebr::Report& p_Report, const char* p_Health = nullptr)
    {
        ebr::ResetForTests();
        ebr::Arm(k_Csid.data(), k_Csid.size(), false);
        if (p_Health)
            ebr::NoteHealth(k_Csid.data(), k_Csid.size(), p_Health, std::strlen(p_Health));
        return ebr::Neutralize(p_Body.data(), p_Body.size(), p_Report);
    }

    void Synthetic()
    {
        ebr::Report s_Report;

        // Real 3.290 death event.
        const std::string s_Death = Wrap("{\"Name\":\"ContractFailed\"," + k_Common
            + ",\"Value\":{\"FailType\":\"HitmanDead\",\"Reasons\":[\"Agent \\\"47\\\" down\\/out\"]},"
              "\"XboxGameMode\":0.000000,\"XboxDifficulty\":0.000000,\"Timestamp\":79.174438," + k_Trailer);
        CHECK(IsValidJson(s_Death), "synthetic: death body invalid");

        // Freelancer guard at the wire.
        {
            ebr::ResetForTests();
            ebr::Arm(k_Csid.data(), k_Csid.size(), true);
            const std::string s_Other = "\"ContractSessionId\":\"2516077703554352703-99999999-8888-4777-8666-555555555555\","
                                        "\"ContractId\":\"f8ec92c2-4fa2-471e-ae08-545480c746ee\"";
            auto Cpd = [&](const std::string& p_Common, const std::string& p_Value) {
                return "{\"Name\":\"CpdSet\"," + p_Common + ",\"Value\":" + p_Value + ",\"Timestamp\":1.0," + k_Trailer;
            };
            const std::string s_Flip = Wrap(Cpd(s_Other, "{\"GearWall_ThematicIndex\":2.000000}"));
            const std::string s_Flip2 = Wrap(Cpd(s_Other, "{\"MyValue01\":1.000000}"));
            const std::string s_Reset = Wrap(Cpd(s_Other, "{\"AnyActiveCampaign\":false,\"CPD_CampaignStep\":0.000000}"));
            const std::string s_OneKey = Wrap(Cpd(s_Other, "{\"MyMoney\":0.000000}"));
            const std::string s_Contained = Wrap(Cpd(k_Common, "{\"InventoryItems\":[],\"MyMoney\":9373.000000}"));
            const std::string s_NoCpd = Wrap("{\"Name\":\"Hero_Health\"," + s_Other + ",\"Value\":33.000000," + k_Trailer);
            const std::string s_Broken = "{\"userId\":\"u\",\"values\":[{\"Name\":\"CpdSet\",";
            CHECK(!ebr::HasUnsafeCpdSet(s_Flip.data(), s_Flip.size()), "guard: page flip blocked");
            CHECK(!ebr::HasUnsafeCpdSet(s_Flip2.data(), s_Flip2.size()), "guard: second page flip blocked");
            CHECK(ebr::HasUnsafeCpdSet(s_Reset.data(), s_Reset.size()), "guard: campaign reset passed");
            CHECK(ebr::HasUnsafeCpdSet(s_OneKey.data(), s_OneKey.size()), "guard: 1-key campaign field passed");
            CHECK(!ebr::HasUnsafeCpdSet(s_Contained.data(), s_Contained.size()), "guard: contained attempt's CpdSet blocked");
            CHECK(!ebr::HasUnsafeCpdSet(s_NoCpd.data(), s_NoCpd.size()), "guard: body without CpdSet blocked");
            CHECK(ebr::HasUnsafeCpdSet(s_Broken.data(), s_Broken.size()), "guard: unreadable CpdSet body passed");
        }

        // A nameless, valueless event of a contained attempt is left as it is
        // (real one: Freelancer showdown, 2026-11-13), alone or next to one that
        // is rewritten.
        {
            const std::string s_Inert = "{\"Timestamp\":806.490234,\"Name\":\"\"," + k_Common + ",\"Value\":\"\"," + k_Trailer;
            const std::string s_Counter = "{\"Timestamp\":806.490234,\"Name\":\"GunslingerUpdate\"," + k_Common
                + ",\"Value\":{\"Pistols\":0}," + k_Trailer;
            std::string s_Alone = Wrap(s_Inert);
            const std::string s_AloneOrig = s_Alone;
            CHECK(Run(s_Alone, s_Report) == ebr::Result::Clean && s_Alone == s_AloneOrig, "synthetic: inert event changed");
            std::string s_Pair = Wrap(s_Counter + "," + s_Inert);
            const size_t s_PairLen = s_Pair.size();
            CHECK(Run(s_Pair, s_Report) == ebr::Result::Rewritten && s_Report.neutralized == 1 && s_Pair.size() == s_PairLen
                      && IsValidJson(s_Pair) && s_Pair.find("\"Name\":\"\"") != std::string::npos
                      && s_Pair.find("GunslingerUpdate") == std::string::npos,
                  "synthetic: inert event next to a rewritten one");
        }

        {
            std::string s_Body = s_Death;
            CHECK(Run(s_Body, s_Report, "28.744013") == ebr::Result::Rewritten, "synthetic: death not rewritten");
            CHECK(s_Body.size() == s_Death.size() && IsValidJson(s_Body), "synthetic: death result invalid");
            CHECK(s_Body.find("\"Value\":28.744013,") != std::string::npos, "synthetic: remembered health not used");
            CHECK(s_Body.find("\"Timestamp\":79.174438,\"Name\":\"Hero_Health\"") != std::string::npos, "synthetic: shape");
            CHECK(s_Report.ladder == 0 && std::strcmp(s_Report.names, "ContractFailed") == 0, "synthetic: report");
            CHECK(ebr::MentionsQuarantined(s_Body.data(), s_Body.size()) == 1, "synthetic: test-only level");
        }

        // Tightest real case: the 3.280 Freelancer death event (2 bytes to spare).
        const std::string s_Wounded = Wrap("{\"Timestamp\":1970.123456,\"Name\":\"MissionWounded_Event\"," + k_Common
            + ",\"Value\":\"\"," + k_Trailer);
        {
            std::string s_Body = s_Wounded;
            CHECK(Run(s_Body, s_Report) == ebr::Result::Rewritten && s_Report.ladder == 0, "synthetic: wounded");
            CHECK(IsValidJson(s_Body) && s_Body.find("\"Value\":33.000000,") != std::string::npos, "synthetic: wounded result");
        }

        // Shortest shape seen on the wire: 8-character name, empty value.
        for (const char* s_Name : { "Caught4k", "BlueEgg", "X" })
        {
            const std::string s_Short = Wrap(std::string("{\"Timestamp\":12.345678,\"Name\":\"") + s_Name + "\","
                + k_Common + ",\"Value\":\"\"," + k_Trailer);
            std::string s_Body = s_Short;
            const ebr::Result s_Result = Run(s_Body, s_Report);
            if (std::strlen(s_Name) >= 7)
            {
                CHECK(s_Result == ebr::Result::Rewritten && IsValidJson(s_Body), "synthetic: short event did not fit");
                CHECK(s_Report.ladder >= 2, "synthetic: short event should need the ladder");
                CHECK(s_Body.size() == s_Short.size(), "synthetic: short length");
            }
            else
            {
                // One-character name: the Timestamp fraction has to go too.
                CHECK(s_Result == ebr::Result::Rewritten ? IsValidJson(s_Body) : s_Result == ebr::Result::Error,
                      "synthetic: tiny event");
            }
        }

        // Mixed body: one event of another attempt must stay byte-identical.
        {
            const std::string s_Other = "{\"Timestamp\":5.000000,\"Name\":\"Spotted\",\"ContractSessionId\":\"1111111111111111111-aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee\","
                "\"ContractId\":\"x\",\"Value\":[\"a\",{\"\":1}],\"UserId\":\"u\",\"SessionId\":\"s\",\"Origin\":\"gameclient\",\"Id\":\"i\"}";
            const std::string s_Mixed = Wrap(s_Other + ",{\"Name\":\"ContractFailed\"," + k_Common
                + ",\"Value\":{\"FailType\":\"PrimaryObjective\",\"Reasons\":[\"$loc X\"]},\"Timestamp\":9.5," + k_Trailer);
            std::string s_Body = s_Mixed;
            CHECK(Run(s_Body, s_Report) == ebr::Result::Rewritten && s_Report.neutralized == 1 && s_Report.events == 2,
                  "synthetic: mixed");
            CHECK(s_Body.find(s_Other) != std::string::npos && IsValidJson(s_Body), "synthetic: other attempt touched");
        }

        // Malformed input must never be rewritten.
        {
            std::string s_Dup = Wrap("{\"Name\":\"ContractFailed\",\"Name\":\"Hero_Health\"," + k_Common
                + ",\"Value\":{},\"Timestamp\":1.0," + k_Trailer);
            CHECK(Run(s_Dup, s_Report) == ebr::Result::Error, "synthetic: duplicate key accepted");

            std::string s_Deep = Wrap("{\"Name\":\"ContractFailed\"," + k_Common + ",\"Value\":" + std::string(70, '[')
                + std::string(70, ']') + ",\"Timestamp\":1.0," + k_Trailer);
            CHECK(Run(s_Deep, s_Report) == ebr::Result::Error, "synthetic: depth cap");

            std::string s_NoTs = Wrap("{\"Name\":\"ContractFailed\"," + k_Common + ",\"Value\":{}," + k_Trailer);
            CHECK(Run(s_NoTs, s_Report) == ebr::Result::Error, "synthetic: missing Timestamp accepted");

            std::string s_Garbage = Wrap("{\"Name\":\"ContractFailed\"," + k_Common + ",\"Value\":{},\"Timestamp\":1.0," + k_Trailer) + "x";
            CHECK(Run(s_Garbage, s_Report) == ebr::Result::Error, "synthetic: trailing garbage accepted");
        }

        // Truncation at every length: never Rewritten, never a crash.
        for (size_t s_Len = 0; s_Len < s_Death.size(); ++s_Len)
        {
            std::string s_Body = s_Death.substr(0, s_Len);
            const ebr::Result s_Result = Run(s_Body, s_Report);
            if (s_Result == ebr::Result::Rewritten || s_Result == ebr::Result::Clean)
                Fail("synthetic: truncated body accepted at length", std::to_string(s_Len));
        }

        // Pseudo-random bytes after the prefix.
        uint32_t s_Seed = 12345;
        for (int s_Round = 0; s_Round < 20000; ++s_Round)
        {
            std::string s_Body = s_Death;
            for (int k = 0; k < 4; ++k)
            {
                s_Seed = s_Seed * 1664525u + 1013904223u;
                const size_t s_At = (s_Seed >> 8) % s_Body.size();
                s_Seed = s_Seed * 1664525u + 1013904223u;
                s_Body[s_At] = "{}[]\",:\\ 0a"[(s_Seed >> 8) % 11];
            }
            const std::string s_Before = s_Body;
            const ebr::Result s_Result = Run(s_Body, s_Report);
            if (s_Result == ebr::Result::Rewritten)
            {
                CHECK(s_Body.size() == s_Before.size(), "fuzz: length changed");
                CHECK(s_Body.find("\"Name\":\"ContractFailed\",\"ContractSessionId\":\"" + k_Csid) == std::string::npos,
                      "fuzz: quarantined failure survived");
            }
            else if (s_Result == ebr::Result::Clean || s_Result == ebr::Result::NotEvents)
            {
                CHECK(s_Body == s_Before, "fuzz: untouched result but bytes changed");
            }
        }

        // A contained id anywhere other than an event's own ContractSessionId
        // cannot be neutralized there, so the body must be refused.
        {
            const std::string s_Health = "{\"Timestamp\":5.000000,\"Name\":\"Hero_Health\"," + k_Common
                + ",\"Value\":33.000000," + k_Trailer;
            const std::string s_OtherHead = "{\"Timestamp\":6.000000,\"Name\":\"Spotted\","
                "\"ContractSessionId\":\"1111111111111111111-aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee\",\"ContractId\":\"x\",";
            const std::string s_OtherTail = "\"UserId\":\"u\",\"SessionId\":\"s\",\"Origin\":\"gameclient\",\"Id\":\"i\"}";

            // Already-harmless events of a contained attempt pass untouched.
            std::string s_Plain = Wrap(s_Health + "," + s_Health);
            const std::string s_PlainBefore = s_Plain;
            CHECK(Run(s_Plain, s_Report) == ebr::Result::Clean && s_Plain == s_PlainBefore, "stray id: plain health body");

            // The id nested in another attempt's Value.
            std::string s_Nested = Wrap(s_OtherHead + "\"Value\":{\"Previous\":\"" + k_Csid + "\"}," + s_OtherTail);
            CHECK(IsValidJson(s_Nested), "stray id: nested case invalid");
            CHECK(Run(s_Nested, s_Report) == ebr::Result::Error, "stray id: nested in a Value was accepted");
            CHECK(std::strstr(s_Report.error, "outside an event") != nullptr, "stray id: refused for another reason");

            // The id under a differently named key of an event.
            std::string s_Renamed = Wrap("{\"Timestamp\":7.000000,\"Name\":\"ContractFailed\",\"contractSessionId\":\"" + k_Csid
                + "\",\"ContractId\":\"x\",\"Value\":{\"FailType\":\"HitmanDead\"}," + s_OtherTail);
            CHECK(IsValidJson(s_Renamed), "stray id: renamed case invalid");
            CHECK(Run(s_Renamed, s_Report) == ebr::Result::Error, "stray id: renamed key was accepted");
            CHECK(std::strstr(s_Report.error, "outside an event") != nullptr, "stray id: refused for another reason");

            // The id in a root-level key after the values array.
            std::string s_Root = "{\"userId\":\"u1234567\",\"values\":[" + s_Health + "],\"lastSession\":\"" + k_Csid + "\"}";
            CHECK(IsValidJson(s_Root), "stray id: root case invalid");
            CHECK(Run(s_Root, s_Report) == ebr::Result::Error, "stray id: root-level key was accepted");
            CHECK(std::strstr(s_Report.error, "outside an event") != nullptr, "stray id: refused for another reason");

            // A real failure plus a stray mention: rewritten, then still refused.
            std::string s_Both = Wrap("{\"Name\":\"ContractFailed\"," + k_Common
                + ",\"Value\":{\"FailType\":\"HitmanDead\",\"Reasons\":[\"x\"]},\"Timestamp\":79.174438," + k_Trailer
                + "," + s_OtherHead + "\"Value\":\"" + k_Csid + "\"," + s_OtherTail);
            CHECK(IsValidJson(s_Both), "stray id: mixed case invalid");
            CHECK(Run(s_Both, s_Report) == ebr::Result::Error, "stray id: mention next to a rewritten event was accepted");
            CHECK(std::strstr(s_Report.error, "outside an event") != nullptr, "stray id: refused for another reason");
        }

        // Table behaviour.
        ebr::ResetForTests();
        CHECK(!ebr::HasQuarantine() && !ebr::Arm("bad\"id", 6, true), "table: bad id armed");
        CHECK(!ebr::HasStakedQuarantine(), "table: staked without any attempt");
        for (size_t i = 0; i < ebr::k_Slots; ++i)
        {
            const std::string s_Id = "session-" + std::to_string(1000 + i);
            CHECK(ebr::Arm(s_Id.data(), s_Id.size(), i % 2 == 0), "table: arm");
        }
        CHECK(ebr::QuarantineCount() == ebr::k_Slots && !ebr::Arm("session-9999", 12, true),
              "table: an attempt beyond the capacity was armed");
        CHECK(ebr::IsQuarantined("session-1003", 12) && !ebr::IsQuarantined("session-2003", 12), "table: lookup");
        CHECK(ebr::HasStakedQuarantine(), "table: staked attempts not reported");

        // A full table of contained attempts cannot learn a new attempt's type.
        ebr::NoteContractStart("session-3000", 12, ebr::Kind::Arcade);
        CHECK(ebr::KindOf("session-3000", 12) == ebr::Kind::Unknown, "table: kind stored in a full table");

        // Stakes: test-only first, then raised.
        ebr::ResetForTests();
        ebr::NoteContractStart(k_Csid.data(), k_Csid.size(), ebr::Kind::Arcade);
        CHECK(ebr::KindOf(k_Csid.data(), k_Csid.size()) == ebr::Kind::Arcade, "table: kind");
        CHECK(ebr::KindOf("session-4000", 12) == ebr::Kind::Unknown, "table: kind of an unseen attempt");
        CHECK(ebr::Arm(k_Csid.data(), k_Csid.size(), false) && ebr::HasQuarantine() && !ebr::HasStakedQuarantine(),
              "table: test-only attempt counted as staked");
        CHECK(ebr::Arm(k_Csid.data(), k_Csid.size(), true) && ebr::HasStakedQuarantine() && ebr::QuarantineCount() == 1,
              "table: raising the stakes");

        // Server closes: the two records of one close are merged; a record that
        // disagrees is kept; a past id is retired exactly once.
        {
            const std::string s_PastId = "2516080689509182607-11111111-2222-3333-4444-555555555555";
            const auto s_Record = [](const char* p_Name, const std::string& p_Id, const char* p_Key, const char* p_Type) {
                return std::string("{\"Name\":\"") + p_Name + "\",\"ContractSessionId\":\"" + p_Id + "\",\"Value\":{\"" + p_Key
                    + "\":\"" + p_Type + "\"},\"IsReplicated\":false}";
            };

            ebr::ResetForTests();
            ebr::AddPast(s_PastId.data(), s_PastId.size());
            ebr::Arm(k_Csid.data(), k_Csid.size(), true);

            const std::string s_Pair = "[" + s_Record("SegmentClosing", s_PastId, "CloseType", "ContractFailed:OrphanedSession")
                + "," + s_Record("ContractFailed", s_PastId, "FailType", "OrphanedSession")
                + "," + s_Record("SegmentClosing", k_Csid, "CloseType", "GameRestart") + "]";

            ebr::ServerClose s_Closes[8];
            size_t s_Found = ebr::FindServerCloses(s_Pair.data(), s_Pair.size(), s_Closes, 8);
            CHECK(s_Found == 2, "closes: an agreeing pair was not merged");
            bool s_SawPast = false, s_SawLive = false;
            for (size_t i = 0; i < s_Found; ++i)
            {
                if (s_PastId == s_Closes[i].csid)
                    s_SawPast = s_Closes[i].past && s_Closes[i].orphaned;
                if (k_Csid == s_Closes[i].csid)
                    s_SawLive = !s_Closes[i].past && !s_Closes[i].orphaned
                        && std::strcmp(s_Closes[i].closeType, "GameRestart") == 0;
            }
            CHECK(s_SawPast && s_SawLive, "closes: wrong verdicts");

            const std::string s_Split = "[" + s_Record("SegmentClosing", s_PastId, "CloseType", "ContractFailed:OrphanedSession")
                + "," + s_Record("ContractFailed", s_PastId, "FailType", "HitmanDead") + "]";
            s_Found = ebr::FindServerCloses(s_Split.data(), s_Split.size(), s_Closes, 8);
            CHECK(s_Found == 2 && s_Closes[0].orphaned != s_Closes[1].orphaned, "closes: a disagreeing record was dropped");

            CHECK(ebr::FindServerCloses(s_Pair.data(), s_Pair.size(), s_Closes, 1) == 1, "closes: output limit ignored");

            CHECK(ebr::RetirePast(s_PastId.data(), s_PastId.size()), "closes: first retire");
            CHECK(!ebr::RetirePast(s_PastId.data(), s_PastId.size()), "closes: retired twice");
            CHECK(!ebr::RetirePast(k_Csid.data(), k_Csid.size()), "closes: retired an id that is not a past one");
            CHECK(ebr::HasPast() && ebr::FindServerCloses(s_Pair.data(), s_Pair.size(), s_Closes, 8) == 2,
                  "closes: a retired id must stay watched in this run");
        }

        // Event-layer helpers.
        const std::string s_Restart = "{\"Name\":\"ContractFailed\"," + k_Common + ",\"Value\":\"Contract ended manually: OnRestartLevel\"}";
        const std::string s_Exit = "{\"Name\":\"ContractFailed\"," + k_Common + ",\"Value\":\"Contract ended manually: User pressed exit to Main menu\"}";
        const std::string s_Dead = "{\"Name\":\"ContractFailed\"," + k_Common + ",\"Value\":{\"FailType\":\"HitmanDead\"}}";
        const std::string s_Cpd = "{\"Name\":\"CpdSet\"," + k_Common + ",\"Value\":{\"MildChess_MissionFailed\":true}}";
        CHECK(!ebr::IsFailureText(s_Restart.data(), s_Restart.size(), false), "text: restart is a failure");
        CHECK(ebr::IsFailureText(s_Exit.data(), s_Exit.size(), false) && !ebr::IsFailureText(s_Exit.data(), s_Exit.size(), true), "text: exit");
        CHECK(ebr::IsFailureText(s_Dead.data(), s_Dead.size(), true), "text: death");
        CHECK(!ebr::IsFailureText(s_Cpd.data(), s_Cpd.size(), false), "text: CpdSet field name is a failure");
        const char* s_Tok = nullptr;
        size_t s_TokLen = 0;
        const std::string s_Health = "{\"Timestamp\":1.0,\"Name\":\"Hero_Health\",\"Value\":33.000000,\"UserId\":\"u\"}";
        CHECK(ebr::ExtractNumberAfter(s_Health.data(), s_Health.size(), "\"Value\":", &s_Tok, &s_TokLen)
              && std::string(s_Tok, s_TokLen) == "33.000000", "text: health value");
    }
}

int main(int argc, char** argv)
{
    Synthetic();
    std::printf("synthetic cases done, failures so far: %d\n", g_Failures);

    Stats s_Stats;
    size_t s_Files = 0;
    for (int i = 1; i < argc; ++i)
    {
        const std::filesystem::path s_Path = argv[i];
        if (std::filesystem::is_directory(s_Path))
        {
            for (const auto& s_Entry : std::filesystem::directory_iterator(s_Path))
                if (s_Entry.path().extension() == ".log")
                {
                    ReplayFile(s_Entry.path(), s_Stats);
                    ++s_Files;
                }
        }
        else
        {
            ReplayFile(s_Path, s_Stats);
            ++s_Files;
        }
    }

    std::printf("files: %zu\n", s_Files);
    std::printf("POST bodies: %zu (events bodies %zu with %zu events, other bodies %zu)\n", s_Stats.bodies,
                s_Stats.eventBodies, s_Stats.events, s_Stats.otherBodies);
    std::printf("with every attempt quarantined: %zu bodies rewritten, %zu events neutralized, %zu real failure bodies\n",
                s_Stats.rewrittenBodies, s_Stats.rewrittenEvents, s_Stats.failureBodies);
    std::printf("fit ladder (bodies by highest level): full=%zu integer=%zu zero=%zu trimmed-timestamp=%zu\n",
                s_Stats.ladder[0], s_Stats.ladder[1], s_Stats.ladder[2], s_Stats.ladder[3]);
    std::printf("event-layer lines: %zu, failure events: %zu, ContractStart arcade=%zu elusive=%zu evergreen=%zu, "
                "ContractLoad=%zu\n",
                s_Stats.eventLines, s_Stats.failureTexts, s_Stats.arcadeStarts, s_Stats.elusiveStarts,
                s_Stats.evergreenStarts, s_Stats.loads);
    std::printf("responses with an orphan close: %zu, recognised: %zu, with a merged record pair: %zu\n",
                s_Stats.responses, s_Stats.orphanCloses, s_Stats.mergedCloses);
    std::printf("with the log's recent attempts all kept contained: %zu events bodies, %zu rewritten, none refused "
                "unless listed above\n",
                s_Stats.cumulativeBodies, s_Stats.cumulativeRewritten);
    std::printf("%s (%d failure%s)\n", g_Failures == 0 ? "ALL CHECKS PASSED" : "CHECKS FAILED", g_Failures,
                g_Failures == 1 ? "" : "s");
    return g_Failures == 0 ? 0 : 1;
}
