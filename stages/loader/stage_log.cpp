#include <windows.h>
#include <stdarg.h>
#include <stdio.h>

#include "stage_log.hpp"

static wchar_t g_log_path[MAX_PATH];
static CRITICAL_SECTION g_log_cs;
static bool g_log_ready = false;

void slog_open(const wchar_t* game_dir) {
    if (g_log_ready) return;
    InitializeCriticalSection(&g_log_cs);
    wcscpy_s(g_log_path, game_dir);
    wcscat_s(g_log_path, L"\\SF6_StageSlots.log");
    FILE* f = nullptr;
    if (_wfopen_s(&f, g_log_path, L"w") == 0 && f) fclose(f);
    g_log_ready = true;
}

void slog(const char* fmt, ...) {
    if (!g_log_ready) return;
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

std::string narrow(const std::wstring& w) {
    if (w.empty()) return std::string();
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n > 0 ? n : 0, '\0');
    if (n > 0) WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &s[0], n, nullptr, nullptr);
    return s;
}

std::wstring widen(const std::string& s) {
    if (s.empty()) return std::wstring();
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring w(n > 0 ? n : 0, L'\0');
    if (n > 0) MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], n);
    return w;
}
