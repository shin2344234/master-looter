#include "mem.h"

#include <cstring>
#include <vector>

namespace ml::mem
{
    // ------------------------------------------------------------ module ----
    const Module& Game()
    {
        static const Module m = []
        {
            Module r;
            const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
            if (!base) return r;
            const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
            if (dos->e_magic != IMAGE_DOS_SIGNATURE) return r;
            const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
            if (nt->Signature != IMAGE_NT_SIGNATURE) return r;
            r.base = base;
            r.size = nt->OptionalHeader.SizeOfImage;
            return r;
        }();
        return m;
    }

    bool InImage(uintptr_t p)
    {
        const Module& m = Game();
        return m.base && p >= m.base && p < m.base + m.size;
    }

    // ---------------------------------------------------------- readable ----
    // VirtualQuery costs ~250 us in this process (the syscall is intercepted by
    // something), so results are cached per region for a few seconds. Entities
    // live in a few dozen heap regions, so the cache hit rate is high.
    namespace
    {
        struct Region { uintptr_t base, end; DWORD when; bool ok; };
        Region g_regs[256];
        int    g_regN = 0, g_regNext = 0;
        CRITICAL_SECTION g_regCs;
        LONG   g_regCsReady = 0;
        volatile LONG g_faults = 0;

        void RegLock()
        {
            if (InterlockedCompareExchange(&g_regCsReady, 1, 0) == 0)
                InitializeCriticalSection(&g_regCs);
            while (InterlockedCompareExchange(&g_regCsReady, 2, 2) != 2)
            {
                // First initializer publishes state 2 once the CS is ready.
                if (InterlockedCompareExchange(&g_regCsReady, 2, 1) == 1) break;
                Sleep(0);
            }
            EnterCriticalSection(&g_regCs);
        }
        void RegUnlock() { LeaveCriticalSection(&g_regCs); }
    }

    long FaultCount() { return g_faults; }

