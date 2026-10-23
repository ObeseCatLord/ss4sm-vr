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
};
// Match the main module by TimeDateStamp/SizeOfImage, then require the in-memory .text CRC32 to equal
// the recorded file CRC. Returns false (and the payload stays inert) for any unknown or modified build.
bool identifyBuild(Build& out);
const Build& build();
}  // namespace se4
