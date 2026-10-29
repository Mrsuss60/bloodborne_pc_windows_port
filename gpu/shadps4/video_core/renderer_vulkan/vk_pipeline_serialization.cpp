// SPDX-FileCopyrightText: Copyright 2025-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "common/serdes.h"
#include "core/emulator_settings.h"
#include "shader_recompiler/frontend/fetch_shader.h"
#include "shader_recompiler/info.h"
#include "video_core/cache_storage.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_pipeline_cache.h"
#include <chrono>
#include <future>
#include "common/hash.h"
#include "video_core/renderer_vulkan/vk_shader_util.h" 

namespace Serialization {
/* You should increment versions below once corresponding serialization scheme is changed. */
static constexpr u32 ShaderBinaryVersion = 24u; // bbport: interpolated integer fix (Pascal)
static constexpr u32 ShaderMetaVersion = 8u; // the object motion buffer addresses
static constexpr u32 PipelineKeyVersion = 5u; // bbport: Info layout (ImageResource::needs_native)
} // namespace Serialization

namespace Vulkan {

void RegisterPipelineData(const ComputePipelineKey& key,
                          ComputePipeline::SerializationSupport& sdata) {
    if (!Storage::DataBase::Instance().IsOpened()) {
        return;
    }

    Serialization::Archive ar{};
    Serialization::Writer pldata{ar};

    pldata.Write(Serialization::PipelineKeyVersion);
    pldata.Write(u32{1}); // compute

    key.Serialize(ar);
    sdata.Serialize(ar);

    Storage::DataBase::Instance().Save(Storage::BlobType::PipelineKey,
                                       fmt::format("c_{:#018x}", key.value), ar.TakeOff());
}

void RegisterPipelineData(const GraphicsPipelineKey& key, u64 hash,
                          GraphicsPipeline::SerializationSupport& sdata) {
    // Motion vector pipelines are rebuilt every session (LoadShaderMeta never loads their
    // vertex stage), so a saved key could only fail at the next warm-up.
    if (!Storage::DataBase::Instance().IsOpened() || key.motion_vectors) {
        return;
    }

    Serialization::Archive ar{};
    Serialization::Writer pldata{ar};

    pldata.Write(Serialization::PipelineKeyVersion);
    pldata.Write(u32{0}); // graphics

    key.Serialize(ar);
    sdata.Serialize(ar);

    Storage::DataBase::Instance().Save(Storage::BlobType::PipelineKey,
                                       fmt::format("g_{:#018x}", hash), ar.TakeOff());
}

void RegisterShaderMeta(const Shader::Info& info,
                        const std::optional<Shader::Gcn::FetchShaderData>& fetch_shader_data,
                        const Shader::StageSpecialization& spec, size_t perm_hash,
                        size_t perm_idx) {
    if (!Storage::DataBase::Instance().IsOpened()) {
        return;
    }

    Serialization::Archive ar;
    Serialization::Writer meta{ar};

    meta.Write(Serialization::ShaderMetaVersion);
    meta.Write(Serialization::ShaderBinaryVersion);

    meta.Write(perm_hash);
    meta.Write(perm_idx);

    spec.Serialize(ar);
    info.Serialize(ar);

    Storage::DataBase::Instance().Save(Storage::BlobType::ShaderMeta,
                                       fmt::format("{:#018x}", perm_hash), ar.TakeOff());
}

void RegisterShaderBinary(std::vector<u32>&& spv, u64 pgm_hash, size_t perm_idx) {
    if (!Storage::DataBase::Instance().IsOpened()) {
        return;
    }

    Storage::DataBase::Instance().Save(Storage::BlobType::ShaderBinary,
                                       fmt::format("{:#018x}_{}", pgm_hash, perm_idx),
                                       std::move(spv));
}

void RegisterShaderInput(std::vector<u8>&& input, u64 pgm_hash, size_t perm_idx) {
    if (!Storage::DataBase::Instance().IsOpened()) {
        return;
    }
    Storage::DataBase::Instance().Save(Storage::BlobType::ShaderInput,
                                       fmt::format("{:#018x}_{}", pgm_hash, perm_idx),
                                       std::move(input));
}

bool LoadShaderMeta(Serialization::Archive& ar, Shader::Info& info,
                    std::optional<Shader::Gcn::FetchShaderData>& fetch_shader_data,
                    Shader::StageSpecialization& spec, size_t& perm_idx) {
    Serialization::Reader meta{ar};

    u32 meta_version{};
    meta.Read(meta_version);
    if (meta_version != Serialization::ShaderMetaVersion) {
        return false;
    }

    u32 binary_version{};
    meta.Read(binary_version);
    if (binary_version != Serialization::ShaderBinaryVersion) {
        return false;
    }

    u64 perm_hash_ar{};
    meta.Read(perm_hash_ar);
    meta.Read(perm_idx);

    spec.Deserialize(ar);
    info.Deserialize(ar);

    // Motion vertex shaders embed session-local buffer device addresses. They must be
    // recompiled for the current allocation, never loaded from a previous process.
    if (info.hw_stage == Shader::HwStage::Vertex && spec.runtime_info.hw.vs.motion_vectors) {
        return false;
    }

    fetch_shader_data = spec.fetch_shader_data;
    return true;
}

bool LoadShaderMeta(Serialization::Archive& ar, Shader::Info& info,
                    Shader::StageSpecialization& spec, size_t& perm_idx) {
    std::optional<Shader::Gcn::FetchShaderData> unused_fetch{};
    return LoadShaderMeta(ar, info, unused_fetch, spec, perm_idx);
}

void ComputePipelineKey::Serialize(Serialization::Archive& ar) const {
    Serialization::Writer key{ar};
    key.Write(value);
}

bool ComputePipelineKey::Deserialize(Serialization::Archive& ar) {
    Serialization::Reader key{ar};
    key.Read(value);
    return true;
}

void ComputePipeline::SerializationSupport::Serialize(Serialization::Archive& ar) const {
    // Nothing here yet
    return;
}

bool ComputePipeline::SerializationSupport::Deserialize(Serialization::Archive& ar) {
    // Nothing here yet
    return true;
}

bool PipelineCache::LoadComputePipeline(Serialization::Archive& ar) {
    compute_key.Deserialize(ar);

    ComputePipeline::SerializationSupport sdata{};
    sdata.Deserialize(ar);

    std::vector<u8> meta_blob;
    Storage::DataBase::Instance().Load(Storage::BlobType::ShaderMeta,
                                       fmt::format("{:#018x}", compute_key.value), meta_blob);
    if (meta_blob.empty()) {
        return false;
    }

    Serialization::Archive meta_ar{std::move(meta_blob)};

    if (!LoadPipelineStage(meta_ar, 0)) {
        return false;
    }

    const auto [it, is_new] = compute_pipelines.try_emplace(compute_key);
    ASSERT(is_new);

    it.value() =
        std::make_unique<ComputePipeline>(instance, scheduler, desc_heap, profile, *pipeline_cache,
                                          compute_key, *sel.infos[0], sel.modules[0], sdata, true);

    sel.infos.fill(nullptr);
    sel.modules.fill(nullptr);

    return true;
}

void GraphicsPipelineKey::Serialize(Serialization::Archive& ar) const {
    Serialization::Writer key{ar};

    key.Write(this, sizeof(*this));
}

bool GraphicsPipelineKey::Deserialize(Serialization::Archive& ar) {
    Serialization::Reader key{ar};

    key.Read(this, sizeof(*this));
    return true;
}

void GraphicsPipeline::SerializationSupport::Serialize(Serialization::Archive& ar) const {
    Serialization::Writer sdata{ar};

    sdata.Write(&vertex_attributes, sizeof(vertex_attributes));
    sdata.Write(&vertex_bindings, sizeof(vertex_bindings));
    sdata.Write(&divisors, sizeof(divisors));
    sdata.Write(multisampling);
    sdata.Write(tcs);
    sdata.Write(tes);
}

bool GraphicsPipeline::SerializationSupport::Deserialize(Serialization::Archive& ar) {
    Serialization::Reader sdata{ar};

    sdata.Read(&vertex_attributes, sizeof(vertex_attributes));
    sdata.Read(&vertex_bindings, sizeof(vertex_bindings));
    sdata.Read(&divisors, sizeof(divisors));
    sdata.Read(multisampling);
    sdata.Read(tcs);
    sdata.Read(tes);
    return true;
}

bool PipelineCache::LoadGraphicsPipeline(Serialization::Archive& ar, std::vector<PreloadJob>& jobs) {
    sel.infos.fill(nullptr);
    sel.modules.fill(nullptr);
    sel.fetch_shader.reset();
    sel.graphics_key.Deserialize(ar);

    GraphicsPipeline::SerializationSupport sdata{};
    sdata.Deserialize(ar);

    for (int stage_idx = 0; stage_idx < MaxShaderStages; ++stage_idx) {
        const auto& hash = sel.graphics_key.stage_hashes[stage_idx];
        if (!hash) {
            continue;
        }

        std::vector<u8> meta_blob;
        Storage::DataBase::Instance().Load(Storage::BlobType::ShaderMeta,
                                           fmt::format("{:#018x}", hash), meta_blob);
        if (meta_blob.empty()) {
            return false;
        }

        Serialization::Archive meta_ar{std::move(meta_blob)};

        if (!LoadPipelineStage(meta_ar, stage_idx)) {
            return false;
        }
    }

    jobs.emplace_back(sel.graphics_key,
                      [this, key = sel.graphics_key, infos = sel.infos,
                       runtime_infos = sel.runtime_infos, fetch = sel.fetch_shader,
                       modules = sel.modules, sdata = std::move(sdata)]() mutable {
                          return std::make_unique<GraphicsPipeline>(
                              instance, scheduler, desc_heap, profile, key, *pipeline_cache,
                              infos, runtime_infos, fetch, modules, sdata, true);
                      });

    sel.infos.fill(nullptr);
    sel.modules.fill(nullptr);
    sel.fetch_shader.reset();

    return true;
}

bool PipelineCache::LoadGraphicsPipeline(Serialization::Archive& ar) {
    sel.graphics_key.Deserialize(ar);

    GraphicsPipeline::SerializationSupport sdata{};
    sdata.Deserialize(ar);

    for (int stage_idx = 0; stage_idx < MaxShaderStages; ++stage_idx) {
        const auto& hash = sel.graphics_key.stage_hashes[stage_idx];
        if (!hash) {
            continue;
        }

        std::vector<u8> meta_blob;
        Storage::DataBase::Instance().Load(Storage::BlobType::ShaderMeta,
                                           fmt::format("{:#018x}", hash), meta_blob);
        if (meta_blob.empty()) {
            return false;
        }

        Serialization::Archive meta_ar{std::move(meta_blob)};

        if (!LoadPipelineStage(meta_ar, stage_idx)) {
            return false;
        }
    }

    const auto [it, is_new] = graphics_pipelines.try_emplace(sel.graphics_key);
    ASSERT(is_new);

    it.value() = std::make_unique<GraphicsPipeline>(
        instance, scheduler, desc_heap, profile, sel.graphics_key, *pipeline_cache, sel.infos,
        sel.runtime_infos, sel.fetch_shader, sel.modules, sdata, true);

    sel.infos.fill(nullptr);
    sel.modules.fill(nullptr);
    sel.fetch_shader.reset();

    return true;
}

bool PipelineCache::LoadPipelineStage(Serialization::Archive& ar, size_t stage) {
    auto program = std::make_unique<Program>();
    Shader::StageSpecialization spec{};
    spec.info = &program->info;
    size_t perm_idx{};
    if (!LoadShaderMeta(ar, program->info, sel.fetch_shader, spec, perm_idx)) {
        return false;
    }

    std::vector<u32> spv{};
    Storage::DataBase::Instance().Load(Storage::BlobType::ShaderBinary,
                                       fmt::format("{:#018x}_{}", program->info.pgm_hash, perm_idx),
                                       spv);
    if (spv.empty() || spv[0] != 0x07230203u) {
        LOG_WARNING(Render_Vulkan, "Cached shader binary {}_{:x}_{} is missing or invalid SPIR-V",
                    program->info.hw_stage, program->info.pgm_hash, perm_idx);
        return false;
    }

    // Permutation hash depends on shader variation index. To prevent collisions, we need insert it
    // at the exact position rather than append

    vk::ShaderModule module{};

    auto [it_pgm, new_program] = program_cache.try_emplace(program->info.pgm_hash);
    if (new_program) {
        module = CompileSPV(spv, instance.GetDevice());
        if (!module) {
            return false;
        }
        it_pgm.value() = std::move(program);
    } else {
        const auto& it = std::ranges::find(it_pgm.value()->modules, spec, &Program::Module::spec);
        if (it != it_pgm.value()->modules.end()) {
            // A matching permutation is valid only at its original index. A different index means
            // the store holds entries from more than one cache generation, so this pipeline is
            // left to compile at runtime.
            const auto idx = std::distance(it_pgm.value()->modules.begin(), it);
            if (perm_idx != idx) {
                LOG_WARNING(Render_Vulkan,
                            "Cached permutation {} of {}_{:x} conflicts with index {}, skipping "
                            "preload",
                            perm_idx, program->info.hw_stage, program->info.pgm_hash, idx);
                return false;
            }
            module = it->module;
        } else {
            module = CompileSPV(spv, instance.GetDevice());
            if (!module) {
                return false;
            }
        }
    }
    it_pgm.value()->InsertPermut(module, std::move(spec), perm_idx);

    sel.infos[stage] = &it_pgm.value()->info;
    sel.modules[stage] = module;

    return true;
}

void PipelineCache::SaveProfile() {
    constexpr u32 CacheMagic = 0x42425043u;
    u32 hdr[4] = {CacheMagic, Serialization::ShaderBinaryVersion, Serialization::ShaderMetaVersion, Serialization::PipelineKeyVersion};
    std::vector<u8> data(sizeof(hdr) + sizeof(profile));
    std::memcpy(data.data(), hdr, sizeof(hdr));
    std::memcpy(data.data() + sizeof(hdr), &profile, sizeof(profile));
    Storage::DataBase::Instance().Save(Storage::BlobType::ShaderProfile, "profile", std::move(data));
}

void PipelineCache::WarmUp() {
    if (!EmulatorSettings.IsPipelineCacheEnabled()) {
        return;
    }

    const auto warmup_start = std::chrono::steady_clock::now();

    Storage::DataBase::Instance().Open();

    // Check if cache is compatible
    std::vector<u8> profile_data{};
    Storage::DataBase::Instance().Load(Storage::BlobType::ShaderProfile, "profile", profile_data);
    constexpr u32 CacheMagic = 0x42425043u;
    const auto save_profile = [&]() {
        u32 hdr[4] = {CacheMagic, Serialization::ShaderBinaryVersion, Serialization::ShaderMetaVersion, Serialization::PipelineKeyVersion};
        std::vector<u8> data(sizeof(hdr) + sizeof(profile));
        std::memcpy(data.data(), hdr, sizeof(hdr));
        std::memcpy(data.data() + sizeof(hdr), &profile, sizeof(profile));
        Storage::DataBase::Instance().Save(Storage::BlobType::ShaderProfile, "profile", std::move(data));
    };
    if (profile_data.empty()) {
        Storage::DataBase::Instance().FinishPreload();
        SaveProfile();
        return;
    }
    Shader::Profile cached_profile{};
    bool valid = false;
    if (profile_data.size() == 16 + sizeof(Shader::Profile)) {
        u32 hdr[4];
        std::memcpy(hdr, profile_data.data(), sizeof(hdr));
        if (hdr[0] == CacheMagic &&
            hdr[1] == Serialization::ShaderBinaryVersion &&
            hdr[2] == Serialization::ShaderMetaVersion &&
            hdr[3] == Serialization::PipelineKeyVersion) {
            std::memcpy(&cached_profile, profile_data.data() + 16, sizeof(cached_profile));
            valid = (cached_profile == profile);
        }
    } else if (profile_data.size() == sizeof(Shader::Profile)) {
        std::memcpy(&cached_profile, profile_data.data(), sizeof(cached_profile));
        valid = (cached_profile == profile);
    }
    if (!valid) {
        LOG_WARNING(Render, "Pipeline cache isn't compatible with current system: rebuilding it");
        Storage::DataBase::Instance().Clear();
        Storage::DataBase::Instance().FinishPreload();
        SaveProfile();
        return;
    }

    const char* env_warmup = std::getenv("BB_WARMUP_ASYNC");
    const bool warmup_async = !env_warmup || (std::strcmp(env_warmup, "0") != 0);

    u32 num_pipelines{};
    u32 num_total_pipelines{};
    u32 num_removed{};

    if (!warmup_async) {
        try {
            num_removed = Storage::DataBase::Instance().ForEachBlob(
                Storage::BlobType::PipelineKey, [&](std::vector<u8>&& data) {
                    ++num_total_pipelines;

                    Serialization::Archive ar{std::move(data)};
                    Serialization::Reader pldata{ar};

                    u32 version{};
                    pldata.Read(version);
                    if (version != Serialization::PipelineKeyVersion) {
                        return false;
                    }

                    u32 is_compute{};
                    pldata.Read(is_compute);

                    bool result{};
                    if (is_compute) {
                        result = LoadComputePipeline(ar);
                    } else {
                        result = LoadGraphicsPipeline(ar);
                    }

                    if (result) {
                        ++num_pipelines;
                    }
                    return result;
                });
        } catch (const Serialization::CorruptData& e) {
            LOG_ERROR(Render, "Corrupted pipeline cache detected during WarmUp: {}. Clearing cache.",
                      e.what());
            Storage::DataBase::Instance().Clear();
            Storage::DataBase::Instance().FinishPreload();
            profile_data.resize(sizeof(profile));
            std::memcpy(profile_data.data(), &profile, sizeof(profile));
            Storage::DataBase::Instance().Save(Storage::BlobType::ShaderProfile, "profile",
                                               std::move(profile_data));
            return;
        }
    } else {
        struct StagedPermut {
            Program* program = nullptr;
            size_t perm_idx = 0;
            Shader::StageSpecialization spec;
            std::shared_future<vk::ShaderModule> future;
        };

        struct WarmUpGraphicsEntry {
            GraphicsPipelineKey key;
            GraphicsPipeline::SerializationSupport sdata;
            std::array<const Shader::Info*, MaxShaderStages> infos{};
            std::array<std::shared_future<vk::ShaderModule>, MaxShaderStages> futures{};
            std::optional<Shader::Gcn::FetchShaderData> fetch_shader;
        };

        struct WarmUpComputeEntry {
            ComputePipelineKey key;
            ComputePipeline::SerializationSupport sdata;
            const Shader::Info* info = nullptr;
            std::shared_future<vk::ShaderModule> future;
        };

        std::vector<WarmUpGraphicsEntry> staged_graphics;
        std::vector<WarmUpComputeEntry> staged_compute;
        std::vector<StagedPermut> staged_permutations;
        tsl::robin_map<u64, std::shared_future<vk::ShaderModule>> spv_futures;

        auto stage_shader = [&](Serialization::Archive& ar,
                                std::optional<Shader::Gcn::FetchShaderData>& out_fetch_shader,
                                const Shader::Info*& out_info,
                                std::shared_future<vk::ShaderModule>& out_future) -> bool {
            auto program = std::make_unique<Program>();
            Shader::StageSpecialization spec{};
            spec.info = &program->info;
            size_t perm_idx{};
            if (!LoadShaderMeta(ar, program->info, out_fetch_shader, spec, perm_idx)) {
                return false;
            }

            std::vector<u32> spv{};
            Storage::DataBase::Instance().Load(
                Storage::BlobType::ShaderBinary,
                fmt::format("{:#018x}_{}", program->info.pgm_hash, perm_idx), spv);
            if (spv.empty() || spv[0] != 0x07230203u) {
                LOG_WARNING(Render_Vulkan, "Cached shader binary {}_{:x}_{} is missing or invalid SPIR-V",
                            program->info.hw_stage, program->info.pgm_hash, perm_idx);
                return false;
            }

            const u64 perm_hash = HashCombine(program->info.pgm_hash, perm_idx);
            Program* program_ptr = nullptr;
            {
                std::unique_lock lk{programs_mutex};
                auto [it_pgm, new_program] = program_cache.try_emplace(program->info.pgm_hash);
                if (new_program) {
                    it_pgm.value() = std::move(program);
                }
                program_ptr = it_pgm.value().get();
            }

            out_info = &program_ptr->info;
            spec.info = &program_ptr->info;

            auto it = std::ranges::find_if(staged_permutations, [&](const StagedPermut& p) {
                return p.program == program_ptr && p.spec == spec;
            });
            if (it != staged_permutations.end()) {
                if (it->perm_idx != perm_idx) {
                    LOG_WARNING(Render_Vulkan,
                                "Cached permutation {} of {}_{:x} conflicts with index {}, skipping preload",
                                perm_idx, program_ptr->info.hw_stage, program_ptr->info.pgm_hash, it->perm_idx);
                    return false;
                }
                out_future = it->future;
            } else {
                auto conflict_it = std::ranges::find_if(staged_permutations, [&](const StagedPermut& p) {
                    return p.program == program_ptr && p.perm_idx == perm_idx;
                });
                if (conflict_it != staged_permutations.end()) {
                    LOG_WARNING(Render_Vulkan,
                                "Cached permutation index {} of {}_{:x} already used with different spec, skipping preload",
                                perm_idx, program_ptr->info.hw_stage, program_ptr->info.pgm_hash);
                    return false;
                }

                auto task = std::make_shared<std::packaged_task<vk::ShaderModule()>>(
                    [device = instance.GetDevice(), spv_data = std::move(spv)]() {
                        return CompileSPV(spv_data, device);
                    });
                out_future = task->get_future().share();
                spv_futures.emplace(perm_hash, out_future);
                GetCompiler().Submit([task]() { (*task)(); });
                staged_permutations.push_back(StagedPermut{
                    .program = program_ptr,
                    .perm_idx = perm_idx,
                    .spec = std::move(spec),
                    .future = out_future,
                });
            }
            return true;
        };

        try {
            num_removed = Storage::DataBase::Instance().ForEachBlob(
                Storage::BlobType::PipelineKey, [&](std::vector<u8>&& data) {
                    ++num_total_pipelines;

                    Serialization::Archive ar{std::move(data)};
                    Serialization::Reader pldata{ar};

                    u32 version{};
                    pldata.Read(version);
                    if (version != Serialization::PipelineKeyVersion) {
                        return false;
                    }

                    u32 is_compute{};
                    pldata.Read(is_compute);

                    if (is_compute) {
                        ComputePipelineKey key{};
                        key.Deserialize(ar);
                        ComputePipeline::SerializationSupport sdata{};
                        sdata.Deserialize(ar);

                        std::vector<u8> meta_blob;
                        Storage::DataBase::Instance().Load(
                            Storage::BlobType::ShaderMeta,
                            fmt::format("{:#018x}", key.value), meta_blob);
                        if (meta_blob.empty()) {
                            return false;
                        }

                        Serialization::Archive meta_ar{std::move(meta_blob)};
                        std::optional<Shader::Gcn::FetchShaderData> fetch_dummy;
                        const Shader::Info* c_info = nullptr;
                        std::shared_future<vk::ShaderModule> c_future;
                        if (!stage_shader(meta_ar, fetch_dummy, c_info, c_future)) {
                            return false;
                        }

                        staged_compute.push_back(WarmUpComputeEntry{
                            .key = key,
                            .sdata = sdata,
                            .info = c_info,
                            .future = c_future,
                        });
                        return true;
                    } else {
                        GraphicsPipelineKey key{};
                        key.Deserialize(ar);
                        GraphicsPipeline::SerializationSupport sdata{};
                        sdata.Deserialize(ar);

                        WarmUpGraphicsEntry entry{};
                        entry.key = key;
                        entry.sdata = sdata;

                        for (int stage_idx = 0; stage_idx < MaxShaderStages; ++stage_idx) {
                            const auto& hash = key.stage_hashes[stage_idx];
                            if (!hash) {
                                continue;
                            }
                            std::vector<u8> meta_blob;
                            Storage::DataBase::Instance().Load(
                                Storage::BlobType::ShaderMeta,
                                fmt::format("{:#018x}", hash), meta_blob);
                            if (meta_blob.empty()) {
                                return false;
                            }
                            Serialization::Archive meta_ar{std::move(meta_blob)};
                            if (!stage_shader(meta_ar, entry.fetch_shader, entry.infos[stage_idx],
                                              entry.futures[stage_idx])) {
                                return false;
                            }
                        }

                        staged_graphics.push_back(std::move(entry));
                        return true;
                    }
                });
        } catch (const Serialization::CorruptData& e) {
            LOG_ERROR(Render, "Corrupted pipeline cache detected during WarmUp: {}. Clearing cache.",
                      e.what());
            Storage::DataBase::Instance().Clear();
            Storage::DataBase::Instance().FinishPreload();
            profile_data.resize(sizeof(profile));
            std::memcpy(profile_data.data(), &profile, sizeof(profile));
            Storage::DataBase::Instance().Save(Storage::BlobType::ShaderProfile, "profile",
                                               std::move(profile_data));
            return;
        }

        // Between passes: wait for all compilation futures to complete and populate program permutations
        for (auto& item : staged_permutations) {
            vk::ShaderModule mod = item.future.get();
            if (mod) {
                std::unique_lock lk{programs_mutex};
                item.program->InsertPermut(mod, std::move(item.spec), item.perm_idx);
            }
        }

        // Pass 2: synchronous pipeline instantiation on startup thread
        for (auto& entry : staged_compute) {
            vk::ShaderModule mod = entry.future.get();
            if (!mod) {
                continue;
            }
            const auto [it, is_new] = compute_pipelines.try_emplace(entry.key);
            if (is_new) {
                it.value() = std::make_unique<ComputePipeline>(
                    instance, scheduler, desc_heap, profile, *pipeline_cache, entry.key,
                    *entry.info, mod, entry.sdata, true);
                ++num_pipelines;
            }
        }

        const std::array<Shader::RuntimeInfo, MaxShaderStages> empty_runtime_infos{};
        for (auto& entry : staged_graphics) {
            bool all_stages_ok = true;
            std::array<vk::ShaderModule, MaxShaderStages> modules{};
            for (size_t s = 0; s < MaxShaderStages; ++s) {
                if (entry.futures[s].valid()) {
                    modules[s] = entry.futures[s].get();
                    if (!modules[s]) {
                        all_stages_ok = false;
                        break;
                    }
                }
            }
            if (!all_stages_ok) {
                continue;
            }
            const auto [it, is_new] = graphics_pipelines.try_emplace(entry.key);
            if (is_new) {
                it.value() = std::make_unique<GraphicsPipeline>(
                    instance, scheduler, desc_heap, profile, entry.key, *pipeline_cache,
                    entry.infos, empty_runtime_infos, entry.fetch_shader, modules, entry.sdata, true);
                ++num_pipelines;
            }
        }
    }

    sel.infos.fill(nullptr);
    sel.modules.fill(nullptr);
    sel.fetch_shader.reset();

    const auto warmup_end = std::chrono::steady_clock::now();
    const auto warmup_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(warmup_end - warmup_start).count();

    LOG_INFO(Render, "Pipeline cache warmup completed in {} ms (preloaded {} pipelines, mode={})",
             warmup_ms, num_pipelines, warmup_async ? "async" : "sync");
    if (num_total_pipelines > num_pipelines) {
        LOG_WARNING(Render, "{} cached pipelines could not be loaded: removed {} keys, kept {}",
                    num_total_pipelines - num_pipelines, num_removed, num_pipelines);
    }

    Storage::DataBase::Instance().FinishPreload();
}

std::string PipelineCache::GetDriverCacheKey() const {
    const auto props = instance.GetPhysicalDevice().getProperties();
    // Unique key per vendor, device ID, and driver version to avoid invalid binary loads across updates
    return fmt::format("vk_pipe_cache_{:04x}_{:04x}_{:08x}",
                       props.vendorID, props.deviceID, props.driverVersion);
}

void PipelineCache::SaveDriverPipelineCache() {
    if (!pipeline_cache || !Storage::DataBase::Instance().IsOpened()) {
        return;
    }
    try {
        auto [res, data] = instance.GetDevice().getPipelineCacheData(*pipeline_cache);
        if (res == vk::Result::eSuccess && !data.empty()) {
            LOG_INFO(Render, "Saving native Vulkan driver pipeline cache ({} bytes)", data.size());
            Storage::DataBase::Instance().Save(Storage::BlobType::PipelineBinary,
                                               GetDriverCacheKey(), std::move(data));
        }
    } catch (const std::exception& e) {
        LOG_WARNING(Render, "Failed to retrieve native Vulkan driver pipeline cache: {}", e.what());
    }
}

void PipelineCache::Sync() {
    SaveDriverPipelineCache();
    Storage::DataBase::Instance().Close();
}

} // namespace Vulkan

