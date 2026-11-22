// SPDX-FileCopyrightText: Copyright 2024-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <ranges>
#include <string>
#include <unordered_set>

#include "bbport_settings.h"
#include "bbport_threads.h"
#include "bbport_toggles.h"

#include "common/hash.h"
#include "common/io_file.h"
#include "common/path_util.h"
#include "common/scope_exit.h"
#include "core/debug_state.h"
#include "core/emulator_settings.h"
#include "shader_recompiler/backend/spirv/emit_spirv.h"
#include "shader_recompiler/info.h"
#include "shader_recompiler/recompiler.h"
#include "shader_recompiler/runtime_info.h"
#include "video_core/amdgpu/liverpool.h"
#include "video_core/cache_storage.h"
#include "video_core/renderer_vulkan/liverpool_to_vk.h"
#include "video_core/renderer_vulkan/motion_history.h"
#include "video_core/renderer_vulkan/vk_draw_prep.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_pipeline_serialization.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_shader_util.h"
#include "video_core/renderer_vulkan/vk_shader_capture.h"
#include "video_core/renderer_vulkan/vk_shader_hle.h"

namespace Vulkan {

using Shader::HwStage;
using Shader::Output;
using Shader::SwStage;

std::mutex& PipelineCreationMutex() {
    static std::mutex mutex;
    return mutex;
}

constexpr static auto SpirvVersion1_6 = 0x00010600U;

constexpr static std::array DescriptorHeapSizes = {
    vk::DescriptorPoolSize{vk::DescriptorType::eUniformBuffer, 2048},
    vk::DescriptorPoolSize{vk::DescriptorType::eStorageBuffer, 16384},
    vk::DescriptorPoolSize{vk::DescriptorType::eSampledImage, 16384},
    vk::DescriptorPoolSize{vk::DescriptorType::eStorageImage, 2048},
    vk::DescriptorPoolSize{vk::DescriptorType::eSampler, 2048},
};

static u32 MapOutputs(std::span<Shader::OutputMap, 3> outputs, const AmdGpu::VsOutputControl& ctl) {
    u32 num_outputs = 0;

    if (ctl.vs_out_misc_enable) {
        auto& misc_vec = outputs[num_outputs++];
        misc_vec[0] = ctl.use_vtx_point_size ? Output::PointSize : Output::None;
        misc_vec[1] = ctl.use_vtx_edge_flag
                          ? Output::EdgeFlag
                          : (ctl.use_vtx_gs_cut_flag ? Output::GsCutFlag : Output::None);
        misc_vec[2] =
            ctl.use_vtx_kill_flag
                ? Output::KillFlag
                : (ctl.use_vtx_render_target_idx ? Output::RenderTargetIndex : Output::None);
        misc_vec[3] = ctl.use_vtx_viewport_idx ? Output::ViewportIndex : Output::None;
    }

    if (ctl.vs_out_ccdist0_enable) {
        auto& ccdist0 = outputs[num_outputs++];
        ccdist0[0] = ctl.IsClipDistEnabled(0)
                         ? Output::ClipDist0
                         : (ctl.IsCullDistEnabled(0) ? Output::CullDist0 : Output::None);
        ccdist0[1] = ctl.IsClipDistEnabled(1)
                         ? Output::ClipDist1
                         : (ctl.IsCullDistEnabled(1) ? Output::CullDist1 : Output::None);
        ccdist0[2] = ctl.IsClipDistEnabled(2)
                         ? Output::ClipDist2
                         : (ctl.IsCullDistEnabled(2) ? Output::CullDist2 : Output::None);
        ccdist0[3] = ctl.IsClipDistEnabled(3)
                         ? Output::ClipDist3
                         : (ctl.IsCullDistEnabled(3) ? Output::CullDist3 : Output::None);
    }

    if (ctl.vs_out_ccdist1_enable) {
        auto& ccdist1 = outputs[num_outputs++];
        ccdist1[0] = ctl.IsClipDistEnabled(4)
                         ? Output::ClipDist4
                         : (ctl.IsCullDistEnabled(4) ? Output::CullDist4 : Output::None);
        ccdist1[1] = ctl.IsClipDistEnabled(5)
                         ? Output::ClipDist5
                         : (ctl.IsCullDistEnabled(5) ? Output::CullDist5 : Output::None);
        ccdist1[2] = ctl.IsClipDistEnabled(6)
                         ? Output::ClipDist6
                         : (ctl.IsCullDistEnabled(6) ? Output::CullDist6 : Output::None);
        ccdist1[3] = ctl.IsClipDistEnabled(7)
                         ? Output::ClipDist7
                         : (ctl.IsCullDistEnabled(7) ? Output::CullDist7 : Output::None);
    }

    return num_outputs;
}

const Shader::RuntimeInfo& PipelineCache::BuildRuntimeInfo(PipelineSelection& sel, HwStage stage,
                                                             SwStage l_stage) {
    auto& info = sel.runtime_infos[u32(l_stage)];
    const auto& regs = (*sel.regs);
    const auto BuildCommon = [&](const auto& program) {
        info.props.num_user_data = program.settings.num_user_regs;
        info.props.num_input_vgprs = program.settings.vgpr_comp_cnt;
        info.props.num_allocated_vgprs = program.NumVgprs();
        info.props.fp_denorm_mode32 = program.settings.fp_denorm_mode32;
        info.props.fp_denorm_mode16_64 = program.settings.fp_denorm_mode64;
        info.props.fp_round_mode32 = program.settings.fp_round_mode32;
        info.props.fp_round_mode16_64 = program.settings.fp_round_mode64;
    };
    info.Initialize(stage, l_stage);
    switch (stage) {
    case HwStage::Local: {
        BuildCommon(regs.ls_program);
        // bbport: the Local stage is only meaningful alongside a bound hull shader; its
        // tessellation constants come from the hull Info. RefreshGraphicsStages defers the draw
        // when the hull program is missing or still compiling, so a null here is unexpected and
        // must not be dereferenced.
        info.hw.ls.ls_stride = 0;
        if (const auto* hull_info = sel.infos[u32(SwStage::TessellationControl)]) {
            Shader::TessellationDataConstantBuffer tess_constants{};
            hull_info->ReadTessConstantBuffer(tess_constants);
            info.hw.ls.ls_stride = tess_constants.ls_stride;
        }
        break;
    }
    case HwStage::Hull:
        BuildCommon(regs.hs_program);
        break;
    case HwStage::Export:
        BuildCommon(regs.es_program);
        info.hw.es.vertex_data_size = regs.vgt_esgs_ring_itemsize;
        break;
    case HwStage::Geometry: {
        BuildCommon(regs.gs_program);
        info.hw.gs.num_outputs = MapOutputs(info.hw.gs.outputs, regs.vs_output_control);
        info.hw.gs.output_vertices = regs.vgt_gs_max_vert_out;
        info.hw.gs.num_invocations =
            regs.vgt_gs_instance_cnt.IsEnabled() ? regs.vgt_gs_instance_cnt.count : 1;
        if (regs.stage_enable.raw == AmdGpu::ShaderStageEnable::LsHsEsGs) {
            info.hw.gs.in_primitive = [&]() {
                switch (regs.tess_config.topology) {
                case AmdGpu::TessellationTopology::Point:
                    return AmdGpu::PrimitiveType::PointList;
                case AmdGpu::TessellationTopology::Line:
                    return AmdGpu::PrimitiveType::LineList;
                case AmdGpu::TessellationTopology::TriangleCw:
                case AmdGpu::TessellationTopology::TriangleCcw:
                    return AmdGpu::PrimitiveType::TriangleList;
                default:
                    UNREACHABLE();
                }
            }();
        } else {
            info.hw.gs.in_primitive = regs.primitive_type;
        }
        for (u32 stream_id = 0; stream_id < Shader::GsMaxOutputStreams; ++stream_id) {
            info.hw.gs.out_primitive[stream_id] =
                regs.vgt_gs_out_prim_type.GetPrimitiveType(stream_id);
        }
        info.hw.gs.in_vertex_data_size = regs.vgt_esgs_ring_itemsize;
        info.hw.gs.out_vertex_data_size = regs.vgt_gs_vert_itemsize[0];
        info.hw.gs.mode = regs.vgt_gs_mode.mode;
        const auto params_vc = AmdGpu::GetParams(regs.vs_program);
        info.hw.gs.vs_copy = params_vc.code;
        info.hw.gs.vs_copy_hash = params_vc.hash;
        DumpShader(info.hw.gs.vs_copy, info.hw.gs.vs_copy_hash, Shader::HwStage::Vertex, 0,
                   "copy.bin");
        break;
    }
    case HwStage::Vertex: {
        BuildCommon(regs.vs_program);
        info.hw.vs.user_clip_plane_mask = regs.clipper_control.user_clip_plane_enable;
        info.hw.vs.num_outputs = MapOutputs(info.hw.vs.outputs, regs.vs_output_control);
        info.hw.vs.emulate_depth_negative_one_to_one =
            !instance.IsDepthClipControlSupported() &&
            regs.clipper_control.clip_space == AmdGpu::ClipSpace::MinusWToW;
        info.hw.vs.clip_disable = regs.IsClipDisabled();
        info.hw.vs.motion_vectors = sel.motion;
        break;
    }
    case HwStage::Fragment: {
        BuildCommon(regs.ps_program);
        info.hw.fs.en_flags = regs.ps_input_ena;
        info.hw.fs.addr_flags = regs.ps_input_addr;
        info.hw.fs.num_inputs = regs.num_interp;
        info.hw.fs.front_face_all_bits = regs.barycentric_control.front_face_all_bits;
        info.hw.fs.depth_before_shader = regs.depth_shader_control.depth_before_shader != 0;
        info.hw.fs.num_samples =
            regs.ps_input_addr.sample_coverage_ena && regs.ps_input_ena.sample_coverage_ena
                ? regs.aa_config.NumSamples()
                : 1;
        info.hw.fs.z_export_format = regs.z_export_format;
        info.hw.fs.motion_vectors = sel.motion;
        u8 stencil_ref_export_enable = regs.depth_shader_control.stencil_op_val_export_enable |
                                       regs.depth_shader_control.stencil_test_val_export_enable;
        info.hw.fs.mrtz_mask = regs.depth_shader_control.z_export_enable |
                               (stencil_ref_export_enable << 1) |
                               (regs.depth_shader_control.mask_export_enable << 2) |
                               (regs.depth_shader_control.coverage_to_mask_enable << 3);
        const auto& cb0_blend = regs.blend_control[0];
        if (cb0_blend.enable) {
            info.hw.fs.dual_source_blending =
                LiverpoolToVK::IsDualSourceBlendFactor(cb0_blend.color_dst_factor) ||
                LiverpoolToVK::IsDualSourceBlendFactor(cb0_blend.color_src_factor);
            if (cb0_blend.separate_alpha_blend) {
                info.hw.fs.dual_source_blending |=
                    LiverpoolToVK::IsDualSourceBlendFactor(cb0_blend.alpha_dst_factor) ||
                    LiverpoolToVK::IsDualSourceBlendFactor(cb0_blend.alpha_src_factor);
            }
        } else {
            info.hw.fs.dual_source_blending = false;
        }
        const auto& ps_inputs = regs.ps_inputs;
        for (u32 i = 0; i < regs.num_interp; i++) {
            info.hw.fs.inputs[i] = {
                .param_index = u8(ps_inputs[i].input_offset),
                .is_default = bool(ps_inputs[i].use_default),
                .is_flat = bool(ps_inputs[i].flat_shade),
                .default_value = u8(ps_inputs[i].default_value),
            };
        }
        for (u32 i = 0; i < Shader::MaxColorBuffers; i++) {
            info.hw.fs.color_buffers[i] = sel.graphics_key.color_buffers[i];
        }
        // Lowered user clip planes ride the same emulation path as guest-exported distances, so
        // the fragment side arms whenever the hardware vertex stage lowers them, keeping its input
        // locations in sync with the shifted vertex outputs.
        const bool lowers_user_clip_planes =
            regs.clipper_control.user_clip_plane_enable &&
            !regs.stage_enable.IsStageEnabled(static_cast<u32>(HwStage::Geometry));
        info.hw.fs.clip_distance_emulation =
            ((regs.vs_output_control.clip_distance_enable &&
              !regs.stage_enable.IsStageEnabled(static_cast<u32>(HwStage::Local))) ||
             lowers_user_clip_planes) &&
            profile.needs_clip_distance_emulation;
        break;
    }
    case HwStage::Compute: {
        const auto& cs_pgm = liverpool->GetCsRegs();
        info.props.num_user_data = cs_pgm.settings.num_user_regs;
        info.props.num_allocated_vgprs = cs_pgm.settings.num_vgprs * 4;
        info.props.fp_denorm_mode32 = cs_pgm.settings.fp_denorm_mode32;
        info.props.fp_denorm_mode16_64 = cs_pgm.settings.fp_denorm_mode64;
        info.props.fp_round_mode32 = cs_pgm.settings.fp_round_mode32;
        info.props.fp_round_mode16_64 = cs_pgm.settings.fp_round_mode64;
        info.hw.cs.workgroup_size = {cs_pgm.num_thread_x.full, cs_pgm.num_thread_y.full,
                                     cs_pgm.num_thread_z.full};
        info.hw.cs.tgid_enable = {cs_pgm.IsTgidEnabled(0), cs_pgm.IsTgidEnabled(1),
                                  cs_pgm.IsTgidEnabled(2)};
        info.hw.cs.shared_memory_size = cs_pgm.SharedMemSize();
        break;
    }
    default:
        break;
    }
    switch (l_stage) {
    case SwStage::Vertex:
        info.sw.vs.step_rate_0 = regs.vgt_instance_step_rate_0;
        info.sw.vs.step_rate_1 = regs.vgt_instance_step_rate_1;
        info.sw.vs.vertex_sgpr_offset = sel.draw_indirect_params.vertex_sgpr_offset;
        info.sw.vs.instance_sgpr_offset = sel.draw_indirect_params.instance_sgpr_offset;
        info.sw.vs.tess_emulated_primitive =
            regs.primitive_type == AmdGpu::PrimitiveType::RectList ||
            regs.primitive_type == AmdGpu::PrimitiveType::QuadList;
        break;
    case SwStage::TessellationControl: {
        info.sw.tcs.num_input_control_points = regs.ls_hs_config.hs_input_control_points;
        info.sw.tcs.num_threads = regs.ls_hs_config.hs_output_control_points;
        info.sw.tcs.tess_type = regs.tess_config.type;
        info.sw.tcs.offchip_lds_enable = regs.hs_program.settings.oc_lds_en;
        break;
    }
    case SwStage::TessellationEval: {
        info.sw.tes.tess_type = regs.tess_config.type;
        info.sw.tes.tess_topology = regs.tess_config.topology;
        info.sw.tes.tess_partitioning = regs.tess_config.partitioning;
        break;
    }
    default:
        break;
    }
    return info;
}

PipelineCache::PipelineCache(const Instance& instance_, Scheduler& scheduler_,
                             AmdGpu::Liverpool* liverpool_, u32 sparse_page_shift)
    : instance{instance_}, scheduler{scheduler_}, liverpool{liverpool_},
      desc_heap{instance, scheduler.GetWorkSemaphore(), DescriptorHeapSizes} {
    sel.regs = &liverpool->regs;
    const auto& vk12_props = instance.GetVk12Properties();
    profile = Shader::Profile{
        .max_viewport_width = instance.GetMaxViewportWidth(),
        .max_viewport_height = instance.GetMaxViewportHeight(),
        .max_shared_memory_size = instance.MaxComputeSharedMemorySize(),
        .supported_spirv = SpirvVersion1_6,
        .subgroup_size = instance.SubgroupSize(),
        .sparse_page_shift = sparse_page_shift,
        .support_int8 = instance.IsShaderInt8Supported(),
        .support_int16 = instance.IsShaderInt16Supported(),
        .support_int64 = instance.IsShaderInt64Supported(),
        .support_float16 = instance.IsShaderFloat16Supported(),
        .support_float64 = instance.IsShaderFloat64Supported(),
        .supports_denorm_behavior_independence =
            vk12_props.denormBehaviorIndependence != vk::ShaderFloatControlsIndependence::eNone,
        .supports_rounding_mode_independence =
            vk12_props.roundingModeIndependence != vk::ShaderFloatControlsIndependence::eNone,
        .support_fp16_denorm_preserve = bool(vk12_props.shaderDenormPreserveFloat16),
        .support_fp16_denorm_flush = bool(vk12_props.shaderDenormFlushToZeroFloat16),
        .support_fp16_round_to_zero = bool(vk12_props.shaderRoundingModeRTZFloat16),
        .support_fp32_denorm_preserve = bool(vk12_props.shaderDenormPreserveFloat32),
        .support_fp32_denorm_flush = bool(vk12_props.shaderDenormFlushToZeroFloat32),
        .support_fp32_round_to_zero = bool(vk12_props.shaderRoundingModeRTZFloat32),
        .support_fp64_denorm_preserve = bool(vk12_props.shaderDenormPreserveFloat64),
        .support_fp64_denorm_flush = bool(vk12_props.shaderDenormFlushToZeroFloat64),
        .support_fp64_round_to_zero = bool(vk12_props.shaderRoundingModeRTZFloat64),
        .support_fp16_signed_zero_inf_nan_preserve =
            bool(vk12_props.shaderSignedZeroInfNanPreserveFloat16),
        .support_fp32_signed_zero_inf_nan_preserve =
            bool(vk12_props.shaderSignedZeroInfNanPreserveFloat32),
        .support_fp64_signed_zero_inf_nan_preserve =
            bool(vk12_props.shaderSignedZeroInfNanPreserveFloat64),
        .supports_image_load_store_lod = instance_.IsImageLoadStoreLodSupported(),
        .supports_native_cube_calc = instance_.IsAmdGcnShaderSupported(),
        .supports_trinary_minmax = instance_.IsAmdShaderTrinaryMinMaxSupported(),
        .supports_buffer_fp32_atomic_min_max =
            instance_.IsShaderAtomicFloatBuffer32MinMaxSupported(),
        .supports_image_fp32_atomic_min_max = instance_.IsShaderAtomicFloatImage32MinMaxSupported(),
        .supports_buffer_int64_atomics = instance_.IsBufferInt64AtomicsSupported(),
        .supports_shared_int64_atomics = instance_.IsSharedInt64AtomicsSupported(),
        .supports_workgroup_explicit_memory_layout =
            instance_.IsWorkgroupMemoryExplicitLayoutSupported(),
        .supports_amd_shader_explicit_vertex_parameter =
            instance_.IsAmdShaderExplicitVertexParameterSupported(),
        .supports_fragment_shader_barycentric = instance_.IsFragmentShaderBarycentricSupported(),
        .supports_shader_subgroup_clock = instance_.IsShaderSubgroupClockSupported(),
        .needs_manual_interpolation = instance.IsFragmentShaderBarycentricSupported() &&
                                      instance.GetDriverID() == vk::DriverId::eNvidiaProprietary,
        // bbport: older NVIDIA (Pascal) has no barycentrics; BB_INTERP_INT_FIX=0/1 overrides.
        .needs_integer_interpolation_fix = [&] {
            if (const char* env = std::getenv("BB_INTERP_INT_FIX")) {
                return env[0] == '1';
            }
            return !instance.IsFragmentShaderBarycentricSupported() &&
                   instance.GetDriverID() == vk::DriverId::eNvidiaProprietary;
        }(),
        .needs_lds_barriers = instance.GetDriverID() == vk::DriverId::eNvidiaProprietary ||
                              instance.GetDriverID() == vk::DriverId::eMesaKosmickrisp,
        .needs_buffer_offsets = instance.StorageMinAlignment() > 4,
        .needs_unorm_fixup = instance.GetDriverID() == vk::DriverId::eMesaKosmickrisp,
        .needs_clip_distance_emulation = instance.GetDriverID() == vk::DriverId::eNvidiaProprietary,
        .supports_shader_stencil_export = instance_.IsShaderStencilExportSupported(),
        .fast_shader_math = EmulatorSettings.IsFastShaderMath(),
    };
    std::vector<u8> driver_cache_data{};
    if (EmulatorSettings.IsPipelineCacheEnabled()) {
        auto& db = Storage::DataBase::Instance();
        db.Open();
        if (!EmulatorSettings.IsPipelineCacheArchived()) {
            db.EnsureEpoch(PipelineStore::CacheEpoch);
        }
        const std::string driver_cache_key = GetDriverCacheKey();
        db.Load(Storage::BlobType::PipelineBinary, driver_cache_key, driver_cache_data);
        if (!driver_cache_data.empty()) {
            LOG_INFO(Render, "Loading native Vulkan driver pipeline cache ({} bytes)",
                     driver_cache_data.size());
            std::printf("Shader cache: native driver cache %zu bytes (%s)\n",
                        driver_cache_data.size(), driver_cache_key.c_str());
        } else {
            std::printf("Shader cache: native driver cache empty (%s)\n", driver_cache_key.c_str());
        }
    }

    const vk::PipelineCacheCreateInfo cache_ci{
        .initialDataSize = driver_cache_data.size(),
        .pInitialData = driver_cache_data.empty() ? nullptr : driver_cache_data.data(),
    };
    auto [cache_result, cache] = instance.GetDevice().createPipelineCacheUnique(cache_ci);
    if (cache_result != vk::Result::eSuccess) {
        LOG_WARNING(Render, "Failed to create pipeline cache from stored driver data ({}): recreating empty",
                    vk::to_string(cache_result));
        auto [empty_res, empty_cache] = instance.GetDevice().createPipelineCacheUnique({});
        if (empty_res != vk::Result::eSuccess) {
            /* bbport: VK_NULL_HANDLE is a legal pipeline cache, so pipeline creation can go on
             * without one (no driver-side caching) instead of exiting the process. */
            LOG_CRITICAL(Render,
                         "Failed to create empty pipeline cache ({}); continuing without a "
                         "driver pipeline cache",
                         vk::to_string(empty_res));
        } else {
            cache = std::move(empty_cache);
        }
    }
    pipeline_cache = std::move(cache);
}

PipelineCompiler& PipelineCache::GetCompiler() {
    if (!compiler) {
        const char* env_threads = std::getenv("BB_ASYNC_PIPELINE_THREADS");
        const char* env_async = std::getenv("BB_ASYNC_PIPELINES");
        const int cfg_val = BbSettings::Get().async_pipelines.load();
        const u32 concurrency = std::thread::hardware_concurrency();
        const u32 default_threads = concurrency <= 2 ? 1u : std::clamp(concurrency / 2, 2u, 8u);
        u32 threads = default_threads;
        if (env_threads && *env_threads) {
            threads = u32(std::strtoul(env_threads, nullptr, 10));
        } else if (env_async && *env_async && env_async[0] != '0') {
            const u32 parsed = u32(std::strtoul(env_async, nullptr, 10));
            if (parsed >= 1) {
                threads = parsed;
            }
        } else if (cfg_val > 0) {
            threads = u32(cfg_val);
        }
        compiler = std::make_unique<PipelineCompiler>(std::max(threads, 1u));
        std::printf("GPU: async pipeline compiler active on %u worker threads (BB_ASYNC_PIPELINES=0: disable)\n",
                    std::max(threads, 1u));
    }
    return *compiler;
}



PipelineCache::~PipelineCache() {
    // bbport: the saver thread and worker compiler touch the device and cache state; join both
    // before tearing down import state, pipelines, or cached HLE/aux Vulkan handles.
    StopDriverCacheSaver();
    if (compiler) {
        compiler->DiscardPending();
        compiler.reset();
    }
    pending_programs.clear();
    pending_permutations.clear();
    import.reset();
    graphics_pipelines.clear();
    compute_pipelines.clear();
    const vk::Device device = instance.GetDevice();
    for (const auto& [_, program] : program_cache) {
        for (const auto& mod : program->modules) {
            if (mod.module) {
                device.destroyShaderModule(mod.module);
            }
        }
    }
    program_cache.clear();
    ClearHleShaderCaches();
    ClearAuxShaderCaches();
    ClearPipelineLayoutCache();
}

// bbport: shader/pipeline compile time on the GPU thread, reported by BB_FRAME_STATS.
std::atomic<u64> g_bb_compile_ns;
std::atomic<u32> g_bb_compiles;
std::atomic<u32> g_bb_background_pipelines_total{0};
std::atomic<u32> g_bb_background_pipelines_done{0};
std::atomic<bool> g_bb_shader_import_active{false};
std::atomic<u32> g_bb_recipe_import_done{0};
std::atomic<u32> g_bb_recipe_import_total{0};
std::atomic<u32> g_bb_boot_compile_done{0};
std::atomic<u32> g_bb_boot_compile_total{0};
std::atomic<bool> g_bb_boot_precompile_active{false};
std::atomic<u32> g_bb_compiles_total{0};
namespace {
struct CompileTimer {
    std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
    ~CompileTimer() {
        g_bb_compile_ns += u64(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                   std::chrono::steady_clock::now() - start)
                                   .count());
        ++g_bb_compiles;
        g_bb_compiles_total.fetch_add(1, std::memory_order_relaxed);
    }
};

/// bbport BB_ASYNC_PIPELINES: per render target (its address), how many frames in a row draws
/// went to it. Only the GPU thread (pipeline selection) touches it.
struct TargetStreak {
    u32 last_frame = ~0u;
    u32 frames = 0;
};
tsl::robin_map<u64, TargetStreak> target_streaks = [] {
    tsl::robin_map<u64, TargetStreak> map;
    map.reserve(128);
    return map;
}();

/// bbport BB_ASYNC_PIPELINES: global draw accounting for active gameplay detection.
static u32 g_frame_seen = ~0u;
static u64 g_draws_at_start = 0;
static u64 g_last_frame_draws = 0;

inline void UpdateFrameDrawCount() {
    const u32 frame = BbStats::frame_number.load(std::memory_order_relaxed);
    const u64 draws = BbStats::draws.load(std::memory_order_relaxed);
    if (frame != g_frame_seen) {
        g_last_frame_draws = draws - g_draws_at_start;
        g_draws_at_start = draws;
        g_frame_seen = frame;
    }
}

/// The draw's render target: its first color buffer written, else its depth buffer, else 0.
u64 DrawTarget(const AmdGpu::Regs& regs) {
    if (regs.color_control.mode != AmdGpu::ColorControl::OperationMode::Disable) {
        for (u32 cb = 0; cb < AmdGpu::NUM_COLOR_BUFFERS; ++cb) {
            if (regs.color_buffers[cb] && regs.color_target_mask.GetMask(cb)) {
                return regs.color_buffers[cb].Address();
            }
        }
    }
    return regs.depth_buffer.DepthValid() ? regs.depth_buffer.DepthAddress() : 0;
}

/// Counts the frames in a row a target is drawn to (once per frame and target).
void NoteDrawTarget(const AmdGpu::Regs& regs) {
    UpdateFrameDrawCount();
    static u64 last_target = 0;
    static u32 last_frame = ~0u;
    const u32 frame = BbStats::frame_number.load(std::memory_order_relaxed);
    const u64 target = DrawTarget(regs);
    if (target == 0 || (target == last_target && frame == last_frame)) {
        return;
    }
    last_target = target;
    last_frame = frame;
    auto& streak = target_streaks[target];
    if (streak.last_frame != frame) {
        streak.frames = streak.last_frame + 1 == frame ? streak.frames + 1 : 1;
        streak.last_frame = frame;
    }
}
} // namespace

