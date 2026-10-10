// SPDX-License-Identifier: GPL-2.0-or-later
// Null descriptor stand-ins (MoltenVK has no nullDescriptor): a zeroed buffer, and a view of
// every declared image type that a shader reads as zero through that type (null_resources_sample
// .comp: 1D to cube, multisampled, depth and depth comparison).
#include <array>
#include <cassert>
#include <cstdio>
#include <cstring>
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_null_resources.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "null_resources_sample_comp.h"
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
    // Multisampled slots (texture2d_ms) get multisampled views, color and depth.
    for (const bool depth : {false, true}) {
        assert(null.View(ImageType::Color2DMsaa, depth) != null.View(ImageType::Color2D, depth));
        assert(null.View(ImageType::Color2DMsaaArray, depth) !=
               null.View(ImageType::Color2DArray, depth));
        assert(null.View(ImageType::Color2DMsaa, depth) !=
               null.View(ImageType::Color2DMsaaArray, depth));
    }
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

    // Sampling: each stand-in bound to the binding type a shader declares for it.
    const auto device = instance.GetDevice();
    constexpr u32 NumImages = 11, NumResults = 11;
    std::array<vk::DescriptorSetLayoutBinding, NumImages + 3> bindings{};
    for (u32 i = 0; i < bindings.size(); ++i) {
        bindings[i] = {.binding = i,
                       .descriptorType = i < NumImages       ? vk::DescriptorType::eSampledImage
                                         : i < NumImages + 2 ? vk::DescriptorType::eSampler
                                                             : vk::DescriptorType::eStorageBuffer,
                       .descriptorCount = 1,
                       .stageFlags = vk::ShaderStageFlagBits::eCompute};
    }
    auto set_layout = device.createDescriptorSetLayoutUnique(
        {.flags = vk::DescriptorSetLayoutCreateFlagBits::ePushDescriptorKHR,
         .bindingCount = static_cast<u32>(bindings.size()),
         .pBindings = bindings.data()},
        nullptr, d).value;
    auto layout = device.createPipelineLayoutUnique(
        {.setLayoutCount = 1, .pSetLayouts = &*set_layout}, nullptr, d).value;
    auto module = device.createShaderModuleUnique(
        {.codeSize = sizeof(NULL_RESOURCES_SAMPLE_COMP), .pCode = NULL_RESOURCES_SAMPLE_COMP},
        nullptr, d).value;
    auto pipeline = device.createComputePipelineUnique(
        {}, {.stage = {.stage = vk::ShaderStageFlagBits::eCompute, .module = *module, .pName = "main"},
             .layout = *layout},
        nullptr, d).value;
    auto point = device.createSamplerUnique({}, nullptr, d).value;
    auto compare = device.createSamplerUnique(
        {.compareEnable = vk::True, .compareOp = vk::CompareOp::eLessOrEqual}, nullptr, d).value;

    const VkBufferCreateInfo ri{.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                                .size = NumResults * 16,
                                .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT};
    VkBuffer results{};
    VmaAllocation results_allocation{};
    VmaAllocationInfo results_info{};
    assert(vmaCreateBuffer(instance.GetAllocator(), &ri, &ac, &results, &results_allocation,
                           &results_info) == VK_SUCCESS);
    auto* values = static_cast<float*>(results_info.pMappedData);
    for (u32 i = 0; i < NumResults * 4; ++i) {
        values[i] = 1.f; // a slot the shader does not write stays 1
    }
    vmaFlushAllocation(instance.GetAllocator(), results_allocation, 0, VK_WHOLE_SIZE);

    const std::array<vk::ImageView, NumImages> sampled = {
        null.View(ImageType::Color1D),           null.View(ImageType::Color1DArray),
        null.View(ImageType::Color2D),           null.View(ImageType::Color2DArray),
        null.View(ImageType::Color3D),           null.View(ImageType::Cube),
        null.View(ImageType::Color2DMsaa),       null.View(ImageType::Color2DMsaaArray),
        null.View(ImageType::Color2D, true),     null.View(ImageType::Color2DMsaa, true),
        null.View(ImageType::Color2D, true)};
    std::array<vk::DescriptorImageInfo, NumImages + 2> image_infos{};
    std::array<vk::WriteDescriptorSet, NumImages + 3> writes{};
    for (u32 i = 0; i < NumImages; ++i) {
        image_infos[i] = {.imageView = sampled[i], .imageLayout = vk::ImageLayout::eGeneral};
    }
    image_infos[NumImages] = {.sampler = *point};
    image_infos[NumImages + 1] = {.sampler = *compare};
    const vk::DescriptorBufferInfo results_buffer{results, 0, VK_WHOLE_SIZE};
    for (u32 i = 0; i < writes.size(); ++i) {
        writes[i] = {.dstBinding = i,
                     .descriptorCount = 1,
                     .descriptorType = bindings[i].descriptorType,
                     .pImageInfo = i < NumImages + 2 ? &image_infos[i] : nullptr,
                     .pBufferInfo = i == NumImages + 2 ? &results_buffer : nullptr};
    }
    const auto run = scheduler.CommandBuffer();
    run.bindPipeline(vk::PipelineBindPoint::eCompute, *pipeline, d);
    run.pushDescriptorSetKHR(vk::PipelineBindPoint::eCompute, *layout, 0, writes, d);
    run.dispatch(1, 1, 1, d);
    const vk::MemoryBarrier2 results_to_host{
        .srcStageMask = vk::PipelineStageFlagBits2::eComputeShader,
        .srcAccessMask = vk::AccessFlagBits2::eShaderWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eHost,
        .dstAccessMask = vk::AccessFlagBits2::eHostRead};
    run.pipelineBarrier2(
        vk::DependencyInfo{.memoryBarrierCount = 1, .pMemoryBarriers = &results_to_host}, d);
    scheduler.Finish();
    vmaInvalidateAllocation(instance.GetAllocator(), results_allocation, 0, VK_WHOLE_SIZE);
    for (u32 i = 0; i < NumResults * 4; ++i) {
        // Depth texel reads (results 8 and 9) are (D, 0, 0, 1) by the Vulkan spec.
        const bool depth_alpha = (i / 4 == 8 || i / 4 == 9) && i % 4 == 3;
        const float expected = depth_alpha ? 1.f : 0.f;
        if (values[i] != expected) {
            std::fprintf(stderr, "Null resources: result %u component %u = %g\n", i / 4, i % 4,
                         values[i]);
        }
        assert(values[i] == expected);
    }
    vmaDestroyBuffer(instance.GetAllocator(), results, results_allocation);
    std::puts("Null resources: every stand-in reads as zero through its declared type PASS");
    std::puts("Null resources: color, depth and multisampled views of every image type, zeroed buffer PASS");
    return 0;
}
