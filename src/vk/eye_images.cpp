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
    bool got = false;
    bool gotPreferred = false;  // single-sample with TRANSFER_SRC (the engine copies submitted eyes from it)
} g_cap;
// If the engine creates the image after the bracket (another thread or deferred creation), the first
// matching single-sample colour image within this window still becomes the eye's image.
struct Pending {
    uint32_t w = 0, h = 0;
    ULONGLONG deadline = 0;
} g_pending[2];
constexpr ULONGLONG kPendingMs = 2000;
VkImage g_image[2]{};
VkFormat g_format = VK_FORMAT_UNDEFINED;
uint32_t g_w, g_h;
bool g_submitted[2];

bool eyeCandidate(const VkImageCreateInfo& ci, uint32_t w, uint32_t h) {
    return ci.imageType == VK_IMAGE_TYPE_2D && ci.extent.width == w && ci.extent.height == h &&
           ci.samples == VK_SAMPLE_COUNT_1_BIT && (ci.usage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT);
}
void record(int eye, VkImage image, const VkImageCreateInfo& ci, const char* how) {
    g_image[eye - 1] = image;
    g_format = ci.format;
    g_w = ci.extent.width;
    g_h = ci.extent.height;
    VRLOG("eyes: eye %d image %p %ux%u format %d usage %x (%s)", eye, (void*)image, g_w, g_h, int(ci.format), ci.usage, how);
}
}  // namespace

void beginCapture(int eye, uint32_t w, uint32_t h) {
    AcquireSRWLockExclusive(&g_lock);
    g_cap = {eye, w, h, GetCurrentThreadId(), false, false};
    g_pending[eye - 1] = {};
    ReleaseSRWLockExclusive(&g_lock);
}

void endCapture() {
    AcquireSRWLockExclusive(&g_lock);
    if (g_cap.eye && !g_cap.got) {
        g_pending[g_cap.eye - 1] = {g_cap.w, g_cap.h, GetTickCount64() + kPendingMs};
        VRLOG("eyes: no image created for eye %d inside the engine call - waiting up to %llu ms", g_cap.eye, kPendingMs);
    }
    g_cap = {};
    ReleaseSRWLockExclusive(&g_lock);
}

void onImageCreated(VkImage image, const VkImageCreateInfo& ci) {
    AcquireSRWLockExclusive(&g_lock);
    if (g_cap.eye && g_cap.thread == GetCurrentThreadId() && eyeCandidate(ci, g_cap.w, g_cap.h)) {
        bool preferred = (ci.usage & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) != 0;
        // First candidate, upgraded once by a TRANSFER_SRC one; never replaced by a later plain one.
        if (!g_cap.got || (preferred && !g_cap.gotPreferred)) {
            record(g_cap.eye, image, ci, g_cap.got ? "replaces earlier candidate" : "in engine call");
            g_cap.got = true;
            g_cap.gotPreferred = preferred;
        }
    } else {
        ULONGLONG now = GetTickCount64();
        for (int e = 0; e < 2; ++e) {
            Pending& p = g_pending[e];
            if (!p.deadline || now > p.deadline || !eyeCandidate(ci, p.w, p.h) || image == g_image[1 - e]) continue;
            record(e + 1, image, ci, "deferred");
            p = {};
            break;
        }
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
