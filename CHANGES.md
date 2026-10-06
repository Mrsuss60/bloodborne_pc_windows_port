# Bloodborne PC - Windows Native Port Improvements & Changes

This document details the optimizations, fixes, and refactoring applied to ensure stability, smooth frame pacing, low CPU utilization, and clear diagnostics on 4-core / 4-thread systems (such as the Intel Core i5-4590, 14 GB DDR3, GTX 1650 4 GB) while dynamically adapting to other hardware configurations (AMD GPUs, hybrid P/E CPUs, variable RAM/VRAM, and audio configurations). It also documents features and stability improvements ported from Linux upstream 0.3.

---

## 1. Crash Analysis & Resolutions (Wolf Enemy Combat & Stability)

### A. Dynamic VRAM GC Thresholds (`gpu/shadps4/video_core/texture_cache/texture_cache.cpp`)
- **Root Cause**: On 4 GB GPUs like the GTX 1650, texture cache garbage collection was set to trigger critical eviction only at 97% of VRAM. When the player entered combat with the first wolf enemy, dynamic particle effects, blood decals, and new texture allocations pushed VRAM beyond 100%, triggering `vk::Result=ErrorOutOfDeviceMemory STOP: GPU library assertion failed`.
- **Change**: Configured safe, progressive dynamic GC thresholds (70% trigger, 80% pressure, 88% critical eviction), fully configurable via environment variables (`BB_GC_TRIGGER`, `BB_GC_PRESSURE`, `BB_GC_CRITICAL`). This ensures stale images are evicted smoothly before GPU memory is exhausted.
- **Presenter Resilience** (`gpu/shadps4/video_core/renderer_vulkan/vk_presenter.cpp`): Added fallback allocation without `WITHIN_BUDGET` flag and host memory fallback in `RecreateFrame` during transient memory pressure.

### B. Remnant Slice Remapping in Win32 Memory Views (`src/win32_memory.c`, `src/runtime_memory.c`)
- **Root Cause**: Windows memory virtualization using `VirtualAlloc2` placeholders and `MapViewOfFile3` was dropping entire 64 KiB section views when guest code partially unmapped an allocation, failing to preserve and remap remnant prefix and suffix slices.
- **Change**: Implemented view slice tracking and automatic remnant remapping with `restore_remnant()`. Added a 64-entry lock-free ring buffer tracking recent memory operations (`g_mem_op_ring`), dumped during crash diagnosis.

### C. Removal of Silent Null Skips in VEH (`src/probe.c`)
- **Root Cause**: The Vectored Exception Handler had silent instruction skips (`8b 04 07`, `8b 3c 19`, `c5 fa 5c`) that forced destination registers to zero. This masked upstream null pointer bugs and cascaded into all general-purpose registers (RAX, RBX, RCX, RDX, RDI, RBP) becoming 0, culminating in the crash at guest offset `0x28ce9b5`.
- **Change**: Removed silent skips. If an unhandled memory fault occurs, a full crash dump (`out/crash_<timestamp>.log`) is generated with registers, stack frames, RBP chains, and faulting thread import history before clean termination.

### D. Pipeline Cache SRT Walker Fault Handler (`gpu/shadps4/shader_recompiler/ir/passes/flatten_extended_userdata_pass.cpp`)
- **Root Cause**: Restored pipeline cache entries invoked SRT walkers without ensuring the signal handler was registered, leading to access violation crashes.
- **Change**: Added `EnsureSrtFaultHandler()` on walker registration and wrapped concurrent instruction patching in a mutex (`patch_mutex`) to support parallel draw-preparation workers.

### E. AvPlayer Thread Safety (`gpu/shim/core/libraries/kernel/threads.h`)
- **Root Cause**: AvPlayer demuxer and decoder threads attempted to join themselves or collided with owner destructors during playback teardown.
- **Change**: Implemented thread-safe `Take()`/`Finish()` pattern under `std::mutex` ensuring clean detachment and non-reentrant joining.

