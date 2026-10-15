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

add_library(pop_native STATIC src/native_renderer.cpp)
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
