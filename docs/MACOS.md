# macOS (Apple Silicon) build

**Status: runs the game on an M1 Pro (16 GB): 1080p at full detail holds the game's 30 FPS in
most areas, ~21 FPS in the heaviest geometry** (see [Performance](#performance) and
[Known limits](#known-limits)).

## Why an x86-64 build under Rosetta 2

The port runs the game's own x86-64 code natively, inside the loader's process. On Apple
Silicon only Rosetta 2 can execute it, and Rosetta translates whole processes: the loader,
the runtime, the GPU library and every dependency are built for x86-64. Rendering still runs
natively on the Apple GPU: Vulkan goes through MoltenVK to Metal.

Rosetta executes AVX/AVX2; `run.sh` sets `ROSETTA_ADVERTISE_AVX=1` so CPUID reports them too.

## How the port differs from Linux

| Linux | macOS |
|---|---|
| Guest pool: one `memfd`, mapped at any offset | A named Mach memory entry (`mach_make_memory_entry_64`), mapped with `mach_vm_map` |
| Guest ranges from `0x1000000000` (below 1 TiB) | Apple Silicon reserves `0x1000000000`–`0x7000000000` for the GPU, also in Rosetta processes: host-owned guest memory starts at `0x7000000000`, the guest user range at `0x7800000000` |
| Released direct memory: `fallocate` punches a hole | Pages that hold data (per `mincore`) are zeroed, then released with `MADV_FREE_REUSABLE` |
| `mov rax, fs:[0]` rewritten to `gs:[0]`, GS base = guest TCB (`arch_prctl`) | GS is the pthread TSD array: the loader points those loads at `gs:[slot*8]`, a TSD slot holding the guest TCB (`patch_tls_loads`, `darwin_compat.c`) |
| `ucontext` `gregs`, `/proc/self/task` + `tgkill` | `uc_mcontext->__ss`, `task_threads` + `pthread_kill` |

The other Darwin shims (clocks, timed locks, per-thread rusage, barriers) are in
`src/darwin_compat.h`.

**Threads**: Cocoa runs windows on the process's main thread only. On Linux and Windows the
game runs on the main thread and the window on a thread of its own; on macOS the loader starts
the game on a thread of its own (`run_guest`) and the main thread keeps the window: `bbgpu_init`
creates it there, as a Metal view (SDL needs no Vulkan support), and `bbgpu_run_window_loop`
runs its events.

On the GPU side:

- **No `nullDescriptor` on MoltenVK.** `NullResources` (`vk_null_resources.h`) stand in: a
  zeroed buffer, and zeroed 1×1 images with a view for each image type (RGBA8; D32 for depth
  slots, which comparison sampling turns into Metal depth textures; 4-sample images for
  multisampled slots, which Metal declares as `texture2d_ms`). `null-resources-test` reads
  every stand-in through the type a shader declares; with `MTL_DEBUG_LAYER=1` Metal also
  validates each binding's texture type. The rasterizer binds them
  where it would write null buffers, vertex buffers or image views. Elsewhere `nullDescriptor`
  stays required.
- **No sparse residency on MoltenVK.** The buffer cache keeps guest memory in sparse 4 GiB
  arenas elsewhere. Here the arenas are dense buffers of `BB_ARENA_MB` (256 MiB by default;
  Metal commits a buffer whole on its first GPU use, so 4 GiB arenas would cost 4 GiB each).
  A binding that spans several arenas merges them into one (contents copied, the old ones
  destroyed once the GPU is past them; `arena-span-test` covers which arenas merge);
  `launcher.log` shows `GPU: merged … buffer arenas`. With little memory, `BB_ARENA_MB=64`.
- **`robustBufferAccess2`/`robustImageAccess2`** are enabled only where the driver has them
  (MoltenVK has no `robustBufferAccess2`; Metal bounds accesses itself).
- **Portability**: the instance enables `VK_KHR_portability_enumeration`, the device
  `VK_KHR_portability_subset`.
- **No push descriptors.** MoltenVK 1.4 puts the push constants at the Metal buffer index of a
  push descriptor set's buffers once the set has 11 or more bindings, so the shader library
  fails to compile (`cannot reserve 'buffer' resource location at index 0`), with or without
  argument buffers. Pipelines use regular descriptor sets instead (`MaxPushDescriptors()` is 0).
- **No `NoContraction` in shaders.** The recompiler marks every float add, subtract, multiply
  and fma `NoContraction` (GCN does not fuse them). SPIRV-Cross, inside MoltenVK, implements it
  by calling an `[[clang::optnone]]` helper for each of those operations, so Metal compiled
  every game shader unoptimized: 5-50x slower (G-buffer 46 ms → 2 ms, lighting 40 ms → <1 ms
  at 720p). On macOS `Profile::skip_no_contraction` leaves the decoration out, and the vertex
  position is declared `Invariant` (`[[position, invariant]]`) so depth pre-passes and the
  passes drawing the same meshes compute it identically; without that, lighting flickers. The
  shader cache version differs on macOS (`ShaderBinaryVersion` 0x108) so caches made for
  other GPUs are not reused.
- **Draws with no effect are skipped** (`Rasterizer::DrawHasNoEffect`): no color or depth
  target, no pixel shader, no stores (occlusion queries are answered on the CPU). The game
  issues some with a 16384x16384 render area, which a tile-based GPU walks whole (~1 ms each).
- **The dispatcher starts from the Vulkan loader `libbbgpu` links** (`vkGetInstanceProcAddr`
  bound at link time). Vulkan-Hpp's `DynamicLoader` may find MoltenVK itself, which also
  exports `vkGetInstanceProcAddr` but cannot take the loader's handles.
- Code built with other Vulkan platform macros than `vk_platform.cpp` (the GPU tests) must not
  read fields of `VULKAN_HPP_DEFAULT_DISPATCHER`: its layout differs between them. The tests
  call the loader's `vkGetInstanceProcAddr` instead.

## Dependencies

1. Xcode Command Line Tools (`xcode-select --install`), CMake, Ninja, and from the (arm64)
   Homebrew: `brew install cmake ninja pkgconf glslang nasm`.
2. x86-64 libraries from [vcpkg](https://github.com/microsoft/vcpkg). Homebrew no longer
   installs as x86-64 on Apple Silicon.

   ```bash
   git clone https://github.com/microsoft/vcpkg.git && ./vcpkg/bootstrap-vcpkg.sh -disableMetrics
   ./vcpkg/vcpkg install --triplet x64-osx --host-triplet arm64-osx \
       sdl3 vulkan-loader vulkan-headers fmt magic-enum robin-map vulkan-memory-allocator \
       xxhash miniz zydis xbyak "ffmpeg[avcodec,avformat,swscale,swresample]" \
       boost-headers boost-cmake boost-asio boost-container boost-container-hash boost-icl \
       boost-intrusive boost-pool boost-preprocessor
   export VCPKG_ROOT=$PWD/vcpkg   # or BB_MACOS_DEPS=<any x86-64 prefix>
   ```

3. MoltenVK: a universal (x86-64 + arm64) build from the
   [Khronos releases](https://github.com/KhronosGroup/MoltenVK/releases) (`MoltenVK-macos.tar`,
   `MoltenVK/dynamic/dylib/macOS/`). Either copy `libMoltenVK.dylib` and `MoltenVK_icd.json`
   next to `out/bb-probe`, or point the loader at the manifest:
   `export BB_MOLTENVK_ICD=/path/to/MoltenVK_icd.json` (`run.sh`; `VK_DRIVER_FILES` works too).

## Launcher

Double-click `launch_gui.command` in Finder (or run `python3 launcher.py`; Python 3.10+ with
Tkinter, as in the python.org installer). It is the same launcher as on Windows, running
`run.sh`; its **macOS Setup** box takes the vcpkg folder and `MoltenVK_icd.json` (filled in when
they sit next to this repository, as in the steps above). **Verify Setup** checks Rosetta 2,
the dependencies, MoltenVK (it asks the GPU through Vulkan) and the game folder (CUSA03173 with
update 1.09). The first launch builds `bb-probe`.

From a terminal instead:

```bash
BB_GAME_DIR=~/Games/CUSA03173 VCPKG_ROOT=~/Documents/GitHub/vcpkg \
BB_MOLTENVK_ICD=/path/to/MoltenVK_icd.json bash run.sh
```

## Build and test

```bash
bash build.sh --test
VK_DRIVER_FILES=/path/to/MoltenVK_icd.json out/bb-probe --vulkan-only
ninja -C out/gpu shader-user-data-test motion-history-test ui-composition-test \
    upscaler-support-test motion-shader-test scene-resolution-test taa-shader-test \
    camera-motion-test null-resources-test arena-span-test
ninja -C out/gpu window-test && (cd out/gpu && ./window-test)   # opens a window for 3 s
```

Verified on an M1 Pro (macOS 26.6, MoltenVK 1.4.2):

- `build.sh --test`: pad, runtime (including the TSD thread pointer on two threads),
  file-mods, semaphore and content tests pass.
- `--vulkan-only`: command submission and readback on the Apple GPU pass.
- GPU tests: all nine above pass on the Apple GPU (scene targets, TAA and camera motion
  shaders, null descriptor stand-ins sampled by a compute shader, also under
  `MTL_DEBUG_LAYER=1`).

## Performance

The log prints `Perf:` every 5 s: FPS, draws, dispatches, uploads, vertices and render passes
per frame. **fn+F10** opens the in-game menu (Mac keyboards have no Insert key); the launcher's
**Show FPS counter** puts FPS and frame time in a corner, and **Recommended (Mac)** applies
1080p, 30 FPS and full detail.

Measured on an M1 Pro at 1080p, full detail: 30 FPS (the game's own rate) in most areas; ~21 FPS
at the clinic spawn, where the frame has ~1,500 draws and ~19 M vertices. There the GPU is the
limit (94-98% busy, ~48 ms per frame) and the draw recording thread is ~80% busy. The cost
follows the vertex count: the recompiled vertex shaders read per-instance and skinning data
with ~40 separate 32-bit buffer loads per vertex (offsets known only at run time, so Metal
cannot merge them into vector loads).

Tools:

- `BB_GPU_PROFILE=1`: GPU ms per frame for each render pass and dispatch, every 5 s. MoltenVK
  samples GPU timestamps only in query pools of up to 4096 queries (larger pools fall back to
  one CPU time per finished command buffer), so the profiler uses 4 x 1024 on macOS.
- `BB_PASS_TRACE=1`: why render passes end, per frame (tile-based GPUs store and reload the
  attachments at every end), including passes split and resumed on the same targets.
- `MVK_CONFIG_SHADER_DUMP_DIR=<dir>`: the MSL MoltenVK generates for each shader. The SPIR-V
  carries the recompiler's hash as a string, so `strings` finds a shader's dump.

Tried without gain: `MVK_CONFIG_FAST_MATH_ENABLED=1` (+5%, risks NaN/Inf differences),
`MVK_CONFIG_USE_MTLHEAP=0` (MoltenVK then cannot create block-texel-view images), lower model
LOD (the heaviest geometry is instanced props and shadow maps).

## Known limits

- **Heavy geometry is GPU-bound** (see [Performance](#performance)): vector loads for the
  vertex shaders' buffer reads are the next step.
- **Skip Intro**: the patch tears the intro movie down while it starts; leave it off.
- **Geometry shaders**: Metal has none, and the renderer does not emulate them: draws with an
  ES/GS stage are skipped (nothing they draw appears). Each skipped ES/GS program is logged
  once (`Geometry stage unsupported by the device: skipping draws of ES … GS …`) and the
  total is printed at exit (`GPU: N draws/dispatches skipped for geometry stages`). Whether Bloodborne
  uses them, and so whether emulation (ES/GS run as compute, the output drawn from a buffer)
  is worth writing, is unknown until the game runs.
- **Shaders that write multisampled images** (stores or atomics on an MSAA image): Metal cannot
  write `texture2d_ms` (MoltenVK: `shaderStorageImageMultisample` = 0), so MoltenVK fails to
  build such a pipeline and the renderer would assert. Those draws and dispatches are skipped
  instead, logged once per shader (`Multisampled storage image unsupported by the device:
  skipping …`) and counted at exit (`GPU: N draws/dispatches skipped for multisampled storage
  images`). Reading MSAA images works. Emulating the writes would mean storing MSAA images
  differently in the texture cache; worth it only if the game turns out to need it.
- FSR 4 and other features that depend on specific GPU vendors are not expected to work on
  Apple GPUs.