### F. High-Entropy ASLR & Address Space Collision (Error 487) (`gpu/CMakeLists.txt`, `src/win32_memory.c`, `src/probe.c`, `src/runtime_memory.c`)
- **Root Cause**: Windows 64-bit High-Entropy ASLR and DynamicBase were enabled by default on `bbport.exe`. Windows randomized the main thread stack and CRT heap into `[0x800000000, 0xfc00000000)` (~1 TB guest address space). When `VirtualAlloc2` attempted to reserve the full 1 TB placeholder, it collided with the preexisting thread stack and returned `Windows error 487 (ERROR_INVALID_ADDRESS)`. As a result, `space()` failed, `runtime_low_map` could not allocate, image loading fell back to host allocations exceeding 40 bits (`1<<40`), and guest thread stack allocations failed with `"Cannot allocate guest thread"`.
- **Change**:
  1. Linked `bbport.exe` with `-Wl,--disable-dynamicbase` and `-Wl,--disable-high-entropy-va` in `gpu/CMakeLists.txt`, ensuring bottom-up memory layout below 32 GB.
  2. Called `runtime_memory_reserve_space()` at the very start of `main()` in `src/probe.c` before Vulkan, driver DLLs, or background worker threads are initialized.
  3. Added a chunked placeholder fallback in `win_mem_space()`: if a single 1 TB placeholder fails due to any preexisting system or hook allocation, it queries memory and reserves all free regions in `[start, end)` as placeholders, setting `space_start` and `space_end` successfully.

---

## 2. Audio Subsystem Refactoring (Distortion & Stuttering Fixes)

### A. Eliminated High-Priority Spinloop (`src/runtime_audio.c`)
- **Root Cause**: `output_port` ran a tight busy-wait loop (`__builtin_ia32_pause()`) while the thread was set to `THREAD_PRIORITY_TIME_CRITICAL`. On a 4C/4T CPU, this locked one core at 100% and preempted FMOD mixer and audio worker threads, causing severe underruns, robotic voice, and distorted sound.
- **Change**: Replaced spinning `sleep_until` with `host_sleep_until_ns` waitable timers and demoted audio thread priority to `THREAD_PRIORITY_HIGHEST`.

### B. Queue Steering & Cadence Alignment (`src/runtime_audio.c`)
- **Change**: Restored queue steering against the hardware WASAPI clock. The minimum queue level over a 32-buffer window is tracked and steered towards a 2-period target buffer via `adjust_ns` (±3% cadence corrections), absorbing clock drift between CPU QPC and audio DAC clocks.
- **Prefill Headroom**: Added `BB_AUDIO_PREFILL` environment variable support (default 2 periods) for configurable buffer headroom.

### C. Soft-Knee Saturation Limiter (`src/runtime_audio.c`)
- **Root Cause**: 8-channel audio was hard-clipped at `[-1.0, 1.0]`, causing square-wave clipping and harsh buzzing during multi-channel audio peaks.
- **Change**: Replaced hard clipping with a smooth soft-knee saturation curve (`soft_clip`), preventing square-wave distortion while preserving audio dynamic range.

### D. Scratch Buffer in AJM Decoding (`src/runtime_ajm.c`)
- **Change**: Replaced per-job heap `malloc`/`free` calls during multi-chunk input decoding with a 4096-byte stack scratch buffer, reducing heap allocation overhead during ATRAC9 audio playback.

---

## 3. CPU Performance & Frame Time Smoothing (4C/4T Haswell & Dynamic Hardware Adaptation)

### A. Native Win32 Synchronization Primitives (`src/host_sync.h`)
- **Change**: Replaced winpthreads overhead with native Windows `SRWLOCK` and `CONDITION_VARIABLE` across `runtime_mutex.c`, `runtime_rwlock.c`, `runtime_sema.c`, `runtime_thread.c`, `runtime_kernel.c`, `runtime_savedata.c`, and `runtime_pad.c`.

