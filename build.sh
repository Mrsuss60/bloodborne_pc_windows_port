#!/usr/bin/env bash
set -euo pipefail
cd -- "$(dirname -- "$0")"
mkdir -p out
# macOS: an x86-64 build run by Rosetta 2 on Apple Silicon (the game's own x86-64 code runs
# in-process). Dependencies: an x86-64 prefix, BB_MACOS_DEPS, by default vcpkg's x64-osx tree
# ($VCPKG_ROOT/installed/x64-osx; see docs/MACOS.md).
darwin=0
if [[ $(uname -s) == Darwin ]]; then
    darwin=1
    deps=${BB_MACOS_DEPS:-${VCPKG_ROOT:+$VCPKG_ROOT/installed/x64-osx}}
    if [[ -z $deps || ! -d $deps/lib ]]; then echo 'Set BB_MACOS_DEPS (or VCPKG_ROOT) to the x86-64 dependency prefix.' >&2; exit 1; fi
    # Only that prefix: the arm64 Homebrew's libraries cannot link into this build.
    export PKG_CONFIG_LIBDIR=$deps/lib/pkgconfig:$deps/share/pkgconfig
    CC=${CC:-clang}
    arch_flags=(-arch x86_64)
fi
if [[ -z ${CC:-} ]]; then
    CC=$(command -v cc || command -v gcc || true)
    if [[ -z $CC ]]; then
        for candidate in /nix/store/*-gcc-wrapper-*/bin/gcc; do
            if [[ -x $candidate ]]; then CC=$candidate; break; fi
        done
    fi
fi
if [[ -z ${CC:-} ]]; then echo 'Install GCC/Clang or set CC.' >&2; exit 1; fi
# Dependencies come from pkg-config (Vulkan loader/headers, SDL3). On NixOS the
# environment is provided by shell.nix; re-enter it automatically if needed.
if ! { command -v pkg-config >/dev/null && pkg-config --exists vulkan sdl3 && command -v cmake >/dev/null && command -v ninja >/dev/null; }; then
    if [[ -z ${BB_IN_NIX_SHELL:-} ]] && command -v nix-shell >/dev/null; then
        exec env BB_IN_NIX_SHELL=1 nix-shell shell.nix --run "bash build.sh $*"
    fi
    echo 'Need pkg-config with vulkan and sdl3, cmake and ninja (see shell.nix).' >&2; exit 1
fi
read -r -a includes <<< "$(pkg-config --cflags vulkan sdl3)"
# Static dependency builds (vcpkg on macOS) need their private libraries and frameworks too.
static=(); if (( darwin )); then static=(--static); fi
read -r -a libraries <<< "$(pkg-config --libs ${static[@]+"${static[@]}"} vulkan sdl3)"
# The Vulkan loader is a shared library in the dependency prefix.
if (( darwin )); then libraries+=(-Wl,-rpath,"$deps/lib"); fi
# GPU library (shadPS4 video core + drivers), built by CMake into out/gpu/libbbgpu.so.
# BB_PGO: generate (instrumented build that writes pgo/ while the game runs), use, off.
# Default: use the profile in pgo/ when there is one. BB_LTO=OFF disables link-time optimization.
pgo=${BB_PGO:-}
if [[ -z $pgo ]]; then
    if [[ -n $(find pgo -name '*.gcda' -print -quit 2>/dev/null) ]]; then pgo=use; else pgo=off; fi
fi
mkdir -p pgo
# Submodules (git clone --recursive, or: git submodule update --init) and this port's changes
# to FSR-Vulkan (gpu/patches/fsr-vulkan), applied to its working tree once.
if [[ ! -f gpu/third_party/fsr-vulkan/CMakeLists.txt || ! -f gpu/third_party/imgui/imgui.h ]]; then
    git submodule update --init --recursive
