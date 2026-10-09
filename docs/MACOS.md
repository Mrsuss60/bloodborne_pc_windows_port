# macOS (Apple Silicon) build

**Status: builds and passes the runtime tests; the game does not run yet** (see
[Known blockers](#known-blockers)).

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

## Build and test

```bash
bash build.sh --test
VK_DRIVER_FILES=/path/to/MoltenVK_icd.json out/bb-probe --vulkan-only
ninja -C out/gpu shader-user-data-test motion-history-test ui-composition-test \
    upscaler-support-test motion-shader-test
```

Verified on an M1 Pro (macOS 26.6, MoltenVK 1.4.2):

- `build.sh --test`: pad, runtime (including the TSD thread pointer on two threads),
  file-mods, semaphore and content tests pass.
- `--vulkan-only`: command submission and readback on the Apple GPU pass.
- GPU tests: the five above pass. `scene-resolution-test`, `taa-shader-test` and
  `camera-motion-test` stop at device creation (next section).

## Known blockers

- **`nullDescriptor`**: the renderer requires `VK_EXT_robustness2`'s `nullDescriptor`.
  MoltenVK 1.4.2 does not provide it, so Vulkan device creation fails. Running the game needs
  a fallback (dummy buffers/images where null descriptors are bound).
- **Geometry shaders**: Metal has none. shadPS4 emulates some stages; untested here.
- `robustBufferAccess2`/`robustImageAccess2` are only enabled where the driver has them
  (MoltenVK has no `robustBufferAccess2`; Metal bounds accesses itself). This is a behaviour
  difference from the other platforms.
- FSR 4 and other features that depend on specific GPU vendors are not expected to work on
  Apple GPUs.
- Not tested with the game: the CPU side (loader, memory, thread pointer) is covered only
  by the tests above.