bool PipelineCache::PrepareGraphicsPipeline(PipelineSelection& worker_sel) {
    // Tessellation stages read constant buffers from memory at selection time: not prepared.
    if (worker_sel.regs->stage_enable.hs_en) {
        return false;
    }
    return RefreshGraphicsKey(worker_sel) && !worker_sel.worker->failed;
}

const GraphicsPipeline* PipelineCache::TryPreparedPipeline(const PreparedDraw& prepared) {
    if (prepared.state.load(std::memory_order_acquire) != PreparedDraw::Ready ||
        prepared.reg_checksum != liverpool->gfx_reg_checksum) {
        return nullptr;
    }
    // Same registers; the stage programs and their flattened user data (which also covers
    // everything read from guest memory for the specialization) must match as well. This is
    // the per-stage work GetProgram does on the regular path.
    const auto& regs = liverpool->regs;
    for (u32 i = 0; i < prepared.num_stages; ++i) {
        const auto& stage = prepared.stages[i];
        const auto* pgm = regs.ProgramForStage(static_cast<u32>(stage.hw_stage));
        if (!pgm || !pgm->Address<u32*>()) {
            return nullptr;
        }
        const auto params = AmdGpu::GetParams(*pgm);
        if (params.hash != stage.hash) {
            return nullptr;
        }
        auto& info = const_cast<Program*>(stage.program)->info;
        info.pgm_base = params.Base();
        info.user_data = params.user_data;
        info.RefreshFlatBuf();
        if (info.pgm_base != stage.pgm_base || info.flattened_ud_buf.size() != stage.flat_size) {
            return nullptr;
        }
        if (!BbToggle::Disabled(BbToggle::FlatHashValidation) && stage.flat_hash != 0) {
            const u64 computed_hash = XXH3_64bits(info.flattened_ud_buf.data(),
                                                  info.flattened_ud_buf.size() * sizeof(u32));
            if (computed_hash != stage.flat_hash) {
                return nullptr;
            }
        }
        if (
            std::memcmp(info.flattened_ud_buf.data(), stage.flat,
                        stage.flat_size * sizeof(u32)) != 0) {
            return nullptr;
        }
    }
    const auto it = graphics_pipelines.find(prepared.key);
    return it != graphics_pipelines.end() ? it->second.get() : nullptr;
}

