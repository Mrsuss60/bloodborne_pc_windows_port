#!/usr/bin/env python3
"""Utility functions, constants, and batched background worker for Bloodborne PC Launcher."""

import ctypes
import hashlib
import locale
import os
import subprocess
import sys
import time
from datetime import datetime
from pathlib import Path
from PySide6.QtCore import QThread, Signal

# Dynamic path resolution: works correctly whether run via python or as a compiled .exe
if getattr(sys, "frozen", False):
    APP_DIR = Path(sys.executable).resolve().parent
else:
    APP_DIR = Path(__file__).resolve().parent

if not (APP_DIR / "run.bat").exists() and (APP_DIR.parent / "run.bat").exists():
    APP_DIR = APP_DIR.parent

RUN_BAT = APP_DIR / "run.bat"
LOG_FILE = APP_DIR / "bblauncher.log"
LOG_DIR = APP_DIR / "logs"
LOG_KEEP = 20
SETTINGS_FILE = APP_DIR / "bblauncher_settings.json"
PATCHES_XML = APP_DIR / "patches" / "Bloodborne.xml"
DEFAULT_GAME = Path(os.environ.get("BB_GAME_DIR", "../CUSA03173"))
LAUNCHER_VERSION = "0.8.0"


def build_stamp() -> str:
    """Return the launcher build date (exe when frozen, source file otherwise)."""
    try:
        target = Path(sys.executable) if getattr(sys, "frozen", False) else Path(__file__)
        return datetime.fromtimestamp(target.stat().st_mtime).strftime("%Y-%m-%d")
    except OSError:
        return ""


FPS_CHOICES = ["uncap", "60", "90", "30"]
# Displayed in the Display & Graphics tab. The item data is the rate passed to run.bat
# ("uncap" for Uncapped); 30 is the native PS4 path in both modes.
FPS_LIVE_CHOICES = [
    ("60 (Live patch)", "60"),
    ("90 (Live patch)", "90"),
    ("120 (Live patch)", "120"),
    ("Uncap (Live patch)", "uncap"),
    ("30 (Native PS4)", "30"),
]
FPS_XML_CHOICES = [
    ("60 (XML patch)", "60"),
    ("90 (XML patch)", "90"),
    ("Uncap (XML patch)", "uncap"),
    ("30 (Native PS4)", "30"),
]
RES_CHOICES = ["1920x1080 (Native)", "1280x720", "2560x1440", "3840x2160"]
WINDOW_MODE_CHOICES = ["Windowed", "Borderless", "Fullscreen"]
ANISO_CHOICES = [
    ("16x", "16", "16x anisotropic filtering of scene textures (recommended)"),
    ("8x", "8", "8x anisotropic filtering"),
    ("4x", "4", "4x anisotropic filtering"),
    ("2x", "2", "2x anisotropic filtering"),
    ("Off", "0", "Game's default anisotropic filtering (no override)"),
]
AA_CHOICES = ["Native FXAA (PS4 Default)", "Disable AA (No FXAA)", "TAA (Temporal Anti-Aliasing)"]
UPSCALER_CHOICES = [
    ("Off", "off"),
    ("FSR 3.1", "fsr31"),
    ("FSR 4", "fsr4"),
    ("FSR 4.1.1", "fsr411"),
    ("DLSS (NVIDIA RTX)", "dlss"),
]
PRESET_CHOICES = [
    ("Native AA (no upscaling)", "0"),
    ("Quality", "1"),
    ("Balanced", "2"),
    ("Performance", "3"),
    ("Ultra Performance", "4"),
]
LIVE_RES_CHOICES = [
    ("Off - startup patch (recommended)", "0"),
    ("Auto - strong discrete GPUs", "auto"),
    ("On - apply changes live", "1"),
]
ASYNC_PIPELINE_CHOICES = [
    ("Auto (dynamic threads)", "auto"),
    ("Off (synchronous on GPU)", "0"),
    ("1 Worker Thread", "1"),
    ("2 Worker Threads", "2"),
    ("4 Worker Threads", "4"),
    ("8 Worker Threads", "8"),
]
DEBUG_VIEW_CHOICES = [
    ("Off", "0"),
    ("Reactive mask", "1"),
    ("Motion vectors", "2"),
]
HUD_QUADRANT_CHOICES = [
    ("Top Left", "0"),
    ("Top Right", "1"),
    ("Bottom Left", "2"),
    ("Bottom Right", "3"),
]
MODEL_LOD_CHOICES = [
    ("Highest (-2)", "-2"),
    ("Higher (-1)", "-1"),
    ("Game default (0)", "0"),
    ("Lower (1)", "1"),
    ("Lowest (2)", "2"),
]
MENU_LANGUAGE_CHOICES = [
    ("English", "en"),
    ("Russian", "ru"),
]
CONTENT_SKU_CHOICES = [
    ("Full game", "full"),
    ("Trial", "trial"),
]
# bbport.ini effect switches read by patches.py. (key, label, default on)
EFFECT_CHOICES = [
    ("effect_chromatic_aberration", "Chromatic Aberration", False),
    ("effect_dof", "Depth of Field (DoF)", False),
    ("effect_motion_blur", "Motion Blur", False),
    ("effect_ssao", "SSAO Shading", True),
    ("effect_dynamic_shadows", "Dynamic Light Shadows", True),
    ("effect_ssr", "Screen Space Reflections (SSR)", False),
    ("skip_intro", "Skip Intro Videos", True),
    ("debug_camera", "Free Debug Camera", False),
    ("debug_menu", "Debug Menu (needs font files)", False),
]
PREP_WORKER_CHOICES = [
    ("Auto (dynamic threads)", "auto"),
    ("Off (single-threaded)", "0"),
    ("1 (recommended for 4-threaded CPUs)", "1"),
    ("2 (recommended for 6-threaded CPUs)", "2"),
    ("3 (recommended for 8-threaded CPUs)", "3"),
    ("4 (recommended for 10-12 threaded CPUs)", "4"),
    ("6 (recommended for 16-threaded CPUs)", "6"),
    ("8 (recommended for 24+ threaded CPUs)", "8"),
]
VK_NOISE = "<Warning> vk_instance.cpp"
NO_WINDOW = getattr(subprocess, "CREATE_NO_WINDOW", 0)

