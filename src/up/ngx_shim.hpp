#pragma once
// NGX (DLSS) without the MSVC static library; see ngx_shim.cpp.
#include <windows.h>
#include <vulkan/vulkan.h>
#include "nvsdk_ngx.h"
#include "nvsdk_ngx_vk.h"
#include "nvsdk_ngx_helpers_vk.h"

namespace ngx {
bool load();                 // the driver's NGX core (_nvngx.dll)
FARPROC sym(const char* name);
bool selfTest(NVSDK_NGX_Parameter* p);
// Core-level init (snippet-build signature of the export).
using InitExt2Fn = NVSDK_NGX_Result(NVSDK_CONV*)(unsigned long long appId, const wchar_t* dataPath, VkInstance, VkPhysicalDevice,
                                                 VkDevice, PFN_vkGetInstanceProcAddr, PFN_vkGetDeviceProcAddr,
                                                 NVSDK_NGX_Version, const NVSDK_NGX_Parameter*);
}  // namespace ngx
