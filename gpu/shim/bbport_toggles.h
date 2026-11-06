// bbport: optimizations that can be switched off while the game runs (BB_TOGGLE_FILE,
// see runtime_memory.c), to find which one changes rendering without restarting.
#pragma once
#include <atomic>
#include <csetjmp>
#include <chrono>
#include <cstdint>
#include <cstdlib>

#include "bbport_threads.h"

extern "C" std::uint64_t runtime_disabled_optimizations;
/// The second toggle word (BB_TOGGLE_FILE2): every bit of the first word is assigned.
extern "C" std::uint64_t runtime_disabled_optimizations2;

namespace BbToggle {
enum : std::uint64_t {
    RegionCache = 1,
    FetchShaderCache = 2,
    PageTrackingEarlyExit = 4,
    PendingPollLimit = 8,
    ThreadedRecording = 16,
    ImageDescCache = 32,
    LockFreeUploadCheck = 64,
    FindImageCache = 128,
    DeferredUploads = 256,
    AccessMemo = 512,
    TextureBindingMemo = 1024,
    CoarseReadTracking = 2048,
    PreparedResources = 4096,
    DrawPreparation = 8192,
    DeferredStreamCopies = 16384,
    HotPages = 32768,
    FaultWindow = 65536,
    ParallelCopies = 131072,
    AsyncFences = 262144,
    PoolSmallCopies = 524288,
    RecordPrefetch = 1ull << 32,
    TextureViewMemo = 1ull << 33,
    TextureBindHelper = 1ull << 34,
    EarlyDrawInputs = 1ull << 35,
    ConstantRing = 1ull << 36,
    DrawPipeline = 1ull << 37,
    PipelinedTasks = 1ull << 38,
    PendingFenceWaits = 1ull << 39,
    PipelinedDispatch = 1ull << 40,
    RecorderFences = 1ull << 41, ///< Stage-B fences deferred to the recording thread (liverpool.cpp)
    PipelinedMemoryWrites = 1ull << 42,
    MultiCopyShader = 1ull << 43,
    RenderStateMemo = 1ull << 44,
    TextureSetMemo = 1ull << 45,
    PipelinedIndirectDraws = 1ull << 46,
    SceneAttachmentsOnly = 1ull << 47,
    SampleSceneProxies = 1ull << 48,
    OrderedGuestWrites = 1ull << 49,
    SceneHalfRes = 1ull << 50, ///< live scaling also reduces the 960x540 post targets
    UpdateImageFastPath = 1u << 30,
    /// Bit set: the texture collector frees nothing (A/B of its evictions while the game runs).
    TextureCollector = 1ull << 31,
    // TAA A/B in one run: optional techniques, off by default (no measured gain, 2026-10-02).
    TaaTonemapBlend = 1ull << 51,
    TaaClip = 1ull << 52,
    TaaVariance = 1ull << 53,
    TaaFilter = 1ull << 54,
    // On by default (bit set: off): history of a thin feature this jitter phase missed is kept
    // when nothing moves. Static-camera flicker of railings/window bars p99.9 -45% (2026-10-03).
    TaaKeepNearerHistory = 1ull << 55,
    AuxTessCache = 1ull << 56, ///< caches auxiliary tess/clip shader modules across pipelines
    SceneMipBias = 1ull << 57, ///< negative LOD bias of G-buffer samplers at reduced scene sizes
    AsyncFullScreenTuning = 1ull << 58, ///< lower full-screen index threshold (12 -> 6) for async compile
    FlatHashValidation = 1ull << 59, ///< fast hash check on flattened user-data before memcmp
    SelectionCache = 1ull << 60, ///< caches pipeline selection across draws with identical registers
    /// Scene textures use BB_ANISO (16) times anisotropic filtering instead of the game's
    /// ratio; bit set: the game's own samplers (A/B while the game runs).
    ForcedAniso = 1ull << 61,
    DepthMinFilter = 1ull << 62, ///< conservative 2x2 min-filter for forward-Z depth downsampling
    TaaDefaults = 1ull << 63, ///< modern TAA defaults: YCoCg variance clipping, tonemap blend, 3x3 filter
    // Bits 20-29 are used as raw debug toggles by the camera/object motion and the upscaler.
};
/// Switches that need a bit of their own; the first word above has all 64 assigned, so these
/// are bits of runtime_disabled_optimizations2 (the file named by BB_TOGGLE_FILE2).
enum : std::uint64_t {
    /// Zero base_address when hashing AmdGpu::Buffer for motion geometry. This used to be
    /// 1ull << 41, shared with RecorderFences; a BB_TOGGLE_FILE value of 1<<41 now turns only
    /// RecorderFences off, and this switch is set from the second file.
    StableMotionHash = 1ull << 0,
    /// The crash guards added to the recorder and the guest fault handler (set: off).
    DefensiveGuards = 1ull << 1,
    /// Pages written by the PS4 red-zone vertex generators stay writable (set: off).
    StickyFaultPages = 1ull << 2,
};
inline bool Disabled(std::uint64_t bit) {
    return (__atomic_load_n(&runtime_disabled_optimizations, __ATOMIC_RELAXED) & bit) != 0;
}
/// Disabled(), for the switches in the second word.
inline bool Disabled2(std::uint64_t bit) {
    return (__atomic_load_n(&runtime_disabled_optimizations2, __ATOMIC_RELAXED) & bit) != 0;
}
} // namespace BbToggle

