#pragma once
// Thread-safe line logger writing next to the payload (ss4smvr/ss4smvr.log).
namespace se4 {
void logOpen(const wchar_t* path);
// Directory of the payload (where the log lives), without a trailing separator.
void payloadDirectory(wchar_t* out, unsigned size);
void logf(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
}
#define VRLOG(...) ::se4::logf(__VA_ARGS__)