# All 21 text languages supported by Bloodborne (PS4 system language code)
LANGUAGE_CHOICES = [
    ("English (United States)", "1"),
    ("English (United Kingdom)", "18"),
    ("Russian", "8"),
    ("Japanese", "0"),
    ("French", "2"),
    ("Spanish (Spain)", "3"),
    ("Spanish (Latin America)", "20"),
    ("German", "4"),
    ("Italian", "5"),
    ("Dutch", "6"),
    ("Portuguese (Portugal)", "7"),
    ("Portuguese (Brazil)", "17"),
    ("Polish", "16"),
    ("Turkish", "19"),
    ("Korean", "9"),
    ("Traditional Chinese", "10"),
    ("Simplified Chinese", "11"),
    ("Finnish", "12"),
    ("Swedish", "13"),
    ("Danish", "14"),
    ("Norwegian", "15"),
]

def connected_gpus(app_dir: Path = APP_DIR) -> list:
    """(index, vendor_id, type, name, is_best) of GPUs in Vulkan order via bb-gpu-capabilities.exe --gpus"""
    candidates = [
        app_dir / "out" / "bb-gpu-capabilities.exe",
        app_dir / "bb-gpu-capabilities.exe",
        app_dir / "tools" / "bb-gpu-capabilities.exe",
    ]
    tool = next((c for c in candidates if c.is_file()), None)
    if not tool:
        return []
    env = {k: v for k, v in os.environ.items() if k != "BB_GPU_ID"}
    try:
        res = subprocess.run([str(tool), "--gpus"], capture_output=True, text=True, timeout=8, env=env, creationflags=NO_WINDOW)
    except (OSError, subprocess.SubprocessError):
        return []
    rows = [ln.split("\t") for ln in res.stdout.splitlines() if ln.count("\t") == 4]
    return [(int(idx), vendor, kind, name, best == "1") for idx, vendor, kind, name, best in rows if idx.isdigit()]

