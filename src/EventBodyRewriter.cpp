#include "EventBodyRewriter.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>

#include <atomic>
#include <cstring>

namespace ebr
{
namespace
{
    // =========================================================================
    // Attempt table
    // =========================================================================

    struct Slot
    {
        char csid[k_MaxToken];
        uint8_t csidLen;  // 0 = free
        Kind kind;
        bool quarantined;
        bool staked;
        char health[20];
        uint8_t healthLen;
        uint32_t age;
    };

    constexpr size_t k_PastSlots = 64;
    constexpr char k_DefaultHealth[] = "33.000000";

    SRWLOCK g_Lock = SRWLOCK_INIT;
    Slot g_Slots[k_Slots] = {};
    uint32_t g_Age = 0;
    std::atomic<uint32_t> g_QuarantineCount{ 0 };
    std::atomic<uint32_t> g_StakedCount{ 0 };

    char g_Past[k_PastSlots][k_MaxToken] = {};
    uint8_t g_PastLen[k_PastSlots] = {};
    bool g_PastDone[k_PastSlots] = {};  // the server's close has been reported
    uint32_t g_PastNext = 0;
    std::atomic<uint32_t> g_PastCount{ 0 };

    struct SharedLock
    {
        SharedLock() { AcquireSRWLockShared(&g_Lock); }
        ~SharedLock() { ReleaseSRWLockShared(&g_Lock); }
    };

    struct ExclusiveLock
    {
        ExclusiveLock() { AcquireSRWLockExclusive(&g_Lock); }
        ~ExclusiveLock() { ReleaseSRWLockExclusive(&g_Lock); }
    };

    // Ids are copied into JSON and searched for in raw bytes, so only accept
    // characters that can never need escaping.
    bool ValidId(const char* p_Id, size_t p_Len)
    {
        if (!p_Id || p_Len < 8 || p_Len >= k_MaxToken)
            return false;

        for (size_t i = 0; i < p_Len; ++i)
        {
            const char c = p_Id[i];
            const bool s_Ok = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z')
                || (c >= 'A' && c <= 'Z') || c == '-' || c == '_';
            if (!s_Ok)
                return false;
        }
        return true;
    }

    const char* Mem(const char* p_Hay, size_t p_HayLen, const char* p_Needle, size_t p_NeedleLen)
    {
        if (!p_Hay || !p_Needle || p_NeedleLen == 0 || p_HayLen < p_NeedleLen)
            return nullptr;

        const char* s_End = p_Hay + (p_HayLen - p_NeedleLen) + 1;
        for (const char* p = p_Hay; p < s_End; ++p)
        {
            p = static_cast<const char*>(memchr(p, p_Needle[0], static_cast<size_t>(s_End - p)));
            if (!p)
                return nullptr;
            if (memcmp(p, p_Needle, p_NeedleLen) == 0)
                return p;
        }
        return nullptr;
    }

    // String-literal needle.
    template <size_t N>
    const char* MemLit(const char* p_Hay, size_t p_HayLen, const char (&p_Lit)[N])
    {
        return Mem(p_Hay, p_HayLen, p_Lit, N - 1);
    }

    // Lock must be held.
    Slot* FindSlot(const char* p_Csid, size_t p_Len)
    {
        for (auto& s_Slot : g_Slots)
            if (s_Slot.csidLen == p_Len && memcmp(s_Slot.csid, p_Csid, p_Len) == 0)
                return &s_Slot;
        return nullptr;
    }

    // Exclusive lock must be held. Never evicts a quarantined attempt.
    Slot* FindOrCreateSlot(const char* p_Csid, size_t p_Len)
    {
        if (Slot* s_Found = FindSlot(p_Csid, p_Len))
            return s_Found;

        Slot* s_Victim = nullptr;
        for (auto& s_Slot : g_Slots)
        {
            if (s_Slot.csidLen == 0)
            {
                s_Victim = &s_Slot;
                break;
            }
        }

        if (!s_Victim)
        {
            for (auto& s_Slot : g_Slots)
            {
                if (s_Slot.quarantined)
                    continue;
                if (!s_Victim || s_Slot.age < s_Victim->age)
                    s_Victim = &s_Slot;
            }
        }

        if (!s_Victim)
            return nullptr;

        *s_Victim = Slot{};
        memcpy(s_Victim->csid, p_Csid, p_Len);
        s_Victim->csidLen = static_cast<uint8_t>(p_Len);
        s_Victim->age = ++g_Age;
        return s_Victim;
    }

    // Lock must be held.
    bool IsPastLocked(const char* p_Csid, size_t p_Len)
    {
        for (size_t i = 0; i < k_PastSlots; ++i)
            if (g_PastLen[i] == p_Len && memcmp(g_Past[i], p_Csid, p_Len) == 0)
                return true;
        return false;
    }

    // True if the attempt is quarantined; also returns the health value to use.
    bool QuarantineInfo(const char* p_Csid, size_t p_Len, char* p_Health, size_t& p_HealthLen)
    {
        if (p_Len == 0 || p_Len >= k_MaxToken)
            return false;

        SharedLock s_Guard;
        const Slot* s_Slot = FindSlot(p_Csid, p_Len);
        if (!s_Slot || !s_Slot->quarantined)
            return false;

        if (s_Slot->healthLen > 0)
        {
            memcpy(p_Health, s_Slot->health, s_Slot->healthLen);
            p_HealthLen = s_Slot->healthLen;
        }
        else
        {
            memcpy(p_Health, k_DefaultHealth, sizeof(k_DefaultHealth) - 1);
            p_HealthLen = sizeof(k_DefaultHealth) - 1;
        }
        return true;
    }

