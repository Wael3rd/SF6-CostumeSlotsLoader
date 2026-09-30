// SF6 AMD AGS proxy: Costume Slot Loader + Stage Slots
// ------------------------------------------------------
// The costume loader's proxy (amd_ags_proxy.cpp, built from its sources, unchanged) with the
// stage slots around it. The game imports amd_ags_x64.dll from its own folder, so this DLL is
// mapped before any engine code runs: the right moment to rebuild the paks.
//
// DllMain: (1) stage_loader_run: stage pak, right above the mod paks (SEH-wrapped)
//          (2) costume_loader_run: costume pak, above the stage pak (SEH-wrapped)
//          (3) stage_slots_start: reserves a small block near the executable and starts the
//              watcher thread that hooks path_to_hash once a stage variant is selected
//          (4) if amd_ags_x64_chain.dll exists next to us, load it from a thread
// All AGS exports are forwarded to amd_ags_x64_real.dll via linker directives: no AGS code here.
//
// Chaining another tool that also wants amd_ags_x64.dll (HARD READ, MatchScout, ...):
//   its  amd_ags_x64.dll  -> rename to  amd_ags_x64_chain.dll
//   the real AMD library  -> amd_ags_x64_real.dll   (both proxies forward to it)

#include <windows.h>
#include <stdio.h>
#include "ags_forwarders.h"
#include "loader_core.hpp"
#include "../stage_loader.hpp"
#include "../stage_log.hpp"
#include "../stage_redirect.hpp"

// Tell loader_core that we are NOT the exe (suppress stdout output).
bool g_is_exe = false;

static const wchar_t* CHAIN_NAME = L"amd_ags_x64_chain.dll";

static wchar_t g_chain_path[MAX_PATH];
static wchar_t g_dll_dir[MAX_PATH];
static std::vector<StageVariant> g_stage_variants;     // read by the watcher for the whole session

// Appends one line to the loader log (same file as loader_core), so a chained module is visible
// in support logs. Runs on its own thread, after DllMain returned.
static void chain_log(const wchar_t* path, bool ok) {
    char log_path[MAX_PATH * 2];
    int n = WideCharToMultiByte(CP_UTF8, 0, g_dll_dir, -1, log_path, sizeof(log_path) - 32, nullptr, nullptr);
    if (n <= 0) return;
    strcat_s(log_path, sizeof(log_path), "\\SF6_CostumeLoader.log");
    FILE* f = nullptr;
    if (fopen_s(&f, log_path, "a") != 0 || !f) return;
    char mod[MAX_PATH * 2];
    if (WideCharToMultiByte(CP_UTF8, 0, path, -1, mod, sizeof(mod), nullptr, nullptr) <= 0) mod[0] = 0;
    fprintf(f, "  chain: %s %s\n", mod, ok ? "loaded" : "FAILED");
    fclose(f);
}

static void run_stage_loader() {
    __try {
        stage_loader_run(g_dll_dir, g_stage_variants);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        slog("stage loader: unexpected fault, stages left as they were");
    }
}

static DWORD WINAPI load_chain(LPVOID) {
    HMODULE h = LoadLibraryW(g_chain_path);
    chain_log(g_chain_path, h != nullptr);
    return 0;
}

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID reserved) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(module);

        DWORD n = GetModuleFileNameW(module, g_dll_dir, MAX_PATH);
        if (n == 0 || n >= MAX_PATH) return TRUE;
        for (int i = (int)n - 1; i >= 0; --i) {
            if (g_dll_dir[i] == L'\\' || g_dll_dir[i] == L'/') { g_dll_dir[i] = 0; break; }
        }

        // ---- Stage slots: stage pak first (it sits below the costume pak) ----
        slog_open(g_dll_dir);
        run_stage_loader();

        // ---- Costume loader: synchronous, SEH-wrapped. Never crash the game. ----
        __try {
            costume_loader_run(g_dll_dir, nullptr);
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {
            // Silently swallow: the log file (if opened) holds the diagnostics.
        }

        // ---- Stage slots: watcher thread only, nothing is hooked from DllMain ----
        stage_slots_start(g_dll_dir, &g_stage_variants);

        // ---- Optional chained proxy, opt-in by file presence, off the loader lock ----
        wcscpy_s(g_chain_path, MAX_PATH, g_dll_dir);
        wcscat_s(g_chain_path, MAX_PATH, L"\\");
        wcscat_s(g_chain_path, MAX_PATH, CHAIN_NAME);
        if (GetFileAttributesW(g_chain_path) != INVALID_FILE_ATTRIBUTES) {
            HANDLE t = CreateThread(nullptr, 0, load_chain, nullptr, 0, nullptr);
            if (t) CloseHandle(t);
        }
    }
    return TRUE;
}