    bool Readable(uintptr_t a, size_t n)
    {
        if (!Plausible(a) || n == 0) return false;
        const DWORD now = GetTickCount();
        RegLock();
        for (int i = 0; i < g_regN; ++i)
        {
            if (a < g_regs[i].base || a + n > g_regs[i].end) continue;
            if (now - g_regs[i].when < 5000) { const bool ok = g_regs[i].ok; RegUnlock(); return ok; }
            break;
        }
        RegUnlock();
        MEMORY_BASIC_INFORMATION mbi;
        if (!VirtualQuery(reinterpret_cast<LPCVOID>(a), &mbi, sizeof mbi)) return false;
        const bool ok = mbi.State == MEM_COMMIT && !(mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD));
        const uintptr_t base = reinterpret_cast<uintptr_t>(mbi.BaseAddress);
        const uintptr_t end  = base + mbi.RegionSize;
        RegLock();
        int slot;
        if (g_regN < 256) slot = g_regN++;
        else { slot = g_regNext; g_regNext = (g_regNext + 1) % 256; }
        g_regs[slot] = { base, end, now, ok };
        RegUnlock();
        return ok && (end - a) >= n;
    }

    // ----------------------------------------------------------- readers ----
    bool Read8(uintptr_t a, uint8_t* out)
    {
        if (!Plausible(a)) return false;
        __try { *out = *reinterpret_cast<volatile uint8_t*>(a); return true; }
        __except (EXCEPTION_EXECUTE_HANDLER) { InterlockedIncrement(&g_faults); return false; }
    }
    bool Read16(uintptr_t a, uint16_t* out)
    {
        if (!Plausible(a)) return false;
        __try { *out = *reinterpret_cast<volatile uint16_t*>(a); return true; }
        __except (EXCEPTION_EXECUTE_HANDLER) { InterlockedIncrement(&g_faults); return false; }
    }
    bool Read32(uintptr_t a, uint32_t* out)
    {
        if (!Plausible(a)) return false;
        __try { *out = *reinterpret_cast<volatile uint32_t*>(a); return true; }
        __except (EXCEPTION_EXECUTE_HANDLER) { InterlockedIncrement(&g_faults); return false; }
    }
    bool Read64(uintptr_t a, uint64_t* out)
    {
        if (!Plausible(a)) return false;
        __try { *out = *reinterpret_cast<volatile uint64_t*>(a); return true; }
        __except (EXCEPTION_EXECUTE_HANDLER) { InterlockedIncrement(&g_faults); return false; }
    }
    bool ReadPtr(uintptr_t a, uintptr_t* out)
    {
        uint64_t v = 0;
        if (!Read64(a, &v) || !Plausible(static_cast<uintptr_t>(v))) return false;
        *out = static_cast<uintptr_t>(v);
        return true;
    }
    bool ReadF32x3(uintptr_t a, float* out)
    {
        if (!Plausible(a)) return false;
        __try
        {
            out[0] = *reinterpret_cast<volatile float*>(a);
            out[1] = *reinterpret_cast<volatile float*>(a + 4);
            out[2] = *reinterpret_cast<volatile float*>(a + 8);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { InterlockedIncrement(&g_faults); return false; }
    }
    bool ReadBytes(uintptr_t a, void* out, size_t n)
    {
        if (!Plausible(a)) return false;
        __try { memcpy(out, reinterpret_cast<const void*>(a), n); return true; }
        __except (EXCEPTION_EXECUTE_HANDLER) { InterlockedIncrement(&g_faults); return false; }
    }
    bool ReadCString(uintptr_t a, char* out, size_t n)
    {
        if (!Plausible(a) || n == 0) return false;
        __try
        {
            size_t i = 0;
            for (; i < n - 1; ++i)
            {
                const char c = *reinterpret_cast<volatile char*>(a + i);
                if (c == 0) break;
                if (static_cast<unsigned char>(c) < 0x20 || static_cast<unsigned char>(c) > 0x7E) return false;
                out[i] = c;
            }
            out[i] = 0;
            return i > 0;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { InterlockedIncrement(&g_faults); return false; }
    }
    bool ReadEngineString(uintptr_t slot, char* out, size_t n)
    {
        uintptr_t obj = 0, cstr = 0;
        if (!ReadPtr(slot, &obj) || !ReadPtr(obj, &cstr)) return false;
        return ReadCString(cstr, out, n);
    }
    uintptr_t Deref(uintptr_t p, unsigned off)
    {
        uintptr_t v = 0;
        if (!p || !ReadPtr(p + off, &v)) return 0;
        return Readable(v, 8) ? v : 0;
    }

    // ----------------------------------------------------------- scanner ----
    namespace
    {
        struct Pattern { std::vector<uint8_t> bytes; std::vector<bool> mask; size_t firstFixed = 0; };

        int Nibble(char c)
        {
            if (c >= '0' && c <= '9') return c - '0';
            if (c >= 'a' && c <= 'f') return c - 'a' + 10;
            if (c >= 'A' && c <= 'F') return c - 'A' + 10;
            return -1;
        }

        Pattern Parse(const char* p)
        {
            Pattern out;
            for (size_t i = 0; p[i];)
            {
                const char c = p[i];
                if (c == ' ' || c == '\t') { ++i; continue; }
                if (c == '?')
                {
                    out.bytes.push_back(0); out.mask.push_back(false);
                    ++i; if (p[i] == '?') ++i;
                    continue;
                }
                const int hi = Nibble(c);
                if (hi < 0) { ++i; continue; }
                int lo = hi;
                if (Nibble(p[i + 1]) >= 0) { lo = Nibble(p[i + 1]); i += 2; } else ++i;
                out.bytes.push_back(static_cast<uint8_t>((hi << 4) | lo)); out.mask.push_back(true);
            }
            while (out.firstFixed < out.bytes.size() && !out.mask[out.firstFixed]) ++out.firstFixed;
            return out;
        }

        bool ReadableProtect(DWORD protect)
        {
            if (protect & PAGE_GUARD) return false;
            if (protect == PAGE_NOACCESS || protect == PAGE_EXECUTE) return false;
            return (protect & (PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READ |
                               PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) != 0;
        }

        // Committed, readable, merged spans of the module. The exe is packed and
        // its section flags are not trustworthy, so walk the real pages.
        std::vector<std::pair<uintptr_t, uintptr_t>> Spans()
        {
            std::vector<std::pair<uintptr_t, uintptr_t>> out;
            const Module& m = Game();
            if (!m.base) return out;
            const uintptr_t end = m.base + m.size;
            uintptr_t a = m.base;
            MEMORY_BASIC_INFORMATION mbi{};
            while (a < end && VirtualQuery(reinterpret_cast<LPCVOID>(a), &mbi, sizeof mbi) == sizeof mbi)
            {
                const uintptr_t rb = reinterpret_cast<uintptr_t>(mbi.BaseAddress);
                uintptr_t re = rb + mbi.RegionSize;
                if (re > end) re = end;
                if (mbi.State == MEM_COMMIT && ReadableProtect(mbi.Protect))
                {
                    if (!out.empty() && out.back().second == rb) out.back().second = re;
                    else out.emplace_back(rb, re);
                }
                if (mbi.RegionSize == 0) break;
                a = rb + mbi.RegionSize;
            }
            return out;
        }

        // Visits every hit in address order until visit() returns true.
        uintptr_t ScanAll(const Pattern& pat, bool (*visit)(uintptr_t, void*), void* ctx)
        {
            const size_t len = pat.bytes.size();
            if (!len || pat.firstFixed >= len) return 0;
            const uint8_t anchor = pat.bytes[pat.firstFixed];
            for (const auto& sp : Spans())
            {
                if (sp.second - sp.first < len) continue;
                const uint8_t* p    = reinterpret_cast<const uint8_t*>(sp.first);
                const uint8_t* last = reinterpret_cast<const uint8_t*>(sp.second - len);
                for (; p <= last; ++p)
                {
                    if (p[pat.firstFixed] != anchor) continue;
                    bool hit = true;
                    for (size_t i = 0; i < len; ++i)
                        if (pat.mask[i] && p[i] != pat.bytes[i]) { hit = false; break; }
                    if (hit && visit(reinterpret_cast<uintptr_t>(p), ctx)) return reinterpret_cast<uintptr_t>(p);
                }
            }
            return 0;
        }

        struct CountCtx { size_t n, max; uintptr_t first; };
        bool CountVisit(uintptr_t hit, void* c)
        {
            auto* cc = static_cast<CountCtx*>(c);
            if (!cc->n) cc->first = hit;
            return ++cc->n >= cc->max;
        }
    }

    uintptr_t Find(const char* pattern)
    {
        const Pattern pat = Parse(pattern);
        CountCtx cc{ 0, 1, 0 };
        ScanAll(pat, CountVisit, &cc);
        return cc.first;
    }
    size_t Count(const char* pattern, size_t maxCount)
    {
        const Pattern pat = Parse(pattern);
        CountCtx cc{ 0, maxCount, 0 };
        ScanAll(pat, CountVisit, &cc);
        return cc.n;
    }
    uintptr_t FindIf(const char* pattern, bool (*visit)(uintptr_t, void*), void* ctx)
    {
        const Pattern pat = Parse(pattern);
        return ScanAll(pat, visit, ctx);
    }
    uintptr_t FindUnique(const char* pattern, size_t* hitsOut)
    {
        const Pattern pat = Parse(pattern);
        CountCtx cc{ 0, 2, 0 };
        ScanAll(pat, CountVisit, &cc);
        if (hitsOut) *hitsOut = cc.n;
        return cc.n == 1 ? cc.first : 0;
    }
    bool MatchAt(uintptr_t addr, const char* pattern)
    {
        const Pattern pat = Parse(pattern);
        const size_t len = pat.bytes.size();
        if (!len) return false;
        uint8_t buf[128];
        if (len > sizeof buf || !ReadBytes(addr, buf, len)) return false;
        for (size_t i = 0; i < len; ++i)
            if (pat.mask[i] && buf[i] != pat.bytes[i]) return false;
        return true;
    }

    uintptr_t RipAt(uintptr_t instr, unsigned instrLen)
    {
        uint32_t d = 0;
        if (!Read32(instr + instrLen - 4, &d)) return 0;
        return instr + instrLen + static_cast<int32_t>(d);
    }

    // -------------------------------------------------------------- RTTI ----
    const char* RttiName(uintptr_t obj)
    {
        uintptr_t vt = 0, col = 0;
        if (!ReadPtr(obj, &vt) || !InImage(vt)) return nullptr;
        if (!ReadPtr(vt - 8, &col) || !InImage(col)) return nullptr;
        uint32_t sig = 0, tdOff = 0;
        if (!Read32(col, &sig) || sig != 1) return nullptr;
        if (!Read32(col + 0x0C, &tdOff) || !tdOff) return nullptr;
        const uintptr_t td = Game().base + tdOff;
        if (!InImage(td + 0x10)) return nullptr;
        uint8_t c = 0;
        if (!Read8(td + 0x10, &c) || c != '.') return nullptr;
        return reinterpret_cast<const char*>(td + 0x10);
    }
    const char* RttiShort(uintptr_t obj)
    {
        const char* n = RttiName(obj);
        if (n && n[0] == '.' && n[1] == '?' && n[2] == 'A') return n + 4;
        return n;
    }

    int FindVtablesByName(const char* name, uintptr_t* out, int max)
    {
        const Module& m = Game();
        if (!m.base) return 0;
        const size_t nlen = strlen(name) + 1;
        // TypeDescriptor: the decorated name lives at td+0x10.
        uintptr_t td = 0;
        for (const auto& sp : Spans())
        {
            if (sp.second - sp.first < nlen) continue;
            const uint8_t* p = reinterpret_cast<const uint8_t*>(sp.first);
            const uint8_t* last = reinterpret_cast<const uint8_t*>(sp.second - nlen);
            for (; p <= last; ++p)
                if (*p == '.' && memcmp(p, name, nlen) == 0) { td = reinterpret_cast<uintptr_t>(p) - 0x10; break; }
            if (td) break;
        }
        if (!td) return 0;
        const uint32_t tdRva = static_cast<uint32_t>(td - m.base);
        int n = 0;
        // COL: +0x00 signature 1, +0x0C TypeDescriptor RVA. The vtable follows
        // the qword that points at the COL.
        std::vector<uintptr_t> cols;
        for (const auto& sp : Spans())
        {
            for (uintptr_t p = sp.first & ~3ull; p + 0x18 <= sp.second; p += 4)
            {
                const uint32_t* q = reinterpret_cast<const uint32_t*>(p);
                if (q[3] == tdRva && q[0] == 1) cols.push_back(p);
            }
        }
        for (uintptr_t col : cols)
        {
            for (const auto& sp : Spans())
            {
                for (uintptr_t p = sp.first & ~7ull; p + 16 <= sp.second && n < max; p += 8)
                    if (*reinterpret_cast<const uint64_t*>(p) == col) { out[n++] = p + 8; break; }
                if (n >= max) break;
            }
        }
        return n;
    }

    uintptr_t FindGlobalHoldingVtable(const uintptr_t* vtables, int n, long* candidates)
    {
        const Module& m = Game();
        if (!m.base || n <= 0) return 0;
        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(m.base);
        const auto* nt  = reinterpret_cast<const IMAGE_NT_HEADERS64*>(m.base + dos->e_lfanew);
        const IMAGE_SECTION_HEADER* sec = IMAGE_FIRST_SECTION(nt);
        long cand = 0;
        for (int si = 0; si < nt->FileHeader.NumberOfSections; ++si)
        {
            const DWORD ch = sec[si].Characteristics;
            // Writable, non-executable data only: the game's globals live in
            // .srdata; .debug$P is huge and flagged both writable and executable.
            if (!(ch & IMAGE_SCN_MEM_WRITE) || (ch & IMAGE_SCN_MEM_EXECUTE)) continue;
            uintptr_t b = m.base + sec[si].VirtualAddress;
            uintptr_t e = b + sec[si].Misc.VirtualSize;
            if (e > m.base + m.size) e = m.base + m.size;
            for (uintptr_t rp = b; rp < e;)
            {
                MEMORY_BASIC_INFORMATION mbi;
                if (!VirtualQuery(reinterpret_cast<LPCVOID>(rp), &mbi, sizeof mbi)) break;
                uintptr_t rEnd = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
                if (rEnd > e) rEnd = e;
                if (mbi.State != MEM_COMMIT || (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD))) { rp = rEnd; continue; }
                for (uintptr_t p = rp & ~7ull; p + 8 <= rEnd; p += 8)
                {
                    const uint64_t v = *reinterpret_cast<const uint64_t*>(p);
                    if (!Plausible(static_cast<uintptr_t>(v)) || InImage(static_cast<uintptr_t>(v))) continue;
                    ++cand;
                    uint64_t head = 0;
                    if (!Read64(static_cast<uintptr_t>(v), &head)) continue;
                    for (int i = 0; i < n; ++i)
                        if (head == vtables[i]) { if (candidates) *candidates = cand; return p; }
                }
                rp = rEnd;
            }
        }
        if (candidates) *candidates = cand;
        return 0;
    }
}