void PipelineCache::RetirePendingGraphics(bool force) {
    if (pending_graphics.empty()) {
        return;
    }
    static thread_local u32 last_retire_frame = ~0u;
    const u32 frame = BbStats::frame_number.load(std::memory_order_relaxed);
    if (!force && frame == last_retire_frame) {
        return;
    }
    last_retire_frame = frame;
    boost::container::small_vector<std::pair<GraphicsPipelineKey, std::shared_ptr<PendingPipeline>>, 8>
        completed;
    for (const auto& [key, job] : pending_graphics) {
        if (job->ready.load(std::memory_order_acquire)) {
            completed.emplace_back(key, job);
        }
    }
    for (const auto& [key, job] : completed) {
        pending_graphics.erase(key);
        FinishPipeline(key, *job);
    }
}

const GraphicsPipeline* PipelineCache::GetGraphicsPipeline(const DrawIndirectParams params,
                                                           const PreparedDraw* prepared,
                                                           bool indirect) {
    PollShaderImport();
    RetirePendingGraphics();
    MaybeSaveDriverPipelineCache();
    used_prepared = nullptr;
    NoteDrawTarget(liverpool->regs);
    if (prepared) {
        if (const auto* pipeline = TryPreparedPipeline(*prepared)) {
            used_prepared = prepared;
            return pipeline;
        }
    }
    bool hit = false;
    if (!BbToggle::Disabled(BbToggle::SelectionCache) && selection_cache.valid &&
        selection_cache.reg_checksum == liverpool->gfx_reg_checksum &&
        selection_cache.indirect_params.vertex_sgpr_offset == params.vertex_sgpr_offset &&
        selection_cache.indirect_params.instance_sgpr_offset == params.instance_sgpr_offset) {
        sel.graphics_key = selection_cache.key;
        sel.infos = selection_cache.infos;
        sel.modules = selection_cache.modules;
        sel.fetch_shader = selection_cache.fetch_shader;
        sel.motion = selection_cache.motion;
        sel.draw_indirect_params = params;
        hit = true;
    }

    if (!hit) {
        sel.draw_indirect_params = params;
        if (!RefreshGraphicsKey(sel)) {
            selection_cache.valid = false;
            return nullptr;
        }
        if (!BbToggle::Disabled(BbToggle::SelectionCache)) {
            selection_cache.valid = true;
            selection_cache.reg_checksum = liverpool->gfx_reg_checksum;
            selection_cache.indirect_params = params;
            selection_cache.key = sel.graphics_key;
            selection_cache.infos = sel.infos;
            selection_cache.modules = sel.modules;
            selection_cache.fetch_shader = sel.fetch_shader;
            selection_cache.motion = sel.motion;
        }
    }

    if (const auto found = graphics_pipelines.find(sel.graphics_key); found != graphics_pipelines.end()) {
        return found->second.get();
    }
    // bbport BB_ASYNC_PIPELINES: being compiled on a worker, or to be.
    const bool skippable = AsyncSkippable(sel, indirect);
    /* bbport: a pipeline the driver refused `MaxPipelineFailures` times is not attempted again
     * (the failure was logged then). Its draws are dropped, as they already were while it was
     * pending; the rest of the frame keeps rendering. */
    if (const auto failed = graphics_failures.find(sel.graphics_key);
        failed != graphics_failures.end() && failed->second >= MaxPipelineFailures) {
        return nullptr;
    }
    if (const auto pending = pending_graphics.find(sel.graphics_key); pending != pending_graphics.end()) {
        const std::shared_ptr<PendingPipeline> job = pending->second;
        bool steal = false;
        if (!job->ready.load(std::memory_order_acquire)) {
            std::unique_lock lk{job->mutex};
            if (!job->done) {
                if ((job->from_cache || !skippable) && job->make && !job->started.exchange(true)) {
                    steal = true;
                } else {
                    if (job->from_cache && skippable) {
                        // Cached warmup/imported pipeline already building on a worker (usually
                        // <1 ms from the driver pipeline cache): wait briefly before skipping.
                        if (!job->cv.wait_for(lk, std::chrono::milliseconds(4),
                                              [&] { return job->done; })) {
                            ++async_skipped;
                            return nullptr;
                        }
                    } else if (skippable) {
                        ++async_skipped;
                        return nullptr; // drawn from the frame its pipeline is ready
                    } else {
                        // A pass that is not drawn every frame needs it now: the wait is compile time.
                        const auto start = std::chrono::steady_clock::now();
                        // bbport: during active gameplay, never freeze the main render thread waiting
                        // indefinitely on a background worker. Wait briefly (up to 2ms) for fast cache
                        // hits; if still compiling, drop the draw for this frame so 60 FPS is maintained.
                        const bool in_game = g_last_frame_draws >= 300 && sel.regs && sel.regs->depth_buffer.DepthValid();
                        const bool finished = in_game
                            ? job->cv.wait_for(lk, std::chrono::milliseconds(2), [&] { return job->done; })
                            : (job->cv.wait(lk, [&] { return job->done; }), true);
                        g_bb_compile_ns += u64(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                                   std::chrono::steady_clock::now() - start)
                                                   .count());
                        if (!finished) {
                            ++async_skipped;
                            return nullptr;
                        }
                        ++async_waited;
                    }
                }
            }
        }
        if (steal) {
            std::unique_ptr<GraphicsPipeline> built;
            try {
                built = job->make();
            } catch (const std::exception& e) {
                LOG_WARNING(Render_Vulkan, "Inline graphics pipeline {:#x} failed: {}",
                            std::hash<GraphicsPipelineKey>{}(sel.graphics_key), e.what());
            }
            std::scoped_lock lk{job->mutex};
            job->pipeline = std::move(built);
            job->make = {};
            job->done = true;
            job->ready.store(true, std::memory_order_release);
            job->cv.notify_all();
        }
        pending_graphics.erase(sel.graphics_key);
        return FinishPipeline(sel.graphics_key, *job);
    }
    if (skippable) {
        auto& comp = GetCompiler();
        auto job = std::make_shared<PendingPipeline>();
        job->modules = sel.modules;
        PendingPipeline* raw_job = job.get();
        job->make = [this, raw_job, key = sel.graphics_key, infos = sel.infos,
                     runtime_infos = sel.runtime_infos, fetch = sel.fetch_shader,
                     modules = sel.modules]() mutable -> std::unique_ptr<GraphicsPipeline> {
            GraphicsPipeline::SerializationSupport sdata{};
            auto pipeline = std::make_unique<GraphicsPipeline>(
                instance, scheduler, desc_heap, profile, key, *pipeline_cache, infos,
                runtime_infos, fetch, modules, sdata, false);
            if (pipeline) {
                RegisterPipelineData(key, std::hash<GraphicsPipelineKey>{}(key), sdata);
                raw_job->saved = true;
            }
            raw_job->sdata = std::move(sdata);
            return pipeline;
        };
        pending_graphics.emplace(sel.graphics_key, job);
        ++async_started;
        ++g_bb_compiles;
        g_bb_compiles_total.fetch_add(1, std::memory_order_relaxed);
        const size_t key_hash = std::hash<GraphicsPipelineKey>{}(sel.graphics_key);
        comp.Submit([job, key_hash]() mutable {
            if (job->started.exchange(true)) {
                return;
            }
            std::unique_ptr<GraphicsPipeline> pipeline;
            try {
                pipeline = job->make();
            } catch (const std::exception& e) {
                /* bbport: the driver refused this pipeline. The job completes without one and
                 * FinishPipeline() accounts and skips it; an exception must never escape a
                 * worker thread (std::terminate). */
                LOG_WARNING(Render_Vulkan, "Async graphics pipeline {:#x} failed: {}", key_hash,
                            e.what());
            }
            std::scoped_lock lk{job->mutex};
            job->pipeline = std::move(pipeline);
            job->make = {};
            job->done = true;
            job->ready.store(true, std::memory_order_release);
            job->cv.notify_all();
        });
        sel.fetch_shader.reset();
        return nullptr;
    }
    const auto [it, is_new] = graphics_pipelines.try_emplace(sel.graphics_key);
    if (is_new) {
        const auto pipeline_hash = std::hash<GraphicsPipelineKey>{}(sel.graphics_key);
        LOG_INFO(Render_Vulkan, "Compiling graphics pipeline {:#x}", pipeline_hash);
        CompileTimer timer;

        GraphicsPipeline::SerializationSupport sdata{};
        try {
            it.value() = std::make_unique<GraphicsPipeline>(
                instance, scheduler, desc_heap, profile, sel.graphics_key, *pipeline_cache,
                sel.infos, sel.runtime_infos, sel.fetch_shader, sel.modules, sdata, false);
        } catch (const std::exception& e) {
            /* bbport: the driver refused the pipeline (see PipelineCreationMutex). The empty map
             * entry is removed and the draw skipped; the key is retried a bounded number of
             * times by the next draws, then left out. Previously this aborted the process. */
            graphics_pipelines.erase(sel.graphics_key);
            const u32 failures = ++graphics_failures[sel.graphics_key];
            if (failures <= MaxPipelineFailures) {
                LOG_WARNING(Render_Vulkan,
                            "Graphics pipeline {:#x} failed to create (attempt {} of {}): {}; "
                            "the draw is skipped",
                            pipeline_hash, failures, u32{MaxPipelineFailures}, e.what());
            }
            return nullptr;
        }

        RegisterPipelineData(sel.graphics_key, pipeline_hash, sdata);
        ++num_new_pipelines;

        if (EmulatorSettings.IsShaderCollect()) {
            for (auto stage = 0; stage < MaxShaderStages; ++stage) {
                if (sel.infos[stage]) {
                    auto& m = sel.modules[stage];
                    module_related_pipelines[m].emplace_back(sel.graphics_key);
                }
            }
        }
        sel.fetch_shader.reset();
    }
    return it->second.get();
}

