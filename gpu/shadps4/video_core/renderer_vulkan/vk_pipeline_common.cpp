// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <span>
#include <mutex>
#include <vector>

#include <boost/container/small_vector.hpp>
#include <boost/container/static_vector.hpp>
#include <tsl/robin_map.h>
#include <xxhash.h>

#include "shader_recompiler/resource.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_pipeline_cache.h"
#include "video_core/renderer_vulkan/vk_pipeline_common.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"

namespace Vulkan {

namespace {

struct CanonicalLayoutKey {
    VkDevice device{};
    bool is_compute{};
    bool push_descriptors{};
    std::vector<vk::DescriptorSetLayoutBinding> bindings;

    bool operator==(const CanonicalLayoutKey& other) const noexcept {
        return device == other.device && is_compute == other.is_compute &&
               push_descriptors == other.push_descriptors && bindings == other.bindings;
    }
};

struct CanonicalLayoutKeyHash {
    size_t operator()(const CanonicalLayoutKey& k) const noexcept {
        u64 h = u64(reinterpret_cast<uintptr_t>(k.device));
        h = h * 0x9E3779B97F4A7C15ull ^ (u64(k.is_compute) | (u64(k.push_descriptors) << 1));
        if (!k.bindings.empty()) {
            static_assert(std::has_unique_object_representations_v<vk::DescriptorSetLayoutBinding>);
            h ^= XXH3_64bits(k.bindings.data(),
                             k.bindings.size() * sizeof(vk::DescriptorSetLayoutBinding));
        }
        return size_t(h);
    }
};

struct CanonicalLayoutEntry {
    vk::UniqueDescriptorSetLayout desc_layout;
    vk::UniquePipelineLayout pipeline_layout;
};

std::mutex g_layout_cache_mutex;
tsl::robin_map<CanonicalLayoutKey, CanonicalLayoutEntry, CanonicalLayoutKeyHash> g_layout_cache;

} // namespace

void ClearPipelineLayoutCache() {
    std::scoped_lock lk{g_layout_cache_mutex};
    g_layout_cache.clear();
}

void Pipeline::InitCanonicalLayouts(std::span<const vk::DescriptorSetLayoutBinding> bindings,
                                    bool push_descriptors) {
    uses_push_descriptors = push_descriptors;
    const vk::Device device = instance.GetDevice();
    CanonicalLayoutKey cache_key{
        .device = static_cast<VkDevice>(device),
        .is_compute = is_compute,
        .push_descriptors = push_descriptors,
        .bindings = std::vector<vk::DescriptorSetLayoutBinding>(bindings.begin(), bindings.end()),
    };

    {
        std::scoped_lock lk{g_layout_cache_mutex};
        if (const auto it = g_layout_cache.find(cache_key); it != g_layout_cache.end()) {
            desc_layout = *it->second.desc_layout;
            pipeline_layout = *it->second.pipeline_layout;
            return;
        }
    }

    const auto flags = push_descriptors
                           ? vk::DescriptorSetLayoutCreateFlagBits::ePushDescriptorKHR
                           : vk::DescriptorSetLayoutCreateFlagBits{};
    const vk::DescriptorSetLayoutCreateInfo desc_layout_ci = {
        .flags = flags,
        .bindingCount = static_cast<u32>(bindings.size()),
        .pBindings = bindings.data(),
    };
    auto [ds_res, ds_layout] = device.createDescriptorSetLayoutUnique(desc_layout_ci);
    if (ds_res != vk::Result::eSuccess) {
        throw std::runtime_error(
            fmt::format("Failed to create {} descriptor set layout: {}",
                        is_compute ? "compute" : "graphics", vk::to_string(ds_res)));
    }

    const vk::PushConstantRange push_constants = {
        .stageFlags = is_compute ? vk::ShaderStageFlagBits::eCompute : AllGraphicsStageBits,
        .offset = 0,
        .size = sizeof(Shader::PushData),
    };
    const vk::DescriptorSetLayout raw_ds_layout = *ds_layout;
    const vk::PipelineLayoutCreateInfo layout_info = {
        .setLayoutCount = 1U,
        .pSetLayouts = &raw_ds_layout,
        .pushConstantRangeCount = 1U,
        .pPushConstantRanges = &push_constants,
    };
    auto [pl_res, pl_layout] = device.createPipelineLayoutUnique(layout_info);
    if (pl_res != vk::Result::eSuccess) {
        throw std::runtime_error(
            fmt::format("Failed to create {} pipeline layout: {}",
                        is_compute ? "compute" : "graphics", vk::to_string(pl_res)));
    }

    std::scoped_lock lk{g_layout_cache_mutex};
    const auto [it, _] = g_layout_cache.try_emplace(
        std::move(cache_key), CanonicalLayoutEntry{std::move(ds_layout), std::move(pl_layout)});
    desc_layout = *it->second.desc_layout;
    pipeline_layout = *it->second.pipeline_layout;
}

Pipeline::Pipeline(const Instance& instance_, Scheduler& scheduler_, DescriptorHeap& desc_heap_,
                   const Shader::Profile& profile_, vk::PipelineCache pipeline_cache,
                   bool is_compute_ /*= false*/)
    : instance{instance_}, scheduler{scheduler_}, desc_heap{desc_heap_}, profile{profile_},
      is_compute{is_compute_} {}

Pipeline::~Pipeline() = default;


void Pipeline::BindResources(DescriptorWrites& set_writes,
                             const Shader::PushData& push_data) const {
    const auto bind_point =
        IsCompute() ? vk::PipelineBindPoint::eCompute : vk::PipelineBindPoint::eGraphics;
    const auto stage_flags = IsCompute() ? vk::ShaderStageFlagBits::eCompute : AllGraphicsStageBits;
    const vk::PipelineLayout layout = pipeline_layout;
    scheduler.Record([layout, stage_flags, push_data](vk::CommandBuffer cmdbuf) {
        cmdbuf.pushConstants(layout, stage_flags, 0u, sizeof(push_data), &push_data);
    });

    // Bind descriptor set.
    if (set_writes.empty()) {
        return;
    }

    if (uses_push_descriptors) {
        if (!scheduler.IsRecordingDeferred()) {
            scheduler.Record([&](vk::CommandBuffer cmdbuf) {
                cmdbuf.pushDescriptorSetKHR(bind_point, layout, 0, set_writes);
            });
            return;
        }
        // Writes and the infos they point to are laid out in the recording chunk, with the
        // pointers already aimed at those copies: the command only carries a span.
        size_t bytes = set_writes.size() * sizeof(vk::WriteDescriptorSet) + 64;
        for (const auto& write : set_writes) {
            bytes += write.descriptorCount *
                         (sizeof(vk::DescriptorBufferInfo) + sizeof(vk::DescriptorImageInfo) +
                          sizeof(vk::BufferView)) +
                     32;
        }
        scheduler.ReserveRecordData(bytes);
        const auto writes = scheduler.RecordData(std::span<const vk::WriteDescriptorSet>{set_writes});
        auto* patched = const_cast<vk::WriteDescriptorSet*>(writes.data());
        for (size_t i = 0; i < writes.size(); ++i) {
            auto& write = patched[i];
            if (write.pBufferInfo) {
                write.pBufferInfo = scheduler.RecordData(std::span{write.pBufferInfo, write.descriptorCount}).data();
            }
            if (write.pImageInfo) {
                write.pImageInfo = scheduler.RecordData(std::span{write.pImageInfo, write.descriptorCount}).data();
            }
            if (write.pTexelBufferView) {
                write.pTexelBufferView =
                    scheduler.RecordData(std::span{write.pTexelBufferView, write.descriptorCount}).data();
            }
        }
        scheduler.Record([bind_point, layout, writes](vk::CommandBuffer cmdbuf) {
            cmdbuf.pushDescriptorSetKHR(bind_point, layout, 0, writes);
        });
        return;
    }

    // Descriptor set updates are device calls: they stay on this thread.
    const auto desc_set = desc_heap.Commit(desc_layout);
    for (auto& set_write : set_writes) {
        set_write.dstSet = desc_set;
    }
    instance.GetDevice().updateDescriptorSets(set_writes, {});
    scheduler.Record([bind_point, layout, desc_set](vk::CommandBuffer cmdbuf) {
        cmdbuf.bindDescriptorSets(bind_point, layout, 0, desc_set, {});
    });
}

std::string Pipeline::GetDebugString() const {
    std::string stage_desc;
    for (const auto& stage : stages) {
        if (stage) {
            const auto shader_name = PipelineCache::GetShaderName(stage->hw_stage, stage->pgm_hash);
            if (stage_desc.empty()) {
                stage_desc = shader_name;
            } else {
                stage_desc = fmt::format("{},{}", stage_desc, shader_name);
            }
        }
    }
    return stage_desc;
}

} // namespace Vulkan