DEFAULT_SETTINGS = {
    "eboot": "",
    "fps": "60",
    "res": RES_CHOICES[0],
    "aniso": "2x",
    "prep_workers": PREP_WORKER_CHOICES[0][0],
    "async_pipelines": "auto",
    "anti_aliasing": AA_CHOICES[0],
    "gpu": "",
    "language": "1",
    "mods": False,
    "hide_vk": False,
    "feat_upscaler": False,
    "upscaler_model": UPSCALER_CHOICES[0][0],
    "feat_overlay": True,
    "feat_hud": True,
    "feat_fps_patch": True,
    "window_mode": "Borderless",
    "fullscreen": False,
    "enabled_patches": ["Skip Intro"],
    # run.bat / environment options (Advanced Settings tab). Empty means "run.bat default".
    "timeout": "",
    "vblank_hz": "",
    "frames_ahead": "2",
    "dmem_mb": "",
    "extra_args": "",
    "sku": "full",
    "mods_dir": "",
    "mods_config": "",
    "patches_dir": "",
    "patches_config": "",
    "user_dir": "",
    # Home dashboard / console
    "log_filter": "all",
    "log_autoscroll": True,
    "last_run_time": "",
    "last_run_rc": None,
    "last_run_duration": 0,
}

GAMEPLAY_KEYBINDS = [
    ("key.attack", "Light Attack (R1)", "Mouse Left"),
    ("key.heavy_attack", "Heavy Attack (R2)", "Shift + Mouse Left"),
    ("key.firearm", "Firearm / Gun (L2)", "Mouse Right"),
    ("key.transform", "Weapon Transform (L1)", "Tab"),
    ("key.lock_on", "Lock-On / Camera Reset (R3)", "Mouse Middle"),
    ("key.move_forward", "Move Forward", "W"),
    ("key.move_backward", "Move Backward", "S"),
    ("key.move_left", "Move Left", "A"),
    ("key.move_right", "Move Right", "D"),
    ("key.dodge", "Dodge / Roll / Sprint (Circle)", "Space"),
    ("key.interact", "Interact / Talk (Cross)", "E"),
    ("key.use_item", "Use Item (Square)", "R"),
    ("key.heal", "Heal / Blood Vial (Triangle)", "F"),
    ("key.dpad_up", "Blood Bullets (Up)", "1"),
    ("key.dpad_down", "Switch Item (Down)", "2"),
    ("key.dpad_left", "Switch Left Weapon (Left)", "3"),
    ("key.dpad_right", "Switch Right Weapon (Right)", "4"),
    ("key.gestures", "Gestures Menu (Touchpad L)", "G"),
    ("key.personal_effects", "Personal Effects (Touchpad R)", "Backspace"),
    ("key.menu", "System Menu (Options)", "Escape"),
    ("key.walk", "Walk (hold)", "Left Alt"),
]

# UI hotkeys are keyboard-only: mapping a menu toggle to an attack mouse button would fire
# while playing.  They are still stored in bbport.ini and shown in the in-game keybind list.
SYSTEM_KEYBINDS = [
    ("key.overlay_toggle", "In-Game Overlay Toggle", "Insert"),
    ("key.hud_toggle", "Performance HUD Toggle", "F11"),
    ("key.cheat_toggle", "Cheat Menu Toggle", "F4"),
    ("key.mouse_capture_toggle", "Mouse Capture / Release Toggle", "F1"),
]

DEFAULT_KEYBINDS = GAMEPLAY_KEYBINDS + SYSTEM_KEYBINDS

# Movement layouts.  These are the labels printed on the keyboard; apply_keybind_preset
# converts them to the physical SDL scancode names that the game actually binds.
KEYBIND_PRESETS = {
    "qwerty": {
        "key.move_forward": "W",
        "key.move_left": "A",
        "key.move_backward": "S",
        "key.move_right": "D",
    },
    "azerty": {
        "key.move_forward": "Z",
        "key.move_left": "Q",
        "key.move_backward": "S",
        "key.move_right": "D",
    },
}

