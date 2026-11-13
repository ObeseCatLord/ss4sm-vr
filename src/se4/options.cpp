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
constexpr int kMaxEntries = 256;
Entry g_entries[kMaxEntries];
int g_count;
char g_buf[32768];  // static: load runs early (no heap, small stack)
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
    DWORD n = 0;
    const BOOL ok = ReadFile(f, g_buf, sizeof g_buf - 1, &n, nullptr);
    DWORD more = 0;
    char probe;
    const bool truncated = ok && n == sizeof g_buf - 1 && ReadFile(f, &probe, 1, &more, nullptr) && more;
    CloseHandle(f);
    g_buf[ok ? n : 0] = 0;
    char* text = g_buf;
    if (static_cast<unsigned char>(text[0]) == 0xEF && static_cast<unsigned char>(text[1]) == 0xBB && static_cast<unsigned char>(text[2]) == 0xBF)
        text += 3;  // UTF-8 byte order mark (Notepad)
    int dropped = 0;
    for (char* line = strtok(text, "\r\n"); line; line = strtok(nullptr, "\r\n")) {
        if (char* hash = strchr(line, '#')) *hash = 0;
        char* eq = strchr(line, '=');
        if (!eq) continue;
        *eq = 0;
        char* k = line;
        while (*k == ' ' || *k == '\t') ++k;
        char* end = eq;
        while (end > k && (end[-1] == ' ' || end[-1] == '\t')) *--end = 0;
        if (!*k || strlen(k) >= sizeof g_entries[0].key) continue;
        const int value = int(strtol(eq + 1, nullptr, 0));
        int i = 0;
        while (i < g_count && strcmp(g_entries[i].key, k)) ++i;  // a repeated key: the last one wins
        if (i == g_count) {
            if (g_count == kMaxEntries) {
                ++dropped;
                continue;
            }
            strcpy(g_entries[g_count++].key, k);
        }
        g_entries[i].value = value;
        VRLOG("option %s = %d", k, value);
    }
    if (dropped || truncated) VRLOG("options: %d entries ignored%s (limit %d entries, %u bytes)", dropped, truncated ? ", file truncated" : "",
                                    kMaxEntries, unsigned(sizeof g_buf - 1));
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
