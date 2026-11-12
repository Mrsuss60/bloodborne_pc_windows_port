// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <string>
#include <thread>
#include <type_traits>
#include <vector>

#include "video_core/renderer_vulkan/vk_fsr_sdk.h"

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

std::string Utf8(const std::filesystem::path& path) {
    const auto u8 = path.u8string();
    return {reinterpret_cast<const char*>(u8.data()), u8.size()};
}

} // namespace

std::filesystem::path FsrSdk::Directory() {
#ifdef _WIN32
    if (const wchar_t* dir = _wgetenv(L"BB_FSR_SDK_DIR"); dir && dir[0]) {
        return dir;
    }
#else
    if (const char* dir = std::getenv("BB_FSR_SDK_DIR"); dir && dir[0]) {
        return dir;
    }
#endif
    return ExecutableDirectory().parent_path() / "fsr_sdk";
}

std::string FsrSdk::DirectoryUtf8() {
    return Utf8(Directory());
}

bool FsrSdk::FilesPresent() {
#ifdef _WIN32
    const char* setting = std::getenv("BB_FSR_SDK");
    if (setting && setting[0] == '0') {
        return false;
    }
    std::error_code error;
    const auto dir = Directory();
    return std::filesystem::is_regular_file(dir / LoaderName, error) &&
           std::filesystem::is_regular_file(dir / UpscalerName, error);
#else
    return false;
#endif
}

#ifdef _WIN32

namespace {

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
struct UpscaleVersion {
    Header header;
    u32 version;
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
struct DeviceContext {
    VkDevice device;
    VkPhysicalDevice physical_device;
    PFN_vkGetDeviceProcAddr get_device_proc_addr;
};
struct BackendVk {
    Header header;
    DeviceContext device;
};
struct QueryVersions {
    Header header;
    u64 create_desc_type;
    void* device;
    u64* output_count;
    u64* version_ids;
    const char** version_names;
};
struct QueryProviderVersion {
    Header header;
    u64 version_id;
    const char* version_name;
};
static_assert(sizeof(Resource) == 48 && sizeof(CreateUpscale) == 48 && sizeof(DispatchUpscale) == 432);
static_assert(offsetof(CreateUpscale, max_upscale_size) == 28 && offsetof(CreateUpscale, message) == 40);
static_assert(offsetof(DispatchUpscale, output) == 312 && offsetof(DispatchUpscale, jitter_offset) == 360 &&
              offsetof(DispatchUpscale, enable_sharpening) == 392 &&
              offsetof(DispatchUpscale, reset) == 408 && offsetof(DispatchUpscale, flags) == 428);
static_assert(sizeof(BackendVk) == 40 && sizeof(QueryVersions) == 56 && sizeof(UpscaleVersion) == 24);

constexpr StructType CreateUpscaleType = 0x00010000u;
constexpr StructType DispatchUpscaleType = 0x00010001u;
constexpr StructType UpscaleVersionType = 0x0001000bu;
constexpr StructType BackendVkType = 0x01000003u;
constexpr StructType QueryVersionsType = 4u;
constexpr StructType QueryProviderVersionType = 6u;
constexpr u32 ApiVersion = (4u << 22) | (1u << 12) | 2u;

constexpr u32 FlagHdr = 1u << 0;
constexpr u32 FlagAutoExposure = 1u << 5;
constexpr u32 FormatRgba16Float = 4, FormatRg16Float = 18, FormatR32Float = 28;
constexpr u32 StateUav = 2, StateComputeRead = 4;
constexpr u32 UsageUav = 1u << 1, UsageDepth = 1u << 2, UsageStencil = 1u << 5;
constexpr u32 ResourceTexture2d = 2;
constexpr u32 ReturnOk = 0;
constexpr size_t FramesInFlight = 3;

using CreateContext = u32 (*)(void** context, Header* desc, const void* callbacks);
using DestroyContext = u32 (*)(void** context, const void* callbacks);
using Query = u32 (*)(void** context, Header* desc);
using Dispatch = u32 (*)(void** context, const Header* desc);

void ProviderMessage(u32 type, const wchar_t* text) {
    std::printf("Upscaler: AMD FSR SDK %s: %ls\n", type == 0 ? "error" : "warning", text ? text : L"");
}

const char* ReturnName(u32 code) {
    static constexpr std::array<const char*, 8> names{
        "ok",          "error",         "unknown descriptor type", "runtime error",
        "no provider", "out of memory", "invalid parameter",       "descriptor type too new"};
    return code < names.size() ? names[code] : "unknown";
}

bool Enabled(const char* name, bool fallback) {
    const char* env = std::getenv(name);
    return env && env[0] ? env[0] != '0' : fallback;
}

} // namespace

struct FsrSdk::Impl {
    const Instance& instance;
    Scheduler& scheduler;
    HMODULE module{};
    CreateContext create{};
    DestroyContext destroy{};
    Query query{};
    Dispatch dispatch{};
    void* context{};
    u32 out_width{}, out_height{};
    bool auto_exposure{true};
    std::deque<u64> ticks;
    std::string problem, described, provider_name;
    bool fatal{};
    bool versions_logged{};
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
        if (module) {
            FreeLibrary(module);
        }
    }

