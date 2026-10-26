// CPlayerVRData enable and guards (docs/re/R3_vr_gameplay.md). Addresses come from the generated build
// tables; offsets inside functions are found by matching the instruction bytes in the running build and
// are refused unless the match is exact and unique.
#include "se4/player_vr.hpp"
#include "se4/net.hpp"
#include "se4/body.hpp"
#include "se4/gadgets.hpp"
#include "se4/build.hpp"
#include "se4/cvars.hpp"
#include "se4/log.hpp"
#include "se4/options.hpp"
#include "se4/patch.hpp"
#include "se4/vr_device.hpp"
#include "se4/vehicles.hpp"
#include <windows.h>
#include <MinHook.h>
#include <cstdint>
#include <cstring>

namespace player_vr {
namespace {
uint8_t* g_bindRet;     // return address of bind_local_user_psd's call to set_playing_in_vr
unsigned g_vrdOff;      // player -> CPlayerVRData*
void* volatile g_localVrd;

int32_t rel32(const uint8_t* p) {
    int32_t v;
    memcpy(&v, p, 4);
    return v;
}

// --- SetPlayingInVR: the local user's binding passes 0; pass 1 while our device is installed ---
using SetPlayingFn = void(__cdecl*)(void* psd, int value);
SetPlayingFn realSetPlaying;
void hookedSetPlaying(void* psd, int value) {
    if (!value && __builtin_return_address(0) == g_bindRet && vrdev::installed()) {
        VRLOG("player_vr: local user psd %p marked as playing in VR", psd);
        value = 1;
    }
    realSetPlaying(psd, value);
}

// --- vrd_init: log and remember the local player's vrd ---
using VrdInitFn = void(__cdecl*)(void* vrd, void* player);
VrdInitFn realVrdInit;
void hookedVrdInit(void* vrd, void* player) {
    realVrdInit(vrd, player);
    int local = *reinterpret_cast<int32_t*>(static_cast<uint8_t*>(vrd) + 0x328);  // vt+0x670 result (R3 s1)
    VRLOG("player_vr: CPlayerVRData %p initialised for player %p (local %d)", vrd, player, local);
    if (auto* layout = se4::build().at<int32_t>(se4::gen::prj_iGamepadButtonLayout))
        VRLOG("player_vr: gamepad button layout %d (the virtual pad assumes GBL_STANDARD 12)", *layout);
    if (local) g_localVrd = vrd;
    {
        const int32_t* b = reinterpret_cast<const int32_t*>(static_cast<uint8_t*>(vrd) + 0x288);
        VRLOG("player_vr: vrd button indices +0x288..+0x2a4: %d %d %d %d %d %d %d %d", b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7]);
    }
    // Lab only: give all weapons once the local VR player exists (needs +cht_bEnableCheats 1).
    if (local && se4::options::get("lab_giveall", 0)) {
        // Gadgets, upgrades and skills too: the give-all flags global (GACF_*: gadgets 4, upgrades 8, skills 0x10) found in the cheats menu's own handler
        // (mov edx,[rip+flags]; mov ecx,edx; ...; or ecx,4).
        if (auto* fn = se4::build().at<uint8_t>(se4::gen::giveall_gadgets_script))
            for (int i = 0; i < 0x100; ++i)
                if (fn[i] == 0x8B && fn[i + 1] == 0x15 && fn[i + 6] == 0x8B && fn[i + 7] == 0xCA &&
                    [&] { for (int k = 8; k < 20; ++k) if (fn[i + k] == 0x83 && fn[i + k + 1] == 0xC9 && fn[i + k + 2] == 0x04) return true; return false; }()) {
                    auto* flags = reinterpret_cast<uint32_t*>(fn + i + 6 + *reinterpret_cast<int32_t*>(fn + i + 2));
                    VRLOG("player_vr: lab give-all flags %p: 0x%x -> 0x%x", flags, *flags, *flags | 0x1cu);
                    *flags |= 0x1cu;  // gadgets, upgrades, skills (dual wielding is a skill)
                    break;
                }
        se4::setIndex(se4::gen::cht_bGiveAll, 1);
    }
}

// --- native VR use path (vrd, hand 0 right / 1 left): the right B (gamepad::kick) runs it, see useHand() ---
using HandUseFn = void(__cdecl*)(void* vrd, int hand);
HandUseFn realHandUse;
void hookedHandUse(void* vrd, int hand) {
    static ULONGLONG last;
    if (GetTickCount64() - last > 300) last = GetTickCount64(), VRLOG("player_vr: native VR use (hand %d)", hand);
    realHandUse(vrd, hand);
}

// --- controller world placement: callers outside the player (VR view hand ray 0x1401d8430) pass the
// player's vrd without a null check; a zero placement makes that caller return (R3 s7.3) ---
using CtrlWorldFn = float*(__cdecl*)(void* vrd, float* out, int role);
CtrlWorldFn realCtrlWorld;
float* hookedCtrlWorld(void* vrd, float* out, int role) {
    if (!vrd) {
        memset(out, 0, 7 * sizeof(float));
        return out;
    }
    return realCtrlWorld(vrd, out, role);
}

// --- ClientActionVR: the apply half writes the player's vrd unguarded (R3 s7.4). Drop the action for a
// player without one. All 13 parameters are integer-class or stack slots, so this forwards them exactly. ---
using CavrFn = void(__cdecl*)(uint8_t*, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t,
                              uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t);
CavrFn realCavr;
void hookedCavr(uint8_t* player, uintptr_t a2, uintptr_t a3, uintptr_t a4, uintptr_t a5, uintptr_t a6, uintptr_t a7,
                uintptr_t a8, uintptr_t a9, uintptr_t a10, uintptr_t a11, uintptr_t a12, uintptr_t a13) {
    if (!player || !*reinterpret_cast<void**>(player + g_vrdOff)) {
        static LONG once;
        if (!InterlockedExchange(&once, 1)) VRLOG("player_vr: dropped ClientActionVR for player %p without VR data", player);
        return;
    }
    realCavr(player, a2, a3, a4, a5, a6, a7, a8, a9, a10, a11, a12, a13);
}

// --- ClientAction (desktop) -> ClientActionVR for the local VR player (option native_vr_action). The desktop
// sender (0x14048b160) computed every argument; ClientActionVR takes the same ones (lock-on aim is not part of
// it) plus the device-local head and both world controller placements, as the native VR sender 0x14048b730
// builds them, and the VR movement front Euler (vrd+0x628, same offset in SS4 and SM). ---
using CaFn = void(__cdecl*)(uint8_t*, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t,
                            uintptr_t, uintptr_t, uintptr_t);
CaFn realCa;
bool g_nativeAction, g_nativeTurn;
void hookedCa(uint8_t* player, uintptr_t fire, uintptr_t look, uintptr_t lockOn, uintptr_t relMove, uintptr_t clientPos,
              uintptr_t clientH, uintptr_t henFloor, uintptr_t henMover, uintptr_t reset1, uintptr_t reset2) {
    void* vrd = player ? *reinterpret_cast<void**>(player + g_vrdOff) : nullptr;
    if (vrd && vrd == g_localVrd && vrdev::installed()) vehicles::step(player, vrd);
    if (!nativeVrAction() || !vrd || vrd != g_localVrd || !vrdev::installed()) {
        realCa(player, fire, look, lockOn, relMove, clientPos, clientH, henFloor, henMover, reset1, reset2);
        return;
    }
    float head[7], right[7], left[7], front[3];
    vrdev::headPlacement(head);
    realCtrlWorld(vrd, right, 0);
    realCtrlWorld(vrd, left, 1);
    memcpy(front, static_cast<uint8_t*>(vrd) + 0x628, sizeof front);
    static LONG once;
    if (!InterlockedExchange(&once, 1)) VRLOG("player_vr: local VR player sends ClientActionVR");
    realCavr(player, fire, reinterpret_cast<uintptr_t>(head), reinterpret_cast<uintptr_t>(right),
             reinterpret_cast<uintptr_t>(left), relMove, clientPos, clientH, reinterpret_cast<uintptr_t>(front), henFloor,
             henMover, reset1, reset2);
}

// Per-hand VR controller model pose (0x140722f30): reads the hand's model (+0x70, or +0x78 for the alternate
// pose) and passes it on without a null check; content without that controller model crashed here.
using CtrlModelFn = void(__cdecl*)(uint8_t* hand, void* dp, int a, int b, int alt);
CtrlModelFn realCtrlModel;
void hookedCtrlModel(uint8_t* hand, void* dp, int a, int b, int alt) {
    if (!hand || !*reinterpret_cast<void**>(hand + (alt ? 0x78 : 0x70))) return;
    realCtrlModel(hand, dp, a, b, alt);
}

bool hook(void* target, void* detour, void** real, const char* what) {
    if (!target || MH_CreateHook(target, detour, real) != MH_OK || MH_EnableHook(target) != MH_OK) {
        VRLOG("player_vr: %s hook FAILED (%p)", what, target);
        return false;
    }
    return true;
}

// Return address of the single `call rel32` to `callee` within the first `len` bytes of `fn`.
uint8_t* findCallRet(uint8_t* fn, size_t len, void* callee) {
    uint8_t* found = nullptr;
    for (size_t i = 0; i + 5 <= len; ++i) {
        if (fn[i] != 0xE8) continue;
        uint8_t* next = fn + i + 5;
        if (next + rel32(fn + i + 1) != callee) continue;
        if (found) return nullptr;  // ambiguous
        found = next;
    }
    return found;
}

// player->vrd offset: ClientActionVR's apply loads it (`mov rcx, [rsi+disp32]`) right before the first
// store to vrd+0x32c (head pose).
unsigned findVrdOffset(const uint8_t* fn) {
    for (size_t i = 0; i + 7 <= 0x300; ++i) {
        if (fn[i] != 0x48 || fn[i + 1] != 0x8B || fn[i + 2] != 0x8E) continue;
        for (size_t j = i + 7; j < i + 7 + 24; ++j)
            if (fn[j] == 0x2C && fn[j + 1] == 0x03 && fn[j + 2] == 0 && fn[j + 3] == 0) {
                int32_t d = rel32(fn + i + 3);
                return d > 0x1000 && d < 0x4000 ? unsigned(d) : 0;
            }
    }
    return 0;
}

// `mov rax, global; mov rcx,[rax]; test rcx,rcx; jz zero; mov rax,[rcx]; jmp [rax+slot]; zero: xor eax,eax; ret`
// near the image: calls the device slot with the caller's own arguments and stack, or returns 0 without a device.
uint8_t* nullCheckThunk(void* nearTo, void** global, uint32_t slot) {
    uint8_t* thunk = se4::patch::allocNear(nearTo, 64);
    if (!thunk) return nullptr;
    static const uint8_t body[] = {
        0x48, 0xB8, 0, 0, 0, 0, 0, 0, 0, 0,  // mov rax, imm64 (global)
        0x48, 0x8B, 0x08,                    // mov rcx, [rax]
        0x48, 0x85, 0xC9,                    // test rcx, rcx
        0x74, 0x09,                          // jz zero
        0x48, 0x8B, 0x01,                    // mov rax, [rcx]
        0xFF, 0xA0, 0, 0, 0, 0,              // jmp [rax+slot]
        0x31, 0xC0,                          // zero: xor eax, eax
        0xC3,                                // ret
    };
    memcpy(thunk, body, sizeof body);
    memcpy(thunk + 2, &global, 8);
    memcpy(thunk + 23, &slot, 4);
    FlushInstructionCache(GetCurrentProcess(), thunk, sizeof body);
    return thunk;
}

// 0x14043aac0 (R9 s5) is a forwarder `mov rcx,[rip+global]; mov rax,[rcx]; jmp [rax+0x1d8]` run every tick for
// every player with vrd, including remote VR players on a desktop peer without a device. Its entry becomes a
// jump to the null-checking thunk. Found by exact bytes; must be unique in .text.
bool patchSlot1d8Forwarder(void** global) {
    HMODULE exe = GetModuleHandleW(nullptr);
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(exe);
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(reinterpret_cast<uint8_t*>(exe) + dos->e_lfanew);
    auto* sec = IMAGE_FIRST_SECTION(nt);
    uint8_t* hit = nullptr;
    int n = 0;
    for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++sec) {
        if (memcmp(sec->Name, ".text", 6)) continue;
        uint8_t* t = reinterpret_cast<uint8_t*>(exe) + sec->VirtualAddress;
        for (size_t k = 0; k + 17 <= sec->Misc.VirtualSize; ++k) {
            uint8_t* p = t + k;
            if (p[0] != 0x48 || p[1] != 0x8B || p[2] != 0x0D || p[7] != 0x48 || p[8] != 0x8B || p[9] != 0x01 ||
                p[10] != 0x48 || p[11] != 0xFF || p[12] != 0xA0 || rel32(p + 13) != 0x1d8)
                continue;
            if (p + 7 + rel32(p + 3) != reinterpret_cast<uint8_t*>(global)) continue;
            hit = p;
            ++n;
        }
    }
    if (n != 1) {
        VRLOG("player_vr: slot 0x1d8 forwarder: found %d, expected 1 - not patched", n);
        return false;
    }
    uint8_t* thunk = nullCheckThunk(hit, global, 0x1d8);
    intptr_t rel = thunk ? thunk - (hit + 5) : 0;
    if (!thunk || rel != int32_t(rel)) return false;
    DWORD old;
    if (!VirtualProtect(hit, 5, PAGE_EXECUTE_READWRITE, &old)) return false;
    hit[0] = 0xE9;
    int32_t r = int32_t(rel);
    memcpy(hit + 1, &r, 4);
    VirtualProtect(hit, 5, old, &old);
    FlushInstructionCache(GetCurrentProcess(), hit, 5);
    VRLOG("player_vr: slot 0x1d8 forwarder %p goes through null-checking thunk %p", static_cast<void*>(hit), static_cast<void*>(thunk));
    return true;
}