const GraphicsPipeline* PipelineCache::FinishPipeline(const GraphicsPipelineKey& key,
                                                      PendingPipeline& job) {
    if (!job.pipeline) {
        if (job.from_cache) {
            // Preloaded warmup/imported pipelines use serialized state and may not match this
            // driver; drop silently without burning live draw retries or logging false draw skips.
            return nullptr;
        }
        /* bbport: the pipeline could not be created (the creator already logged the driver
         * error). Account it and let the next draws retry up to the cap; a null pipeline is
         * never stored, so a later attempt is possible and no draw dereferences it. */
        const u32 failures = ++graphics_failures[key];
        if (failures <= MaxPipelineFailures) {
            LOG_WARNING(Render_Vulkan,
                        "Graphics pipeline {:#x} is unavailable (attempt {} of {}): the draw is "
                        "skipped",
                        std::hash<GraphicsPipelineKey>{}(key), failures, u32{MaxPipelineFailures});
        }
        return nullptr;
    }
    const auto pipeline_hash = std::hash<GraphicsPipelineKey>{}(key);
    if (!job.saved) {
        RegisterPipelineData(key, pipeline_hash, job.sdata);
    }
    if (!job.from_cache) {
        ++num_new_pipelines;
    }
    if (EmulatorSettings.IsShaderCollect()) {
        for (const vk::ShaderModule module : job.modules) {
            if (module) {
                module_related_pipelines[module].emplace_back(key);
            }
        }
    }
    graphics_failures.erase(key);
    auto& slot = graphics_pipelines[key];
    slot = std::move(job.pipeline);
    return slot.get();
}