# SDL scancode names describe physical US key positions.  bbport.ini and the game keep those
# names so a binding stays on the same physical key; the launcher shows the character printed
# on the key for the active Windows layout.  Windows scan code -> SDL scancode name.
_WIN_SCAN_TO_SDL_NAME = {
    0x02: "1", 0x03: "2", 0x04: "3", 0x05: "4", 0x06: "5",
    0x07: "6", 0x08: "7", 0x09: "8", 0x0A: "9", 0x0B: "0",
    0x0C: "-", 0x0D: "=",
    0x10: "Q", 0x11: "W", 0x12: "E", 0x13: "R", 0x14: "T",
    0x15: "Y", 0x16: "U", 0x17: "I", 0x18: "O", 0x19: "P",
    0x1A: "[", 0x1B: "]",
    0x1E: "A", 0x1F: "S", 0x20: "D", 0x21: "F", 0x22: "G",
    0x23: "H", 0x24: "J", 0x25: "K", 0x26: "L",
    0x27: ";", 0x28: "'", 0x29: "`",
    0x2B: "\\",
    0x2C: "Z", 0x2D: "X", 0x2E: "C", 0x2F: "V", 0x30: "B",
    0x31: "N", 0x32: "M",
    0x33: ",", 0x34: ".", 0x35: "/",
    0x39: "Space",
}
_SDL_NAME_TO_WIN_SCAN = {name: scan for scan, name in _WIN_SCAN_TO_SDL_NAME.items()}
_USER32 = None


def _get_user32():
    """Load user32 lazily so the launcher still imports on non-Windows hosts."""
    global _USER32
    if _USER32 is None:
        if os.name != "nt":
            _USER32 = False
        else:
            try:
                from ctypes import wintypes

                lib = ctypes.WinDLL("user32", use_last_error=True)
                lib.GetKeyNameTextW.argtypes = [wintypes.LONG, wintypes.LPWSTR, ctypes.c_int]
                lib.GetKeyNameTextW.restype = ctypes.c_int
                _USER32 = lib
            except Exception:
                _USER32 = False
    return _USER32 or None


def _key_name_text(scan_code: int) -> str:
    """Return the active layout's label for a Windows scan code, or an empty string."""
    user32 = _get_user32()
    if user32 is None or not scan_code:
        return ""
    buf = ctypes.create_unicode_buffer(64)
    if user32.GetKeyNameTextW(int(scan_code) << 16, buf, len(buf)) > 0:
        return buf.value
    return ""


def win_scan_to_scancode_name(scan_code: int) -> str:
    """Map a Windows scan code to the SDL scancode name used by bbport.ini."""
    return _WIN_SCAN_TO_SDL_NAME.get(int(scan_code) & 0xFF, "")


def scancode_display_name(stored_name: str) -> str:
    """Show a stored SDL scancode name as the label printed on the active keyboard.

    Digits keep their US names ("1".."0") because a French AZERTY key reports its
    unshifted punctuation character ("&", "é", ...) and a number is clearer in the UI.
    """
    if not stored_name:
        return "None"
    if stored_name.lower() in ("none", "unknown"):
        return stored_name
    if len(stored_name) != 1 or not stored_name.isascii():
        return stored_name
    if stored_name.isdigit():
        return stored_name
    scan_code = _SDL_NAME_TO_WIN_SCAN.get(stored_name.upper())
    if scan_code is None:
        return stored_name
    return _key_name_text(scan_code) or stored_name


def display_name_to_scancode_label(label: str) -> str:
    """Resolve a printed key label (for example AZERTY "Z") to a stored SDL name ("W")."""
    label = (label or "").strip()
    if not label:
        return label
    if len(label) != 1 or not label.isascii():
        return label
    target = label.upper()
    # Walk the physical key table so the answer follows the active layout.  On a
    # French layout this maps Z->W, Q->A, W->Z and A->Q; on QWERTY it is the identity.
    for stored_name, scan_code in _SDL_NAME_TO_WIN_SCAN.items():
        if len(stored_name) == 1 and stored_name.isalpha():
            if _key_name_text(scan_code).upper() == target:
                return stored_name
    if target in _SDL_NAME_TO_WIN_SCAN:
        return target
    return label


