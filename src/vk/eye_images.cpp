#include "vk/eye_images.hpp"
#include "se4/log.hpp"
#include <windows.h>

namespace eyes {
namespace {
SRWLOCK g_lock = SRWLOCK_INIT;
struct Capture {
    int eye = 0;
    uint32_t w = 0, h = 0;
    DWORD thread = 0;
} g_cap;
VkImage g_image[2]{};
VkFormat g_format = VK_FORMAT_UNDEFINED;
uint32_t g_w, g_h;
bool g_submitted[2];
}  // namespace

void beginCapture(int eye, uint32_t w, uint32_t h) {
    AcquireSRWLockExclusive(&g_lock);
    g_cap = {eye, w, h, GetCurrentThreadId()};
    ReleaseSRWLockExclusive(&g_lock);
}

void endCapture() {
    AcquireSRWLockExclusive(&g_lock);
    g_cap = {};
    ReleaseSRWLockExclusive(&g_lock);
}

void onImageCreated(VkImage image, const VkImageCreateInfo& ci) {
    AcquireSRWLockExclusive(&g_lock);
    if (g_cap.eye && g_cap.thread == GetCurrentThreadId() && ci.extent.width == g_cap.w && ci.extent.height == g_cap.h &&
        (ci.usage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT)) {
        g_image[g_cap.eye - 1] = image;
        g_format = ci.format;
        g_w = ci.extent.width;
        g_h = ci.extent.height;
        VRLOG("eyes: eye %d image %p %ux%u format %d usage %x", g_cap.eye, (void*)image, g_w, g_h, int(ci.format), ci.usage);
    }
    ReleaseSRWLockExclusive(&g_lock);
}

void onImageDestroyed(VkImage image) {
    AcquireSRWLockExclusive(&g_lock);
    for (auto& i : g_image)
        if (i == image) i = VK_NULL_HANDLE;
    ReleaseSRWLockExclusive(&g_lock);
}

void submitted(int eye) {
    if (eye != 1 && eye != 2) return;
    AcquireSRWLockExclusive(&g_lock);
    g_submitted[eye - 1] = true;
    ReleaseSRWLockExclusive(&g_lock);
}

Frame take() {
    Frame f;
    AcquireSRWLockExclusive(&g_lock);
    f.image[0] = g_image[0];
    f.image[1] = g_image[1];
    f.format = g_format;
    f.width = g_w;
    f.height = g_h;
    f.ready = g_submitted[0] && g_submitted[1] && g_image[0] && g_image[1];
    g_submitted[0] = g_submitted[1] = false;
    ReleaseSRWLockExclusive(&g_lock);
    return f;
}
}  // namespace eyes