    void Fail(std::string reason, bool permanent) {
        problem = std::move(reason);
        fatal |= permanent;
    }

    BackendVk Backend() const {
        return {{BackendVkType, nullptr},
                {VkDevice(instance.GetDevice()), VkPhysicalDevice(instance.GetPhysicalDevice()),
                 VULKAN_HPP_DEFAULT_DISPATCHER.vkGetDeviceProcAddr}};
    }

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
        described.clear();
    }

    bool Load() {
        if (module) {
            return true;
        }
        if (!FsrSdk::FilesPresent()) {
            Fail("the AMD FSR SDK files are missing from " + FsrSdk::DirectoryUtf8(), true);
            return false;
        }
        const auto path = FsrSdk::Directory() / LoaderName;
        module = LoadLibraryExW(path.c_str(), nullptr,
                                LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
        if (!module) {
            Fail("cannot load " + Utf8(path) + " (error " + std::to_string(GetLastError()) + ")", true);
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
        load(query, "ffxQuery");
        load(dispatch, "ffxDispatch");
        if (!complete) {
            Fail(Utf8(path) + " is not the AMD FSR API loader", true);
            return false;
        }
        return true;
    }

    void LogVersions() {
        if (versions_logged) {
            return;
        }
        versions_logged = true;
        DeviceContext device = Backend().device;
        u64 count = 0;
        QueryVersions versions{{QueryVersionsType, nullptr}, CreateUpscaleType, &device, &count, nullptr, nullptr};
        if (query(nullptr, &versions.header) != ReturnOk || count == 0) {
            std::printf("Upscaler: AMD FSR SDK offers no upscaler version for this GPU\n");
            return;
        }
        std::vector<u64> ids(count);
        std::vector<const char*> names(count, nullptr);
        versions.version_ids = ids.data();
        versions.version_names = names.data();
        if (query(nullptr, &versions.header) != ReturnOk) {
            return;
        }
        std::string list;
        for (u64 i = 0; i < count; ++i) {
            list += (i ? ", " : "") + std::string{names[i] ? names[i] : "?"};
        }
        std::printf("Upscaler: AMD FSR SDK upscaler versions for this GPU: %s\n", list.c_str());
    }

    bool CreateContextNow(u32 ow, u32 oh) {
        if (ow < 64 || oh < 64) {
            Fail("output size " + std::to_string(ow) + "x" + std::to_string(oh) + " is below 64x64", true);
            return false;
        }
        LogVersions();
        CreateUpscale desc{};
        desc.header.type = CreateUpscaleType;
        desc.flags = FlagHdr | (auto_exposure ? FlagAutoExposure : 0u);
        desc.max_render_size = {ow, oh};
        desc.max_upscale_size = {ow, oh};
        desc.message = ProviderMessage;
        UpscaleVersion version{{UpscaleVersionType, nullptr}, ApiVersion};
        BackendVk backend = Backend();
        desc.header.next = &version.header;
        version.header.next = &backend.header;
        const u32 result = create(&context, &desc.header, nullptr);
        if (result != ReturnOk) {
            context = nullptr;
            Fail(std::string{"context creation failed: "} + ReturnName(result) + " (" +
                     std::to_string(result) + ")",
                 true);
            return false;
        }
        out_width = ow;
        out_height = oh;
        QueryProviderVersion name{{QueryProviderVersionType, nullptr}, 0, nullptr};
        std::string shown =
            query(&context, &name.header) == ReturnOk && name.version_name ? name.version_name : "";
        shown.erase(0, shown.find_first_not_of(' '));
        provider_name = shown.empty() ? "AMD FSR SDK" : "FSR " + shown;
        described = provider_name + " (AMD FSR SDK 2.4), output " + std::to_string(out_width) + "x" +
                    std::to_string(out_height);
        return true;
    }

    void StartCreate(u32 ow, u32 oh) {
        if (prep.joinable()) {
            prep.join();
        }
        preparing = true;
        std::printf("Upscaler: AMD FSR SDK preparing its upscaler in the background (%ux%u); FSR 3.1 "
                    "until it is ready\n",
                    ow, oh);
        prep = std::thread([this, ow, oh] {
            const auto start = std::chrono::steady_clock::now();
            created_ok = CreateContextNow(ow, oh);
            if (created_ok) {
                std::printf("Upscaler: %s ready after %.1f s\n", provider_name.c_str(),
                            std::chrono::duration<double>(std::chrono::steady_clock::now() - start)
                                .count());
            }
            preparing = false;
        });
    }

    void Prewarm(u32 ow, u32 oh, bool exposure) {
        if (fatal || preparing || prep.joinable() || context || !ow || !oh || !Load()) {
            return;
        }
        auto_exposure = exposure;
        StartCreate(ow, oh);
    }

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
            return false;
        }
        if (prep.joinable()) {
            prep.join();
            if (!created_ok) {
                return false;
            }
        }
        if (fatal || !Load()) {
            return false;
        }
        const auto exact = [&](const Fsr4Upscaler::Image& image) {
            return image.width == f.render_width && image.height == f.render_height;
        };
        if (!exact(f.color) || !exact(f.depth) || !exact(f.motion)) {
            Fail("the scene targets are larger than the render size (fixed render resolution mode)", true);
            return false;
        }
        if (!context || f.output.width != out_width || f.output.height != out_height ||
            f.auto_exposure != auto_exposure) {
            DestroyContextNow(true);
            auto_exposure = f.auto_exposure;
            if (!Enabled("BB_FSR_SDK_ASYNC", true)) {
                if (!CreateContextNow(f.output.width, f.output.height)) {
                    return false;
                }
            } else {
                StartCreate(f.output.width, f.output.height);
                return false;
            }
        }
        while (ticks.size() >= FramesInFlight) {
            scheduler.Wait(ticks.front());
            ticks.pop_front();
        }
        DispatchUpscale d{};
        d.header.type = DispatchUpscaleType;
        d.command_list = reinterpret_cast<void*>(VkCommandBuffer(f.cmdbuf));
        d.color = ResourceOf(f.color, f.render_width, f.render_height, FormatRgba16Float, 0, StateComputeRead);
        d.depth = ResourceOf(f.depth, f.render_width, f.render_height, FormatR32Float,
                             UsageDepth | UsageStencil, StateComputeRead);
        d.motion_vectors =
            ResourceOf(f.motion, f.render_width, f.render_height, FormatRg16Float, 0, StateComputeRead);
        d.output = ResourceOf(f.output, f.output.width, f.output.height, FormatRgba16Float, UsageUav, StateUav);
        d.jitter_offset = {f.jitter[0], f.jitter[1]};
        const bool flip_y = BbToggle::Disabled2(BbToggle::Fsr4MotionYFlip);
        d.motion_vector_scale = {1.0f, flip_y ? -1.0f : 1.0f};
        d.render_size = {f.render_width, f.render_height};
        d.upscale_size = {f.output.width, f.output.height};
        d.enable_sharpening = f.sharpen && f.sharpness > 0.0f;
        d.sharpness = std::clamp(f.sharpness, 0.0f, 1.0f);
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
            Fail(std::string{"dispatch failed: "} + ReturnName(result) + " (" + std::to_string(result) + ")",
                 true);
            return false;
        }
        ticks.push_back(scheduler.CurrentTick());
        problem.clear();
        return true;
    }
};

