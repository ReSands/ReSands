# Native renderer (Stage B): pop_native core library + pop_native_view (offscreen viewer for judging
# against GS frames). Portable: Vulkan through volk (no SDK needed; the loader comes with the GPU
# driver on Windows/Linux, MoltenVK + vulkan-loader on macOS), dependencies fetched at pinned
# versions, shaders embedded as SPIR-V (shaders/build.sh).
#   cmake -S tools/native -B game/native-build -DCMAKE_BUILD_TYPE=Release && cmake --build game/native-build
cmake_minimum_required(VERSION 3.20)
project(pop_native CXX C)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(FetchContent)
FetchContent_Declare(vulkan_headers GIT_REPOSITORY https://github.com/KhronosGroup/Vulkan-Headers.git
                     GIT_TAG vulkan-sdk-1.4.341.0 GIT_SHALLOW TRUE)
FetchContent_Declare(volk GIT_REPOSITORY https://github.com/zeux/volk.git
                     GIT_TAG vulkan-sdk-1.4.341.0 GIT_SHALLOW TRUE)
# header-only: SOURCE_SUBDIR points at no CMakeLists, so MakeAvailable only downloads them
FetchContent_Declare(cgltf GIT_REPOSITORY https://github.com/jkuhlmann/cgltf.git
                     GIT_TAG v1.15 GIT_SHALLOW TRUE SOURCE_SUBDIR no-cmake)
FetchContent_Declare(stb GIT_REPOSITORY https://github.com/nothings/stb.git
                     GIT_TAG 31c1ad37456438565541f4919958214b6e762fb4 SOURCE_SUBDIR no-cmake)
FetchContent_MakeAvailable(vulkan_headers)
set(VOLK_PULL_IN_VULKAN OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(volk)
FetchContent_MakeAvailable(cgltf stb)
target_link_libraries(volk PUBLIC Vulkan::Headers)

add_library(pop_native STATIC src/native_renderer.cpp src/meshdecode.cpp src/assets.cpp)
target_include_directories(pop_native PUBLIC src)
target_link_libraries(pop_native PUBLIC volk Vulkan::Headers)

# SDL3 for the window: an installed package if present (Homebrew), otherwise built from source
find_package(SDL3 3.4 CONFIG QUIET)
if(NOT SDL3_FOUND)
  set(SDL_SHARED OFF CACHE BOOL "" FORCE)
  set(SDL_STATIC ON CACHE BOOL "" FORCE)
  set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
  FetchContent_Declare(sdl3 GIT_REPOSITORY https://github.com/libsdl-org/SDL.git
                       GIT_TAG release-3.4.16 GIT_SHALLOW TRUE)
  FetchContent_MakeAvailable(sdl3)
endif()

add_executable(pop_native_view src/viewer.cpp)
target_include_directories(pop_native_view PRIVATE ${cgltf_SOURCE_DIR} ${stb_SOURCE_DIR})
target_link_libraries(pop_native_view PRIVATE pop_native SDL3::SDL3)

# pop_assets_test: lists every texture record of a pack decoded by pop_assets (judge: tools/native/assets_check.py)
add_executable(pop_assets_test src/assets_test.cpp)
target_link_libraries(pop_assets_test PRIVATE pop_native)

# pop_postfx_check: judges postfx.h against the game's per-pass framebuffer dumps (POP_PASSDUMP_AT)
add_executable(pop_postfx_check src/postfx_check.cpp)
target_include_directories(pop_postfx_check PRIVATE src ${stb_SOURCE_DIR})

# pop_postfx_gpu_judge: GPU (compute) ports of the post effects vs the CPU references in postfx.h
add_executable(pop_postfx_gpu_judge src/postfx_gpu_judge.cpp src/gpu_compute.cpp)
target_include_directories(pop_postfx_gpu_judge PRIVATE src ${stb_SOURCE_DIR})
target_link_libraries(pop_postfx_gpu_judge PRIVATE volk Vulkan::Headers)

# pop_state_frame: a native frame from a pop-sotc save state, offline, with the live code
# (tools/overrides/pop_native_live.cpp) compiled in. Needs the runtime's headers:
#   -DPOP_RUNTIME_SRC=<repo>/game/recomp/runtime-src (Mac) or game/recomp-sotc/runtime-src (PC)
# Its src/runner directory sits six levels below the repository root, which the live code's
# "../../../../../../tools/native/src/*.cpp" includes rely on.
set(POP_RUNTIME_SRC "" CACHE PATH "PS2Recomp runtime source for pop_state_frame")
if(POP_RUNTIME_SRC)
  add_executable(pop_state_frame src/state_frame.cpp)
  set_target_properties(pop_state_frame PROPERTIES CXX_STANDARD 20) # the runtime headers use C++20
  target_include_directories(pop_state_frame PRIVATE src ${POP_RUNTIME_SRC}/ps2xRuntime/include
                             ${POP_RUNTIME_SRC}/ps2xRuntime/src/lib/Kernel ${POP_RUNTIME_SRC}/ps2xIOP/include
                             ${POP_RUNTIME_SRC}/ps2xRuntime/src/runner)
  if(CMAKE_SYSTEM_PROCESSOR MATCHES "arm64|aarch64|ARM64")
    FetchContent_Declare(sse2neon GIT_REPOSITORY https://github.com/DLTcollab/sse2neon.git
                         GIT_TAG v1.9.1 GIT_SHALLOW TRUE SOURCE_SUBDIR no-cmake)
    FetchContent_MakeAvailable(sse2neon)
    target_include_directories(pop_state_frame PRIVATE ${sse2neon_SOURCE_DIR})
    target_compile_definitions(pop_state_frame PRIVATE USE_SSE2NEON)
  endif()
endif()
