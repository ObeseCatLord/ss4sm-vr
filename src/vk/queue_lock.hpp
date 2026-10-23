#pragma once
// One lock serialises every use of the graphics queue shared with the OpenXR runtime: the game's
// vkQueueSubmit*/vkQueuePresentKHR/vkQueueWaitIdle on that queue and our XR calls that may touch it.
#include <windows.h>

namespace vkq {
void initLock();
void lock();
void unlock();
struct Guard {
    Guard() { lock(); }
    ~Guard() { unlock(); }
    Guard(const Guard&) = delete;
    Guard& operator=(const Guard&) = delete;
};
}  // namespace vkq
