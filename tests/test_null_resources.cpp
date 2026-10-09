// SPDX-License-Identifier: GPL-2.0-or-later
// Null descriptor stand-ins (MoltenVK has no nullDescriptor): a view of every declared image
// type and a zeroed buffer, readable once created.
#include <array>
#include <cassert>
#include <cstdio>
#include <cstring>
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_null_resources.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include <vk_mem_alloc.h>

#ifdef __APPLE__
// VK_NO_PROTOTYPES (vk_common.h): the loader's entry point, linked from libvulkan.
extern "C" VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetInstanceProcAddr(VkInstance instance,
                                                                         const char* name);
#endif

int main() {
    Vulkan::Instance instance(0, false);
    vk::detail::DispatchLoaderDynamic d;
#ifdef __APPLE__
    d.init(&::vkGetInstanceProcAddr);
#else
    static vk::detail::DynamicLoader loader;
    d.init(loader.getProcAddress<PFN_vkGetInstanceProcAddr>("vkGetInstanceProcAddr"));
#endif
    d.init(instance.GetInstance());
    d.init(instance.GetDevice());
    Vulkan::Scheduler scheduler(instance);
    Vulkan::NullResources null(instance, scheduler);

    using AmdGpu::ImageType;
    constexpr std::array types = {ImageType::Color1D,      ImageType::Color1DArray,
                                  ImageType::Color2D,      ImageType::Color2DArray,
                                  ImageType::Color3D,      ImageType::Cube,
                                  ImageType::Color2DMsaa,  ImageType::Color2DMsaaArray};
    for (const auto type : types) {
        assert(null.View(type));
    }
    assert(null.View(ImageType::Color2D) != null.View(ImageType::Color2DArray));
    assert(null.View(ImageType::Cube) != null.View(ImageType::Color2DArray));
    // Depth slots (comparison sampling: Metal depth textures) get depth views.
    for (const auto type : types) {
        assert(null.View(type, true));
    }
    assert(null.View(ImageType::Color2D, true) != null.View(ImageType::Color2D));
    assert(null.View(ImageType::Cube, true) != null.View(ImageType::Color2DArray, true));
    assert(null.Buffer());

    // Readback of the whole buffer, plus a canary past its end.
    const vk::DeviceSize size = Vulkan::NullResources::BufferSize + 16;
    const VkBufferCreateInfo bi{.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                                .size = size,
                                .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT};
    const VmaAllocationCreateInfo ac{.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT |
                                              VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT,
                                     .usage = VMA_MEMORY_USAGE_AUTO};
    VkBuffer staging{};
    VmaAllocation allocation{};
    VmaAllocationInfo ai{};
    assert(vmaCreateBuffer(instance.GetAllocator(), &bi, &ac, &staging, &allocation, &ai) ==
           VK_SUCCESS);
    std::memset(ai.pMappedData, 0xab, size);
    vmaFlushAllocation(instance.GetAllocator(), allocation, 0, VK_WHOLE_SIZE);

    scheduler.EndRendering();
    const auto cmd = scheduler.CommandBuffer();
    const vk::BufferCopy whole{.srcOffset = 0, .dstOffset = 0,
                               .size = Vulkan::NullResources::BufferSize};
    cmd.copyBuffer(null.Buffer(), staging, whole, d);
    const vk::MemoryBarrier2 to_host{.srcStageMask = vk::PipelineStageFlagBits2::eTransfer,
                                     .srcAccessMask = vk::AccessFlagBits2::eTransferWrite,
                                     .dstStageMask = vk::PipelineStageFlagBits2::eHost,
                                     .dstAccessMask = vk::AccessFlagBits2::eHostRead};
    cmd.pipelineBarrier2(vk::DependencyInfo{.memoryBarrierCount = 1, .pMemoryBarriers = &to_host},
                         d);
    scheduler.Finish();
    vmaInvalidateAllocation(instance.GetAllocator(), allocation, 0, VK_WHOLE_SIZE);
    const auto* bytes = static_cast<const unsigned char*>(ai.pMappedData);
    for (vk::DeviceSize i = 0; i < Vulkan::NullResources::BufferSize; ++i) {
        assert(bytes[i] == 0);
    }
    assert(bytes[Vulkan::NullResources::BufferSize] == 0xab); // the copy stayed in range
    vmaDestroyBuffer(instance.GetAllocator(), staging, allocation);
    std::puts("Null resources: color and depth views of every image type, zeroed buffer PASS");
    return 0;
}