fi
for patch in gpu/patches/fsr-vulkan/*.patch; do
    if ! git -C gpu/third_party/fsr-vulkan apply --reverse --check "$PWD/$patch" 2>/dev/null; then
        git -C gpu/third_party/fsr-vulkan apply "$PWD/$patch"
    fi
done
cmake_platform=()
if (( darwin )); then
    # Only the x86-64 prefix: the arm64 Homebrew's libraries cannot link into this build.
    cmake_platform=(-DCMAKE_OSX_ARCHITECTURES=x86_64 -DCMAKE_PREFIX_PATH="$deps"
                    -DCMAKE_IGNORE_PREFIX_PATH=/opt/homebrew)
fi
cmake -S gpu -B out/gpu -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DBB_PGO="$pgo" \
    -DBB_LTO="${BB_LTO:-ON}" -DBB_PGO_DIR="$PWD/pgo" ${cmake_platform[@]+"${cmake_platform[@]}"} >/dev/null
echo "GPU library: PGO $pgo, LTO ${BB_LTO:-ON}"
# A failed GPU build must stop here: an older libbbgpu.so would otherwise be used silently.
if ! ninja -C out/gpu bbgpu > out/gpu-build.log 2>&1; then
    grep -v '^\[' out/gpu-build.log | tail -40 >&2
    echo 'GPU library build failed (full log: out/gpu-build.log)' >&2; exit 1
fi
# $ORIGIN/gpu: packaged copies keep the library next to the binary without patching it.
gpu=(-Lout/gpu -lbbgpu -Wl,-rpath,'$ORIGIN/gpu' -Wl,-rpath,"$PWD/out/gpu" -rdynamic)
runtime=(src/runtime*.c)
pie=(-no-pie)
if (( darwin )); then
    # Mach-O executables export their symbols (the GPU library resolves runtime_* in them).
    gpu=(-Lout/gpu -lbbgpu -Wl,-rpath,@executable_path/gpu -Wl,-rpath,"$PWD/out/gpu")
    runtime+=(src/darwin_compat.c)
    pie=()
fi
arch_flags=(${arch_flags[@]+"${arch_flags[@]}"})
# Third-party decoders: compiled once, without this project's -Werror policy.
atrac9=(third_party/LibAtrac9/C/src/*.c)
if [[ ! -f out/libatrac9.a || -n $(find third_party/LibAtrac9/C/src -newer out/libatrac9.a -name '*.c') ]]; then
    rm -rf out/atrac9 && mkdir -p out/atrac9
    for source in "${atrac9[@]}"; do "$CC" ${arch_flags[@]+"${arch_flags[@]}"} -std=c99 -O2 -g -w -c "$source" -o "out/atrac9/$(basename "${source%.c}").o"; done
    ar rcs out/libatrac9.a out/atrac9/*.o
fi
"$CC" ${arch_flags[@]+"${arch_flags[@]}"} -std=c11 -O2 -g -Wall -Wextra -Werror -pthread ${pie[@]+"${pie[@]}"} "${includes[@]}" -I. -Isrc src/probe.c "${runtime[@]}" src/vulkan_smoke.c out/libatrac9.a -lm "${gpu[@]}" "${libraries[@]}" -o out/bb-probe
echo "Built $PWD/out/bb-probe"
# GPU check for run.sh (live_resolution=auto): links only the Vulkan loader.
"$CC" ${arch_flags[@]+"${arch_flags[@]}"} -std=c11 -O2 -Wall -Wextra -Werror "${includes[@]}" tools/gpu_capabilities.c "${libraries[@]}" -o out/bb-gpu-capabilities
if [[ ${1:-} == --test ]]; then
    "$CC" ${arch_flags[@]+"${arch_flags[@]}"} -std=c11 -O2 -g -Wall -Wextra -Werror -pthread "${includes[@]}" -I. -Isrc tests/test_pad.c src/runtime_host.c "${libraries[@]}" -o out/pad-test
    out/pad-test
    "$CC" ${arch_flags[@]+"${arch_flags[@]}"} -std=c11 -O2 -g -Wall -Wextra -Werror -pthread "${includes[@]}" -I. -Isrc tests/test_runtime.c "${runtime[@]}" out/libatrac9.a -lm "${gpu[@]}" "${libraries[@]}" -o out/runtime-test
    out/runtime-test
    "$CC" ${arch_flags[@]+"${arch_flags[@]}"} -std=c11 -O2 -g -Wall -Wextra -Werror -pthread -Isrc tests/test_file_mods.c -o out/file-mods-test
    out/file-mods-test
    "$CC" ${arch_flags[@]+"${arch_flags[@]}"} -std=c11 -O2 -g -Wall -Wextra -Werror -pthread "${includes[@]}" -I. -Isrc tests/test_sema.c "${runtime[@]}" out/libatrac9.a -lm "${gpu[@]}" "${libraries[@]}" -o out/sema-test
    out/sema-test
    "$CC" ${arch_flags[@]+"${arch_flags[@]}"} -std=c11 -D_GNU_SOURCE -O2 -g -Wall -Wextra -Werror -I. -Isrc tests/test_content.c src/runtime_content.c -o out/content-test
    out/content-test
fi