KEYBINDINGS = [(k, desc) for _, desc, k in DEFAULT_KEYBINDS]

_SYS_ENCODING = locale.getpreferredencoding(False)
_instance_mutex = []
_bb_guard_mod = False


def get_bb_guard():
    global _bb_guard_mod
    if _bb_guard_mod is False:
        try:
            scripts_dir = str(APP_DIR / "scripts")
            if scripts_dir not in sys.path:
                sys.path.insert(0, scripts_dir)
            import bb_guard
            _bb_guard_mod = bb_guard
        except Exception:
            _bb_guard_mod = None
    return _bb_guard_mod


def acquire_single_instance() -> bool:
    if os.name != "nt":
        return True
    try:
        kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
        name = "Local\\BloodborneLauncher_" + hashlib.md5(str(APP_DIR).lower().encode()).hexdigest()[:12]
        handle = kernel32.CreateMutexW(None, False, name)
        if handle and ctypes.get_last_error() == 183:
            return False
        _instance_mutex.append(handle)
    except Exception:
        pass
    return True


def decode_line(raw: bytes) -> str:
    try:
        return raw.decode("utf-8")
    except UnicodeDecodeError:
        return raw.decode(_SYS_ENCODING, errors="replace")


def shader_cache_dir() -> Path:
    user = Path(os.environ.get("BB_USER_DIR") or "user")
    return (user if user.is_absolute() else APP_DIR / user) / "cache"


def resolve_game_dir(target: str):
    p = Path(target.strip().strip('"'))
    if p.is_file() and p.name.lower() == "eboot.bin":
        return p.parent
    if p.is_dir() and (p / "eboot.bin").is_file():
        return p
    return None


def running_instances():
    guard = get_bb_guard()
    if guard is None:
        return []
    try:
        return guard.find_instances(APP_DIR / "out")
    except Exception:
        return []


def _taskkill_tree(pid: int) -> bool:
    """Force-kill a Windows process and its descendants before the parent can disappear."""
    if os.name != "nt":
        return False
    try:
        result = subprocess.run(
            ["taskkill", "/F", "/T", "/PID", str(pid)],
            stdin=subprocess.DEVNULL,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
            timeout=8,
            creationflags=NO_WINDOW,
        )
        return result.returncode == 0
    except (OSError, subprocess.SubprocessError):
        return False


def terminate_bb_instances(timeout: float = 2.0) -> list[int]:
    """Terminate this install's bbport.exe processes. Returns PIDs that survived."""
    guard = get_bb_guard()
    survivors = []
    for instance in running_instances():
        if guard is None:
            survivors.append(instance.pid)
            continue
        try:
            if not guard.terminate(instance.pid, timeout=timeout):
                survivors.append(instance.pid)
        except Exception:
            survivors.append(instance.pid)
    return survivors


def kill_tree(proc: subprocess.Popen):
    """Force-stop cmd.exe -> run.bat -> bbport.exe, even if the wrapper already exited.

    taskkill /T must run while the cmd.exe parent still owns the tree.  The bb_guard sweep
    then catches a game process that outlived its wrapper (or was started from another
    shell), and the second sweep handles the small race between the two steps.
    """
    if os.name == "nt" and proc is not None and proc.poll() is None:
        _taskkill_tree(proc.pid)

    terminate_bb_instances(timeout=2.0)

    if proc is not None and proc.poll() is None:
        try:
            proc.terminate()
            try:
                proc.wait(timeout=1.0)
            except subprocess.TimeoutExpired:
                proc.kill()
                try:
                    proc.wait(timeout=2.0)
                except subprocess.TimeoutExpired:
                    pass
        except OSError:
            pass

    # A game process can remain after cmd.exe is gone.  Kill it now, not on the next launch.
    terminate_bb_instances(timeout=1.0)


