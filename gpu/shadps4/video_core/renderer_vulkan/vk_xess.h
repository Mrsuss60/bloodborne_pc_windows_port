// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: Intel XeSS Super Resolution (XeSS-SR) as an alternative to FSR and DLSS, through
// Intel's redistributable libxess.dll (XeSS SDK 3.0.2, tools/fetch_xess.sh) loaded at run time:
// no Intel SDK headers or libraries at build time. Windows only (Intel ships no Linux library).
// On Intel Arc it runs the XMX model, on other GPUs the DP4a one; the SDK decides.
//
// It consumes the FSR 4 frame (vk_fsr4.h), like DLSS: scene color, depth, motion vectors in render
// pixels (previous - current, without jitter) and the jitter offset; the output is written in
// General. XeSS Frame Generation (libxess_fg.dll) and XeLL are D3D12 only in every SDK release so
// far, so they are not used here (docs/xess.md).

#pragma once

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "video_core/renderer_vulkan/vk_fsr4.h"

namespace Vulkan {

class Instance;
class Scheduler;

/// The library and what it asks of the Vulkan instance and device, before either is created.
namespace Xess {
inline constexpr const char* LibraryName = "libxess.dll";
/// BB_XESS_DIR, else engine/xess next to the executable's folder (like fsr_sdk).
std::filesystem::path Directory();
std::string DirectoryUtf8();
/// libxess.dll is there and BB_XESS is not 0.
bool FilesPresent();
/// Loads libxess.dll once (Windows). False when it is missing or lacks the Vulkan entry points.
bool Load();
/// The instance extensions XeSS needs (none on Vulkan 1.1+ drivers so far) and the lowest API
/// version; empty when the library is not loaded or the driver lacks them.
std::vector<const char*> RequiredInstanceExtensions(u32* min_api_version);
/// The device extensions XeSS needs on this device, or false when it cannot run there.
bool RequiredDeviceExtensions(vk::Instance instance, vk::PhysicalDevice physical_device,
                              std::vector<const char*>& extensions);
} // namespace Xess

class XessUpscaler {
public:
    XessUpscaler(const Instance& instance, Scheduler& scheduler);
    ~XessUpscaler();

    /// The library is loaded and a context exists on this device.
    [[nodiscard]] bool Available() const noexcept;

    /// Records XeSS-SR into `frame.cmdbuf` (the render/output ratio picks the XeSS quality mode).
    /// False when XeSS cannot run; Problem() says why.
    bool Record(const Fsr4Upscaler::Frame& frame);

    [[nodiscard]] const char* Problem() const noexcept;
    [[nodiscard]] bool Fatal() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

} // namespace Vulkan
