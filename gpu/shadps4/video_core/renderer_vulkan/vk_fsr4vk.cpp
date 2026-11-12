// SPDX-License-Identifier: GPL-2.0-or-later
// From github.com/0xCydral/bloodborne_pc (GPL-2.0-or-later), the same shadPS4-based lineage as this
// port, v1.7.0-beta.2: its fsr4vk integration, unchanged apart from this note.

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <atomic>
#include <chrono>
#include <deque>
#include <string>
#include <thread>
#include <type_traits>

#include "video_core/renderer_vulkan/vk_fsr4vk.h"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include "bbport_toggles.h"
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

} // namespace

std::filesystem::path Fsr4Vk::Directory() {
#ifdef _WIN32
    // The wide environment: a folder with a non-ASCII name must read the same here and in
    // bb-gpu-capabilities, whatever the process's ANSI code page is.
    if (const wchar_t* dir = _wgetenv(L"BB_FSR4VK_DIR"); dir && dir[0]) {
        return dir;
    }
#else
    if (const char* dir = std::getenv("BB_FSR4VK_DIR"); dir && dir[0]) {
        return dir;
    }
#endif
    return ExecutableDirectory() / "fsr4vk";
}

std::string Fsr4Vk::LibraryPathUtf8() {
    const auto u8 = (Directory() / LibraryName).u8string();
    return {reinterpret_cast<const char*>(u8.data()), u8.size()};
}

bool Fsr4Vk::FilesPresent() {
#ifdef _WIN32
    const char* setting = std::getenv("BB_FSR4VK");
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

// The ABI of the FFX API (AMD FidelityFX SDK api/include/ffx_api.h, ffx_api_types.h and
// upscalers/include/ffx_upscale.h, MIT) as fsr4vk's provider implements it, plus fsr4vk's own
// descriptors (provider/ffx_vk_*.h in its repository). The port's headers are newer than the SDK
// this provider was built with, so the layouts are repeated here and checked below.
using StructType = u64;
struct Header {
    StructType type;
    Header* next;
};
struct Dimensions {
    u32 width, height;
};
struct Float2 {
    float x, y;
};
struct ResourceDescription {
    u32 type, format, width, height, depth, mip_count, flags, usage;
};
struct Resource {
    void* resource;
    ResourceDescription description;
    u32 state;
};
using Message = void (*)(u32 type, const wchar_t* text);
struct CreateUpscale {
    Header header;
    u32 flags;
    Dimensions max_render_size, max_upscale_size;
    Message message;
};
struct DispatchUpscale {
    Header header;
    void* command_list;
    Resource color, depth, motion_vectors, exposure, reactive, transparency_and_composition, output;
    Float2 jitter_offset, motion_vector_scale;
    Dimensions render_size, upscale_size;
    bool enable_sharpening;
    float sharpness, frame_time_delta, pre_exposure;
    bool reset;
    float camera_near, camera_far, camera_fov_vertical, view_space_to_meters;
    u32 flags;
};
struct OverrideVersion {
    Header header;
    u64 version_id;
};
struct BackendVk {
    Header header;
    VkDevice device;
    VkPhysicalDevice physical_device;
    PFN_vkGetDeviceProcAddr get_device_proc_addr;
};
struct ApiVersion {
    Header header;
    u32 api_version;
};
struct QueryDeviceFeatures {
    Header header;
    u32 version;
};
struct CreateDeviceFeatures {
    Header header;
    u32 enabled_flags, api_version, queue_family_index;
};
struct QueryProviderVersion {
    Header header;
    u64 version_id;
    const char* version_name;
};
struct QueryPresetCapabilities {
    Header header;
    u32 forced_preset_mask;
};
struct ConfigurePreset {
    Header header;
    u32 preset;
};
static_assert(sizeof(Resource) == 48 && sizeof(CreateUpscale) == 48 && sizeof(DispatchUpscale) == 432 &&
              sizeof(OverrideVersion) == 24);
static_assert(offsetof(CreateUpscale, max_upscale_size) == 28 && offsetof(CreateUpscale, message) == 40);
static_assert(offsetof(DispatchUpscale, output) == 312 && offsetof(DispatchUpscale, jitter_offset) == 360 &&
              offsetof(DispatchUpscale, enable_sharpening) == 392 &&
              offsetof(DispatchUpscale, reset) == 408 && offsetof(DispatchUpscale, flags) == 428);

constexpr StructType CreateUpscaleType = 0x00010000u;
constexpr StructType DispatchUpscaleType = 0x00010001u;
constexpr StructType OverrideVersionType = 5u;
constexpr StructType QueryProviderVersionType = 6u;
constexpr StructType BackendVkType = 3u;
constexpr StructType ApiVersionType = 0x46535234564b4150ull;
constexpr StructType QueryDeviceFeaturesType = 0x4653523444455651ull;
constexpr StructType CreateDeviceFeaturesType = 0x4653523444455643ull;
constexpr StructType QueryPresetCapabilitiesType = 0x4653523450434150ull;
constexpr StructType ConfigurePresetType = 0x4653523450534554ull;
// (4 << 22) | (1 << 12) | 1 under the provider's 0xF5A5CA1E tag: FSR 4.1.1 INT8.
constexpr u64 Version411 = (0xF5A5CA1Eull << 32) | ((4ull << 22) | (1ull << 12) | 1ull);

constexpr u32 FlagHdr = 1u << 0;
constexpr u32 FlagDepthInverted = 1u << 3;
constexpr u32 FlagAutoExposure = 1u << 5;
// FfxApiSurfaceFormat values. The depth image is D32_SFLOAT_S8_UINT; the provider takes its Vulkan
// format from the DEPTHTARGET and STENCILTARGET usage flags, and R32_FLOAT is the FFX format it expects.
constexpr u32 FormatRgba16Float = 4, FormatRg16Float = 18, FormatR32Float = 28;
constexpr u32 StateUav = 2, StateComputeRead = 4;
constexpr u32 UsageUav = 1u << 1, UsageDepth = 1u << 2, UsageStencil = 1u << 5;
constexpr u32 ResourceTexture2d = 2;
constexpr u32 DeviceRobustBufferAccess = 1;
constexpr u32 ReturnOk = 0;
// The provider keeps eight descriptor, constant and view slots; the ninth outstanding frame
// would overwrite the first one's.
constexpr size_t FramesInFlight = 8;
// 0 native, 1 quality, 2 balanced, 3 performance, 4 drs, 5 ultra performance; the port's
// presets 0..4 skip DRS.
constexpr std::array<u32, 5> ProviderPreset{0, 1, 2, 3, 5};
constexpr std::array<const char*, 5> PresetName{"native", "quality", "balanced", "performance",
                                                "ultra performance"};

using CreateContext = u32 (*)(void** context, Header* desc, const void* callbacks);
using DestroyContext = u32 (*)(void** context, const void* callbacks);
using Configure = u32 (*)(void** context, const Header* desc);
using Query = u32 (*)(void** context, Header* desc);
using Dispatch = u32 (*)(void** context, const Header* desc);

void ProviderMessage(u32 type, const wchar_t* text) {
    std::printf("Upscaler: fsr4vk %s: %ls\n", type == 0 ? "error" : "warning", text ? text : L"");
}

const char* ReturnName(u32 code) {
    static constexpr std::array<const char*, 8> names{
        "ok",        "error",          "unknown descriptor type", "runtime error",
        "no provider", "out of memory", "invalid parameter",       "descriptor type too new"};
    return code < names.size() ? names[code] : "unknown";
}

} // namespace