namespace Shader {

void Info::Serialize(Serialization::Archive& ar) const {
    Serialization::Writer info{ar};

    info.Write(this, sizeof(InfoPersistent));
    info.Write(flattened_ud_buf);
    srt_info.Serialize(ar);
}

bool Info::Deserialize(Serialization::Archive& ar) {
    Serialization::Reader info{ar};

    info.Read(this, sizeof(Shader::InfoPersistent));
    info.Read(flattened_ud_buf);

    return srt_info.Deserialize(ar);
}

void Gcn::FetchShaderData::Serialize(Serialization::Archive& ar) const {
    Serialization::Writer fetch{ar};
    ar.Grow(6 + attributes.size() * sizeof(VertexAttribute));

    fetch.Write(size);
    fetch.Write(vertex_offset_sgpr);
    fetch.Write(instance_offset_sgpr);
    fetch.Write(attributes);
}

bool Gcn::FetchShaderData::Deserialize(Serialization::Archive& ar) {
    Serialization::Reader fetch{ar};

    fetch.Read(size);
    fetch.Read(vertex_offset_sgpr);
    fetch.Read(instance_offset_sgpr);
    fetch.Read(attributes);

    return true;
}

void PersistentSrtInfo::Serialize(Serialization::Archive& ar) const {
    Serialization::Writer srt{ar};

    srt.Write(this, sizeof(*this));
    if (walker_func_size) {
        srt.Write(reinterpret_cast<void*>(walker_func), walker_func_size);
    }
}

bool PersistentSrtInfo::Deserialize(Serialization::Archive& ar) {
    Serialization::Reader srt{ar};

    srt.Read(this, sizeof(*this));

    if (walker_func_size) {
        walker_func = RegisterWalkerCode(ar.CurrPtr(), walker_func_size);
        ar.Advance(walker_func_size);
    }

    return true;
}

void StageSpecialization::Serialize(Serialization::Archive& ar) const {
    Serialization::Writer spec{ar};

    spec.Write(start);
    spec.Write(runtime_info);

    spec.Write(bitset.to_string());

    if (fetch_shader_data) {
        spec.Write(sizeof(*fetch_shader_data));
        fetch_shader_data->Serialize(ar);
    } else {
        spec.Write(size_t{0});
    }

    spec.Write(vs_attribs);
    spec.Write(buffers);
    spec.Write(images);
    spec.Write(fmasks);
    spec.Write(samplers);
}

bool StageSpecialization::Deserialize(Serialization::Archive& ar) {
    Serialization::Reader spec{ar};

    spec.Read(start);
    spec.Read(runtime_info);

    std::string bits{};
    spec.Read(bits);
    bitset = std::bitset<MaxStageResources>(bits);

    u64 fetch_data_size{};
    spec.Read(fetch_data_size);

    if (fetch_data_size) {
        Gcn::FetchShaderData fetch_data;
        fetch_data.Deserialize(ar);
        fetch_shader_data = fetch_data;
    }

    spec.Read(vs_attribs);
    spec.Read(buffers);
    spec.Read(images);
    spec.Read(fmasks);
    spec.Read(samplers);

    return true;
}

} // namespace Shader