bool PipelineCache::AsyncSkippable(const PipelineSelection& selection, bool indirect) {
    // Only a pass drawn every frame may go without a draw for a frame or two (its render target
    // drawn to in each of the last frames, below). A one-time render compiles at once, as during
    // loading screens (a frame of fewer than 300 draws before this one). The pipeline must not
    // depend on the draw's vertex buffers (VK_EXT_vertex_input_dynamic_state). A size rule (the
    // scene's, the output's) missed most passes: many set a 16384 scissor.
    static const bool enabled = [] {
        const char* env = std::getenv("BB_ASYNC_PIPELINES");
        if (env) {
            return env[0] != '0';
        }
        const int cfg_val = BbSettings::Get().async_pipelines.load();
        if (cfg_val == 0) {
            return false;
        }
        return true;
    }();
    if (!enabled || !instance.IsVertexInputDynamicState() || !selection.regs) {
        return false;
    }
    UpdateFrameDrawCount();
    const u64 last_frame_draws = g_last_frame_draws;
    static u32 printed = 0;
    const u32 second = BbStats::coarse_second.load(std::memory_order_relaxed);
    if (async_started != 0 && second - printed >= 5) {
        printed = second;
        std::printf("GPU: %llu pipelines compiled in the background, %llu draws went without "
                    "theirs meanwhile, %llu waited for one\n",
                    (unsigned long long)async_started, (unsigned long long)async_skipped,
                    (unsigned long long)async_waited);
        async_started = async_skipped = async_waited = 0;
    }
    if (last_frame_draws < 300) {
        return false;
    }
    // A full-screen pass (one triangle or quad: lighting, fog, post-processing) waits for its
    // pipeline: going without one for a frame left whole frames grey. World-space indirect draws
    // (such as GPU-simulated blood particles and culled meshes) can compile asynchronously as long
    // as clipping is enabled and they target a persistent scene render target (depth valid,
    // >512x512, streak >= 8 below); BB_ASYNC_INDIRECT=0 restores synchronous indirect compiles.
    static const bool env_async_full_screen = [] {
        const char* env = std::getenv("BB_ASYNC_FULL_SCREEN");
        return env && env[0] == '1';
    }();
    static const bool async_indirect = [] {
        const char* env = std::getenv("BB_ASYNC_INDIRECT");
        return !(env && env[0] == '0');
    }();
    const u32 FullScreenIndices = env_async_full_screen ? 3u : (!BbToggle::Disabled(BbToggle::AsyncFullScreenTuning) ? 6u : 12u);
    if (indirect) {
        if (!async_indirect || selection.regs->IsClipDisabled()) {
            return false;
        }
    } else if (selection.regs->num_indices <= FullScreenIndices) {
        return false;
    }
    // Never skip pure 2D / UI / compositing passes (color write with no depth buffer) or small
    // offscreen color targets (e.g. FaceGen character portraits, icons, cubemap faces <= 512x512).
    // Those are often rendered once or for a handful of frames and dropping them leaves blank UI
    // portraits or broken textures.
    const auto& regs = *selection.regs;
    if (regs.color_control.mode != AmdGpu::ColorControl::OperationMode::Disable) {
        for (u32 cb = 0; cb < AmdGpu::NUM_COLOR_BUFFERS; ++cb) {
            if (regs.color_buffers[cb] && regs.color_target_mask.GetMask(cb)) {
                if (!regs.depth_buffer.DepthValid()) {
                    return false;
                }
                if (regs.color_buffers[cb].Pitch() <= 512 &&
                    regs.color_buffers[cb].Height() <= 512) {
                    return false;
                }
                break;
            }
        }
    } else {
        if (!regs.depth_buffer.DepthValid()) {
            return false;
        }
    }
    // A pass drawn every frame: its render target drawn to in each of the last frames (the
    // scene, the output, shadow maps, half-size and depth-only passes alike). In active gameplay
    // (last_frame_draws >= 300), 3D scene/world draws targeting a valid depth buffer (decals, blood
    // particles, combat hit effects, newly engaged enemies) must be allowed to compile asynchronously
    // on their very first frame (streak >= 1) rather than freezing the main render thread for 8 frames.
    // UI bakes, small textures (<=512x512) and loading screens are already protected above.
    static const bool has_env_streak = std::getenv("BB_ASYNC_PIPELINE_STREAK") != nullptr;
    static const u32 env_streak = has_env_streak
        ? u32(std::strtoul(std::getenv("BB_ASYNC_PIPELINE_STREAK"), nullptr, 10)) : 8u;
    const u32 streak_req = has_env_streak ? env_streak : (regs.depth_buffer.DepthValid() ? 1u : 8u);
    const u64 target = DrawTarget(regs);
    const auto streak = target_streaks.find(target);
    return target != 0 && streak != target_streaks.end() && streak->second.frames >= streak_req;
}

const ComputePipeline* PipelineCache::GetComputePipeline() {
    MaybeSaveDriverPipelineCache();
    if (!RefreshComputeKey()) {
        return nullptr;
    }
    /// bbport: a compute pipeline the driver refused repeatedly is not attempted again.
    if (const auto failed = compute_failures.find(compute_key);
        failed != compute_failures.end() && failed->second >= MaxPipelineFailures) {
        return nullptr;
    }
    const auto [it, is_new] = compute_pipelines.try_emplace(compute_key);
    if (is_new) {
        const auto pipeline_hash = std::hash<ComputePipelineKey>{}(compute_key);
        LOG_INFO(Render_Vulkan, "Compiling compute pipeline {:#x}", pipeline_hash);
        CompileTimer timer;

        ComputePipeline::SerializationSupport sdata{};
        try {
            it.value() = std::make_unique<ComputePipeline>(
                instance, scheduler, desc_heap, profile, *pipeline_cache, compute_key,
                *sel.infos[0], sel.modules[0], sdata, false);
        } catch (const std::exception& e) {
            /* bbport: the driver refused the pipeline. Remove the empty entry, account it and
             * skip the dispatch; a bounded number of later dispatches retries it. */
            compute_pipelines.erase(compute_key);
            const u32 failures = ++compute_failures[compute_key];
            if (failures <= MaxPipelineFailures) {
                LOG_WARNING(Render_Vulkan,
                            "Compute pipeline {:#x} failed to create (attempt {} of {}): {}; "
                            "the dispatch is skipped",
                            pipeline_hash, failures, u32{MaxPipelineFailures}, e.what());
            }
            return nullptr;
        }
        RegisterPipelineData(compute_key, sdata);
        ++num_new_pipelines;

        if (EmulatorSettings.IsShaderCollect()) {
            auto& m = sel.modules[0];
            module_related_pipelines[m].emplace_back(compute_key);
        }
    }
    return it->second.get();
}

