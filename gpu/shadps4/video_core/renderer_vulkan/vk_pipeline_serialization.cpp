// SPDX-FileCopyrightText: Copyright 2025-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "common/serdes.h"
#include "common/scope_exit.h"
#include "common/thread.h"
#include "core/emulator_settings.h"
#include "shader_recompiler/frontend/fetch_shader.h"
#include "shader_recompiler/info.h"
#include "video_core/cache_storage.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_pipeline_cache.h"
#include <chrono>
#include <filesystem>
#include <future>
#include <string>
#include <unordered_set>
#include "bbport_settings.h"
#include "shim/bb_env.h"
#include "common/hash.h"
#include "video_core/renderer_vulkan/vk_shader_util.h" 
#include "video_core/renderer_vulkan/vk_pipeline_serialization.h"

namespace Serialization {
/* bbport: the versions live in vk_pipeline_serialization.h (PipelineStore) so the runtime, the
 * shader-rebuild tool and the recipe importer cannot drift apart. When they did, the rebuild tool
 * rewrote profile.bin to its own version after every start and WarmUp wiped the cache next time. */
static_assert(Vulkan::PipelineStore::ShaderBinaryVersion == 25u,
              "modernized recompiler & fast shader math (Phase 2)");
static_assert(Vulkan::PipelineStore::ShaderMetaVersion == 8u,
              "the object motion buffer addresses");
static_assert(Vulkan::PipelineStore::PipelineKeyVersion == 5u,
              "Info layout (ImageResource::needs_native)");
static constexpr u32 ShaderBinaryVersion = Vulkan::PipelineStore::ShaderBinaryVersion;
static constexpr u32 ShaderMetaVersion = Vulkan::PipelineStore::ShaderMetaVersion;
static constexpr u32 PipelineKeyVersion = Vulkan::PipelineStore::PipelineKeyVersion;
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

namespace {
void RemoveCorruptShaderMeta(u64 hash, const char* reason) {
    auto& db = Storage::DataBase::Instance();
    if (!db.IsOpened()) {
        return;
    }
    const auto name = fmt::format("{:#018x}.meta", hash);
    std::error_code ec;
    std::filesystem::remove(db.Directory() / name, ec);
    LOG_WARNING(Render, "Removed corrupt shader meta {} ({}); remaining cache preserved", name,
                reason);
}
} // namespace

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

    try {
        if (!LoadPipelineStage(meta_ar, 0)) {
            return false;
        }
    } catch (const Serialization::CorruptData& e) {
        RemoveCorruptShaderMeta(compute_key.value, e.what());
        return false;
    }

    const auto [it, is_new] = compute_pipelines.try_emplace(compute_key);
    if (!is_new) {
        return false;
    }

    try {
        it.value() = std::make_unique<ComputePipeline>(
            instance, scheduler, desc_heap, profile, *pipeline_cache, compute_key, *sel.infos[0],
            sel.modules[0], sdata, true);
    } catch (const std::exception& e) {
        /* bbport: a cached pipeline the driver refuses must not abort startup (the constructor
         * now throws instead of asserting). Drop the entry; the shader compiles on demand. */
        compute_pipelines.erase(compute_key);
        LOG_WARNING(Render_Vulkan, "Cached compute pipeline {:#x} failed to create: {}",
                    compute_key.value, e.what());
        return false;
    }

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
    sdata.Write(fragment);
}

bool GraphicsPipeline::SerializationSupport::Deserialize(Serialization::Archive& ar) {
    Serialization::Reader sdata{ar};

    sdata.Read(&vertex_attributes, sizeof(vertex_attributes));
    sdata.Read(&vertex_bindings, sizeof(vertex_bindings));
    sdata.Read(&divisors, sizeof(divisors));
    sdata.Read(multisampling);
    sdata.Read(tcs);
    sdata.Read(tes);
    if (!ar.IsEoS()) {
        sdata.Read(fragment);
    } else {
        fragment.clear();
    }
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

        try {
            if (!LoadPipelineStage(meta_ar, stage_idx)) {
                return false;
            }
        } catch (const Serialization::CorruptData& e) {
            RemoveCorruptShaderMeta(hash, e.what());
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

        try {
            if (!LoadPipelineStage(meta_ar, stage_idx)) {
                return false;
            }
        } catch (const Serialization::CorruptData& e) {
            RemoveCorruptShaderMeta(hash, e.what());
            return false;
        }
    }

    const auto [it, is_new] = graphics_pipelines.try_emplace(sel.graphics_key);
    if (!is_new) {
        return false;
    }

    try {
        it.value() = std::make_unique<GraphicsPipeline>(
            instance, scheduler, desc_heap, profile, sel.graphics_key, *pipeline_cache, sel.infos,
            sel.runtime_infos, sel.fetch_shader, sel.modules, sdata, true);
    } catch (const std::exception& e) {
        /* bbport: a cached pipeline the driver refuses must not abort startup (the constructor
         * now throws instead of asserting). Drop the entry; the shader compiles on demand. */
        graphics_pipelines.erase(sel.graphics_key);
        LOG_WARNING(Render_Vulkan, "Cached graphics pipeline failed to create: {}", e.what());
        return false;
    }

    sel.infos.fill(nullptr);
    sel.modules.fill(nullptr);
    sel.fetch_shader.reset();

    return true;
}

