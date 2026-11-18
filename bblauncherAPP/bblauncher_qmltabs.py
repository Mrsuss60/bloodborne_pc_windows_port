#!/usr/bin/env python3
"""Shim graph and signal wiring for the QML launcher shell.

This replaces the tab builders from the old widget UI.  It creates the shim
objects the controller expects, wires the same signals the widget code wired,
and exposes everything to QML as context properties.
"""

from __future__ import annotations

from pathlib import Path
from PySide6.QtCore import Property, QObject, Qt, QTimer, Signal, Slot
from PySide6.QtWidgets import QWidget

from bblauncher_qmlview import (
    PatchModel,
    RecordModel,
    ShButton,
    ShCheck,
    ShCombo,
    ShEdit,
    ShKeybind,
    ShLabel,
    ShLog,
    ShSlider,
    ShStack,
    Shim,
    export_shims,
)
from bblauncher_Utils import (
    AA_CHOICES,
    ANISO_CHOICES,
    APP_DIR,
    ASYNC_PIPELINE_CHOICES,
    CONTENT_SKU_CHOICES,
    DEBUG_VIEW_CHOICES,
    DEFAULT_GAME,
    DLSS_PRESET_CHOICES,
    EFFECT_CHOICES,
    FPS_LIVE_CHOICES,
    FPS_XML_CHOICES,
    FSR3_TUNING_PRESETS,
    GAMEPLAY_KEYBINDS,
    HUD_QUADRANT_CHOICES,
    LANGUAGE_CHOICES,
    LAUNCHER_VERSION,
    LIVE_FPS_CONFLICTING_PATCHES,
    LIVE_RES_CHOICES,
    MENU_LANGUAGE_CHOICES,
    MODEL_LOD_CHOICES,
    PREP_WORKER_CHOICES,
    PRESET_CHOICES,
    RES_CHOICES,
    SYSTEM_KEYBINDS,
    TEXTURE_QUALITY_CHOICES,
    UPSCALER_CHOICES,
    WINDOW_MODE_CHOICES,
    build_stamp,
    connected_gpus,
    load_xml_patches,
    scancode_display_name,
    win_scan_to_scancode_name,
)


class WheelGuard(QObject):
    """Kept for controller compatibility; QML handles wheel routing itself."""

    def eventFilter(self, obj, event):  # noqa: N802
        return False


def _qt_key_to_name(key: int) -> str:
    """Translate a Qt key code into the storage name the engine expects."""
    K = Qt.Key
    key_map = {
        int(K.Key_Space): "Space",
        int(K.Key_Return): "Return",
        int(K.Key_Enter): "Return",
        int(K.Key_Tab): "Tab",
        int(K.Key_Backspace): "Backspace",
        int(K.Key_Left): "Left",
        int(K.Key_Right): "Right",
        int(K.Key_Up): "Up",
        int(K.Key_Down): "Down",
        int(K.Key_PageUp): "PageUp",
        int(K.Key_PageDown): "PageDown",
        int(K.Key_Home): "Home",
        int(K.Key_End): "End",
        int(K.Key_Insert): "Insert",
        int(K.Key_CapsLock): "CapsLock",
        int(K.Key_NumLock): "Numlock",
        int(K.Key_Print): "PrintScreen",
        int(K.Key_ScrollLock): "ScrollLock",
        int(K.Key_Pause): "Pause",
        int(K.Key_Alt): "Left Alt",
        int(K.Key_Control): "Left Ctrl",
    }
    if key in key_map:
        return key_map[key]
    if int(K.Key_F1) <= key <= int(K.Key_F24):
        return f"F{key - int(K.Key_F1) + 1}"
    if int(K.Key_A) <= key <= int(K.Key_Z):
        return chr(key)
    if int(K.Key_0) <= key <= int(K.Key_9):
        return chr(key)
    return "Unknown"


# --------------------------------------------------------------------------- #
#  base view
# --------------------------------------------------------------------------- #

