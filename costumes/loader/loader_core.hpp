#pragma once

// costume_loader_run: main entry point for the costume slot loader.
// game_dir: wide path to the SF6 game root (where re_chunk_000.pak lives).
// base_pak_override: if non-null, use this path for re_chunk_000.pak instead
//                    of <game_dir>\re_chunk_000.pak (for testing).
// Returns 0 on success, non-zero on error.
// Safe to call from DllMain(DLL_PROCESS_ATTACH): uses only kernel32+bcrypt,
// static CRT (/MT), no LoadLibrary, no COM, no thread sync.
int costume_loader_run(const wchar_t* game_dir, const wchar_t* base_pak_override = nullptr);