// vrd_init calls device slot 0x208 twice as `mov rcx,[rip+global]; mov rax|rdx,[rcx]; call [rax|rdx+0x208]`
// (16 bytes) with no null check: it runs for every VR player, including remote VR players on a machine
// whose device global is NULL (R3 s7.2). Each site becomes `call thunk` + NOPs; the thunk returns 0 when
// the global is NULL and otherwise tail-jumps to the slot with the same stack the original call had.
bool patchSlot208(uint8_t* fn, void** global) {
    uint8_t* sites[2];
    int n = 0;
    for (size_t i = 0; i + 16 <= 0x800; ++i) {
        uint8_t* p = fn + i;
        if (p[0] != 0x48 || p[1] != 0x8B || p[2] != 0x0D) continue;
        if (p + 7 + rel32(p + 3) != reinterpret_cast<uint8_t*>(global)) continue;
        bool rax = p[7] == 0x48 && p[8] == 0x8B && p[9] == 0x01 && p[10] == 0xFF && p[11] == 0x90;
        bool rdx = p[7] == 0x48 && p[8] == 0x8B && p[9] == 0x11 && p[10] == 0xFF && p[11] == 0x92;
        if (!(rax || rdx) || rel32(p + 12) != 0x208) continue;
        if (n == 2) return false;
        sites[n++] = p;
    }
    if (n != 2) {
        VRLOG("player_vr: vrd_init slot 0x208 sites: found %d, expected 2 - not patched", n);
        return false;
    }
    uint8_t* thunk = nullCheckThunk(fn, global, 0x208);
    if (!thunk) {
        VRLOG("player_vr: no executable memory near the image - slot 0x208 not patched");
        return false;
    }
    for (uint8_t* p : sites) {
        DWORD old;
        if (!VirtualProtect(p, 16, PAGE_EXECUTE_READWRITE, &old)) return false;
        uint8_t code[16] = {0xE8, 0, 0, 0, 0, 0x0F, 0x1F, 0x84, 0, 0, 0, 0, 0, 0x0F, 0x1F, 0x00};
        int32_t rel = int32_t(thunk - (p + 5));
        memcpy(code + 1, &rel, 4);
        memcpy(p, code, 16);
        VirtualProtect(p, 16, old, &old);
    }
    FlushInstructionCache(GetCurrentProcess(), fn, 0x800);
    VRLOG("player_vr: vrd_init slot 0x208 calls at %p, %p go through null-checking thunk %p", sites[0], sites[1], thunk);
    return true;
}
// Lab only (docs/re/R20_lab_lua.md): if ss4smvr/lab.lua exists next to the payload, run it once in the local
// player's world (its script context: worldGlobals, Wait, engine functions) after lab_lua_delay ticks (default 90).
using CtxFn = void*(__cdecl*)();
using LsiFn = void*(__cdecl*)(void* world);
using ExecFn = int(__cdecl*)(void* lsi, void* ctx, const char* src, uint32_t flags);
void labLua(uint8_t* player) {
    static int state;  // 0 not loaded, 1 waiting, 2 done
    static char* text;
    static int delay;
    if (state == 2) return;
    if (state == 0) {
        state = 2;
        wchar_t path[MAX_PATH];
        HMODULE self = nullptr;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(&labLua), &self);
        const DWORD n = GetModuleFileNameW(self, path, MAX_PATH);
        wchar_t* slash = n && n < MAX_PATH - 16 ? wcsrchr(path, L'\\') : nullptr;
        if (!slash) return;
        wcscpy(slash + 1, L"lab.lua");
        HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
        if (f == INVALID_HANDLE_VALUE) return;
        const DWORD size = GetFileSize(f, nullptr);
        static const char prefix[] = "--[[@lab_lua-@]] ";
        text = size < (1u << 20) ? static_cast<char*>(HeapAlloc(GetProcessHeap(), 0, size + sizeof prefix)) : nullptr;
        DWORD got = 0;
        if (text) {
            memcpy(text, prefix, sizeof prefix - 1);
            ReadFile(f, text + sizeof prefix - 1, size, &got, nullptr);
            text[sizeof prefix - 1 + got] = 0;
        }
        CloseHandle(f);
        if (!text) return;
        delay = se4::options::get("lab_lua_delay", 90);
        state = 1;
        VRLOG("player_vr: lab.lua loaded (%lu bytes), runs in %d ticks", got, delay);
        return;
    }
    if (delay-- > 0) return;
    state = 2;
    const auto& b = se4::build();
    auto ctx = reinterpret_cast<CtxFn>(b.at<void>(se4::gen::script_ctx));
    auto toLsi = reinterpret_cast<LsiFn>(b.at<void>(se4::gen::lua_world_to_lsi));
    auto exec = reinterpret_cast<ExecFn>(b.at<void>(se4::gen::lua_exec_string));
    void* world = *reinterpret_cast<void**>(player + 0x30);
    void* lsi = ctx && toLsi && exec && world ? toLsi(world) : nullptr;
    if (!lsi || *reinterpret_cast<int32_t*>(static_cast<uint8_t*>(lsi) + 8) != 1) return VRLOG("player_vr: lab.lua - no world script interface");
    const int ok = exec(lsi, ctx(), text, 0);
    VRLOG("player_vr: lab.lua executed (%s)", ok ? "ok" : "failed - see 'Lua error' in the game log");
}

