// Stage slots: path_to_hash detour. See stage_redirect.hpp.

#include <windows.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdint.h>
#include <string.h>
#include <atomic>
#include <memory>
#include <string>
#include <vector>

#include "stage_redirect.hpp"

namespace {

wchar_t g_log_path[MAX_PATH];
wchar_t g_rules_path[MAX_PATH];
ULONGLONG g_start_tick = 0;
CRITICAL_SECTION g_log_cs;

// Nothing is hooked before this much time has passed since the game started: tools that hook
// path_to_hash themselves (REFramework's loose file loader) find it by walking the code from
// via.io.file.exists and give up when the entry is already a jump.
const ULONGLONG HOOK_DELAY_MS = 30000;
const int MAX_PATH_CHARS = 1024;
const int MAX_LOGGED_PATHS = 20000;

std::atomic<long> g_logged_paths{0};
std::atomic<long long> g_calls{0};
std::atomic<long long> g_redirects{0};

void logf(const char* fmt, ...) {
    EnterCriticalSection(&g_log_cs);
    FILE* f = nullptr;
    if (_wfopen_s(&f, g_log_path, L"a") == 0 && f) {
        SYSTEMTIME st;
        GetLocalTime(&st);
        fprintf(f, "[%02d:%02d:%02d.%03d] ", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
        va_list ap;
        va_start(ap, fmt);
        vfprintf(f, fmt, ap);
        va_end(ap);
        fputc('\n', f);
        fclose(f);
    }
    LeaveCriticalSection(&g_log_cs);
}

std::string narrow(const wchar_t* w) {
    char buf[MAX_PATH_CHARS * 3];
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, buf, sizeof(buf), nullptr, nullptr);
    return n > 0 ? std::string(buf) : std::string("?");
}

// ---------------------------------------------------------------------------------------------
// Rules. Published as an immutable block through an atomic pointer; a block is never freed
// (the detour may still be reading it), a few bytes per edit of the file.
// ---------------------------------------------------------------------------------------------

struct Rule { std::wstring from, to; };     // from: lower case
struct Rules {
    std::vector<Rule> redirects;
    std::vector<std::wstring> traces;       // lower case
};

std::atomic<const Rules*> g_rules{nullptr};
std::vector<std::unique_ptr<Rules>> g_all_rules;   // watcher thread only

inline wchar_t lower(wchar_t c) { return (c >= L'A' && c <= L'Z') ? wchar_t(c + 32) : c; }

inline bool match_at(const wchar_t* s, size_t left, const std::wstring& lo) {
    if (lo.size() > left) return false;
    for (size_t k = 0; k < lo.size(); ++k)
        if (lower(s[k]) != lo[k]) return false;
    return true;
}

bool contains_ci(const wchar_t* s, size_t n, const std::wstring& lo) {
    if (lo.empty() || lo.size() > n) return false;
    for (size_t i = 0; i + lo.size() <= n; ++i)
        if (match_at(s + i, n - i, lo)) return true;
    return false;
}

// Writes the rewritten path into out. False when no rule applies (or it would not fit).
bool apply_redirects(const Rules* r, const wchar_t* in, size_t n, wchar_t* out, size_t cap) {
    bool changed = false;
    size_t o = 0, i = 0;
    while (i < n) {
        const Rule* hit = nullptr;
        for (const auto& ru : r->redirects)
            if (match_at(in + i, n - i, ru.from)) { hit = &ru; break; }
        if (hit) {
            if (o + hit->to.size() + 1 > cap) return false;
            memcpy(out + o, hit->to.data(), hit->to.size() * sizeof(wchar_t));
            o += hit->to.size();
            i += hit->from.size();
            changed = true;
        } else {
            if (o + 2 > cap) return false;
            out[o++] = in[i++];
        }
    }
    out[o] = 0;
    return changed;
}

// ---------------------------------------------------------------------------------------------
// The detour
// ---------------------------------------------------------------------------------------------

using PathToHashFn = uint64_t (*)(const wchar_t* path);
PathToHashFn g_original = nullptr;

void log_path(const char* tag, const wchar_t* path, const wchar_t* to) {
    if (g_logged_paths.fetch_add(1) >= MAX_LOGGED_PATHS) return;
    if (to) logf("%s %s -> %s", tag, narrow(path).c_str(), narrow(to).c_str());
    else    logf("%s %s", tag, narrow(path).c_str());
}

uint64_t detour(const wchar_t* path) {
    g_calls.fetch_add(1, std::memory_order_relaxed);
    const Rules* r = g_rules.load(std::memory_order_acquire);
    if (r == nullptr || path == nullptr) return g_original(path);

    size_t n = wcsnlen(path, MAX_PATH_CHARS);
    if (n == 0 || n >= MAX_PATH_CHARS) return g_original(path);

    for (const auto& t : r->traces) {
        if (contains_ci(path, n, t)) { log_path("trace", path, nullptr); break; }
    }
    if (r->redirects.empty()) return g_original(path);

    wchar_t buf[MAX_PATH_CHARS + 64];
    if (!apply_redirects(r, path, n, buf, sizeof(buf) / sizeof(buf[0]))) return g_original(path);

    g_redirects.fetch_add(1, std::memory_order_relaxed);
    log_path("redirect", path, buf);
    return g_original(buf);
}

// ---------------------------------------------------------------------------------------------
// Finding path_to_hash in the game executable.
//
// Its end is unique and stable: two murmur3 calls (seed 0xFFFFFFFF, upper then lower case
// copy of the path) whose results are packed as (upper << 32) | lower:
//     41 B8 FF FF FF FF       mov r8d, 0xFFFFFFFF
//     4C 8D 4C 24 24          lea r9, [rsp+24h]
//     48 8B CB                mov rcx, rbx
//     E8 ?? ?? ?? ??          call murmur3
//     8B 44 24 20             mov eax, [rsp+20h]
//     8B 4C 24 24             mov ecx, [rsp+24h]
//     48 C1 E0 20             shl rax, 20h
//     48 0B C1                or rax, rcx
// The entry is found walking back to the int3 padding. It starts with
//     40 55 53 41 56          push rbp / push rbx / push r14
// or with a 5-byte jump when another tool already hooked it, then
//     48 8D AC 24             lea rbp, [rsp-...]
// ---------------------------------------------------------------------------------------------

const int16_t TAIL[] = {
    0x41, 0xB8, 0xFF, 0xFF, 0xFF, 0xFF, 0x4C, 0x8D, 0x4C, 0x24, 0x24, 0x48, 0x8B, 0xCB,
    0xE8, -1, -1, -1, -1, 0x8B, 0x44, 0x24, 0x20, 0x8B, 0x4C, 0x24, 0x24,
    0x48, 0xC1, 0xE0, 0x20, 0x48, 0x0B, 0xC1,
};
const size_t TAIL_LEN = sizeof(TAIL) / sizeof(TAIL[0]);

bool tail_at(const uint8_t* p) {
    for (size_t k = 0; k < TAIL_LEN; ++k)
        if (TAIL[k] >= 0 && p[k] != (uint8_t)TAIL[k]) return false;
    return true;
}

bool entry_at(const uint8_t* p) {
    if (((uintptr_t)p & 0xF) != 0 || p[-1] != 0xCC) return false;
    bool original = p[0] == 0x40 && p[1] == 0x55 && p[2] == 0x53 && p[3] == 0x41 && p[4] == 0x56;
    bool jumped = p[0] == 0xE9;
    return (original || jumped) && p[5] == 0x48 && p[6] == 0x8D && p[7] == 0xAC && p[8] == 0x24;
}

// murmur3's body constant 0xCC9E2D51 somewhere in the first bytes of the called function
bool is_murmur(const uint8_t* fn) {
    for (int k = 0; k < 0x100; ++k)
        if (fn[k] == 0x51 && fn[k + 1] == 0x2D && fn[k + 2] == 0x9E && fn[k + 3] == 0xCC) return true;
    return false;
}

struct ScanResult { uintptr_t entry; int tails; int entries; };

// SEH only, no C++ objects: the executable is protected and some pages may not be readable.
void scan_section(const uint8_t* begin, size_t size, ScanResult* res) {
    __try {
        const uint8_t* end = begin + size - TAIL_LEN;
        for (const uint8_t* p = begin; p < end; ++p) {
            p = (const uint8_t*)memchr(p, 0x41, end - p);
            if (!p) break;
            if (!tail_at(p)) continue;
            res->tails++;
            int32_t rel;
            memcpy(&rel, p + 15, 4);
            const uint8_t* callee = p + 19 + rel;
            if (!is_murmur(callee)) continue;
            for (int back = 0x100; back <= 0x1000; ++back) {
                const uint8_t* e = p - back;
                if (entry_at(e)) { res->entries++; res->entry = (uintptr_t)e; break; }
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

uintptr_t find_path_to_hash() {
    auto base = (const uint8_t*)GetModuleHandleW(nullptr);
    auto dos = (const IMAGE_DOS_HEADER*)base;
    auto nt = (const IMAGE_NT_HEADERS64*)(base + dos->e_lfanew);
    auto sec = IMAGE_FIRST_SECTION(nt);
    ScanResult res{0, 0, 0};
    for (int i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
        if (!(sec[i].Characteristics & IMAGE_SCN_MEM_EXECUTE)) continue;
        size_t size = sec[i].Misc.VirtualSize;
        if (size < 0x1000) continue;
        scan_section(base + sec[i].VirtualAddress, size, &res);
    }
    logf("scan: %d tail(s), %d entr%s", res.tails, res.entries, res.entries == 1 ? "y" : "ies");
    if (res.entries != 1) return 0;
    logf("path_to_hash at exe+0x%llx (first byte %02X)", (unsigned long long)(res.entry - (uintptr_t)base),
         *(const uint8_t*)res.entry);
    return res.entry;
}

// ---------------------------------------------------------------------------------------------
// The hook itself. The entry gets a 5-byte relative jump to a relay, so the relay must sit
// within 2 GB of the executable; late in a session that space is taken (the executable alone
// is 600 MB), hence a small block reserved at startup, from DllMain.
//   relay:      jmp [rip+0] -> detour
//   trampoline: what the entry did: the jump of the tool that hooked it first (absolute), or
//               the original 5 bytes (push rbp / push rbx / push r14) then back to entry+5.
// The entry is 16-byte aligned: the 5 bytes are replaced by one atomic 8-byte exchange.
// ---------------------------------------------------------------------------------------------

uint8_t* g_near = nullptr;

void reserve_near_block() {
    auto base = (uintptr_t)GetModuleHandleW(nullptr);
    auto dos = (const IMAGE_DOS_HEADER*)base;
    auto nt = (const IMAGE_NT_HEADERS64*)(base + dos->e_lfanew);
    uintptr_t image_end = base + nt->OptionalHeader.SizeOfImage;
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    const uintptr_t gran = si.dwAllocationGranularity;
    const uintptr_t reach = 0x60000000;
    const uintptr_t lowest = (uintptr_t)si.lpMinimumApplicationAddress;
    // below the image, walking down
    for (uintptr_t a = (base - gran) & ~(gran - 1); a > lowest && base - a < reach;) {
        MEMORY_BASIC_INFORMATION mbi;
        if (!VirtualQuery((LPCVOID)a, &mbi, sizeof(mbi))) break;
        if (mbi.State == MEM_FREE) {
            void* p = VirtualAlloc((LPVOID)a, 0x1000, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
            if (p) { g_near = (uint8_t*)p; return; }
            a -= gran;
        } else {
            uintptr_t ab = (uintptr_t)mbi.AllocationBase;
            if (ab < gran) break;
            a = (ab - gran) & ~(gran - 1);
        }
    }
    // above the image, walking up
    for (uintptr_t a = (image_end + gran - 1) & ~(gran - 1); a - image_end < reach;) {
        MEMORY_BASIC_INFORMATION mbi;
        if (!VirtualQuery((LPCVOID)a, &mbi, sizeof(mbi))) break;
        if (mbi.State == MEM_FREE) {
            void* p = VirtualAlloc((LPVOID)a, 0x1000, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
            if (p) { g_near = (uint8_t*)p; return; }
            a += gran;
        } else {
            a = ((uintptr_t)mbi.BaseAddress + mbi.RegionSize + gran - 1) & ~(gran - 1);
        }
    }
}

void write_abs_jmp(uint8_t* at, uintptr_t dest) {
    at[0] = 0xFF; at[1] = 0x25;                 // jmp qword ptr [rip+0]
    at[2] = at[3] = at[4] = at[5] = 0;
    memcpy(at + 6, &dest, 8);
}

bool install_hook() {
    uintptr_t target = find_path_to_hash();
    if (!target) { logf("path_to_hash not found: no redirection this session"); return false; }
    if (!g_near) { logf("no memory block near the executable: no redirection this session"); return false; }
    auto entry = (uint8_t*)target;

    uint64_t before = *(volatile uint64_t*)entry;
    uint8_t* relay = g_near;
    uint8_t* tramp = g_near + 32;
    write_abs_jmp(relay, (uintptr_t)&detour);
    if (entry[0] == 0xE9) {
        int32_t rel;
        memcpy(&rel, entry + 1, 4);
        uintptr_t dest = target + 5 + (intptr_t)rel;
        write_abs_jmp(tramp, dest);
        logf("entry already hooked (jump to 0x%llx): chained", (unsigned long long)dest);
    } else if (entry[0] == 0x40 && entry[1] == 0x55 && entry[2] == 0x53 && entry[3] == 0x41 && entry[4] == 0x56) {
        memcpy(tramp, entry, 5);
        write_abs_jmp(tramp + 5, target + 5);
    } else {
        logf("unexpected entry bytes %02X %02X %02X %02X %02X", entry[0], entry[1], entry[2], entry[3], entry[4]);
        return false;
    }
    FlushInstructionCache(GetCurrentProcess(), g_near, 64);
    g_original = (PathToHashFn)tramp;

    intptr_t rel = (intptr_t)relay - (intptr_t)(target + 5);
    if (rel > INT32_MAX || rel < INT32_MIN) { logf("relay out of reach"); return false; }
    uint64_t after = (before & 0xFFFFFF0000000000ull) | 0xE9ull | ((uint64_t)(uint32_t)(int32_t)rel << 8);

    DWORD old = 0;
    if (!VirtualProtect(entry, 8, PAGE_EXECUTE_READWRITE, &old)) { logf("VirtualProtect failed (%lu)", GetLastError()); return false; }
    bool ok = InterlockedCompareExchange64((volatile LONG64*)entry, (LONG64)after, (LONG64)before) == (LONG64)before;
    VirtualProtect(entry, 8, old, &old);
    FlushInstructionCache(GetCurrentProcess(), entry, 8);
    if (!ok) { logf("entry changed while hooking: not hooked"); return false; }
    logf("hook installed (relay at 0x%llx)", (unsigned long long)(uintptr_t)relay);
    return true;
}

// ---------------------------------------------------------------------------------------------
// Rules file watcher
// ---------------------------------------------------------------------------------------------

std::wstring trim_lower(const std::wstring& s, bool lower_case) {
    size_t a = s.find_first_not_of(L" \t\r\n");
    size_t b = s.find_last_not_of(L" \t\r\n");
    std::wstring r = a == std::wstring::npos ? std::wstring() : s.substr(a, b - a + 1);
    if (lower_case) for (auto& c : r) c = lower(c);
    return r;
}

std::unique_ptr<Rules> read_rules() {
    auto rules = std::make_unique<Rules>();
    FILE* f = nullptr;
    if (_wfopen_s(&f, g_rules_path, L"rb") != 0 || !f) return rules;
    char line[2048];
    while (fgets(line, sizeof(line), f)) {
        wchar_t w[2048];
        if (MultiByteToWideChar(CP_UTF8, 0, line, -1, w, 2048) <= 0) continue;
        std::wstring s(w);
        if (s.size() >= 1 && s[0] == 0xFEFF) s.erase(0, 1);
        size_t eq = s.find(L'=');
        if (eq == std::wstring::npos || s[0] == L'#') continue;
        std::wstring key = trim_lower(s.substr(0, eq), true);
        std::wstring val = trim_lower(s.substr(eq + 1), false);
        if (key.empty()) continue;
        if (key == L"trace") { if (!val.empty()) rules->traces.push_back(trim_lower(val, true)); }
        else rules->redirects.push_back({key, val});
    }
    fclose(f);
    return rules;
}

void publish(std::unique_ptr<Rules> rules) {
    const Rules* p = nullptr;
    if (rules && (!rules->redirects.empty() || !rules->traces.empty())) {
        p = rules.get();
        g_all_rules.push_back(std::move(rules));
    }
    g_rules.store(p, std::memory_order_release);
    if (!p) { logf("rules: none"); return; }
    for (const auto& r : p->redirects) logf("rule: %s -> %s", narrow(r.from.c_str()).c_str(), narrow(r.to.c_str()).c_str());
    for (const auto& t : p->traces) logf("trace: %s", narrow(t.c_str()).c_str());
}

DWORD WINAPI watcher(LPVOID) {
    FILETIME last{};
    bool present = false, hooked = false, gave_up = false;
    long long last_calls = -1;
    ULONGLONG last_stats = 0;
    for (;;) {
        Sleep(250);
        WIN32_FILE_ATTRIBUTE_DATA fa;
        bool exists = GetFileAttributesExW(g_rules_path, GetFileExInfoStandard, &fa) != 0;
        if (!exists) {
            if (present) { present = false; publish(nullptr); }
        } else if (!present || CompareFileTime(&fa.ftLastWriteTime, &last) != 0) {
            present = true;
            last = fa.ftLastWriteTime;
            publish(read_rules());
        }
        ULONGLONG now = GetTickCount64();
        if (!hooked && !gave_up && g_rules.load() != nullptr && now - g_start_tick >= HOOK_DELAY_MS) {
            hooked = install_hook();
            gave_up = !hooked;
        }
        if (hooked && now - last_stats >= 10000) {
            last_stats = now;
            long long c = g_calls.load();
            if (c != last_calls) {
                last_calls = c;
                logf("stats: %lld paths hashed, %lld redirected", c, g_redirects.load());
            }
        }
    }
}

} // namespace

void stage_slots_start(const wchar_t* game_dir) {
    g_start_tick = GetTickCount64();
    InitializeCriticalSection(&g_log_cs);
    wcscpy_s(g_log_path, game_dir);
    wcscat_s(g_log_path, L"\\SF6_StageSlots.log");
    wcscpy_s(g_rules_path, game_dir);
    wcscat_s(g_rules_path, L"\\reframework\\data\\SF6_StageSlots_Data\\redirect.txt");
    {
        FILE* f = nullptr;   // fresh log each launch
        if (_wfopen_s(&f, g_log_path, L"w") == 0 && f) fclose(f);
    }
    logf("=== SF6 Stage Slots ===");
    reserve_near_block();
    if (g_near) logf("near block at 0x%llx", (unsigned long long)(uintptr_t)g_near);
    else logf("no free block near the executable");
    HANDLE t = CreateThread(nullptr, 0, watcher, nullptr, 0, nullptr);
    if (t) CloseHandle(t);
    else logf("cannot start the watcher thread");
}
