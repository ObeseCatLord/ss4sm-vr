#include "se4/options.hpp"
#include "se4/log.hpp"
#include <windows.h>
#include <cstdlib>
#include <cstring>

namespace se4::options {
namespace {
struct Entry {
    char key[48];
    int value;
};
Entry g_entries[32];
int g_count;
}  // namespace

void load(const wchar_t* inDir) {
    wchar_t path[MAX_PATH];
    wcsncpy(path, inDir, MAX_PATH - 1);
    path[MAX_PATH - 1] = 0;
    wchar_t* slash = wcsrchr(path, L'\\');
    if (!slash || slash - path > MAX_PATH - 16) return;
    wcscpy(slash + 1, L"options.txt");
    HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    char buf[4096];
    DWORD n = 0;
    ReadFile(f, buf, sizeof buf - 1, &n, nullptr);
    CloseHandle(f);
    buf[n] = 0;
    for (char* line = strtok(buf, "\r\n"); line && g_count < 32; line = strtok(nullptr, "\r\n")) {
        if (char* hash = strchr(line, '#')) *hash = 0;
        char* eq = strchr(line, '=');
        if (!eq) continue;
        *eq = 0;
        char* k = line;
        while (*k == ' ' || *k == '\t') ++k;
        char* end = eq;
        while (end > k && (end[-1] == ' ' || end[-1] == '\t')) *--end = 0;
        if (!*k || strlen(k) >= sizeof g_entries[0].key) continue;
        strcpy(g_entries[g_count].key, k);
        g_entries[g_count].value = int(strtol(eq + 1, nullptr, 0));
        VRLOG("option %s = %d", k, g_entries[g_count].value);
        ++g_count;
    }
}

int get(const char* key, int fallback) {
    for (int i = 0; i < g_count; ++i)
        if (!strcmp(g_entries[i].key, key)) return g_entries[i].value;
    return fallback;
}

bool has(const char* key) {
    for (int i = 0; i < g_count; ++i)
        if (!strcmp(g_entries[i].key, key)) return true;
    return false;
}
}  // namespace se4::options