def rotate_log():
    try:
        if LOG_FILE.is_file() and LOG_FILE.stat().st_size > 0:
            LOG_DIR.mkdir(exist_ok=True)
            stamp = datetime.fromtimestamp(LOG_FILE.stat().st_mtime).strftime("%Y%m%d_%H%M%S")
            LOG_FILE.replace(LOG_DIR / f"bblauncher_{stamp}.log")
            for old in sorted(LOG_DIR.glob("bblauncher_*.log"))[:-LOG_KEEP]:
                old.unlink()
    except OSError:
        pass


def load_xml_patches(xml_path: Path = PATCHES_XML):
    import xml.etree.ElementTree as ET

    patches = []
    if not xml_path.is_file():
        return patches
    try:
        tree = ET.parse(xml_path)
        for meta in tree.getroot().iter("Metadata"):
            if meta.get("AppVer") == "01.09" and meta.get("AppElf", "eboot.bin") == "eboot.bin":
                name = meta.get("Name", "").strip()
                if not name:
                    continue
                author = meta.get("Author", "").strip() or "Unknown"
                note = meta.get("Note", "").strip().replace("\\n", "\n")
                n = name.lower()
                if "resolution patch" in n or "light grid" in n or "optimal 1080p" in n:
                    cat = "Resolutions & Grids"
                elif "fps" in n:
                    cat = "Framerate & Engine"
                elif any(k in n for k in ["debug", "capture", "http"]):
                    cat = "Debug & Tools"
                elif any(k in n for k in ["stealth", "silent", "no dead", "rally", "cheat", "sensitive analog", "unlock game region", "enemy control"]):
                    cat = "Gameplay & Cheats"
                elif any(k in n for k in ["blur", "shadow", "dof", "ssao", "chromatic", "camera", "reflections", "text scale", "physics", "aa"]):
                    cat = "Visuals & Camera"
                else:
                    cat = "Performance & Fixes"

                patches.append({
                    "name": name,
                    "author": author,
                    "note": note,
                    "category": cat,
                })
    except Exception as e:
        print(f"Error loading patches XML: {e}")
    return patches


LIVE_FPS_CONFLICTING_PATCHES = {
    "30 FPS++",
    "60FPS (no deltatime)",
    "60 FPS++",
    "90 FPS++",
    "Uncap FPS++",
    "Sprint Fix (High FPS)",
    "60 FPS++ (Deltatime)",
    "Performance Patch (perf increase)",
}


LIVE_CLOCK_SIGNATURES = (
    (0x2018E6B, b"\x48\x8d\x75\xc0\xe8"),
    (0x2018E36, b"\xc7\x45\xc8"),
)


def check_live_clock_signature(game_dir: Path) -> tuple[bool, str]:
    """Verify the CUSA03173 v1.09 eboot bytes used by the live FPS clock hook.

    The loader maps guest vaddr 0 to image offset 0. Parse the PS4 SELF with the same
    helper the game check uses, then read the guarded windows from the ELF image so a
    different eboot is reported instead of hooked (probe.c falls back to XML patches).
    """
    eboot_path = game_dir / "eboot.bin"
    try:
        data = eboot_path.read_bytes()
    except OSError as ex:
        return False, f"could not read eboot.bin ({ex})"
    try:
        scripts_dir = str(APP_DIR / "scripts")
        if scripts_dir not in sys.path:
            sys.path.insert(0, scripts_dir)
        from prepare import parse_self
        elf, _header, ph, _segments, _missing = parse_self(data, "eboot.bin")
    except Exception as ex:
        return False, f"eboot.bin could not be parsed ({ex})"
    loads = [p for p in ph if p["type"] in (1, 0x61000010)]
    if not loads:
        return False, "eboot.bin has no loadable segments"
    image_size = max(p["vaddr"] + p["memsz"] for p in loads)
    if image_size <= 0x2035000:
        return False, "eboot image is smaller than the live clock site"

    def read_vaddr(vaddr: int, size: int):
        for p in loads:
            if p["vaddr"] <= vaddr and vaddr + size <= p["vaddr"] + p["filesz"]:
                start = p["offset"] + (vaddr - p["vaddr"])
                return bytes(elf[start:start + size])
        return None

    for vaddr, expected in LIVE_CLOCK_SIGNATURES:
        if read_vaddr(vaddr, len(expected)) != expected:
            return False, "eboot signature does not match the CUSA03173 v1.09 live clock site"
    return True, "live clock signature verified"


