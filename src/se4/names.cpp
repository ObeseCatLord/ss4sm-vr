#include "se4/names.hpp"
#include "se4/build.hpp"
#include <windows.h>

namespace se4::names {
namespace {
bool readable(const void* p, size_t n) {
    MEMORY_BASIC_INFORMATION mbi;
    if (!p || reinterpret_cast<uintptr_t>(p) < 0x10000 || !VirtualQuery(p, &mbi, sizeof mbi) || mbi.State != MEM_COMMIT ||
        (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) || !(mbi.Protect & (PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE)))
        return false;
    return static_cast<const uint8_t*>(mbi.BaseAddress) + mbi.RegionSize >= static_cast<const uint8_t*>(p) + n;
}
}  // namespace

const char* ascii(const void* p) {
    if (!readable(p, 1)) return nullptr;
    MEMORY_BASIC_INFORMATION mbi;
    VirtualQuery(p, &mbi, sizeof mbi);
    const char* s = static_cast<const char*>(p);
    const size_t room = static_cast<const uint8_t*>(mbi.BaseAddress) + mbi.RegionSize - static_cast<const uint8_t*>(p);
    for (size_t i = 0; i < room && i < 260; ++i) {
        if (!s[i]) return i >= 3 ? s : nullptr;
        if (s[i] < 0x20 || s[i] > 0x7e) return nullptr;
    }
    return nullptr;
}

namespace {
// Weapon params are long-lived resources: their file name is looked up (with its memory checks) once per params.
struct NameOf {
    const uint8_t* params;
    const char* name;
};
NameOf g_names[32];
int g_nextName;
SRWLOCK g_nameLock = SRWLOCK_INIT;
const char* lookup(uint8_t* params);
}  // namespace

const char* weaponFile(uint8_t* weapon) {
    const uint32_t off = se4::build().off(se4::gen::CWeaponEntity_en_pwpWeaponParams);
    if (!weapon || !off) return nullptr;
    uint8_t* params = *reinterpret_cast<uint8_t**>(weapon + off);  // a live weapon entity: its own field is readable
    if (!params) return nullptr;
    AcquireSRWLockShared(&g_nameLock);
    for (const auto& n : g_names)
        if (n.params == params) {
            const char* s = n.name;
            ReleaseSRWLockShared(&g_nameLock);
            return s;
        }
    ReleaseSRWLockShared(&g_nameLock);
    const char* s = lookup(params);
    if (s) {
        AcquireSRWLockExclusive(&g_nameLock);
        g_names[g_nextName] = {params, s};
        g_nextName = (g_nextName + 1) % 32;
        ReleaseSRWLockExclusive(&g_nameLock);
    }
    return s;
}

namespace {
const char* lookup(uint8_t* params) {
    if (!readable(params + 0x10, 8)) return nullptr;
    uint8_t* prf = *reinterpret_cast<uint8_t**>(params + 0x10);
    if (!readable(prf ? prf + 0x18 : nullptr, 8)) return nullptr;
    void* field = *reinterpret_cast<void**>(prf + 0x18);
    if (const char* s = ascii(field)) return s;
    return readable(field, 8) ? ascii(*static_cast<void**>(field)) : nullptr;
}
}  // namespace
}  // namespace se4::names
