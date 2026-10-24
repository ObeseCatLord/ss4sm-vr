cmake_minimum_required(VERSION 3.24)
project(ss4smvr LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

option(SS4SMVR_TESTS "Build portable host-side unit tests instead of Windows targets" OFF)
set(OPENXR_SOURCE "" CACHE PATH "Local OpenXR-SDK checkout (pinned 75c53b6, OpenXR 1.1.53)")
set(MINHOOK_SOURCE "" CACHE PATH "Local MinHook checkout (pinned c3fcafd, v1.3.4)")
set(VULKAN_HEADERS_ROOT "/usr/include" CACHE PATH "Directory containing vulkan/ and vk_video/ headers")

if(SS4SMVR_TESTS)
  enable_testing()
  add_subdirectory(tests)
  return()
endif()

if(NOT WIN32 OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
  message(FATAL_ERROR "Windows targets must build for x86-64 (use cmake/mingw-w64-x86_64.cmake)")
endif()

# Stage only the Vulkan headers so the cross compiler never sees the rest of the host /usr/include.
set(VK_STAGE ${CMAKE_BINARY_DIR}/vkheaders)
file(COPY ${VULKAN_HEADERS_ROOT}/vulkan ${VULKAN_HEADERS_ROOT}/vk_video DESTINATION ${VK_STAGE})

set(BUILD_API_LAYERS OFF CACHE BOOL "" FORCE)
set(BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(BUILD_LOADER ON CACHE BOOL "" FORCE)
set(DYNAMIC_LOADER ON CACHE BOOL "" FORCE)
set(BUILD_WITH_SYSTEM_JSONCPP OFF CACHE BOOL "" FORCE)
include(FetchContent)
if(OPENXR_SOURCE)
  add_subdirectory(${OPENXR_SOURCE} ${CMAKE_BINARY_DIR}/openxr EXCLUDE_FROM_ALL)
else()
  FetchContent_Declare(openxr BINARY_DIR ${CMAKE_BINARY_DIR}/openxr
    GIT_REPOSITORY https://github.com/KhronosGroup/OpenXR-SDK.git GIT_TAG 75c53b6e853dc12c7b3c771edc9c9c841b15faaa)
  FetchContent_MakeAvailable(openxr)
endif()
target_link_options(openxr_loader PRIVATE -static -static-libgcc -static-libstdc++)

add_executable(vk_xr_probe src/probe/vk_xr_probe.cpp)
target_include_directories(vk_xr_probe PRIVATE src ${VK_STAGE})
target_compile_definitions(vk_xr_probe PRIVATE VK_NO_PROTOTYPES XR_USE_GRAPHICS_API_VULKAN WIN32_LEAN_AND_MEAN NOMINMAX)
target_link_libraries(vk_xr_probe PRIVATE openxr_loader)
target_link_options(vk_xr_probe PRIVATE -static -static-libgcc -static-libstdc++)

# MinHook (static)
if(NOT MINHOOK_SOURCE)
  FetchContent_Declare(minhook GIT_REPOSITORY https://github.com/TsudaKageyu/minhook.git GIT_TAG c3fcafdc10146beb5919319d0683e44e3c30d537)
  FetchContent_Populate(minhook)
  set(MINHOOK_SOURCE ${minhook_SOURCE_DIR})
endif()
add_library(minhook STATIC ${MINHOOK_SOURCE}/src/buffer.c ${MINHOOK_SOURCE}/src/hook.c ${MINHOOK_SOURCE}/src/trampoline.c
  ${MINHOOK_SOURCE}/src/hde/hde64.c)
target_include_directories(minhook PUBLIC ${MINHOOK_SOURCE}/include)

# msimg32.dll proxy (Bin/x64/msimg32.dll)
add_library(msimg32_proxy SHARED src/proxy/proxy.cpp src/proxy/msimg32.def)
set_target_properties(msimg32_proxy PROPERTIES OUTPUT_NAME msimg32 PREFIX "")
target_compile_definitions(msimg32_proxy PRIVATE WIN32_LEAN_AND_MEAN)
target_link_options(msimg32_proxy PRIVATE -static -static-libgcc -static-libstdc++)

# Payload (Bin/x64/ss4smvr/ss4smvr.dll)
add_library(ss4smvr SHARED src/se4/payload.cpp src/se4/log.cpp src/se4/build.cpp src/se4/cvars.cpp src/se4/procaddr.cpp src/se4/gamepad.cpp src/se4/focus.cpp src/se4/vr_device.cpp src/se4/player_vr.cpp src/se4/options.cpp
  src/vk/vk_layer.cpp src/vk/queue_lock.cpp src/vk/eye_images.cpp src/xr/runtime.cpp src/xr/input.cpp)
set_target_properties(ss4smvr PROPERTIES PREFIX "")
target_include_directories(ss4smvr PRIVATE src ${VK_STAGE})
target_compile_definitions(ss4smvr PRIVATE VK_USE_PLATFORM_WIN32_KHR VK_NO_PROTOTYPES XR_USE_GRAPHICS_API_VULKAN WIN32_LEAN_AND_MEAN NOMINMAX)
target_link_libraries(ss4smvr PRIVATE minhook openxr_loader)
target_link_options(ss4smvr PRIVATE -static -static-libgcc -static-libstdc++)
