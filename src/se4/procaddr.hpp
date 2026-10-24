#pragma once
// One GetProcAddress hook for game-module callers; subsystems substitute their own entry points.
#include <windows.h>

namespace se4 {
bool installProcAddressHook();
bool callerInGame(void* returnAddress);
}  // namespace se4

namespace vk_layer {
FARPROC interceptProc(HMODULE module, const char* name, FARPROC real);  // vulkan-1.dll
}
namespace gamepad {
FARPROC interceptProc(HMODULE module, const char* name, FARPROC real);  // xinput*.dll
}
