// NGX for MinGW: the DLSS SDK's Vulkan helpers (nvsdk_ngx_helpers_vk.h) call NVSDK_NGX_Parameter_* and
// NVSDK_NGX_VULKAN_* functions that normally come from the MSVC static library nvsdk_ngx_s.lib. Here they are
// provided by this file:
//   - NVSDK_NGX_VULKAN_*: forwarded to the driver's NGX core (_nvngx.dll), loaded at runtime from the driver
//     store path in the registry (Windows) or system32 (Proton), as the SDK's own loader does.
//   - NVSDK_NGX_Parameter_*: calls into the parameter object's vtable. The object is created by the MSVC-built
//     core, so its vtable has MSVC layout, in which a group of overloaded virtual functions appears in reverse
//     declaration order. NVSDK_NGX_Parameter declares Set x8, Get x8, Reset, which gives the slots below.
#include "up/ngx_shim.hpp"
#include "se4/log.hpp"
#include <windows.h>
#include <cstring>
#include <initializer_list>

namespace ngx {
namespace {
HMODULE g_core;
enum Slot : unsigned {
    SetVoidPtr = 0, SetD3D12, SetD3D11, SetI, SetUI, SetD, SetF, SetULL,
    GetVoidPtr = 8, GetD3D12, GetD3D11, GetI, GetUI, GetD, GetF, GetULL,
    ResetSlot = 16,
};
void** vt(NVSDK_NGX_Parameter* p) { return *reinterpret_cast<void***>(p); }

bool regPath(HKEY root, const wchar_t* key, const wchar_t* value, wchar_t* out) {
    HKEY k;
    if (RegOpenKeyExW(root, key, 0, KEY_READ, &k) != ERROR_SUCCESS) return false;
    DWORD size = MAX_PATH * sizeof(wchar_t), type = 0;
    bool ok = RegQueryValueExW(k, value, nullptr, &type, reinterpret_cast<BYTE*>(out), &size) == ERROR_SUCCESS && type == REG_SZ;
    RegCloseKey(k);
    return ok;
}
}  // namespace

bool load() {
    if (g_core) return true;
    wchar_t dir[MAX_PATH] = {};
    const struct { const wchar_t* key; const wchar_t* value; } keys[] = {
        {L"System\\CurrentControlSet\\Services\\nvlddmkm\\Parameters\\NGXCore", L"NGXPath"},
        {L"System\\CurrentControlSet\\Services\\nvlddmkm\\NGXCore", L"NGXPath"},
        {L"SOFTWARE\\NVIDIA Corporation\\Global\\NGXCore", L"FullPath"},
    };
    for (const auto& k : keys) {
        if (!regPath(HKEY_LOCAL_MACHINE, k.key, k.value, dir)) continue;
        for (const wchar_t* name : {L"\\_nvngx.dll", L"\\nvngx.dll"}) {
            wchar_t path[MAX_PATH * 2];
            wcscpy(path, dir);
            wcscat(path, name);
            if ((g_core = LoadLibraryW(path))) break;
        }
        if (g_core) break;
    }
    if (!g_core) g_core = LoadLibraryW(L"_nvngx.dll");  // Proton: system32
    if (!g_core) g_core = LoadLibraryW(L"nvngx.dll");
    VRLOG("ngx: core %s", g_core ? "loaded" : "not found (no NVIDIA driver NGX)");
    return g_core != nullptr;
}

FARPROC sym(const char* name) { return g_core ? GetProcAddress(g_core, name) : nullptr; }

bool selfTest(NVSDK_NGX_Parameter* p) {
    // Round trips through the assumed slots; a wrong layout shows up as mismatches (or a fault, which is why
    // this runs once before any feature is created).
    NVSDK_NGX_Parameter_SetUI(p, "ss4smvr.test.ui", 0x1234u);
    NVSDK_NGX_Parameter_SetF(p, "ss4smvr.test.f", 1.5f);
    NVSDK_NGX_Parameter_SetULL(p, "ss4smvr.test.ull", 0x100000005ull);
    unsigned int ui = 0;
    float f = 0;
    unsigned long long ull = 0;
    bool ok = NVSDK_NGX_Parameter_GetUI(p, "ss4smvr.test.ui", &ui) == NVSDK_NGX_Result_Success && ui == 0x1234u &&
              NVSDK_NGX_Parameter_GetF(p, "ss4smvr.test.f", &f) == NVSDK_NGX_Result_Success && f == 1.5f &&
              NVSDK_NGX_Parameter_GetULL(p, "ss4smvr.test.ull", &ull) == NVSDK_NGX_Result_Success && ull == 0x100000005ull;
    VRLOG("ngx: parameter vtable self-test %s (ui %x f %.2f ull %llx)", ok ? "passed" : "FAILED", ui, f, ull);
    return ok;
}
}  // namespace ngx

