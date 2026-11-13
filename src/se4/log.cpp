#include "se4/log.hpp"
#include <windows.h>
#include <cstdarg>
#include <cstdio>

namespace se4 {
namespace {
CRITICAL_SECTION g_lock;
HANDLE g_file = INVALID_HANDLE_VALUE;
LARGE_INTEGER g_t0, g_freq;
wchar_t g_dir[MAX_PATH];
}

void logOpen(const wchar_t* path) {
    InitializeCriticalSection(&g_lock);
    wcsncpy(g_dir, path, MAX_PATH - 1);
    if (wchar_t* slash = wcsrchr(g_dir, L'\\')) *slash = 0;
    QueryPerformanceFrequency(&g_freq);
    QueryPerformanceCounter(&g_t0);
    const DWORD share = FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE;
    g_file = CreateFileW(path, GENERIC_WRITE, share, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (g_file == INVALID_HANDLE_VALUE) {
        // A stale handle (e.g. a killed previous run still known to wineserver) can block the usual name.
        wchar_t alt[MAX_PATH];
        _snwprintf(alt, MAX_PATH, L"%ls\\ss4smvr-%lu.log", g_dir, GetCurrentProcessId());
        g_file = CreateFileW(alt, GENERIC_WRITE, share, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    }
}

void payloadDirectory(wchar_t* out, unsigned size) {
    wcsncpy(out, g_dir, size - 1);
    out[size - 1] = 0;
}

void logf(const char* fmt, ...) {
    if (g_file == INVALID_HANDLE_VALUE) return;
    char buf[2048];
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    double t = double(now.QuadPart - g_t0.QuadPart) / double(g_freq.QuadPart);
    int n = snprintf(buf, sizeof buf, "[%9.3f %5lu] ", t, GetCurrentThreadId());
    va_list ap;
    va_start(ap, fmt);
    int m = vsnprintf(buf + n, sizeof buf - n - 2, fmt, ap);
    va_end(ap);
    if (m < 0) m = 0;
    n += (m < int(sizeof buf) - n - 2) ? m : int(sizeof buf) - n - 3;
    buf[n++] = '\r';
    buf[n++] = '\n';
    EnterCriticalSection(&g_lock);
    DWORD w;
    WriteFile(g_file, buf, DWORD(n), &w, nullptr);  // in the OS's hands: survives a crash of the game
    // To disk at most every 2 s (a whole-system hang, e.g. a GPU fault): a flush per line stalled the frame.
    static ULONGLONG lastFlush;
    if (const ULONGLONG ms = GetTickCount64(); ms - lastFlush > 2000) FlushFileBuffers(g_file), lastFlush = ms;
    LeaveCriticalSection(&g_lock);
}
}  // namespace se4
