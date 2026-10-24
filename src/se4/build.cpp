#include "se4/build.hpp"
#include "se4/log.hpp"
#include <windows.h>
#include <cstring>

namespace se4 {
unsigned g_debugSkip;
namespace {
Build g_build;

uint32_t crc32(const uint8_t* p, size_t n) {
    static uint32_t table[256];
    if (!table[1])
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            table[i] = c;
        }
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; ++i) c = table[(c ^ p[i]) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}
}  // namespace

bool Build::sm() const { return table && table->build == 628492; }
const Build& build() { return g_build; }

bool identifyBuild(Build& out) {
    auto base = reinterpret_cast<uint8_t*>(GetModuleHandleW(nullptr));
    auto dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    auto nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    uint32_t stamp = nt->FileHeader.TimeDateStamp, size = nt->OptionalHeader.SizeOfImage;
    const gen::BuildTable* match = nullptr;
    for (auto& b : gen::kBuilds)
        if (b.timeDateStamp == stamp && b.sizeOfImage == size) match = &b;
    if (!match) {
        VRLOG("build: unknown executable (stamp %08x size %08x) - VR stays inactive", stamp, size);
        return false;
    }
    auto sec = IMAGE_FIRST_SECTION(nt);
    uint32_t crc = 0;
    bool found = false;
    for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i)
        if (!memcmp(sec[i].Name, ".text", 6)) {
            DWORD n = sec[i].Misc.VirtualSize < sec[i].SizeOfRawData ? sec[i].Misc.VirtualSize : sec[i].SizeOfRawData;
            crc = crc32(base + sec[i].VirtualAddress, n);
            found = true;
        }
    if (!found || crc != match->textCrc32) {
        VRLOG("build: %s header matches but .text crc %08x != %08x - VR stays inactive", match->name, crc, match->textCrc32);
        return false;
    }
    out.table = match;
    out.base = reinterpret_cast<uintptr_t>(base);
    g_build = out;
    VRLOG("build: %s (build %u) verified", match->name, match->build);
    return true;
}
}  // namespace se4