FsrSdk::FsrSdk(const Instance& instance, Scheduler& scheduler)
    : impl{std::make_unique<Impl>(instance, scheduler)} {}

FsrSdk::~FsrSdk() = default;

bool FsrSdk::Record(const Fsr4Upscaler::Frame& frame) {
    return impl->Record(frame);
}

void FsrSdk::Prewarm(u32 output_width, u32 output_height, bool auto_exposure) {
    impl->Prewarm(output_width, output_height, auto_exposure);
}

bool FsrSdk::Preparing() const noexcept {
    return impl->preparing;
}

const char* FsrSdk::Problem() const noexcept {
    if (impl->preparing) {
        return "preparing its shaders in the background; FSR 3.1 until they are ready";
    }
    return impl->problem.empty() ? nullptr : impl->problem.c_str();
}

bool FsrSdk::Fatal() const noexcept {
    return !impl->preparing && impl->fatal;
}

std::string FsrSdk::Describe() const {
    return impl->described;
}

#else

struct FsrSdk::Impl {};
FsrSdk::FsrSdk(const Instance&, Scheduler&) {}
FsrSdk::~FsrSdk() = default;
bool FsrSdk::Record(const Fsr4Upscaler::Frame&) {
    return false;
}
void FsrSdk::Prewarm(u32, u32, bool) {}
bool FsrSdk::Preparing() const noexcept {
    return false;
}
const char* FsrSdk::Problem() const noexcept {
    return "the AMD FSR SDK is available on Windows only";
}
bool FsrSdk::Fatal() const noexcept {
    return true;
}
std::string FsrSdk::Describe() const {
    return {};
}

#endif

} // namespace Vulkan
