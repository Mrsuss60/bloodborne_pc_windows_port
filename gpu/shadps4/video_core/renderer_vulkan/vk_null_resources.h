// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: stand-ins for null descriptors on drivers without VK_EXT_robustness2's nullDescriptor
// (MoltenVK). Unbound buffers read a zeroed buffer, unbound images a zeroed 1x1 image whose
// view type matches the shader's declaration (Metal checks texture types).

#pragma once

#include <array>
#include <memory>

#include "video_core/amdgpu/resource.h"
#include "video_core/renderer_vulkan/vk_common.h"
#include "video_core/texture_cache/image.h"

namespace VideoCore {
class Buffer;
}

namespace Vulkan {

class Instance;
class Scheduler;

class NullResources {
public:
    /// Size of the zeroed buffer: covers uniform and storage ranges declared as whole-size.
    static constexpr vk::DeviceSize BufferSize = 64 * 1024;

    explicit NullResources(const Instance& instance, Scheduler& scheduler);
    ~NullResources();

    NullResources(const NullResources&) = delete;
    NullResources& operator=(const NullResources&) = delete;

    /// Zeroed buffer usable as any buffer descriptor or vertex buffer.
    vk::Buffer Buffer() const noexcept;

    /// Zeroed 1x1 RGBA8 view of the given type, in the general layout (sampled or storage).
    vk::ImageView View(AmdGpu::ImageType type) const noexcept;

private:
    enum Slot : u32 { View1D, View1DArray, View2D, View2DArray, View3D, ViewCube, NumViews };

    std::unique_ptr<VideoCore::Buffer> buffer;
    VideoCore::UniqueImage image_1d;
    VideoCore::UniqueImage image_2d; ///< 6 layers, cube compatible
    VideoCore::UniqueImage image_3d;
    std::array<vk::UniqueImageView, NumViews> views;
};

} // namespace Vulkan
