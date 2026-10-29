// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <condition_variable>
#include <deque>
#include <functional>
#include <future>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <shared_mutex>
#include <variant>
#include <boost/container/static_vector.hpp>
#include <tsl/robin_map.h>
#include <filesystem>
#ifdef _WIN32
#include <windows.h>
#endif

namespace Vulkan {
extern std::atomic<u64> g_bb_compile_ns;
extern std::atomic<u32> g_bb_compiles;
extern std::atomic<u32> g_bb_background_pipelines_total;
extern std::atomic<u32> g_bb_background_pipelines_done;
extern std::atomic<bool> g_bb_shader_import_active;
} // namespace Vulkan
#include "shader_recompiler/profile.h"
#include "shader_recompiler/recompiler.h"
#include "shader_recompiler/specialization.h"
#include "video_core/renderer_vulkan/vk_compute_pipeline.h"
#include "video_core/renderer_vulkan/vk_graphics_pipeline.h"
#include "video_core/renderer_vulkan/vk_resource_pool.h"

template <>
struct std::hash<vk::ShaderModule> {
    std::size_t operator()(const vk::ShaderModule& module) const noexcept {
        return std::hash<size_t>{}(reinterpret_cast<size_t>((VkShaderModule)module));
    }
};

namespace AmdGpu {
class Liverpool;
}

namespace Serialization {
struct Archive;
}

namespace Shader {
struct Info;
}

namespace Vulkan {

class Instance;
class Scheduler;
class ShaderCache;

struct Program {
    struct Module {
        vk::ShaderModule module;
        Shader::StageSpecialization spec;
        u64 flat_hash = 0;
    };
    static constexpr size_t MaxPermutations = 8;
    using ModuleList = boost::container::small_vector<Module, MaxPermutations>;

    Shader::Info info;
    ModuleList modules{};
    size_t last_used = 0; ///< bbport: permutation of the previous lookup, compared first
    /// bbport: `info` as translated, for draw-preparation workers (they must not read `info`,
    /// whose user data the GPU thread rewrites every draw). Guarded by programs_mutex.
    std::unique_ptr<Shader::Info> info_template;

    Program() = default;
    Program(Shader::HwStage stage, Shader::SwStage l_stage, Shader::ShaderParams params)
        : info{stage, l_stage, params} {}

    void AddPermut(vk::ShaderModule module, Shader::StageSpecialization&& spec, u64 flat_hash = 0) {
        modules.emplace_back(module, std::move(spec), flat_hash);
    }

    void InsertPermut(vk::ShaderModule module, Shader::StageSpecialization&& spec,
                      size_t perm_idx, u64 flat_hash = 0) {
        modules.resize(std::max(modules.size(), perm_idx + 1)); // <-- beware of realloc
        modules[perm_idx] = {module, std::move(spec), flat_hash};
    }
};

struct DrawIndirectParams {
    u16 vertex_sgpr_offset;
    u32 instance_sgpr_offset;
};

} // namespace Vulkan

namespace AmdGpu {
union Regs;
}

namespace Vulkan {

/// bbport: state of one graphics/compute pipeline selection. The GPU thread owns one
/// (PipelineCache::sel); draw-preparation workers use their own with their register copies.
struct PipelineSelection {
    const AmdGpu::Regs* regs{};
    std::array<Shader::RuntimeInfo, MaxShaderStages> runtime_infos{};
    std::array<const Shader::Info*, MaxShaderStages> infos{};
    std::array<vk::ShaderModule, MaxShaderStages> modules{};
    std::optional<Shader::Gcn::FetchShaderData> fetch_shader{};
    GraphicsPipelineKey graphics_key{};
    bool motion = false;
    DrawIndirectParams draw_indirect_params{};
    struct PrepWorker* worker{}; ///< set: read-only selection for a draw-preparation worker
};

/// bbport: a draw-preparation worker's own program state (see vk_draw_prep.h).
struct PrepWorker {
    struct Stage {
        const Program* program;
        u64 hash;
        Shader::HwStage hw_stage;
        VAddr pgm_base;
        const std::vector<u32>* flat;
    };
    // Node-based: stages keep pointers to these Infos while later stages are inserted.
    std::unordered_map<const Program*, Shader::Info> infos;
    boost::container::static_vector<Stage, MaxShaderStages> stages;
    bool failed = false;
};

struct PreparedDraw;

class PipelineCompiler {
public:
    explicit PipelineCompiler(u32 threads) {
        for (u32 i = 0; i < threads; ++i) {
            workers.emplace_back([this] { Run(); });
        }
    }
    ~PipelineCompiler() {
        {
            std::scoped_lock lk{mutex};
            stop = true;
        }
        cv.notify_all();
        for (auto& worker : workers) {
            worker.join();
        }
    }
    void Submit(std::function<void()> job) {
        {
            std::scoped_lock lk{mutex};
            jobs.push_back(std::move(job));
        }
        cv.notify_one();
    }

private:
    void Run() {
        for (;;) {
            std::function<void()> job;
            {
                std::unique_lock lk{mutex};
                cv.wait(lk, [&] { return stop || !jobs.empty(); });
                if (jobs.empty()) {
                    return; // stopping, every job done
                }
                job = std::move(jobs.front());
                jobs.pop_front();
            }
            job();
        }
    }
    std::mutex mutex;
    std::condition_variable cv;
    std::deque<std::function<void()>> jobs;
    std::vector<std::thread> workers;
    bool stop = false;
};

class PipelineCache {
public:
    explicit PipelineCache(const Instance& instance, Scheduler& scheduler,
                           AmdGpu::Liverpool* liverpool, u32 sparse_page_shift);
    ~PipelineCache();

