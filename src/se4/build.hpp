#pragma once
// Identifies the running game executable against the generated per-build tables.
#include <cstdint>
#include "se4/symbols_gen.hpp"

namespace se4 {
struct Build {
    const gen::BuildTable* table = nullptr;
    uintptr_t base = 0;
    bool sm() const;  // Siberian Mayhem
    template <class T> T* at(gen::Sym s) const {
        uint32_t rva = table ? table->rva[s] : 0;
        return rva ? reinterpret_cast<T*>(base + rva) : nullptr;
    }
    // Byte offset of a class member from the engine's reflection records (0 = unknown in this build).
    uint32_t off(gen::Mem m) const { return table ? table->mem[m] : 0; }
};
// Match the main module by TimeDateStamp/SizeOfImage, then require the in-memory .text CRC32 to equal
// the recorded file CRC. Returns false (and the payload stays inert) for any unknown or modified build.
bool identifyBuild(Build& out);
const Build& build();
// Lab bisect mask from ss4smvr/debug_skip.txt (0 in normal use). Bits: 2 procaddr hook, 4 focus hooks,
// 8 VR device install, 0x10 VR player enable and guards (player_vr), 0x20 native projected crosshair, 0x40 vehicle grab steering, 0x80 body IK.
extern unsigned g_debugSkip;
// VR is opt-in while in development: env SS4SMVR_VR=1 or the file ss4smvr/enable_vr next to the payload.
extern bool g_vrEnabled;
}  // namespace se4