bool PipelineCache::LoadPipelineStage(Serialization::Archive& ar, size_t stage,
                                      vk::ShaderModule precompiled) {
    bool consumed = false;
    SCOPE_EXIT {
        if (!consumed && precompiled) {
            instance.GetDevice().destroyShaderModule(precompiled);
        }
    };
    auto program = std::make_unique<Program>();
    Shader::StageSpecialization spec{};
    spec.info = &program->info;
    size_t perm_idx{};
    if (!LoadShaderMeta(ar, program->info, sel.fetch_shader, spec, perm_idx)) {
        return false;
    }

    // bbport: the permutation index is only the name of the module in the store, not the
    // shader's identity. Different sessions appended permutations in the order the game first
    // used them, so the same specialization can be stored under another index. An equal
    // specialization is the same SPIR-V, so reuse the module already in memory and keep the
    // pipeline; refusing it dropped hundreds of otherwise valid preloads every launch.
    auto it_pgm = program_cache.find(program->info.pgm_hash);
    if (it_pgm != program_cache.end()) {
        Program* live = it_pgm.value().get();
        const auto it = std::ranges::find(live->modules, spec, &Program::Module::spec);
        if (it != live->modules.end()) {
            sel.infos[stage] = &live->info;
            sel.modules[stage] = it->module;
            return true;
        }
    }

    vk::ShaderModule module = precompiled;
    if (!module) {
        std::vector<u32> spv{};
        Storage::DataBase::Instance().Load(
            Storage::BlobType::ShaderBinary,
            fmt::format("{:#018x}_{}", program->info.pgm_hash, perm_idx), spv);
        if (spv.empty() || spv[0] != 0x07230203u) {
            LOG_WARNING(Render_Vulkan,
                        "Cached shader binary {}_{:x}_{} is missing or invalid SPIR-V",
                        program->info.hw_stage, program->info.pgm_hash, perm_idx);
            return false;
        }

        module = CompileSPV(spv, instance.GetDevice());
        if (!module) {
            return false;
        }
    }

    const u64 flat_hash =
        XXH3_64bits(program->info.flattened_ud_buf.data(),
                    program->info.flattened_ud_buf.size() * sizeof(u32));

    if (it_pgm == program_cache.end()) {
        Program* live = program.get();
        live->InsertPermut(module, std::move(spec), perm_idx, flat_hash);
        consumed = true;
        live->info_template = std::make_unique<Shader::Info>(live->info);
        program_cache.emplace(live->info.pgm_hash, std::move(program));
        sel.infos[stage] = &live->info;
    } else {
        Program* live = it_pgm.value().get();
        if (!live->info_template) {
            live->info_template = std::make_unique<Shader::Info>(live->info);
        }
        // `spec` was built against the temporary program; point it at the live one before it is
        // stored (the previous code left a dangling Shader::Info in the module list).
        spec.info = &live->info;
        if (perm_idx < live->modules.size() && live->modules[perm_idx].spec.Valid()) {
            // The stored index is occupied by a different specialization. Appending keeps the
            // modules referenced by pipelines loaded earlier in this warm-up valid.
            live->AddPermut(module, std::move(spec), flat_hash);
        } else {
            live->InsertPermut(module, std::move(spec), perm_idx, flat_hash);
        }
        consumed = true;
        sel.infos[stage] = &live->info;
    }
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

namespace {
/* bbport: metas stamped with a version this build cannot load (LoadShaderMeta rejects both a
 * different meta and binary version) are dead weight. They are left behind by builds/tools that
 * wrote an older constant; the keys that referenced them are dropped on the first warm-up that
 * sees them. Delete the metas and their companion .spv/.in blobs so the cache converges instead
 * of keeping dead copies and making "N stale metas" look like an active writer. A companion is
 * named <pgm_hash>_<perm_idx>, and its meta's file name is exactly HashCombine(pgm_hash,
 * perm_idx), so it is orphaned precisely when that hash is not one of the live metas. */
void PurgeStaleShaderMetas(bool clean_orphans = false) {
    auto& db = Storage::DataBase::Instance();
    if (!db.IsOpened() || EmulatorSettings.IsPipelineCacheArchived()) {
        return;
    }

    std::unordered_set<u64> live_metas;
    const u32 removed = db.ForEachBlob(
        Storage::BlobType::ShaderMeta, [&](std::vector<u8>&& data) -> Storage::BlobAction {
            if (data.size() < 16) {
                return Storage::BlobAction::Delete;
            }
            u32 meta_version{};
            u32 binary_version{};
            u64 name{};
            std::memcpy(&meta_version, data.data() + 0, sizeof(meta_version));
            std::memcpy(&binary_version, data.data() + 4, sizeof(binary_version));
            std::memcpy(&name, data.data() + 8, sizeof(name));
            if (meta_version != Serialization::ShaderMetaVersion ||
                binary_version != Serialization::ShaderBinaryVersion) {
                return Storage::BlobAction::Delete;
            }
            live_metas.insert(name);
            return Storage::BlobAction::Keep;
        });
    if (removed == 0 && !clean_orphans) {
        return;
    }

    u32 companions{};
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator{db.Directory(), ec}) {
        if (!entry.is_regular_file(ec)) {
            continue;
        }
        const auto& path = entry.path();
        const auto ext = path.extension().string();
        if (ext != ".spv" && ext != ".in") {
            continue;
        }
        const auto stem = path.stem().string();
        const auto sep = stem.rfind('_');
        if (sep == std::string::npos) {
            continue;
        }
        u64 pgm_hash{};
        u64 perm_idx{};
        try {
            pgm_hash = std::stoull(stem.substr(0, sep), nullptr, 16);
            perm_idx = std::stoull(stem.substr(sep + 1), nullptr, 10);
        } catch (const std::exception&) {
            continue;
        }
        if (!live_metas.contains(HashCombine(pgm_hash, perm_idx))) {
            companions += std::filesystem::remove(path, ec) ? 1 : 0;
        }
    }
    LOG_WARNING(Render,
                "Removed {} shader metas stamped with an unsupported version and {} orphaned "
                "shader blobs; every meta left is version {}/{}",
                removed, companions, Serialization::ShaderMetaVersion,
                Serialization::ShaderBinaryVersion);
}
} // namespace

