# Native Windows Port of Bloodborne

![Platform](https://flat.badgen.net/badge/PLATFORM/WINDOWS/0078D6?labelColor=1a0f0f&icon=windows)
[![Latest release](https://img.shields.io/github/v/release/Mrsuss60/bloodborne_pc_windows_port?display_name=tag&style=flat&logo=github&logoColor=white&labelColor=1a0f0f&color=8b0000&label=RELEASE)](https://github.com/Mrsuss60/bloodborne_pc_windows_port/releases)
[![Downloads](https://flat.badgen.net/github/assets-dl/Mrsuss60/bloodborne_pc_windows_port/total?label=DOWNLOADS&labelColor=1a0f0f&color=b22222)](https://github.com/Mrsuss60/bloodborne_pc_windows_port/releases)
[![Stars](https://flat.badgen.net/github/stars/Mrsuss60/bloodborne_pc_windows_port?label=STARS&labelColor=1a0f0f&color=c9a227&icon=github)](https://github.com/Mrsuss60/bloodborne_pc_windows_port/stargazers)
[![Last commit](https://flat.badgen.net/github/last-commit/Mrsuss60/bloodborne_pc_windows_port?label=LAST%20COMMIT&labelColor=1a0f0f&color=6b6b6b&icon=git)](https://github.com/Mrsuss60/bloodborne_pc_windows_port/commits)

* Discord Server: https://discord.gg/JdZsJ2ArZ2
* Download Latest Release: [v0.8.0](https://github.com/Mrsuss60/bloodborne_pc_windows_port/releases/tag/v0.8.0)
#
`bloodborne_pc_windows_port` is a 64-bit Windows port of the PlayStation 4 executable of *Bloodborne* (CUSA03173, version 1.09). 

This project is a Windows adaptation of the original Linux port ([bbport](https://github.com/deadinside28/bloodborne_pc) by [deadinside28](https://github.com/deadinside28)). It replaces the Linux-specific kernel, memory mapping, and POSIX threading implementation with a native Win32 runtime, allowing the game to run on Windows with Vulkan.

> **Important Architecture Clarifications:**
> - **Native CPU Execution (No CPU Emulation or JIT)**: Because the PlayStation 4 uses an x86-64 processor, the game's compiled CPU machine code runs natively and directly on your PC processor. There is no CPU emulator, virtual machine, or dynamic recompiler (JIT) translation layer.
> - **GPU Emulation Handled by shadPS4**: Desktop graphics cards cannot natively parse PS4 AMD GCN command packets or PS4 GPU hardware shaders. All GPU emulation, command stream translation, and shader recompilation into Vulkan SPIR-V pipelines are handled by the [shadPS4](https://github.com/shadps4-emu/shadPS4) Vulkan video core (`gpu/`, `libbbgpu`), customized with a multithreaded draw pipeline and FSR upscaling.
> - **Not a Source Decompilation or Source Port**: This project is not a PC port recompiled from leaked source code or a reverse-engineered C/C++ decompilation. It executes the retail PS4 binary directly by preparing an offline flat memory image and providing a specialized High-Level Emulation (HLE) runtime for PS4 system calls.
> - **No game files or copyrighted assets are included.** You must provide your own decrypted dump of Bloodborne (CUSA03173, version 1.09).
> - This project is not affiliated with Sony Interactive Entertainment, FromSoftware, or AMD.

**Status: Playable on Windows.** The game boots, loads saves, and runs with audio, gamepad support, and state persistence.

---

## Architecture: Native vs Emulated

| Component | Execution Model | Implementation Details |
|---|---|---|
| **CPU / Game Logic** | **100% Native** | Direct x86-64 execution on host CPU. No CPU emulation, no JIT, no instruction translation. |
| **System Calls / OS** | **Native HLE Runtime** | Custom lightweight C runtime (`src/`) implementing memory mapping, threading, file I/O, and PS4 libc/libSceFios2 calls. |
| **GPU / Graphics** | **Emulation / Translation (shadPS4)** | Handled by the [shadPS4](https://github.com/shadps4-emu/shadPS4) Vulkan core (`gpu/`). Translates PS4 PM4 command streams and GCN shaders into Vulkan SPIR-V pipelines with custom multithreaded command dispatch. |
| **Upscaling** | **Native Vulkan** | AMD FSR 3.1 and FSR 4 integration using custom synthetic motion vectors and subpixel jitter. |
| **Audio** | **Native Win32** | Decoded directly using bundled LibAtrac9 and output via SDL3 audio streams. |
| **Controls** | **Native Win32 / SDL3** | Raw mouse capture with a direct 360-degree camera rotation hook (`src/probe.c`) alongside native gamepad support. |

---

## Windows Port Changes

This repository adapts the original Linux codebase specifically for Windows systems:

- **Win32 Memory & Synchronization**: Replaced Linux `mmap`, `mprotect`, and POSIX primitives with Windows `VirtualAlloc`, `VirtualProtect`, and native Win32 synchronization events (`src/win32_compat.c`, `src/win32_memory.c`, `src/runtime_host.c`).
- **Direct 360-Degree Native Mouse Look**: Replaced legacy gamepad stick emulation with a direct camera hook into the engine's update routines (`src/probe.c`). Raw mouse deltas inject directly into camera orientation angles and dynamic motion buffers, enabling full 360-degree rotation (yaw and pitch), real-time view matrix recalculation, and customization sliders for responsiveness, acceleration, and smoothing.
- **Graphical Launcher**: Modular PySide6 launcher (`BloodborneLauncher.exe` / `bblauncherAPP/`), providing real-time configuration for target frame rates (30, 60, 90, uncap), render resolutions, FSR upscaling, community patches, and full interactive keybind remapping.
- **Command Line Launcher**: `run.bat` provides direct scripted startup, automatic patch compilation, and mod staging on Windows. Automatically prioritizes bundled portable Python runtimes (`python-embed/`).
- **Vulkan Frame Pacing**: Integrated presenter queue pacing (`BB_FRAMES_AHEAD=2`) to bound GPU run-ahead, stabilizing frametimes and smoothing 0.1% and 1.0% stutter lows.
- **Texture Barrier Fix**: Fixed dynamic texture streaming race conditions in `tile_manager.cpp`, ensuring item icons in the HUD and loading screens render correctly without static noise.
- **AT9 Audio Playback**: Bundles LibAtrac9 compilation to decode AT9 game audio natively on Windows with soft-clipping saturation curves to eliminate audio pops.
- **Windows Build System**: Standalone `build.bat` script that compiles the complete project with MinGW-w64 (GCC), CMake, and Ninja with multi-core parallel job support.

---

## Technical Highlights

- **Native Execution**: The game eboot is converted offline into a flat memory image; PS4 libc and libSceFios2 functions are linked directly into the binary. No CPU emulation or instruction translation occurs at run time.
- **shadPS4 Vulkan Core**: Graphics rendering and shader translation are handled through a dedicated integration of shadPS4's Vulkan backend, with hundreds of engine-specific performance fixes and synchronization enhancements.
- **High Framerate Support**: Community simulation patches allow running at 60 FPS, 90 FPS, or uncapped with accurate physics and movement speeds.
- **Synthetic Motion Vectors & Temporal Upscaling**: Because Bloodborne lacks an internal velocity buffer, the port computes camera motion from depth buffers and object motion from previous frame vertex positions. The scene can render at lower internal resolutions (e.g., 720p or 1080p) while FSR 3.1 / 4 upscales to your display resolution, leaving UI elements crisp at native resolution.
- **Persistent Pipeline Binary Caches**: Hardware-compiled Vulkan driver pipeline binaries (`.vkp`) are saved to disk and reloaded on startup, eliminating recurring shader compilation stutter.
- **Multi-threaded Draw Pipeline**: PS4 command buffers are decoded on one thread while draw calls are bound and recorded on another, preventing CPU bottlenecks.
- **In-Game Overlay**: Press `F1`, `Insert`, or `L3 + R3` on your controller to open the real-time configuration menu for resolution, sharpness, keybinds, and graphics toggles.

---

## Requirements

- **Operating System**: Windows 10 or Windows 11 (64-bit).
- **Graphics Card**: A Vulkan 1.3 capable GPU with current vendor drivers:
  - NVIDIA GeForce GTX 10-series or newer.
  - AMD Radeon RX 400-series or newer.
  - Intel Arc A-series or newer.
- **Processor**: 64-bit x86-64 processor supporting **AVX 1.0** and SSE4.2 (Intel Sandy Bridge / AMD Bulldozer or newer).
  - *Note on AVX vs AVX2*: **AVX2 is NOT required.** However, **AVX 1.0 is an architectural requirement** because PlayStation 4 native binaries and libraries are compiled with `-mavx` (using VEX-encoded vector instructions like `vxorps`). Legacy processors without AVX (such as Intel Core 2 Quad or Nehalem/Westmere Xeon X56xx) cannot execute these instructions and will fail pre-launch validation.
  - *OSXSAVE / Virtualization*: Windows must have OSXSAVE enabled (`XCR0` YMM state saving). When running inside hypervisors or VMs, ensure AVX and XSAVE passthrough are enabled.
  - *FSGSBASE*: Non-FSGSBASE CPUs are fully supported and gracefully fall back to Windows TLS slots.
- **Game Files**: Decrypted `CUSA03173` directory containing `eboot.bin` (version 1.09).

*(Note: Release archives include a pre-configured `python-embed/` runtime; manual Python installation is not required for release users).*

### Build Dependencies (Only if compiling from source)
- **Compiler**: MinGW-w64 GCC 14.x, 15.x, or 16.x (tested on GCC 15.1 and GCC 16.2 via [w64devkit](https://github.com/skeeto/w64devkit) or MSYS2 MinGW64).
- **Build Tools**: CMake 3.25+, Ninja build tool, `glslangValidator` (from Vulkan SDK or glslang package).
- **Libraries & Headers**:
  - `SDL3` (development libraries)
  - `Vulkan-Headers` / Vulkan SDK
  - `fmt`
  - `Boost` (headers / Asio)
  - `Zydis`
  - `FFmpeg` (`libavcodec`, `libavformat`, `libavutil`, `libswresample`, `libswscale`)
  - `xxHash` (`libxxhash`)
  - `VulkanMemoryAllocator` (VMA)
  - `xbyak`
  - `miniz`
  - `magic_enum`
  - `robin-map` (`tsl-robin-map`)

#### Installing Dependencies on MSYS2 MinGW64:
If building with MSYS2, install all required build tools and libraries with a single command:
```bash
pacman -S --needed \
  mingw-w64-x86_64-gcc \
  mingw-w64-x86_64-cmake \
  mingw-w64-x86_64-ninja \
  mingw-w64-x86_64-glslang \
  mingw-w64-x86_64-vulkan-headers \
  mingw-w64-x86_64-vulkan-loader \
  mingw-w64-x86_64-sdl3 \
  mingw-w64-x86_64-fmt \
  mingw-w64-x86_64-boost \
  mingw-w64-x86_64-zydis \
  mingw-w64-x86_64-ffmpeg \
  mingw-w64-x86_64-xxhash \
  mingw-w64-x86_64-vulkan-memory-allocator \
  mingw-w64-x86_64-xbyak \
  mingw-w64-x86_64-miniz \
  mingw-w64-x86_64-magic_enum \
  mingw-w64-x86_64-robin-map
```
*(If using standalone w64devkit, ensure the dependency headers and libraries are located in your prefix or pass `-DCMAKE_PREFIX_PATH="C:/msys64/mingw64;..."` or set `SDL3_DIR` / `VULKAN_SDK` in `build.bat`)*.

---

## Building and Running

### 1. Build from Source

Clone the repository recursively:
```cmd
git clone --recursive https://github.com/Mrsuss60/bloodborne_pc_windows_port.git
cd bloodborne_pc_windows_port
```

> **Windows Path Length Warning**: FidelityFX contains deeply nested shader files that can exceed Windows' default 260-character path limit (`MAX_PATH`). If your project folder is located deep in your directory structure, either enable long paths in Git via `git config --system core.longpaths true` (or clone close to a drive root like `C:\bbport`), or set up a mapped drive with `subst X: .`.

Ensure MinGW, CMake, and Ninja are accessible in your environment or PATH (set `W64DEVKIT_DIR`, `SDL3_DIR`, `VULKAN_SDK` as needed), then run:
```cmd
build.bat
```

This compiles LibAtrac9, the Vulkan video core (`out/gpu/bbgpu.dll`), and the main loader executable (`out/bbport.exe`).

### 2. Launch the Game

#### Option A: Using the Graphical Launcher (Recommended)
Double-click `BloodborneLauncher.exe` (or run it from the root directory):
- Select your `eboot.bin` file or game folder.
- Select your target frame rate (60 FPS recommended for smooth frame pacing).
- Select your internal resolution (e.g., 1280x720 with FSR enabled for high performance).
- Enable desired patches under the Patches tab (e.g., *Skip Intro*, *Performance Patch*).
- Configure mouse look sliders and customize keybinds under the Settings tab.
- Click **Launch Bloodborne**.

#### Option B: Using the Command Line
Run `run.bat` pointing to your game directory:
```cmd
run.bat "C:\Games\Bloodborne\CUSA03173"
```

Save files and shader caches are stored in `user/`. Settings are saved to `bbport.ini`.

---

## Controls and In-Game Features

- **Mouse & Keyboard**:
  - **Mouse Look**: Direct 360-degree native camera rotation hook with 1:1 mouse input. Configurable sensitivity, smoothing, responsiveness, and acceleration sliders in the launcher and in-game overlay. Relative cursor capture releases automatically when menus open or when alt-tabbing.
  - **Light Attack (R1)**: Mouse Left
  - **Heavy / Charged Attack (R2)**: Shift + Mouse Left
  - **Firearm / Gun (L2)**: Mouse Right
  - **Weapon Transform (L1)**: Tab
  - **Lock-On / Reset Camera (R3)**: Mouse Middle
  - **Move**: W / A / S / D
  - **Walk (hold while moving)**: Left Alt
  - **Dodge / Roll / Sprint**: Space (hold to sprint, tap while sprinting to jump)
  - **Interact / Talk**: E (or Enter)
  - **Use Item**: R
  - **Heal / Blood Vial**: F
  - **Blood Bullets (Up)**: 1
  - **Switch Item (Down)**: 2
  - **Switch Left Weapon (Left)**: 3
  - **Switch Right Weapon (Right)**: 4
  - **Gestures Menu (Touchpad L)**: G
  - **Personal Effects (Touchpad R)**: Backspace
  - **System Menu (Options)**: Escape
- **Gamepad**: Controllers are supported out of the box through SDL3.
- **In-Game Settings**: Press `F1`, `Insert` on keyboard, or `L3 + R3` on gamepad to toggle the overlay menu. Changes write to `bbport.ini`. Language can be toggled between English and Russian under *Other → Language*.
- **Free Camera**: When enabled in the launcher or overlay, hold `Cross` and press `L3` to cycle camera modes.
- **Debug Menu**: If debug fonts (`DbgFont14h.ccm` and `DbgFont14h.tpf`) are installed into `dvdroot_ps4/font/`, open the menu using the controller touchpad.

---

## Repository Layout

| Path | Description |
|---|---|
| `src/` | Win32 loader (`probe.c`), memory virtualization, and PS4 HLE runtime |
| `gpu/` | Vulkan video core, multi-stage command pipeline, and upscaler integration |
| `bblauncherAPP/` | Modular PySide6 desktop launcher application source and packaging scripts |
| `scripts/` | Eboot preparation, patch compiler (`patches.py`), and instance guard (`bb_guard.py`) |
| `patches/` | Community XML patches for Bloodborne version 1.09 |
| `build.bat` | Multi-core Windows compilation script |
| `run.bat` | Direct execution batch script with embedded Python fallback |
| `tools/` | Developer utilities and patch extraction scripts |
| `docs/` | Architecture notes, upscaler documentation, and change logs |

---

## Credits and Licenses

This project is licensed under the **GNU General Public License v2 or later** ([LICENSE](LICENSE)) due to its use of shadPS4 components.
The game menu mouse pointer is adapted from [bbhost](https://github.com/droogie/bbhost) (GPL-3.0-or-later), so builds that contain it are distributed under the **GNU GPL v3 or later** ([licenses/GPL-3.0.txt](licenses/GPL-3.0.txt), [licenses/BBHOST-PORTS.md](licenses/BBHOST-PORTS.md)).

Special thanks to the original creators and open source projects that made this port possible:

- **deadinside28**: Creator of the original Linux port (`bbport`), pioneering the flat memory image architecture, custom Vulkan two-stage pipeline, relative mouse look implementation, and synthetic motion vector generation.
- **shadPS4 Team**: Base Vulkan video core and shader recompiler.
- **droogie and the bbhost contributors**: game menu mouse pointer ([bbhost](https://github.com/droogie/bbhost)), via **DarkIzuku**'s bloodborne_pc port.
- **FireBurn**: FSR-Vulkan runtime and temporal upscaling backend.
- **Thealexbarney**: LibAtrac9 audio decoding library.
- **ocornut**: Dear ImGui library for the in-game settings overlay.
- **Community Patch Authors**: Kyo, Lance McDonald, illusion, emoose, auser1337, and contributors for the 60 FPS, camera, and engine patches.
- **zackcage6**: testing and bug reporting.
- **[ALuaNegra](https://github.com/ALuaNegra)**: pull request [#89](https://github.com/Mrsuss60/bloodborne_pc_windows_port/pull/89) (transient depth target reclamation and VRAM pressure mitigation).
- **[parsifal295](https://github.com/parsifal295)**: pull requests [#35](https://github.com/Mrsuss60/bloodborne_pc_windows_port/pull/35), [#36](https://github.com/Mrsuss60/bloodborne_pc_windows_port/pull/36), [#37](https://github.com/Mrsuss60/bloodborne_pc_windows_port/pull/37).
- **[wizardnun](https://github.com/wizardnun)**: pull request [#39](https://github.com/Mrsuss60/bloodborne_pc_windows_port/pull/39).
- **[Vorshim92](https://github.com/Vorshim92)**: pull requests [#65](https://github.com/Mrsuss60/bloodborne_pc_windows_port/pull/65), [#66](https://github.com/Mrsuss60/bloodborne_pc_windows_port/pull/66), [#67](https://github.com/Mrsuss60/bloodborne_pc_windows_port/pull/67), [#73](https://github.com/Mrsuss60/bloodborne_pc_windows_port/pull/73), [#74](https://github.com/Mrsuss60/bloodborne_pc_windows_port/pull/74), [#76](https://github.com/Mrsuss60/bloodborne_pc_windows_port/pull/76), [#77](https://github.com/Mrsuss60/bloodborne_pc_windows_port/pull/77).
- **[HatsyRei](https://github.com/HatsyRei)**: pull requests [#103](https://github.com/Mrsuss60/bloodborne_pc_windows_port/pull/103), [#123](https://github.com/Mrsuss60/bloodborne_pc_windows_port/pull/123), [#147](https://github.com/Mrsuss60/bloodborne_pc_windows_port/pull/147), [#150](https://github.com/Mrsuss60/bloodborne_pc_windows_port/pull/150), [#152](https://github.com/Mrsuss60/bloodborne_pc_windows_port/pull/152).
