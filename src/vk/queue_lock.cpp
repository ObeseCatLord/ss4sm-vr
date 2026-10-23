#include "vk/queue_lock.hpp"

namespace vkq {
namespace {
CRITICAL_SECTION g_cs;
LONG g_init;
}
void initLock() {
    if (!InterlockedExchange(&g_init, 1)) InitializeCriticalSectionAndSpinCount(&g_cs, 4000);
}
void lock() { EnterCriticalSection(&g_cs); }
void unlock() { LeaveCriticalSection(&g_cs); }
}  // namespace vkq
