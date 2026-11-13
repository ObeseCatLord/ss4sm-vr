#include "se4/patch.hpp"
#include <windows.h>
#include <cstring>

namespace se4::patch {
namespace {
// A region within rel32 reach of `target`, searched downwards from it.
uint8_t* reserveNear(void* target, size_t size, DWORD protect) {
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    uintptr_t gran = si.dwAllocationGranularity;
    uintptr_t start = reinterpret_cast<uintptr_t>(target) & ~(gran - 1);
    for (uintptr_t a = start - gran; a > start - 0x7ff00000ull; a -= gran) {
        void* p = VirtualAlloc(reinterpret_cast<void*>(a), size, MEM_RESERVE | MEM_COMMIT, protect);
        if (p) return static_cast<uint8_t*>(p);
    }
    return nullptr;
}
// Bump allocation from one region per kind (code: executable; data: read-write only), a new region when the current
// one is full or out of rel32 reach of the target. Patches are installed on one thread at startup.
struct Arena {
    uint8_t* base = nullptr;
    size_t used = 0;
    DWORD protect;
};
Arena g_code{nullptr, 0, PAGE_EXECUTE_READWRITE}, g_data{nullptr, 0, PAGE_READWRITE};
constexpr size_t kArena = 0x10000;
bool inReach(const uint8_t* p, const void* target) {
    const intptr_t d = reinterpret_cast<intptr_t>(p) - reinterpret_cast<intptr_t>(target);
    return d > -0x7ff00000ll && d < 0x7ff00000ll;
}
uint8_t* take(Arena& a, void* target, size_t size) {
    size = (size + 15) & ~size_t(15);
    if (size > kArena) return reserveNear(target, size, a.protect);
    if (!a.base || a.used + size > kArena || !inReach(a.base, target) || !inReach(a.base + kArena, target)) {
        uint8_t* fresh = reserveNear(target, kArena, a.protect);
        if (!fresh) return nullptr;
        a.base = fresh, a.used = 0;
    }
    uint8_t* p = a.base + a.used;
    a.used += size;
    return p;
}
}  // namespace

uint8_t* allocNear(void* target, size_t size) { return take(g_code, target, size); }
uint8_t* allocNearData(void* target, size_t size) { return take(g_data, target, size); }

uint8_t* nearJump(void* imageAddress, void* fn) {
    static uint8_t* block;
    static size_t used;
    constexpr size_t kBlock = 4096, kEntry = 16;
    if (!block || used + kEntry > kBlock) {
        block = allocNear(imageAddress, kBlock);
        used = 0;
        if (!block) return nullptr;
    }
    uint8_t* e = block + used;
    used += kEntry;
    // jmp qword ptr [rip+0]; dq fn
    const uint8_t code[6] = {0xFF, 0x25, 0, 0, 0, 0};
    memcpy(e, code, 6);
    memcpy(e + 6, &fn, 8);
    FlushInstructionCache(GetCurrentProcess(), e, kEntry);
    return e;
}

int findReturnOneCalls(uint8_t* fn, size_t len, uint8_t** out, int max) {
    static const uint8_t ret1[6] = {0xB8, 0x01, 0x00, 0x00, 0x00, 0xC3};
    int n = 0;
    for (size_t i = 0; i + 5 <= len; ++i) {
        if (fn[i] != 0xE8) continue;
        int32_t rel;
        memcpy(&rel, fn + i + 1, 4);
        const uint8_t* t = fn + i + 5 + rel;
        MEMORY_BASIC_INFORMATION mi;
        if (!VirtualQuery(t, &mi, sizeof mi) || mi.State != MEM_COMMIT || !(mi.Protect & 0xF0)) continue;  // executable
        if (memcmp(t, ret1, 6)) continue;
        if (n < max) out[n] = fn + i;
        ++n;
    }
    return n;
}

bool writeCode(uint8_t* site, const uint8_t* expect, const uint8_t* bytes, size_t n) {
    if (!site || memcmp(site, expect, n)) return false;
    DWORD old;
    if (!VirtualProtect(site, n, PAGE_EXECUTE_READWRITE, &old)) return false;
    memcpy(site, bytes, n);
    VirtualProtect(site, n, old, &old);
    FlushInstructionCache(GetCurrentProcess(), site, n);
    return true;
}
bool retargetCall(uint8_t* site, void* newTarget) {
    if (site[0] != 0xE8) return false;
    intptr_t rel = static_cast<uint8_t*>(newTarget) - (site + 5);
    if (rel != int32_t(rel)) return false;
    DWORD old;
    if (!VirtualProtect(site, 5, PAGE_EXECUTE_READWRITE, &old)) return false;
    int32_t r = int32_t(rel);
    memcpy(site + 1, &r, 4);
    VirtualProtect(site, 5, old, &old);
    FlushInstructionCache(GetCurrentProcess(), site, 5);
    return true;
}
}  // namespace se4::patch
