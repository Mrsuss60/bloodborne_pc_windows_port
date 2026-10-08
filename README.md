# bloodborne_pc_windows_port: Native Windows Port of Bloodborne

`bloodborne_pc_windows_port` is a native 64-bit Windows port of the PlayStation 4 executable of *Bloodborne* (CUSA03173, version 1.09). 

This project is a Windows adaptation of the original Linux port (`bbport` by `deadinside28`). It replaces the Linux-specific kernel, memory mapping, and POSIX threading implementation with a native Win32 runtime, allowing the game to run directly on Windows with Vulkan.

The game's x86-64 code executes natively on your CPU without general emulation. System library calls are handled by a dedicated lightweight runtime, and GPU commands are translated directly to Vulkan with support for AMD FSR 3.1 and FSR 4 temporal upscaling.

> **No game files or copyrighted assets are included.** You must provide your own decrypted dump of Bloodborne (CUSA03173, version 1.09).
> This project is not affiliated with Sony Interactive Entertainment, FromSoftware, or AMD.

**Status: Playable on Windows.** The game boots, loads saves, and runs with audio, gamepad support, and state persistence.

---

## Windows Port Changes

This repository adapts the original Linux codebase specifically for Windows systems:

- **Win32 Memory & Synchronization**: Replaced Linux `mmap`, `mprotect`, and POSIX primitives with Windows `VirtualAlloc`, `VirtualProtect`, and native Win32 synchronization events (`src/win32_compat.c`, `src/win32_memory.c`, `src/runtime_host.c`).
- **Graphical Launcher**: Includes `launch_gui.bat` (powered by `launcher.py`), a desktop control panel to configure target frame rates (30, 60, 90, uncap), rendering resolution, FSR upscaling, and all 61 toggleable community patches from `Bloodborne.xml`.
- **Command Line Launcher**: `run.bat` provides direct scripted startup, automatic patch compilation, and mod staging on Windows.
- **Vulkan Frame Pacing**: Integrated presenter queue pacing (`BB_FRAMES_AHEAD=2`) to bound GPU run-ahead, stabilizing frametimes and smoothing 0.1% and 1.0% stutter lows.
- **Texture Barrier Fix**: Fixed dynamic texture streaming race conditions in `tile_manager.cpp`, ensuring item icons in the HUD and loading screens render correctly without static noise.
- **AT9 Audio Playback**: Bundles LibAtrac9 compilation to decode AT9 game audio natively on Windows.
- **Windows Build System**: Standalone `build.bat` script that compiles the complete project with MinGW-w64 (GCC), CMake, and Ninja.

---

## Technical Highlights

- **Native Execution**: The game eboot is converted offline into a flat memory image; PS4 libc and libSceFios2 functions are linked directly into the binary. No CPU emulation or instruction translation occurs at run time.
- **High Framerate Support**: Community simulation patches allow running at 60 FPS, 90 FPS, or uncapped with accurate physics and movement speeds.
- **Synthetic Motion Vectors & Temporal Upscaling**: Because Bloodborne lacks an internal velocity buffer, the port computes camera motion from depth buffers and object motion from previous frame vertex positions. The scene can render at lower internal resolutions (e.g., 720p or 1080p) while FSR 3.1 / 4 upscales to your display resolution, leaving UI elements crisp at native resolution.
- **Multi-threaded Draw Pipeline**: PS4 command buffers are decoded on one thread while draw calls are bound and recorded on another, preventing CPU bottlenecks.
- **In-Game Overlay**: Press `Insert` or `L3 + R3` on your controller to open the real-time configuration menu for resolution, sharpness, and graphics toggles.

---

## Requirements

- **Operating System**: Windows 10 or Windows 11 (64-bit).
- **Graphics Card**: A Vulkan 1.3 capable GPU with current vendor drivers:
  - NVIDIA GeForce GTX 10-series or newer.
  - AMD Radeon RX 400-series or newer.
  - Intel Arc A-series or newer.
- **Processor**: x86-64 processor with AVX2 support.
- **Python**: Python 3.10 or newer (needed for the patch compiler and launcher).
- **Game Files**: Decrypted `CUSA03173` directory containing `eboot.bin` (version 1.09).

### Build Dependencies (Only if compiling from source)
- MinGW-w64 GCC (via [w64devkit](https://github.com/skeeto/w64devkit) or MSYS2 MinGW64).
- CMake 3.25+.
- Ninja build tool.
- SDL3 development libraries.
- Vulkan SDK or Vulkan-Headers.

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
Double-click `launch_gui.bat` or run:
```cmd
launch_gui.bat
```
- Select your `eboot.bin` file or game folder.
- Select your target frame rate (60 FPS recommended for smooth frame pacing).
- Select your internal resolution (e.g., 1280x720 with FSR enabled for high performance).
- Enable desired patches under the Patches tab (e.g., *Performance Patch*, *Disable Motion Blur*, *Skip Intro*).
- Click **Launch Bloodborne**.

#### Option B: Using the Command Line
Run `run.bat` pointing to your game directory:
```cmd
run.bat "C:\Games\Bloodborne\CUSA03173"
```

Save files and shader caches are stored in `user/`. Settings are saved to `bbport.ini`.

---

## Controls and In-Game Features

- **Gamepad**: Controllers are supported out of the box through SDL3.
- **In-Game Settings**: Press `Insert` on keyboard or `L3 + R3` on gamepad to toggle the overlay menu.
- **Free Camera**: When enabled in the launcher or overlay, hold `Cross` and press `L3` (or hold `Space` and press `Z` on keyboard) to cycle camera modes.
- **Debug Menu**: If debug fonts (`DbgFont14h.ccm` and `DbgFont14h.tpf`) are installed into `dvdroot_ps4/font/`, open the menu using `Tab` or the controller touchpad.

---

## Repository Layout

| Path | Description |
|---|---|
| `src/` | Win32 loader (`probe.c`), memory management, and PS4 HLE runtime |
| `gpu/` | Vulkan video core, multi-stage command pipeline, and upscaler integration |
| `scripts/` | Eboot preparation, patch compiler (`patches.py`), and DLL staging |
| `patches/` | Community XML patches for Bloodborne version 1.09 |
| `launcher.py` | Tkinter desktop launcher for Windows |
| `launch_gui.bat` | One-click shortcut for the graphical launcher |
| `build.bat` | Windows build script |
| `run.bat` | Direct execution batch script |
| `tools/` | Developer utilities and patch extraction scripts |
| `docs/` | Architecture notes, upscaler documentation, and change logs |

---

## Credits and Licenses

This project is licensed under the **GNU General Public License v2 or later** ([LICENSE](LICENSE)) due to its use of shadPS4 components.

Special thanks to the original creators and open source projects that made this port possible:

- **deadinside28**: Creator of the original Linux port (`bbport`), pioneering the flat memory image architecture, custom Vulkan two-stage pipeline, and synthetic motion vector generation.
- **shadPS4 Team**: Base Vulkan video core and shader recompiler.
- **FireBurn**: FSR-Vulkan runtime and temporal upscaling backend.
- **Thealexbarney**: LibAtrac9 audio decoding library.
- **ocornut**: Dear ImGui library for the in-game settings overlay.
- **Community Patch Authors**: Kyo, Lance McDonald, illusion, emoose, auser1337, and contributors for the 60 FPS, camera, and engine patches.
- **zackcage6**: testing and bug reporting.