void PipelineCache::WarmUp(const std::function<void()>& tick_ui) {
    if (!EmulatorSettings.IsPipelineCacheEnabled()) {
        return;
    }

    const auto warmup_start = std::chrono::steady_clock::now();
    auto last_ui = warmup_start;
    bool ui_active = false;
    const auto maybe_tick_ui = [&] {
        if (!tick_ui) {
            return;
        }
        const auto now = std::chrono::steady_clock::now();
        if (!ui_active) {
            if (now - warmup_start < std::chrono::milliseconds(150)) {
                return;
            }
            ui_active = true;
            g_bb_boot_precompile_active.store(true, std::memory_order_release);
            tick_ui();
            last_ui = std::chrono::steady_clock::now();
            return;
        }
        if (now - last_ui >= std::chrono::milliseconds(33)) {
            tick_ui();
            last_ui = std::chrono::steady_clock::now();
        }
    };
    SCOPE_EXIT {
        if (ui_active) {
            g_bb_boot_precompile_active.store(false, std::memory_order_release);
        }
    };

    Storage::DataBase::Instance().Open();

    // Check if cache is compatible
    std::vector<u8> profile_data{};
    Storage::DataBase::Instance().Load(Storage::BlobType::ShaderProfile, "profile", profile_data);
    constexpr u32 CacheMagic = 0x42425043u;
    if (profile_data.empty()) {
        Storage::DataBase::Instance().FinishPreload();
        SaveProfile();
        return;
    }
    Shader::Profile cached_profile{};
    bool valid = false;
    bool have_cached = false;
    bool stale_version_header = false;
    /* bbport: older builds of the shader-rebuild tool stamped profile.bin with their own copy of
     * the binary version (24) while the runtime wrote 25. The profile contents and the meta/key
     * versions are still current in that case, and every individual .meta is version-checked by
     * LoadShaderMeta, so the header can be updated in place instead of deleting a valid cache.
     * This stops the launcher<->WarmUp version flip from wiping all shaders on every launch. */
    constexpr u32 StaleToolBinaryVersion = 24u;
    if (profile_data.size() == 16 + sizeof(Shader::Profile)) {
        u32 hdr[4];
        std::memcpy(hdr, profile_data.data(), sizeof(hdr));
        if (hdr[0] == CacheMagic &&
            (hdr[1] == Serialization::ShaderBinaryVersion ||
             hdr[1] == StaleToolBinaryVersion) &&
            hdr[2] == Serialization::ShaderMetaVersion &&
            hdr[3] == Serialization::PipelineKeyVersion) {
            std::memcpy(&cached_profile, profile_data.data() + 16, sizeof(cached_profile));
            have_cached = true;
            valid = (cached_profile == profile);
            stale_version_header = valid && hdr[1] != Serialization::ShaderBinaryVersion;
        }
    } else if (profile_data.size() == sizeof(Shader::Profile)) {
        std::memcpy(&cached_profile, profile_data.data(), sizeof(cached_profile));
        have_cached = true;
        valid = (cached_profile == profile);
    }
    if (!valid) {
        /* bbport: name the fields that differ. A wipe leaves no trace otherwise, so a cache that
         * is rebuilt on every launch cannot be told apart from one that is merely cold. */
        if (have_cached) {
            const auto* cached = reinterpret_cast<const u8*>(&cached_profile);
            const auto* current = reinterpret_cast<const u8*>(&profile);
            std::string diff;
            for (size_t i = 0; i < sizeof(Shader::Profile); ++i) {
                if (cached[i] != current[i]) {
                    if (!diff.empty()) {
                        diff += ", ";
                    }
                    diff += fmt::format("[{}] {:#04x} -> {:#04x}", i, cached[i], current[i]);
                }
            }
            LOG_WARNING(Render, "Cached shader profile differs from this system: {}", diff);
        } else {
            LOG_WARNING(Render,
                        "Cached shader profile is unreadable ({} bytes, versions or magic differ)",
                        profile_data.size());
        }
        LOG_WARNING(Render, "Pipeline cache isn't compatible with current system: rebuilding it");
        Storage::DataBase::Instance().Clear();
        Storage::DataBase::Instance().FinishPreload();
        SaveProfile();
        return;
    }
    if (stale_version_header) {
        LOG_INFO(Render,
                 "Cached shader profile matches this system; updating its version header to {} "
                 "without rebuilding the cache",
                 Serialization::ShaderBinaryVersion);
        SaveProfile();
    }

    const char* env_warmup = std::getenv("BB_WARMUP_ASYNC");
    const char* env_async = std::getenv("BB_ASYNC_PIPELINES");
    const bool async_disabled = (env_async && env_async[0] == '0') ||
                                (!env_async && BbSettings::Get().async_pipelines.load() == 0);
    const bool warmup_async =
        env_warmup ? (std::strcmp(env_warmup, "0") != 0) : !async_disabled;

    u32 num_pipelines{};
    u32 num_total_pipelines{};
    u32 num_removed{};
    u32 num_reused_permutations{};
    u32 num_corrupt_metas{};

    if (!warmup_async) {
        num_removed = Storage::DataBase::Instance().ForEachBlob(
            Storage::BlobType::PipelineKey,
            [&](std::vector<u8>&& data) -> Storage::BlobAction {
                ++num_total_pipelines;
                try {
                    Serialization::Archive ar{std::move(data)};
                    Serialization::Reader pldata{ar};

                    u32 version{};
                    pldata.Read(version);
                    if (version != Serialization::PipelineKeyVersion) {
                        return Storage::BlobAction::Delete;
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
                    // Keep valid-version keys even if companion blobs are not yet available.
                    return Storage::BlobAction::Keep;
                } catch (const Serialization::CorruptData& e) {
                    LOG_WARNING(Render,
                                "Removed corrupt pipeline key during WarmUp ({}); keeping cache",
                                e.what());
                    return Storage::BlobAction::Delete;
                }
            });
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
        tsl::robin_map<Program*, boost::container::small_vector<size_t, 4>> staged_by_program;

        enum class StageStatus {
            Ok,
            SkipTransient,
            DropPermanent,
        };

        struct CachedWarmUpStage {
            StageStatus status = StageStatus::DropPermanent;
            const Shader::Info* info = nullptr;
            std::shared_future<vk::ShaderModule> future;
            std::optional<Shader::Gcn::FetchShaderData> fetch_shader;
        };
        tsl::robin_map<u64, CachedWarmUpStage> stage_cache;
        stage_cache.reserve(2048);

        auto stage_shader_uncached =
            [&](Serialization::Archive& ar,
                std::optional<Shader::Gcn::FetchShaderData>& out_fetch_shader,
                const Shader::Info*& out_info,
                std::shared_future<vk::ShaderModule>& out_future) -> StageStatus {
            auto program = std::make_unique<Program>();
            Shader::StageSpecialization spec{};
            spec.info = &program->info;
            size_t perm_idx{};
            if (!LoadShaderMeta(ar, program->info, out_fetch_shader, spec, perm_idx)) {
                return StageStatus::DropPermanent;
            }

            std::vector<u32> spv{};
            Storage::DataBase::Instance().Load(
                Storage::BlobType::ShaderBinary,
                fmt::format("{:#018x}_{}", program->info.pgm_hash, perm_idx), spv);
            if (spv.empty() || spv[0] != 0x07230203u) {
                LOG_WARNING(Render_Vulkan, "Cached shader binary {}_{:x}_{} is missing or invalid SPIR-V",
                            program->info.hw_stage, program->info.pgm_hash, perm_idx);
                return StageStatus::DropPermanent;
            }

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

            // A permutation index is only the name of the module in the store; the specialization
            // is the shader's identity. Earlier sessions appended permutations in a different
            // order, so an equal specialization can be staged under another index. Reuse it and
            // keep the pipeline; this used to discard several hundred preloads per launch.
            auto& indices = staged_by_program[program_ptr];
            for (const size_t idx : indices) {
                const auto& p = staged_permutations[idx];
                if (p.spec == spec) {
                    if (p.perm_idx != perm_idx) {
                        ++num_reused_permutations;
                    }
                    out_future = p.future;
                    return StageStatus::Ok;
                }
            }
            for (const size_t idx : indices) {
                if (staged_permutations[idx].perm_idx == perm_idx) {
                    LOG_WARNING(Render_Vulkan,
                                "Cached permutation index {} of {}_{:x} already used with different spec, skipping preload",
                                perm_idx, program_ptr->info.hw_stage, program_ptr->info.pgm_hash);
                    return StageStatus::SkipTransient;
                }
            }

            auto task = std::make_shared<std::packaged_task<vk::ShaderModule()>>(
                [device = instance.GetDevice(), spv_data = std::move(spv)]() {
                    return CompileSPV(spv_data, device);
                });
            out_future = task->get_future().share();
            GetCompiler().Submit([task]() { (*task)(); });
            indices.push_back(staged_permutations.size());
            staged_permutations.push_back(StagedPermut{
                .program = program_ptr,
                .perm_idx = perm_idx,
                .spec = std::move(spec),
                .future = out_future,
            });
            return StageStatus::Ok;
        };

        // bbport: memoize each shader stage by its meta hash before touching disk. Across ~3000
        // cached pipeline keys the same ~1500 metas/spvs are referenced 4x-50x; caching avoids
        // >10,000 redundant NTFS file opens/reads and duplicate SRT walker codegen during WarmUp.
        auto load_stage_cached =
            [&](u64 hash, std::optional<Shader::Gcn::FetchShaderData>& out_fetch_shader,
                const Shader::Info*& out_info,
                std::shared_future<vk::ShaderModule>& out_future) -> StageStatus {
            if (const auto it = stage_cache.find(hash); it != stage_cache.end()) {
                const auto& cached = it->second;
                if (cached.status == StageStatus::Ok) {
                    out_info = cached.info;
                    out_future = cached.future;
                    if (cached.fetch_shader.has_value()) {
                        out_fetch_shader = cached.fetch_shader;
                    }
                }
                return cached.status;
            }
            CachedWarmUpStage entry{};
            std::vector<u8> meta_blob;
            Storage::DataBase::Instance().Load(
                Storage::BlobType::ShaderMeta, fmt::format("{:#018x}", hash), meta_blob);
            if (meta_blob.empty()) {
                stage_cache.emplace(hash, entry);
                return StageStatus::DropPermanent;
            }
            Serialization::Archive meta_ar{std::move(meta_blob)};
            try {
                entry.status =
                    stage_shader_uncached(meta_ar, entry.fetch_shader, entry.info, entry.future);
            } catch (const Serialization::CorruptData& e) {
                RemoveCorruptShaderMeta(hash, e.what());
                ++num_corrupt_metas;
                entry.status = StageStatus::DropPermanent;
            }
            if (entry.status == StageStatus::Ok) {
                out_info = entry.info;
                out_future = entry.future;
                if (entry.fetch_shader.has_value()) {
                    out_fetch_shader = entry.fetch_shader;
                }
            }
            stage_cache.emplace(hash, entry);
            return entry.status;
        };

        num_removed = Storage::DataBase::Instance().ForEachBlob(
            Storage::BlobType::PipelineKey,
            [&](std::vector<u8>&& data) -> Storage::BlobAction {
                ++num_total_pipelines;
                if ((num_total_pipelines & 31u) == 0) {
                    maybe_tick_ui();
                }
                try {
                    Serialization::Archive ar{std::move(data)};
                    Serialization::Reader pldata{ar};

                    u32 version{};
                    pldata.Read(version);
                    if (version != Serialization::PipelineKeyVersion) {
                        return Storage::BlobAction::Delete;
                    }

                    u32 is_compute{};
                    pldata.Read(is_compute);

                    if (is_compute) {
                        ComputePipelineKey key{};
                        key.Deserialize(ar);
                        ComputePipeline::SerializationSupport sdata{};
                        sdata.Deserialize(ar);

                        std::optional<Shader::Gcn::FetchShaderData> fetch_dummy;
                        const Shader::Info* c_info = nullptr;
                        std::shared_future<vk::ShaderModule> c_future;
                        const StageStatus status =
                            load_stage_cached(key.value, fetch_dummy, c_info, c_future);
                        if (status != StageStatus::Ok) {
                            return status == StageStatus::DropPermanent
                                       ? Storage::BlobAction::Delete
                                       : Storage::BlobAction::Keep;
                        }

                        staged_compute.push_back(WarmUpComputeEntry{
                            .key = key,
                            .sdata = sdata,
                            .info = c_info,
                            .future = c_future,
                        });
                        return Storage::BlobAction::Keep;
                    } else {
                        GraphicsPipelineKey key{};
                        key.Deserialize(ar);
                        GraphicsPipeline::SerializationSupport sdata{};
                        sdata.Deserialize(ar);

                        if (key.motion_vectors) {
                            return Storage::BlobAction::Delete;
                        }

                        WarmUpGraphicsEntry entry{};
                        entry.key = key;
                        entry.sdata = sdata;

                        for (int stage_idx = 0; stage_idx < MaxShaderStages; ++stage_idx) {
                            const auto& hash = key.stage_hashes[stage_idx];
                            if (!hash) {
                                continue;
                            }
                            const StageStatus status =
                                load_stage_cached(hash, entry.fetch_shader, entry.infos[stage_idx],
                                                  entry.futures[stage_idx]);
                            if (status != StageStatus::Ok) {
                                return status == StageStatus::DropPermanent
                                           ? Storage::BlobAction::Delete
                                           : Storage::BlobAction::Keep;
                            }
                        }

                        staged_graphics.push_back(std::move(entry));
                        return Storage::BlobAction::Keep;
                    }
                } catch (const Serialization::CorruptData& e) {
                    LOG_WARNING(Render,
                                "Removed corrupt pipeline key during WarmUp ({}); keeping cache",
                                e.what());
                    return Storage::BlobAction::Delete;
                }
            });

        // Between passes: wait for all compilation futures to complete and populate program permutations
        for (auto& item : staged_permutations) {
            while (tick_ui &&
                   item.future.wait_for(std::chrono::milliseconds(16)) != std::future_status::ready) {
                maybe_tick_ui();
            }
            vk::ShaderModule mod = item.future.get();
            if (mod) {
                std::unique_lock lk{programs_mutex};
                if (!item.program->info_template) {
                    item.program->info_template = std::make_unique<Shader::Info>(item.program->info);
                }
                const u64 flat_hash =
                    XXH3_64bits(item.program->info.flattened_ud_buf.data(),
                                item.program->info.flattened_ud_buf.size() * sizeof(u32));
                item.program->InsertPermut(mod, std::move(item.spec), item.perm_idx, flat_hash);
            }
        }

        // Pass 2: pipeline instantiation
        const std::array<Shader::RuntimeInfo, MaxShaderStages> empty_runtime_infos{};
        if (warmup_async) {
            struct ComputeResult {
                ComputePipelineKey key;
                std::unique_ptr<ComputePipeline> pipeline;
            };
            std::vector<std::future<ComputeResult>> compute_futures;
            compute_futures.reserve(staged_compute.size());
            for (auto& entry : staged_compute) {
                if (compute_pipelines.find(entry.key) != compute_pipelines.end()) {
                    continue;
                }
                auto task = std::make_shared<std::packaged_task<ComputeResult()>>(
                    [this, key = entry.key, info = entry.info, future = entry.future,
                     sdata = entry.sdata]() mutable -> ComputeResult {
                        vk::ShaderModule mod = future.get();
                        if (!mod) {
                            return {};
                        }
                        try {
                            auto pl = std::make_unique<ComputePipeline>(
                                instance, scheduler, desc_heap, profile, *pipeline_cache, key,
                                *info, mod, sdata, true);
                            return {key, std::move(pl)};
                        } catch (const std::exception& e) {
                            LOG_WARNING(Render_Vulkan, "Cached compute pipeline failed to create: {}",
                                        e.what());
                            return {};
                        }
                    });
                compute_futures.push_back(task->get_future());
                GetCompiler().Submit([task]() { (*task)(); });
            }

            for (auto& entry : staged_graphics) {
                if (graphics_pipelines.find(entry.key) != graphics_pipelines.end() ||
                    pending_graphics.find(entry.key) != pending_graphics.end()) {
                    continue;
                }
                auto job = std::make_shared<PendingPipeline>();
                job->saved = true;
                job->from_cache = true;
                job->sdata = entry.sdata;
                job->make = [this, key = entry.key, infos = entry.infos,
                             fetch = entry.fetch_shader, futures = entry.futures,
                             sdata = entry.sdata]() mutable -> std::unique_ptr<GraphicsPipeline> {
                    std::array<vk::ShaderModule, MaxShaderStages> modules{};
                    for (size_t s = 0; s < MaxShaderStages; ++s) {
                        if (futures[s].valid()) {
                            modules[s] = futures[s].get();
                            if (!modules[s]) {
                                return nullptr;
                            }
                        }
                    }
                    const std::array<Shader::RuntimeInfo, MaxShaderStages> empty_runtime_infos{};
                    return std::make_unique<GraphicsPipeline>(
                        instance, scheduler, desc_heap, profile, key, *pipeline_cache,
                        infos, empty_runtime_infos, fetch, modules, sdata, true);
                };
                pending_graphics.emplace(entry.key, job);
                ++num_pipelines;
                g_bb_background_pipelines_total.fetch_add(1, std::memory_order_relaxed);
                const size_t key_hash = std::hash<GraphicsPipelineKey>{}(entry.key);
                GetCompiler().SubmitLow([job, key_hash] {
                    SCOPE_EXIT {
                        g_bb_background_pipelines_done.fetch_add(1, std::memory_order_relaxed);
                    };
                    if (job->started.exchange(true)) {
                        return;
                    }
                    std::unique_ptr<GraphicsPipeline> pipeline;
                    try {
                        pipeline = job->make();
                    } catch (const std::exception& e) {
                        LOG_WARNING(Render_Vulkan, "WarmUp graphics pipeline {:#x} failed: {}",
                                    key_hash, e.what());
                    }
                    std::unique_lock lk{job->mutex};
                    job->pipeline = std::move(pipeline);
                    job->make = {};
                    job->done = true;
                    job->ready.store(true, std::memory_order_release);
                    job->cv.notify_all();
                });
            }

            for (auto& fut : compute_futures) {
                while (tick_ui &&
                       fut.wait_for(std::chrono::milliseconds(16)) != std::future_status::ready) {
                    maybe_tick_ui();
                }
                auto res = fut.get();
                if (res.pipeline) {
                    const auto [it, is_new] =
                        compute_pipelines.try_emplace(res.key, std::move(res.pipeline));
                    if (is_new) {
                        ++num_pipelines;
                    }
                }
            }

            // Give the async compiler pool a bounded head-start while the game hasn't booted yet.
            // When boot precompile UI is active, FinishBootPrecompile() immediately drains the
            // remaining pipelines while animating the live progress overlay.
            const u32 warmup_budget_ms = [&] {
                if (tick_ui && !bb_env_off("BB_BOOT_PRECOMPILE")) {
                    return 0u;
                }
                const char* env = std::getenv("BB_WARMUP_BUDGET_MS");
                return env && *env ? u32(std::strtoul(env, nullptr, 10)) : 350u;
            }();
            if (warmup_budget_ms > 0 && !pending_graphics.empty()) {
                const auto drain_deadline =
                    std::chrono::steady_clock::now() + std::chrono::milliseconds(warmup_budget_ms);
                while (!pending_graphics.empty() &&
                       std::chrono::steady_clock::now() < drain_deadline) {
                    RetirePendingGraphics(true);
                    if (pending_graphics.empty()) {
                        break;
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(2));
                }
                RetirePendingGraphics(true);
            }
        } else {
            for (auto& entry : staged_compute) {
                vk::ShaderModule mod = entry.future.get();
                if (!mod) {
                    continue;
                }
                const auto [it, is_new] = compute_pipelines.try_emplace(entry.key);
                if (is_new) {
                    try {
                        it.value() = std::make_unique<ComputePipeline>(
                            instance, scheduler, desc_heap, profile, *pipeline_cache, entry.key,
                            *entry.info, mod, entry.sdata, true);
                    } catch (const std::exception& e) {
                        /* bbport: one driver-refused cached pipeline is skipped, not fatal. */
                        compute_pipelines.erase(entry.key);
                        LOG_WARNING(Render_Vulkan, "Cached compute pipeline failed to create: {}",
                                    e.what());
                        continue;
                    }
                    ++num_pipelines;
                }
            }

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
                    try {
                        it.value() = std::make_unique<GraphicsPipeline>(
                            instance, scheduler, desc_heap, profile, entry.key, *pipeline_cache,
                            entry.infos, empty_runtime_infos, entry.fetch_shader, modules,
                            entry.sdata, true);
                    } catch (const std::exception& e) {
                        /* bbport: one driver-refused cached pipeline is skipped, not fatal. */
                        graphics_pipelines.erase(entry.key);
                        LOG_WARNING(Render_Vulkan,
                                    "Cached graphics pipeline failed to create: {}", e.what());
                        continue;
                    }
                    ++num_pipelines;
                }
            }
        }
    }

    /* bbport: drop metas left by builds/tools that wrote an older binary version. Only scan all
     * metas on disk when the profile header was stale or keys/metas were actually dropped — on a
     * clean warm launch this avoids ~1,500+ redundant NTFS file reads. */
    if (stale_version_header || num_corrupt_metas != 0 || num_removed != 0) {
        PurgeStaleShaderMetas(num_corrupt_metas != 0);
    }

    sel.infos.fill(nullptr);
    sel.modules.fill(nullptr);
    sel.fetch_shader.reset();

    const auto warmup_end = std::chrono::steady_clock::now();
    const auto warmup_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(warmup_end - warmup_start).count();

    LOG_INFO(Render, "Pipeline cache warmup completed in {} ms (preloaded {} pipelines, mode={})",
             warmup_ms, num_pipelines, warmup_async ? "async" : "sync");
    std::printf("Shader cache: warmed up %u/%u pipelines (%zu graphics ready, %zu pending, "
                "%zu compute ready, %zu shader programs, %u dead keys removed) in %lld ms "
                "(mode=%s)\n",
                num_pipelines, num_total_pipelines, graphics_pipelines.size(),
                pending_graphics.size(), compute_pipelines.size(), program_cache.size(),
                num_removed, static_cast<long long>(warmup_ms), warmup_async ? "async" : "sync");
    if (num_total_pipelines > num_pipelines) {
        LOG_WARNING(Render, "{} cached pipelines could not be loaded: removed {} keys, kept {}",
                    num_total_pipelines - num_pipelines, num_removed, num_pipelines);
    }
    if (num_reused_permutations != 0) {
        LOG_INFO(Render,
                 "{} cached shader stages reused a permutation already staged under a different "
                 "index (same specialization, so the preload was kept)",
                 num_reused_permutations);
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
        const auto device = instance.GetDevice();
        vk::Result res{vk::Result::eSuccess};
        std::vector<u8> data;
        // Clone via mergePipelineCaches under lock so getPipelineCacheData does not stall
        // pipeline creation on GPU or worker threads.
        auto [tmp_res, tmp_cache] = device.createPipelineCacheUnique({});
        bool merged = false;
        if (tmp_res == vk::Result::eSuccess && tmp_cache) {
            std::scoped_lock lock{PipelineCreationMutex()};
            merged = (device.mergePipelineCaches(*tmp_cache, {*pipeline_cache}) ==
                      vk::Result::eSuccess);
        }
        if (merged) {
            auto result = device.getPipelineCacheData(*tmp_cache);
            res = result.result;
            data = std::move(result.value);
        } else {
            std::scoped_lock lock{PipelineCreationMutex()};
            auto result = device.getPipelineCacheData(*pipeline_cache);
            res = result.result;
            data = std::move(result.value);
        }
        if (res == vk::Result::eSuccess && !data.empty()) {
            LOG_INFO(Render, "Saving native Vulkan driver pipeline cache ({} bytes)", data.size());
            Storage::DataBase::Instance().Save(Storage::BlobType::PipelineBinary,
                                               GetDriverCacheKey(), std::move(data));
        }
    } catch (const std::exception& e) {
        LOG_WARNING(Render, "Failed to retrieve native Vulkan driver pipeline cache: {}", e.what());
    }
}