namespace BbStats {
/// Guest writes caught by page protection, and pages currently left unprotected as hot.
inline std::atomic<std::uint64_t> tracker_faults{0};
inline std::atomic<std::int64_t> hot_pages{0};
/// Pages permanently kept writable for the red-zone vertex generators (BB_STICKY_FAULT_PAGES).
inline std::atomic<std::uint64_t> sticky_pages{0};
/// Stall diagnostics (BB_FRAME_STATS): per-frame deltas printed for frames over 40 ms.
inline std::atomic<std::uint64_t> images_registered{0};
inline std::atomic<std::uint64_t> image_upload_bytes{0};
inline std::atomic<std::uint64_t> buffer_upload_bytes{0};
inline std::atomic<int> gpu_thread_clock{-1}; ///< clockid_t of the GPU command thread
inline std::atomic<std::uint64_t> draws{0}, dispatches{0}, submissions{0}, flips{0};
/// Frames the GPU command thread has started (display pass), for per-frame diagnostics.
inline std::atomic<std::uint64_t> gpu_frames{0};
/// Wall time spent in operations suspected of stalls (ns, all threads).
inline std::atomic<std::uint64_t> t_resident{0}, t_protect{0}, t_image_create{0}, t_refresh{0},
    t_staging{0}, t_host_wait{0}, t_copy{0}, copy_bytes{0}, t_read_faults{0}, read_faults{0},
    t_write_faults{0}, t_copy_cpu{0}, copy_sys_us{0}, copy_minflt{0};
/// Diagnostics are collected only with BB_FRAME_STATS=1.
inline const bool enabled = [] {
    const char* env = std::getenv("BB_FRAME_STATS");
    return env && env[0] == '1';
}();
struct Timer {
    std::atomic<std::uint64_t>& total;
    std::chrono::steady_clock::time_point start =
        enabled ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
    ~Timer() {
        if (!enabled) {
            return;
        }
        total.fetch_add(std::chrono::duration_cast<std::chrono::nanoseconds>(
                            std::chrono::steady_clock::now() - start)
                            .count(),
                        std::memory_order_relaxed);
    }
};
/// Wall time the GPU command thread waited for guest submissions (ns).
inline std::atomic<std::uint64_t> gpu_idle_ns{0};
/// Draws recorded into the reduced scene targets, and draws after the scene started.
inline std::atomic<std::uint64_t> reduced_draws{0}, scene_draws{0};
/// Wall time spent blocked in the scheduler (ns): waiting for the recording thread to drain,
/// for host copies before a submission or fence, and for GPU ticks.
inline std::atomic<std::uint64_t> sync_recording_ns{0}, host_copies_wait_ns{0}, tick_wait_ns{0},
    copy_threads_wait_ns{0}, host_copy_waits{0}, host_copy_waits_skipped{0};
/// The texture cache collector: the usage it compares, the mark it starts at, images it freed.
inline std::atomic<std::uint64_t> gc_used_bytes{0}, gc_trigger_bytes{0}, gc_freed_images{0};
/// Its critical mark (images used a moment ago evicted past it).
inline std::atomic<std::uint64_t> gc_critical_bytes{0};
/// Seconds of a steady clock, updated at every guest submission (cheap ages for caches).
inline std::atomic<std::uint32_t> coarse_second{0};
/// Game frames shown (incremented on Present).
inline std::atomic<std::uint32_t> frame_number{0};
/// Images the collector passed over (GPU-written, not evictable without memory pressure).
inline std::atomic<std::uint64_t> gc_kept_images{0};
struct WaitTimer {
    std::atomic<std::uint64_t>& total;
    std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
    ~WaitTimer() {
        total.fetch_add(std::chrono::duration_cast<std::chrono::nanoseconds>(
                            std::chrono::steady_clock::now() - start).count(),
                        std::memory_order_relaxed);
    }
};
/// GPU thread rusage, refreshed after each graphics submission.
inline std::atomic<std::uint64_t> gpu_sys_us{0}, gpu_user_us{0}, gpu_invol_switches{0},
    gpu_vol_switches{0}, gpu_minor_faults{0};
/// Protection faults (signals) taken by the GPU thread itself.
inline std::atomic<std::uint64_t> gpu_signal_faults{0};
/// Protection changes: calls and pages, those removing write access (TLB shootdowns) apart.
inline std::atomic<std::uint64_t> protect_calls{0}, protect_pages{0}, protect_revoke_calls{0},
    protect_revoke_pages{0};
} // namespace BbStats
