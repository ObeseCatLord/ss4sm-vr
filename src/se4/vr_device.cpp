// COpenXRDevice. Every slot is a plain C function: MinGW x64 and MSVC x64 share the calling convention,
// so `this` is the first parameter (RCX) and MSVC's hidden struct-return pointer is the second (RDX).
// Floats travel in the XMM register of their argument position. See docs/re/R1_vtable.md.
#include "se4/vr_device.hpp"
#include "core/math.hpp"
#include "se4/build.hpp"
#include "se4/log.hpp"
#include "vk/eye_images.hpp"
#include "xr/input.hpp"
#include "xr/runtime.hpp"
#include <windows.h>
#include <MinHook.h>
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <utility>

namespace vrdev {
namespace {
constexpr unsigned kSlots = 72;           // SS4 uses 0x000..0x210 (67 slots); extra entries log and return 0
constexpr size_t kBaseSize = 0xb70;       // engine-visible base area (Fusion CVRDevice size)

struct alignas(16) Object {
    void** vtbl;
    uint8_t base[kBaseSize - sizeof(void*)];
    // private state after the base area
    void* gfxNative;
};
Object g_obj;
void* g_vtbl[kSlots];
volatile LONG g_calls[kSlots];
volatile LONG g_seen[kSlots];
bool g_installed;

template <class T> T& field(uint32_t off) { return *reinterpret_cast<T*>(reinterpret_cast<uint8_t*>(&g_obj) + off); }

void note(unsigned slot, void* ret) {
    InterlockedIncrement(&g_calls[slot]);
    if (!InterlockedExchange(&g_seen[slot], 1)) VRLOG("vrdev: first call slot 0x%03x from %p", slot * 8, ret);
}
#define NOTE(off) note((off) / 8, __builtin_return_address(0))

float worldScale() {
    auto* s = se4::build().at<float>(se4::gen::vr_fWorldScale);
    return (s && *s > 0.01f) ? *s : 1.0f;
}

using CStringCtor = void*(__cdecl*)(void* self, const char* text);
void* makeString(void* ret, const char* text) {
    auto ctor = se4::build().at<void>(se4::gen::cstring_ctor);
    if (ctor) reinterpret_cast<CStringCtor>(ctor)(ret, text);
    return ret;
}

core::Pose toPose(const XrPosef& p) {
    return {{p.orientation.x, p.orientation.y, p.orientation.z, p.orientation.w}, {p.position.x, p.position.y, p.position.z}};
}
void writePlacement(float* out, core::Pose p) {
    float s = worldScale();
    out[0] = p.q.x, out[1] = p.q.y, out[2] = p.q.z, out[3] = p.q.w;
    out[4] = p.p.x / s, out[5] = p.p.y / s, out[6] = p.p.z / s;
}
void identityPlacement(float* out) {
    memset(out, 0, 7 * sizeof(float));
    out[3] = 1;
}

// ---- generic stub: logs once, returns 0 ----
template <unsigned N> uintptr_t stub(void*) {
    note(N, __builtin_return_address(0));
    return 0;
}
template <unsigned... I> void fillStubs(std::integer_sequence<unsigned, I...>) { ((g_vtbl[I] = reinterpret_cast<void*>(&stub<I>)), ...); }

// ---- implemented slots ----
void* sDtor(Object* self, unsigned flags) {
    NOTE(0x000);
    VRLOG("vrdev: destructor (flags %u)", flags);
    return self;
}
int sInit(Object*) { NOTE(0x008); return 1; }
void sSetGfx(Object* self, void* native) {
    NOTE(0x010);
    self->gfxNative = native;
    VRLOG("vrdev: graphics device handles %p", native);
}
void sSubmit(Object*, int eye, void*, void*) {
    NOTE(0x018);
    eyes::submitted(eye);
}
void sPhase2(Object*) { NOTE(0x020); }
// Eye render targets are engine render-target holders created exactly like the VR viewer's own
// (engine_alloc(0x90) + holder ctor + AddRef) and (re)created with holder_create(holder, gfx, w, h,
// 2, 0, 0, 0), the call the engine uses for its single-pass target minus the double-width flag.
// The slot returns *(holder_get(holder) + 8), the same object the engine renders into (R1 0x028).
using HolderCreateFn = int(__cdecl*)(void* holder, void* gfx, int w, int h, int fmt, int p6, unsigned flags, int p8);
using HolderGetFn = void*(__cdecl*)(void* holder);
using HolderCtorFn = void*(__cdecl*)(void* mem);
using AddRefFn = void(__cdecl*)(void* obj, void* owner);
using AllocFn2 = void*(__cdecl*)(size_t size, uintptr_t tag);
struct EyeTarget {
    void* holder = nullptr;
    int w = 0, h = 0;
};
EyeTarget g_eye[2];

void eyeSize(int& w, int& h) {
    const auto& v = xr::runtime().views();
    w = int(v.recommendedWidth ? v.recommendedWidth : 1344);
    h = int(v.recommendedHeight ? v.recommendedHeight : 1512);
    if (auto* ratio = se4::build().at<float>(se4::gen::vr_fRenderingPixelRatio); ratio && *ratio > 0.25f && *ratio < 4.0f) {
        w = int(w * *ratio + 0.5f);
        h = int(h * *ratio + 0.5f);
    }
}

void* sEyeRT(Object*, void* gfx, int eye) {
    NOTE(0x028);
    if (eye != 1 && eye != 2) return nullptr;
    const auto& b = se4::build();
    auto alloc = reinterpret_cast<AllocFn2>(b.at<void>(se4::gen::engine_alloc));
    auto ctor = reinterpret_cast<HolderCtorFn>(b.at<void>(se4::gen::holder_ctor));
    auto addref = reinterpret_cast<AddRefFn>(b.at<void>(se4::gen::ref_addref));
    auto create = reinterpret_cast<HolderCreateFn>(b.at<void>(se4::gen::holder_create));
    auto get = reinterpret_cast<HolderGetFn>(b.at<void>(se4::gen::holder_get));
    if (!alloc || !ctor || !addref || !create || !get) return nullptr;
    EyeTarget& t = g_eye[eye - 1];
    if (!t.holder) {
        void* mem = alloc(0x90, 0);
        if (!mem) return nullptr;
        t.holder = ctor(mem);
        addref(t.holder, &t.holder);
        VRLOG("vrdev: eye %d render-target holder %p", eye, t.holder);
    }
    int w, h;
    eyeSize(w, h);
    if (w != t.w || h != t.h) {
        eyes::beginCapture(eye, uint32_t(w), uint32_t(h));
        int id = create(t.holder, gfx, w, h, 2, 0, 0, 0);
        eyes::endCapture();
        t.w = w, t.h = h;
        VRLOG("vrdev: eye %d render target %dx%d (engine id %d)", eye, w, h, id);
    }
    void* wrapper = get(t.holder);
    return wrapper ? *reinterpret_cast<void**>(static_cast<uint8_t*>(wrapper) + 8) : nullptr;
}
void sBeginFrame(Object*) { NOTE(0x030); }
void sPhase1(Object*) { NOTE(0x038); }
void sPhase3(Object*, void*) { NOTE(0x040); }
void sPhase4(Object*) { NOTE(0x048); }
void sStep(Object*) { NOTE(0x050); }
int sAdapter(Object*, int, uint64_t* luid) {
    NOTE(0x058);
    if (luid) *luid = 0;
    return -1;
}
int* sRecSize(Object*, int* ret) {
    NOTE(0x060);
    const auto& v = xr::runtime().views();
    ret[0] = int(v.recommendedWidth ? v.recommendedWidth : 1344);
    ret[1] = int(v.recommendedHeight ? v.recommendedHeight : 1512);
    return ret;
}
int* sMaxSize(Object* self, int* ret) {
    NOTE(0x068);
    return sRecSize(self, ret);
}
float* sProjection(Object*, float* m, int eye, float n, float f) {
    NOTE(0x070);
    memset(m, 0, 16 * sizeof(float));
    const auto& v = xr::runtime().views();
    if ((eye != 1 && eye != 2) || !v.valid) {
        m[0] = m[5] = m[10] = m[15] = 1;
        return m;
    }
    // Horizontal extent per eye; vertical extent shared by both eyes (as the engine expects, R1 0x070).
    const XrFovf& e = v.fov[eye - 1];
    float l = n * std::tan(e.angleLeft), r = n * std::tan(e.angleRight);
    float t = n * std::tan(std::max(v.fov[0].angleUp, v.fov[1].angleUp));
    float b = n * std::tan(std::min(v.fov[0].angleDown, v.fov[1].angleDown));
    m[0] = 2 * n / (r - l), m[2] = (r + l) / (r - l);
    m[5] = 2 * n / (t - b), m[6] = (t + b) / (t - b);
    m[10] = -(f + n) / (f - n), m[11] = -2 * f * n / (f - n);
    m[14] = -1;
    return m;
}
float* sEyeTransform(Object*, float* m, int eye) {
    NOTE(0x078);
    const auto& v = xr::runtime().views();
    core::Pose p{};
    if ((eye == 1 || eye == 2) && v.valid) {
        p = toPose(v.inView[eye - 1].pose);
        p.p = p.p * (1.0f / worldScale());
        p = core::inverse(p);  // head-to-eye, as Fusion returns it
    }
    core::matrix34(p, m);
    return m;
}
float* sHead(Object*, float* ret) {
    NOTE(0x080);
    const auto& v = xr::runtime().views();
    if (!v.valid) {
        identityPlacement(ret);
        ret[5] = 1.7f / worldScale();
        return ret;
    }
    writePlacement(ret, toPose(v.head));
    return ret;
}
void sSetHead(Object*, const float*) { NOTE(0x088); }
float sZeroF(Object*) { return 0.0f; }
int sCtrlPlacement(Object*, int role, float* out) {
    NOTE(0x0b8);
    int hand = role == 0 ? 1 : role == 1 ? 0 : -1;  // engine role 0 = right, 1 = left
    if (hand < 0 || !xr::input().hand(hand).active) {
        identityPlacement(out);
        return 0;
    }
    writePlacement(out, toPose(xr::input().hand(hand).grip));
    return 1;
}
int sCtrlPlacement2(Object* self, int role, float* out) {
    NOTE(0x0c0);
    return sCtrlPlacement(self, role, out);
}
float sButton(Object*, int role, int button, unsigned* flags) {
    NOTE(0x0d0);
    if (flags) *flags = 0;
    int hand = role == 0 ? 1 : role == 1 ? 0 : -1;
    if (hand < 0) return 0;
    if (button == 0) return xr::input().hand(hand).trigger;
    return 0;
}
void* sName(Object*, void* ret) { NOTE(0x110); return makeString(ret, "Vive"); }
void* sCtrlTypeName(Object*, void* ret, int) { NOTE(0x118); return makeString(ret, "Vive"); }
const char* sRoleSerial(Object*, int) { NOTE(0x0b0); return ""; }
int sOne(Object*) { return 1; }
int sApi(Object*) { NOTE(0x148); return 1; }  // "SteamVR" code paths
void* sVkInstExt(Object*, void* ret) { NOTE(0x1a8); return makeString(ret, ""); }
void* sVkDevExt(Object*, void* ret, void*) { NOTE(0x1b0); return makeString(ret, ""); }
int sHasDesktop(Object*) { NOTE(0x1b8); return 1; }
const void* sHiddenMesh(Object*, int, int* count) {
    NOTE(0x178);
    if (count) *count = 0;
    return nullptr;  // OpenXR masks are not index-paired between eyes (R1 0x178)
}
float sFingerCurl(Object*, int, int) { NOTE(0x1e0); return -1.0f; }
void* sSerial(Object*, void* ret, int) { NOTE(0x198); return makeString(ret, ""); }

void buildTable() {
    fillStubs(std::make_integer_sequence<unsigned, kSlots>{});
    auto set = [](unsigned off, auto fn) { g_vtbl[off / 8] = reinterpret_cast<void*>(fn); };
    set(0x000, sDtor);
    set(0x008, sInit);
    set(0x010, sSetGfx);
    set(0x018, sSubmit);
    set(0x020, sPhase2);
    set(0x028, sEyeRT);
    set(0x030, sBeginFrame);
    set(0x038, sPhase1);
    set(0x040, sPhase3);
    set(0x048, sPhase4);
    set(0x050, sStep);
    set(0x058, sAdapter);
    set(0x060, sRecSize);
    set(0x068, sMaxSize);
    set(0x070, sProjection);
    set(0x078, sEyeTransform);
    set(0x080, sHead);
    set(0x088, sSetHead);
    set(0x090, sZeroF);
    set(0x098, sZeroF);
    set(0x0a0, sZeroF);
    set(0x0b0, sRoleSerial);
    set(0x0b8, sCtrlPlacement);
    set(0x0c0, sCtrlPlacement2);
    set(0x0d0, sButton);
    set(0x110, sName);
    set(0x118, sCtrlTypeName);
    set(0x128, sOne);
    set(0x148, sApi);
    set(0x178, sHiddenMesh);
    set(0x198, sSerial);
    set(0x1a8, sVkInstExt);
    set(0x1b0, sVkDevExt);
    set(0x1b8, sHasDesktop);
    set(0x1e0, sFingerCurl);
}

// The viewport's mouse handler (vtable +0x78) scales mouse coordinates by the VR viewer object at
// viewport+0x360 whenever a device exists, but the viewer is only created lazily by the viewport's VR
// render method (vtable +0x58). A mouse event before the first VR frame dereferenced NULL (crash at
// Sam4 0x140053263). Create the viewer first, exactly as the engine's lazy path does.
using MouseFn = void(__cdecl*)(void* viewport, int* pos, int msg, int* size);
using AllocFn = void*(__cdecl*)(size_t size, uintptr_t tag);
using CtorFn = void*(__cdecl*)(void* mem);
MouseFn realMouse;
void ensureViewer(void* viewport) {
    void** slot = reinterpret_cast<void**>(static_cast<uint8_t*>(viewport) + 0x360);
    if (*slot) return;
    auto alloc = reinterpret_cast<AllocFn>(se4::build().at<void>(se4::gen::engine_alloc));
    auto ctor = reinterpret_cast<CtorFn>(se4::build().at<void>(se4::gen::vr_viewer_ctor));
    if (!alloc || !ctor) return;
    void* mem = alloc(0x138, 0);
    if (mem) *slot = ctor(mem);
    VRLOG("vrdev: created VR viewer %p for viewport %p ahead of a mouse event", *slot, viewport);
}
void hookedMouse(void* viewport, int* pos, int msg, int* size) {
    if (g_installed) ensureViewer(viewport);
    realMouse(viewport, pos, msg, size);
}

using ShutdownFn = void(__cdecl*)();
ShutdownFn realShutdown;
void hookedShutdown() {
    void** global = se4::build().at<void*>(se4::gen::vr_device_global);
    if (global && *global == &g_obj) {
        VRLOG("vrdev: engine device shutdown - detaching our device");
        *global = nullptr;
        g_installed = false;
    }
    realShutdown();
}
}  // namespace

bool install() {
    if (g_installed) return true;
    void** global = se4::build().at<void*>(se4::gen::vr_device_global);
    if (!global || !xr::runtime().active()) return false;
    if (*global) {
        VRLOG("vrdev: engine device global already set (%p) - not installing", *global);
        return false;
    }
    buildTable();
    memset(&g_obj, 0, sizeof g_obj);
    g_obj.vtbl = g_vtbl;
    // Controllers never reported idle (+0x20/+0x50/+0x80 "moving" flags, R1 base fields).
    field<int32_t>(0x20) = field<int32_t>(0x50) = field<int32_t>(0x80) = 1;
    *global = &g_obj;
    g_installed = true;
    VRLOG("vrdev: COpenXRDevice installed at %p (global %p)", static_cast<void*>(&g_obj), static_cast<void*>(global));
    return true;
}

bool installed() { return g_installed; }

bool hookViewport() {
    void* target = se4::build().at<void>(se4::gen::viewport_mouse_event);
    if (!target || MH_CreateHook(target, reinterpret_cast<void*>(hookedMouse), reinterpret_cast<void**>(&realMouse)) != MH_OK ||
        MH_EnableHook(target) != MH_OK) {
        VRLOG("vrdev: viewport mouse hook FAILED");
        return false;
    }
    return true;
}

bool hookShutdown() {
    void* target = se4::build().at<void>(se4::gen::vr_device_shutdown);
    if (!target || MH_CreateHook(target, reinterpret_cast<void*>(hookedShutdown), reinterpret_cast<void**>(&realShutdown)) != MH_OK ||
        MH_EnableHook(target) != MH_OK) {
        VRLOG("vrdev: shutdown hook FAILED");
        return false;
    }
    return true;
}

void logStats() {
    if (!g_installed) return;
    char buf[1024];
    int n = 0;
    for (unsigned i = 0; i < kSlots && n < int(sizeof buf) - 24; ++i) {
        LONG c = InterlockedExchange(&g_calls[i], 0);
        if (c) n += snprintf(buf + n, sizeof buf - n, " %03x:%ld", i * 8, c);
    }
    VRLOG("vrdev: calls%s", n ? buf : " none");
}
}  // namespace vrdev