struct Fsr4Vk::Impl {
    const Instance& instance;
    Scheduler& scheduler;
    HMODULE module{};
    CreateContext create{};
    DestroyContext destroy{};
    Configure configure{};
    Query query{};
    Dispatch dispatch{};
    void* context{};
    u32 out_width{}, out_height{};
    u32 preset_mask{};
    int configured_preset{-1};
    std::deque<u64> ticks; ///< scheduler ticks of the frames in flight (the provider's slot ring)
    std::string problem, described, provider_name;
    bool fatal{};
    // bbport: the context is created on a worker thread (StartCreate): creating it compiles the
    // model's compute pipelines, which held the draw recording thread for 10 s on an RTX 5060 the
    // first time (no frame meanwhile). While `preparing`, only the worker touches the context and
    // the fields above; the render thread reads them again after joining it. FSR 3.1 meanwhile.
    std::thread prep;
    std::atomic<bool> preparing{false};
    bool created_ok{};

    Impl(const Instance& instance_, Scheduler& scheduler_)
        : instance{instance_}, scheduler{scheduler_} {}

    ~Impl() {
        if (prep.joinable()) {
            prep.join();
        }
        DestroyContextNow(false);
    }

    void StartCreate(u32 ow, u32 oh) {
        preparing = true;
        std::printf("Upscaler: FSR 4.1.1 (fsr4vk) compiling its shaders in the background (%ux%u); "
                    "FSR 3.1 until it is ready\n",
                    ow, oh);
        prep = std::thread([this, ow, oh] {
            const auto start = std::chrono::steady_clock::now();
            created_ok = CreateContextNow(ow, oh);
            if (created_ok) {
                std::printf("Upscaler: FSR 4.1.1 (fsr4vk) ready after %.1f s\n",
                            std::chrono::duration<double>(std::chrono::steady_clock::now() - start)
                                .count());
            }
            preparing = false;
        });
    }