class LauncherView(QWidget):
    """Owns the shim graph and the QML-facing API.

    Inheriting QWidget keeps the controller usable as a dialog parent and
    gives it the window API calls it already makes.  The widget itself is
    never shown — Qt Quick owns the visible window.
    """

    currentTabChanged = Signal()
    toast = Signal(str)
    closeRequested = Signal()

    def __init__(self, parent: QWidget | None = None):
        super().__init__(parent)
        self._shims: dict[str, QObject] = {}
        self._current_tab = 0
        self._patch_model = PatchModel(self)
        self._characters_model = RecordModel(self)
        self._backups_model = RecordModel(self)
        self.patch_warnings: dict[str, ShLabel] = {}
        self.patch_checkboxes: dict[str, ShCheck] = {}
        self.home_chips: dict[str, ShButton] = {}
        self.keybind_buttons: dict[str, ShKeybind] = {}
        self.effect_checks: dict[str, ShCheck] = {}
        self.effect_shims: list = []
        self.gameplay_keybind_rows: list = []
        self.system_keybind_rows: list = []
        self.stack = None
        self.patches_placeholder = None

    # -- registration -------------------------------------------------------
    def _expose(self, shim: QObject) -> QObject:
        name = getattr(shim, "shim_name", "") or f"shim_{len(self._shims)}"
        self._shims[name] = shim
        return shim

    def _label(self, text: str, name: str, tone: str = "text") -> ShLabel:
        shim = ShLabel(text, name)
        shim._set_tone(tone)
        return self._expose(shim)  # type: ignore[return-value]

    def _button(self, text: str, name: str) -> ShButton:
        return self._expose(ShButton(text, name))  # type: ignore[return-value]

    def _check(self, text: str, name: str) -> ShCheck:
        return self._expose(ShCheck(text, name))  # type: ignore[return-value]

    def _combo(self, name: str) -> ShCombo:
        return self._expose(ShCombo(name))  # type: ignore[return-value]

    def _edit(self, text: str, name: str) -> ShEdit:
        return self._expose(ShEdit(text, name))  # type: ignore[return-value]

    def _slider(self, name: str) -> ShSlider:
        return self._expose(ShSlider(name))  # type: ignore[return-value]

    def _group(self, name: str) -> Shim:
        return self._expose(Shim(name))  # type: ignore[return-value]

    # -- QML properties -----------------------------------------------------
    def _get_current_tab(self) -> int:
        return self._current_tab

    def _set_current_tab(self, value: int) -> None:
        value = max(0, min(5, int(value)))
        if value != self._current_tab:
            self._current_tab = value
            self.currentTabChanged.emit()

    currentTab = Property(int, _get_current_tab, _set_current_tab,
                          notify=currentTabChanged)

    @property
    def patchModel(self) -> PatchModel:  # noqa: N802
        return self._patch_model

    @property
    def charactersModel(self) -> RecordModel:  # noqa: N802
        return self._characters_model

    @property
    def backupsModel(self) -> RecordModel:  # noqa: N802
        return self._backups_model

    # -- engine -------------------------------------------------------------
    def expose(self, engine) -> None:
        export_shims(engine, self._shims, {
            "backend": self,
            "patchModel": self._patch_model,
            "charactersModel": self._characters_model,
            "backupsModel": self._backups_model,
            "effectShims": self.effect_shims,
            "gameplayKeybindRows": self.gameplay_keybind_rows,
            "systemKeybindRows": self.system_keybind_rows,
        })

    # ------------------------------------------------------------------ init
    def init_ui(self) -> None:
        self.stack = self._expose(ShStack("stack"))
        self.patches_placeholder = self._expose(Shim("patches_placeholder"))
        self.patch_model = self._patch_model

        self._build_home()
        self._build_patches()

        # Build the remaining pages eagerly.  The widgets used to be created on
        # first tab activation; with Qt Quick the pages have to be able to bind
        # to their shims as soon as they are lazily loaded, so the graph must be
        # complete before the engine is loaded.  Creating shims is cheap.
        self._display_built = True
        self.page_display = self.create_display_tab()
        self._mk_built = True
        self.page_mk = self.create_mk_tab()
        self._advanced_built = True
        self.page_advanced = self.create_advanced_tab()

        # The save manager rebuild owns models instead of a widget tree, so it
        # must be redirected before anything connects to it.
        self.refresh_save_manager = self.refresh_save_manager_view
        self._saves_built = True
        self.page_saves = self.create_saves_tab()

        self.lbl_version.setText(self._version_text())
        QTimer.singleShot(140, self.refresh_save_manager)

    @staticmethod
    def _version_text() -> str:
        build = build_stamp()
        return f"v{LAUNCHER_VERSION}" + (f" \u00b7 build {build}" if build else "")

    # ------------------------------------------------------------------ home
    def _build_home(self) -> None:
        self.lbl_status = self._label("\u25CF Engine Idle", "lbl_status", "muted")
        self.lbl_run_clock = self._label("", "lbl_run_clock", "muted")
        self.lbl_run_clock.setVisible(False)
        self.lbl_version = self._label("", "lbl_version", "dim")

        self.lbl_path_status = self._label("\u2022", "lbl_path_status", "dim")
        init_eboot = self.settings.get("eboot", "")
        if not init_eboot or not Path(init_eboot).exists():
            if (DEFAULT_GAME / "eboot.bin").exists():
                init_eboot = str(DEFAULT_GAME / "eboot.bin")
            elif (DEFAULT_GAME / "eboot.elf").exists():
                init_eboot = str(DEFAULT_GAME / "eboot.elf")
            else:
                init_eboot = ""
        self.txt_eboot = self._edit(init_eboot, "txt_eboot")
        self.txt_eboot.setPlaceholderText(
            "Path to decrypted eboot.bin, eboot.elf or game folder..."
        )
        self.txt_eboot.setToolTip("Drag and drop eboot.bin, eboot.elf or the game folder here.")
        self.txt_eboot.textChanged.connect(self.on_eboot_path_changed)
        self.txt_eboot.pathDropped.connect(self.on_eboot_path_dropped)

        self.lbl_path_hint = self._label("", "lbl_path_hint", "muted")

        self.lbl_setup_dot = self._label("\u25CF", "lbl_setup_dot", "dim")
        self.lbl_setup_summary = self._label("Checking setup...", "lbl_setup_summary", "text")
        self.btn_setup_details = self._button("Details", "btn_setup_details")
        self.btn_setup_details.setVisible(False)
        self.btn_setup_details.clicked.connect(self.show_setup_details)
        self.btn_setup_recheck = self._button("Re-check", "btn_setup_recheck")
        self.btn_setup_recheck.clicked.connect(
            lambda: self.refresh_setup_status(force_refresh=True)
        )

        self.lbl_check_game = self._label("\u00B7 Game files", "lbl_check_game", "muted")
        self.lbl_check_vulkan = self._label("\u00B7 Vulkan / GPU", "lbl_check_vulkan", "muted")
        self.lbl_check_avx = self._label("\u00B7 AVX", "lbl_check_avx", "muted")
        self.lbl_check_fps = self._label("\u00B7 FPS patch", "lbl_check_fps", "muted")

        self.btn_launch = self._button("\u25B6  LAUNCH BLOODBORNE", "btn_launch")
        self.btn_launch.setEnabled(False)
        self.btn_launch.clicked.connect(self.launch_game)

        self.btn_stop = self._button("\u25A0  Stop", "btn_stop")
        self.btn_stop.setEnabled(False)
        self.btn_stop.clicked.connect(self.stop_game)

        self.lbl_run_info = self._label("\u25CF Select a game path", "lbl_run_info", "muted")
        self.lbl_last_run = self._label("No launch recorded yet.", "lbl_last_run", "muted")

        chip_specs = [
            ("fps", 2, "Target framerate - open Display & Graphics"),
            ("res", 2, "Render resolution - open Display & Graphics"),
            ("window", 2, "Window mode - open Display & Graphics"),
            ("upscaler", 2, "Upscaler - open Display & Graphics"),
            ("mk", 3, "Mouse & Keyboard mode"),
            ("patches", 1, "Enabled patches"),
            ("mods", 5, "Mods and advanced settings"),
        ]
        for key, tab, tip in chip_specs:
            chip = ShButton("\u2014", f"chip_{key}")
            chip.setToolTip(tip)
            chip.clicked.connect(lambda _=False, t=tab: self.switch_tab(t))
            self._expose(chip)
            self.home_chips[key] = chip

        self.cmb_log_filter = self._combo("cmb_log_filter")
        for text, value in (
            ("All output", "all"),
            ("Hide Vulkan warnings", "hide_vk"),
            ("Errors & warnings", "errors"),
        ):
            self.cmb_log_filter.addItem(text, value)
        current_filter = self.settings.get("log_filter", "")
        if current_filter not in ("all", "hide_vk", "errors"):
            current_filter = "hide_vk" if self.settings.get("hide_vk", True) else "all"
        index = self.cmb_log_filter.findData(current_filter)
        self.cmb_log_filter.setCurrentIndex(index if index >= 0 else 0)
        self._log_filter = current_filter
        self.cmb_log_filter.currentIndexChanged.connect(self.on_log_filter_changed)

        self.chk_autoscroll = self._check("Auto-scroll", "chk_autoscroll")
        self.chk_autoscroll.setChecked(bool(self.settings.get("log_autoscroll", True)))
        self.chk_autoscroll.stateChanged.connect(self.on_log_autoscroll_changed)

        self.log_view = self._expose(ShLog("log_view"))
        self.log_view.setMaximumBlockCount(5000)

    # --------------------------------------------------------------- patches
    def _build_patches(self) -> None:
        self.txt_patch_search = self._edit("", "txt_patch_search")
        self.txt_patch_search.setPlaceholderText("Filter patches by keyword or author...")
        self.txt_patch_search.textChanged.connect(lambda _t: self.filter_patches())

        self.cmb_patch_cat = self._combo("cmb_patch_cat")
        self.cmb_patch_cat.addItems([
            "All Categories",
            "Performance & Fixes",
            "Visuals & Camera",
            "Gameplay & Cheats",
            "Framerate & Engine",
            "Resolutions & Grids",
            "Debug & Tools",
        ])
        self.cmb_patch_cat.currentTextChanged.connect(lambda _t: self.filter_patches())

        self.btn_recommended = self._button("Recommended Defaults", "btn_recommended")
        self.btn_recommended.clicked.connect(self.apply_recommended_patches)

        self.lbl_patch_count = self._label(
            f"{len(self.enabled_patches)} / {len(self.all_xml_patches)} enabled",
            "lbl_patch_count",
            "gold",
        )

        self._refresh_patch_rows()

    def create_patches_tab(self):
        """Stack bookkeeping hook used by the controller's lazy builder."""
        self._refresh_patch_rows()
        return self.patches_placeholder

    def _refresh_patch_rows(self) -> None:
        """(Re)create row shims + model from the currently loaded patch list."""
        if not hasattr(self, "txt_patch_search"):
            return

        patches = self.all_xml_patches or load_xml_patches()
        self.all_xml_patches = patches

        live = hasattr(self, "chk_fps_patch") and self.chk_fps_patch.isChecked()
        self.patch_checkboxes = {}
        self.patch_warnings = {}

        for patch in patches:
            name = patch["name"]
            check = ShCheck(name, name)
            warn = ShLabel("(Disabled because Live FPS is On)", f"warn_{name}")
            warn._set_tone("warn")
            warn.setVisible(False)
            self.patch_warnings[name] = warn
            self.patch_checkboxes[name] = check

            if live and name in LIVE_FPS_CONFLICTING_PATCHES:
                check.setChecked(False)
                check.setEnabled(False)
                check.setToolTip("Disabled: Conflicts with Live FPS Patching.")
                warn.setVisible(True)
            else:
                check.setChecked(name in self.enabled_patches)

            check.stateChanged.connect(
                lambda state, p=name: self.on_patch_toggled(p, state)
            )

        self._patch_model.set_patches(patches, self.patch_checkboxes, self.patch_warnings)
        if hasattr(self, "lbl_patch_count"):
            self.lbl_patch_count.setText(
                f"{len(self.enabled_patches)} / {len(self.all_xml_patches)} enabled"
            )

    # ------------------------------------------------------------------
    #  shared helpers for the settings pages
    # ------------------------------------------------------------------
    def _make_slider(self, name: str, label_name: str, minimum: int, maximum: int,
                     value: int, scale: int = 100, decimals: int = 2,
                     signed: bool = False):
        slider = self._slider(name)
        slider.setRange(minimum, maximum)
        slider.setValue(max(minimum, min(maximum, value)))
        label = self._label("", label_name, "text")
        fmt = (lambda v: f"{v / scale:+.{decimals}f}" if signed
               else f"{v / scale:.{decimals}f}")
        label.setText(fmt(slider.value()))
        slider.valueChanged.connect(lambda v, l=label, f=fmt: l.setText(f(v)))
        slider.sliderReleased.connect(self.save_bbport_ini)
        return slider, label

    def _populate_fps_combo(self, live: bool, selected: str | None = None) -> None:
        """Fill the framerate combo for the live/XML mode, keeping the selection."""
        if not hasattr(self, "cmb_fps"):
            return
        current = selected if selected is not None else (self.cmb_fps.currentData() or "60")
        choices = FPS_LIVE_CHOICES if live else FPS_XML_CHOICES
        self.cmb_fps.blockSignals(True)
        self.cmb_fps.clear()
        for label, value in choices:
            self.cmb_fps.addItem(label, value)
        index = self.cmb_fps.findData(current)
        if index < 0:
            index = self.cmb_fps.findData("60")
        self.cmb_fps.setCurrentIndex(index if index >= 0 else 0)
        self.cmb_fps.blockSignals(False)

    def _on_fps_patch_toggled(self, checked: bool) -> None:
        self._populate_fps_combo(checked)
        if hasattr(self, "_sync_patches_to_live_fps"):
            self._sync_patches_to_live_fps(checked)
        self.save_settings()
        self.save_bbport_ini()

    def fill_gpu_choices(self, refresh: bool = False) -> None:
        if not hasattr(self, "cmb_gpu"):
            return
        self.cmb_gpu.blockSignals(True)
        self.cmb_gpu.clear()
        gpus = connected_gpus(refresh=refresh)
        best_name = next((name for _, _, _, name, best in gpus if best), "")
        auto_label = f"Auto (Default: {best_name})" if best_name else "Auto (Default)"
        self.cmb_gpu.addItem(auto_label, "")
        for idx, vendor, kind, name, best in gpus:
            note = f" ({kind})" if kind in ("integrated", "cpu") else ""
            self.cmb_gpu.addItem(f"{name}{note}", name)
        cur_gpu = self.settings.get("gpu", "")
        if cur_gpu:
            idx = self.cmb_gpu.findData(cur_gpu)
            if idx >= 0:
                self.cmb_gpu.setCurrentIndex(idx)
            else:
                self.cmb_gpu.addItem(f"{cur_gpu} (Not detected)", cur_gpu)
                self.cmb_gpu.setCurrentIndex(self.cmb_gpu.count() - 1)
        else:
            self.cmb_gpu.setCurrentIndex(0)
        self.cmb_gpu.blockSignals(False)

    # ------------------------------------------------------------------
    #  DISPLAY & GRAPHICS
    # ------------------------------------------------------------------
    def create_display_tab(self) -> Shim:
        page = self._group("page_display")
        ini_opts = self.load_bbport_ini()

        # ---- Display & render settings ----
        self.cmb_fps = self._combo("cmb_fps")

        self.cmb_res = self._combo("cmb_res")
        self.cmb_res.addItems(RES_CHOICES)
        self.cmb_res.setCurrentText(self.settings.get("res", RES_CHOICES[0]))
        self.cmb_res.currentIndexChanged.connect(
            lambda: (self.save_settings(), self.save_bbport_ini()))

        self.cmb_win_mode = self._combo("cmb_win_mode")
        self.cmb_win_mode.addItems(WINDOW_MODE_CHOICES)
        self.cmb_win_mode.setCurrentText(self.settings.get("window_mode", "Borderless"))
        self.cmb_win_mode.currentIndexChanged.connect(
            lambda: (self.save_settings(), self.save_bbport_ini()))

        self.cmb_aniso = self._combo("cmb_aniso")
        for label, _, _ in ANISO_CHOICES:
            self.cmb_aniso.addItem(label)
        self.cmb_aniso.setCurrentText(self.settings.get("aniso", "2x"))
        self.cmb_aniso.currentIndexChanged.connect(
            lambda: (self.save_settings(), self.save_bbport_ini()))

        self.cmb_aa = self._combo("cmb_aa")
        self.cmb_aa.addItems(AA_CHOICES)
        self.cmb_aa.setCurrentText(self.settings.get("anti_aliasing", AA_CHOICES[0]))
        self.cmb_aa.currentIndexChanged.connect(
            lambda: (self.save_settings(), self.save_bbport_ini()))

        self.cmb_prep_workers = self._combo("cmb_prep_workers")
        for label, _ in PREP_WORKER_CHOICES:
            self.cmb_prep_workers.addItem(label)
        self.cmb_prep_workers.setCurrentText(
            self.settings.get("prep_workers", PREP_WORKER_CHOICES[0][0]))
        self.cmb_prep_workers.currentIndexChanged.connect(
            lambda: (self.save_settings(), self.save_bbport_ini()))

        self.cmb_gpu = self._combo("cmb_gpu")
        self.fill_gpu_choices()
        self.cmb_gpu.currentIndexChanged.connect(
            lambda: (self.save_settings(), self.save_bbport_ini()))

        self.cmb_live_res = self._combo("cmb_live_res")
        for label, value in LIVE_RES_CHOICES:
            self.cmb_live_res.addItem(label, value)
        live_idx = self.cmb_live_res.findData(ini_opts.get("live_resolution", "0").strip().lower())
        self.cmb_live_res.setCurrentIndex(live_idx if live_idx >= 0 else 0)
        self.cmb_live_res.currentIndexChanged.connect(lambda: self.save_bbport_ini())

        self.cmb_async_pipelines = self._combo("cmb_async_pipelines")
        for label, value in ASYNC_PIPELINE_CHOICES:
            self.cmb_async_pipelines.addItem(label, value)
        cur_async = str(ini_opts.get(
            "async_pipelines", self.settings.get("async_pipelines", "auto"))).strip().lower()
        async_idx = self.cmb_async_pipelines.findData(cur_async)
        self.cmb_async_pipelines.setCurrentIndex(async_idx if async_idx >= 0 else 0)
        self.cmb_async_pipelines.currentIndexChanged.connect(
            lambda: (self.save_settings(), self.save_bbport_ini()))

        self.cmb_lang = self._combo("cmb_lang")
        for label, code in LANGUAGE_CHOICES:
            self.cmb_lang.addItem(label, code)
        cur_lang = self.settings.get("language", "1")
        lang_idx = next((i for i, (_, c) in enumerate(LANGUAGE_CHOICES) if c == cur_lang), 0)
        self.cmb_lang.setCurrentIndex(lang_idx)
        self.cmb_lang.currentIndexChanged.connect(
            lambda: (self.save_settings(), self.save_bbport_ini()))

        self.cmb_texture_quality = self._combo("cmb_texture_quality")
        for label, value in TEXTURE_QUALITY_CHOICES:
            self.cmb_texture_quality.addItem(label, value)
        cur_tex = str(ini_opts.get(
            "texture_quality", self.settings.get("texture_quality", "auto"))).strip().lower()
        tex_idx = self.cmb_texture_quality.findData(cur_tex)
        self.cmb_texture_quality.setCurrentIndex(tex_idx if tex_idx >= 0 else 0)
        self.cmb_texture_quality.currentIndexChanged.connect(
            lambda: (self.save_settings(), self.save_bbport_ini()))

        self.chk_fps_patch = self._check(
            "Enable Live FPS Patching (60/90/120/Uncap)", "chk_fps_patch")
        self.chk_fps_patch.setChecked(self.settings.get("feat_fps_patch", True))
        self._populate_fps_combo(self.chk_fps_patch.isChecked(), self.settings.get("fps", "60"))
        self.chk_fps_patch.toggled.connect(self._on_fps_patch_toggled)
        self.cmb_fps.currentIndexChanged.connect(
            lambda: (self.save_settings(), self.save_bbport_ini()))

        # ---- Upscaler settings ----
        self.cmb_upscaler = self._combo("cmb_upscaler")
        for label, value in UPSCALER_CHOICES:
            self.cmb_upscaler.addItem(label, value)
        ini_up = ini_opts.get("upscaler", "").strip().lower()
        ini_up_norm = "fsr31" if ini_up == "fsr3" else ini_up
        upscaler_idx = self.cmb_upscaler.findData(ini_up_norm) if ini_up_norm else -1
        if upscaler_idx < 0:
            init_upscaler = self.settings.get("upscaler_model", "Off")
            if not self.settings.get("feat_upscaler", False) and init_upscaler != "Off":
                init_upscaler = "Off"
            upscaler_idx = self.cmb_upscaler.findText(init_upscaler)
        if upscaler_idx >= 0:
            self.cmb_upscaler.setCurrentIndex(upscaler_idx)
        self.cmb_upscaler.currentIndexChanged.connect(
            lambda: (self._update_upscaler_option_visibility(),
                     self.save_settings(), self.save_bbport_ini()))

        self.cmb_preset = self._combo("cmb_preset")
        for label, value in PRESET_CHOICES:
            self.cmb_preset.addItem(label, value)
        preset_idx = self.cmb_preset.findData(ini_opts.get("preset", "1").strip())
        self.cmb_preset.setCurrentIndex(preset_idx if preset_idx >= 0 else 0)
        self.cmb_preset.currentIndexChanged.connect(
            lambda: (self._update_upscaler_option_visibility(), self.save_bbport_ini()))

        self.grp_custom_scale = self._group("grp_custom_scale")
        self.sl_render_scale, self.lbl_render_scale_val = self._make_slider(
            "sl_render_scale", "lbl_render_scale_val",
            100, 300, self._ini_percent(ini_opts, "render_scale", 1.30, 100, 300))
        self.sl_render_scale.setToolTip(
            "Output / render ratio for Custom preset (1.00 = Native AA, "
            "1.30 = between Native and Quality, 3.00 = Ultra Performance).")

        self.chk_sharpen = self._check("Enable Sharpening", "chk_sharpen")
        self.chk_sharpen.setChecked(ini_opts.get("sharpen", "1") != "0")
        self.chk_sharpen.stateChanged.connect(lambda: self.save_bbport_ini())

        self.chk_jitter = self._check("Jitter", "chk_jitter")
        self.chk_jitter.setChecked(ini_opts.get("jitter", "1") != "0")
        self.chk_jitter.stateChanged.connect(lambda: self.save_bbport_ini())

        self.chk_reactive = self._check("Reactive Mask", "chk_reactive")
        self.chk_reactive.setChecked(ini_opts.get("reactive", "0") != "0")
        self.chk_reactive.stateChanged.connect(lambda: self.save_bbport_ini())

        self.chk_object_motion = self._check("Object Motion Vectors", "chk_object_motion")
        self.chk_object_motion.setChecked(ini_opts.get("object_motion", "0") != "0")
        self.chk_object_motion.stateChanged.connect(lambda: self.save_bbport_ini())

        self.chk_fsr4_auto_exposure = self._check("FSR 4 Auto Exposure", "chk_fsr4_auto_exposure")
        self.chk_fsr4_auto_exposure.setChecked(ini_opts.get("fsr4_auto_exposure", "0") != "0")
        self.chk_fsr4_auto_exposure.stateChanged.connect(lambda: self.save_bbport_ini())

        self.chk_fsr4_invert_jitter = self._check("FSR 4 Invert Jitter", "chk_fsr4_invert_jitter")
        self.chk_fsr4_invert_jitter.setChecked(ini_opts.get("fsr4_invert_jitter", "0") != "0")
        self.chk_fsr4_invert_jitter.stateChanged.connect(lambda: self.save_bbport_ini())

        self.sl_sharpness, self.lbl_sharpness_val = self._make_slider(
            "sl_sharpness", "lbl_sharpness_val",
            0, 200, self._ini_percent(ini_opts, "sharpness", 0.30, 0, 200))
        self.sl_mip_bias, self.lbl_mip_bias_val = self._make_slider(
            "sl_mip_bias", "lbl_mip_bias_val",
            -200, 100, self._ini_percent(ini_opts, "mip_bias", 0.00, -200, 100), signed=True)
        self.sl_mip_bias.setToolTip(
            "Added to the texture mip bias set by the upscaler: below 0 is sharper "
            "(more shimmer), above 0 is softer.")
        self.sl_reactive_scale, self.lbl_reactive_scale_val = self._make_slider(
            "sl_reactive_scale", "lbl_reactive_scale_val",
            0, 1600, self._ini_percent(ini_opts, "reactive_scale", 1.00, 0, 1600))
        self.sl_reactive_threshold, self.lbl_reactive_threshold_val = self._make_slider(
            "sl_reactive_threshold", "lbl_reactive_threshold_val",
            0, 100, self._ini_percent(ini_opts, "reactive_threshold", 0.20, 0, 100))
        self.sl_reactive_max, self.lbl_reactive_max_val = self._make_slider(
            "sl_reactive_max", "lbl_reactive_max_val",
            0, 100, self._ini_percent(ini_opts, "reactive_max", 0.90, 0, 100))

        # ---- DLSS options ----
        self.grp_dlss = self._group("grp_dlss")
        self.cmb_dlss_preset = self._combo("cmb_dlss_preset")
        for label, value in DLSS_PRESET_CHOICES:
            self.cmb_dlss_preset.addItem(label, value)
        self._set_dlss_preset_combo(ini_opts.get("dlss_preset", "k"))
        self.cmb_dlss_preset.currentIndexChanged.connect(lambda: self.save_bbport_ini())

        self.chk_dlss_auto_exposure = self._check("DLSS: Auto Exposure", "chk_dlss_auto_exposure")
        self.chk_dlss_auto_exposure.setChecked(self._ini_bool(ini_opts, "dlss_auto_exposure", False))
        self.chk_dlss_auto_exposure.stateChanged.connect(lambda: self.save_bbport_ini())

        self.chk_dlss_mip_bias = self._check(
            "DLSS: Sharper Textures (mip bias -1)", "chk_dlss_mip_bias")
        self.chk_dlss_mip_bias.setChecked(self._ini_bool(ini_opts, "dlss_mip_bias", True))
        self.chk_dlss_mip_bias.stateChanged.connect(lambda: self.save_bbport_ini())

        self.chk_dlss_jitter_fade = self._check(
            "DLSS: Less Wobble While Turning (experimental)", "chk_dlss_jitter_fade")
        self.chk_dlss_jitter_fade.setChecked(self._ini_bool(ini_opts, "dlss_jitter_fade", False))
        self.chk_dlss_jitter_fade.stateChanged.connect(lambda: self.save_bbport_ini())

        self.chk_upscale_before_post = self._check(
            "Upscale Before Post-Processing (experimental)", "chk_upscale_before_post")
        self.chk_upscale_before_post.setToolTip(
            "Outputs other than 1080p: upscaler takes the HDR scene before "
            "post-processing (applies on restart when enabled via BB_UPSCALE_EARLY=1).")
        self.chk_upscale_before_post.setChecked(
            self._ini_bool(ini_opts, "upscale_before_post", False))
        self.chk_upscale_before_post.stateChanged.connect(lambda: self.save_bbport_ini())

        # ---- FSR 3.1 tuning ----
        self.grp_fsr3 = self._group("grp_fsr3")
        self.cmb_fsr3_preset = self._combo("cmb_fsr3_preset")
        for label, *_vals in FSR3_TUNING_PRESETS:
            self.cmb_fsr3_preset.addItem(label, label)
        self.cmb_fsr3_preset.addItem("Custom", "__custom__")
        self.cmb_fsr3_preset.currentIndexChanged.connect(self._on_fsr3_preset_selected)

        self.sl_fsr3_velocity_factor, self.lbl_fsr3_velocity_factor_val = self._make_slider(
            "sl_fsr3_velocity_factor", "lbl_fsr3_velocity_factor_val",
            0, 1000, self._ini_scaled(ini_opts, "fsr3_velocity_factor", 1.000, 0, 1000, 1000),
            scale=1000, decimals=3)
        self.sl_fsr3_reactiveness_scale, self.lbl_fsr3_reactiveness_scale_val = self._make_slider(
            "sl_fsr3_reactiveness_scale", "lbl_fsr3_reactiveness_scale_val",
            0, 4000, self._ini_scaled(ini_opts, "fsr3_reactiveness_scale", 1.000, 0, 4000, 1000),
            scale=1000, decimals=3)
        self.sl_fsr3_shading_change_scale, self.lbl_fsr3_shading_change_scale_val = self._make_slider(
            "sl_fsr3_shading_change_scale", "lbl_fsr3_shading_change_scale_val",
            0, 4000, self._ini_scaled(ini_opts, "fsr3_shading_change_scale", 1.000, 0, 4000, 1000),
            scale=1000, decimals=3)
        self.sl_fsr3_accumulation, self.lbl_fsr3_accumulation_val = self._make_slider(
            "sl_fsr3_accumulation", "lbl_fsr3_accumulation_val",
            0, 1000, self._ini_scaled(ini_opts, "fsr3_accumulation", 0.333, 0, 1000, 1000),
            scale=1000, decimals=3)
        self.sl_fsr3_min_disocclusion, self.lbl_fsr3_min_disocclusion_val = self._make_slider(
            "sl_fsr3_min_disocclusion", "lbl_fsr3_min_disocclusion_val",
            -1000, 1000,
            self._ini_scaled(ini_opts, "fsr3_min_disocclusion", -0.333, -1000, 1000, 1000),
            scale=1000, decimals=3, signed=True)
        for _sl in (
            self.sl_fsr3_velocity_factor,
            self.sl_fsr3_reactiveness_scale,
            self.sl_fsr3_shading_change_scale,
            self.sl_fsr3_accumulation,
            self.sl_fsr3_min_disocclusion,
        ):
            _sl.valueChanged.connect(lambda _v: self._refresh_fsr3_preset_combo())

        self._refresh_fsr3_preset_combo()
        self._update_upscaler_option_visibility()

        # ---- HUD & diagnostics ----
        self.chk_show_fps = self._check("Show FPS Counter", "chk_show_fps")
        self.chk_show_fps.setChecked(ini_opts.get("show_fps", "0") != "0")
        self.chk_show_fps.stateChanged.connect(lambda: self.save_bbport_ini())

        self.chk_hud = self._check("Show HUD Frame Diagnostics", "chk_hud")
        self.chk_hud.setChecked(ini_opts.get("show_hud", "1") != "0")
        self.chk_hud.stateChanged.connect(lambda: self.save_bbport_ini())

        self.cmb_hud_quadrant = self._combo("cmb_hud_quadrant")
        for label, value in HUD_QUADRANT_CHOICES:
            self.cmb_hud_quadrant.addItem(label, value)
        hud_q_idx = self.cmb_hud_quadrant.findData(ini_opts.get("hud_quadrant", "0").strip())
        self.cmb_hud_quadrant.setCurrentIndex(hud_q_idx if hud_q_idx >= 0 else 0)
        self.cmb_hud_quadrant.currentIndexChanged.connect(lambda: self.save_bbport_ini())

        self.cmb_debug_view = self._combo("cmb_debug_view")
        for label, value in DEBUG_VIEW_CHOICES:
            self.cmb_debug_view.addItem(label, value)
        debug_idx = self.cmb_debug_view.findData(ini_opts.get("debug_view", "0").strip())
        self.cmb_debug_view.setCurrentIndex(debug_idx if debug_idx >= 0 else 0)
        self.cmb_debug_view.currentIndexChanged.connect(lambda: self.save_bbport_ini())

        self.sl_hud_opacity, self.lbl_hud_opacity_val = self._make_slider(
            "sl_hud_opacity", "lbl_hud_opacity_val",
            10, 100, int(float(ini_opts.get("hud_opacity", "0.75")) * 100))
        self.sl_hud_scale, self.lbl_hud_scale_val = self._make_slider(
            "sl_hud_scale", "lbl_hud_scale_val",
            50, 250, int(float(ini_opts.get("hud_scale", "1.00")) * 100))

        # ---- Effects & model detail ----
        tooltips = {
            "effect_ssr": "Not in the original game; costs performance.",
            "skip_intro": "Skips the intro videos when the game starts.",
            "debug_camera": "Free camera: Cross + L3 in game.",
            "debug_menu": "Needs the game's debug font files.",
        }
        self.effect_checks = {}
        self.effect_shims = []
        for key, label, default_on in EFFECT_CHOICES:
            chk = self._check(label, f"eff_{key}")
            chk.setChecked(ini_opts.get(key, "1" if default_on else "0") != "0")
            tip = tooltips.get(key)
            if tip:
                chk.setToolTip(tip)
            chk.stateChanged.connect(lambda: self.save_bbport_ini())
            self.effect_checks[key] = chk
            self.effect_shims.append(chk)

        self.cmb_model_lod = self._combo("cmb_model_lod")
        for label, value in MODEL_LOD_CHOICES:
            self.cmb_model_lod.addItem(label, value)
        lod_idx = self.cmb_model_lod.findData(ini_opts.get("model_lod", "0").strip())
        self.cmb_model_lod.setCurrentIndex(lod_idx if lod_idx >= 0 else 0)
        self.cmb_model_lod.currentIndexChanged.connect(lambda: self.save_bbport_ini())

        return page

    # ------------------------------------------------------------------
    #  MOUSE & KEYBOARD
    # ------------------------------------------------------------------
    def create_mk_tab(self) -> Shim:
        page = self._group("page_mk")
        ini_opts = self.load_bbport_ini()

        self.chk_mk_enabled = self._check("Enable Mouse & Keyboard Mode", "chk_mk_enabled")
        self.chk_mk_enabled.setChecked(ini_opts.get("mk_enabled", "1") != "0")
        self.chk_mk_enabled.stateChanged.connect(lambda: self.save_bbport_ini())

        self.sl_sens_x, self.lbl_sens_x_val = self._make_slider(
            "sl_sens_x", "lbl_sens_x_val", 10, 1000,
            int(float(ini_opts.get("mk_sens_x", "3.20")) * 100))
        self.sl_sens_y, self.lbl_sens_y_val = self._make_slider(
            "sl_sens_y", "lbl_sens_y_val", 10, 1000,
            int(float(ini_opts.get("mk_sens_y", "3.26")) * 100))
        self.sl_smoothing, self.lbl_smoothing_val = self._make_slider(
            "sl_smoothing", "lbl_smoothing_val", 0, 80,
            int(float(ini_opts.get("mk_smoothing", "0.25")) * 100))
        self.sl_responsiveness, self.lbl_responsiveness_val = self._make_slider(
            "sl_responsiveness", "lbl_responsiveness_val", 50, 250,
            int(float(ini_opts.get("mk_responsiveness", "1.00")) * 100))
        self.sl_acceleration, self.lbl_acceleration_val = self._make_slider(
            "sl_acceleration", "lbl_acceleration_val", 100, 200,
            int(float(ini_opts.get("mk_acceleration", "1.30")) * 100))

        self.chk_inv_x = self._check("Invert Horizontal Look (X)", "chk_inv_x")
        self.chk_inv_x.setChecked(ini_opts.get("mk_invert_x", "0") != "0")
        self.chk_inv_x.stateChanged.connect(lambda: self.save_bbport_ini())

        self.chk_inv_y = self._check("Invert Vertical Look (Y)", "chk_inv_y")
        self.chk_inv_y.setChecked(ini_opts.get("mk_invert_y", "0") != "0")
        self.chk_inv_y.stateChanged.connect(lambda: self.save_bbport_ini())

        self.chk_mouse_menu = self._check(
            "Mouse Pointer in Game Menus", "chk_mouse_menu")
        self.chk_mouse_menu.setToolTip(
            "Hover selects a menu row, left click chooses it, right click goes "
            "back, the wheel scrolls.")
        self.chk_mouse_menu.setChecked(ini_opts.get("mouse_menu", "1") != "0")
        self.chk_mouse_menu.stateChanged.connect(lambda: self.save_bbport_ini())

        self.keybind_buttons = {}
        self.gameplay_keybind_rows = []
        self.system_keybind_rows = []

        for ini_key, desc, def_val in GAMEPLAY_KEYBINDS:
            cur_val = ini_opts.get(ini_key, def_val)
            btn = self._expose(ShKeybind(ini_key, cur_val))
            btn.key_assigned.connect(self.on_keybind_button_assigned)
            self.keybind_buttons[ini_key] = btn
            self.gameplay_keybind_rows.append({"desc": desc, "shim": btn})

        for ini_key, desc, def_val in SYSTEM_KEYBINDS:
            cur_val = ini_opts.get(ini_key, def_val)
            btn = self._expose(ShKeybind(ini_key, cur_val, keyboard_only=True))
            btn.key_assigned.connect(self.on_keybind_button_assigned)
            self.keybind_buttons[ini_key] = btn
            self.system_keybind_rows.append({"desc": desc, "shim": btn})

        return page

    # ------------------------------------------------------------------
    #  SAVE GAME MANAGER
    # ------------------------------------------------------------------
    def create_saves_tab(self) -> Shim:
        page = self._group("page_saves")
        settings = self.settings

        self.lbl_active_save_path = self._label(
            "Checking save directory...", "lbl_active_save_path", "text")
        self.lbl_active_save_meta = self._label("-", "lbl_active_save_meta", "muted")

        self.btn_create_backup = self._button("Create Backup", "btn_create_backup")
        self.btn_create_backup.clicked.connect(self.handle_create_backup)
        self.btn_open_saves_folder = self._button("Open Folder", "btn_open_saves_folder")
        self.btn_open_saves_folder.clicked.connect(self.handle_open_saves_folder)
        self.btn_export_active = self._button("Export ZIP", "btn_export_active")
        self.btn_export_active.clicked.connect(self.handle_export_active_save)
        self.btn_import_save = self._button("Import Save", "btn_import_save")
        self.btn_import_save.clicked.connect(self.handle_import_save)

        self.lbl_no_characters = self._label(
            "No character data found (slot table uninitialized or empty).",
            "lbl_no_characters", "dim")
        self.lbl_no_characters.setVisible(False)

        self.lbl_backup_count = self._label(
            "0 Backups available", "lbl_backup_count", "muted")
        self.btn_refresh_backups = self._button("Refresh", "btn_refresh_backups")
        self.btn_refresh_backups.clicked.connect(self.refresh_save_manager)

        self.lbl_no_backups = self._label(
            "No backups saved yet. Click 'Create Backup' above or launch the "
            "game with auto-backup enabled.",
            "lbl_no_backups", "dim")

        self.chk_auto_backup_launch = self._check(
            "Auto-backup before game launch", "chk_auto_backup_launch")
        self.chk_auto_backup_launch.setChecked(
            settings.get("auto_backup_on_launch", True))
        self.chk_auto_backup_launch.toggled.connect(lambda: self.save_settings())

        self.cmb_max_backups = self._combo("cmb_max_backups")
        for count in (5, 10, 20, 50):
            self.cmb_max_backups.addItem(f"{count} pre-launch backups", count)
        cur_max = settings.get("max_rolling_backups", 10)
        idx_max = self.cmb_max_backups.findData(cur_max)
        self.cmb_max_backups.setCurrentIndex(idx_max if idx_max >= 0 else 1)
        self.cmb_max_backups.currentIndexChanged.connect(lambda: self.save_settings())

        self.chk_engine_safepoint = self._check(
            "Engine-level Safepoint Auto-backup", "chk_engine_safepoint")
        self.chk_engine_safepoint.setToolTip(
            "Instructs the C runtime to automatically snapshot save data before "
            "triggering NG+ ending sequences.")
        ini_opts = self.load_bbport_ini()
        self.chk_engine_safepoint.setChecked(
            ini_opts.get("save_auto_backup", "1").strip().lower() in ("1", "true"))
        self.chk_engine_safepoint.toggled.connect(self._sync_save_auto_backup_to_ini)

        return page

    def refresh_save_manager_view(self) -> None:
        """Model-driven replacement for the legacy widget rebuild."""
        if not getattr(self, "_saves_built", False):
            return

        from bblauncher_Utils import (
            find_character_portrait,
            get_active_save_summary,
            list_save_backups,
        )

        eboot = self.txt_eboot.text().strip() if hasattr(self, "txt_eboot") else ""
        summary = get_active_save_summary(APP_DIR, eboot_path=eboot)

        character_rows: list[dict] = []
        if summary and summary.get("found"):
            self.lbl_active_save_path.setText(f"Active Save: {summary['rel_path']}")
            self.lbl_active_save_meta.setText(
                f"Last Modified: {summary['mtime_str']}  |  "
                f"Size: {summary['size_str']}  ({summary['file_count']} files)")
            characters = summary.get("characters", [])
            save_dir = summary.get("path")
            for char in characters:
                portrait = ""
                try:
                    path = find_character_portrait(
                        slot_idx=char["slot"],
                        char_name=char["name"],
                        save_dir=save_dir,
                        base_dir=APP_DIR,
                    )
                    if path and path.is_file():
                        portrait = path.as_uri()
                except Exception:
                    portrait = ""
                character_rows.append({
                    "name": char["name"],
                    "level": char["level"],
                    "playtime": char.get("playtime_clock", char.get("playtime_str", "")),
                    "echoes": f"{char['echoes']:,}",
                    "location": char.get("location", "Hunter's Dream"),
                    "portrait": portrait,
                })
            self.lbl_no_characters.setVisible(not character_rows)
        else:
            self.lbl_active_save_path.setText("No active save found")
            self.lbl_active_save_meta.setText(
                "Game save directory user/savedata has not been initialized.")
            self.lbl_no_characters.setText(
                "Launch Bloodborne once to generate active save data.")
            self.lbl_no_characters.setVisible(True)

        self._characters_model.set_records(character_rows)

        backups = list_save_backups(APP_DIR)
        self.lbl_backup_count.setText(f"{len(backups)} Backup(s) available")

        badges = {
            "manual": ("MANUAL", "#818cf8"),
            "pre_launch": ("PRE-LAUNCH", "#34d399"),
            "pre_restore": ("SAFETY SNAPSHOT", "#fbbf24"),
            "safepoint": ("SAFEPOINT", "#38bdf8"),
            "imported": ("IMPORTED", "#e879f9"),
        }

        backup_rows: list[dict] = []
        for backup in backups:
            b_type = backup.get("backup_type", "manual")
            badge, badge_color = badges.get(b_type, ("BACKUP", "#9ca3af"))
            backup_rows.append({
                "path": backup["folder_path"],
                "label": backup.get("name") or backup.get("folder_name", "Backup"),
                "badge": badge,
                "badgeColor": badge_color,
                "timestamp": backup.get("timestamp_str", ""),
                "size": backup.get("size_str", ""),
                "characters": [
                    {
                        "name": c["name"],
                        "level": c["level"],
                        "playtime": c.get("playtime_clock", c.get("playtime_str", "")),
                        "location": c.get("location", ""),
                    }
                    for c in backup.get("characters", [])
                ],
            })

        self._backups_model.set_records(backup_rows)
        self.lbl_no_backups.setVisible(not backup_rows)

    # ------------------------------------------------------------------
    #  ADVANCED SETTINGS
    # ------------------------------------------------------------------
    def create_advanced_tab(self) -> Shim:
        page = self._group("page_advanced")
        ini_opts = self.load_bbport_ini()
        settings = self.settings

        self.cmb_menu_lang = self._combo("cmb_menu_lang")
        for label, value in MENU_LANGUAGE_CHOICES:
            self.cmb_menu_lang.addItem(label, value)
        menu_idx = self.cmb_menu_lang.findData(ini_opts.get("language", "en").strip().lower())
        self.cmb_menu_lang.setCurrentIndex(menu_idx if menu_idx >= 0 else 0)
        self.cmb_menu_lang.currentIndexChanged.connect(lambda: self.save_bbport_ini())

        self.cmb_sku = self._combo("cmb_sku")
        for label, value in CONTENT_SKU_CHOICES:
            self.cmb_sku.addItem(label, value)
        sku_idx = self.cmb_sku.findData(settings.get("sku", "full"))
        self.cmb_sku.setCurrentIndex(sku_idx if sku_idx >= 0 else 0)
        self.cmb_sku.currentIndexChanged.connect(lambda: self.save_settings())

        self.txt_timeout = self._edit(settings.get("timeout", ""), "txt_timeout")
        self.txt_timeout.setPlaceholderText("0 = run until closed")
        self.txt_timeout.editingFinished.connect(self.save_settings)

        self.txt_vblank = self._edit(settings.get("vblank_hz", ""), "txt_vblank")
        self.txt_vblank.setPlaceholderText("blank = derive from FPS (0 = uncapped)")
        self.txt_vblank.editingFinished.connect(self.save_settings)

        self.txt_frames_ahead = self._edit(
            settings.get("frames_ahead", "2"), "txt_frames_ahead")
        self.txt_frames_ahead.setPlaceholderText("engine default: 1")
        self.txt_frames_ahead.editingFinished.connect(self.save_settings)

        self.txt_dmem = self._edit(settings.get("dmem_mb", ""), "txt_dmem")
        self.txt_dmem.setPlaceholderText("blank = automatic (9152 above 1080p)")
        self.txt_dmem.editingFinished.connect(self.save_settings)

        self.txt_extra_args = self._edit(settings.get("extra_args", ""), "txt_extra_args")
        self.txt_extra_args.setPlaceholderText(
            'appended to run.bat, e.g. --some-flag "path with spaces"')
        self.txt_extra_args.editingFinished.connect(self.save_settings)

        self.chk_mods = self._check("Load mods/ Directory Assets", "chk_mods")
        self.chk_mods.setChecked(settings.get("mods", False))
        self.chk_mods.stateChanged.connect(lambda: self.save_settings())

        self.txt_mods_dir = self._edit(settings.get("mods_dir", ""), "txt_mods_dir")
        self.txt_mods_dir.setPlaceholderText("default: mods")
        self.txt_mods_dir.editingFinished.connect(self.save_settings)

        self.txt_mods_config = self._edit(
            settings.get("mods_config", ""), "txt_mods_config")
        self.txt_mods_config.setPlaceholderText("default: mods.json")
        self.txt_mods_config.editingFinished.connect(self.save_settings)

        self.txt_patches_dir = self._edit(
            settings.get("patches_dir", ""), "txt_patches_dir")
        self.txt_patches_dir.setPlaceholderText("default: patches")
        self.txt_patches_dir.editingFinished.connect(self.save_settings)

        self.txt_patches_config = self._edit(
            settings.get("patches_config", ""), "txt_patches_config")
        self.txt_patches_config.setPlaceholderText("default: patches.json")
        self.txt_patches_config.editingFinished.connect(self.save_settings)

        self.txt_user_dir = self._edit(settings.get("user_dir", ""), "txt_user_dir")
        self.txt_user_dir.setPlaceholderText("default: user")
        self.txt_user_dir.editingFinished.connect(self.save_settings)
        self.txt_user_dir.editingFinished.connect(self.update_cache_info)

        self.lbl_cache_info = self._label("Shader cache: ...", "lbl_cache_info", "muted")
        self.btn_cache = self._button("Reset Shader Cache", "btn_cache")
        self.btn_cache.clicked.connect(self.reset_shader_cache)

        # Defer the slow I/O scan so the page paints instantly.
        QTimer.singleShot(50, self.update_cache_info)
        return page

    # ---------------------------------------------------------------- slots
    @Slot(int)
    def switchTab(self, index: int) -> None:  # noqa: N802
        self.switch_tab(index)

    def switch_tab(self, index: int) -> None:
        """Mirror the legacy lazy-build + ini-sync behaviour."""
        if index == 1:
            if hasattr(self, "_build_patches_tab_now"):
                self._build_patches_tab_now()
            if hasattr(self, "_sync_external_patch_settings"):
                self._sync_external_patch_settings()
            if hasattr(self, "_sync_patches_from_ini"):
                self._sync_patches_from_ini()
        elif index == 2 and hasattr(self, "_build_display_tab_now"):
            self._build_display_tab_now()
        elif index == 3 and hasattr(self, "_build_mk_tab_now"):
            self._build_mk_tab_now()
        elif index == 4 and hasattr(self, "_build_saves_tab_now"):
            self._build_saves_tab_now()
        elif index == 5 and hasattr(self, "_build_advanced_tab_now"):
            self._build_advanced_tab_now()

        self._set_current_tab(index)
        if index == 0 and hasattr(self, "update_home_summary"):
            self.update_home_summary()

    @Slot()
    def requestClose(self) -> None:  # noqa: N802
        self.closeRequested.emit()

    @Slot()
    def launchGame(self) -> None:  # noqa: N802
        self.launch_game()

    @Slot()
    def stopGame(self) -> None:  # noqa: N802
        self.stop_game()

    @Slot()
    def browseEboot(self) -> None:  # noqa: N802
        self.browse_eboot()

    @Slot(str)
    def dropEboot(self, path: str) -> None:  # noqa: N802
        self.on_eboot_path_dropped(path)

    @Slot()
    def openGameFolder(self) -> None:  # noqa: N802
        self.open_game_folder()

    @Slot()
    def showSetupDetails(self) -> None:  # noqa: N802
        self.show_setup_details()

    @Slot()
    def recheckSetup(self) -> None:  # noqa: N802
        self.refresh_setup_status(force_refresh=True)

    @Slot()
    def copyDiagnostics(self) -> None:  # noqa: N802
        self.copy_diagnostics()

    @Slot()
    def openLogFile(self) -> None:  # noqa: N802
        self.open_log_file()

    @Slot()
    def saveLog(self) -> None:  # noqa: N802
        self.save_log()

    @Slot()
    def copyLog(self) -> None:  # noqa: N802
        self.copy_log()

    @Slot()
    def clearLog(self) -> None:  # noqa: N802
        self.clear_log()

    @Slot()
    def applyRecommendedPatches(self) -> None:  # noqa: N802
        self.apply_recommended_patches()

    # -- settings pages -----------------------------------------------------
    @Slot()
    def rescanGpus(self) -> None:  # noqa: N802
        self.fill_gpu_choices(refresh=True)
        self.save_bbport_ini()

    @Slot()
    def saveBbportIni(self) -> None:  # noqa: N802
        self.save_bbport_ini()

    @Slot()
    def saveAdvancedSettings(self) -> None:  # noqa: N802
        self.save_advanced()

    @Slot()
    def openBbportIni(self) -> None:  # noqa: N802
        self.open_bbport_ini()

    @Slot()
    def reloadBbportIni(self) -> None:  # noqa: N802
        self.reload_bbport_ini()

    @Slot()
    def resetShaderCache(self) -> None:  # noqa: N802
        self.reset_shader_cache()

    def _browse_edit(self, which: str):
        mapping = {
            "mods_dir": (self.txt_mods_dir, "dir"),
            "mods_config": (self.txt_mods_config, "file"),
            "patches_dir": (self.txt_patches_dir, "dir"),
            "patches_config": (self.txt_patches_config, "file"),
            "user_dir": (self.txt_user_dir, "dir"),
        }
        return mapping.get(which)

    @Slot(str)
    def browseAdvancedPath(self, which: str) -> None:  # noqa: N802
        entry = self._browse_edit(which)
        if entry is not None:
            self.browse_path(*entry)

    # -- keybinds -----------------------------------------------------------
    @Slot(str)
    def applyKeybindPreset(self, layout: str) -> None:  # noqa: N802
        self.apply_keybind_preset(layout)

    @Slot()
    def resetKeybinds(self) -> None:  # noqa: N802
        self.reset_keybinds_to_defaults()

    def _keybind(self, action_key: str):
        return self.keybind_buttons.get(action_key)

    def _assign_captured(self, action_key: str, stored: str, label: str) -> None:
        btn = self._keybind(action_key)
        if btn is None:
            return
        previous = btn.current_binding
        btn.stop_listening()
        btn.set_binding(stored, label)
        btn.key_assigned.emit(action_key, stored, previous)

    @Slot(str, int, int, str)
    def captureKey(self, action_key: str, key: int, scan_code: int, text: str) -> None:  # noqa: N802
        btn = self._keybind(action_key)
        if btn is None or not btn.listening:
            return
        stored = win_scan_to_scancode_name(scan_code)
        printable = (int(Qt.Key.Key_A) <= key <= int(Qt.Key.Key_Z)
                     or int(Qt.Key.Key_0) <= key <= int(Qt.Key.Key_9)
                     or (len(text) == 1 and text.isprintable()))
        if stored and printable:
            self._assign_captured(action_key, stored, scancode_display_name(stored))
            return
        name = _qt_key_to_name(key)
        if name != "Unknown":
            self._assign_captured(action_key, name, name)

    @Slot(str, int)
    def captureShift(self, action_key: str, scan_code: int) -> None:  # noqa: N802
        btn = self._keybind(action_key)
        if btn is None or not btn.listening:
            return
        name = "Right Shift" if (int(scan_code) & 0xFF) == 0x36 else "Left Shift"
        self._assign_captured(action_key, name, name)

    @Slot(str, int, bool)
    def captureMouse(self, action_key: str, button: int, shift: bool) -> None:  # noqa: N802
        btn = self._keybind(action_key)
        if btn is None or not btn.listening or btn.keyboardOnly:
            return
        MB = Qt.MouseButton
        if button == MB.LeftButton.value:
            name = "Shift + Mouse Left" if shift else "Mouse Left"
        elif button == MB.RightButton.value:
            name = "Shift + Mouse Right" if shift else "Mouse Right"
        elif button == MB.MiddleButton.value:
            name = "Mouse Middle"
        elif button == MB.BackButton.value:
            name = "Mouse X1"
        elif button == MB.ForwardButton.value:
            name = "Mouse X2"
        else:
            return
        self._assign_captured(action_key, name, name)

    @Slot(str)
    def clearKeybind(self, action_key: str) -> None:  # noqa: N802
        btn = self._keybind(action_key)
        if btn is None or not btn.listening:
            return
        previous = btn.current_binding
        btn.stop_listening()
        btn.set_binding("None", "None")
        btn.key_assigned.emit(action_key, "None", previous)

    @Slot(str)
    def cancelKeybind(self, action_key: str) -> None:  # noqa: N802
        btn = self._keybind(action_key)
        if btn is not None:
            btn.cancel()

    # -- save manager -------------------------------------------------------
    @Slot()
    def createBackup(self) -> None:  # noqa: N802
        self.handle_create_backup()

    @Slot()
    def openSavesFolder(self) -> None:  # noqa: N802
        self.handle_open_saves_folder()

    @Slot()
    def exportActiveSave(self) -> None:  # noqa: N802
        self.handle_export_active_save()

    @Slot()
    def importSave(self) -> None:  # noqa: N802
        self.handle_import_save()

    @Slot(str)
    def restoreBackup(self, path: str) -> None:  # noqa: N802
        self.handle_restore_backup(path)

    @Slot(str)
    def exportBackup(self, path: str) -> None:  # noqa: N802
        self.handle_export_backup(path)

    @Slot(str)
    def deleteBackup(self, path: str) -> None:  # noqa: N802
        self.handle_delete_backup(path)

    @Slot()
    def refreshBackups(self) -> None:  # noqa: N802
        self.refresh_save_manager()

    # -- Qt-widget surface the controller still touches ---------------------
    def setWindowTitle(self, *a, **k) -> None:      # noqa: N802
        pass

    def setWindowIcon(self, *a, **k) -> None:       # noqa: N802
        pass

    def setWindowFlags(self, *a, **k) -> None:      # noqa: N802
        pass

    def setStyleSheet(self, *a, **k) -> None:       # noqa: N802
        pass

    def setFixedSize(self, *a, **k) -> None:        # noqa: N802
        pass

    def showMinimized(self, *a, **k) -> None:       # noqa: N802
        pass

    def isActiveWindow(self) -> bool:                # noqa: N802
        return True

    def winId(self) -> int:                          # noqa: N802
        return 0

    def show(self) -> None:
        pass

    def close(self) -> None:
        self.closeRequested.emit()

    def activateWindow(self) -> None:                # noqa: N802
        pass

    def raise_(self) -> None:
        pass
