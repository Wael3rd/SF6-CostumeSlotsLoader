// costume_loader.exe -- thin wrapper calling loader_core
// Usage: costume_loader.exe <game_dir> [--base-pak <path>]

#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif
#define NOMINMAX
#include <windows.h>
#include <cstdio>
#include <cstring>
#include "loader_core.hpp"

bool g_is_exe = true;

int main(int argc, char* argv[]) {
    if (argc < 2) {
        fprintf(stderr, "usage: costume_loader.exe <game_dir> [--base-pak <path>]\n");
        return 1;
    }

    // Convert game_dir to wide
    const char* gd = argv[1];
    wchar_t game_dir[MAX_PATH];
    MultiByteToWideChar(CP_ACP, 0, gd, -1, game_dir, MAX_PATH);

    // Optional base-pak override
    wchar_t base_pak[MAX_PATH] = {};
    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "--base-pak") == 0 && i + 1 < argc) {
            MultiByteToWideChar(CP_ACP, 0, argv[++i], -1, base_pak, MAX_PATH);
        }
    }

    return costume_loader_run(game_dir, base_pak[0] ? base_pak : nullptr);
}