    /// Fsr4Upscaler logs the reason and the fallback to FSR 3.1.
    void Fail(std::string reason, bool permanent) {
        problem = std::move(reason);
        fatal |= permanent;
    }

    /// `recording`: called from Record() on the command buffer being recorded, so only the earlier
    /// submissions are waited for.
    void DestroyContextNow(bool recording) {
        if (!context) {
            return;
        }
        if (recording) {
            scheduler.WaitSubmittedWork();
        } else {
            scheduler.Finish();
        }
        destroy(&context, nullptr);
        context = nullptr;
        ticks.clear();
        configured_preset = -1;
        described.clear();
    }

    bool Load() {
        if (module) {
            return true;
        }
        const auto path = Fsr4Vk::Directory() / LibraryName;
        module = LoadLibraryW(path.c_str());
        if (!module) {
            Fail("cannot load " + Fsr4Vk::LibraryPathUtf8() + " (error " + std::to_string(GetLastError()) + ")",
                 true);
            return false;
        }
        bool complete = true;
        const auto load = [&](auto& function, const char* name) {
            function = reinterpret_cast<std::remove_reference_t<decltype(function)>>(
                GetProcAddress(module, name));
            complete = complete && function;
        };
        load(create, "ffxCreateContext");
        load(destroy, "ffxDestroyContext");
        load(configure, "ffxConfigure");
        load(query, "ffxQuery");
        load(dispatch, "ffxDispatch");
        if (!complete) {
            Fail(Fsr4Vk::LibraryPathUtf8() + " is not an FFX API provider", true);
            return false;
        }
        return true;
    }

    bool CreateContextNow(u32 ow, u32 oh) {
        if (ow < 64 || oh < 64 || ow > 3840 || oh > 2160) {
            Fail("output size " + std::to_string(ow) + "x" + std::to_string(oh) +
                     " is outside 64x64 to 3840x2160",
                 true);
            return false;
        }
        CreateUpscale desc{};
        desc.header.type = CreateUpscaleType;
        // bbport: the scene's depth is standard, 0 at the near plane (CameraMotion::Near), as FSR 3,
        // FSR 4 and DLSS are told; the inverted flag (from the fsr4vk integration's source port)
        // made 4.1.1 read near as far: disocclusion and thin moving objects lost their history.
        // BB_FSR4VK_DEPTH_INVERTED=1 keeps the old flag.
        static const bool inverted = [] {
            const char* env = std::getenv("BB_FSR4VK_DEPTH_INVERTED");
            return env && env[0] == '1';
        }();
        desc.flags = FlagHdr | FlagAutoExposure | (inverted ? FlagDepthInverted : 0u);
        desc.max_render_size = {ow, oh};
        desc.max_upscale_size = {ow, oh};
        desc.message = ProviderMessage;
        OverrideVersion version{{OverrideVersionType, nullptr}, Version411};
        BackendVk backend{{BackendVkType, nullptr},
                          VkDevice(instance.GetDevice()),
                          VkPhysicalDevice(instance.GetPhysicalDevice()),
                          VULKAN_HPP_DEFAULT_DISPATCHER.vkGetDeviceProcAddr};
        ApiVersion api{{ApiVersionType, nullptr}, VK_API_VERSION_1_3};
        QueryDeviceFeatures features_query{{QueryDeviceFeaturesType, nullptr}, 0};
        CreateDeviceFeatures features{{CreateDeviceFeaturesType, nullptr},
                                      instance.IsRobustBufferAccessEnabled()
                                          ? DeviceRobustBufferAccess
                                          : 0u,
                                      VK_API_VERSION_1_3, instance.GetGraphicsQueueFamilyIndex()};
        desc.header.next = &version.header;
        version.header.next = &backend.header;
        backend.header.next = &api.header;
        api.header.next = nullptr;
        if (query(nullptr, &features_query.header) == ReturnOk && features_query.version >= 1) {
            api.header.next = &features.header;
        }
        u32 result = create(&context, &desc.header, nullptr);
        if (result != ReturnOk && !(desc.flags & FlagDepthInverted)) {
            // bbport: on AMD the provider runs the driver's own FSR 4 path, which only takes
            // inverted depth ("provider requires inverted depth and low-resolution non-jittered
            // motion vectors"): the flag again, as up to beta 4, where 4.1.1 ran on a 9070 XT.
            context = nullptr;
            desc.flags |= FlagDepthInverted;
            result = create(&context, &desc.header, nullptr);
            if (result == ReturnOk) {
                std::printf("Upscaler: fsr4vk accepted the context with the inverted depth flag "
                            "(this GPU's FSR 4 path requires it)\n");
            }
        }
        if (result != ReturnOk) {
            context = nullptr;
            Fail(std::string{"context creation failed: "} + ReturnName(result) + " (" +
                     std::to_string(result) + "); the provider may lack FSR 4.1.1 or a device feature",
                 true);
            return false;
        }
        out_width = ow;
        out_height = oh;
        QueryPresetCapabilities caps{{QueryPresetCapabilitiesType, nullptr}, 0};
        preset_mask = query(&context, &caps.header) == ReturnOk ? caps.forced_preset_mask : 0;
        QueryProviderVersion name{{QueryProviderVersionType, nullptr}, 0, nullptr};
        // The provider's names start with a space ("FSR" is prefixed by the host): " 4.1.1 VK INT8".
        std::string shown = query(&context, &name.header) == ReturnOk && name.version_name
                                ? name.version_name
                                : "";
        shown.erase(0, shown.find_first_not_of(' '));
        provider_name = shown.empty() ? "fsr4vk" : "FSR " + shown;
        if ((ow | oh) & 1) {
            std::printf("Upscaler: fsr4vk leaves the last row or column of an odd output size "
                        "unwritten (a known 4.1.1 limitation)\n");
        }
        return true;
    }