    // =========================================================================
    // Wire scanner (iterative, bounds-checked, no allocation)
    // =========================================================================

    struct Span
    {
        uint32_t b = 0;
        uint32_t e = 0;
        bool Has() const { return e > b; }
        uint32_t Len() const { return e - b; }
    };

    struct Ev
    {
        Span obj, ts, name, csid, cid, val, uid, sid, origin, id;
    };

    struct Cur
    {
        const char* d;
        size_t n;
        size_t i;
    };

    bool IsWs(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }

    void Ws(Cur& c)
    {
        while (c.i < c.n && IsWs(c.d[c.i]))
            ++c.i;
    }

    // Cursor at an opening quote; leaves it just past the closing quote.
    bool Str(Cur& c)
    {
        if (c.i >= c.n || c.d[c.i] != '"')
            return false;
        ++c.i;

        while (c.i < c.n)
        {
            const char s_Ch = c.d[c.i];
            if (s_Ch == '\\')
            {
                c.i += 2;  // the escaped byte is never a terminator
                continue;
            }
            ++c.i;
            if (s_Ch == '"')
                return true;
        }
        return false;
    }

    // Skips one JSON value of any type.
    bool Val(Cur& c)
    {
        if (c.i >= c.n)
            return false;

        char s_Ch = c.d[c.i];
        if (s_Ch == '"')
            return Str(c);

        if (s_Ch == '{' || s_Ch == '[')
        {
            char s_Stack[64];
            int s_Depth = 0;

            while (c.i < c.n)
            {
                s_Ch = c.d[c.i];
                if (s_Ch == '"')
                {
                    if (!Str(c))
                        return false;
                    continue;
                }
                if (s_Ch == '{' || s_Ch == '[')
                {
                    if (s_Depth >= 64)
                        return false;
                    s_Stack[s_Depth++] = s_Ch;
                }
                else if (s_Ch == '}' || s_Ch == ']')
                {
                    if (s_Depth == 0)
                        return false;
                    const char s_Open = s_Stack[--s_Depth];
                    if ((s_Ch == '}') != (s_Open == '{'))
                        return false;
                    if (s_Depth == 0)
                    {
                        ++c.i;
                        return true;
                    }
                }
                ++c.i;
            }
            return false;
        }

        // Scalar: number, true, false, null.
        const size_t s_Start = c.i;
        while (c.i < c.n)
        {
            s_Ch = c.d[c.i];
            if (s_Ch == ',' || s_Ch == '}' || s_Ch == ']' || IsWs(s_Ch))
                break;
            if (s_Ch == '"' || s_Ch == '{' || s_Ch == '[' || s_Ch == ':')
                return false;
            ++c.i;
        }
        return c.i > s_Start;
    }

    template <size_t N>
    bool KeyIs(const char* p_Key, size_t p_Len, const char (&p_Lit)[N])
    {
        return p_Len == N - 1 && memcmp(p_Key, p_Lit, N - 1) == 0;
    }

    Span* KeyTarget(Ev& p_Ev, const char* p_Key, size_t p_Len)
    {
        if (KeyIs(p_Key, p_Len, "Timestamp")) return &p_Ev.ts;
        if (KeyIs(p_Key, p_Len, "Name")) return &p_Ev.name;
        if (KeyIs(p_Key, p_Len, "ContractSessionId")) return &p_Ev.csid;
        if (KeyIs(p_Key, p_Len, "ContractId")) return &p_Ev.cid;
        if (KeyIs(p_Key, p_Len, "Value")) return &p_Ev.val;
        if (KeyIs(p_Key, p_Len, "UserId")) return &p_Ev.uid;
        if (KeyIs(p_Key, p_Len, "SessionId")) return &p_Ev.sid;
        if (KeyIs(p_Key, p_Len, "Origin")) return &p_Ev.origin;
        if (KeyIs(p_Key, p_Len, "Id")) return &p_Ev.id;
        return nullptr;
    }

    // Cursor at '{'. Records the spans of the depth-1 keys we care about.
    bool ParseEvent(Cur& c, Ev& p_Ev)
    {
        p_Ev = Ev{};
        p_Ev.obj.b = static_cast<uint32_t>(c.i);
        ++c.i;
        Ws(c);

        if (c.i < c.n && c.d[c.i] == '}')
        {
            ++c.i;
            p_Ev.obj.e = static_cast<uint32_t>(c.i);
            return true;
        }

        for (;;)
        {
            const size_t s_KeyStart = c.i;
            if (!Str(c))
                return false;
            const size_t s_KeyEnd = c.i;

            Ws(c);
            if (c.i >= c.n || c.d[c.i] != ':')
                return false;
            ++c.i;
            Ws(c);

            const size_t s_ValStart = c.i;
            if (!Val(c))
                return false;
            const size_t s_ValEnd = c.i;

            if (Span* s_Target = KeyTarget(p_Ev, c.d + s_KeyStart + 1, s_KeyEnd - s_KeyStart - 2))
            {
                if (s_Target->Has())
                    return false;  // duplicate key: ambiguous, refuse
                s_Target->b = static_cast<uint32_t>(s_ValStart);
                s_Target->e = static_cast<uint32_t>(s_ValEnd);
            }

            Ws(c);
            if (c.i >= c.n)
                return false;
            if (c.d[c.i] == ',')
            {
                ++c.i;
                Ws(c);
                continue;
            }
            if (c.d[c.i] == '}')
            {
                ++c.i;
                p_Ev.obj.e = static_cast<uint32_t>(c.i);
                return true;
            }
            return false;
        }
    }