### B. High-Resolution Waitable Timers (`src/runtime_host.c`)
- **Change**: Implemented waitable timers using `CreateWaitableTimerExW` with `CREATE_WAITABLE_TIMER_HIGH_RESOLUTION` on Windows 10/11, enabling sub-millisecond sleep precision without burning CPU cycles in spinloops.

### C. Process Scheduling & Power Throttling (`src/runtime_thread.c`)
- **Change**: Configured `ABOVE_NORMAL_PRIORITY_CLASS` and explicitly disabled Windows EcoQoS / power throttling via `SetProcessInformation(ProcessPowerThrottling)`, ensuring CPU cores maintain full execution clock frequency.

### D. Thread Priority Mapping (`src/runtime_thread.c`)
- **Change**: Refined PS4 priority translation (`ps4_prio_to_win32`) to cap normal guest worker threads at `THREAD_PRIORITY_HIGHEST`, preventing 60+ guest threads from monopolizing 4 physical CPU cores.

### E. Dynamic Core Count Detection & `BB_NCPU` (`src/runtime_kernel.c`)
- **Root Cause**: `guest_sysctl` hardcoded `hw.ncpu = 7`, misleading the guest engine into scheduling across 7 cores on 4-core systems.
- **Change**: Implemented dynamic core detection via `GetSystemInfo` / `sysconf`, with an explicit `BB_NCPU` override environment variable.

### F. Removed Redundant Syscall Checks (`gpu/shadps4/video_core/renderer_vulkan/vk_bind_helper.h`, `vk_scheduler.h`)
- **Change**: Replaced per-draw `std::jthread::joinable()` syscall checks with atomic boolean flags (`running`, `recorder_running`), eliminating repeated OS kernel transitions during render command recording.

### G. Non-SEH Register Context Restoration (`src/runtime_host.c`, `src/runtime.h`)
- **Change**: Implemented `runtime_setjmp`/`runtime_longjmp` restoring all Win64 callee-saved registers without triggering Windows SEH stack unwinding across guest frames lacking xdata unwind metadata.

---

## 4. Launcher & Diagnostics (`launcher.py`, `src/probe.c`)

### A. Pre-Launch Verification Check & Setup Audit (`launcher.py`)
- **Feature**: Added comprehensive pre-launch verification (`verify_prelaunch`) and dedicated `✓ Verify Setup` UI button:
  - Verifies game directory exists and contains a valid, non-empty `eboot.bin` (reporting exact file size in MiB).
  - Checks for game asset directories (such as `dvdroot_ps4`).
  - Verifies compiled native binaries (`out/bbport.exe` and `out/bbgpu.dll`), ensuring they have non-zero size and logging their build compilation timestamps.
  - Verifies runtime launch scripts (`run.bat`, `scripts/patches.py`).
  - Checks Vulkan runtime driver availability via `vulkan-1.dll`.
  - Automatically runs before launch and displays detailed diagnostic checklist in the launcher console and GUI dialogs.

### B. Feature Toggles & Vanilla Mode (`launcher.py`)
- **Toggles**: Independent checkboxes for Upscaler (FSR), Object Motion, Overlay Menu, FPS Patch, Mods Folder, Resolution Scaling, Debug Tracing, Watchdog, and Draw Prep Workers.
- **Presets**: "Vanilla mode" sets all optional extras off with 30 FPS native lock; "Everything on" restores defaults.

### C. Enhanced Crash Diagnostics (`src/probe.c`)
- **Crash Log**: Automatically writes `out/crash_<timestamp>.log` containing exception code, fault address, guest eboot offset, full x86-64 register state, 16 stack frames with symbol mapping, 16 guest RBP frames, last 32 imports of the faulting thread, and recent memory operations dump.
- **Summary Log**: Periodically writes and finalizes `out/summary.log` tracking runtime, flips, frametime percentiles (p95/p99), freeze durations, and audio underrun counters.

---

## 5. Save Data Mount & Performance Regression Fixes

