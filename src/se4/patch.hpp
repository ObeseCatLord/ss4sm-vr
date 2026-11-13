#pragma once
// In-memory code patching helpers (never touches files): executable memory within rel32 reach of the
// game image, and redirection of verified `call rel32` sites.
#include <cstddef>
#include <cstdint>

namespace se4::patch {
// Executable memory within +-2 GB of `target` (for rel32 calls/jumps from the image), or nullptr.
uint8_t* allocNear(void* target, size_t size);
// The same for data only (read-write, never executable): values a redirected read points at.
uint8_t* allocNearData(void* target, size_t size);
// A near stub that jumps to `fn` (absolute), so rel32 call sites can reach code anywhere.
uint8_t* nearJump(void* imageAddress, void* fn);
// Calls (E8 rel32) within [fn, fn+len) whose target begins with the bytes of `mov eax,1; ret`.
int findReturnOneCalls(uint8_t* fn, size_t len, uint8_t** out, int max);
// Overwrite code bytes after checking that `expect` (same length) is there. False if it is not.
bool writeCode(uint8_t* site, const uint8_t* expect, const uint8_t* bytes, size_t n);
// Retarget an E8 rel32 call site. Returns false if the site is not an E8 or the target is out of reach.
bool retargetCall(uint8_t* site, void* newTarget);
}  // namespace se4::patch