    constexpr char k_Prefix[] = "{\"userId\":\"";
    constexpr char k_Values[] = ",\"values\":[";

    // Positions the cursor just past `"values":[`. False = not an events body.
    bool EnterValues(Cur& c)
    {
        constexpr size_t s_PrefixLen = sizeof(k_Prefix) - 1;
        constexpr size_t s_ValuesLen = sizeof(k_Values) - 1;

        if (!c.d || c.n < s_PrefixLen + s_ValuesLen || c.n > 0x40000000)
            return false;
        if (memcmp(c.d, k_Prefix, s_PrefixLen) != 0)
            return false;

        c.i = s_PrefixLen - 1;  // the userId string's opening quote
        if (!Str(c))
            return false;
        if (c.i > c.n || c.n - c.i < s_ValuesLen || memcmp(c.d + c.i, k_Values, s_ValuesLen) != 0)
            return false;

        c.i += s_ValuesLen;
        return true;
    }

    Result ScanBody(const char* p_Data, size_t p_Len, Ev* p_Events, uint32_t& p_Count)
    {
        p_Count = 0;
        Cur c{ p_Data, p_Len, 0 };
        if (!EnterValues(c))
            return Result::NotEvents;

        Ws(c);
        if (c.i < c.n && c.d[c.i] == ']')
        {
            ++c.i;
        }
        else
        {
            for (;;)
            {
                if (c.i >= c.n || c.d[c.i] != '{')
                    return Result::Error;
                if (p_Count >= k_MaxEvents)
                    return Result::Error;
                if (!ParseEvent(c, p_Events[p_Count]))
                    return Result::Error;
                ++p_Count;

                Ws(c);
                if (c.i >= c.n)
                    return Result::Error;
                if (c.d[c.i] == ',')
                {
                    ++c.i;
                    Ws(c);
                    continue;
                }
                if (c.d[c.i] == ']')
                {
                    ++c.i;
                    break;
                }
                return Result::Error;
            }
        }

        // Rest of the root object: more key/value pairs, then '}' at the very end.
        for (;;)
        {
            Ws(c);
            if (c.i >= c.n)
                return Result::Error;
            if (c.d[c.i] == '}')
            {
                ++c.i;
                break;
            }
            if (c.d[c.i] != ',')
                return Result::Error;
            ++c.i;
            Ws(c);
            if (!Str(c))
                return Result::Error;
            Ws(c);
            if (c.i >= c.n || c.d[c.i] != ':')
                return Result::Error;
            ++c.i;
            Ws(c);
            if (!Val(c))
                return Result::Error;
        }

        Ws(c);
        return c.i == c.n ? Result::Clean : Result::Error;
    }

    bool IsStrTok(const char* p_Data, const Span& p_Span)
    {
        return p_Span.Len() >= 2 && p_Data[p_Span.b] == '"' && p_Data[p_Span.e - 1] == '"';
    }

    template <size_t N>
    bool TokIs(const char* p_Data, const Span& p_Span, const char (&p_Lit)[N])
    {
        return p_Span.Len() == N - 1 && memcmp(p_Data + p_Span.b, p_Lit, N - 1) == 0;
    }

    // An event with an empty Name and an empty or missing Value carries nothing
    // the server could act on, and it is shorter than any Hero_Health
    // replacement, so it is left as it is. Seen once, at the end of a Freelancer
    // showdown (session-20261113-012047.log, mission time 806.49).
    bool IsInertEvent(const char* p_Data, const Ev& p_Ev)
    {
        return TokIs(p_Data, p_Ev.name, "\"\"") && (!p_Ev.val.Has() || TokIs(p_Data, p_Ev.val, "\"\""));
    }

    // p_PlainOnly: digits and '.' only (no sign, no exponent).
    bool IsNumberTok(const char* p_Tok, size_t p_Len, bool p_PlainOnly)
    {
        if (!p_Tok || p_Len == 0 || p_Len > 32)
            return false;

        bool s_Digit = false;
        for (size_t i = 0; i < p_Len; ++i)
        {
            const char c = p_Tok[i];
            if (c >= '0' && c <= '9')
                s_Digit = true;
            else if (c == '.')
                continue;
            else if (!p_PlainOnly && (c == '-' || c == '+' || c == 'e' || c == 'E'))
                continue;
            else
                return false;
        }
        return s_Digit;
    }

    struct Out
    {
        char* p;
        char* end;
        bool ok = true;

        void Put(const char* p_Src, size_t p_Len)
        {
            if (!ok || static_cast<size_t>(end - p) < p_Len)
            {
                ok = false;
                return;
            }
            memcpy(p, p_Src, p_Len);
            p += p_Len;
        }

        template <size_t N>
        void Lit(const char (&p_Lit)[N]) { Put(p_Lit, N - 1); }
    };

    // Digits before the decimal point ("28.744013" -> 2). Whole token if no point.
    size_t IntegerPartLen(const char* p_Tok, size_t p_Len)
    {
        const void* s_Dot = memchr(p_Tok, '.', p_Len);
        if (!s_Dot)
            return p_Len;
        const size_t s_Len = static_cast<size_t>(static_cast<const char*>(s_Dot) - p_Tok);
        return s_Len == 0 ? p_Len : s_Len;
    }

