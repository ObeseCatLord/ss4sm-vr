#pragma once
// Bridge between the engine's eye render targets (created by the VR device through engine functions)
// and the Vulkan images behind them, plus per-frame submit bookkeeping for the projection layer.
#include <vulkan/vulkan.h>
#include <cstdint>

namespace eyes {
// Eyes 1 and 2 are the stereo views; eye 3 is the scope view (src/se4/scope.cpp).
// The device brackets the engine's render-target creation for an eye; images created on this thread
// with the given extent inside the bracket are recorded as that eye's image.
void beginCapture(int eye, uint32_t width, uint32_t height);
// expectNew: the call was meant to create an image (first use or resize); otherwise silence is normal.
void endCapture(bool expectNew = true);
// Called by the vkCreateImage interception.
void onImageCreated(VkImage image, const VkImageCreateInfo& ci);
void onImageDestroyed(VkImage image);
// Slot 0x18 (SubmitEye): the engine finished an eye for this frame (eye 3: the scope transition in slot 0x038).
void submitted(int eye);

struct Frame {
    VkImage image[2]{};
    VkFormat format = VK_FORMAT_UNDEFINED;
    uint32_t width = 0, height = 0;
    bool ready = false;  // both eyes captured and submitted since the last take()
};
// Consumed once per present.
Frame take();

struct ScopeFrame {
    VkImage image = VK_NULL_HANDLE;  // TRANSFER_SRC_OPTIMAL when ready
    VkFormat format = VK_FORMAT_UNDEFINED;
    uint32_t width = 0, height = 0;
    bool ready = false;  // rendered and transitioned since the last takeScope()
};
ScopeFrame takeScope();
}  // namespace eyes