void PipelineCache::MaybeSaveDriverPipelineCache() {
    if (num_new_pipelines == num_new_pipelines_at_last_save) {
        return;
    }
    const auto now = std::chrono::steady_clock::now();
    /* The first new pipeline is written out at once (a small, cheap dump) so even an early crash
     * leaves a cache behind; later dumps are spaced out so the .vkp loses at most a minute. */
    if (last_driver_cache_save.time_since_epoch().count() != 0 &&
        now - last_driver_cache_save < std::chrono::minutes(1)) {
        return;
    }
    last_driver_cache_save = now;
    num_new_pipelines_at_last_save = num_new_pipelines;
    /* bbport: this is called from the GPU thread for every draw. Hand the work to a saver thread:
     * getPipelineCacheData() copies the whole driver cache (10+ MB here) and the write/flush used
     * to block the GPU thread, producing a periodic hitch every minute. */
    std::scoped_lock lk{driver_save_mutex};
    if (driver_save_stop) {
        return;
    }
    if (!driver_save_thread.joinable()) {
        driver_save_thread = std::thread([this] { DriverCacheSaveLoop(); });
    }
    driver_save_pending = true;
    driver_save_cv.notify_one();
}

void PipelineCache::DriverCacheSaveLoop() {
    Common::SetCurrentThreadName("bb:DriverCacheSave");
    std::unique_lock lk{driver_save_mutex};
    for (;;) {
        driver_save_cv.wait(lk, [&] { return driver_save_stop || driver_save_pending; });
        if (driver_save_stop) {
            return;
        }
        driver_save_pending = false;
        lk.unlock();
        SaveDriverPipelineCache();
        /* Make this dump durable here, off the GPU thread. The GPU thread only asked for it. */
        Storage::DataBase::Instance().Flush();
        lk.lock();
    }
}

void PipelineCache::StopDriverCacheSaver() {
    {
        std::scoped_lock lk{driver_save_mutex};
        if (!driver_save_thread.joinable()) {
            return;
        }
        driver_save_stop = true;
    }
    driver_save_cv.notify_all();
#ifdef _WIN32
    HANDLE h = (HANDLE)driver_save_thread.native_handle();
    if (h && WaitForSingleObject(h, 150) == WAIT_TIMEOUT) {
        driver_save_thread.detach();
        return;
    }
#endif
    driver_save_thread.join();
}

void PipelineCache::Sync() {
    StopDriverCacheSaver();
    if (compiler) {
        compiler->DiscardPending();
        compiler.reset();
    }
    import.reset();
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
    ar.Reserve(14 + attributes.size() * sizeof(VertexAttribute));

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