def verify_prelaunch(game_dir: Path) -> tuple[bool, str, list[str]]:
    details = []

    eboot_path = game_dir / "eboot.bin"
    if not eboot_path.is_file():
        return False, f"Missing eboot.bin in game directory:\n{game_dir}", details
    eboot_size = eboot_path.stat().st_size
    if eboot_size == 0:
        return False, f"eboot.bin is empty in:\n{eboot_path}", details
    details.append(f"[OK] Game executable: eboot.bin ({eboot_size / (1024*1024):.1f} MiB)")

    try:
        scripts_dir = str(APP_DIR / "scripts")
        if scripts_dir not in sys.path:
            sys.path.insert(0, scripts_dir)
        import game_check
        chk = game_check.problem(str(game_dir))
        if chk:
            kind, title, version = chk
            explanation = game_check.explain(kind, title, version)
            return False, f"Game File Verification Failed ({kind}):\n\n{explanation}", details
        details.append("[OK] Game Title & Version: Bloodborne CUSA03173 v1.09 verified")
    except Exception:
        details.append("[NOTE] Version check bypassed (standalone)")

    live_ok, live_msg = check_live_clock_signature(game_dir)
    if live_ok:
        details.append(f"[OK] FPS patch: {live_msg}")
    else:
        details.append(f"[WARN] FPS patch: {live_msg}; static XML patches will be used")

    dvdroot = game_dir / "dvdroot_ps4"
    if dvdroot.is_dir():
        details.append("[OK] Asset directory: dvdroot_ps4 detected")

    if not RUN_BAT.exists():
        return False, f"Launcher script run.bat not found in:\n{APP_DIR}", details
    details.append("[OK] Launcher script: run.bat ready")

    bbport_exe = APP_DIR / "out" / "bbport.exe"
    if not bbport_exe.is_file():
        return False, f"Compiled binary bbport.exe missing at:\n{bbport_exe}\nRun build.bat first.", details
    details.append(f"[OK] Compiled binary: bbport.exe ({bbport_exe.stat().st_size / (1024*1024):.1f} MiB)")

    try:
        vk = ctypes.CDLL("vulkan-1.dll")
        details.append("[OK] Vulkan runtime: vulkan-1.dll loaded")
        if os.name == "nt" and hasattr(vk, "_handle") and vk._handle:
            ctypes.windll.kernel32.FreeLibrary(ctypes.c_void_p(vk._handle))
    except OSError:
        details.append("[WARN] vulkan-1.dll not detected in system path")

    # CPU instruction check: AVX 1.0 support
    avx_ok, avx_msg = check_cpu_avx_support()
    if not avx_ok:
        return False, f"Hardware Incompatibility (Unsupported CPU):\n\n{avx_msg}", details
    details.append("[OK] CPU Architecture: AVX 1.0+ vector extensions verified")

    return True, "", details