    // Overwrites one event with a Hero_Health event of the same byte length.
    // Returns nullptr on success, else a static reason.
    const char* RewriteEvent(char* p_Buf, const Ev& p_Ev, const char* p_Health, size_t p_HealthLen,
                             uint32_t& p_Ladder)
    {
        const Span* s_Strings[] = { &p_Ev.csid, &p_Ev.cid, &p_Ev.uid, &p_Ev.sid, &p_Ev.id };
        for (const Span* s_Span : s_Strings)
        {
            if (!s_Span->Has() || !IsStrTok(p_Buf, *s_Span))
                return "event is missing an id field";
            if (s_Span->Len() > k_MaxToken + 2)
                return "event id field is too long";
        }

        if (!p_Ev.ts.Has() || !IsNumberTok(p_Buf + p_Ev.ts.b, p_Ev.ts.Len(), false))
            return "event has no numeric Timestamp";

        if (p_HealthLen > 16 || !IsNumberTok(p_Health, p_HealthLen, true))
            return "bad health value";

        constexpr char s_DefaultOrigin[] = "\"gameclient\"";
        const char* s_Origin = s_DefaultOrigin;
        size_t s_OriginLen = sizeof(s_DefaultOrigin) - 1;
        if (p_Ev.origin.Has() && IsStrTok(p_Buf, p_Ev.origin) && p_Ev.origin.Len() <= 34)
        {
            s_Origin = p_Buf + p_Ev.origin.b;
            s_OriginLen = p_Ev.origin.Len();
        }

        const size_t s_ObjLen = p_Ev.obj.Len();
        const char* s_Ts = p_Buf + p_Ev.ts.b;
        const size_t s_TsFull = p_Ev.ts.Len();

        // Fraction digits may only be dropped from a plain decimal Timestamp.
        const size_t s_TsMin = IsNumberTok(s_Ts, s_TsFull, true) ? IntegerPartLen(s_Ts, s_TsFull)
                                                                  : s_TsFull;

        // Fit ladder: full health value, its integer part, "0", then also drop
        // fraction digits from the Timestamp.
        for (uint32_t s_Level = 0; s_Level < 4; ++s_Level)
        {
            const char* s_Value = p_Health;
            size_t s_ValueLen = p_HealthLen;
            if (s_Level == 1)
            {
                s_ValueLen = IntegerPartLen(p_Health, p_HealthLen);
            }
            else if (s_Level >= 2)
            {
                s_Value = "0";
                s_ValueLen = 1;
            }

            for (size_t s_TsLen = s_TsFull;; --s_TsLen)
            {
                // Never end the number on the decimal point.
                const bool s_TsValid = s_TsLen == s_TsFull || s_TsLen == s_TsMin
                    || s_TsLen > s_TsMin + 1;

                if (s_TsValid)
                {
                    char s_Tmp[1024];
                    Out s_Out{ s_Tmp, s_Tmp + sizeof(s_Tmp) };

                    s_Out.Lit("{\"Timestamp\":");
                    s_Out.Put(s_Ts, s_TsLen);
                    s_Out.Lit(",\"Name\":\"Hero_Health\",\"ContractSessionId\":");
                    s_Out.Put(p_Buf + p_Ev.csid.b, p_Ev.csid.Len());
                    s_Out.Lit(",\"ContractId\":");
                    s_Out.Put(p_Buf + p_Ev.cid.b, p_Ev.cid.Len());
                    s_Out.Lit(",\"Value\":");
                    s_Out.Put(s_Value, s_ValueLen);
                    s_Out.Lit(",\"UserId\":");
                    s_Out.Put(p_Buf + p_Ev.uid.b, p_Ev.uid.Len());
                    s_Out.Lit(",\"SessionId\":");
                    s_Out.Put(p_Buf + p_Ev.sid.b, p_Ev.sid.Len());
                    s_Out.Lit(",\"Origin\":");
                    s_Out.Put(s_Origin, s_OriginLen);
                    s_Out.Lit(",\"Id\":");
                    s_Out.Put(p_Buf + p_Ev.id.b, p_Ev.id.Len());

                    if (!s_Out.ok)
                        return "replacement event is too large";

                    // The source tokens live inside the object being replaced,
                    // so the replacement is built in s_Tmp and copied over last.
                    const size_t s_Used = static_cast<size_t>(s_Out.p - s_Tmp);
                    if (s_Used + 1 <= s_ObjLen)
                    {
                        char* s_Dst = p_Buf + p_Ev.obj.b;
                        memcpy(s_Dst, s_Tmp, s_Used);
                        memset(s_Dst + s_Used, ' ', s_ObjLen - s_Used - 1);
                        s_Dst[s_ObjLen - 1] = '}';
                        p_Ladder = s_Level;
                        return nullptr;
                    }
                }

                // Only the last level may shorten the Timestamp.
                if (s_Level < 3 || s_TsLen <= s_TsMin)
                    break;
            }
        }

        return "replacement does not fit in the original event";
    }

    void AppendList(char* p_List, size_t p_Cap, const char* p_Item, size_t p_Len)
    {
        const size_t s_Used = strlen(p_List);
        if (s_Used + p_Len + 2 > p_Cap)
            return;

        size_t s_Pos = s_Used;
        if (s_Used > 0)
            p_List[s_Pos++] = ',';
        memcpy(p_List + s_Pos, p_Item, p_Len);
        p_List[s_Pos + p_Len] = '\0';
    }