bool PipelineCache::RefreshGraphicsKey(PipelineSelection& sel) {
    std::memset(&sel.graphics_key, 0, sizeof(GraphicsPipelineKey));
    const auto& regs = (*sel.regs);
    auto& key = sel.graphics_key;

    const bool db_enabled = regs.depth_buffer.DepthValid() || regs.depth_buffer.StencilValid();

    key.z_format = regs.depth_buffer.DepthValid() ? regs.depth_buffer.z_info.format
                                                  : AmdGpu::DepthBuffer::ZFormat::Invalid;
    key.stencil_format = regs.depth_buffer.StencilValid()
                             ? regs.depth_buffer.stencil_info.format
                             : AmdGpu::DepthBuffer::StencilFormat::Invalid;
    key.depth_clamp_enable = !regs.depth_render_override.disable_viewport_clamp;
    key.depth_clip_enable = regs.clipper_control.ZclipEnable();
    key.clip_space = regs.clipper_control.clip_space;
    key.provoking_vtx_last = regs.polygon_control.provoking_vtx_last;
    key.prim_type = regs.primitive_type;
    key.polygon_mode = regs.polygon_control.PolyMode();
    key.patch_control_points =
        regs.stage_enable.hs_en ? regs.ls_hs_config.hs_input_control_points : 0;
    key.logic_op = regs.color_control.rop3;
    key.depth_samples = db_enabled ? regs.depth_buffer.NumSamples() : 1;
    key.num_samples = key.depth_samples;
    key.cb_shader_mask = regs.color_shader_mask;

    const bool skip_cb_binding =
        regs.color_control.mode == AmdGpu::ColorControl::OperationMode::Disable;

    // Only potentially animated G-buffer draws may use the extra motion attachment. The
    // ordinary stage variant is needed first to inspect the vertex shader's resources.
    bool motion_possible = false;
    {
        u32 bound = 0;
        for (s32 cb = 0; cb < AmdGpu::NUM_COLOR_BUFFERS && !skip_cb_binding; ++cb) {
            bound += (regs.color_buffers[cb] && regs.color_target_mask.GetMask(cb)) ? 1 : 0;
        }
        motion_possible = Shader::MotionVectors::positions_address != 0 && bound >= 5 &&
                          !regs.color_buffers[Shader::MotionVectors::Output] &&
                          regs.depth_buffer.DepthValid() &&
                          regs.depth_buffer.NumSamples() == 1 && !regs.IsClipDisabled() &&
                          regs.stage_enable.raw == AmdGpu::ShaderStageEnable::VgtStages::Vs;
    }
    sel.motion = false;

    // First pass to fill render target information needed by shader recompiler
    for (s32 cb = 0; cb < AmdGpu::NUM_COLOR_BUFFERS && !skip_cb_binding; ++cb) {
        const auto& col_buf = regs.color_buffers[cb];
        if (!col_buf || !regs.color_target_mask.GetMask(cb)) {
            // No attachment bound or writing to it is disabled.
            continue;
        }

        // Fill color target information
        auto& color_buffer = key.color_buffers[cb];
        color_buffer.data_format = col_buf.GetDataFmt();
        color_buffer.num_format = col_buf.GetNumberFmt();
        color_buffer.num_conversion = col_buf.GetNumberConversion();
        color_buffer.export_format = regs.color_export_format.GetFormat(cb);
        color_buffer.swizzle = col_buf.Swizzle();

        const auto& bc = regs.blend_control[cb];
        color_buffer.blend_self_scale =
            bc.enable && !col_buf.info.blend_bypass &&
            (bc.color_func == AmdGpu::BlendControl::BlendFunc::Min ||
             bc.color_func == AmdGpu::BlendControl::BlendFunc::Max) &&
            bc.color_src_factor == AmdGpu::BlendControl::BlendFactor::SrcColor &&
            bc.color_dst_factor == AmdGpu::BlendControl::BlendFactor::DstColor;
    }

    // Compile and bind shader stages
    if (!RefreshGraphicsStages(sel)) {
        return false;
    }
    if (motion_possible) {
        const auto* vs = sel.infos[static_cast<u32>(Shader::SwStage::Vertex)];
        if (vs) {
            // Shaders with a bone palette (motion_history.h). Small skeletons (weapons,
            // props) also include static world pieces: the rasterizer gives those
            // history only while their constants change.
            for (const auto& resource : vs->buffers) {
                if (resource.IsSpecial()) continue;
                const auto buffer = resource.GetSharp(*vs);
                if (buffer.Valid() && buffer.GetStride() == 16 &&
                    Motion::ClassifyBuffer(buffer.GetSize()) != Motion::BufferRole::Other) {
                    sel.motion = true;
                    break;
                }
            }
        }
        // BB_MOTION_SELECT_LOG=1: each G-buffer vertex shader once, with its buffer sizes
        // and the selection, to find animated models that the size rule leaves out.
        static const bool select_log = [] {
            const char* value = std::getenv("BB_MOTION_SELECT_LOG");
            return value && value[0] == '1';
        }();
        if (select_log && vs) {
            std::string sizes;
            for (const auto& resource : vs->buffers) {
                if (resource.IsSpecial()) continue;
                const auto buffer = resource.GetSharp(*vs);
                sizes += fmt::format(" {}/{}", buffer.GetSize(), buffer.GetStride());
            }
            static std::mutex log_mutex;
            static std::unordered_set<u64> logged;
            std::scoped_lock lock{log_mutex};
            if (logged.insert(vs->pgm_hash ^ std::hash<std::string>{}(sizes)).second) {
                std::printf("Motion select: vs %016llx %s, buffers (size/stride):%s\n",
                            static_cast<unsigned long long>(vs->pgm_hash),
                            sel.motion ? "ON " : "off", sizes.c_str());
            }
        }
        // Keep the old broad path available for visual A/B tests.
        static const bool all_motion = [] {
            const char* value = std::getenv("BB_OBJECT_MOTION_ALL");
            return value && value[0] == '1';
        }();
        if (all_motion) {
            sel.motion = true;
        }
        if (sel.motion && !RefreshGraphicsStages(sel, true)) {
            return false;
        }
    }

    // Second pass to mask out render targets not written by shader and fill remaining info
    u8 color_samples = 0;
    bool all_color_samples_same = true;
    for (s32 cb = 0; cb < key.num_color_attachments && !skip_cb_binding; ++cb) {
        const auto& col_buf = regs.color_buffers[cb];
        const u32 target_mask = regs.color_target_mask.GetMask(cb);
        if (!col_buf || !target_mask) {
            continue;
        }
        if ((key.mrt_mask & (1u << cb)) == 0) {
            std::memset(&key.color_buffers[cb], 0, sizeof(Shader::PsColorBuffer));
            continue;
        }

        // Fill color blending information
        if (regs.blend_control[cb].enable && !col_buf.info.blend_bypass) {
            key.blend_controls[cb] = regs.blend_control[cb];
        }

        // Apply swizzle to target mask
        key.write_masks[cb] =
            vk::ColorComponentFlags{key.color_buffers[cb].swizzle.ApplyMask(target_mask)};

        // Fill color samples
        const u8 prev_color_samples = std::exchange(color_samples, col_buf.NumSamples());
        all_color_samples_same &= color_samples == prev_color_samples || prev_color_samples == 0;
        key.color_samples[cb] = color_samples;
        key.num_samples = std::max(key.num_samples, color_samples);
    }

    if (sel.motion) {
        constexpr u32 mv = Shader::MotionVectors::Output;
        key.motion_vectors = 1;
        key.mrt_mask |= 1u << mv;
        key.num_color_attachments = mv + 1;
        auto& color_buffer = key.color_buffers[mv];
        color_buffer.data_format = AmdGpu::DataFormat::Format32_32_32_32;
        color_buffer.num_format = AmdGpu::NumberFormat::Float;
        color_buffer.swizzle = AmdGpu::IdentityMapping;
        key.write_masks[mv] = vk::ColorComponentFlagBits::eR | vk::ColorComponentFlagBits::eG |
                              vk::ColorComponentFlagBits::eB | vk::ColorComponentFlagBits::eA;
        key.color_samples[mv] = 1;
    }

    // Force all color samples to match depth samples to avoid unsupported MSAA configuration
    if (color_samples != 0) {
        const bool depth_mismatch = db_enabled && color_samples != key.depth_samples;
        if (!all_color_samples_same && !instance.IsMixedAnySamplesSupported() ||
            all_color_samples_same && depth_mismatch && !instance.IsMixedDepthSamplesSupported()) {
            key.color_samples.fill(key.depth_samples);
            key.num_samples = key.depth_samples;
        }
    }

    return true;
}

bool PipelineCache::RefreshGraphicsStages(PipelineSelection& sel, bool rebind_motion_only) {
    const auto& regs = (*sel.regs);
    auto& key = sel.graphics_key;

    Shader::Backend::Bindings binding{};
    const auto bind_stage = [&](HwStage stage_in, SwStage stage_out) -> int {
        const auto stage_in_idx = static_cast<u32>(stage_in);
        const auto stage_out_idx = static_cast<u32>(stage_out);
        if (!regs.stage_enable.IsStageEnabled(stage_in_idx)) {
            key.stage_hashes[stage_out_idx] = 0;
            sel.infos[stage_out_idx] = nullptr;
            return 0;
        }

        const auto* pgm = regs.ProgramForStage(stage_in_idx);
        if (!pgm || !pgm->Address<u32*>()) {
            key.stage_hashes[stage_out_idx] = 0;
            sel.infos[stage_out_idx] = nullptr;
            return 0;
        }

        const auto params = AmdGpu::GetParams(*pgm);
        std::optional<Shader::Gcn::FetchShaderData> fetch_shader_;
        std::tie(sel.infos[stage_out_idx], sel.modules[stage_out_idx], fetch_shader_,
                 key.stage_hashes[stage_out_idx]) =
            GetProgram(sel, stage_in, stage_out, params, binding);
        if (fetch_shader_) {
            sel.fetch_shader = fetch_shader_;
        }
        if (!sel.infos[stage_out_idx] || !sel.modules[stage_out_idx]) {
            return -1;
        }
        return 1;
    };

    if (rebind_motion_only) {
        if (sel.worker) {
            sel.worker->stages.clear();
        }
        return bind_stage(HwStage::Fragment, SwStage::Fragment) >= 0 &&
               bind_stage(HwStage::Vertex, SwStage::Vertex) > 0;
    }

    sel.fetch_shader = std::nullopt;
    sel.infos.fill(nullptr);
    sel.modules.fill(nullptr);

    bool any_pending = false;
    if (bind_stage(HwStage::Fragment, SwStage::Fragment) < 0) {
        any_pending = true;
    }

    const auto* fs_info = sel.infos[static_cast<u32>(SwStage::Fragment)];
    key.mrt_mask = fs_info ? fs_info->mrt_mask : 0u;
    key.num_color_attachments = std::bit_width(key.mrt_mask);

    switch (regs.stage_enable.raw) {
    case AmdGpu::ShaderStageEnable::VgtStages::EsGs:
        if (!instance.IsGeometryStageSupported()) {
            LOG_WARNING(Render_Vulkan, "Geometry shader stage unsupported, skipping");
            return false;
        }
        if (regs.vgt_gs_mode.onchip || regs.vgt_strmout_config.raw) {
            LOG_WARNING(Render_Vulkan, "Geometry shader features unsupported, skipping");
            return false;
        }
        if (bind_stage(HwStage::Export, SwStage::Vertex) < 0) {
            any_pending = true;
        }
        if (bind_stage(HwStage::Geometry, SwStage::Geometry) < 0) {
            any_pending = true;
        }
        break;
    case AmdGpu::ShaderStageEnable::VgtStages::LsHs:
        if (!instance.IsTessellationSupported()) {
            return false;
        }
        // bbport: HwStage::Local reads the hull shader's Info (tessellation constants), so a
        // missing or still-compiling hull program must defer the whole draw. Continuing would
        // build a Local runtime info from a null sel.infos[TessellationControl].
        if (bind_stage(HwStage::Hull, SwStage::TessellationControl) < 0) {
            return false;
        }
        if (bind_stage(HwStage::Vertex, SwStage::TessellationEval) < 0) {
            any_pending = true;
        }
        if (bind_stage(HwStage::Local, SwStage::Vertex) < 0) {
            any_pending = true;
        }
        break;
    case AmdGpu::ShaderStageEnable::VgtStages::LsHsEsGs:
        if (!instance.IsTessellationSupported()) {
            return false;
        }
        if (!instance.IsGeometryStageSupported()) {
            LOG_WARNING(Render_Vulkan, "Geometry shader stage unsupported, skipping");
            return false;
        }
        if (regs.vgt_gs_mode.onchip || regs.vgt_strmout_config.raw) {
            LOG_WARNING(Render_Vulkan, "Geometry shader features unsupported, skipping");
            return false;
        }
        // bbport: as in the LsHs case, HwStage::Local needs the hull shader's Info.
        if (bind_stage(HwStage::Hull, SwStage::TessellationControl) < 0) {
            return false;
        }
        if (bind_stage(HwStage::Export, SwStage::TessellationEval) < 0) {
            any_pending = true;
        }
        if (bind_stage(HwStage::Local, SwStage::Vertex) < 0) {
            any_pending = true;
        }
        if (bind_stage(HwStage::Geometry, SwStage::Geometry) < 0) {
            any_pending = true;
        }
        break;
    case AmdGpu::ShaderStageEnable::VgtStages::Vs:
        if (bind_stage(HwStage::Vertex, SwStage::Vertex) < 0) {
            any_pending = true;
        }
        break;
    default:
        LOG_WARNING(Render_Vulkan, "unimplemented shader stage {}", (u32)regs.stage_enable.raw);
        return false;
    }

    if (any_pending) {
        return false;
    }

    const auto* vs_info = sel.infos[static_cast<u32>(SwStage::Vertex)];
    if (vs_info && sel.fetch_shader && !instance.IsVertexInputDynamicState()) {
        // Without vertex input dynamic state, the pipeline needs to specialize on format.
        // Stride will still be handled outside the pipeline using dynamic state.
        u32 vertex_binding = 0;
        for (const auto& attrib : sel.fetch_shader->attributes) {
            const auto& buffer = attrib.GetSharp(*vs_info);
            ASSERT_MSG(vertex_binding < MaxVertexBufferCount,
                       "Vertex attribute binding count exceeded limit: {} >= {}", vertex_binding,
                       MaxVertexBufferCount);
            key.vertex_buffer_formats[vertex_binding++] =
                Vulkan::LiverpoolToVK::SurfaceFormat(buffer.GetDataFmt(), buffer.GetNumberFmt());
        }
    }

    return true;
}