// Every CPlayerVRData step (simulation thread), armed or not: body IK, and the local player's gadget/wheel-spot
// tick (src/se4/gadgets.cpp), which otherwise only ran from weapon steps.
using VrdTickFn = void(__cdecl*)(void** vrd);
VrdTickFn realVrdTick;
// Riding crash (docs/re/R21_lab_vehicles.md s3): boarding a vehicle drops the player's physics body (player+0x1d0,
// en_pbo) and the native VR locomotion reads it without a check (0x14043aae0, "airborne for long", lab crash at
// 0x14043aaf5). That reader answers "not airborne" without a body. The movement bits stay set, so the VR anchor keeps
// following the seated player (the post-step's follow needs vrd+0x2bc & 0x18).
using InAirFn = int(__cdecl*)(void** vrd);
InAirFn realInAir;
int __cdecl hookedInAir(void** vrd) {
    auto* player = vrd ? static_cast<uint8_t*>(vrd[0]) : nullptr;
    if (!player || !*reinterpret_cast<void**>(player + 0x1d0)) {
        static int n;
        if (n++ == 0) VRLOG("player_vr: VR locomotion read a missing physics body (riding) - answered 'not airborne'");
        return 0;
    }
    return realInAir(vrd);
}

void __cdecl hookedVrdTick(void** vrd) {
    realVrdTick(vrd);
    body::onVrdTick(vrd);
    net::tick(vrd, vrd && vrd == g_localVrd);
    if (vrd && vrd == g_localVrd) {
        gadgets::onTick(static_cast<uint8_t*>(vrd[0]));  // vrd+0 = player (vrd_init)
        labLua(static_cast<uint8_t*>(vrd[0]));
    }
}
}  // namespace

