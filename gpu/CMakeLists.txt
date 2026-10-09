# bbport GPU library: shadPS4 video core + shader recompiler + Gnm/VideoOut
# drivers, adapted to the native_probe runtime through shims in shim/.
cmake_minimum_required(VERSION 3.24)
project(bbgpu LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
if (NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()
# Frame pointers keep the loader's thread dumps (watchdog, SIGUSR2 sampling) walkable.
add_compile_options(-fno-omit-frame-pointer)

# bbport: link-time optimization (BB_LTO, default on) and profile-guided optimization
# (BB_PGO: off, generate = instrumented build writing into BB_PGO_DIR, use = build with that
# profile). Covers bbgpu and the static libraries linked into it. No -march: the same build
# runs on the Steam Deck.
option(BB_LTO "Link-time optimization of bbgpu" ON)
set(BB_PGO "off" CACHE STRING "Profile-guided optimization: off, generate, use")
set(BB_PGO_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../pgo" CACHE PATH "Profile directory")
if (BB_LTO)
    include(CheckIPOSupported)
    check_ipo_supported(RESULT bb_ipo_ok OUTPUT bb_ipo_error)
    if (bb_ipo_ok)
        set(CMAKE_INTERPROCEDURAL_OPTIMIZATION ON)
    else()
        message(WARNING "LTO unavailable: ${bb_ipo_error}")
    endif()
endif()
if (BB_PGO STREQUAL "generate")
    # Atomic counters: the GPU, recording, preparation and copy threads run the same code.
    add_compile_options(-fprofile-generate=${BB_PGO_DIR} -fprofile-update=atomic)
    add_link_options(-fprofile-generate=${BB_PGO_DIR})
    add_compile_definitions(BB_PGO_GENERATE)
elseif (BB_PGO STREQUAL "use")
    # Partial training keeps unprofiled code optimized normally; counters from concurrent
    # threads can be slightly inconsistent (correction). Functions changed since the profile
    # (or built differently for it, bbgpu_init) are compiled without it.
    add_compile_options(-fprofile-use=${BB_PGO_DIR} -fprofile-partial-training
                        -fprofile-correction -Wno-missing-profile -Wno-coverage-mismatch)
endif()

if (WIN32)
    if (DEFINED ENV{MINGW_PREFIX})
        list(APPEND CMAKE_PREFIX_PATH "$ENV{MINGW_PREFIX}")
    elseif (EXISTS "C:/msys64/mingw64")
        list(APPEND CMAKE_PREFIX_PATH "C:/msys64/mingw64")
    endif()
    if (DEFINED ENV{SDL3_DIR})
        list(APPEND CMAKE_PREFIX_PATH "$ENV{SDL3_DIR}")
    endif()
    if (DEFINED ENV{VULKAN_SDK})
        list(APPEND CMAKE_PREFIX_PATH "$ENV{VULKAN_SDK}")
    endif()
    add_compile_definitions(WIN32_LEAN_AND_MEAN NOMINMAX)
    if (EXISTS "C:/msys64/mingw64/lib")
        link_directories("C:/msys64/mingw64/lib")
    endif()
endif()

find_package(Vulkan REQUIRED)
find_package(fmt REQUIRED)
find_package(Boost REQUIRED)
if (WIN32)
    if (NOT TARGET magic_enum::magic_enum)
        add_library(magic_enum::magic_enum INTERFACE IMPORTED)
        find_path(MAGIC_ENUM_INC NAMES magic_enum/magic_enum.hpp PATHS "$ENV{MINGW_PREFIX}/include" "C:/msys64/mingw64/include")
        if (MAGIC_ENUM_INC)
            target_include_directories(magic_enum::magic_enum INTERFACE "${MAGIC_ENUM_INC}")
        endif()
    endif()
else()
    find_package(magic_enum REQUIRED)
endif()
find_package(tsl-robin-map REQUIRED)
if (NOT TARGET GPUOpen::VulkanMemoryAllocator)
    add_library(GPUOpen::VulkanMemoryAllocator INTERFACE IMPORTED)
    find_path(VMA_INC NAMES vk_mem_alloc.h PATHS "$ENV{MINGW_PREFIX}/include" "C:/msys64/mingw64/include")
    if (VMA_INC)
        target_include_directories(GPUOpen::VulkanMemoryAllocator INTERFACE "${VMA_INC}")
    endif()
endif()
find_package(PkgConfig REQUIRED)
pkg_check_modules(XXHASH REQUIRED IMPORTED_TARGET libxxhash)
if (WIN32)
    find_package(SDL3 QUIET CONFIG)
    if (TARGET SDL3::SDL3)
        add_library(PkgConfig::SDL3 INTERFACE IMPORTED)
        target_link_libraries(PkgConfig::SDL3 INTERFACE SDL3::SDL3)
    else()
        find_path(SDL3_INC NAMES SDL3/SDL.h PATHS "$ENV{SDL3_DIR}/include" "C:/msys64/mingw64/include")
        find_library(SDL3_LIB NAMES SDL3 libSDL3.dll.a PATHS "$ENV{SDL3_DIR}/lib" "C:/msys64/mingw64/lib")
        if (SDL3_INC AND SDL3_LIB)
            add_library(PkgConfig::SDL3 INTERFACE IMPORTED)
            target_include_directories(PkgConfig::SDL3 INTERFACE "${SDL3_INC}")
            target_link_libraries(PkgConfig::SDL3 INTERFACE "${SDL3_LIB}")
        else()
            pkg_check_modules(SDL3 REQUIRED IMPORTED_TARGET sdl3)
        endif()
    endif()
elseif (APPLE)
    pkg_check_modules(SDL3 REQUIRED IMPORTED_TARGET sdl3)
else()
    pkg_check_modules(SDL3 REQUIRED IMPORTED_TARGET sdl3)
    pkg_check_modules(X11 REQUIRED IMPORTED_TARGET x11)
endif()
if (WIN32)
    if (NOT TARGET miniz::miniz)
        add_library(miniz::miniz INTERFACE IMPORTED)
        find_path(MINIZ_INC NAMES miniz.h PATHS "$ENV{MINGW_PREFIX}/include" "C:/msys64/mingw64/include")
        if (MINIZ_INC)
            target_include_directories(miniz::miniz INTERFACE "${MINIZ_INC}")
        endif()
        find_library(MINIZ_LIB NAMES miniz libminiz.a PATHS "$ENV{MINGW_PREFIX}/lib" "C:/msys64/mingw64/lib")
        if (MINIZ_LIB)
            target_link_libraries(miniz::miniz INTERFACE "${MINIZ_LIB}")
        endif()
    endif()
else()
    find_package(miniz REQUIRED)
endif()
find_package(Zydis REQUIRED)
find_program(GLSLANG_COMPILER NAMES glslang glslangValidator PATHS "C:/msys64/mingw64/bin" REQUIRED)

set(SIRIT_USE_SYSTEM_SPIRV_HEADERS OFF CACHE BOOL "" FORCE)
add_subdirectory(third_party/sirit EXCLUDE_FROM_ALL)

# bbport: FSR 3.1 temporal upscaling / frame generation, native Vulkan (MIT, FireBurn/FSR-Vulkan).
set(FFX_VK_PORTABLE_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(FFX_VK_PORTABLE_BUILD_PROBE OFF CACHE BOOL "" FORCE)
add_subdirectory(third_party/fsr-vulkan EXCLUDE_FROM_ALL)

# Host shaders -> headers (same scheme as shadPS4's host_shaders/CMakeLists.txt).
set(SHADER_SRC ${CMAKE_CURRENT_SOURCE_DIR}/shadps4/video_core/host_shaders)
set(SHADER_INCLUDE ${CMAKE_CURRENT_BINARY_DIR}/include)
set(SHADER_DIR ${SHADER_INCLUDE}/video_core/host_shaders)
set(SHADER_HEADERS "")
function(add_host_shader FILENAME SHADER_NAME)
    set(OUT ${SHADER_DIR}/${SHADER_NAME}.h)
    string(TOUPPER ${SHADER_NAME} SYMBOL)
    set(DEFINES ${ARGN})
    list(TRANSFORM DEFINES PREPEND "-D")
    add_custom_command(OUTPUT ${OUT}
        COMMAND ${CMAKE_COMMAND} -E make_directory ${SHADER_DIR}
        COMMAND ${GLSLANG_COMPILER} --target-env vulkan1.3 -o ${OUT} --vn ${SYMBOL} ${DEFINES} ${SHADER_SRC}/${FILENAME}
        COMMAND ${CMAKE_COMMAND} "-DHEADER_FILE=${OUT}" -P ${SHADER_SRC}/AddShaderHeaderInclude.cmake
        DEPENDS ${SHADER_SRC}/${FILENAME} VERBATIM)
    set(SHADER_HEADERS ${SHADER_HEADERS} ${OUT} PARENT_SCOPE)
endfunction()
add_host_shader(color_to_ms_depth.frag color_to_ms_depth_frag)
add_host_shader(ms_image_blit.frag ms_image_blit_frag)
add_host_shader(ms_image_blit.frag ms_image_blit_msaa_frag SRC_MSAA=1)
add_host_shader(fault_buffer_process.comp fault_buffer_process_comp)
add_host_shader(fs_tri.vert fs_tri_vert)
add_host_shader(fsr.comp fsr_easu_comp SAMPLE_EASU=1)
add_host_shader(fsr.comp fsr_rcas_comp SAMPLE_RCAS=1)
add_host_shader(post_process.frag post_process_frag)
add_host_shader(camera_motion.comp camera_motion_comp)
add_host_shader(upscale_merge.comp upscale_merge_comp)
add_host_shader(upscale_reactive.comp upscale_reactive_comp)
add_host_shader(taa.comp taa_comp)
add_host_shader(taa_sharpen.comp taa_sharpen_comp)
add_host_shader(taa_sharpen.comp taa_sharpen_ldr_comp LDR_OUTPUT=1)
add_host_shader(buffer_multi_copy.comp buffer_multi_copy_comp)
add_host_shader(depth_resample.frag depth_resample_frag)
add_host_shader(depth_resample.frag depth_stencil_resample_frag HAS_STENCIL=1)
add_host_shader(depth_resample.frag depth_stencil_bits_frag STENCIL_BITS=1)
add_host_shader(camera_motion.comp camera_motion_debug_comp DEBUG_OVERLAY=1)
foreach(BPP IN ITEMS 8 16 32 64 96 128)
    add_host_shader(tiling.comp tiling_micro_${BPP}_comp BITS_PER_PIXEL=${BPP} IS_MACRO_TILED=0)
    add_host_shader(tiling.comp tiling_macro_${BPP}_comp BITS_PER_PIXEL=${BPP} IS_MACRO_TILED=1)
endforeach()
add_custom_target(host_shaders DEPENDS ${SHADER_HEADERS})

file(GLOB_RECURSE VIDEO_CORE CONFIGURE_DEPENDS shadps4/video_core/*.cpp)
file(GLOB_RECURSE RECOMPILER CONFIGURE_DEPENDS shadps4/shader_recompiler/*.cpp)
list(FILTER VIDEO_CORE EXCLUDE REGEX "renderdoc\\.cpp$")
set(COMMON
    shadps4/common/decoder.cpp shadps4/common/error.cpp
    shadps4/common/io_file.cpp shadps4/common/native_clock.cpp shadps4/common/number_utils.cpp
    shadps4/common/path_util.cpp shadps4/common/rdtsc.cpp shadps4/common/spin_lock.cpp shadps4/common/recursive_lock.cpp
    shadps4/common/string_util.cpp shadps4/common/thread.cpp shadps4/common/signal_context.cpp
    shim/common/logging/log.cpp)
file(GLOB AVPLAYER CONFIGURE_DEPENDS shadps4/core/libraries/avplayer/*.cpp)
pkg_check_modules(FFMPEG REQUIRED IMPORTED_TARGET libavformat libavcodec libavutil libswscale libswresample)
if (APPLE)
    # pkg_check_modules folds repeated "-framework X" flags into one "-framework X Y Z";
    # the frameworks of a static FFmpeg are linked explicitly instead.
    set_property(TARGET PkgConfig::FFMPEG PROPERTY INTERFACE_LINK_OPTIONS "")
    target_link_libraries(PkgConfig::FFMPEG INTERFACE "-framework AudioToolbox" "-framework VideoToolbox"
        "-framework CoreFoundation" "-framework CoreMedia" "-framework CoreVideo" "-framework CoreServices"
        "-framework Security")
endif()
file(GLOB SHIM_SOURCES CONFIGURE_DEPENDS shim/*.cpp shim/*/*.cpp)
if (WIN32)
    add_library(bbgpu STATIC ${VIDEO_CORE} ${RECOMPILER} ${COMMON} ${SHIM_SOURCES}
        shadps4/core/libraries/gnmdriver/gnmdriver.cpp
        shadps4/core/libraries/videoout/driver.cpp shadps4/core/libraries/videoout/video_out.cpp
        shadps4/core/libraries/kernel/equeue.cpp ${AVPLAYER}
        shadps4/core/libraries/videodec/video_utils.cpp)
else()
    add_library(bbgpu SHARED ${VIDEO_CORE} ${RECOMPILER} ${COMMON} ${SHIM_SOURCES}
        shadps4/core/libraries/gnmdriver/gnmdriver.cpp
        shadps4/core/libraries/videoout/driver.cpp shadps4/core/libraries/videoout/video_out.cpp
        shadps4/core/libraries/kernel/equeue.cpp ${AVPLAYER}
        shadps4/core/libraries/videodec/video_utils.cpp)
endif()
add_dependencies(bbgpu host_shaders)
# In-game settings menu: Dear ImGui (third_party/imgui, MIT) with its Vulkan backend, and an
# embedded Cyrillic font (third_party/fonts).
target_sources(bbgpu PRIVATE third_party/imgui/imgui.cpp third_party/imgui/imgui_draw.cpp
    third_party/imgui/imgui_tables.cpp third_party/imgui/imgui_widgets.cpp
    third_party/imgui/backends/imgui_impl_vulkan.cpp)
target_include_directories(bbgpu PRIVATE third_party/imgui third_party/imgui/backends)
target_compile_definitions(bbgpu PRIVATE IMGUI_IMPL_VULKAN_NO_PROTOTYPES
    BB_FONT_PATH="${CMAKE_CURRENT_SOURCE_DIR}/third_party/fonts/DejaVuSans.ttf")
set_source_files_properties(shim/bbport_overlay.cpp PROPERTIES
    OBJECT_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/third_party/fonts/DejaVuSans.ttf)
target_include_directories(bbgpu PRIVATE shim shadps4 ${SHADER_INCLUDE} third_party/gcn/include third_party/half/include)
target_compile_definitions(bbgpu PRIVATE BOOST_ASIO_STANDALONE NDEBUG)
if (WIN32)
    target_compile_definitions(bbgpu PRIVATE VK_USE_PLATFORM_WIN32_KHR)
elseif (APPLE)
    target_compile_definitions(bbgpu PRIVATE VK_USE_PLATFORM_METAL_EXT)
endif()
target_compile_options(bbgpu PRIVATE -Wno-deprecated-declarations -Wno-missing-field-initializers)
if (APPLE)
    # Runtime symbols (runtime_memory_*, runtime_process_*) resolve against bb-probe at load time.
    target_link_options(bbgpu PRIVATE -Wl,-undefined,dynamic_lookup)
elseif (NOT WIN32)
    # Runtime symbols (runtime_memory_*, runtime_process_*) resolve against bb-probe.
    target_link_options(bbgpu PRIVATE -Wl,--allow-shlib-undefined)
endif()
target_link_libraries(bbgpu PRIVATE ffx-vulkan::portable ffx-vulkan::fsr4-v07-vulkan)
if (WIN32)
    target_link_libraries(bbgpu PUBLIC Vulkan::Headers fmt::fmt Boost::headers magic_enum::magic_enum
        tsl::robin_map GPUOpen::VulkanMemoryAllocator PkgConfig::XXHASH PkgConfig::SDL3 PkgConfig::FFMPEG miniz::miniz Zydis::Zydis sirit ws2_32)
elseif (APPLE)
    target_link_libraries(bbgpu PUBLIC Vulkan::Headers fmt::fmt Boost::headers magic_enum::magic_enum
        tsl::robin_map GPUOpen::VulkanMemoryAllocator PkgConfig::XXHASH PkgConfig::SDL3 PkgConfig::FFMPEG miniz::miniz Zydis::Zydis sirit)
else()
    target_link_libraries(bbgpu PUBLIC Vulkan::Headers fmt::fmt Boost::headers magic_enum::magic_enum
        tsl::robin_map GPUOpen::VulkanMemoryAllocator PkgConfig::XXHASH PkgConfig::SDL3 PkgConfig::X11 PkgConfig::FFMPEG miniz::miniz Zydis::Zydis sirit)
endif()

# Explicit test targets; neither launches the game nor needs a Vulkan device.
add_executable(shader-user-data-test EXCLUDE_FROM_ALL ../tests/test_shader_user_data.cpp)
target_compile_options(shader-user-data-test PRIVATE -UNDEBUG)
target_include_directories(shader-user-data-test PRIVATE shim shadps4 third_party/gcn/include)
target_link_libraries(shader-user-data-test PRIVATE fmt::fmt Boost::headers)

add_executable(motion-history-test EXCLUDE_FROM_ALL ../tests/test_motion_history.cpp)
target_compile_options(motion-history-test PRIVATE -UNDEBUG -Wall -Wextra -Werror)
target_include_directories(motion-history-test PRIVATE ..)
add_executable(motion-shader-test EXCLUDE_FROM_ALL ../tests/test_motion_shaders.cpp ../tests/test_gpu_runtime_stubs.cpp)
target_include_directories(motion-shader-test PRIVATE shim shadps4 third_party/gcn/include)
target_link_libraries(motion-shader-test PRIVATE bbgpu)
add_executable(ui-composition-test EXCLUDE_FROM_ALL ../tests/test_ui_composition.cpp)
target_compile_options(ui-composition-test PRIVATE -UNDEBUG -Wall -Wextra -Werror)
target_include_directories(ui-composition-test PRIVATE ..)

add_executable(scene-resolution-test EXCLUDE_FROM_ALL ../tests/test_scene_resolution.cpp ../tests/test_gpu_runtime_stubs.cpp)
target_include_directories(scene-resolution-test PRIVATE shim shadps4 third_party/gcn/include)
# The test's Vulkan-Hpp inline methods have a different set of platform extensions
# from vk_platform.cpp. Keep them local and use an explicit dispatcher in the test.
target_compile_options(scene-resolution-test PRIVATE -UNDEBUG -fPIC)
set_source_files_properties(../tests/test_scene_resolution.cpp PROPERTIES
    COMPILE_OPTIONS "-fvisibility=hidden;-fvisibility-inlines-hidden")
target_link_libraries(scene-resolution-test PRIVATE bbgpu)
add_executable(taa-shader-test EXCLUDE_FROM_ALL ../tests/test_taa_shader.cpp ../tests/test_gpu_runtime_stubs.cpp)
target_include_directories(taa-shader-test PRIVATE shim shadps4 ${SHADER_INCLUDE} third_party/gcn/include)
target_compile_options(taa-shader-test PRIVATE -UNDEBUG -fPIC)
set_source_files_properties(../tests/test_taa_shader.cpp PROPERTIES
    COMPILE_OPTIONS "-fvisibility=hidden;-fvisibility-inlines-hidden")
target_link_libraries(taa-shader-test PRIVATE bbgpu)
add_dependencies(taa-shader-test host_shaders)

add_executable(camera-motion-test EXCLUDE_FROM_ALL ../tests/test_camera_motion.cpp ../tests/test_gpu_runtime_stubs.cpp)
target_include_directories(camera-motion-test PRIVATE shim shadps4 ${SHADER_INCLUDE} third_party/gcn/include)
target_compile_options(camera-motion-test PRIVATE -UNDEBUG -fPIC)
target_link_libraries(camera-motion-test PRIVATE bbgpu)
add_dependencies(camera-motion-test host_shaders)

# bbport: FSR 4 benchmark outside the game (tools/fsr4_bench.cpp): ninja -C out/gpu fsr4-bench.
add_executable(upscaler-support-test EXCLUDE_FROM_ALL ../tests/test_upscaler_support.cpp
    shim/bbport_settings.cpp)
target_include_directories(upscaler-support-test PRIVATE shim)
target_compile_options(upscaler-support-test PRIVATE -UNDEBUG)

add_executable(fsr4-bench EXCLUDE_FROM_ALL tools/fsr4_bench.cpp
    shadps4/video_core/renderer_vulkan/fsr411/fsr411.cpp)
target_include_directories(fsr4-bench PRIVATE shadps4/video_core/renderer_vulkan/fsr411)
target_link_libraries(fsr4-bench PRIVATE ffx-vulkan::fsr4-v07-vulkan ffx-vulkan::fsr4-v07-assets Vulkan::Vulkan)

file(GLOB RUNTIME_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/../src/runtime*.c")
if (WIN32)
    list(APPEND RUNTIME_SOURCES
        "${CMAKE_CURRENT_SOURCE_DIR}/../src/win32_compat.c"
        "${CMAKE_CURRENT_SOURCE_DIR}/../src/win32_memory.c"
    )
elseif (APPLE)
    list(APPEND RUNTIME_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/../src/darwin_compat.c")
endif()
add_executable(bbport
    ../src/probe.c
    ${RUNTIME_SOURCES}
    ../src/vulkan_smoke.c
)
target_include_directories(bbport PRIVATE .. ../src)
target_link_libraries(bbport PRIVATE bbgpu "${CMAKE_CURRENT_SOURCE_DIR}/../out/libatrac9.a" Vulkan::Vulkan)
if (WIN32)
    target_link_libraries(bbport PRIVATE ws2_32 dbghelp ntdll winmm)
    target_link_options(bbport PRIVATE "-Wl,--disable-dynamicbase" "-Wl,--disable-high-entropy-va")
endif()
set_target_properties(bbport PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/../out")

add_executable(bb-gpu-capabilities ../tools/gpu_capabilities.c)
target_include_directories(bb-gpu-capabilities PRIVATE .. ../src)
target_link_libraries(bb-gpu-capabilities PRIVATE Vulkan::Vulkan)
set_target_properties(bb-gpu-capabilities PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/../out")

