// SPDX-License-Identifier: GPL-2.0-or-later
#include "video_core/renderer_vulkan/vk_button_prompts.h"

#include <algorithm>
#include <cstdio>

#include "bbport_button_prompts.h"
#include "video_core/buffer_cache/buffer_cache.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_runtime.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/texture_cache/image_view.h"

namespace Vulkan {
namespace {

bool IsSrgb(vk::Format format) {
    switch (format) {
    case vk::Format::eBc1RgbaSrgbBlock:
    case vk::Format::eBc1RgbSrgbBlock:
    case vk::Format::eBc2SrgbBlock:
    case vk::Format::eBc3SrgbBlock:
    case vk::Format::eBc7SrgbBlock:
    case vk::Format::eR8G8B8A8Srgb:
    case vk::Format::eB8G8R8A8Srgb:
    case vk::Format::eA8B8G8R8SrgbPack32:
        return true;
    default:
        return false;
    }
}

} // namespace

ButtonPrompts::ButtonPrompts(const Instance& instance_, Scheduler& scheduler_,
                             VideoCore::BufferCache& buffer_cache_)
    : instance{instance_}, scheduler{scheduler_}, buffer_cache{buffer_cache_},
      image{instance_.GetDevice(), instance_.GetAllocator()} {
    BbPrompts::Start();
}

ButtonPrompts::~ButtonPrompts() = default;

void ButtonPrompts::Update() {
    BbPrompts::Set set;
    if (!BbPrompts::Update(generation, table_generation, set)) {
        return;
    }
    generation = set.generation;
    table_generation = BbPrompts::TableGeneration();
    drawn = set.drawn;
    active.store(std::find(drawn.begin(), drawn.end(), true) != drawn.end(),
                 std::memory_order_release);
    if (!active.load(std::memory_order_relaxed)) {
        return; // the game's icons: nothing to upload
    }
    if (!image) {
        layers = set.count;
        image.Create(vk::ImageCreateInfo{
            .flags = vk::ImageCreateFlagBits::eMutableFormat,
            .imageType = vk::ImageType::e2D,
            .format = vk::Format::eR8G8B8A8Unorm,
            .extent = {BbPrompts::IconSize, BbPrompts::IconSize, 1},
            .mipLevels = 1,
            .arrayLayers = layers,
            .samples = vk::SampleCountFlagBits::e1,
            .tiling = vk::ImageTiling::eOptimal,
            .usage = vk::ImageUsageFlagBits::eSampled | vk::ImageUsageFlagBits::eTransferDst,
            .sharingMode = vk::SharingMode::eExclusive,
            .initialLayout = vk::ImageLayout::eUndefined,
        });
    }
    if (set.count != layers) {
        active.store(false, std::memory_order_release);
        return;
    }
    // The icons at the game's size for the atlases' cells: means of premultiplied texels.
    constexpr u32 Small = BbPrompts::GameIconSize, Factor = BbPrompts::IconSize / Small;
    small.assign(std::size_t(layers) * Small * Small, 0);
    for (u32 layer = 0; layer < layers; ++layer) {
        const u32* icon = set.pixels.data() + std::size_t(layer) * BbPrompts::IconSize * BbPrompts::IconSize;
        for (u32 y = 0; y < Small; ++y) {
            for (u32 x = 0; x < Small; ++x) {
                u32 sum[4]{};
                for (u32 dy = 0; dy < Factor; ++dy) {
                    for (u32 dx = 0; dx < Factor; ++dx) {
                        const u32 texel = icon[(y * Factor + dy) * BbPrompts::IconSize + x * Factor + dx];
                        for (u32 c = 0; c < 4; ++c) {
                            sum[c] += (texel >> (8 * c)) & 0xff;
                        }
                    }
                }
                u32 out = 0;
                for (u32 c = 0; c < 4; ++c) {
                    out |= (sum[c] + Factor * Factor / 2) / (Factor * Factor) << (8 * c);
                }
                small[(std::size_t(layer) * Small + y) * Small + x] = out;
            }
        }
    }
    const u64 bytes = u64(set.pixels.size()) * sizeof(u32);
    auto& stream = buffer_cache.GetStreamBuffer();
    const u64 offset = stream.Copy(set.pixels.data(), bytes, 16);
    // In stream order, ahead of the draws that sample it; the draws before it are done reading it
    // (the barrier's source stages).
    scheduler.EndRendering();
    scheduler.Record([image = vk::Image{image.image}, buffer = stream.Handle(), offset,
                      layers = layers](vk::CommandBuffer cmdbuf) {
        const vk::ImageSubresourceRange range{vk::ImageAspectFlagBits::eColor, 0, 1, 0, layers};
        const vk::ImageMemoryBarrier2 to_copy{
            .srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
            .srcAccessMask = vk::AccessFlagBits2::eShaderRead,
            .dstStageMask = vk::PipelineStageFlagBits2::eCopy,
            .dstAccessMask = vk::AccessFlagBits2::eTransferWrite,
            .oldLayout = vk::ImageLayout::eUndefined,
            .newLayout = vk::ImageLayout::eTransferDstOptimal,
            .image = image,
            .subresourceRange = range,
        };
        cmdbuf.pipelineBarrier2(vk::DependencyInfo{.imageMemoryBarrierCount = 1,
                                                   .pImageMemoryBarriers = &to_copy});
        const vk::BufferImageCopy copy{
            .bufferOffset = offset,
            .bufferRowLength = 0,
            .bufferImageHeight = 0,
            .imageSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, layers},
            .imageOffset = {0, 0, 0},
            .imageExtent = {BbPrompts::IconSize, BbPrompts::IconSize, 1},
        };
        cmdbuf.copyBufferToImage(buffer, image, vk::ImageLayout::eTransferDstOptimal, copy);
        const vk::ImageMemoryBarrier2 to_sample{
            .srcStageMask = vk::PipelineStageFlagBits2::eCopy,
            .srcAccessMask = vk::AccessFlagBits2::eTransferWrite,
            .dstStageMask = vk::PipelineStageFlagBits2::eAllCommands,
            .dstAccessMask = vk::AccessFlagBits2::eShaderRead,
            .oldLayout = vk::ImageLayout::eTransferDstOptimal,
            .newLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
            .image = image,
            .subresourceRange = range,
        };
        cmdbuf.pipelineBarrier2(vk::DependencyInfo{.imageMemoryBarrierCount = 1,
                                                   .pImageMemoryBarriers = &to_sample});
    });
}

