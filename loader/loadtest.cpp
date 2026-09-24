// loadtest.exe -- Verifies amd_ags_x64.dll loads and runs costume_loader_run
// via DllMain in a simulated game root.
//
// Usage: loadtest.exe <simulated_game_root>
//
// The simulated root should contain:
//   re_chunk_000.pak.patch_001.pak, patch_002.pak (mod paks)
//   amd_ags_x64.dll (the proxy we built)
//   reframework\data\SF6_Costumes_Data\loader\  (static + tsv)
// The real re_chunk_000.pak is found via SF6_COSTUME_LOADER_BASE_PAK env var.

#include <windows.h>
#include <cstdio>
#include <cstring>

int main(int argc, char* argv[]) {
    if (argc < 2) {
        fprintf(stderr, "usage: loadtest.exe <simulated_game_root>\n");
        return 1;
    }
    const char* root = argv[1];
    printf("=== loadtest ===\n");
    printf("Simulated root: %s\n", root);

    // Set working directory to the simulated root so the DLL finds its files
    SetCurrentDirectoryA(root);

    // Build path to the DLL
    char dll_path[MAX_PATH];
    snprintf(dll_path, MAX_PATH, "%s\\amd_ags_x64.dll", root);

    printf("Loading %s ...\n", dll_path);

    LARGE_INTEGER freq, t0, t1;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&t0);

    HMODULE h = LoadLibraryA(dll_path);

    QueryPerformanceCounter(&t1);
    double ms = (double)(t1.QuadPart - t0.QuadPart) / freq.QuadPart * 1000.0;

    if (!h) {
        DWORD err = GetLastError();
        fprintf(stderr, "LoadLibrary FAILED: error %lu\n", err);
        return 1;
    }

    printf("LoadLibrary OK in %.1f ms\n", ms);

    // Check the log file
    char log_path[MAX_PATH];
    snprintf(log_path, MAX_PATH, "%s\\SF6_CostumeLoader.log", root);
    FILE* lf = fopen(log_path, "r");
    if (lf) {
        printf("\n--- SF6_CostumeLoader.log ---\n");
        char line[512];
        while (fgets(line, sizeof(line), lf)) printf("%s", line);
        fclose(lf);
        printf("--- end log ---\n");
    } else {
        printf("No log file found.\n");
    }

    // Check if patch_003 was generated
    char pak_path[MAX_PATH];
    snprintf(pak_path, MAX_PATH, "%s\\re_chunk_000.pak.patch_003.pak", root);
    HANDLE pf = CreateFileA(pak_path, GENERIC_READ, FILE_SHARE_READ, nullptr,
                            OPEN_EXISTING, 0, nullptr);
    if (pf != INVALID_HANDLE_VALUE) {
        LARGE_INTEGER sz;
        GetFileSizeEx(pf, &sz);
        CloseHandle(pf);
        printf("\npatch_003.pak generated: %lld bytes\n", sz.QuadPart);
    } else {
        printf("\nNo patch_003.pak found.\n");
    }

    FreeLibrary(h);
    printf("\nDone.\n");
    return 0;
}
