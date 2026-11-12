// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include "video_core/renderer_vulkan/vk_xess.h"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#endif

namespace Vulkan {

namespace {

std::filesystem::path ExecutableDirectory() {
#ifdef _WIN32
    std::wstring path(MAX_PATH, L'\0');
    const DWORD length = GetModuleFileNameW(nullptr, path.data(), DWORD(path.size()));
    path.resize(length);
    return std::filesystem::path{path}.parent_path();
#else
    return {};
#endif
}

std::string Utf8(const std::filesystem::path& path) {
    const auto u8 = path.u8string();
    return {reinterpret_cast<const char*>(u8.data()), u8.size()};
}

} // namespace

std::filesystem::path Xess::Directory() {
#ifdef _WIN32
    if (const wchar_t* dir = _wgetenv(L"BB_XESS_DIR"); dir && dir[0]) {
        return dir;
    }
#else
    if (const char* dir = std::getenv("BB_XESS_DIR"); dir && dir[0]) {
        return dir;
    }
#endif
    return ExecutableDirectory().parent_path() / "xess";
}

std::string Xess::DirectoryUtf8() {
    return Utf8(Directory());
}

bool Xess::FilesPresent() {
#ifdef _WIN32
    const char* setting = std::getenv("BB_XESS");
    if (setting && setting[0] == '0') {
        return false;
    }
    std::error_code error;
    return std::filesystem::is_regular_file(Directory() / LibraryName, error);
#else
    return false;
#endif
}

#ifdef _WIN32

namespace {

// The XeSS-SR interface as libxess.dll exports it (XeSS SDK 3.0.2, xess.h and xess_vk.h; the SDK
// packs its structures to 8 bytes, which is the natural layout of these on x86-64). Calls are
// cdecl, the platform calling convention on x86-64.
using Result = int;
constexpr Result WarningOldDriver = 2;
constexpr bool Failed(Result r) {
    return r < 0;
}

enum Quality : int {
    UltraPerformance = 100,
    Performance = 101,
    Balanced = 102,
    QualityMode = 103,
    UltraQuality = 104,
    UltraQualityPlus = 105,
    NativeAa = 106,
};

enum InitFlags : u32 {
    FlagResponsivePixelMask = 1u << 3,
    FlagLdrInputColor = 1u << 6,
    FlagAutoExposure = 1u << 8,
};

enum LogLevel : int { LogDebug = 0, LogInfo = 1, LogWarning = 2, LogError = 3 };

struct Context;
using Handle = Context*;

#pragma pack(push, 8)
struct Version {
    u16 major, minor, patch, reserved;
};
struct Size2d {
    u32 x, y;
};
struct ImageViewInfo {
    VkImageView image_view;
    VkImage image;
    VkImageSubresourceRange subresource_range;
    VkFormat format;
    unsigned int width;
    unsigned int height;
};
struct ExecuteParams {
    ImageViewInfo color, velocity, depth, exposure_scale, responsive_mask, output;
    float jitter_x, jitter_y;
    float exposure_scale_value;
    u32 reset_history;
    u32 input_width, input_height;
    Size2d input_color_base, input_motion_base, input_depth_base, input_responsive_base;
    Size2d reserved0;
    Size2d output_color_base;
};
struct InitParams {
    Size2d output_resolution;
    int quality;
    u32 init_flags;
    u32 creation_node_mask;
    u32 visible_node_mask;
    VkDeviceMemory temp_buffer_heap;
    u64 buffer_heap_offset;
    VkDeviceMemory temp_texture_heap;
    u64 texture_heap_offset;
    VkPipelineCache pipeline_cache;
};
#pragma pack(pop)
static_assert(sizeof(ImageViewInfo) == 48);
static_assert(sizeof(InitParams) == 64);

using LogCallback = void (*)(const char* message, int level);
using PFN_GetVersion = Result (*)(Version*);
using PFN_GetRequiredInstanceExtensions = Result (*)(u32* count, const char* const** names,
                                                     u32* min_api_version);
using PFN_GetRequiredDeviceExtensions = Result (*)(VkInstance, VkPhysicalDevice, u32* count,
                                                   const char* const** names);
using PFN_CreateContext = Result (*)(VkInstance, VkPhysicalDevice, VkDevice, Handle*);
using PFN_Init = Result (*)(Handle, const InitParams*);
using PFN_Execute = Result (*)(Handle, VkCommandBuffer, const ExecuteParams*);
using PFN_DestroyContext = Result (*)(Handle);
using PFN_SetVelocityScale = Result (*)(Handle, float x, float y);
using PFN_SetLoggingCallback = Result (*)(Handle, int level, LogCallback);
using PFN_IsOptimalDriver = Result (*)(Handle);

struct Library {
    HMODULE module = nullptr;
    bool tried = false;
    Version version{};
    PFN_GetVersion get_version = nullptr;
    PFN_GetRequiredInstanceExtensions instance_extensions = nullptr;
    PFN_GetRequiredDeviceExtensions device_extensions = nullptr;
    PFN_CreateContext create_context = nullptr;
    PFN_Init init = nullptr;
    PFN_Execute execute = nullptr;
    PFN_DestroyContext destroy_context = nullptr;
    PFN_SetVelocityScale set_velocity_scale = nullptr;
    PFN_SetLoggingCallback set_logging_callback = nullptr;
    PFN_IsOptimalDriver is_optimal_driver = nullptr;
};

Library& Lib() {
    static Library library;
    return library;
}

const char* ResultText(Result r) {
    switch (r) {
    case 1:
        return "nonexisting folder";
    case WarningOldDriver:
        return "old driver";
    case -1:
        return "unsupported device";
    case -2:
        return "unsupported driver";
    case -3:
        return "uninitialized";
    case -4:
        return "invalid argument";
    case -5:
        return "device out of memory";
    case -6:
        return "device error";
    case -7:
        return "not implemented";
    case -8:
        return "invalid context";
    case -9:
        return "operation in progress";
    case -10:
        return "unsupported";
    case -11:
        return "cannot load library";
    case -12:
        return "wrong call order";
    default:
        return "unknown error";
    }
}

std::string Describe(const char* what, Result r) {
    char text[256];
    std::snprintf(text, sizeof(text), "%s (%d, %s)", what, r, ResultText(r));
    return text;
}

void Log(const char* message, int level) {
    static constexpr const char* names[] = {"debug", "info", "warning", "error"};
    std::printf("XeSS: SDK %s: %s%s", names[std::clamp(level, 0, 3)], message,
                *message && message[std::strlen(message) - 1] == '\n' ? "" : "\n");
}

/// The XeSS quality mode whose ratio is nearest the engine's render/output ratio, so the render
/// size the engine's preset (or the Custom render_scale) picked stays inside the mode's range:
/// XeSS 1.3+ ratios are 1.3 (Ultra Quality Plus), 1.5, 1.7, 2.0, 2.3 and 3.0 (Ultra Performance).
int QualityFor(u32 render_width, u32 output_width) {
    const float ratio = float(output_width) / float(std::max(render_width, 1u));
    return ratio <= 1.05f  ? NativeAa
           : ratio < 1.4f  ? UltraQualityPlus
           : ratio < 1.6f  ? UltraQuality
           : ratio < 1.85f ? QualityMode
           : ratio < 2.15f ? Balanced
           : ratio < 2.65f ? Performance
                           : UltraPerformance;
}

bool IsFloatFormat(vk::Format f) {
    switch (f) {
    case vk::Format::eR16G16B16A16Sfloat:
    case vk::Format::eR32G32B32A32Sfloat:
    case vk::Format::eB10G11R11UfloatPack32:
    case vk::Format::eR16G16B16Sfloat:
        return true;
    default:
        return false;
    }
}

ImageViewInfo View(const Fsr4Upscaler::Image& image, vk::ImageAspectFlags aspect) {
    return {
        .image_view = static_cast<VkImageView>(image.view),
        .image = static_cast<VkImage>(image.image),
        .subresource_range = {static_cast<VkImageAspectFlags>(aspect), 0, 1, 0, 1},
        .format = static_cast<VkFormat>(image.format),
        .width = image.width,
        .height = image.height,
    };
}

} // namespace

bool Xess::Load() {
    auto& lib = Lib();
    if (lib.tried) {
        return lib.module != nullptr;
    }
    lib.tried = true;
    if (!FilesPresent()) {
        return false;
    }
    const auto path = Directory() / LibraryName;
    // The SDK's own folder first, so a libxess.dll elsewhere on the search path is never taken.
    lib.module = LoadLibraryExW(path.c_str(), nullptr,
                                LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    if (!lib.module) {
        // libxess.dll needs the Microsoft Visual C++ runtime (msvcp140, vcruntime140, _1).
        std::printf("XeSS: cannot load %s (error %lu; the Microsoft Visual C++ 2015-2022 "
                    "runtime is needed)\n",
                    Utf8(path).c_str(), GetLastError());
        return false;
    }
    bool complete = true;
    const auto load = [&](auto& function, const char* name) {
        function = reinterpret_cast<std::remove_reference_t<decltype(function)>>(
            GetProcAddress(lib.module, name));
        complete = complete && function;
    };
    load(lib.get_version, "xessGetVersion");
    load(lib.instance_extensions, "xessVKGetRequiredInstanceExtensions");
    load(lib.device_extensions, "xessVKGetRequiredDeviceExtensions");
    load(lib.create_context, "xessVKCreateContext");
    load(lib.init, "xessVKInit");
    load(lib.execute, "xessVKExecute");
    load(lib.destroy_context, "xessDestroyContext");
    load(lib.set_velocity_scale, "xessSetVelocityScale");
    load(lib.set_logging_callback, "xessSetLoggingCallback");
    load(lib.is_optimal_driver, "xessIsOptimalDriver");
    if (!complete) {
        std::printf("XeSS: %s lacks the XeSS-SR Vulkan entry points\n", Utf8(path).c_str());
        FreeLibrary(lib.module);
        lib.module = nullptr;
        return false;
    }
    lib.get_version(&lib.version);
    std::printf("XeSS: %s %u.%u.%u loaded\n", LibraryName, lib.version.major, lib.version.minor,
                lib.version.patch);
    return true;
}

std::vector<const char*> Xess::RequiredInstanceExtensions(u32* min_api_version) {
    *min_api_version = 0;
    if (!Load()) {
        return {};
    }
    u32 count = 0;
    const char* const* names = nullptr;
    const Result r = Lib().instance_extensions(&count, &names, min_api_version);
    if (Failed(r)) {
        std::printf("%s\n", Describe("XeSS: instance requirements not met", r).c_str());
        *min_api_version = 0;
        return {};
    }
    return {names, names + count};
}

bool Xess::RequiredDeviceExtensions(vk::Instance instance, vk::PhysicalDevice physical_device,
                                    std::vector<const char*>& extensions) {
    extensions.clear();
    if (!Load()) {
        return false;
    }
    u32 count = 0;
    const char* const* names = nullptr;
    const Result r = Lib().device_extensions(VkInstance(instance), VkPhysicalDevice(physical_device),
                                             &count, &names);
    if (Failed(r)) {
        std::printf("%s\n", Describe("XeSS: this device cannot run XeSS-SR", r).c_str());
        return false;
    }
    extensions.assign(names, names + count);
    return true;
}

struct XessUpscaler::Impl {
    const Instance& instance;
    Scheduler& scheduler;
    bool available = false;
    bool fatal = false;
    std::string problem;
    Handle context = nullptr;
    bool initialized = false;
    struct Key {
        u32 out_width, out_height;
        int quality;
        bool hdr;
        bool responsive;
        bool operator==(const Key&) const = default;
    } key{};

    Impl(const Instance& instance_, Scheduler& scheduler_)
        : instance{instance_}, scheduler{scheduler_} {}

    void Fail(std::string why, bool permanent) {
        if (problem != why) {
            std::printf("XeSS: %s\n", why.c_str());
        }
        problem = std::move(why);
        fatal = fatal || permanent;
    }

    void Initialize() {
        if (!instance.IsXessCapable()) {
            Fail("the Vulkan device lacks what XeSS-SR needs (libxess.dll missing from " +
                     Xess::DirectoryUtf8() + ", or the GPU or driver is unsupported)",
                 true);
            return;
        }
        auto& lib = Lib();
        const Result r = lib.create_context(VkInstance(instance.GetInstance()),
                                            VkPhysicalDevice(instance.GetPhysicalDevice()),
                                            VkDevice(instance.GetDevice()), &context);
        if (Failed(r) || !context) {
            context = nullptr;
            Fail(Describe("context creation failed", r), true);
            return;
        }
        const char* log = std::getenv("BB_XESS_LOG");
        lib.set_logging_callback(context, log && log[0] == '1' ? LogDebug : LogError, &Log);
        if (lib.is_optimal_driver(context) == WarningOldDriver) {
            std::printf("XeSS: the GPU driver is older than XeSS recommends: update it for the best "
                        "quality and speed\n");
        }
        available = true;
        std::printf("XeSS: Super Resolution %u.%u.%u available\n", lib.version.major,
                    lib.version.minor, lib.version.patch);
    }

    void Transition(vk::CommandBuffer cmdbuf, const Fsr4Upscaler::Frame& f, bool to_read_only) {
        // XeSS reads its inputs in ShaderReadOnlyOptimal from the compute stage and does no
        // synchronization of its own; the engine keeps them in General. A layout change covers
        // every aspect of the depth image (the scene's is D32S8 where the GPU has it).
        const bool stencil = f.depth.format == vk::Format::eD32SfloatS8Uint ||
                             f.depth.format == vk::Format::eD24UnormS8Uint ||
                             f.depth.format == vk::Format::eD16UnormS8Uint;
        const std::array<std::pair<const Fsr4Upscaler::Image*, vk::ImageAspectFlags>, 4> inputs{{
            {&f.color, vk::ImageAspectFlagBits::eColor},
            {&f.depth, stencil ? vk::ImageAspectFlagBits::eDepth | vk::ImageAspectFlagBits::eStencil
                               : vk::ImageAspectFlags{vk::ImageAspectFlagBits::eDepth}},
            {&f.motion, vk::ImageAspectFlagBits::eColor},
            {&f.reactive, vk::ImageAspectFlagBits::eColor},
        }};
        std::array<vk::ImageMemoryBarrier2, 4> barriers;
        u32 count = 0;
        for (const auto& [image, aspect] : inputs) {
            if (!image->image) {
                continue;
            }
            barriers[count++] = vk::ImageMemoryBarrier2{
                .srcStageMask = to_read_only ? vk::PipelineStageFlagBits2::eAllCommands
                                             : vk::PipelineStageFlagBits2::eComputeShader,
                .srcAccessMask = to_read_only ? vk::AccessFlagBits2::eMemoryWrite
                                              : vk::AccessFlagBits2::eShaderRead,
                .dstStageMask = to_read_only ? vk::PipelineStageFlagBits2::eComputeShader
                                             : vk::PipelineStageFlagBits2::eAllCommands,
                .dstAccessMask = to_read_only ? vk::AccessFlagBits2::eShaderRead
                                              : vk::AccessFlagBits2::eMemoryRead |
                                                    vk::AccessFlagBits2::eMemoryWrite,
                .oldLayout = to_read_only ? vk::ImageLayout::eGeneral
                                          : vk::ImageLayout::eShaderReadOnlyOptimal,
                .newLayout = to_read_only ? vk::ImageLayout::eShaderReadOnlyOptimal
                                          : vk::ImageLayout::eGeneral,
                .image = image->image,
                .subresourceRange = {aspect, 0, 1, 0, 1},
            };
        }
        // The output stays in General; its earlier reads and the later ones see XeSS's writes.
        const vk::MemoryBarrier2 memory{
            .srcStageMask = to_read_only ? vk::PipelineStageFlagBits2::eAllCommands
                                         : vk::PipelineStageFlagBits2::eComputeShader,
            .srcAccessMask = to_read_only ? vk::AccessFlagBits2::eMemoryRead |
                                                vk::AccessFlagBits2::eMemoryWrite
                                          : vk::AccessFlagBits2::eShaderStorageWrite,
            .dstStageMask = to_read_only ? vk::PipelineStageFlagBits2::eComputeShader
                                         : vk::PipelineStageFlagBits2::eAllCommands,
            .dstAccessMask = to_read_only ? vk::AccessFlagBits2::eShaderStorageRead |
                                                vk::AccessFlagBits2::eShaderStorageWrite
                                          : vk::AccessFlagBits2::eMemoryRead |
                                                vk::AccessFlagBits2::eMemoryWrite,
        };
        cmdbuf.pipelineBarrier2({.memoryBarrierCount = 1,
                                 .pMemoryBarriers = &memory,
                                 .imageMemoryBarrierCount = count,
                                 .pImageMemoryBarriers = barriers.data()});
    }

    bool Record(const Fsr4Upscaler::Frame& f) {
        if (!available) {
            return false;
        }
        auto& lib = Lib();
        const Key wanted{f.output.width, f.output.height, QualityFor(f.render_width, f.output.width),
                         IsFloatFormat(f.color.format), f.reactive.view != nullptr};
        if (!initialized || !(wanted == key)) {
            // XeSS may be initialized again on the same context once its earlier command buffers
            // are done; the open one (f.cmdbuf) has no XeSS work yet.
            if (initialized) {
                scheduler.WaitSubmittedWork();
            }
            u32 flags = 0; // motion vectors at render size, in pixels, without jitter; standard depth
            flags |= wanted.hdr ? FlagAutoExposure : FlagLdrInputColor;
            flags |= wanted.responsive ? FlagResponsivePixelMask : 0u;
            const InitParams params{
                .output_resolution = {wanted.out_width, wanted.out_height},
                .quality = wanted.quality,
                .init_flags = flags,
                .creation_node_mask = 1,
                .visible_node_mask = 1,
            };
            const Result r = lib.init(context, &params);
            if (Failed(r)) {
                initialized = false;
                Fail(Describe("initialization failed", r), r == -1 || r == -2 || r == -10);
                return false;
            }
            initialized = true;
            key = wanted;
            // The engine's motion vectors are previous - current in render pixels: XeSS's own
            // convention (current to previous, pixels), so no scale.
            lib.set_velocity_scale(context, 1.0f, 1.0f);
            std::printf("XeSS: %ux%u -> %ux%u, quality mode %d, %s input%s\n", f.render_width,
                        f.render_height, key.out_width, key.out_height, key.quality,
                        key.hdr ? "HDR" : "LDR", key.responsive ? ", responsive mask" : "");
        }
        ExecuteParams params{
            .color = View(f.color, vk::ImageAspectFlagBits::eColor),
            .velocity = View(f.motion, vk::ImageAspectFlagBits::eColor),
            .depth = View(f.depth, vk::ImageAspectFlagBits::eDepth),
            .output = View(f.output, vk::ImageAspectFlagBits::eColor),
            .jitter_x = f.jitter[0],
            .jitter_y = f.jitter[1],
            .exposure_scale_value = 1.0f,
            .reset_history = f.reset ? 1u : 0u,
            .input_width = f.render_width,
            .input_height = f.render_height,
        };
        if (key.responsive) {
            params.responsive_mask = View(f.reactive, vk::ImageAspectFlagBits::eColor);
        }
        Transition(f.cmdbuf, f, true);
        const Result r = lib.execute(context, VkCommandBuffer(f.cmdbuf), &params);
        Transition(f.cmdbuf, f, false);
        if (Failed(r)) {
            Fail(Describe("execution failed", r), false);
            return false;
        }
        problem.clear();
        return true;
    }

    ~Impl() {
        if (!context) {
            return;
        }
        scheduler.Finish();
        Lib().destroy_context(context);
    }
};

#else

struct XessUpscaler::Impl {
    Impl(const Instance&, Scheduler&) {}
    void Initialize() {}
    bool Record(const Fsr4Upscaler::Frame&) {
        return false;
    }
    bool available = false;
    bool fatal = true;
    std::string problem = "XeSS is available on Windows only";
};

bool Xess::Load() {
    return false;
}

std::vector<const char*> Xess::RequiredInstanceExtensions(u32* min_api_version) {
    *min_api_version = 0;
    return {};
}

bool Xess::RequiredDeviceExtensions(vk::Instance, vk::PhysicalDevice,
                                    std::vector<const char*>& extensions) {
    extensions.clear();
    return false;
}

#endif

XessUpscaler::XessUpscaler(const Instance& instance, Scheduler& scheduler)
    : impl{std::make_unique<Impl>(instance, scheduler)} {
    impl->Initialize();
}

XessUpscaler::~XessUpscaler() = default;

bool XessUpscaler::Available() const noexcept {
    return impl->available;
}

bool XessUpscaler::Record(const Fsr4Upscaler::Frame& frame) {
    return impl->Record(frame);
}

const char* XessUpscaler::Problem() const noexcept {
    return impl->problem.empty() ? nullptr : impl->problem.c_str();
}

bool XessUpscaler::Fatal() const noexcept {
    return impl->fatal;
}

} // namespace Vulkan
