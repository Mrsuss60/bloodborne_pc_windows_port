// SPDX-License-Identifier: GPL-2.0-or-later
#include "video_core/renderer_vulkan/vk_null_resources.h"

#include "common/logging/log.h"
#include "video_core/buffer_cache/buffer.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_platform.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"

namespace Vulkan {

namespace {
constexpr vk::Format Format = vk::Format::eR8G8B8A8Unorm;
constexpr vk::ImageUsageFlags Usage = vk::ImageUsageFlagBits::eSampled |
                                      vk::ImageUsageFlagBits::eStorage |
                                      vk::ImageUsageFlagBits::eTransferDst;
} // namespace

NullResources::NullResources(const Instance& instance, Scheduler& scheduler) {
    const auto device = instance.GetDevice();
    buffer = std::make_unique<VideoCore::Buffer>(instance, 0, BufferSize,
                                                 VideoCore::MemoryType::DeviceLocal);

    const auto make_image = [&](VideoCore::UniqueImage& image, vk::ImageType type, u32 layers,
                                vk::ImageCreateFlags flags) {
        image = VideoCore::UniqueImage(device, instance.GetAllocator());
        image.Create(vk::ImageCreateInfo{
            .flags = flags,
            .imageType = type,
            .format = Format,
            .extent = {1, 1, 1},
            .mipLevels = 1,
            .arrayLayers = layers,
            .samples = vk::SampleCountFlagBits::e1,
            .tiling = vk::ImageTiling::eOptimal,
            .usage = Usage,
            .initialLayout = vk::ImageLayout::eUndefined,
        });
    };
    make_image(image_1d, vk::ImageType::e1D, 1, {});
    make_image(image_2d, vk::ImageType::e2D, 6, vk::ImageCreateFlagBits::eCubeCompatible);
    make_image(image_3d, vk::ImageType::e3D, 1, {});

    const auto make_view = [&](const VideoCore::UniqueImage& image, vk::ImageViewType type,
                               u32 layers) {
        return Check(device.createImageViewUnique({
            .image = vk::Image(image),
            .viewType = type,
            .format = Format,
            .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, layers},
        }));
    };
    views[View1D] = make_view(image_1d, vk::ImageViewType::e1D, 1);
    views[View1DArray] = make_view(image_1d, vk::ImageViewType::e1DArray, 1);
    views[View2D] = make_view(image_2d, vk::ImageViewType::e2D, 1);
    views[View2DArray] = make_view(image_2d, vk::ImageViewType::e2DArray, 6);
    views[View3D] = make_view(image_3d, vk::ImageViewType::e3D, 1);
    views[ViewCube] = make_view(image_2d, vk::ImageViewType::eCube, 6);

    // Zero everything once and leave the images in the general layout for good.
    scheduler.EndRendering();
    const auto cmdbuf = scheduler.CommandBuffer();
    cmdbuf.fillBuffer(buffer->Handle(), 0, VK_WHOLE_SIZE, 0);
    const std::array<vk::Image, 3> images = {vk::Image(image_1d), vk::Image(image_2d),
                                             vk::Image(image_3d)};
    const vk::ImageSubresourceRange all{vk::ImageAspectFlagBits::eColor, 0, 1, 0,
                                        VK_REMAINING_ARRAY_LAYERS};
    std::array<vk::ImageMemoryBarrier2, 3> to_general{};
    for (size_t i = 0; i < images.size(); ++i) {
        to_general[i] = vk::ImageMemoryBarrier2{
            .srcStageMask = vk::PipelineStageFlagBits2::eNone,
            .dstStageMask = vk::PipelineStageFlagBits2::eTransfer,
            .dstAccessMask = vk::AccessFlagBits2::eTransferWrite,
            .oldLayout = vk::ImageLayout::eUndefined,
            .newLayout = vk::ImageLayout::eGeneral,
            .image = images[i],
            .subresourceRange = all,
        };
    }
    cmdbuf.pipelineBarrier2(vk::DependencyInfo{
        .imageMemoryBarrierCount = static_cast<u32>(to_general.size()),
        .pImageMemoryBarriers = to_general.data(),
    });
    for (const auto image : images) {
        cmdbuf.clearColorImage(image, vk::ImageLayout::eGeneral, vk::ClearColorValue{}, all);
    }
    const vk::MemoryBarrier2 visible{
        .srcStageMask = vk::PipelineStageFlagBits2::eTransfer,
        .srcAccessMask = vk::AccessFlagBits2::eTransferWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eAllCommands,
        .dstAccessMask = vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eMemoryWrite,
    };
    cmdbuf.pipelineBarrier2(vk::DependencyInfo{
        .memoryBarrierCount = 1,
        .pMemoryBarriers = &visible,
    });
    LOG_INFO(Render_Vulkan, "No nullDescriptor: unbound resources read zeroed stand-ins");
}

NullResources::~NullResources() = default;

vk::Buffer NullResources::Buffer() const noexcept {
    return buffer->Handle();
}

vk::ImageView NullResources::View(AmdGpu::ImageType type) const noexcept {
    switch (type) {
    case AmdGpu::ImageType::Color1D:
        return *views[View1D];
    case AmdGpu::ImageType::Color1DArray:
        return *views[View1DArray];
    case AmdGpu::ImageType::Color2DArray:
    case AmdGpu::ImageType::Color2DMsaaArray:
        return *views[View2DArray];
    case AmdGpu::ImageType::Color3D:
        return *views[View3D];
    case AmdGpu::ImageType::Cube:
        return *views[ViewCube];
    default: // Color2D; MSAA reads of an unbound image use a single-sample view
        return *views[View2D];
    }
}

} // namespace Vulkan