bool PipelineCache::RefreshComputeKey() {
    Shader::Backend::Bindings binding{};
    const auto& cs_pgm = liverpool->GetCsRegs();
    const auto cs_params = AmdGpu::GetParams(cs_pgm);
    std::tie(sel.infos[0], sel.modules[0], sel.fetch_shader, compute_key.value) =
        GetProgram(sel, HwStage::Compute, SwStage::Compute, cs_params, binding);
    return sel.infos[0] != nullptr && bool(sel.modules[0]);
}

vk::ShaderModule PipelineCache::CompileModule(Shader::Info& info, Shader::RuntimeInfo& runtime_info,
                                              const std::span<const u32>& code, size_t perm_idx,
                                              Shader::Backend::Bindings& binding) {
    LOG_INFO(Render_Vulkan, "Compiling {} shader {:#x} {}", info.hw_stage, info.pgm_hash,
             perm_idx != 0 ? "(permutation)" : "");
    DumpShader(code, info.pgm_hash, info.hw_stage, perm_idx, "bin");
    CompileTimer timer;

    const bool capture = BbShaderCapture::Enabled();
    std::optional<Shader::Info> in_info;
    const auto in_runtime_info = runtime_info;
    const auto in_binding = binding;
    const bool keep_input = EmulatorSettings.IsPipelineCacheEnabled();
    if (capture || keep_input) {
        in_info.emplace(info);
        BbShaderCapture::BeginInput();
    }
    SCOPE_EXIT {
        if (capture || keep_input) {
            Shader::guest_read_hook = nullptr;
        }
    };

    thread_local Shader::Pools tl_pools;
    const auto ir_program = Shader::TranslateProgram(code, tl_pools, info, runtime_info, profile);
    auto spv = Shader::Backend::SPIRV::EmitSPIRV(profile, runtime_info, ir_program, binding);
    if (keep_input) {
        RegisterShaderInput(
            BbShaderCapture::TakeInput(*in_info, in_runtime_info, in_binding, code, perm_idx,
                                       info.flattened_ud_buf),
            info.pgm_hash, perm_idx);
    }
    if (capture) {
        BbShaderCapture::Write(*in_info, in_runtime_info, profile, in_binding, code, perm_idx, spv,
                               info, binding);
    }
    DumpShader(spv, info.pgm_hash, info.hw_stage, perm_idx, "spv");

    vk::ShaderModule module;

    std::optional<std::vector<u32>> patch;
    const bool patch_enabled = EmulatorSettings.IsPatchShaders();
    if (patch_enabled) {
        patch = GetShaderPatch(info.pgm_hash, info.hw_stage, perm_idx, "spv");
    }
    const bool is_patched = patch.has_value();
    if (is_patched) {
        LOG_INFO(Loader, "Loaded patch for {} shader {:#x}", info.hw_stage, info.pgm_hash);
        module = CompileSPV(*patch, instance.GetDevice());
    } else {
        module = CompileSPV(spv, instance.GetDevice());
    }

    RegisterShaderBinary(std::move(spv), info.pgm_hash, perm_idx);

    const auto name = GetShaderName(info.hw_stage, info.pgm_hash, perm_idx);
    Vulkan::SetObjectName(instance.GetDevice(), module, name);
    if (EmulatorSettings.IsShaderCollect()) {
        DebugState.CollectShader(name, info.sw_stage, module, spv, code,
                                 patch ? *patch : std::span<const u32>{}, is_patched);
    }
    return module;
}