    void WarmUp();
    void Sync();

    bool LoadComputePipeline(Serialization::Archive& ar);
    bool LoadGraphicsPipeline(Serialization::Archive& ar);
    struct PreloadJob {
        GraphicsPipelineKey key;
        std::function<std::unique_ptr<GraphicsPipeline>()> make;
    };
    bool LoadGraphicsPipeline(Serialization::Archive& ar, std::vector<PreloadJob>& jobs);
    bool LoadPipelineStage(Serialization::Archive& ar, size_t stage);

    /// `indirect`: the draw's arguments are in memory (bbport: never skipped while compiling).
    const GraphicsPipeline* GetGraphicsPipeline(const DrawIndirectParams params = {},
                                                const PreparedDraw* prepared = nullptr,
                                                bool indirect = false);

    /// bbport: worker side of draw preparation: selects the pipeline key for `sel.regs` without
    /// creating anything. False when a program or permutation does not exist yet.
    bool PrepareGraphicsPipeline(PipelineSelection& sel);

    /// bbport: GPU-thread side: the pipeline for a prepared draw after checking that registers
    /// and flattened user data match; null to take the regular path.
    const GraphicsPipeline* TryPreparedPipeline(const PreparedDraw& prepared);

    /// bbport: the prepared draw the last GetGraphicsPipeline used, or null (regular path).
    [[nodiscard]] const PreparedDraw* UsedPrepared() const noexcept {
        return used_prepared;
    }

    const ComputePipeline* GetComputePipeline();

    using Result = std::tuple<const Shader::Info*, vk::ShaderModule,
                              std::optional<Shader::Gcn::FetchShaderData>, u64>;
    Result GetProgram(PipelineSelection& sel, Shader::HwStage stage, Shader::SwStage l_stage,
                      const Shader::ShaderParams& params, Shader::Backend::Bindings& binding);

    std::optional<vk::ShaderModule> ReplaceShader(vk::ShaderModule module,
                                                  std::span<const u32> spv_code);

    static std::string GetShaderName(Shader::HwStage stage, u64 hash,
                                     std::optional<size_t> perm = {});

    auto& GetProfile() const {
        return profile;
    }

private:
    bool RefreshGraphicsKey(PipelineSelection& sel);
    bool RefreshGraphicsStages(PipelineSelection& sel);
    bool RefreshComputeKey();

    void DumpShader(std::span<const u32> code, u64 hash, Shader::HwStage stage, size_t perm_idx,
                    std::string_view ext);
    std::optional<std::vector<u32>> GetShaderPatch(u64 hash, Shader::HwStage stage, size_t perm_idx,
                                                   std::string_view ext);
    vk::ShaderModule CompileModule(Shader::Info& info, Shader::RuntimeInfo& runtime_info,
                                   const std::span<const u32>& code, size_t perm_idx,
                                   Shader::Backend::Bindings& binding);
    const Shader::RuntimeInfo& BuildRuntimeInfo(PipelineSelection& sel, Shader::HwStage stage,
                                                Shader::SwStage l_stage);

    [[nodiscard]] bool IsPipelineCacheDirty() const {
        return num_new_pipelines > 0;
    }

