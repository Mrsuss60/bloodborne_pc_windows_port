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
constexpr vk::Format DepthFormat = vk::Format::eD32Sfloat;
constexpr vk::ImageUsageFlags Usage = vk::ImageUsageFlagBits::eSampled |
                                      vk::ImageUsageFlagBits::eStorage |
                                      vk::ImageUsageFlagBits::eTransferDst;
constexpr vk::ImageUsageFlags DepthUsage =
    vk::ImageUsageFlagBits::eSampled | vk::ImageUsageFlagBits::eTransferDst;
// Vulkan guarantees 4 samples for sampled images. Attachment usage: Metal clears multisampled
// textures through a render pass.
constexpr vk::SampleCountFlagBits MsaaSamples = vk::SampleCountFlagBits::e4;
constexpr vk::ImageUsageFlags MsaaUsage = vk::ImageUsageFlagBits::eSampled |
                                          vk::ImageUsageFlagBits::eTransferDst |
                                          vk::ImageUsageFlagBits::eColorAttachment;
constexpr vk::ImageUsageFlags DepthMsaaUsage = DepthUsage |
                                               vk::ImageUsageFlagBits::eDepthStencilAttachment;
} // namespace

NullResources::NullResources(const Instance& instance, Scheduler& scheduler) {
    const auto device = instance.GetDevice();
    buffer = std::make_unique<VideoCore::Buffer>(instance, 0, BufferSize,
                                                 VideoCore::MemoryType::DeviceLocal);

    const auto make_image = [&](VideoCore::UniqueImage& image, vk::ImageType type, u32 layers,
                                vk::ImageCreateFlags flags, vk::Format format = Format,
                                vk::ImageUsageFlags usage = Usage,
                                vk::SampleCountFlagBits samples = vk::SampleCountFlagBits::e1) {
        image = VideoCore::UniqueImage(device, instance.GetAllocator());
        image.Create(vk::ImageCreateInfo{
            .flags = flags,
            .imageType = type,
            .format = format,
            .extent = {1, 1, 1},
            .mipLevels = 1,
            .arrayLayers = layers,
            .samples = samples,
            .tiling = vk::ImageTiling::eOptimal,
            .usage = usage,
            .initialLayout = vk::ImageLayout::eUndefined,
        });
    };
    make_image(image_1d, vk::ImageType::e1D, 1, {});
    make_image(image_2d, vk::ImageType::e2D, 6, vk::ImageCreateFlagBits::eCubeCompatible);
    make_image(image_3d, vk::ImageType::e3D, 1, {});
    make_image(image_depth, vk::ImageType::e2D, 6, vk::ImageCreateFlagBits::eCubeCompatible,
               DepthFormat, DepthUsage);
    make_image(image_msaa, vk::ImageType::e2D, 1, {}, Format, MsaaUsage, MsaaSamples);
    make_image(image_depth_msaa, vk::ImageType::e2D, 1, {}, DepthFormat, DepthMsaaUsage,
               MsaaSamples);

    const auto make_view = [&](const VideoCore::UniqueImage& image, vk::ImageViewType type,
                               u32 layers, vk::Format format = Format,
                               vk::ImageAspectFlags aspect = vk::ImageAspectFlagBits::eColor) {
        return Check(device.createImageViewUnique({
            .image = vk::Image(image),
            .viewType = type,
            .format = format,
            .subresourceRange = {aspect, 0, 1, 0, layers},
        }));
    };
    views[View1D] = make_view(image_1d, vk::ImageViewType::e1D, 1);
    views[View1DArray] = make_view(image_1d, vk::ImageViewType::e1DArray, 1);
    views[View2D] = make_view(image_2d, vk::ImageViewType::e2D, 1);
    views[View2DArray] = make_view(image_2d, vk::ImageViewType::e2DArray, 6);
    views[View3D] = make_view(image_3d, vk::ImageViewType::e3D, 1);
    views[ViewCube] = make_view(image_2d, vk::ImageViewType::eCube, 6);
    constexpr auto depth = vk::ImageAspectFlagBits::eDepth;
    views[Depth2D] = make_view(image_depth, vk::ImageViewType::e2D, 1, DepthFormat, depth);
    views[Depth2DArray] = make_view(image_depth, vk::ImageViewType::e2DArray, 6, DepthFormat, depth);
    views[DepthCube] = make_view(image_depth, vk::ImageViewType::eCube, 6, DepthFormat, depth);
    views[ViewMsaa] = make_view(image_msaa, vk::ImageViewType::e2D, 1);
    views[ViewMsaaArray] = make_view(image_msaa, vk::ImageViewType::e2DArray, 1);
    views[DepthMsaa] = make_view(image_depth_msaa, vk::ImageViewType::e2D, 1, DepthFormat, depth);
    views[DepthMsaaArray] =
        make_view(image_depth_msaa, vk::ImageViewType::e2DArray, 1, DepthFormat, depth);

    // Zero everything once and leave the images in the general layout for good.
    scheduler.EndRendering();
    const auto cmdbuf = scheduler.CommandBuffer();
    cmdbuf.fillBuffer(buffer->Handle(), 0, VK_WHOLE_SIZE, 0);
    const std::array<vk::Image, 4> images = {vk::Image(image_1d), vk::Image(image_2d),
                                             vk::Image(image_3d), vk::Image(image_msaa)};
    const vk::ImageSubresourceRange all{vk::ImageAspectFlagBits::eColor, 0, 1, 0,
                                        VK_REMAINING_ARRAY_LAYERS};
    const vk::ImageSubresourceRange all_depth{vk::ImageAspectFlagBits::eDepth, 0, 1, 0,
                                              VK_REMAINING_ARRAY_LAYERS};
    const std::array<vk::Image, 2> depth_images = {vk::Image(image_depth),
                                                   vk::Image(image_depth_msaa)};
    std::array<vk::ImageMemoryBarrier2, images.size() + depth_images.size()> to_general{};
    for (size_t i = 0; i < depth_images.size(); ++i) {
        to_general[images.size() + i] = vk::ImageMemoryBarrier2{
            .srcStageMask = vk::PipelineStageFlagBits2::eNone,
            .dstStageMask = vk::PipelineStageFlagBits2::eTransfer,
            .dstAccessMask = vk::AccessFlagBits2::eTransferWrite,
            .oldLayout = vk::ImageLayout::eUndefined,
            .newLayout = vk::ImageLayout::eGeneral,
            .image = depth_images[i],
            .subresourceRange = all_depth,
        };
    }
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
    for (const auto image : depth_images) {
        cmdbuf.clearDepthStencilImage(image, vk::ImageLayout::eGeneral,
                                      vk::ClearDepthStencilValue{0.f, 0}, all_depth);
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

vk::ImageView NullResources::View(AmdGpu::ImageType type, bool is_depth) const noexcept {
    if (is_depth) {
        switch (type) {
        case AmdGpu::ImageType::Color2DMsaa:
            return *views[DepthMsaa];
        case AmdGpu::ImageType::Color2DMsaaArray:
            return *views[DepthMsaaArray];
        case AmdGpu::ImageType::Color1DArray:
        case AmdGpu::ImageType::Color2DArray:
            return *views[Depth2DArray];
        case AmdGpu::ImageType::Cube:
            return *views[DepthCube];
        default:
            return *views[Depth2D];
        }
    }
    switch (type) {
    case AmdGpu::ImageType::Color1D:
        return *views[View1D];
    case AmdGpu::ImageType::Color1DArray:
        return *views[View1DArray];
    case AmdGpu::ImageType::Color2DArray:
        return *views[View2DArray];
    case AmdGpu::ImageType::Color2DMsaa:
        return *views[ViewMsaa];
    case AmdGpu::ImageType::Color2DMsaaArray:
        return *views[ViewMsaaArray];
    case AmdGpu::ImageType::Color3D:
        return *views[View3D];
    case AmdGpu::ImageType::Cube:
        return *views[ViewCube];
    default: // Color2D
        return *views[View2D];
    }
}

} // namespace Vulkan
