#pragma once
#include <string>

// SF6_StageSlots.log next to the game executable. slog_open truncates it (once per launch);
// slog is thread-safe (the path_to_hash detour runs on the engine's loading threads).
void slog_open(const wchar_t* game_dir);
void slog(const char* fmt, ...);

std::string narrow(const std::wstring& w);
std::wstring widen(const std::string& s);