def check_cpu_avx_support() -> tuple[bool, str]:
    if os.name != "nt":
        return True, ""
    try:
        kernel32 = ctypes.windll.kernel32
        kernel32.VirtualAlloc.restype = ctypes.c_void_p
        kernel32.VirtualAlloc.argtypes = [ctypes.c_void_p, ctypes.c_size_t, ctypes.c_uint32, ctypes.c_uint32]
        kernel32.VirtualFree.argtypes = [ctypes.c_void_p, ctypes.c_size_t, ctypes.c_uint32]

        shellcode = bytes([
            0x53,                               # push rbx
            0xB8, 0x01, 0x00, 0x00, 0x00,       # mov eax, 1
            0x31, 0xC9,                         # xor ecx, ecx
            0x0F, 0xA2,                         # cpuid
            0xF7, 0xC1, 0x00, 0x00, 0x00, 0x10, # test ecx, 10000000h (bit 28 - AVX)
            0x74, 0x1A,                         # jz no_avx
            0xF7, 0xC1, 0x00, 0x00, 0x00, 0x08, # test ecx, 08000000h (bit 27 - OSXSAVE)
            0x74, 0x18,                         # jz no_osxsave
            0x31, 0xC9,                         # xor ecx, ecx
            0x0F, 0x01, 0xD0,                   # xgetbv (eax:edx = XCR0)
            0x83, 0xE0, 0x06,                   # and eax, 6 (bit 1=XMM, bit 2=YMM)
            0x83, 0xF8, 0x06,                   # cmp eax, 6
            0x75, 0x0B,                         # jne no_osxsave
            0xB8, 0x01, 0x00, 0x00, 0x00,       # mov eax, 1 (AVX OK)
            0x5B,                               # pop rbx
            0xC3,                               # ret
            # no_osxsave:
            0xB8, 0xFF, 0xFF, 0xFF, 0xFF,       # mov eax, -1
            0x5B,                               # pop rbx
            0xC3,                               # ret
            # no_avx:
            0x31, 0xC0,                         # xor eax, eax
            0x5B,                               # pop rbx
            0xC3                                # ret
        ])
        ptr = kernel32.VirtualAlloc(None, len(shellcode), 0x3000, 0x40)
        if not ptr:
            return True, ""
        ctypes.memmove(ptr, shellcode, len(shellcode))
        fn = ctypes.CFUNCTYPE(ctypes.c_int32)(ptr)
        status = fn()
        kernel32.VirtualFree(ptr, 0, 0x8000)

        if status == 0:
            return False, (
                "Your CPU lacks support for AVX (Advanced Vector Extensions 1.0).\n\n"
                "Bloodborne and native PS4 system libraries are built with -mavx and "
                "require AVX hardware instructions to run.\n\n"
                "Older processors without AVX (such as Intel Core 2 Quad, Nehalem/Westmere "
                "Xeon X56xx, or first-gen Core i7) cannot execute these binaries."
            )
        elif status == -1:
            return False, (
                "Your CPU supports AVX, but AVX state saving (OSXSAVE / XCR0 YMM) is disabled "
                "or masked by the operating system / hypervisor.\n\n"
                "If running inside a virtual machine or hypervisor, please enable AVX/XSAVE passthrough."
            )
        return True, ""
    except Exception:
        return True, ""


class ProcessWorker(QThread):
    batch_emitted = Signal(list)
    process_finished = Signal(int)

    def __init__(self, cmd, env, cwd):
        super().__init__()
        self.cmd = cmd
        self.env = env
        self.cwd = cwd
        self.proc = None
        self._stop_requested = False

    def run(self):
        # STOP can be pressed while the worker is still starting.  Do not launch a game
        # that the user has already asked to stop.
        if self._stop_requested:
            self.process_finished.emit(-1)
            return
        try:
            self.proc = subprocess.Popen(
                self.cmd,
                cwd=str(self.cwd),
                env=self.env,
                stdin=subprocess.DEVNULL,
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                creationflags=NO_WINDOW,
            )
            # Close the race between the check above and Popen returning.
            if self._stop_requested:
                kill_tree(self.proc)
        except Exception as ex:
            self.batch_emitted.emit([f"Launch Error: {ex}"])
            self.process_finished.emit(-1)
            return

        batch = []
        last_flush = time.monotonic()

        try:
            for raw in iter(self.proc.stdout.readline, b""):
                batch.append(decode_line(raw).rstrip())
                now = time.monotonic()
                if len(batch) >= 60 or (now - last_flush) >= 0.035:
                    self.batch_emitted.emit(batch)
                    batch = []
                    last_flush = now
            if batch:
                self.batch_emitted.emit(batch)
        except Exception as ex:
            self.batch_emitted.emit([f"Read error: {ex}"])
        finally:
            try:
                self.proc.stdout.close()
            except OSError:
                pass
            rc = self.proc.wait()
            self.process_finished.emit(rc)

    def terminate_proc(self):
        self._stop_requested = True
        kill_tree(self.proc)