    bool SelectPreset(int preset) {
        if (preset == configured_preset) {
            return true;
        }
        const u32 id = ProviderPreset[size_t(preset)];
        std::string model = "automatic (by scale)";
        if (preset_mask & (1u << id)) {
            ConfigurePreset configuration{{ConfigurePresetType, nullptr}, id};
            if (const u32 result = configure(&context, &configuration.header); result != ReturnOk) {
                Fail(std::string{"cannot select the "} + PresetName[size_t(preset)] +
                         " model: " + ReturnName(result),
                     true);
                return false;
            }
            model = std::string{PresetName[size_t(preset)]};
        } else {
            std::printf("Upscaler: fsr4vk cannot force the %s model; it picks the model by scale\n",
                        PresetName[size_t(preset)]);
        }
        configured_preset = preset;
        described = provider_name + ", " + model + " model, output " + std::to_string(out_width) +
                    "x" + std::to_string(out_height);
        return true;
    }

    /// The inputs and the output are General; the provider reads its inputs as shader-read-only
    /// images and writes the output as a storage image.
    void Transition(vk::CommandBuffer cmdbuf, const Fsr4Upscaler::Frame& f, bool to_read_only) {
        const std::array<std::pair<const Fsr4Upscaler::Image*, vk::ImageAspectFlags>, 3> inputs{{
            {&f.color, vk::ImageAspectFlagBits::eColor},
            {&f.depth, vk::ImageAspectFlagBits::eDepth | vk::ImageAspectFlagBits::eStencil},
            {&f.motion, vk::ImageAspectFlagBits::eColor},
        }};
        std::array<vk::ImageMemoryBarrier2, 3> barriers;
        for (size_t i = 0; i < inputs.size(); ++i) {
            barriers[i] = vk::ImageMemoryBarrier2{
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
                .image = inputs[i].first->image,
                .subresourceRange = {inputs[i].second, 0, 1, 0, 1},
            };
        }
        // The output: earlier users are done before the provider writes it, and its writes are
        // visible to everything after.
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
                                 .imageMemoryBarrierCount = u32(barriers.size()),
                                 .pImageMemoryBarriers = barriers.data()});
    }

    static Resource ResourceOf(const Fsr4Upscaler::Image& image, u32 width, u32 height, u32 format,
                             u32 usage, u32 state) {
        Resource r{};
        r.resource = reinterpret_cast<void*>(VkImage(image.image));
        r.description = {ResourceTexture2d, format, width, height, 1, 1, 0, usage};
        r.state = state;
        return r;
    }

    bool Record(const Fsr4Upscaler::Frame& f) {
        if (preparing) {
            return false; // Problem(): still compiling; FSR 3.1 this frame
        }
        if (prep.joinable()) {
            prep.join();
            if (!created_ok) {
                return false; // CreateContextNow's Fail() said why
            }
        }
        if (fatal) {
            return false;
        }
        if (!Load()) {
            return false;
        }
        // The provider reads the render-size part from the origin of each input.
        const auto exact = [&](const Fsr4Upscaler::Image& image) {
            return image.width == f.render_width && image.height == f.render_height;
        };
        if (!exact(f.color) || !exact(f.depth) || !exact(f.motion)) {
            Fail("the scene targets are larger than the render size (fixed render resolution mode)",
                 true);
            return false;
        }
        if (!context || f.output.width != out_width || f.output.height != out_height) {
            DestroyContextNow(true);
            static const bool async = [] {
                const char* env = std::getenv("BB_FSR4VK_ASYNC");
                return !(env && env[0] == '0');
            }();
            if (!async) {
                if (!CreateContextNow(f.output.width, f.output.height)) {
                    return false;
                }
            } else {
                StartCreate(f.output.width, f.output.height);
                return false;
            }
        }
        if (!SelectPreset(std::clamp(f.preset, 0, 4))) {
            return false;
        }
        // The provider's slot ring holds FramesInFlight frames: the oldest must be done.
        while (ticks.size() >= FramesInFlight) {
            scheduler.Wait(ticks.front());
            ticks.pop_front();
        }
        DispatchUpscale d{};
        d.header.type = DispatchUpscaleType;
        d.command_list = reinterpret_cast<void*>(VkCommandBuffer(f.cmdbuf));
        // The depth image is D32_SFLOAT_S8_UINT, sampled through its depth aspect (see FormatR32Float).
        d.color = ResourceOf(f.color, f.render_width, f.render_height, FormatRgba16Float, 0,
                           StateComputeRead);
        d.depth = ResourceOf(f.depth, f.render_width, f.render_height, FormatR32Float, UsageDepth | UsageStencil,
                           StateComputeRead);
        d.motion_vectors = ResourceOf(f.motion, f.render_width, f.render_height, FormatRg16Float, 0,
                                    StateComputeRead);
        d.output = ResourceOf(f.output, f.output.width, f.output.height, FormatRgba16Float, UsageUav,
                            StateUav);
        d.jitter_offset = {f.jitter[0], f.jitter[1]};
        // Vectors are in render pixels; the provider divides by the render size.
        // Fsr4MotionYFlip set in the second toggle word means FSR 4 gets y negated (bbport_toggles.h),
        // as in vk_fsr4.cpp. Clear: y stays.
        const bool flip_y = BbToggle::Disabled2(BbToggle::Fsr4MotionYFlip);
        d.motion_vector_scale = {1.0f, flip_y ? -1.0f : 1.0f};
        d.render_size = {f.render_width, f.render_height};
        d.upscale_size = {f.output.width, f.output.height};
        d.frame_time_delta = f.frame_ms;
        d.pre_exposure = 1.0f;
        d.reset = f.reset;
        d.camera_near = f.near_plane;
        d.camera_far = f.far_plane;
        d.camera_fov_vertical = f.vertical_fov;
        d.view_space_to_meters = 1.0f;
        Transition(f.cmdbuf, f, true);
        const u32 result = dispatch(&context, &d.header);
        Transition(f.cmdbuf, f, false);
        if (result != ReturnOk) {
            Fail(std::string{"dispatch failed: "} + ReturnName(result) + " (" +
                     std::to_string(result) + ")",
                 true);
            return false;
        }
        ticks.push_back(scheduler.CurrentTick());
        problem.clear();
        return true;
    }
};