vk::ImageView ButtonPrompts::View(u32 icon, const VideoCore::ImageViewInfo& info) {
    if (!Active() || icon == 0 || icon > drawn.size() || !drawn[icon - 1]) {
        return {};
    }
    const bool array = info.type == AmdGpu::ImageType::Color2DArray;
    if (!array && info.type != AmdGpu::ImageType::Color2D) {
        return {};
    }
    const bool srgb = IsSrgb(info.format);
    const u32 mapping = u32(info.mapping.r) | u32(info.mapping.g) << 8 | u32(info.mapping.b) << 16 |
                        u32(info.mapping.a) << 24;
    auto& view = views[{icon, srgb, array, mapping}];
    if (!view) {
        auto result = instance.GetDevice().createImageViewUnique(vk::ImageViewCreateInfo{
            .image = image.image,
            .viewType = array ? vk::ImageViewType::e2DArray : vk::ImageViewType::e2D,
            .format = srgb ? vk::Format::eR8G8B8A8Srgb : vk::Format::eR8G8B8A8Unorm,
            .components = info.mapping,
            .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, icon - 1, 1},
        });
        if (result.result != vk::Result::eSuccess) {
            std::printf("Button prompts: no image view (%s)\n", vk::to_string(result.result).c_str());
            return {};
        }
        view = std::move(result.value);
    }
    return *view;
}

