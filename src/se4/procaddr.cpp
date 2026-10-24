#include "se4/procaddr.hpp"
#include "se4/build.hpp"
#include "se4/log.hpp"
#include <MinHook.h>
#include <cwchar>

namespace se4 {
namespace {
using GetProcAddressFn = FARPROC(WINAPI*)(HMODULE, LPCSTR);
GetProcAddressFn realGetProcAddress;

FARPROC WINAPI hookGetProcAddress(HMODULE mod, LPCSTR name) {
    FARPROC real = realGetProcAddress(mod, name);
    if (!real || !callerInGame(__builtin_return_address(0))) return real;
    if (reinterpret_cast<uintptr_t>(name) < 0x10000) {
        // Ordinal lookups: xinput1_4 #100 is XInputGetStateEx (same signature as XInputGetState).
        wchar_t file[MAX_PATH];
        if (GetModuleFileNameW(mod, file, MAX_PATH) && wcsstr(file, L"xinput") && reinterpret_cast<uintptr_t>(name) == 100)
            return gamepad::interceptProc(mod, "XInputGetStateEx", real);
        return real;
    }
    if (name[0] == 'v' && name[1] == 'k') return vk_layer::interceptProc(mod, name, real);
    if (name[0] == 'X' && name[1] == 'I') return gamepad::interceptProc(mod, name, real);
    return real;
}
}  // namespace

bool callerInGame(void* ret) {
    const auto& b = build();
    if (!b.table) return false;
    auto a = reinterpret_cast<uintptr_t>(ret);
    return a >= b.base && a < b.base + b.table->sizeOfImage;
}

bool installProcAddressHook() {
    void* target = reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "GetProcAddress"));
    if (!target || MH_CreateHook(target, reinterpret_cast<void*>(hookGetProcAddress), reinterpret_cast<void**>(&realGetProcAddress)) != MH_OK ||
        MH_EnableHook(target) != MH_OK) {
        VRLOG("GetProcAddress hook FAILED");
        return false;
    }
    VRLOG("GetProcAddress hook installed");
    return true;
}
}  // namespace se4