// ---- NVSDK_NGX_Parameter_* (MSVC vtable layout) ----
using namespace ngx;
#define NGX_SET(fn, T, slot) \
    void NVSDK_CONV fn(NVSDK_NGX_Parameter* p, const char* n, T v) { reinterpret_cast<void (*)(NVSDK_NGX_Parameter*, const char*, T)>(vt(p)[slot])(p, n, v); }
#define NGX_GET(fn, T, slot)                                                                                              \
    NVSDK_NGX_Result NVSDK_CONV fn(NVSDK_NGX_Parameter* p, const char* n, T* v) {                                         \
        return reinterpret_cast<NVSDK_NGX_Result (*)(NVSDK_NGX_Parameter*, const char*, T*)>(vt(p)[slot])(p, n, v);       \
    }
extern "C" {
NGX_SET(NVSDK_NGX_Parameter_SetULL, unsigned long long, SetULL)
NGX_SET(NVSDK_NGX_Parameter_SetF, float, SetF)
NGX_SET(NVSDK_NGX_Parameter_SetD, double, SetD)
NGX_SET(NVSDK_NGX_Parameter_SetUI, unsigned int, SetUI)
NGX_SET(NVSDK_NGX_Parameter_SetI, int, SetI)
NGX_SET(NVSDK_NGX_Parameter_SetVoidPointer, void*, SetVoidPtr)
NGX_GET(NVSDK_NGX_Parameter_GetULL, unsigned long long, GetULL)
NGX_GET(NVSDK_NGX_Parameter_GetF, float, GetF)
NGX_GET(NVSDK_NGX_Parameter_GetD, double, GetD)
NGX_GET(NVSDK_NGX_Parameter_GetUI, unsigned int, GetUI)
NGX_GET(NVSDK_NGX_Parameter_GetI, int, GetI)
NGX_GET(NVSDK_NGX_Parameter_GetVoidPointer, void*, GetVoidPtr)
}

// ---- NVSDK_NGX_VULKAN_* forwarders (only the ones the mod and the helpers use) ----
#define NGX_FWD(ret, name, params, args)                                \
    ret NVSDK_CONV name params {                                        \
        static auto f = reinterpret_cast<ret(NVSDK_CONV*) params>(sym(#name)); \
        return f ? f args : NVSDK_NGX_Result_FAIL_NotImplemented;       \
    }
NGX_FWD(NVSDK_NGX_Result, NVSDK_NGX_VULKAN_CreateFeature,
        (VkCommandBuffer cb, NVSDK_NGX_Feature id, NVSDK_NGX_Parameter* p, NVSDK_NGX_Handle** h), (cb, id, p, h))
NGX_FWD(NVSDK_NGX_Result, NVSDK_NGX_VULKAN_CreateFeature1,
        (VkDevice d, VkCommandBuffer cb, NVSDK_NGX_Feature id, NVSDK_NGX_Parameter* p, NVSDK_NGX_Handle** h), (d, cb, id, p, h))
NGX_FWD(NVSDK_NGX_Result, NVSDK_NGX_VULKAN_EvaluateFeature,
        (VkCommandBuffer cb, const NVSDK_NGX_Handle* h, const NVSDK_NGX_Parameter* p, PFN_NVSDK_NGX_ProgressCallback cbk), (cb, h, p, cbk))
NGX_FWD(NVSDK_NGX_Result, NVSDK_NGX_VULKAN_ReleaseFeature, (NVSDK_NGX_Handle* h), (h))
NGX_FWD(NVSDK_NGX_Result, NVSDK_NGX_VULKAN_GetCapabilityParameters, (NVSDK_NGX_Parameter** p), (p))
NGX_FWD(NVSDK_NGX_Result, NVSDK_NGX_VULKAN_AllocateParameters, (NVSDK_NGX_Parameter** p), (p))
NGX_FWD(NVSDK_NGX_Result, NVSDK_NGX_VULKAN_DestroyParameters, (NVSDK_NGX_Parameter* p), (p))
NGX_FWD(NVSDK_NGX_Result, NVSDK_NGX_VULKAN_Shutdown1, (VkDevice d), (d))
NGX_FWD(NVSDK_NGX_Result, NVSDK_NGX_VULKAN_RequiredExtensions,
        (unsigned int* ic, const char*** ie, unsigned int* dc, const char*** de), (ic, ie, dc, de))
// The helpers call the "_C" variant (C progress callback); the core exports only the C++ one, which takes the
// same arguments. The helpers always pass a null callback.
NVSDK_NGX_Result NVSDK_CONV NVSDK_NGX_VULKAN_EvaluateFeature_C(VkCommandBuffer cb, const NVSDK_NGX_Handle* h, const NVSDK_NGX_Parameter* p,
                                                              PFN_NVSDK_NGX_ProgressCallback_C) {
    return NVSDK_NGX_VULKAN_EvaluateFeature(cb, h, p, nullptr);
}