unsigned vrdOffset() { return g_vrdOff; }
void controllerWorld(void* vrd, int role, float out[7]) {
    if (realCtrlWorld && vrd) realCtrlWorld(vrd, out, role);
    else memset(out, 0, 7 * sizeof(float));
}
void headWorld(void* vrd, float out[7]) {
    memset(out, 0, 7 * sizeof(float));
    static auto fn = reinterpret_cast<void*(__cdecl*)(void*, float*)>(se4::build().at<void>(se4::gen::vrd_head_world));
    if (fn && vrd) fn(vrd, out);
}
bool nativeVrAction() { return g_nativeAction || net::clientSession(); }  // a multiplayer client always: the server needs the hands
void useHand(int hand) {
    if (realHandUse && g_localVrd) realHandUse(g_localVrd, hand);
    else VRLOG("player_vr: no VR use path (hook %p, vrd %p)", reinterpret_cast<void*>(realHandUse), g_localVrd);
}
bool nativeTurn() { return g_nativeTurn || net::clientSession(); }
void* localVrd() { return g_localVrd; }

bool install(bool localVr) {
    const auto& b = se4::build();
    auto* setter = b.at<uint8_t>(se4::gen::set_playing_in_vr);
    auto* bind = b.at<uint8_t>(se4::gen::bind_local_user_psd);
    auto* init = b.at<uint8_t>(se4::gen::vrd_init);
    auto* ctrl = b.at<uint8_t>(se4::gen::vrd_controller_world);
    auto* cavr = b.at<uint8_t>(se4::gen::client_action_vr);
    void** global = b.at<void*>(se4::gen::vr_device_global);
    if (!setter || !bind || !init || !ctrl || !cavr || !global) {
        VRLOG("player_vr: build lacks VR player symbols - not installed");
        return false;
    }
    g_vrdOff = findVrdOffset(cavr);
    if (void* use = b.at<void>(se4::gen::vr_hand_use); use && localVr &&
        (MH_CreateHook(use, reinterpret_cast<void*>(hookedHandUse), reinterpret_cast<void**>(&realHandUse)) != MH_OK || MH_EnableHook(use) != MH_OK))
        VRLOG("player_vr: VR use hook FAILED");
    g_bindRet = findCallRet(bind, 0x200, setter);
    VRLOG("player_vr: vrd offset 0x%x, local-user setter call returns to %p (%s)", g_vrdOff,
          static_cast<void*>(g_bindRet), localVr ? "VR player" : "desktop peer guards only");
    if (!g_vrdOff || !g_bindRet) return false;
    // Guards first: they must be in place before any player can have VR data. A desktop peer needs them too,
    // because remote VR players get a CPlayerVRData on every machine (psd_bIsPlayingInVR replicates).
    bool ok = patchSlot208(init, global);
    ok &= patchSlot1d8Forwarder(global);
    ok &= hook(ctrl, reinterpret_cast<void*>(hookedCtrlWorld), reinterpret_cast<void**>(&realCtrlWorld), "controller placement");
    ok &= hook(cavr, reinterpret_cast<void*>(hookedCavr), reinterpret_cast<void**>(&realCavr), "ClientActionVR");
    ok &= hook(init, reinterpret_cast<void*>(hookedVrdInit), reinterpret_cast<void**>(&realVrdInit), "vrd_init");
    if (auto* cm = b.at<uint8_t>(se4::gen::vr_ctrl_model_pose))
        ok &= hook(cm, reinterpret_cast<void*>(hookedCtrlModel), reinterpret_cast<void**>(&realCtrlModel), "controller model");
    // The vrd tick: the riding guard for every VR player (desktop peers tick remote VR players too), and the local
    // player's gadget/spot tick.
    if (auto* tick = b.at<uint8_t>(se4::gen::vrd_tick))
        ok &= hook(tick, reinterpret_cast<void*>(hookedVrdTick), reinterpret_cast<void**>(&realVrdTick), "vrd tick");
    if (auto* inAir = b.at<uint8_t>(se4::gen::vr_loco_in_air))
        ok &= hook(inAir, reinterpret_cast<void*>(hookedInAir), reinterpret_cast<void**>(&realInAir), "VR locomotion airborne test");
    if (!localVr) return ok;
    g_nativeAction = se4::options::get("native_vr_action", 0) != 0;
    g_nativeTurn = se4::options::get("native_turn", g_nativeAction) != 0;
    if (auto* ca = b.at<uint8_t>(se4::gen::client_action))
        ok &= hook(ca, reinterpret_cast<void*>(hookedCa), reinterpret_cast<void**>(&realCa), "ClientAction");
    else ok &= !g_nativeAction;
    if (!ok) return false;  // never enable VR players without every guard
    return hook(setter, reinterpret_cast<void*>(hookedSetPlaying), reinterpret_cast<void**>(&realSetPlaying), "SetPlayingInVR");
}

void onDeviceInstalled() {
    // 0x140050860 (controller placement from the device rather than emulated hands) is true for a
    // non-emulated device only with this cvar or an active "VRController" input device. SS4/SM have no such
    // input device, and the engine switches to it whenever device+0xb3c is set (game loop 0x140058800), so
    // the device keeps +0xb3c at 0 and this cvar (read only there) is forced instead (R3 follow-up).
    if (se4::options::get("vr_exclusive_input", 1)) se4::setIndex(se4::gen::vr_bVRControllerExclusiveInput, 1);
}
}  // namespace player_vr
