#!/usr/bin/env python3
"""Bloodborne PC (Windows) Launcher GUI."""

import json
import locale
import os
import queue
import subprocess
import threading
import tkinter as tk
import xml.etree.ElementTree as ET
from datetime import datetime
from pathlib import Path
from tkinter import ttk, filedialog, messagebox

APP_DIR = Path(__file__).resolve().parent
RUN_BAT = APP_DIR / "run.bat"
LOG_FILE = APP_DIR / "launcher.log"
SETTINGS_FILE = APP_DIR / "launcher_settings.json"
PATCHES_XML = APP_DIR / "patches" / "Bloodborne.xml"
DEFAULT_GAME = Path(os.environ.get("BB_GAME_DIR", "../CUSA03173"))

FPS_CHOICES = ["uncap", "60", "90", "30"]
RES_CHOICES = ["1920x1080 (Native)", "1280x720", "2560x1440", "3840x2160"]
TIMEOUT_CHOICES = ["0 (no limit)", "10", "30", "60", "120", "300", "600"]
ANISO_CHOICES = [
    ("16x", "16", "16x anisotropic filtering of scene textures (recommended)"),
    ("8x", "8", "8x anisotropic filtering"),
    ("4x", "4", "4x anisotropic filtering"),
    ("2x", "2", "2x anisotropic filtering"),
    ("Off", "0", "Game's default anisotropic filtering (no override)"),
]
MAX_LOG_LINES = 5000
VK_NOISE = "<Warning> vk_instance.cpp"
NO_WINDOW = getattr(subprocess, "CREATE_NO_WINDOW", 0)


def load_xml_patches(xml_path: Path = PATCHES_XML):
    """Load all 01.09 eboot.bin patches from Bloodborne.xml into a list of dicts."""
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
                note = meta.get("Note", "").strip()
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


def decode_line(raw: bytes) -> str:
    """Decode a console line: UTF-8 first, then the system code page."""
    try:
        return raw.decode("utf-8")
    except UnicodeDecodeError:
        return raw.decode(locale.getpreferredencoding(False), errors="replace")


def resolve_game_dir(target: str):
    """Return the game folder for an eboot.bin path or a folder path, else None."""
    p = Path(target.strip().strip('"'))
    if p.is_file() and p.name.lower() == "eboot.bin":
        return p.parent
    if p.is_dir() and (p / "eboot.bin").is_file():
        return p
    return None


def kill_tree(proc: subprocess.Popen):
    """Kill run.bat and everything it started (bbport.exe included)."""
    if os.name == "nt":
        subprocess.run(
            ["taskkill", "/PID", str(proc.pid), "/T", "/F"],
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
            creationflags=NO_WINDOW,
        )
    else:
        proc.kill()