vk::ImageView ButtonPrompts::AtlasView(Runtime& runtime, VideoCore::Image& image,
                                       const VideoCore::ImageViewInfo& info) {
    if (!Active() || image.prompt_atlas == 0 || info.type != AmdGpu::ImageType::Color2D) {
        return {};
    }
    const auto cells = BbPrompts::AtlasCells(image.prompt_atlas);
    const auto is_drawn = [&](const BbPrompts::AtlasCell& cell) {
        return cell.icon <= drawn.size() && drawn[cell.icon - 1] && cell.icon <= layers;
    };
    if (std::none_of(cells.begin(), cells.end(), is_drawn)) {
        return {};
    }
    if (!image.backing) {
        return {};
    }
    // A blit decodes the game's (BC7: a blit source on every device with BC textures).
    const vk::Format source_format = image.backing->image.image_ci.format;
    if (source_format != vk::Format::eBc7UnormBlock && source_format != vk::Format::eBc7SrgbBlock) {
        return {};
    }
    const u32 width = image.info.size.width, height = image.info.size.height;
    Atlas& atlas = atlases[image.image_uid];
    if (!atlas.image) {
        atlas.image = VideoCore::UniqueImage{instance.GetDevice(), instance.GetAllocator()};
        atlas.image.Create(vk::ImageCreateInfo{
            .flags = vk::ImageCreateFlagBits::eMutableFormat,
            .imageType = vk::ImageType::e2D,
            .format = vk::Format::eR8G8B8A8Unorm,
            .extent = {width, height, 1},
            .mipLevels = 1,
            .arrayLayers = 1,
            .samples = vk::SampleCountFlagBits::e1,
            .tiling = vk::ImageTiling::eOptimal,
            .usage = vk::ImageUsageFlagBits::eSampled | vk::ImageUsageFlagBits::eTransferDst,
            .sharingMode = vk::SharingMode::eExclusive,
            .initialLayout = vk::ImageLayout::eUndefined,
        });
    }
    if (atlas.upload != image.prompt_upload || atlas.generation != generation) {
        atlas.upload = image.prompt_upload;
        atlas.generation = generation;
        constexpr u32 Small = BbPrompts::GameIconSize;
        std::vector<vk::BufferImageCopy> copies;
        std::vector<u32> pixels;
        for (const auto& cell : cells) {
            if (!is_drawn(cell) || cell.x + Small > width || cell.y + Small > height) {
                continue;
            }
            copies.push_back(vk::BufferImageCopy{
                .bufferOffset = pixels.size() * sizeof(u32),
                .imageSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1},
                .imageOffset = {s32(cell.x), s32(cell.y), 0},
                .imageExtent = {Small, Small, 1},
            });
            const u32* icon = small.data() + std::size_t(cell.icon - 1) * Small * Small;
            pixels.insert(pixels.end(), icon, icon + Small * Small);
        }
        auto& stream = buffer_cache.GetStreamBuffer();
        const u64 offset = stream.Copy(pixels.data(), pixels.size() * sizeof(u32), 16);
        for (auto& copy : copies) {
            copy.bufferOffset += offset;
        }
        scheduler.EndRendering();
        runtime.Transit(&image, vk::ImageLayout::eTransferSrcOptimal, vk::PipelineStageFlagBits2::eBlit,
                        vk::AccessFlagBits2::eTransferRead);
        runtime.FlushBarriers();
        scheduler.Record([source = image.GetImage(), target = vk::Image{atlas.image.image},
                          buffer = stream.Handle(), copies = std::move(copies), width,
                          height](vk::CommandBuffer cmdbuf) {
            const vk::ImageSubresourceRange range{vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1};
            const vk::ImageMemoryBarrier2 to_blit{
                .srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
                .srcAccessMask = vk::AccessFlagBits2::eShaderRead,
                .dstStageMask = vk::PipelineStageFlagBits2::eBlit,
                .dstAccessMask = vk::AccessFlagBits2::eTransferWrite,
                .oldLayout = vk::ImageLayout::eUndefined,
                .newLayout = vk::ImageLayout::eTransferDstOptimal,
                .image = target,
                .subresourceRange = range,
            };
            cmdbuf.pipelineBarrier2(vk::DependencyInfo{.imageMemoryBarrierCount = 1,
                                                       .pImageMemoryBarriers = &to_blit});
            const vk::ImageBlit blit{
                .srcSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1},
                .srcOffsets = std::array{vk::Offset3D{0, 0, 0}, vk::Offset3D{s32(width), s32(height), 1}},
                .dstSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1},
                .dstOffsets = std::array{vk::Offset3D{0, 0, 0}, vk::Offset3D{s32(width), s32(height), 1}},
            };
            cmdbuf.blitImage(source, vk::ImageLayout::eTransferSrcOptimal, target,
                             vk::ImageLayout::eTransferDstOptimal, blit, vk::Filter::eNearest);
            const vk::ImageMemoryBarrier2 to_copy{
                .srcStageMask = vk::PipelineStageFlagBits2::eBlit,
                .srcAccessMask = vk::AccessFlagBits2::eTransferWrite,
                .dstStageMask = vk::PipelineStageFlagBits2::eCopy,
                .dstAccessMask = vk::AccessFlagBits2::eTransferWrite,
                .oldLayout = vk::ImageLayout::eTransferDstOptimal,
                .newLayout = vk::ImageLayout::eTransferDstOptimal,
                .image = target,
                .subresourceRange = range,
            };
            cmdbuf.pipelineBarrier2(vk::DependencyInfo{.imageMemoryBarrierCount = 1,
                                                       .pImageMemoryBarriers = &to_copy});
            cmdbuf.copyBufferToImage(buffer, target, vk::ImageLayout::eTransferDstOptimal, copies);
            const vk::ImageMemoryBarrier2 to_sample{
                .srcStageMask = vk::PipelineStageFlagBits2::eCopy,
                .srcAccessMask = vk::AccessFlagBits2::eTransferWrite,
                .dstStageMask = vk::PipelineStageFlagBits2::eAllCommands,
                .dstAccessMask = vk::AccessFlagBits2::eShaderRead,
                .oldLayout = vk::ImageLayout::eTransferDstOptimal,
                .newLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
                .image = target,
                .subresourceRange = range,
            };
            cmdbuf.pipelineBarrier2(vk::DependencyInfo{.imageMemoryBarrierCount = 1,
                                                       .pImageMemoryBarriers = &to_sample});
        });
    }
    const bool srgb = IsSrgb(info.format);
    const u32 mapping = u32(info.mapping.r) | u32(info.mapping.g) << 8 | u32(info.mapping.b) << 16 |
                        u32(info.mapping.a) << 24;
    auto& view = atlas.views[{srgb, false, mapping}];
    if (!view) {
        auto result = instance.GetDevice().createImageViewUnique(vk::ImageViewCreateInfo{
            .image = atlas.image.image,
            .viewType = vk::ImageViewType::e2D,
            .format = srgb ? vk::Format::eR8G8B8A8Srgb : vk::Format::eR8G8B8A8Unorm,
            .components = info.mapping,
            .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
        });
        if (result.result != vk::Result::eSuccess) {
            std::printf("Button prompts: no atlas view (%s)\n", vk::to_string(result.result).c_str());
            return {};
        }
        view = std::move(result.value);
    }
    return *view;
}

} // namespace Vulkan