PipelineCache::Result PipelineCache::GetProgram(PipelineSelection& sel, HwStage hw_stage,
                                                SwStage sw_stage,
                                                const Shader::ShaderParams& params,
                                                Shader::Backend::Bindings& binding) {
    auto runtime_info = BuildRuntimeInfo(sel, hw_stage, sw_stage);
    if (sel.worker) {
        // bbport: draw-preparation worker: look up only, with the worker's own Info copy.
        auto& worker = *sel.worker;
        std::shared_lock lk{programs_mutex};
        const auto found_program = program_cache.find(params.hash);
        if (found_program == program_cache.end() || !found_program->second->info_template) {
            worker.failed = true;
            return {};
        }
        const Program* program = found_program->second.get();
        auto [it_info, new_info] = worker.infos.try_emplace(program, *program->info_template);
        auto& info = it_info->second;
        info.pgm_base = params.Base();
        info.user_data = params.user_data;
        // The walk of resource tables and the fetch shader parse read guest memory through
        // pointers in the registers; ahead of the GPU thread that memory may already be
        // reused. A fault returns here (runtime_fault_recover) and the draw is left to the GPU
        // thread. A jump out of the specialization leaks its partial allocations (rare).
        BbRecoverBuf recover;
        struct FaultRecoverGuard {
            explicit FaultRecoverGuard(BbRecoverBuf* buf) {
                runtime_fault_recover = buf;
            }
            ~FaultRecoverGuard() {
                runtime_fault_recover = nullptr;
            }
        } fault_guard{&recover};
        if (BB_RECOVER_SET(recover)) {
            worker.failed = true;
            return {};
        }
        info.RefreshFlatBuf();
        auto spec = Shader::StageSpecialization(info, runtime_info, profile, binding);
        const auto it = std::ranges::find(program->modules, spec, &Program::Module::spec);
        if (it == program->modules.end()) {
            worker.failed = true;
            return {};
        }
        info.AddBindings(binding);
        const size_t perm_idx = std::distance(program->modules.begin(), it);
        worker.stages.push_back(
            {program, params.hash, hw_stage, info.pgm_base, &info.flattened_ud_buf});
        return std::make_tuple(&info, it->module, it->spec.fetch_shader_data,
                               HashCombine(params.hash, perm_idx));
    }

    const bool in_game = g_last_frame_draws >= 300 && sel.regs && sel.regs->depth_buffer.DepthValid();

    Program* program = nullptr;
    {
        std::shared_lock lk{programs_mutex};
        auto it_pgm = program_cache.find(params.hash);
        if (it_pgm != program_cache.end()) {
            program = it_pgm->second.get();
        }
    }

    if (!program) {
        if (auto it_pending = pending_programs.find(params.hash); it_pending != pending_programs.end()) {
            auto job = it_pending->second;
            if (job->ready.load(std::memory_order_acquire)) {
                pending_programs.erase(it_pending);
                std::shared_lock lk{programs_mutex};
                auto it_pgm = program_cache.find(params.hash);
                if (it_pgm != program_cache.end()) {
                    program = it_pgm->second.get();
                }
            } else if (in_game) {
                std::unique_lock lk{job->mutex};
                if (!job->done) {
                    job->cv.wait_for(lk, std::chrono::milliseconds(2), [&] { return job->done; });
                }
                if (job->done && job->ready.load(std::memory_order_acquire)) {
                    pending_programs.erase(it_pending);
                    std::shared_lock lk_pm{programs_mutex};
                    auto it_pgm = program_cache.find(params.hash);
                    if (it_pgm != program_cache.end()) {
                        program = it_pgm->second.get();
                    }
                } else {
                    return {};
                }
            }
        }
    }

    if (!program) {
        if (in_game) {
            auto job = std::make_shared<PendingProgram>();
            pending_programs.emplace(params.hash, job);

            std::vector<u32> code_copy(params.code.begin(), params.code.end());
            std::array<u32, Shader::ShaderParams::NumShaderUserData> user_data_copy{};
            std::ranges::copy(params.user_data, user_data_copy.begin());
            const u64 pgm_hash = params.hash;
            const auto start_binding = binding;
            auto r_info = runtime_info;

            GetCompiler().Submit([this, job, pgm_hash, hw_stage, sw_stage,
                                  code_copy = std::move(code_copy), user_data_copy,
                                  start_binding, r_info]() mutable {
                try {
                    Shader::ShaderParams thread_params{
                        .user_data = user_data_copy,
                        .code = code_copy,
                        .hash = pgm_hash,
                    };

                    auto new_prog = std::make_unique<Program>(hw_stage, sw_stage, thread_params);
                    new_prog->info.RefreshFlatBuf();
                    auto b = start_binding;
                    auto module = CompileModule(new_prog->info, r_info, thread_params.code, 0, b);
                    auto spec = Shader::StageSpecialization(new_prog->info, r_info, profile, start_binding);
                    const auto perm_hash = HashCombine(pgm_hash, 0);

                    RegisterShaderMeta(new_prog->info, spec.fetch_shader_data, spec, perm_hash, 0);
                    const u64 flat_hash = XXH3_64bits(new_prog->info.flattened_ud_buf.data(),
                                                      new_prog->info.flattened_ud_buf.size() * sizeof(u32));
                    new_prog->AddPermut(module, std::move(spec), flat_hash);
                    new_prog->info_template = std::make_unique<Shader::Info>(new_prog->info);

                    {
                        std::unique_lock lk{programs_mutex};
                        program_cache.emplace(pgm_hash, std::move(new_prog));
                    }
                } catch (const std::exception& e) {
                    LOG_WARNING(Render_Vulkan, "Async shader program {:#x} failed: {}", pgm_hash, e.what());
                }

                {
                    std::scoped_lock lk{job->mutex};
                    job->done = true;
                }
                job->ready.store(true, std::memory_order_release);
                job->cv.notify_all();
            });

            return {};
        }

        auto new_program = std::make_unique<Program>(hw_stage, sw_stage, params);
        auto start = binding;
        const auto module =
            CompileModule(new_program->info, runtime_info, params.code, 0, binding);
        auto spec = Shader::StageSpecialization(new_program->info, runtime_info, profile, start);
        const auto perm_hash = HashCombine(params.hash, 0);

        RegisterShaderMeta(new_program->info, spec.fetch_shader_data, spec, perm_hash, 0);
        const u64 flat_hash = XXH3_64bits(new_program->info.flattened_ud_buf.data(),
                                          new_program->info.flattened_ud_buf.size() * sizeof(u32));
        new_program->AddPermut(module, std::move(spec), flat_hash);
        new_program->info_template = std::make_unique<Shader::Info>(new_program->info);
        Program* live_program = new_program.get();
        {
            std::unique_lock lk{programs_mutex};
            program_cache.emplace(params.hash, std::move(new_program));
        }
        return std::make_tuple(&live_program->info, module,
                               live_program->modules[0].spec.fetch_shader_data, perm_hash);
    }

    if (!program->info_template) {
        // Programs loaded by the pipeline cache warm-up get their template on first use.
        std::unique_lock lk{programs_mutex};
        if (!program->info_template) {
            program->info_template = std::make_unique<Shader::Info>(program->info);
        }
    }
    auto& info = program->info;
    info.pgm_base = params.Base(); // Needs to be actualized for inline cbuffer address fixup
    info.user_data = params.user_data;
    info.RefreshFlatBuf();
    auto spec = Shader::StageSpecialization(info, runtime_info, profile, binding);

    size_t perm_idx = 0;
    u64 perm_hash = 0;
    vk::ShaderModule module{};
    bool found_perm = false;
    std::optional<Shader::Gcn::FetchShaderData> fetch_shader_data{};

    {
        std::shared_lock lk{programs_mutex};
        auto it = program->last_used < program->modules.size() &&
                          program->modules[program->last_used].spec == spec
                      ? program->modules.begin() + program->last_used
                      : std::ranges::find(program->modules, spec, &Program::Module::spec);
        if (it != program->modules.end()) {
            program->last_used = std::distance(program->modules.begin(), it);
            perm_idx = program->last_used;
            perm_hash = HashCombine(params.hash, perm_idx);
            module = it->module;
            fetch_shader_data = it->spec.fetch_shader_data;
            found_perm = true;
        } else {
            perm_idx = program->modules.size();
            perm_hash = HashCombine(params.hash, perm_idx);
        }
    }

    if (found_perm) {
        info.AddBindings(binding);
        return std::make_tuple(&program->info, module, fetch_shader_data, perm_hash);
    }

    const u64 perm_job_key = perm_hash;
    if (auto it_pending = pending_permutations.find(perm_job_key); it_pending != pending_permutations.end()) {
        auto job = it_pending->second;
        if (job->ready.load(std::memory_order_acquire)) {
            pending_permutations.erase(it_pending);
            std::shared_lock lk{programs_mutex};
            auto it = std::ranges::find(program->modules, spec, &Program::Module::spec);
            if (it != program->modules.end()) {
                program->last_used = std::distance(program->modules.begin(), it);
                perm_idx = program->last_used;
                module = it->module;
                fetch_shader_data = it->spec.fetch_shader_data;
                found_perm = true;
            }
        } else if (in_game) {
            std::unique_lock lk{job->mutex};
            if (!job->done) {
                job->cv.wait_for(lk, std::chrono::milliseconds(2), [&] { return job->done; });
            }
            if (job->done && job->ready.load(std::memory_order_acquire)) {
                pending_permutations.erase(it_pending);
                std::shared_lock lk_pm{programs_mutex};
                auto it = std::ranges::find(program->modules, spec, &Program::Module::spec);
                if (it != program->modules.end()) {
                    program->last_used = std::distance(program->modules.begin(), it);
                    perm_idx = program->last_used;
                    module = it->module;
                    fetch_shader_data = it->spec.fetch_shader_data;
                    found_perm = true;
                }
            } else {
                return {};
            }
        }
    }

    if (found_perm) {
        info.AddBindings(binding);
        return std::make_tuple(&program->info, module, fetch_shader_data, perm_hash);
    }

    if (in_game) {
        auto job = std::make_shared<PendingProgram>();
        pending_permutations.emplace(perm_job_key, job);

        std::vector<u32> code_copy(params.code.begin(), params.code.end());
        std::array<u32, Shader::ShaderParams::NumShaderUserData> user_data_copy{};
        std::ranges::copy(params.user_data, user_data_copy.begin());
        const u64 pgm_hash = params.hash;
        const auto start_binding = binding;
        auto r_info = runtime_info;

        GetCompiler().Submit([this, job, pgm_hash, hw_stage, sw_stage,
                              code_copy = std::move(code_copy), user_data_copy,
                              start_binding, r_info, perm_idx, perm_hash,
                              spec = std::move(spec)]() mutable {
            try {
                Shader::ShaderParams thread_params{
                    .user_data = user_data_copy,
                    .code = code_copy,
                    .hash = pgm_hash,
                };

                auto new_info = Shader::Info(hw_stage, sw_stage, thread_params);
                new_info.RefreshFlatBuf();
                auto b = start_binding;
                auto mod = CompileModule(new_info, r_info, thread_params.code, perm_idx, b);

                RegisterShaderMeta(new_info, spec.fetch_shader_data, spec, perm_hash, perm_idx);
                std::unique_lock lk{programs_mutex};
                const u64 flat_hash = XXH3_64bits(new_info.flattened_ud_buf.data(),
                                                  new_info.flattened_ud_buf.size() * sizeof(u32));
                auto pit = program_cache.find(pgm_hash);
                if (pit != program_cache.end()) {
                    spec.info = &pit.value()->info;
                    pit.value()->AddPermut(mod, std::move(spec), flat_hash);
                }
            } catch (const std::exception& e) {
                LOG_WARNING(Render_Vulkan, "Async shader permutation {:#x} failed: {}", pgm_hash, e.what());
            }

            {
                std::scoped_lock lk_job{job->mutex};
                job->done = true;
            }
            job->ready.store(true, std::memory_order_release);
            job->cv.notify_all();
        });

        return {};
    }

    auto new_info = Shader::Info(hw_stage, sw_stage, params);
    module = CompileModule(new_info, runtime_info, params.code, perm_idx, binding);

    RegisterShaderMeta(new_info, spec.fetch_shader_data, spec, perm_hash, perm_idx);
    {
        std::unique_lock lk{programs_mutex};
        const u64 flat_hash = XXH3_64bits(new_info.flattened_ud_buf.data(),
                                          new_info.flattened_ud_buf.size() * sizeof(u32));
        program->AddPermut(module, std::move(spec), flat_hash);
    }
    return std::make_tuple(&program->info, module,
                           program->modules[perm_idx].spec.fetch_shader_data, perm_hash);
}

std::optional<vk::ShaderModule> PipelineCache::ReplaceShader(vk::ShaderModule module,
                                                             std::span<const u32> spv_code) {
    selection_cache.valid = false;
    std::optional<vk::ShaderModule> new_module{};
    for (const auto& [_, program] : program_cache) {
        for (auto& m : program->modules) {
            if (m.module == module) {
                const auto& d = instance.GetDevice();
                d.destroyShaderModule(m.module);
                m.module = CompileSPV(spv_code, d);
                new_module = m.module;
            }
        }
    }
    if (module_related_pipelines.contains(module)) {
        auto& pipeline_keys = module_related_pipelines[module];
        for (auto& key : pipeline_keys) {
            if (std::holds_alternative<GraphicsPipelineKey>(key)) {
                auto& graphics_key = std::get<GraphicsPipelineKey>(key);
                graphics_pipelines.erase(graphics_key);
                if (const auto pit = pending_graphics.find(graphics_key);
                    pit != pending_graphics.end() && pit->second->ready.load(std::memory_order_acquire)) {
                    pending_graphics.erase(graphics_key);
                }
            } else if (std::holds_alternative<ComputePipelineKey>(key)) {
                auto& compute_key = std::get<ComputePipelineKey>(key);
                compute_pipelines.erase(compute_key);
            }
        }
    }
    return new_module;
}

std::string PipelineCache::GetShaderName(Shader::HwStage stage, u64 hash,
                                         std::optional<size_t> perm) {
    if (perm) {
        return fmt::format("{}_{:#018x}_{}", stage, hash, *perm);
    }
    return fmt::format("{}_{:#018x}", stage, hash);
}

void PipelineCache::DumpShader(std::span<const u32> code, u64 hash, Shader::HwStage stage,
                               size_t perm_idx, std::string_view ext) {
    if (!EmulatorSettings.IsDumpShaders()) {
        return;
    }

    using namespace Common::FS;
    const auto dump_dir = GetUserPath(PathType::ShaderDir) / "dumps";
    if (!std::filesystem::exists(dump_dir)) {
        std::filesystem::create_directories(dump_dir);
    }
    const auto filename = fmt::format("{}.{}", GetShaderName(stage, hash, perm_idx), ext);
    const auto file = IOFile{dump_dir / filename, FileAccessMode::Create};
    file.WriteSpan(code);
}

std::optional<std::vector<u32>> PipelineCache::GetShaderPatch(u64 hash, Shader::HwStage stage,
                                                              size_t perm_idx,
                                                              std::string_view ext) {
    if (!EmulatorSettings.IsPatchShaders()) {
        return {};
    }

    using namespace Common::FS;
    const auto patch_dir = GetUserPath(PathType::ShaderDir) / "patch";
    if (!std::filesystem::exists(patch_dir)) {
        std::filesystem::create_directories(patch_dir);
    }
    const auto filename = fmt::format("{}.{}", GetShaderName(stage, hash, perm_idx), ext);
    const auto filepath = patch_dir / filename;
    if (!std::filesystem::exists(filepath)) {
        return {};
    }
    const auto file = IOFile{patch_dir / filename, FileAccessMode::Read};
    std::vector<u32> code(file.GetSize() / sizeof(u32));
    file.Read(code);
    return code;
}
} // namespace Vulkan
