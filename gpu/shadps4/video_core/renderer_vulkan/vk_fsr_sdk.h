// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <filesystem>
#include <memory>
#include <string>

#include "video_core/renderer_vulkan/vk_fsr4.h"

namespace Vulkan {

class Instance;
class Scheduler;

class FsrSdk {
public:
    static constexpr const char* LoaderName = "amd_fidelityfx_loader_vk.dll";
    static constexpr const char* UpscalerName = "amd_fidelityfx_upscaler_vk.dll";
    static std::filesystem::path Directory();
    static bool FilesPresent();
    static std::string DirectoryUtf8();

    FsrSdk(const Instance& instance, Scheduler& scheduler);
    ~FsrSdk();

    bool Record(const Fsr4Upscaler::Frame& frame);
    void Prewarm(u32 output_width, u32 output_height, bool auto_exposure);

    [[nodiscard]] bool Preparing() const noexcept;
    [[nodiscard]] const char* Problem() const noexcept;
    [[nodiscard]] bool Fatal() const noexcept;
    [[nodiscard]] std::string Describe() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

} // namespace Vulkan
