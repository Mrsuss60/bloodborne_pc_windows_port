// SPDX-FileCopyrightText: Copyright 2025 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "shader_recompiler/frontend/fetch_shader.h"
#include "video_core/renderer_vulkan/vk_pipeline_cache.h"
#include "video_core/renderer_vulkan/vk_shader_util.h"

namespace Vulkan {

namespace PipelineStore {
/* bbport: single source of truth for the cache version. The runtime constants in
 * vk_pipeline_serialization.cpp used to be a separate copy, and when only one of them was
 * bumped the launcher rewrote profile.bin to the other value on every start. WarmUp then saw
 * a mismatching profile and wiped the whole cache, which is why shaders recompiled forever. */
static constexpr u32 ShaderBinaryVersion = 25u;
static constexpr u32 ShaderMetaVersion = 8u;
static constexpr u32 PipelineKeyVersion = 5u;
/* bbport: monotonic one-time cache reset epoch stored in <exe_dir>/cache_epoch.bin (out/).
 * Bump this when an update must clear pre-existing user caches once without breaking
 * shader_recipes.bin format compatibility (ShaderMetaVersion / PipelineKeyVersion).
 *   Epoch 1: post-0.9.0 (clears pre-fix caches that may hold patched SRT walkers / bad serdes)
 *   Epoch 2: clears caches that imported shifted legacy 16-byte recipe metas / stale import.failed */
static constexpr u32 CacheEpoch = 2u;
}

void RegisterShaderInput(std::vector<u8>&& input, u64 pgm_hash, size_t perm_idx);
bool LoadShaderMeta(Serialization::Archive& ar, Shader::Info& info,
                    std::optional<Shader::Gcn::FetchShaderData>& fetch_shader_data,
                    Shader::StageSpecialization& spec, size_t& perm_idx);
bool LoadShaderMeta(Serialization::Archive& ar, Shader::Info& info,
                    Shader::StageSpecialization& spec, size_t& perm_idx);

void RegisterPipelineData(const ComputePipelineKey& key,
                          ComputePipeline::SerializationSupport& sdata);
void RegisterPipelineData(const GraphicsPipelineKey& key, u64 hash,
                          GraphicsPipeline::SerializationSupport& sdata);
void RegisterShaderMeta(const Shader::Info& info,
                        const std::optional<Shader::Gcn::FetchShaderData>& fetch_shader_data,
                        const Shader::StageSpecialization& spec, size_t perm_hash, size_t perm_idx);
void RegisterShaderBinary(std::vector<u32>&& spv, u64 pgm_hash, size_t perm_idx);

inline void RegisterShaderMeta(const Shader::Info& info,
                               const Shader::StageSpecialization& spec, size_t perm_hash, size_t perm_idx) {
    std::optional<Shader::Gcn::FetchShaderData> unused_fetch{};
    RegisterShaderMeta(info, unused_fetch, spec, perm_hash, perm_idx);
}

} // namespace Vulkan
