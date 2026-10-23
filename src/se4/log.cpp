#include "se4/log.hpp"
#include <windows.h>
#include <cstdarg>
#include <cstdio>

namespace se4 {
namespace {
CRITICAL_SECTION g_lock;
HANDLE g_file = INVALID_HANDLE_VALUE;
LARGE_INTEGER g_t0, g_freq;
}

void logOpen(const wchar_t* path) {
    InitializeCriticalSection(&g_lock);
    QueryPerformanceFrequency(&g_freq);
    QueryPerformanceCounter(&g_t0);
    g_file = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
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
    WriteFile(g_file, buf, DWORD(n), &w, nullptr);
    FlushFileBuffers(g_file);
    LeaveCriticalSection(&g_lock);
}
}  // namespace se4