    std::string GetDriverCacheKey() const;
    void SaveDriverPipelineCache();

private:
    const Instance& instance;
    Scheduler& scheduler;
    AmdGpu::Liverpool* liverpool;
    DescriptorHeap desc_heap;
    vk::UniquePipelineCache pipeline_cache;
    vk::UniquePipelineLayout pipeline_layout;
    Shader::Profile profile{};
    Shader::Pools pools;
    tsl::robin_map<size_t, std::unique_ptr<Program>> program_cache;
    /// bbport: exclusive for program/permutation insertions, shared for worker lookups.
    std::shared_mutex programs_mutex;
    u64 prepared_hits = 0, prepared_misses = 0;
    const PreparedDraw* used_prepared = nullptr;
    tsl::robin_map<ComputePipelineKey, std::unique_ptr<ComputePipeline>> compute_pipelines;
    tsl::robin_map<GraphicsPipelineKey, std::unique_ptr<GraphicsPipeline>> graphics_pipelines;
    struct SelectionCache {
        bool valid = false;
        u64 reg_checksum = ~0ull;
        DrawIndirectParams indirect_params{};
        GraphicsPipelineKey key;
        std::array<const Shader::Info*, MaxShaderStages> infos{};
        std::array<vk::ShaderModule, MaxShaderStages> modules{};
        std::optional<Shader::Gcn::FetchShaderData> fetch_shader{};
        bool motion = false;
    };
    SelectionCache selection_cache{};
    PipelineCompiler& GetCompiler();
    PipelineSelection sel{}; ///< GPU thread selection state
    ComputePipelineKey compute_key{};
    u32 num_new_pipelines{}; // new pipelines added to the cache since the game start

    // Only if Config::collectShadersForDebug()
    tsl::robin_map<vk::ShaderModule,
                   std::vector<std::variant<GraphicsPipelineKey, ComputePipelineKey>>>
        module_related_pipelines;

    /// bbport BB_ASYNC_PIPELINES: a new graphics pipeline of a pass drawn every frame is compiled
    /// on worker threads (PipelineCompiler) while its draws go without it for a frame or two,
    /// instead of the game standing still for the driver (18 ms a pipeline on a GTX 1060, 1-4 s
    /// hitches on a first session). Other passes (one-time renders) compile at once, as before.
    struct PendingPipeline {
        std::mutex mutex;
        std::condition_variable cv;
        bool done = false;
        std::unique_ptr<GraphicsPipeline> pipeline;
        GraphicsPipeline::SerializationSupport sdata{};
        std::array<vk::ShaderModule, MaxShaderStages> modules{};
        bool saved = false;
        std::function<std::unique_ptr<GraphicsPipeline>()> make;
        std::atomic<bool> started{false};
    };
    bool AsyncSkippable(const PipelineSelection& sel, bool indirect);
    const GraphicsPipeline* FinishPipeline(const GraphicsPipelineKey& key, PendingPipeline& job);
    tsl::robin_map<GraphicsPipelineKey, std::shared_ptr<PendingPipeline>> pending_graphics;
    u64 async_started = 0, async_skipped = 0, async_waited = 0; ///< statistics
    std::unique_ptr<PipelineCompiler> compiler; ///< last: its threads are joined first

    struct ShaderImport;
    void SaveProfile();
    void StartShaderImport();
#ifdef _WIN32
    static bool LaunchShaderImport(ShaderImport& state);
#endif
    void PollShaderImport() {
        if (import_ready.load(std::memory_order_relaxed)) [[unlikely]] {
            MergeShaderImport(false);
        }
    }
    void MergeShaderImport(bool at_start);
    void MergeImportedProgram(ShaderImport& state, size_t index);
    void AdoptImportedProgram(ShaderImport& state, u64 hash, u64 perms);
    void MergeImportedKey(ShaderImport& state, std::vector<u8>&& bytes);
    void QueueImportedCompute(ShaderImport& state, Serialization::Archive& ar);
    void QueueImportedGraphics(ShaderImport& state, Serialization::Archive& ar);
    std::optional<size_t> AdoptImportedPermutation(std::vector<u8>&& meta, std::vector<u32>&& spv,
                                                   std::vector<u8>&& input);
    std::atomic<bool> import_ready{false};
    std::unique_ptr<ShaderImport> import;
};

struct PipelineCache::ShaderImport {
#ifdef _WIN32
    std::atomic<void*> process{nullptr};
    std::wstring exe, command;
    std::atomic<bool> stopping{false};
#else
    pid_t pid = -1;
#endif
    std::atomic<bool> running{false};
    std::thread watcher;
    bool listed = false, done = false;
    std::vector<std::pair<u64, std::vector<std::filesystem::path>>> programs;
    std::vector<std::filesystem::path> keys;
    size_t next_program = 0, next_key = 0;
    u32 frame = ~0u;
    u32 merged = 0, skipped = 0, queued = 0, compute = 0, adopted = 0;
    std::unordered_map<u64, u64> remap;
    std::chrono::steady_clock::time_point start;
    std::mutex compute_mutex;
    std::vector<std::pair<ComputePipelineKey, std::unique_ptr<ComputePipeline>>> compute_made;
    std::atomic<u32> compute_queued{0};

    ~ShaderImport() {
#ifdef _WIN32
        stopping = true;
        if (void* handle = process.load(); running.load() && handle) {
            TerminateProcess(static_cast<HANDLE>(handle), 1);
        }
#else
        if (running.load()) {
            kill(pid, SIGTERM);
        }
#endif
        if (watcher.joinable()) {
            watcher.join();
        }
    }
};

} // namespace Vulkan
