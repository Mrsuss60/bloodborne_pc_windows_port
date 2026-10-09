// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: stand-ins for null descriptors on drivers without VK_EXT_robustness2's nullDescriptor
// (MoltenVK). Unbound buffers read a zeroed buffer, unbound images a zeroed 1x1 image whose
// view type matches the shader's declaration (Metal checks texture types). Depth slots get a
// depth image: comparison sampling makes them Metal depth textures.

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

    /// Zeroed 1x1 view of the given type in the general layout: RGBA8 (sampled or storage), or
    /// D32 for depth slots (sampled; 1D and 3D types fall back to 2D).
    vk::ImageView View(AmdGpu::ImageType type, bool is_depth = false) const noexcept;

private:
    enum Slot : u32 {
        View1D,
        View1DArray,
        View2D,
        View2DArray,
        View3D,
        ViewCube,
        Depth2D,
        Depth2DArray,
        DepthCube,
        NumViews
    };

    std::unique_ptr<VideoCore::Buffer> buffer;
    VideoCore::UniqueImage image_1d;
    VideoCore::UniqueImage image_2d; ///< 6 layers, cube compatible
    VideoCore::UniqueImage image_3d;
    VideoCore::UniqueImage image_depth; ///< D32, 6 layers, cube compatible
    std::array<vk::UniqueImageView, NumViews> views;
};

} // namespace Vulkan
