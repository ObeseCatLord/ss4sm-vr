#include "vk/vk_observe.hpp"
#include "se4/log.hpp"
#include <windows.h>
#include <vulkan/vulkan.h>
#include <MinHook.h>
#include <cstring>

namespace vk_observe {
namespace {
PFN_vkGetInstanceProcAddr realGIPA;
PFN_vkCreateInstance realCreateInstance;
PFN_vkCreateDevice realCreateDevice;

void logChain(const char* what, const void* pNext) {
    for (auto* s = static_cast<const VkBaseInStructure*>(pNext); s; s = s->pNext) {
        if (s->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MULTIVIEW_FEATURES) {
            auto* m = reinterpret_cast<const VkPhysicalDeviceMultiviewFeatures*>(s);
            VRLOG("vk: %s chain multiview features multiview=%u", what, m->multiview);
        } else if (s->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES) {
            auto* m = reinterpret_cast<const VkPhysicalDeviceVulkan11Features*>(s);
            VRLOG("vk: %s chain vulkan11 features multiview=%u", what, m->multiview);
        } else {
            VRLOG("vk: %s chain sType %d", what, int(s->sType));
        }
    }
}

VKAPI_ATTR VkResult VKAPI_CALL hookCreateInstance(const VkInstanceCreateInfo* ci, const VkAllocationCallbacks* a, VkInstance* out) {
    VRLOG("vk: vkCreateInstance api %u.%u, %u layers, %u extensions", VK_VERSION_MAJOR(ci->pApplicationInfo ? ci->pApplicationInfo->apiVersion : 0),
          VK_VERSION_MINOR(ci->pApplicationInfo ? ci->pApplicationInfo->apiVersion : 0), ci->enabledLayerCount, ci->enabledExtensionCount);
    for (uint32_t i = 0; i < ci->enabledExtensionCount; ++i) VRLOG("vk:   instance ext %s", ci->ppEnabledExtensionNames[i]);
    VkResult r = realCreateInstance(ci, a, out);
    VRLOG("vk: vkCreateInstance -> %d", int(r));
    return r;
}

VKAPI_ATTR VkResult VKAPI_CALL hookCreateDevice(VkPhysicalDevice pd, const VkDeviceCreateInfo* ci, const VkAllocationCallbacks* a, VkDevice* out) {
    VRLOG("vk: vkCreateDevice %u queue infos, %u extensions", ci->queueCreateInfoCount, ci->enabledExtensionCount);
    for (uint32_t i = 0; i < ci->queueCreateInfoCount; ++i)
        VRLOG("vk:   queue family %u count %u", ci->pQueueCreateInfos[i].queueFamilyIndex, ci->pQueueCreateInfos[i].queueCount);
    for (uint32_t i = 0; i < ci->enabledExtensionCount; ++i) VRLOG("vk:   device ext %s", ci->ppEnabledExtensionNames[i]);
    logChain("vkCreateDevice", ci->pNext);
    VkResult r = realCreateDevice(pd, ci, a, out);
    VRLOG("vk: vkCreateDevice -> %d", int(r));
    return r;
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL hookGIPA(VkInstance inst, const char* name) {
    if (name && !strcmp(name, "vkCreateInstance")) return reinterpret_cast<PFN_vkVoidFunction>(hookCreateInstance);
    if (name && !strcmp(name, "vkCreateDevice")) return reinterpret_cast<PFN_vkVoidFunction>(hookCreateDevice);
    return realGIPA(inst, name);
}

template <class T> bool hook(HMODULE m, const char* name, void* detour, T* orig) {
    void* target = reinterpret_cast<void*>(GetProcAddress(m, name));
    if (!target) { VRLOG("vk: export %s missing", name); return false; }
    if (MH_CreateHook(target, detour, reinterpret_cast<void**>(orig)) != MH_OK || MH_EnableHook(target) != MH_OK) {
        VRLOG("vk: hook %s failed", name);
        return false;
    }
    return true;
}
}  // namespace

void onVulkanLoaded(void* module) {
    auto m = static_cast<HMODULE>(module);
    bool ok = hook(m, "vkGetInstanceProcAddr", reinterpret_cast<void*>(hookGIPA), &realGIPA) &&
              hook(m, "vkCreateInstance", reinterpret_cast<void*>(hookCreateInstance), &realCreateInstance) &&
              hook(m, "vkCreateDevice", reinterpret_cast<void*>(hookCreateDevice), &realCreateDevice);
    VRLOG("vk: observation hooks %s", ok ? "installed" : "incomplete");
}
}  // namespace vk_observe