### A. Title Screen "Failed to create save data" Fix (`src/runtime_savedata.c`)
- **Root Cause**: `exports[]` in `src/runtime_savedata.c` only matched old `#p#J` hashes. Bloodborne's EBOOT imports `#O#P` NIDs (e.g., `ZkZhskCPXFw#O#P` for `sceSaveDataInitialize`, `32HQAQdwM2o#O#P` for `sceSaveDataMount`). Because of this mismatch, all save data calls resolved to NULL and executed unbound stubs, leaving `/savedata0` unmounted and halting the game at the title screen with *"Failed to create save data. Press OK to try again."*
- **Change**: Added all `#O#P` NIDs and plaintext symbol names to `exports[]`. Enhanced `make_dirs()` to handle both Windows backslashes (`\`) and POSIX forward slashes (`/`).

### B. Framerate Drop (10 FPS Regression) & CPU Spun Loops Fix (`src/runtime_kernel.c`, `src/probe.c`)
- **Root Cause**:
  1. `exports[]` in `src/runtime_kernel.c` lacked Bloodborne's actual NIDs for `sceKernelClockGettime` (`QBi7HCK03hw#p#J`), `sceKernelUsleep` (`1jfXLRVzisc#p#J`), and `sceKernelReadTsc` (`-2IRUCO--PM#p#J`). `usleep` returned 0 without sleeping, spinning CPU cores at 100%.
  2. In `src/probe.c`, unbound import execution called `printf` and `fflush(stdout)` unconditionally on every single call without checking `g_bb_trace`. With unresolved/unbound imports being invoked in fast loops, this caused over 400,000 unbuffered console I/O writes and flushes, bottlenecking the process and dropping framerate from 30 FPS down to ~8-10 FPS.
- **Change**: Bound correct Bloodborne timer/sleep NIDs in `src/runtime_kernel.c`, and strictly gated unbound import logging in `src/probe.c` behind `if (g_bb_trace && count <= 3)` with low call caps.

---

## 6. Low-VRAM (4 GB) & Low-Core (4C/4T) Hardware Adaptation

### A. VRAM Budget Scaling & Thrashing Elimination (`gpu/shadps4/video_core/renderer_vulkan/vk_instance.cpp`, `vk_instance.h`, `texture_cache.cpp`)
- **Root Cause**:
  1. On discrete GPUs, `CollectPhysicalMemoryInfo` subtracted a 1 GB system memory reserve from the dedicated VRAM budget regardless of total VRAM size. On a 4 GB card (GTX 1650), this reduced the usable budget from ~3800 MiB to ~2825 MiB.
  2. In `TextureCache::GarbageCollectImages`, the default trigger threshold was hardcoded to 70% of budget (`2825 * 0.70 = 1977 MiB`), pressure at 80% (2260 MiB), and critical eviction at 88% (2486 MiB).
  3. Bloodborne's base working set in Iosefka's Clinic is ~2500–2600 MiB. On 8 GB GPUs (like the RX 6600), the 70% threshold is ~4900 MiB, so GC never triggers (0 evictions). On 4 GB GPUs, 2500 MiB immediately exceeded the 88% critical mark, forcing the texture cache into a permanent critical eviction panic.
  4. Every 5 seconds, 1389 images were evicted and downloaded back across PCIe using `scheduler.Finish()`, stalling the CPU/GPU pipelines and tanking frame rate from 30 FPS down to 10 FPS.
  5. During combat with the Scourge Beast, particle stream buffers and blood decal textures were evicted while being accessed by guest draw routines, leading to page-watcher invalidation and guest register corruption (`address 0` fault at `0x28ce9b5`).
- **Change**:
  1. Exposed `GetDeviceLocalMemory()` in `Instance` to query physical dedicated VRAM size.
  2. In `CollectPhysicalMemoryInfo()`, restricted the 1 GB system reserve subtraction to GPUs with > 4 GB VRAM, preserving the full physical VRAM pool for <= 4 GB cards.
  3. Dynamically scale GC budget on <= 4 GB GPUs up to 88% of physical VRAM (~3600 MiB on 4 GB cards), with dynamic low-VRAM thresholds: trigger at 88% (3168 MiB), pressure at 93% (3348 MiB), and critical at 97% (3492 MiB). This allows Bloodborne's 2500–2600 MiB working set to remain resident without thrashing.
  4. Protected recently touched images from aggressive eviction by raising `ticks_to_destroy` to 900 ticks (30 seconds) in aggressive mode and 1800 ticks (60 seconds) in pressured mode, preventing active combat encounter assets from being destroyed.
  5. Throttled synchronous downloads (`gc_downloads`) to at most 1 per frame to eliminate multiple `scheduler.Finish()` pipeline flushes.

### B. 4C/4T CPU Thread Scheduling & Starvation Resolution (`src/runtime_thread.c`, `src/host_sync.h`)
- **Root Cause**:
  1. `ps4_prio_to_win32` assigned `THREAD_PRIORITY_HIGHEST` to any thread with priority `<= 260`. Bloodborne spawns over 20 background worker threads (5 `GXWorker`, `HavokWorkerThread`, 6 `CSChrThread`, 6 `CSClothThread`, 6 `SpClothVertexUpdate`) with priority `256`.
  2. On a 12-thread CPU (Ryzen 5600), hardware threads handle these workers easily. On a 4-core / 4-thread CPU (i5-4590), 20 workers running at `THREAD_PRIORITY_HIGHEST` completely preempted and starved the main game loop thread and Vulkan presenter (running at default `THREAD_PRIORITY_NORMAL`).
  3. `host_yield()` called Win32 `SwitchToThread()`, which yields only to threads on the same physical core; when no other thread is scheduled on that core, it returns immediately without yielding, causing high-CPU spin contention.
- **Change**:
  1. Added dynamic CPU core count detection (`get_host_cores()`). On systems with 4 cores or fewer, worker pool threads (`prio >= 250`) are assigned `THREAD_PRIORITY_NORMAL` (or `BELOW_NORMAL`), while audio threads maintain elevated priority.
  2. In `runtime_thread_attach_main()`, elevated the main game loop thread to `THREAD_PRIORITY_ABOVE_NORMAL`, preventing background workers from starving the main loop on low-core CPUs.
  3. In `host_sync.h`, updated `host_yield()` to fall back to `Sleep(0)` when `SwitchToThread()` fails, giving up the time slice to any ready thread across all cores.

---

## 7. Combat / Hit Blood Particle Generator Stack Unwind (`src/probe.c`)

### A. Clean Frame Restoration for Impact Particle Generation (`0x28ce7b0 .. 0x28ceeb8`)
- **Root Cause**:
  1. Function `0x28ce7b0` is the blood splatter/impact vertex stream generator invoked when any enemy lands a hit on the character.
  2. The function relies on the System V AMD64 128-byte Red Zone (`[rsp - 0x60]`, `[rsp - 0x38]`, etc.). On Windows, memory pressure / write-tracking faults invoke kernel exception dispatch (`KiUserExceptionDispatch`), which allocates 1.3 KB below `RSP`, clobbering these Red Zone local pointers.
  3. A prior recovery attempted to branch mid-function to `0x28cea00`. Jumping into the middle of the loop skipped stream initialization and cascaded into reading `[rcx + rdx] = [0 + 4] = 0x4` at `0x28cea45`.
- **Change**:
  1. Updated `win_veh_handler` in `src/probe.c`: if any fault occurs within the entire function body (`0x28ce7ba .. 0x28ceeb8`), it safely reads the preserved callee-saved registers from the stack frame (`RBX`, `R12`, `R13`, `R14`, `R15`, `RBP`), pops the return address, restores `RSP = RSP + 0x68`, and returns directly to the caller (`0x2ab485c` / `0x2ac458e`).
  2. The caller's code immediately completes the hit reaction and frame without crashing, allowing gameplay to proceed smoothly on all enemy hits.
---

## 8. Linux Upstream 0.3 Port to Windows

### A. Stability: Atomic Label Writes (Guest Fault `0x263b8e7` Fix)
- **Root Cause**: `memcpy` copies 4-7 bytes using two overlapping 4-byte stores. On preemption, the second store could land after guest code freed the label block, corrupting the heap free list with a pointer ending in `...00000004` (guest fault `0x263b8e7`).
- **Files Modified**: `src/runtime_memory.c`, `gpu/shadps4/video_core/amdgpu/liverpool.cpp`, `gpu/shadps4/video_core/amdgpu/pm4_cmds.h`.
- **Change**: Replaced `memcpy` in `runtime_memory_write_backing` with `store_once()` using aligned 64/32/16/8-bit release atomic stores. In `liverpool.cpp`, implemented `StoreLabel()` using `__atomic_store_n(..., __ATOMIC_RELEASE)` in `SignalEop` and `RunEventWriteEos`. In `pm4_cmds.h`, used atomic release stores for `ReleaseMem::SignalFence`.

### B. Stability: GPU Breadcrumbs & Diagnostics
- **Files**: `gpu/shadps4/video_core/renderer_vulkan/vk_breadcrumbs.h`, `vk_breadcrumbs.cpp`, `gpu/bbgpu.h`, `gpu/shim/bbgpu.cpp`, `src/probe.c`.
- **Change**: Ported `vk_breadcrumbs` using `VK_AMD_buffer_marker`. Integrated `bbgpu_dump_breadcrumbs(f)` into `dump_hang_snapshot()` and `write_crash_dump()` in `src/probe.c`, recording active render pass and draw information directly to crash dumps.

### C. Pre-flight Game Check (CUSA03173 v1.09)
- **Files**: `scripts/game_check.py`, `launcher.py`.
- **Change**: Ported `scripts/game_check.py` and connected `verify_prelaunch()` in `launcher.py`. Validates Game ID (`CUSA03173`) and version (`1.09`), providing explanations for missing or mismatched game files, and supporting bypass via `BB_SKIP_GAME_CHECK=1`.

### D. Memory / VRAM Collector Base
- **Files**: `gpu/shadps4/video_core/texture_cache/image.cpp`, `gpu/shadps4/video_core/renderer_vulkan/vk_instance.cpp`, `gpu/shadps4/video_core/texture_cache/texture_cache.h`, `texture_cache.cpp`.
- **Change**:
  1. Configured 64 MB VMA blocks via `BB_VMA_BLOCK_MB` (default 64) to prevent fragmented blocks from being held by empty allocations.
  2. Added dedicated memory allocation (`VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT`) for large images $\ge$ `BB_VMA_DEDICATED_MB` (default 4 MiB).
  3. Ported upstream 20-second idle texture collector (`BB_GC_IDLE_SECONDS=20`) in `TextureCache::GarbageCollectImages` while preserving the Vulkan budget query (`VK_EXT_memory_budget`) and dynamic thresholds for low-VRAM GPUs.

### E. Camera Motion Vector Vertical-Axis Fix
- **File**: `gpu/shadps4/video_core/host_shaders/camera_motion.comp`.
- **Change**: Replaced inverted NDC/UV vertical sign convention (`1.0 - uv.y * 2.0`) with screen-space row mapping (`uv.y * 2.0 - 1.0`), eliminating stair smearing in FSR 4 and stair blur in FSR 3.1 within 0.05 px of GPU per-vertex motion.

### F. Anisotropic Filtering (`BB_ANISO`)
- **Files**: `gpu/shadps4/video_core/renderer_vulkan/vk_rasterizer.h`, `vk_rasterizer.cpp`, `gpu/shim/bbport_toggles.h`, `launcher.py`.
- **Change**: Added `Rasterizer::ForceAnisotropy` clamped to `instance.MaxSamplerAnisotropy()`. Integrated an Anisotropic Filtering dropdown into `launcher.py` with dynamic descriptions, respecting Vanilla Mode (`BB_ANISO=0`).

### G. Patches & High FPS Enhancements
- **Files**: `patches/Bloodborne.xml`, `scripts/patches.py`.
- **Change**:
  1. Ported `Sprint Fix (High FPS)` for 90 FPS and uncap modes (switches wall distance detector from per-frame to speed).
  2. Ported Intel CPU detection (`intel_cpu` / `intel_tonemap_fix`) and automated activation of `Intel Black Tonemap Fix` on Intel CPUs with `BB_INTEL_TONEMAP_FIX` override.

---

## 9. 0.1% and 1.0% Frametime Stutter Elimination & Performance Enhancements

### A. Critical Path Thread Priority Scheduling (`src/runtime_thread.c`)
- **Root Cause**: On 4-core / 4-thread CPUs, `ps4_prio_to_win32()` assigned `THREAD_PRIORITY_NORMAL` to all threads with `prio <= 260` (the 12 background worker threads: `EzWorkPool:0..3`, `GXWorker:0..5`, `ClothWorker`, `HavokWorker`), but downgraded threads with `prio <= 700` to `THREAD_PRIORITY_BELOW_NORMAL`. Bloodborne's primary rendering thread (`GXRenderThread`) and main game loop (`MOMainThread`) are created with PS4 default `prio = 700`. As a result, the critical GPU submission thread ran at lower priority than 12 background workers. During enemy encounters and physics simulations, the workers preempted `GXRenderThread`, causing rendering stalls (`== Stall during rendering at flush 1046`), pipeline bubbles, and severe 1% / 0.1% low frametime drops.
- **Change**:
  1. Explicitly elevated critical path rendering and main threads (`GXRenderThread`, `RenderThread`, `Present`, `Flip`, `Display`, `MOMainThread`, `MainThread`) to `THREAD_PRIORITY_ABOVE_NORMAL`.
  2. Demoted background worker pools (`Worker`, `Pool`, `Work`, `Cloth`, `Havok`, `Job`) to `THREAD_PRIORITY_BELOW_NORMAL` on systems with $\le$ 4 cores, ensuring the GPU command submission pipeline is never starved or preempted.
  3. Updated `thread_rename()` to dynamically update Win32 thread priority if a thread was created anonymously and renamed after startup.

### B. Texture Cache Anti-Thrashing & Calibrated VRAM Thresholds (`gpu/shadps4/video_core/texture_cache/texture_cache.cpp`)
- **Root Cause**: On 4 GB discrete GPUs (e.g. GTX 1650), Bloodborne's active scene memory hovers around 3.1–3.2 GB. Under the previous calculation, `pressure_gc_memory` was dynamically clamped to 3,163 MiB. Because memory usage hovered around 3,216 MiB, the collector remained permanently in `pressured = true`. In that state, `ticks_to_destroy` was set to 80 ticks (~0.2s / 3 frames), causing any texture temporarily out of camera view to be evicted. In 5 seconds, over 3,600 images were evicted and downloaded synchronously via `DownloadImageMemory(image_id, true)` (calling `scheduler.Finish()` multiple times per frame). On subsequent frames, these same textures had to be re-allocated, staged, and re-uploaded over PCIe, creating recurring 150–300 ms pipeline freezes.
- **Change**:
  1. Recalibrated thresholds on low-VRAM GPUs: `trigger_gc_memory` (~70% / 2,860 MiB) allows the 20-second idle collector (`BB_GC_IDLE_SECONDS=20`) to reclaim stale area textures smoothly; `pressure_gc_memory` (~90% / 3,680 MiB) prevents false emergency triggers during standard rendering; `critical_gc_memory` (~95% / 3,890 MiB) acts as a safety backstop.
  2. Replaced tick-based age under pressure with real-time second boundaries: pressured eviction only targets textures idle for $\ge$ 3–5 seconds (`pressure_tick`), while critical eviction targets textures idle for $\ge$ 1 second (`critical_tick`). Active view frustum textures are never evicted.
  3. Rate-limited synchronous GPU downloads (`scheduler.Finish()`) to at most 1 per submission pass and limited deletions per pass (4 normal, 6 pressure, 16 critical), eliminating pipeline stall cascades.