    // Finds p_Key inside [p_From, p_To) and returns the bytes up to the next quote.
    bool StringAfter(const char* p_From, const char* p_To, const char* p_Key, size_t p_KeyLen,
                     const char** p_Val, size_t* p_ValLen)
    {
        if (!p_From || p_To <= p_From)
            return false;

        const char* s_Key = Mem(p_From, static_cast<size_t>(p_To - p_From), p_Key, p_KeyLen);
        if (!s_Key)
            return false;

        const char* s_Start = s_Key + p_KeyLen;
        const char* s_End = static_cast<const char*>(
            memchr(s_Start, '"', static_cast<size_t>(p_To - s_Start)));
        if (!s_End)
            return false;

        *p_Val = s_Start;
        *p_ValLen = static_cast<size_t>(s_End - s_Start);
        return true;
    }

    // True when every quarantined id found in the bytes is the ContractSessionId
    // of an event (which is then a Hero_Health event, original or rewritten).
    // An id anywhere else — inside a Value, under another key, after the values
    // array — was not neutralized, so the body must not be sent as it is.
    bool QuarantinedIdsOnlyInEvents(const char* p_Buf, size_t p_Len, const Ev* p_Events, uint32_t p_Count)
    {
        const char* s_End = p_Buf + p_Len;

        SharedLock s_Guard;
        for (const auto& s_Slot : g_Slots)
        {
            const size_t s_IdLen = s_Slot.csidLen;
            if (!s_Slot.quarantined || s_IdLen == 0)
                continue;

            size_t s_Raw = 0;
            for (const char* p = Mem(p_Buf, p_Len, s_Slot.csid, s_IdLen); p;
                 p = Mem(p + s_IdLen, static_cast<size_t>(s_End - (p + s_IdLen)), s_Slot.csid, s_IdLen))
                ++s_Raw;

            if (s_Raw == 0)
                continue;

            size_t s_Owned = 0;
            for (uint32_t i = 0; i < p_Count; ++i)
            {
                const Span& s_Csid = p_Events[i].csid;
                if (s_Csid.Has() && IsStrTok(p_Buf, s_Csid) && s_Csid.Len() - 2 == s_IdLen
                    && memcmp(p_Buf + s_Csid.b + 1, s_Slot.csid, s_IdLen) == 0)
                    ++s_Owned;
            }

            if (s_Raw != s_Owned)
                return false;
        }
        return true;
    }
}

// =============================================================================
// Attempt table
// =============================================================================

void NoteContractStart(const char* p_Csid, size_t p_Len, Kind p_Kind)
{
    if (!ValidId(p_Csid, p_Len))
        return;

    ExclusiveLock s_Guard;
    if (Slot* s_Slot = FindOrCreateSlot(p_Csid, p_Len))
        if (p_Kind != Kind::Unknown)
            s_Slot->kind = p_Kind;
}

Kind KindOf(const char* p_Csid, size_t p_Len)
{
    if (!ValidId(p_Csid, p_Len))
        return Kind::Unknown;

    SharedLock s_Guard;
    const Slot* s_Slot = FindSlot(p_Csid, p_Len);
    return s_Slot ? s_Slot->kind : Kind::Unknown;
}

void NoteHealth(const char* p_Csid, size_t p_Len, const char* p_Token, size_t p_TokenLen)
{
    if (!ValidId(p_Csid, p_Len) || p_TokenLen > 16 || !IsNumberTok(p_Token, p_TokenLen, true))
        return;

    ExclusiveLock s_Guard;
    if (Slot* s_Slot = FindOrCreateSlot(p_Csid, p_Len))
    {
        memcpy(s_Slot->health, p_Token, p_TokenLen);
        s_Slot->healthLen = static_cast<uint8_t>(p_TokenLen);
    }
}

bool Arm(const char* p_Csid, size_t p_Len, bool p_Staked)
{
    if (!ValidId(p_Csid, p_Len))
        return false;

    ExclusiveLock s_Guard;
    Slot* s_Slot = FindOrCreateSlot(p_Csid, p_Len);
    if (!s_Slot)
        return false;

    if (!s_Slot->quarantined)
    {
        s_Slot->quarantined = true;
        g_QuarantineCount.fetch_add(1);
    }
    if (p_Staked && !s_Slot->staked)
    {
        s_Slot->staked = true;
        g_StakedCount.fetch_add(1);
    }
    return true;
}

bool IsQuarantined(const char* p_Csid, size_t p_Len, bool* p_Staked)
{
    if (!HasQuarantine() || !p_Csid || p_Len == 0 || p_Len >= k_MaxToken)
        return false;

    SharedLock s_Guard;
    const Slot* s_Slot = FindSlot(p_Csid, p_Len);
    if (!s_Slot || !s_Slot->quarantined)
        return false;

    if (p_Staked)
        *p_Staked = s_Slot->staked;
    return true;
}

bool HasQuarantine()
{
    return g_QuarantineCount.load(std::memory_order_relaxed) != 0;
}

bool HasStakedQuarantine()
{
    return g_StakedCount.load(std::memory_order_relaxed) != 0;
}

uint32_t QuarantineCount()
{
    return g_QuarantineCount.load(std::memory_order_relaxed);
}

void AddPast(const char* p_Csid, size_t p_Len)
{
    if (!ValidId(p_Csid, p_Len))
        return;

    ExclusiveLock s_Guard;
    if (IsPastLocked(p_Csid, p_Len))
        return;

    const uint32_t s_Index = g_PastNext++ % k_PastSlots;
    memset(g_Past[s_Index], 0, k_MaxToken);
    memcpy(g_Past[s_Index], p_Csid, p_Len);
    g_PastLen[s_Index] = static_cast<uint8_t>(p_Len);
    g_PastDone[s_Index] = false;
    g_PastCount.store(1);
}

bool HasPast()
{
    return g_PastCount.load(std::memory_order_relaxed) != 0;
}

bool RetirePast(const char* p_Csid, size_t p_Len)
{
    if (!ValidId(p_Csid, p_Len))
        return false;

    ExclusiveLock s_Guard;
    for (size_t i = 0; i < k_PastSlots; ++i)
    {
        if (g_PastLen[i] != p_Len || memcmp(g_Past[i], p_Csid, p_Len) != 0)
            continue;
        if (g_PastDone[i])
            return false;
        g_PastDone[i] = true;
        return true;
    }
    return false;
}

int MentionsQuarantined(const char* p_Data, size_t p_Len)
{
    if (!HasQuarantine() || !p_Data || p_Len == 0)
        return 0;

    // Snapshot the ids so the search runs without the lock.
    char s_Ids[k_Slots][k_MaxToken];
    uint8_t s_Lens[k_Slots];
    bool s_Staked[k_Slots];
    size_t s_Count = 0;
    {
        SharedLock s_Guard;
        for (const auto& s_Slot : g_Slots)
        {
            if (!s_Slot.quarantined || s_Slot.csidLen == 0)
                continue;
            memcpy(s_Ids[s_Count], s_Slot.csid, s_Slot.csidLen);
            s_Lens[s_Count] = s_Slot.csidLen;
            s_Staked[s_Count] = s_Slot.staked;
            ++s_Count;
        }
    }

    int s_Level = 0;
    for (size_t i = 0; i < s_Count; ++i)
    {
        if (!Mem(p_Data, p_Len, s_Ids[i], s_Lens[i]))
            continue;
        if (s_Staked[i])
            return 2;
        s_Level = 1;
    }
    return s_Level;
}

// =============================================================================
// Event-layer text
// =============================================================================

bool ExtractCsid(const char* p_Text, size_t p_Len, const char** p_Csid, size_t* p_CsidLen)
{
    constexpr char s_Key[] = "\"ContractSessionId\":\"";
    if (!p_Text || !p_Csid || !p_CsidLen)
        return false;

    const char* s_Val = nullptr;
    size_t s_ValLen = 0;
    if (!StringAfter(p_Text, p_Text + p_Len, s_Key, sizeof(s_Key) - 1, &s_Val, &s_ValLen))
        return false;
    if (!ValidId(s_Val, s_ValLen))
        return false;

    *p_Csid = s_Val;
    *p_CsidLen = s_ValLen;
    return true;
}

bool NameIs(const char* p_Text, size_t p_Len, const char* p_Name)
{
    char s_Needle[128];
    const size_t s_NameLen = p_Name ? strlen(p_Name) : 0;
    if (s_NameLen == 0 || s_NameLen > sizeof(s_Needle) - 12)
        return false;

    constexpr char s_Head[] = "\"Name\":\"";
    size_t s_Pos = sizeof(s_Head) - 1;
    memcpy(s_Needle, s_Head, s_Pos);
    memcpy(s_Needle + s_Pos, p_Name, s_NameLen);
    s_Pos += s_NameLen;
    s_Needle[s_Pos++] = '"';

    return Mem(p_Text, p_Len, s_Needle, s_Pos) != nullptr;
}

Kind KindFromContractStart(const char* p_Text, size_t p_Len)
{
    if (MemLit(p_Text, p_Len, "\"ContractType\":\"arcade\"")) return Kind::Arcade;
    if (MemLit(p_Text, p_Len, "\"ContractType\":\"elusive\"")) return Kind::Elusive;
    if (MemLit(p_Text, p_Len, "\"ContractType\":\"evergreen\"")) return Kind::Evergreen;
    if (MemLit(p_Text, p_Len, "\"ContractType\":\"")) return Kind::Other;
    return Kind::Unknown;
}

bool IsFailureText(const char* p_Text, size_t p_Len, bool p_AllowExitToMenu)
{
    if (NameIs(p_Text, p_Len, "MissionFailed_Event") || NameIs(p_Text, p_Len, "MissionWounded_Event")
        || NameIs(p_Text, p_Len, "MildChess_MissionFailed"))
        return true;

    if (!NameIs(p_Text, p_Len, "ContractFailed"))
        return false;

    if (MemLit(p_Text, p_Len, "OnRestartLevel") || MemLit(p_Text, p_Len, "OnReplanLevel")
        || MemLit(p_Text, p_Len, "OnLoadGame"))
        return false;

    if (p_AllowExitToMenu && MemLit(p_Text, p_Len, "exit to Main menu"))
        return false;

    return true;
}

bool ExtractNumberAfter(const char* p_Text, size_t p_Len, const char* p_Key, const char** p_Tok,
                        size_t* p_TokLen)
{
    const size_t s_KeyLen = p_Key ? strlen(p_Key) : 0;
    const char* s_Key = Mem(p_Text, p_Len, p_Key, s_KeyLen);
    if (!s_Key || !p_Tok || !p_TokLen)
        return false;

    const char* s_Start = s_Key + s_KeyLen;
    const char* s_End = p_Text + p_Len;
    const char* p = s_Start;
    while (p < s_End && ((*p >= '0' && *p <= '9') || *p == '.'))
        ++p;

    const size_t s_Len = static_cast<size_t>(p - s_Start);
    if (!IsNumberTok(s_Start, s_Len, true))
        return false;

    *p_Tok = s_Start;
    *p_TokLen = s_Len;
    return true;
}

// =============================================================================
// Wire bodies
// =============================================================================

bool LooksLikeEventsBody(const char* p_Data, size_t p_Len)
{
    Cur c{ p_Data, p_Len, 0 };
    return EnterValues(c);
}

namespace
{
    // {"GearWall_ThematicIndex":<number>} or {"MyValue01":<number>}
    bool IsPageFlipValue(const char* p_Data, const Span& p_Val)
    {
        static constexpr const char* k_Keys[] = { "{\"GearWall_ThematicIndex\":", "{\"MyValue01\":" };
        if (!p_Val.Has() || p_Data[p_Val.e - 1] != '}')
            return false;
        for (const char* s_Key : k_Keys)
        {
            const size_t s_KeyLen = strlen(s_Key);
            if (p_Val.Len() <= s_KeyLen + 1 || memcmp(p_Data + p_Val.b, s_Key, s_KeyLen) != 0)
                continue;
            const char* s_Num = p_Data + p_Val.b + s_KeyLen;
            const size_t s_NumLen = p_Val.Len() - s_KeyLen - 1;
            return IsNumberTok(s_Num, s_NumLen, false);
        }
        return false;
    }
}

bool HasUnsafeCpdSet(const char* p_Data, size_t p_Len)
{
    if (!p_Data || !Mem(p_Data, p_Len, "\"CpdSet\"", 8))
        return false;

    Ev s_Events[k_MaxEvents];
    uint32_t s_Count = 0;
    if (ScanBody(p_Data, p_Len, s_Events, s_Count) != Result::Clean)
        return true;  // mentions CpdSet but cannot be read: fail closed

    for (uint32_t i = 0; i < s_Count; ++i)
    {
        const Ev& s_Ev = s_Events[i];
        if (!TokIs(p_Data, s_Ev.name, "\"CpdSet\""))
            continue;
        if (s_Ev.csid.Has() && IsStrTok(p_Data, s_Ev.csid)
            && IsQuarantined(p_Data + s_Ev.csid.b + 1, s_Ev.csid.Len() - 2))
            continue;  // rewritten by Neutralize
        if (IsPageFlipValue(p_Data, s_Ev.val))
            continue;
        return true;
    }
    return false;
}

Result Neutralize(char* p_Buf, size_t p_Len, Report& p_Report)
{
    p_Report = Report{};

    Ev s_Events[k_MaxEvents];
    uint32_t s_Count = 0;
    const Result s_Scan = ScanBody(p_Buf, p_Len, s_Events, s_Count);
    if (s_Scan != Result::Clean)
    {
        p_Report.result = s_Scan;
        if (s_Scan == Result::Error)
            p_Report.error = "events body does not parse";
        return s_Scan;
    }

    p_Report.events = s_Count;

    for (uint32_t i = 0; i < s_Count; ++i)
    {
        const Ev& s_Ev = s_Events[i];
        if (!s_Ev.csid.Has() || !IsStrTok(p_Buf, s_Ev.csid))
            continue;

        char s_Health[20];
        size_t s_HealthLen = 0;
        if (!QuarantineInfo(p_Buf + s_Ev.csid.b + 1, s_Ev.csid.Len() - 2, s_Health, s_HealthLen))
            continue;

        if (TokIs(p_Buf, s_Ev.name, "\"Hero_Health\"") || IsInertEvent(p_Buf, s_Ev))
            continue;  // already harmless

        // Record what is being replaced before the bytes are overwritten.
        if (s_Ev.name.Has() && IsStrTok(p_Buf, s_Ev.name))
            AppendList(p_Report.names, sizeof(p_Report.names), p_Buf + s_Ev.name.b + 1,
                       s_Ev.name.Len() - 2);
        if (s_Ev.id.Has() && IsStrTok(p_Buf, s_Ev.id))
            AppendList(p_Report.ids, sizeof(p_Report.ids), p_Buf + s_Ev.id.b + 1, s_Ev.id.Len() - 2);

        uint32_t s_Ladder = 0;
        if (const char* s_Error = RewriteEvent(p_Buf, s_Ev, s_Health, s_HealthLen, s_Ladder))
        {
            p_Report.result = Result::Error;
            p_Report.error = s_Error;
            return Result::Error;
        }

        ++p_Report.neutralized;
        if (s_Ladder > p_Report.ladder)
            p_Report.ladder = s_Ladder;
    }

    constexpr const char* s_StrayId = "a contained attempt is named outside an event's ContractSessionId";

    if (p_Report.neutralized == 0)
    {
        if (HasQuarantine() && !QuarantinedIdsOnlyInEvents(p_Buf, p_Len, s_Events, s_Count))
        {
            p_Report.result = Result::Error;
            p_Report.error = s_StrayId;
            return Result::Error;
        }

        p_Report.result = Result::Clean;
        return Result::Clean;
    }

    // Verify by scanning the result again from scratch.
    Ev s_Check[k_MaxEvents];
    uint32_t s_CheckCount = 0;
    if (ScanBody(p_Buf, p_Len, s_Check, s_CheckCount) != Result::Clean || s_CheckCount != s_Count)
    {
        p_Report.result = Result::Error;
        p_Report.error = "rewritten body failed verification";
        return Result::Error;
    }

    for (uint32_t i = 0; i < s_CheckCount; ++i)
    {
        const Ev& s_Ev = s_Check[i];
        if (!s_Ev.csid.Has() || !IsStrTok(p_Buf, s_Ev.csid))
            continue;
        if (!IsQuarantined(p_Buf + s_Ev.csid.b + 1, s_Ev.csid.Len() - 2))
            continue;
        if (!TokIs(p_Buf, s_Ev.name, "\"Hero_Health\"") && !IsInertEvent(p_Buf, s_Ev))
        {
            p_Report.result = Result::Error;
            p_Report.error = "a quarantined event survived the rewrite";
            return Result::Error;
        }
    }

    if (!QuarantinedIdsOnlyInEvents(p_Buf, p_Len, s_Check, s_CheckCount))
    {
        p_Report.result = Result::Error;
        p_Report.error = s_StrayId;
        return Result::Error;
    }

    p_Report.result = Result::Rewritten;
    return Result::Rewritten;
}

// =============================================================================
// Server responses
// =============================================================================

size_t FindServerCloses(const char* p_Text, size_t p_Len, ServerClose* p_Out, size_t p_MaxOut)
{
    if (!p_Text || !p_Out || p_MaxOut == 0 || (!HasQuarantine() && !HasPast()))
        return 0;

    struct Needle
    {
        const char* name;
        size_t nameLen;
        const char* key;
        size_t keyLen;
    };

    constexpr char s_Closing[] = "\"Name\":\"SegmentClosing\"";
    constexpr char s_Failed[] = "\"Name\":\"ContractFailed\"";
    constexpr char s_CloseType[] = "\"CloseType\":\"";
    constexpr char s_FailType[] = "\"FailType\":\"";
    constexpr char s_CsidKey[] = "\"ContractSessionId\":\"";
    constexpr char s_EventEnd[] = "\"IsReplicated\"";
    constexpr ptrdiff_t s_MaxWindow = 8192;

    const Needle s_Needles[] = {
        { s_Closing, sizeof(s_Closing) - 1, s_CloseType, sizeof(s_CloseType) - 1 },
        { s_Failed, sizeof(s_Failed) - 1, s_FailType, sizeof(s_FailType) - 1 },
    };

    const char* s_TextEnd = p_Text + p_Len;
    size_t s_Found = 0;

    for (const Needle& s_Needle : s_Needles)
    {
        const char* p = p_Text;
        while (s_Found < p_MaxOut && p < s_TextEnd)
        {
            p = Mem(p, static_cast<size_t>(s_TextEnd - p), s_Needle.name, s_Needle.nameLen);
            if (!p)
                break;

            // One server event runs from its Name to its IsReplicated field.
            const char* s_WinEnd = Mem(p, static_cast<size_t>(s_TextEnd - p), s_EventEnd,
                                       sizeof(s_EventEnd) - 1);
            if (!s_WinEnd || s_WinEnd - p > s_MaxWindow)
                s_WinEnd = (s_TextEnd - p > s_MaxWindow) ? p + s_MaxWindow : s_TextEnd;

            const char* s_Csid = nullptr;
            size_t s_CsidLen = 0;
            const bool s_HasCsid = StringAfter(p, s_WinEnd, s_CsidKey, sizeof(s_CsidKey) - 1, &s_Csid,
                                               &s_CsidLen)
                && ValidId(s_Csid, s_CsidLen);

            if (s_HasCsid)
            {
                bool s_Past = false;
                bool s_Ours = IsQuarantined(s_Csid, s_CsidLen);
                if (!s_Ours && HasPast())
                {
                    SharedLock s_Guard;
                    s_Ours = s_Past = IsPastLocked(s_Csid, s_CsidLen);
                }

                if (s_Ours)
                {
                    ServerClose s_Close{};
                    memcpy(s_Close.csid, s_Csid, s_CsidLen);
                    s_Close.past = s_Past;

                    const char* s_Type = nullptr;
                    size_t s_TypeLen = 0;
                    if (StringAfter(p, s_WinEnd, s_Needle.key, s_Needle.keyLen, &s_Type, &s_TypeLen))
                    {
                        if (s_TypeLen >= sizeof(s_Close.closeType))
                            s_TypeLen = sizeof(s_Close.closeType) - 1;
                        memcpy(s_Close.closeType, s_Type, s_TypeLen);
                    }

                    s_Close.orphaned = strcmp(s_Close.closeType, "ContractFailed:OrphanedSession") == 0
                        || strcmp(s_Close.closeType, "OrphanedSession") == 0;

                    // The server reports one close as a SegmentClosing and a
                    // ContractFailed record. Keep the second only if it disagrees.
                    bool s_Duplicate = false;
                    for (size_t i = 0; i < s_Found && !s_Duplicate; ++i)
                        s_Duplicate = p_Out[i].orphaned == s_Close.orphaned
                            && strcmp(p_Out[i].csid, s_Close.csid) == 0;

                    if (!s_Duplicate)
                        p_Out[s_Found++] = s_Close;
                }
            }

            p = s_WinEnd > p ? s_WinEnd : p + s_Needle.nameLen;
        }
    }

    return s_Found;
}

void ResetForTests()
{
    ExclusiveLock s_Guard;
    memset(g_Slots, 0, sizeof(g_Slots));
    memset(g_Past, 0, sizeof(g_Past));
    memset(g_PastLen, 0, sizeof(g_PastLen));
    memset(g_PastDone, 0, sizeof(g_PastDone));
    g_Age = 0;
    g_PastNext = 0;
    g_QuarantineCount.store(0);
    g_StakedCount.store(0);
    g_PastCount.store(0);
}
}