Fsr4Vk::Fsr4Vk(const Instance& instance, Scheduler& scheduler)
    : impl{std::make_unique<Impl>(instance, scheduler)} {}

Fsr4Vk::~Fsr4Vk() = default;

bool Fsr4Vk::Record(const Fsr4Upscaler::Frame& frame) {
    return impl->Record(frame);
}

const char* Fsr4Vk::Problem() const noexcept {
    if (impl->preparing) {
        return "compiling its shaders in the background; FSR 3.1 until they are ready";
    }
    return impl->problem.empty() ? nullptr : impl->problem.c_str();
}

bool Fsr4Vk::Fatal() const noexcept {
    return !impl->preparing && impl->fatal;
}

std::string Fsr4Vk::Describe() const {
    return impl->described;
}

#else

struct Fsr4Vk::Impl {};
Fsr4Vk::Fsr4Vk(const Instance&, Scheduler&) {}
Fsr4Vk::~Fsr4Vk() = default;
bool Fsr4Vk::Record(const Fsr4Upscaler::Frame&) {
    return false;
}
const char* Fsr4Vk::Problem() const noexcept {
    return "fsr4vk is built for Windows only";
}
bool Fsr4Vk::Fatal() const noexcept {
    return true;
}
std::string Fsr4Vk::Describe() const {
    return {};
}

#endif

} // namespace Vulkan