class BloodborneLauncher(tk.Tk):
    def __init__(self):
        super().__init__()
        self.title("Bloodborne PC Launcher")
        self.geometry("780x880")
        self.minsize(680, 720)

        self.proc = None
        self.log_fh = None
        self.hidden_count = 0
        self.log_queue = queue.Queue()
        self.settings = self.load_settings()
        self.all_xml_patches = load_xml_patches()
        self.enabled_patches = set(self.settings.get("enabled_patches", ["Skip Intro"]))

        self.feat_skip_intro = tk.BooleanVar(value="Skip Intro" in self.enabled_patches)
        self.feat_perf_patch = tk.BooleanVar(value="Performance Patch (perf increase)" in self.enabled_patches)
        self.feat_no_blur = tk.BooleanVar(value="Disable Motion Blur (perf increase)" in self.enabled_patches)

        self.build_style()
        self.build_ui()

        self.protocol("WM_DELETE_WINDOW", self.on_close)
        self.after(50, self.poll_queue)
        self.log("Ready. Select your eboot.bin and press Launch Bloodborne.")

    # ------------------------------------------------------------------ UI
    def build_style(self):
        self.configure(bg="#1a1a1a")
        style = ttk.Style(self)
        style.theme_use("clam")

        bg_dark, bg_card, fg_text = "#1a1a1a", "#252526", "#e0e0e0"
        bg_input = "#1e1e1e"
        accent, accent_hover = "#990000", "#b30000"

        style.configure(".", background=bg_dark, foreground=fg_text, font=("Segoe UI", 10))
        style.configure("TLabel", background=bg_dark, foreground=fg_text)
        style.configure("Card.TFrame", background=bg_card, relief="flat")
        style.configure("Card.TLabel", background=bg_card, foreground=fg_text)
        style.configure("Card.TCheckbutton", background=bg_card, foreground=fg_text)
        style.configure("Header.TLabel", font=("Segoe UI", 16, "bold"), foreground="#c5a059", background=bg_dark)
        style.configure("SubHeader.TLabel", font=("Segoe UI", 9), foreground="#888888", background=bg_dark)

        # Path Entry styling
        style.configure("TEntry",
                        fieldbackground=bg_input,
                        foreground="#ffffff",
                        insertcolor="#ffffff",
                        bordercolor="#3e3e42",
                        lightcolor="#3e3e42",
                        darkcolor="#3e3e42")

        # Combobox styling
        style.configure("TCombobox",
                        fieldbackground=bg_input,
                        background="#333333",
                        foreground="#ffffff",
                        arrowcolor="#ffffff",
                        bordercolor="#3e3e42",
                        lightcolor="#3e3e42",
                        darkcolor="#3e3e42")
        style.map("TCombobox",
                  fieldbackground=[("readonly", bg_input)],
                  foreground=[("readonly", "#ffffff")],
                  selectbackground=[("readonly", bg_input)],
                  selectforeground=[("readonly", "#ffffff")])

        # Style the dropdown popdown listbox via Tk option database
        self.option_add("*TCombobox*Listbox.background", bg_input)
        self.option_add("*TCombobox*Listbox.foreground", "#ffffff")
        self.option_add("*TCombobox*Listbox.selectBackground", accent)
        self.option_add("*TCombobox*Listbox.selectForeground", "#ffffff")

        style.configure("Action.TButton", font=("Segoe UI", 11, "bold"),
                        background=accent, foreground="white", borderwidth=0)
        style.map("Action.TButton",
                  background=[("disabled", "#4a2a2a"), ("active", accent_hover)],
                  foreground=[("disabled", "#888888")])

        style.configure("Secondary.TButton", font=("Segoe UI", 9),
                        background="#3a3a3c", foreground="white", borderwidth=0)
        style.map("Secondary.TButton",
                  background=[("disabled", "#2a2a2c"), ("active", "#4a4a4c")],
                  foreground=[("disabled", "#777777")])

    def build_ui(self):
        main = ttk.Frame(self, padding=16)
        main.pack(fill="both", expand=True)

        ttk.Label(main, text="BLOODBORNE PC", style="Header.TLabel").pack(anchor="w")
        ttk.Label(main, text="Native Windows Port (bbport) Launcher",
                  style="SubHeader.TLabel").pack(anchor="w", pady=(0, 10))

        # eboot.bin selection
        card = ttk.Frame(main, style="Card.TFrame", padding=12)
        card.pack(fill="x", pady=(0, 10))
        ttk.Label(card, text="Game Executable (eboot.bin):", style="Card.TLabel",
                  font=("Segoe UI", 10, "bold")).pack(anchor="w", pady=(0, 5))

        select_frame = ttk.Frame(card, style="Card.TFrame")
        select_frame.pack(fill="x")

        initial_path = self.settings.get("eboot", "")
        if not initial_path or not Path(initial_path).exists():
            initial_path = ""
            if (DEFAULT_GAME / "eboot.bin").exists():
                initial_path = str(DEFAULT_GAME / "eboot.bin")
            elif DEFAULT_GAME.exists():
                initial_path = str(DEFAULT_GAME)

        self.eboot_var = tk.StringVar(value=initial_path)
        ttk.Entry(select_frame, textvariable=self.eboot_var,
                  font=("Consolas", 10)).pack(side="left", fill="x", expand=True, padx=(0, 10))
        ttk.Button(select_frame, text="Browse...", style="Secondary.TButton",
                   command=self.browse_eboot).pack(side="right")

        # Performance & Render options
        opts = ttk.Frame(main, style="Card.TFrame", padding=12)
        opts.pack(fill="x", pady=(0, 10))
        ttk.Label(opts, text="Performance & Render Options:", style="Card.TLabel",
                  font=("Segoe UI", 10, "bold")).grid(row=0, column=0, columnspan=4, sticky="w", pady=(0, 8))

        fps = self.settings.get("fps", "uncap")
        res = self.settings.get("res", RES_CHOICES[0])
        if res in ("Default (1080p)", "1920x1080"):
            res = RES_CHOICES[0]
        timeout_val = str(self.settings.get("timeout", "0"))
        if timeout_val not in [c.split()[0] for c in TIMEOUT_CHOICES]:
            matching_choice = "0 (no limit)"
        else:
            matching_choice = next((c for c in TIMEOUT_CHOICES if c.startswith(timeout_val + " ") or c == timeout_val), TIMEOUT_CHOICES[0])

        aniso = self.settings.get("aniso", "16x")
        if aniso not in [c[0] for c in ANISO_CHOICES]:
            aniso = "16x"

        self.fps_var = tk.StringVar(value=fps if fps in FPS_CHOICES else "uncap")
        self.res_var = tk.StringVar(value=res if res in RES_CHOICES else RES_CHOICES[0])
        self.fullscreen = tk.BooleanVar(value=self.settings.get("fullscreen", False))
        self.timeout_var = tk.StringVar(value=matching_choice)
        self.aniso_var = tk.StringVar(value=aniso)
        self.aniso_desc_var = tk.StringVar()

        ttk.Label(opts, text="Frame Rate Target:", style="Card.TLabel").grid(row=1, column=0, sticky="w", pady=3)
        ttk.Combobox(opts, textvariable=self.fps_var, values=FPS_CHOICES,
                     state="readonly", width=12).grid(row=1, column=1, sticky="w", pady=3, padx=(6, 20))

        ttk.Label(opts, text="Render Resolution:", style="Card.TLabel").grid(row=1, column=2, sticky="w", pady=3)
        self.res_combo = ttk.Combobox(opts, textvariable=self.res_var, values=RES_CHOICES,
                                      state="readonly", width=16)
        self.res_combo.grid(row=1, column=3, sticky="w", pady=3, padx=(6, 0))
        ttk.Checkbutton(opts, text="Fullscreen", variable=self.fullscreen,
                        style="Card.TCheckbutton").grid(row=1, column=4, sticky="w", pady=3, padx=(12, 0))

        ttk.Label(opts, text="Anisotropic Filtering:", style="Card.TLabel").grid(row=2, column=0, sticky="w", pady=3)
        self.aniso_combo = ttk.Combobox(opts, textvariable=self.aniso_var,
                                        values=[c[0] for c in ANISO_CHOICES],
                                        state="readonly", width=12)
        self.aniso_combo.grid(row=2, column=1, sticky="w", pady=3, padx=(6, 20))
        self.aniso_combo.bind("<<ComboboxSelected>>", lambda e: self.update_aniso_desc())

        self.aniso_desc_lbl = ttk.Label(opts, textvariable=self.aniso_desc_var,
                                        style="Card.TLabel", font=("Segoe UI", 8),
                                        foreground="#888888")
        self.aniso_desc_lbl.grid(row=2, column=2, columnspan=2, sticky="w", pady=3)
        self.update_aniso_desc()

        # Features Card
        feat_card = ttk.Frame(main, style="Card.TFrame", padding=12)
        feat_card.pack(fill="x", pady=(0, 10))

        feat_header = ttk.Frame(feat_card, style="Card.TFrame")
        feat_header.pack(fill="x", pady=(0, 8))
        ttk.Label(feat_header, text="Features & Toggles:", style="Card.TLabel",
                  font=("Segoe UI", 10, "bold")).pack(side="left")

        ttk.Button(feat_header, text="⚙  All XML Patches...", style="Secondary.TButton",
                   command=self.open_patches_dialog).pack(side="left", padx=(12, 0))
        ttk.Button(feat_header, text="⌨  Mouse & Keyboard...", style="Secondary.TButton",
                   command=self.open_mk_dialog).pack(side="left", padx=(6, 0))

        ttk.Button(feat_header, text="Vanilla mode", style="Secondary.TButton",
                   command=self.set_vanilla_mode).pack(side="right", padx=(6, 0))
        ttk.Button(feat_header, text="Everything on", style="Secondary.TButton",
                   command=self.set_everything_on).pack(side="right")

        self.feat_upscaler = tk.BooleanVar(value=self.settings.get("feat_upscaler", True))
        self.feat_overlay = tk.BooleanVar(value=self.settings.get("feat_overlay", True))
        self.feat_hud = tk.BooleanVar(value=self.settings.get("feat_hud", False))
        self.feat_fps_patch = tk.BooleanVar(value=self.settings.get("feat_fps_patch", True))
        self.feat_mods = tk.BooleanVar(value=self.settings.get("mods", True))
        self.feat_res_scaling = tk.BooleanVar(value=self.settings.get("feat_res_scaling", True))

        grid_f = ttk.Frame(feat_card, style="Card.TFrame")
        grid_f.pack(fill="x")

        # Row 0
        ttk.Checkbutton(grid_f, text="Upscaler (FSR 3.1)", variable=self.feat_upscaler,
                        style="Card.TCheckbutton").grid(row=0, column=0, sticky="w", pady=2, padx=(0, 15))
        ttk.Checkbutton(grid_f, text="FPS patch (Uncap / 60 / 90)", variable=self.feat_fps_patch,
                        style="Card.TCheckbutton").grid(row=0, column=1, sticky="w", pady=2, padx=(0, 15))
        ov_box = ttk.Frame(grid_f, style="Card.TFrame")
        ov_box.grid(row=0, column=2, sticky="w", pady=2)
        ttk.Checkbutton(ov_box, text="Overlay menu", variable=self.feat_overlay,
                        style="Card.TCheckbutton").pack(side="left")
        ttk.Checkbutton(ov_box, text="HUD stats", variable=self.feat_hud,
                        style="Card.TCheckbutton").pack(side="left", padx=(10, 0))

        # Row 1
        ttk.Checkbutton(grid_f, text="Enable mods folder (mods/)", variable=self.feat_mods,
                        style="Card.TCheckbutton").grid(row=1, column=0, sticky="w", pady=2, padx=(0, 15))
        cb_res = ttk.Checkbutton(grid_f, text="Resolution scaling", variable=self.feat_res_scaling,
                                 style="Card.TCheckbutton", command=self.update_res_scaling_state)
        cb_res.grid(row=1, column=1, sticky="w", pady=2, padx=(0, 15))

        # Row 2 (Popular XML Patches)
        ttk.Checkbutton(grid_f, text="Skip Intro & Logos", variable=self.feat_skip_intro,
                        style="Card.TCheckbutton", command=self.on_quick_patch_toggle).grid(row=2, column=0, sticky="w", pady=2, padx=(0, 15))
        ttk.Checkbutton(grid_f, text="Performance Patch (Kyo)", variable=self.feat_perf_patch,
                        style="Card.TCheckbutton", command=self.on_quick_patch_toggle).grid(row=2, column=1, sticky="w", pady=2, padx=(0, 15))
        ttk.Checkbutton(grid_f, text="Disable Motion Blur", variable=self.feat_no_blur,
                        style="Card.TCheckbutton", command=self.on_quick_patch_toggle).grid(row=2, column=2, sticky="w", pady=2)

        self.update_res_scaling_state()

        # launch / stop
        action = ttk.Frame(main)
        action.pack(fill="x", pady=(4, 8))
        self.launch_btn = ttk.Button(action, text="▶  LAUNCH BLOODBORNE", style="Action.TButton",
                                     command=self.launch_game)
        self.launch_btn.pack(side="left", fill="x", expand=True, ipady=8)
        self.stop_btn = ttk.Button(action, text="■  Stop", style="Secondary.TButton",
                                   command=self.stop_game, state="disabled")
        self.stop_btn.pack(side="right", padx=(10, 0), ipady=8)

        # log header
        log_header = ttk.Frame(main)
        log_header.pack(fill="x")
        ttk.Label(log_header, text="Launcher Log:", font=("Segoe UI", 9, "bold")).pack(side="left")
        self.hide_vk_var = tk.BooleanVar(value=self.settings.get("hide_vk", True))
        ttk.Checkbutton(log_header, text="Hide Vulkan warnings",
                        variable=self.hide_vk_var).pack(side="right")

        # bottom button bar
        bar = ttk.Frame(main)
        bar.pack(side="bottom", fill="x", pady=(6, 0))
        ttk.Button(bar, text="✓ Verify Setup", style="Secondary.TButton",
                   command=self.manual_verify_setup).pack(side="left")
        ttk.Button(bar, text="Copy log", style="Secondary.TButton", command=self.copy_log).pack(side="left", padx=6)
        ttk.Button(bar, text="Clear", style="Secondary.TButton", command=self.clear_log).pack(side="left")
        ttk.Button(bar, text="Open launcher.log", style="Secondary.TButton",
                   command=self.open_log_file).pack(side="left", padx=6)

        # log text + scrollbar
        log_frame = ttk.Frame(main)
        log_frame.pack(fill="both", expand=True, pady=(2, 0))
        self.log_text = tk.Text(log_frame, height=8, bg="#111111", fg="#a0a0a0", insertbackground="white",
                                font=("Consolas", 9), relief="flat", wrap="word")
        scroll = ttk.Scrollbar(log_frame, orient="vertical", command=self.log_text.yview)
        self.log_text.configure(yscrollcommand=scroll.set)
        scroll.pack(side="right", fill="y")
        self.log_text.pack(side="left", fill="both", expand=True)

    def update_aniso_desc(self):
        curr = self.aniso_var.get()
        desc = next((c[2] for c in ANISO_CHOICES if c[0] == curr), "")
        self.aniso_desc_var.set(f"↳ {desc}" if desc else "")

    def update_res_scaling_state(self):
        if self.feat_res_scaling.get():
            self.res_combo.config(state="readonly")
        else:
            self.res_combo.config(state="disabled")

    def set_vanilla_mode(self):
        self.feat_upscaler.set(False)
        self.feat_overlay.set(False)
        self.feat_hud.set(False)
        self.feat_fps_patch.set(False)
        self.feat_mods.set(False)
        self.feat_res_scaling.set(False)
        self.fps_var.set("30")
        self.aniso_var.set("Off")
        self.enabled_patches.clear()
        self.sync_quick_patch_vars()
        self.update_aniso_desc()
        self.update_res_scaling_state()
        self.save_settings()
        self.log("Preset applied: Vanilla mode (all optional features off, FPS preset=30 native, BB_ANISO=0, all patches off).")

    def set_everything_on(self):
        self.feat_upscaler.set(True)
        self.feat_overlay.set(True)
        self.feat_hud.set(True)
        self.feat_fps_patch.set(True)
        self.feat_mods.set(True)
        self.feat_res_scaling.set(True)
        self.fps_var.set("uncap")
        self.aniso_var.set("16x")
        self.enabled_patches = {"Skip Intro", "Performance Patch (perf increase)", "Disable Motion Blur (perf increase)"}
        self.sync_quick_patch_vars()
        self.update_aniso_desc()
        self.update_res_scaling_state()
        self.save_settings()
        self.log("Preset applied: Everything on (restored all defaults and recommended patches).")

    def on_quick_patch_toggle(self):
        if self.feat_skip_intro.get():
            self.enabled_patches.add("Skip Intro")
        else:
            self.enabled_patches.discard("Skip Intro")

        if self.feat_perf_patch.get():
            self.enabled_patches.add("Performance Patch (perf increase)")
        else:
            self.enabled_patches.discard("Performance Patch (perf increase)")

        if self.feat_no_blur.get():
            self.enabled_patches.add("Disable Motion Blur (perf increase)")
        else:
            self.enabled_patches.discard("Disable Motion Blur (perf increase)")

        self.save_settings()

    def sync_quick_patch_vars(self):
        self.feat_skip_intro.set("Skip Intro" in self.enabled_patches)
        self.feat_perf_patch.set("Performance Patch (perf increase)" in self.enabled_patches)
        self.feat_no_blur.set("Disable Motion Blur (perf increase)" in self.enabled_patches)

    def open_patches_dialog(self):
        dlg = tk.Toplevel(self)
        dlg.title(f"Bloodborne XML Patch Manager ({len(self.all_xml_patches)} Patches)")
        dlg.geometry("740x700")
        dlg.minsize(580, 500)
        dlg.transient(self)
        dlg.grab_set()
        dlg.configure(bg="#1a1a1a")

        working_set = set(self.enabled_patches)
        bg_dark = "#1a1a1a"
        bg_card = "#252526"

        top_frame = ttk.Frame(dlg, style="Card.TFrame", padding=10)
        top_frame.pack(fill="x", padx=10, pady=(10, 6))

        filter_row = ttk.Frame(top_frame, style="Card.TFrame")
        filter_row.pack(fill="x", pady=(0, 6))

        ttk.Label(filter_row, text="Search:", style="Card.TLabel").pack(side="left", padx=(0, 4))
        search_var = tk.StringVar()
        search_entry = ttk.Entry(filter_row, textvariable=search_var, width=22)
        search_entry.pack(side="left", padx=(0, 10))

        ttk.Label(filter_row, text="Category:", style="Card.TLabel").pack(side="left", padx=(0, 4))
        cats = ["All Categories", "Performance & Fixes", "Visuals & Camera", "Gameplay & Cheats",
                "Framerate & Engine", "Resolutions & Grids", "Debug & Tools"]
        cat_var = tk.StringVar(value="All Categories")
        cat_combo = ttk.Combobox(filter_row, textvariable=cat_var, values=cats, state="readonly", width=18)
        cat_combo.pack(side="left")

        btn_row = ttk.Frame(top_frame, style="Card.TFrame")
        btn_row.pack(fill="x")

        status_lbl = ttk.Label(btn_row, text="", style="Card.TLabel", font=("Segoe UI", 9, "bold"))
        status_lbl.pack(side="right")

        list_container = ttk.Frame(dlg, style="Card.TFrame", padding=6)
        list_container.pack(fill="both", expand=True, padx=10, pady=(0, 6))

        canvas = tk.Canvas(list_container, bg=bg_dark, highlightthickness=0)
        vscroll = ttk.Scrollbar(list_container, orient="vertical", command=canvas.yview)
        canvas.configure(yscrollcommand=vscroll.set)

        scrollable_frame = ttk.Frame(canvas, style="Card.TFrame")
        scrollable_frame.bind(
            "<Configure>",
            lambda e: canvas.configure(scrollregion=canvas.bbox("all"))
        )
        canvas_window = canvas.create_window((0, 0), window=scrollable_frame, anchor="nw")

        def on_canvas_configure(e):
            canvas.itemconfig(canvas_window, width=e.width)
        canvas.bind("<Configure>", on_canvas_configure)

        def on_mousewheel(event):
            canvas.yview_scroll(int(-1 * (event.delta / 120)), "units")
        dlg.bind("<MouseWheel>", on_mousewheel)

        canvas.pack(side="left", fill="both", expand=True)
        vscroll.pack(side="right", fill="y")

        all_patch_vars = {}
        visible_vars = {}
        for p in self.all_xml_patches:
            pname = p["name"]
            all_patch_vars[pname] = tk.BooleanVar(value=(pname in working_set))

        def update_status():
            status_lbl.config(text=f"{len(working_set)} of {len(self.all_xml_patches)} active")

        def make_toggle_cb(pname, var):
            def on_toggle():
                if var.get():
                    working_set.add(pname)
                else:
                    working_set.discard(pname)
                update_status()
            return on_toggle

        def select_visible():
            for p, var in visible_vars.items():
                var.set(True)
                working_set.add(p)
            update_status()

        def deselect_visible():
            for p, var in visible_vars.items():
                var.set(False)
                working_set.discard(p)
            update_status()

        def set_recommended():
            working_set.clear()
            working_set.update(["Skip Intro", "Performance Patch (perf increase)", "Disable Motion Blur (perf increase)"])
            for p, var in all_patch_vars.items():
                var.set(p in working_set)
            update_status()

        ttk.Button(btn_row, text="Select Visible", style="Secondary.TButton",
                   command=select_visible).pack(side="left", padx=(0, 6))
        ttk.Button(btn_row, text="Deselect Visible", style="Secondary.TButton",
                   command=deselect_visible).pack(side="left", padx=(0, 6))
        ttk.Button(btn_row, text="Recommended Defaults", style="Secondary.TButton",
                   command=set_recommended).pack(side="left")

        def refresh_list(*args):
            for widget in scrollable_frame.winfo_children():
                widget.destroy()
            visible_vars.clear()

            q = search_var.get().strip().lower()
            selected_cat = cat_var.get()

            for patch in self.all_xml_patches:
                pname = patch["name"]
                author = patch["author"]
                note = patch["note"]
                cat = patch["category"]

                if selected_cat != "All Categories" and cat != selected_cat:
                    continue

                if q and (q not in pname.lower() and q not in note.lower() and q not in author.lower()):
                    continue

                var = all_patch_vars[pname]
                visible_vars[pname] = var

                item_card = tk.Frame(scrollable_frame, bg=bg_card, padx=8, pady=4, relief="flat",
                                     highlightbackground="#333333", highlightthickness=1)
                item_card.pack(fill="x", padx=4, pady=3)

                header_row = tk.Frame(item_card, bg=bg_card)
                header_row.pack(fill="x")

                cb = tk.Checkbutton(
                    header_row, text=pname, variable=var,
                    command=make_toggle_cb(pname, var),
                    bg=bg_card, fg="#ffffff", selectcolor="#1e1e1e",
                    activebackground=bg_card, activeforeground="#ffffff",
                    font=("Segoe UI", 9, "bold"), anchor="w"
                )
                cb.pack(side="left")

                badge_text = f"by {author}" if author else ""
                if badge_text:
                    tk.Label(header_row, text=badge_text, bg=bg_card, fg="#888888",
                             font=("Segoe UI", 8)).pack(side="left", padx=(8, 0))

                tk.Label(header_row, text=f"[{cat}]", bg=bg_card, fg="#c5a059",
                         font=("Segoe UI", 8)).pack(side="right")

                if note:
                    note_lbl = tk.Label(
                        item_card, text=note, bg=bg_card, fg="#aaaaaa",
                        font=("Segoe UI", 8, "italic"), justify="left", wraplength=640, anchor="w"
                    )
                    note_lbl.pack(fill="x", padx=(24, 0), pady=(1, 2))

        search_var.trace_add("write", refresh_list)
        cat_combo.bind("<<ComboboxSelected>>", refresh_list)

        refresh_list()
        update_status()

        bottom_bar = ttk.Frame(dlg, style="Card.TFrame", padding=10)
        bottom_bar.pack(fill="x", padx=10, pady=(0, 10))

        def on_save():
            self.enabled_patches = set(working_set)
            self.sync_quick_patch_vars()
            self.save_settings()
            self.log(f"[PATCHES] Saved {len(self.enabled_patches)} active patch(es): "
                     f"{', '.join(sorted(self.enabled_patches)) if self.enabled_patches else 'None'}")
            dlg.destroy()

        def on_cancel():
            dlg.destroy()

        dlg.protocol("WM_DELETE_WINDOW", on_cancel)

        ttk.Button(bottom_bar, text="Save & Apply", style="Action.TButton",
                   command=on_save).pack(side="right", padx=(8, 0), ipady=4)
        ttk.Button(bottom_bar, text="Cancel", style="Secondary.TButton",
                   command=on_cancel).pack(side="right", ipady=4)

    def open_mk_dialog(self):
        """Mouse & Keyboard Settings Dialog with real-time bbport.ini synchronization."""
        dlg = tk.Toplevel(self)
        dlg.title("Mouse & Keyboard Settings")
        dlg.geometry("640x620")
        dlg.minsize(560, 520)
        dlg.transient(self)
        dlg.grab_set()
        dlg.configure(bg="#1a1a1a")

        ini_path = APP_DIR / "bbport.ini"

        # Load current values from bbport.ini or fallback to defaults
        ini_values = {}
        if ini_path.is_file():
            try:
                for line in ini_path.read_text(encoding="utf-8").splitlines():
                    line = line.strip()
                    if line and not line.startswith("#") and "=" in line:
                        k, v = line.split("=", 1)
                        ini_values[k.strip()] = v.strip()
            except OSError:
                pass

        mk_enabled_var = tk.BooleanVar(value=ini_values.get("mk_enabled", "1") != "0")
        mk_invert_x_var = tk.BooleanVar(value=ini_values.get("mk_invert_x", "0") != "0")
        mk_invert_y_var = tk.BooleanVar(value=ini_values.get("mk_invert_y", "0") != "0")

        try:
            sens_x_val = float(ini_values.get("mk_sens_x", "1.0"))
        except ValueError:
            sens_x_val = 1.0

        try:
            sens_y_val = float(ini_values.get("mk_sens_y", "1.0"))
        except ValueError:
            sens_y_val = 1.0

        try:
            deadzone_val = float(ini_values.get("mk_deadzone", "0.05"))
        except ValueError:
            deadzone_val = 0.05

        try:
            smoothing_val = float(ini_values.get("mk_smoothing", "0.2"))
        except ValueError:
            smoothing_val = 0.2

        sens_x_var = tk.DoubleVar(value=sens_x_val)
        sens_y_var = tk.DoubleVar(value=sens_y_val)
        deadzone_var = tk.DoubleVar(value=deadzone_val)
        smoothing_var = tk.DoubleVar(value=smoothing_val)

        main_frame = ttk.Frame(dlg, style="Card.TFrame", padding=14)
        main_frame.pack(fill="both", expand=True, padx=12, pady=12)

        ttk.Label(main_frame, text="Mouse & Keyboard Controls", style="Card.TLabel",
                  font=("Segoe UI", 12, "bold")).pack(anchor="w", pady=(0, 4))
        ttk.Label(main_frame, text="Configurable mouse and keyboard controls with real-time in-game synchronization.",
                  style="Card.TLabel", font=("Segoe UI", 9), foreground="#aaaaaa").pack(anchor="w", pady=(0, 10))

        ttk.Checkbutton(main_frame, text="Enable Mouse & Keyboard Mode",
                        variable=mk_enabled_var, style="Card.TCheckbutton").pack(anchor="w", pady=(0, 10))

        # Camera & Sensitivity card
        cam_card = ttk.Frame(main_frame, style="Card.TFrame", padding=10)
        cam_card.pack(fill="x", pady=(0, 10))

        ttk.Label(cam_card, text="Camera & Sensitivity Options", style="Card.TLabel",
                  font=("Segoe UI", 10, "bold")).grid(row=0, column=0, columnspan=3, sticky="w", pady=(0, 8))

        # Sens X
        ttk.Label(cam_card, text="Horizontal Sensitivity (X):", style="Card.TLabel").grid(row=1, column=0, sticky="w", pady=4)
        lbl_sx = ttk.Label(cam_card, text=f"{sens_x_var.get():.2f}", style="Card.TLabel", width=6)
        scale_sx = ttk.Scale(cam_card, from_=0.1, to=5.0, variable=sens_x_var,
                             command=lambda v: lbl_sx.config(text=f"{float(v):.2f}"))
        scale_sx.grid(row=1, column=1, sticky="ew", padx=8, pady=4)
        lbl_sx.grid(row=1, column=2, sticky="w", pady=4)

        # Sens Y
        ttk.Label(cam_card, text="Vertical Sensitivity (Y):", style="Card.TLabel").grid(row=2, column=0, sticky="w", pady=4)
        lbl_sy = ttk.Label(cam_card, text=f"{sens_y_var.get():.2f}", style="Card.TLabel", width=6)
        scale_sy = ttk.Scale(cam_card, from_=0.1, to=5.0, variable=sens_y_var,
                             command=lambda v: lbl_sy.config(text=f"{float(v):.2f}"))
        scale_sy.grid(row=2, column=1, sticky="ew", padx=8, pady=4)
        lbl_sy.grid(row=2, column=2, sticky="w", pady=4)

        # Deadzone
        ttk.Label(cam_card, text="Mouse Deadzone:", style="Card.TLabel").grid(row=3, column=0, sticky="w", pady=4)
        lbl_dz = ttk.Label(cam_card, text=f"{deadzone_var.get():.2f}", style="Card.TLabel", width=6)
        scale_dz = ttk.Scale(cam_card, from_=0.0, to=0.2, variable=deadzone_var,
                             command=lambda v: lbl_dz.config(text=f"{float(v):.2f}"))
        scale_dz.grid(row=3, column=1, sticky="ew", padx=8, pady=4)
        lbl_dz.grid(row=3, column=2, sticky="w", pady=4)

        # Smoothing
        ttk.Label(cam_card, text="Mouse Smoothing:", style="Card.TLabel").grid(row=4, column=0, sticky="w", pady=4)
        lbl_sm = ttk.Label(cam_card, text=f"{smoothing_var.get():.2f}", style="Card.TLabel", width=6)
        scale_sm = ttk.Scale(cam_card, from_=0.0, to=0.8, variable=smoothing_var,
                             command=lambda v: lbl_sm.config(text=f"{float(v):.2f}"))
        scale_sm.grid(row=4, column=1, sticky="ew", padx=8, pady=4)
        lbl_sm.grid(row=4, column=2, sticky="w", pady=4)

        # Invert checkboxes
        inv_frame = ttk.Frame(cam_card, style="Card.TFrame")
        inv_frame.grid(row=5, column=0, columnspan=3, sticky="w", pady=(8, 0))
        ttk.Checkbutton(inv_frame, text="Invert Horizontal Camera (X)",
                        variable=mk_invert_x_var, style="Card.TCheckbutton").pack(side="left", padx=(0, 16))
        ttk.Checkbutton(inv_frame, text="Invert Vertical Camera (Y)",
                        variable=mk_invert_y_var, style="Card.TCheckbutton").pack(side="left")

        cam_card.columnconfigure(1, weight=1)

        # Keybinds Reference Card
        bind_card = ttk.Frame(main_frame, style="Card.TFrame", padding=10)
        bind_card.pack(fill="both", expand=True, pady=(0, 10))

        ttk.Label(bind_card, text="Control Bindings", style="Card.TLabel",
                  font=("Segoe UI", 10, "bold")).pack(anchor="w", pady=(0, 6))

        bindings = [
            ("Left Click (LMB)", "Right Hand Attack (R1) / Trick Normal Attack"),
            ("Shift + Left Click", "Strong / Heavy Attack (R2)"),
            ("Right Click (RMB)", "Left Hand Weapon (L2) / Fire Firearm"),
            ("Shift + Right Click", "Trick Weapon Transform (L1)"),
            ("Middle Click / Q", "Lock-On Target / Reset Camera (R3)"),
            ("W / A / S / D", "Character Movement (Left Stick)"),
            ("Space", "Roll / Backstep / Sprint (Circle)"),
            ("E", "Action / Interact / Talk (Cross)"),
            ("R", "Use Quick Item / Blood Vial (Square)"),
            ("X", "Switch Weapon Mode / Two-Hand (Triangle)"),
            ("C / Z", "Crouch / Gestures (L3)"),
            ("Tab / G", "Gestures & Personal Effects (Touchpad Left)"),
            ("Esc", "Game Menu / Options (Options)"),
            ("Arrow Keys / 1,2,3,4", "Switch Consumables & Weapons (D-Pad)"),
        ]

        bind_container = ttk.Frame(bind_card, style="Card.TFrame")
        bind_container.pack(fill="both", expand=True)

        for idx, (k_name, k_act) in enumerate(bindings):
            row = idx % 7
            col = (idx // 7) * 2
            ttk.Label(bind_container, text=f"{k_name}:", style="Card.TLabel",
                      font=("Segoe UI", 8, "bold"), foreground="#c5a059").grid(row=row, column=col, sticky="w", padx=(0, 4), pady=2)
            ttk.Label(bind_container, text=k_act, style="Card.TLabel",
                      font=("Segoe UI", 8), foreground="#cccccc").grid(row=row, column=col+1, sticky="w", padx=(0, 16), pady=2)

        # Bottom actions
        action_bar = ttk.Frame(dlg, style="Card.TFrame", padding=10)
        action_bar.pack(fill="x", padx=12, pady=(0, 12))

        def write_mk_settings():
            # Update bbport.ini directly so game detects change in real time
            lines = []
            keys_to_update = {
                "mk_enabled": "1" if mk_enabled_var.get() else "0",
                "mk_sens_x": f"{sens_x_var.get():.2f}",
                "mk_sens_y": f"{sens_y_var.get():.2f}",
                "mk_invert_x": "1" if mk_invert_x_var.get() else "0",
                "mk_invert_y": "1" if mk_invert_y_var.get() else "0",
                "mk_deadzone": f"{deadzone_var.get():.2f}",
                "mk_smoothing": f"{smoothing_var.get():.2f}",
            }
            written_keys = set()
            if ini_path.is_file():
                try:
                    for line in ini_path.read_text(encoding="utf-8").splitlines():
                        trimmed = line.strip()
                        if trimmed and not trimmed.startswith("#") and "=" in trimmed:
                            k, _ = trimmed.split("=", 1)
                            k = k.strip()
                            if k in keys_to_update:
                                lines.append(f"{k}={keys_to_update[k]}")
                                written_keys.add(k)
                                continue
                        lines.append(line)
                except OSError:
                    pass

            for k, v in keys_to_update.items():
                if k not in written_keys:
                    lines.append(f"{k}={v}")

            try:
                ini_path.write_text("\n".join(lines) + "\n", encoding="utf-8")
                self.log(f"[M&K] Settings updated: sens=({sens_x_var.get():.2f}, {sens_y_var.get():.2f}), "
                         f"inv=({int(mk_invert_x_var.get())}, {int(mk_invert_y_var.get())}), enabled={mk_enabled_var.get()}")
            except OSError as ex:
                self.log(f"[ERROR] Could not save {ini_path}: {ex}")

        def on_save_close():
            write_mk_settings()
            dlg.destroy()

        def on_close():
            dlg.destroy()

        dlg.protocol("WM_DELETE_WINDOW", on_close)

        ttk.Button(action_bar, text="Save & Close", style="Action.TButton",
                   command=on_save_close).pack(side="right", padx=(8, 0), ipady=4)
        ttk.Button(action_bar, text="Cancel", style="Secondary.TButton",
                   command=on_close).pack(side="right", ipady=4)

    # ------------------------------------------------------------ settings
    def load_settings(self) -> dict:
        try:
            data = json.loads(SETTINGS_FILE.read_text(encoding="utf-8"))
            return data if isinstance(data, dict) else {}
        except (OSError, ValueError):
            return {}

    def save_settings(self):
        data = {
            "eboot": self.eboot_var.get().strip(),
            "fps": self.fps_var.get(),
            "res": self.res_var.get(),
            "aniso": self.aniso_var.get(),
            "mods": self.feat_mods.get(),
            "hide_vk": self.hide_vk_var.get(),
            "feat_upscaler": self.feat_upscaler.get(),
            "feat_overlay": self.feat_overlay.get(),
            "feat_hud": self.feat_hud.get(),
            "feat_fps_patch": self.feat_fps_patch.get(),
            "feat_res_scaling": self.feat_res_scaling.get(),
            "fullscreen": self.fullscreen.get(),
            "enabled_patches": sorted(list(self.enabled_patches)),
        }
        try:
            SETTINGS_FILE.write_text(json.dumps(data, indent=2), encoding="utf-8")
        except OSError:
            pass

    # ------------------------------------------------------------- logging
    def insert_lines(self, lines):
        if not lines:
            return
        at_bottom = self.log_text.yview()[1] >= 0.999
        self.log_text.insert("end", "\n".join(lines) + "\n")
        total = int(self.log_text.index("end-1c").split(".")[0])
        if total > MAX_LOG_LINES:
            self.log_text.delete("1.0", f"{total - MAX_LOG_LINES}.0")
        if at_bottom:
            self.log_text.see("end")

    def write_file(self, lines):
        if self.log_fh:
            try:
                self.log_fh.write("\n".join(lines) + "\n")
                self.log_fh.flush()
            except OSError:
                self.log_fh = None

    def log(self, message: str):
        self.write_file([message])
        self.insert_lines([message])

    def poll_queue(self):
        lines, finished = [], None
        try:
            for _ in range(500):
                item = self.log_queue.get_nowait()
                if isinstance(item, tuple):
                    finished = item[1]
                    break
                lines.append(item)
        except queue.Empty:
            pass

        if lines:
            for ln in lines:
                self.check_feature_warnings(ln)
            self.write_file(lines)
            if self.hide_vk_var.get():
                shown = [ln for ln in lines if VK_NOISE not in ln]
                self.hidden_count += len(lines) - len(shown)
                lines = shown
            self.insert_lines(lines)
        if finished is not None:
            self.on_finished(finished)
        self.after(50, self.poll_queue)

    def check_feature_warnings(self, line: str):
        if not self.feat_upscaler.get() and "Upscaler: FSR" in line and "available (on)" in line:
            self.log(f"[WARNING] Feature switch discrepancy: Upscaler is toggled OFF but startup reported: {line.strip()}")
        if not self.feat_overlay.get() and "Overlay: menu ready" in line:
            self.log(f"[WARNING] Feature switch discrepancy: Overlay menu is toggled OFF but startup reported: {line.strip()}")
        if not self.feat_fps_patch.get() and "writes from ['" in line and "FPS++" in line:
            self.log(f"[WARNING] Feature switch discrepancy: FPS patch is toggled OFF but patches reported: {line.strip()}")

    def copy_log(self):
        self.clipboard_clear()
        self.clipboard_append(self.log_text.get("1.0", "end-1c"))

    def clear_log(self):
        self.log_text.delete("1.0", "end")

    def open_log_file(self):
        if not LOG_FILE.exists():
            messagebox.showinfo("No log yet", "launcher.log is created the first time you launch the game.")
            return
        try:
            os.startfile(str(LOG_FILE))
        except (AttributeError, OSError) as ex:
            messagebox.showerror("Error", f"Could not open {LOG_FILE}:\n{ex}")

    # ------------------------------------------------------------- actions
    def browse_eboot(self):
        current = resolve_game_dir(self.eboot_var.get()) if self.eboot_var.get().strip() else None
        start = current or (DEFAULT_GAME if DEFAULT_GAME.exists() else Path("D:/"))
        chosen = filedialog.askopenfilename(
            title="Select Bloodborne eboot.bin",
            filetypes=[("PS4 Executable (*.bin)", "*.bin"), ("All files", "*.*")],
            initialdir=str(start),
        )
        if chosen:
            chosen = os.path.normpath(chosen)
            self.eboot_var.set(chosen)
            self.log(f"Selected executable: {chosen}")

    def verify_prelaunch(self, game_dir: Path) -> tuple[bool, str, list[str]]:
        """Verify that game directory, EBOOT, compiled source binaries, and scripts are ready."""
        details = []

        # 1. Game directory and eboot.bin
        eboot_path = game_dir / "eboot.bin"
        if not eboot_path.is_file():
            return False, f"Missing eboot.bin in game directory:\n{game_dir}\n\nPlease ensure your dumped game folder contains eboot.bin.", details
        eboot_size = eboot_path.stat().st_size
        if eboot_size == 0:
            return False, f"eboot.bin is empty (0 bytes) in:\n{eboot_path}", details
        eboot_mb = eboot_size / (1024 * 1024)
        details.append(f"[OK] Game executable: eboot.bin ({eboot_mb:.1f} MiB)")

        # 1b. Game Title & Version verification (CUSA03173 1.09)
        try:
            import sys
            scripts_dir = str(APP_DIR / "scripts")
            if scripts_dir not in sys.path:
                sys.path.insert(0, scripts_dir)
            import game_check
            chk = game_check.problem(str(game_dir))
            if chk:
                kind, title, version = chk
                explanation = game_check.explain(kind, title, version)
                return False, (
                    f"Game File Verification Failed ({kind}):\n\n"
                    f"{explanation}\n\n"
                    f"Expected: Bloodborne CUSA03173 with Update 1.09 merged.\n"
                    f"Why blocked: Upstream port patches and memory hooks rely strictly on the 1.09 executable layout.\n"
                    f"To bypass (not recommended): set environment variable BB_SKIP_GAME_CHECK=1."
                ), details
            else:
                details.append("[OK] Game Title & Version: Bloodborne CUSA03173 v1.09 verified")
        except Exception as e:
            details.append(f"[NOTE] Game version check check skipped: {e}")

        # 2. Check for game asset directory if present
        dvdroot = game_dir / "dvdroot_ps4"
        if dvdroot.is_dir():
            details.append("[OK] Game asset directory: dvdroot_ps4 detected")
        else:
            details.append("[NOTE] dvdroot_ps4 not detected directly under game root (custom layout or loose files)")

        # 3. Launcher script
        if not RUN_BAT.exists():
            return False, f"Launcher script run.bat not found in:\n{APP_DIR}", details
        details.append("[OK] Launcher script: run.bat ready")

        # 4. Compiled bbport.exe
        bbport_exe = APP_DIR / "out" / "bbport.exe"
        if not bbport_exe.is_file():
            return False, (
                f"Compiled game executable 'bbport.exe' was not found in:\n{bbport_exe}\n\n"
                "The source code has not been compiled yet.\n"
                "Please run 'build.bat' in the project directory to compile the project before launching."
            ), details
        bbport_stat = bbport_exe.stat()
        if bbport_stat.st_size == 0:
            return False, f"Compiled 'bbport.exe' is empty (0 bytes) in:\n{bbport_exe}", details
        bbport_time = datetime.fromtimestamp(bbport_stat.st_mtime).strftime("%Y-%m-%d %H:%M:%S")
        details.append(f"[OK] Compiled binary: bbport.exe ({bbport_stat.st_size / (1024*1024):.1f} MiB, built {bbport_time})")

        # 5. GPU subsystem
        bbgpu_dll = APP_DIR / "out" / "bbgpu.dll"
        libbbgpu_a = APP_DIR / "out" / "gpu" / "libbbgpu.a"
        if bbgpu_dll.is_file():
            bbgpu_stat = bbgpu_dll.stat()
            bbgpu_time = datetime.fromtimestamp(bbgpu_stat.st_mtime).strftime("%Y-%m-%d %H:%M:%S")
            details.append(f"[OK] GPU library: bbgpu.dll ({bbgpu_stat.st_size / (1024*1024):.1f} MiB, built {bbgpu_time})")
        elif libbbgpu_a.is_file():
            gpu_stat = libbbgpu_a.stat()
            gpu_time = datetime.fromtimestamp(gpu_stat.st_mtime).strftime("%Y-%m-%d %H:%M:%S")
            details.append(f"[OK] GPU subsystem: statically linked via libbbgpu.a ({gpu_stat.st_size / (1024*1024):.1f} MiB, built {gpu_time})")
        else:
            details.append("[OK] GPU subsystem: statically linked into bbport.exe")

        # 6. Critical runtime libraries staged in out/
        sdl3_dll = APP_DIR / "out" / "SDL3.dll"
        if sdl3_dll.is_file():
            details.append("[OK] Runtime library: out/SDL3.dll staged")
        else:
            details.append("[WARN] Runtime library: out/SDL3.dll not staged in out/")

        # 6. Patches script
        patches_py = APP_DIR / "scripts" / "patches.py"
        if not patches_py.is_file():
            return False, f"Game patch script not found at:\n{patches_py}", details
        details.append("[OK] Patch engine: scripts/patches.py ready")

        # 7. Check Vulkan driver runtime
        try:
            import ctypes
            ctypes.CDLL("vulkan-1.dll")
            details.append("[OK] Vulkan runtime: vulkan-1.dll loaded successfully")
        except OSError:
            details.append("[WARN] Vulkan runtime: vulkan-1.dll not found in standard system paths")

        return True, "", details

    def manual_verify_setup(self):
        target = self.eboot_var.get().strip()
        if not target:
            messagebox.showerror("Error", "Please select eboot.bin or the game folder first.")
            return

        game_dir = resolve_game_dir(target)
        if game_dir is None:
            messagebox.showerror("Invalid Path", f"Could not find eboot.bin in:\n{target}")
            return

        self.log("--- Running Pre-Launch Verification Check ---")
        ready, err_msg, details = self.verify_prelaunch(game_dir)
        for line in details:
            self.log(f"  {line}")

        if ready:
            self.log("[PRE-LAUNCH] All checks PASSED! System and game files are ready.")
            messagebox.showinfo("Verification Passed", "All components are ready for launch!\n\n" + "\n".join(details))
        else:
            self.log(f"[PRE-LAUNCH ERROR] {err_msg.splitlines()[0]}")
            messagebox.showerror("Pre-Launch Verification Failed", err_msg)

    def launch_game(self):
        if self.proc is not None:
            return

        target = self.eboot_var.get().strip()
        if not target:
            messagebox.showerror("Error", "Please select eboot.bin or the game folder first.")
            return

        game_dir = resolve_game_dir(target)
        if game_dir is None:
            messagebox.showerror("Invalid Path", f"Could not find eboot.bin in:\n{target}")
            return

        ready, err_msg, details = self.verify_prelaunch(game_dir)
        if not ready:
            self.log(f"[PRE-LAUNCH ERROR] Verification failed: {err_msg.splitlines()[0]}")
            messagebox.showerror("Pre-Launch Verification Failed", err_msg)
            return

        self.log("[PRE-LAUNCH] Pre-flight verification passed:")
        for line in details:
            self.log(f"  {line}")

        env = os.environ.copy()
        env["BB_GAME_DIR"] = str(game_dir)
        env["BB_FPS"] = self.fps_var.get()
        env["BB_MODS_ENABLED"] = "1" if self.feat_mods.get() else "0"

        # Upscaler (FSR)
        if not self.feat_upscaler.get():
            env["BB_UPSCALER"] = "off"
        else:
            env.pop("BB_UPSCALER", None)

        # Overlay menu & HUD stats
        if not self.feat_overlay.get():
            env["BB_OVERLAY"] = "0"
        else:
            env.pop("BB_OVERLAY", None)

        if self.feat_hud.get():
            env["BB_HUD"] = "1"
        else:
            env.pop("BB_HUD", None)

        # FPS patch
        if not self.feat_fps_patch.get():
            env["BB_FPS_PATCH"] = "0"
        else:
            env.pop("BB_FPS_PATCH", None)

        # Resolution scaling
        env.pop("BB_RENDER_RES", None)
        res_choice = self.res_var.get()
        if self.feat_res_scaling.get():
            if res_choice not in ("1920x1080 (Native)", "Default (1080p)", "1920x1080"):
                env["BB_RENDER_RES"] = res_choice
            else:
                res_choice = "1920x1080 (Native, no scaling patch)"
        else:
            res_choice = "Native (1080p, scaling off)"

        # Fullscreen window (read by gpu/shim/window.cpp)
        env["BB_FULLSCREEN"] = "1" if self.fullscreen.get() else "0"

        # Frame ahead queue (smooth frametimes & bound queue latency: 2 = balanced)
        env.setdefault("BB_FRAMES_AHEAD", "2")

        # Anisotropic filtering (0 = game default, 2/4/8/16 = forced)
        aniso_mapping = {c[0]: c[1] for c in ANISO_CHOICES}
        aniso_val = aniso_mapping.get(self.aniso_var.get(), "16")
        env["BB_ANISO"] = aniso_val

        # Bloodborne.xml Patches
        active_patches = sorted(list(self.enabled_patches))
        if active_patches:
            env["BB_PATCHES"] = ";".join(active_patches)
        else:
            env.pop("BB_PATCHES", None)

        try:
            self.log_fh = open(LOG_FILE, "w", encoding="utf-8")
        except OSError:
            self.log_fh = None

        features_str = (
            f"Features: FSR={'on' if self.feat_upscaler.get() else 'off'}, "
            f"Overlay={'on' if self.feat_overlay.get() else 'off'}, "
            f"HUD={'on' if self.feat_hud.get() else 'off'}, "
            f"FPSPatch={'on' if self.feat_fps_patch.get() else 'off'}, "
            f"Mods={'on' if self.feat_mods.get() else 'off'}, "
            f"ResScaling={'on' if self.feat_res_scaling.get() else 'off'}, "
            f"Fullscreen={'on' if self.fullscreen.get() else 'off'}"
        )

        self.hidden_count = 0
        self.log("=" * 50)
        self.log(f"Starting Bloodborne from: {game_dir}   [{datetime.now():%Y-%m-%d %H:%M:%S}]")
        self.log(f"FPS: {self.fps_var.get()} | Res: {res_choice} | Mods: {env['BB_MODS_ENABLED']}")
        self.log(features_str)
        self.log(f"XML Patches ({len(active_patches)} active): {', '.join(active_patches) if active_patches else 'None'}")
        self.log("=" * 50)

        cmd = ["cmd.exe", "/d", "/c", RUN_BAT.name] if os.name == "nt" else [str(RUN_BAT)]
        try:
            self.proc = subprocess.Popen(
                cmd,
                cwd=str(APP_DIR),
                env=env,
                stdin=subprocess.DEVNULL,
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                creationflags=NO_WINDOW,
            )
        except Exception as ex:
            self.log(f"Launch Error: {ex}")
            self.proc = None
            self.close_log_file()
            return

        self.save_settings()
        self.launch_btn.config(state="disabled")
        self.stop_btn.config(state="normal")
        threading.Thread(target=self.reader_thread, args=(self.proc,), daemon=True).start()

    def reader_thread(self, proc: subprocess.Popen):
        try:
            for raw in iter(proc.stdout.readline, b""):
                self.log_queue.put(decode_line(raw).rstrip())
        except Exception as ex:
            self.log_queue.put(f"Launcher read error: {ex}")
        finally:
            try:
                proc.stdout.close()
            except OSError:
                pass
            self.log_queue.put(("exit", proc.wait()))

    def stop_game(self):
        proc = self.proc
        if proc is not None and proc.poll() is None:
            self.log("Stopping game process tree...")
            kill_tree(proc)

    def on_finished(self, rc: int):
        if self.hidden_count:
            self.log(f"({self.hidden_count} Vulkan warning lines hidden; full output is in launcher.log)")
        code = f"{rc}" if 0 <= rc < 256 else f"{rc} (0x{rc & 0xFFFFFFFF:08X})"
        self.log(f"Process finished with code: {code}")
        self.proc = None
        self.close_log_file()
        self.launch_btn.config(state="normal")
        self.stop_btn.config(state="disabled")

    def close_log_file(self):
        if self.log_fh:
            try:
                self.log_fh.close()
            except OSError:
                pass
            self.log_fh = None

    def on_close(self):
        if self.proc is not None and self.proc.poll() is None:
            if not messagebox.askokcancel("Quit", "Bloodborne is still running. Stop it and quit?"):
                return
            kill_tree(self.proc)
        self.save_settings()
        self.destroy()


if __name__ == "__main__":
    BloodborneLauncher().mainloop()