#!/usr/bin/env python3
"""Main application controller and fast-boot entry point for Bloodborne PC Launcher."""

import os
import sys
from pathlib import Path

if len(sys.argv) > 1 and any(arg.lower().endswith(".py") for arg in sys.argv[1:]):
    import runpy

    idx = next(i for i, arg in enumerate(sys.argv[1:], 1) if arg.lower().endswith(".py"))
    target_script = str(Path(sys.argv[idx]).resolve())
    sys.argv = [target_script] + sys.argv[idx + 1:]
    sys.path.insert(0, str(Path(target_script).parent))
    runpy.run_path(target_script, run_name="__main__")
    sys.exit(0)
import ctypes
import json
import os
import platform
import sys
import threading
import time
from datetime import datetime
from PySide6.QtCore import QEvent, QTimer, Qt
from PySide6.QtGui import QIcon, QTextCursor
from PySide6.QtWidgets import (
    QApplication,
    QDialog,
    QFileDialog,
    QHBoxLayout,
    QLabel,
    QMainWindow,
    QMessageBox,
    QPlainTextEdit,
    QPushButton,
    QVBoxLayout,
    QWidget,
)

from bblauncher_UI import STYLE_SHEET, LauncherUI, WheelGuard, apply_dwm_styling, install_dialog_styling
from bblauncher_Utils import (
    ANISO_CHOICES,
    APP_DIR,
    DEFAULT_GAME,
    DEFAULT_SETTINGS,
    EFFECT_CHOICES,
    KEYBIND_PRESETS,
    LAUNCHER_VERSION,
    LIVE_FPS_CONFLICTING_PATCHES,
    LOG_DIR,
    LOG_FILE,
    PREP_WORKER_CHOICES,
    RUN_BAT,
    SETTINGS_FILE,
    UPSCALER_CHOICES,
    VK_NOISE,
    ProcessWorker,
    acquire_single_instance,
    build_stamp,
    connected_gpus,
    display_name_to_scancode_label,
    get_bb_guard,
    kill_tree,
    load_xml_patches,
    resolve_game_dir,
    rotate_log,
    running_instances,
    verify_prelaunch,
)


# XML patches that mirror bbport.ini effect switches.
# The tuple is (ini key, ini value when the patch is enabled).
EFFECT_PATCH_MAP = {
    "Skip Intro": ("skip_intro", "1"),
    "Disable Motion Blur (perf increase)": ("effect_motion_blur", "0"),
    "Disable Dynamic Light Shadows (perf increase)": ("effect_dynamic_shadows", "0"),
    "Disable Chromatic Aberration": ("effect_chromatic_aberration", "0"),
    "Disable DoF": ("effect_dof", "0"),
    "Disable SSAO": ("effect_ssao", "0"),
    "Enable Screen Space Reflections (READ NOTE)": ("effect_ssr", "1"),
    "Restore Debug Camera": ("debug_camera", "1"),
    "Restore Debug Menu (READ NOTES)": ("debug_menu", "1"),
}


class BloodborneLauncher(LauncherUI, QMainWindow):
    def __init__(self):
        super().__init__()
        self.setWindowTitle("Bloodborne PC Launcher")
        self.setFixedSize(1200, 720)
        self.setStyleSheet(STYLE_SHEET)

        app = QApplication.instance()
        if app is not None:
            self._wheel_guard = WheelGuard(self)
            app.installEventFilter(self._wheel_guard)
        
        icon_file = APP_DIR / "icon.ico"
        if not icon_file.is_file():
            icon_file = Path(__file__).resolve().parent / "icon.ico"
        if icon_file.is_file():
            self.setWindowIcon(QIcon(str(icon_file)))

        self.worker = None
        self.log_fh = None
        self.hidden_count = 0
        self._log_history = []
        self._log_filter = "hide_vk"
        self._run_started = None
        self._path_valid = False
        self._setup_kind = "empty"
        self._stop_requested = False

        self._patches_built = False
        self._display_built = False
        self._mk_built = False
        self._advanced_built = False
        self._syncing_ini = False
        self.all_xml_patches = []
        self.patch_checkboxes = {}

        self.settings = self.load_settings()
        self.enabled_patches = set(self.settings.get("enabled_patches", DEFAULT_SETTINGS["enabled_patches"]))
        self._last_settings_mtime = 0
        try:
            if SETTINGS_FILE.is_file():
                self._last_settings_mtime = SETTINGS_FILE.stat().st_mtime
        except OSError:
            pass

        self._suppress_settings_save = True
        self.init_ui()
        self.log("Ready. Check setup status and press Launch Bloodborne.")

        self.run_timer = QTimer(self)
        self.run_timer.setInterval(1000)
        self.run_timer.timeout.connect(self.update_run_clock)

        self._setup_timer = QTimer(self)
        self._setup_timer.setSingleShot(True)
        self._setup_timer.setInterval(450)
        self._setup_timer.timeout.connect(self.refresh_setup_status)

        self.update_path_state()
        self.update_last_run()
        self.update_home_summary()
        QTimer.singleShot(250, self.refresh_setup_status)

        # Deferred background warming for secondary tabs after window is already shown
        QTimer.singleShot(100, self._build_display_tab_now)
        QTimer.singleShot(300, self._build_patches_tab_now)

        self._last_ini_mtime = 0
        try:
            ini_file = APP_DIR / "bbport.ini"
            if ini_file.is_file():
                self._last_ini_mtime = ini_file.stat().st_mtime
        except OSError:
            pass
        self.ini_sync_timer = QTimer(self)
        self.ini_sync_timer.setInterval(1000)
        self.ini_sync_timer.timeout.connect(self.sync_bbport_ini_to_ui)
        self.ini_sync_timer.start()
        self._suppress_settings_save = False

    def showEvent(self, event):
        super().showEvent(event)
        if not getattr(self, "_dwm_applied", False):
            self._dwm_applied = True
            apply_dwm_styling(int(self.winId()))

    def changeEvent(self, event):
        if event.type() == QEvent.ActivationChange and self.isActiveWindow():
            self.sync_bbport_ini_to_ui()
        super().changeEvent(event)

    def _build_patches_tab_now(self):
        if getattr(self, "_patches_built", False):
            return
        self._patches_built = True
        previous = getattr(self, "_suppress_settings_save", False)
        self._suppress_settings_save = True
        try:
            self.all_xml_patches = load_xml_patches()
            self._update_enabled_patches_from_ini()
            patches_widget = self.create_patches_tab()
            if hasattr(self, "patches_placeholder"):
                idx = self.stack.indexOf(self.patches_placeholder)
                if idx >= 0:
                    self.stack.removeWidget(self.patches_placeholder)
                    self.patches_placeholder.deleteLater()
                    self.stack.insertWidget(idx, patches_widget)
            self.update_patch_count_label()
        finally:
            self._suppress_settings_save = previous
        if hasattr(self, "home_chips"):
            self.update_home_summary()

    def _install_tab(self, placeholder_name: str, widget: QWidget):
        placeholder = getattr(self, placeholder_name, None)
        if placeholder is not None:
            idx = self.stack.indexOf(placeholder)
            if idx >= 0:
                self.stack.removeWidget(placeholder)
                placeholder.deleteLater()
                self.stack.insertWidget(idx, widget)
                return
        self.stack.addWidget(widget)

    def _build_display_tab_now(self):
        if getattr(self, "_display_built", False):
            return
        self._display_built = True
        previous = getattr(self, "_suppress_settings_save", False)
        self._suppress_settings_save = True
        try:
            self._install_tab("display_placeholder", self.create_display_tab())
        finally:
            self._suppress_settings_save = previous

    def _build_mk_tab_now(self):
        if getattr(self, "_mk_built", False):
            return
        self._mk_built = True
        previous = getattr(self, "_suppress_settings_save", False)
        self._suppress_settings_save = True
        try:
            self._install_tab("mk_placeholder", self.create_mk_tab())
        finally:
            self._suppress_settings_save = previous

    def _build_advanced_tab_now(self):
        if getattr(self, "_advanced_built", False):
            return
        self._advanced_built = True
        previous = getattr(self, "_suppress_settings_save", False)
        self._suppress_settings_save = True
        try:
            self._install_tab("advanced_placeholder", self.create_advanced_tab())
        finally:
            self._suppress_settings_save = previous

    def load_settings(self) -> dict:
        merged = dict(DEFAULT_SETTINGS)
        try:
            if SETTINGS_FILE.is_file():
                loaded = json.loads(SETTINGS_FILE.read_text(encoding="utf-8"))
                if isinstance(loaded, dict):
                    merged.update(loaded)
        except Exception:
            pass
        return merged

    def current_config(self) -> dict:
        cfg = dict(self.settings)
        cfg["eboot"] = self.txt_eboot.text().strip()
        try:
            log_filter = self.cmb_log_filter.currentData() or "hide_vk"
        except AttributeError:
            log_filter = self._log_filter or "hide_vk"
        self._log_filter = log_filter
        cfg["hide_vk"] = log_filter in ("hide_vk", "errors")
        cfg["log_filter"] = log_filter
        cfg["log_autoscroll"] = self.chk_autoscroll.isChecked()
        cfg["enabled_patches"] = sorted(list(self.enabled_patches))

        if self._display_built:
            win_mode = self.cmb_win_mode.currentText()
            upscaler_choice = self.cmb_upscaler.currentText()
            cfg.update({
                "fps": self.cmb_fps.currentData() or "60",
                "res": self.cmb_res.currentText(),
                "aniso": self.cmb_aniso.currentText(),
                "prep_workers": self.cmb_prep_workers.currentText(),
                "anti_aliasing": self.cmb_aa.currentText(),
                "gpu": self.cmb_gpu.currentData() or "",
                "language": self.cmb_lang.currentData() or "1",
                "feat_upscaler": self.chk_fsr.isChecked() and upscaler_choice != "Off",
                "upscaler_model": upscaler_choice,
                "feat_overlay": True,
                "feat_fps_patch": self.chk_fps_patch.isChecked(),
                "window_mode": win_mode,
                "fullscreen": win_mode == "Fullscreen",
            })

        if self._advanced_built:
            cfg.update({
                "mods": self.chk_mods.isChecked(),
                "timeout": self.txt_timeout.text().strip(),
                "vblank_hz": self.txt_vblank.text().strip(),
                "frames_ahead": self.txt_frames_ahead.text().strip(),
                "dmem_mb": self.txt_dmem.text().strip(),
                "extra_args": self.txt_extra_args.text().strip(),
                "sku": self.cmb_sku.currentData() or "full",
                "mods_dir": self.txt_mods_dir.text().strip(),
                "mods_config": self.txt_mods_config.text().strip(),
                "patches_dir": self.txt_patches_dir.text().strip(),
                "patches_config": self.txt_patches_config.text().strip(),
                "user_dir": self.txt_user_dir.text().strip(),
            })
        else:
            cfg["mods"] = bool(cfg.get("mods", False))

        return cfg

    def save_settings(self):
        if getattr(self, "_suppress_settings_save", False):
            return
        data = self.current_config()
        self.settings = data
        try:
            SETTINGS_FILE.write_text(json.dumps(data, indent=2), encoding="utf-8")
            try:
                self._last_settings_mtime = SETTINGS_FILE.stat().st_mtime
            except OSError:
                pass
        except OSError:
            pass
        if hasattr(self, "home_chips"):
            self.update_home_summary()

    # ------------------------------------------------------------------
    # Home dashboard: path validation, setup readiness, launch profile
    # ------------------------------------------------------------------
    def on_eboot_path_changed(self):
        self.update_path_state()
        if not hasattr(self, "_setup_timer"):
            return
        target = self.txt_eboot.text().strip()
        if not target:
            self._setup_timer.stop()
            self._set_setup_display("empty", "Select a game path to begin.")
            return
        if not self._path_valid:
            self._setup_timer.stop()
            self._set_setup_display(
                "error",
                "Path does not contain an eboot.bin.",
                f"Selected path:\n{target}\n\nNo eboot.bin was found in that folder.",
            )
            self._set_setup_checks("fail", "unknown", "unknown")
            return
        self._set_setup_display("checking", "Checking setup...")
        self._setup_timer.start()

    def on_eboot_path_dropped(self, path: str):
        value = str(path or "").strip().strip('"')
        if not value:
            return
        self.txt_eboot.setText(os.path.normpath(value))
        self.log(f"Selected game path: {os.path.normpath(value)}")

    def update_path_state(self):
        target = self.txt_eboot.text().strip()
        game_dir = resolve_game_dir(target) if target else None
        self._path_valid = game_dir is not None

        if game_dir is not None:
            self.lbl_path_status.setText("\u2713")
            self.lbl_path_status.setStyleSheet(
                "color: #4ade80; font-size: 14px; font-weight: 700; background: transparent;"
            )
            self.lbl_path_hint.setText(f"Game folder: {game_dir}")
            self.lbl_path_hint.setToolTip(str(game_dir))
        elif target:
            self.lbl_path_status.setText("!")
            self.lbl_path_status.setStyleSheet(
                "color: #f0b429; font-size: 14px; font-weight: 700; background: transparent;"
            )
            self.lbl_path_hint.setText("No eboot.bin found at this path. Drag eboot.bin or the game folder here.")
            self.lbl_path_hint.setToolTip(target)
        else:
            self.lbl_path_status.setText("\u2022")
            self.lbl_path_status.setStyleSheet(
                "color: #6a6a78; font-size: 14px; font-weight: 700; background: transparent;"
            )
            self.lbl_path_hint.setText("Select eboot.bin or drag the game folder here.")
            self.lbl_path_hint.setToolTip("")

        if game_dir is not None:
            self._set_setup_checks("ok", "unknown", "unknown")
        elif target:
            self._set_setup_checks("fail", "unknown", "unknown")
        else:
            self._set_setup_checks("unknown", "unknown", "unknown")

        running = self._run_started is not None or bool(self.worker and self.worker.isRunning())
        if running:
            return

        self.btn_launch.setEnabled(self._path_valid)
        if self._path_valid:
            self._setup_kind = "checking"
            self._set_run_info("checking", "\u25cf Checking setup...")
        elif target:
            self._setup_kind = "error"
            self._set_run_info("error", "\u25cf Invalid game path")
        else:
            self._setup_kind = "empty"
            self._set_run_info("empty", "\u25cf Select a game path")

    def _set_run_info(self, kind: str, text: str):
        colors = {
            "ready": "#4ade80",
            "warn": "#f0b429",
            "error": "#e65555",
            "checking": "#f0b429",
            "running": "#4ade80",
            "idle": "#6a6a78",
            "empty": "#6a6a78",
        }
        self.lbl_run_info.setText(text)
        self.lbl_run_info.setStyleSheet(
            f"color: {colors.get(kind, '#92929e')}; font-size: 12px; font-weight: 600; background: transparent;"
        )

    def _set_setup_display(self, kind: str, summary: str, details: str = ""):
        self._setup_kind = kind
        colors = {
            "ready": "#4ade80",
            "warn": "#f0b429",
            "checking": "#f0b429",
            "error": "#e65555",
            "empty": "#6a6a78",
        }
        self.lbl_setup_dot.setStyleSheet(
            f"color: {colors.get(kind, '#6a6a78')}; font-size: 13px; background: transparent;"
        )
        self.lbl_setup_summary.setText(summary)
        self.lbl_setup_summary.setToolTip(summary)
        self._setup_details_text = details
        self.btn_setup_details.setVisible(bool(details.strip()))
        self.btn_setup_details.setText("Details")

        if self._run_started is None:
            run_labels = {
                "ready": "\u25cf Ready",
                "warn": "\u25cf Ready (warnings)",
                "checking": "\u25cf Checking setup...",
                "error": "\u25cf Setup needs attention",
                "empty": "\u25cf Select a game path",
            }
            if kind in run_labels:
                self._set_run_info(kind, run_labels[kind])

    def _set_setup_checks(self, game: str, vulkan: str, avx: str, fps: str = "unknown"):
        if not hasattr(self, "lbl_check_game"):
            return
        icons = {"ok": "\u2713", "warn": "!", "fail": "\u2715", "unknown": "\u00b7"}
        colors = {
            "ok": "#4ade80",
            "warn": "#f0b429",
            "fail": "#e65555",
            "unknown": "#5a5a66",
        }
        for label, name, state in (
            (self.lbl_check_game, "Game files", game),
            (self.lbl_check_vulkan, "Vulkan / GPU", vulkan),
            (self.lbl_check_avx, "AVX", avx),
            (self.lbl_check_fps, "FPS patch", fps),
        ):
            label.setText(f"{icons.get(state, '\u00b7')} {name}")
            label.setStyleSheet(
                f"color: {colors.get(state, '#5a5a66')}; font-size: 11px; background: transparent;"
            )

    def refresh_setup_status(self):
        if hasattr(self, "_setup_timer"):
            self._setup_timer.stop()
        target = self.txt_eboot.text().strip()

        if not target:
            self._set_setup_display("empty", "Select a game path to begin.")
            self._set_setup_checks("unknown", "unknown", "unknown")
            return

        game_dir = resolve_game_dir(target)
        if not game_dir:
            self._set_setup_display(
                "error",
                "Path does not contain an eboot.bin.",
                f"Selected path:\n{target}\n\nNo eboot.bin was found in that folder.",
            )
            self._set_setup_checks("fail", "unknown", "unknown")
            return

        try:
            ready, err, details = verify_prelaunch(game_dir)
        except Exception as ex:
            ready, err, details = False, str(ex), []

        detail_lines = [str(line) for line in (details or [])]
        if err:
            detail_lines.extend(["", str(err)])
        details_text = "\n".join(detail_lines).strip()

        if ready:
            has_warnings = any(line.lstrip().upper().startswith("[WARN") for line in detail_lines)
            kind = "warn" if has_warnings else "ready"
            summary = "Ready to launch (with warnings)." if has_warnings else "Ready to launch."
        else:
            first = next((line.strip() for line in str(err or "").splitlines() if line.strip()), "")
            summary_text = first.rstrip(":").strip() or "Setup needs attention"
            summary = summary_text if summary_text.endswith((".", ")", "!")) else summary_text + "."
            kind = "error"

        self._set_setup_display(kind, summary, details_text)

        if "Game File Verification Failed" in str(err or "") or "Missing eboot.bin" in str(err or ""):
            game_state = "fail"
        elif any(("[OK] Game executable" in line or "[OK] Game Title" in line) for line in detail_lines):
            game_state = "ok"
        else:
            game_state = "fail" if not ready else "unknown"

        vk_line = next((line for line in detail_lines if "Vulkan runtime" in line), "")
        if "[OK]" in vk_line:
            vulkan_state = "ok"
        elif "[WARN]" in vk_line:
            vulkan_state = "warn"
        else:
            vulkan_state = "unknown"

        avx_line = next((line for line in detail_lines if "CPU Architecture" in line), "")
        if "[OK]" in avx_line:
            avx_state = "ok"
        elif "AVX" in str(err or "") or "Unsupported CPU" in str(err or ""):
            avx_state = "fail"
        else:
            avx_state = "unknown"

        fps_line = next((line for line in detail_lines if "FPS patch" in line), "")
        if "[OK]" in fps_line:
            fps_state = "ok"
        elif "[WARN]" in fps_line:
            fps_state = "warn"
        else:
            fps_state = "unknown"

        self._set_setup_checks(game_state, vulkan_state, avx_state, fps_state)

    def show_setup_details(self):
        details = str(getattr(self, "_setup_details_text", "") or "").strip()
        if not details:
            details = self.lbl_setup_summary.text()
        dialog = QDialog(self)
        dialog.setWindowTitle("Setup Details")
        dialog.setStyleSheet(STYLE_SHEET)
        dialog.setMinimumSize(620, 360)
        dialog.resize(680, 400)

        layout = QVBoxLayout(dialog)
        layout.setContentsMargins(18, 16, 18, 14)
        layout.setSpacing(10)

        title = QLabel(self.lbl_setup_summary.text())
        title.setObjectName("CardTitle")
        title.setWordWrap(True)
        layout.addWidget(title)

        text = QPlainTextEdit(details)
        text.setReadOnly(True)
        text.setStyleSheet(
            'background-color: #101014; color: #dcdce0; border: 1px solid #24242d; '
            'border-radius: 5px; padding: 8px; '
            'font-family: "Consolas", monospace; font-size: 11px;'
        )
        layout.addWidget(text, 1)

        row = QHBoxLayout()
        row.addStretch()
        btn_close = QPushButton("Close")
        btn_close.setObjectName("SecondaryBtn")
        btn_close.setFixedWidth(96)
        btn_close.clicked.connect(dialog.accept)
        row.addWidget(btn_close)
        layout.addLayout(row)
        dialog.exec()

    def open_game_folder(self):
        target = self.txt_eboot.text().strip()
        game_dir = resolve_game_dir(target) if target else None
        if not game_dir:
            QMessageBox.information(
                self,
                "Game Folder",
                "Select a valid eboot.bin or game folder first.",
            )
            return
        try:
            os.startfile(str(game_dir))
        except OSError as ex:
            QMessageBox.critical(self, "Game Folder", f"Could not open the game folder:\n{ex}")

    def update_home_summary(self):
        if not hasattr(self, "home_chips"):
            return

        cfg = self.current_config()
        ini = self.load_bbport_ini()

        def set_chip(key: str, text: str, tip: str):
            chip = self.home_chips.get(key)
            if chip is not None:
                chip.setText(text)
                chip.setToolTip(tip)

        fps = str(cfg.get("fps", "60"))
        fps_text = "Uncapped" if fps.lower() == "uncap" else f"{fps} FPS"
        set_chip("fps", fps_text, "Target framerate - click to open Display & Graphics")

        res = str(cfg.get("res", "")).replace(" (Native)", "").replace("x", "\u00d7")
        set_chip("res", res or "Resolution", "Render resolution - click to open Display & Graphics")

        window = str(cfg.get("window_mode", "Windowed"))
        set_chip("window", window, "Window mode - click to open Display & Graphics")

        aa = str(cfg.get("anti_aliasing", ""))
        model = str(cfg.get("upscaler_model", "Off"))
        if "TAA" in aa:
            upscaler = "TAA"
        elif not cfg.get("feat_upscaler") or model == "Off":
            upscaler = "No upscaler"
        else:
            upscaler = model
        set_chip("upscaler", upscaler, "Upscaler - click to open Display & Graphics")

        mk_value = str(ini.get("mk_enabled", "1")).strip().lower()
        mk_on = mk_value not in ("0", "", "false", "off", "no")
        set_chip("mk", "M&K On" if mk_on else "M&K Off", "Mouse & Keyboard mode")

        patches = list(cfg.get("enabled_patches", []))
        if ("Disable" in aa or "TAA" in aa) and "Disable AA" not in patches:
            patches.append("Disable AA")
        patch_count = len(patches)
        set_chip(
            "patches",
            f"{patch_count} patch" if patch_count == 1 else f"{patch_count} patches",
            "Enabled patches - click to open Patches",
        )

        mods = bool(cfg.get("mods"))
        set_chip("mods", "Mods On" if mods else "Mods Off", "Mods - click to open Advanced Settings")

    def update_run_clock(self):
        if self._run_started is None:
            return
        elapsed = max(0, int(time.monotonic() - self._run_started))
        hours, rem = divmod(elapsed, 3600)
        minutes, seconds = divmod(rem, 60)
        if hours:
            clock = f"{hours:02d}:{minutes:02d}:{seconds:02d}"
        else:
            clock = f"{minutes:02d}:{seconds:02d}"
        self._set_run_info("running", f"\u25cf Game Running \u2014 {clock}")

    @staticmethod
    def _format_duration(seconds) -> str:
        try:
            total = max(0, int(seconds))
        except (TypeError, ValueError):
            return ""
        if total < 60:
            return f"{total}s"
        minutes, secs = divmod(total, 60)
        if minutes < 60:
            return f"{minutes}m {secs:02d}s"
        hours, minutes = divmod(minutes, 60)
        return f"{hours}h {minutes:02d}m"

    def update_last_run(self):
        if not hasattr(self, "lbl_last_run"):
            return
        stamp = str(self.settings.get("last_run_time", "") or "")
        if not stamp:
            self.lbl_last_run.setText("No launch recorded yet. Launch the game to populate this panel.")
            self.lbl_last_run.setStyleSheet("color: #7c7c88; font-size: 11px; background: transparent;")
            self.lbl_last_run.setToolTip("")
            return

        try:
            when_dt = datetime.fromisoformat(stamp)
            days_ago = (datetime.now().date() - when_dt.date()).days
            if days_ago == 0:
                when = f"today {when_dt:%H:%M}"
            elif days_ago == 1:
                when = f"yesterday {when_dt:%H:%M}"
            else:
                when = when_dt.strftime("%Y-%m-%d %H:%M")
        except ValueError:
            when = stamp

        try:
            rc = int(self.settings.get("last_run_rc"))
        except (TypeError, ValueError):
            rc = None

        if rc is None:
            result = "result unknown"
        elif rc == 0:
            result = "exited normally"
        elif rc < 0:
            result = "launch failed"
        else:
            result = f"exited with code {rc}"

        duration = self._format_duration(self.settings.get("last_run_duration", 0))
        text = f"Last launch: {when} \u00b7 {result}"
        if duration:
            text += f" \u00b7 {duration}"

        self.lbl_last_run.setText(text)
        if rc == 0:
            color = "#7fb08a"
        elif rc is None:
            color = "#7c7c88"
        else:
            color = "#e0a33c"
        self.lbl_last_run.setStyleSheet(
            f"color: {color}; font-size: 11px; background: transparent;"
        )
        self.lbl_last_run.setToolTip(
            f"Recorded: {stamp}" + (f"\nDuration: {duration}" if duration else "")
        )

    def copy_diagnostics(self):
        try:
            text = self._build_diagnostics()
        except Exception as ex:
            QMessageBox.critical(self, "Diagnostics", f"Could not build diagnostics:\n{ex}")
            return
        QApplication.clipboard().setText(text)
        self.log("[DIAG] Diagnostics copied to the clipboard.")
        if hasattr(self, "btn_diag"):
            self.btn_diag.setText("Copied")
            QTimer.singleShot(1600, lambda: self.btn_diag.setText("Copy Diagnostics"))

    def _build_diagnostics(self) -> str:
        cfg = self.current_config()
        game_dir = resolve_game_dir(cfg.get("eboot", ""))
        lines = [
            "Bloodborne PC Launcher diagnostics",
            f"Generated: {datetime.now():%Y-%m-%d %H:%M:%S}",
            f"Launcher: v{LAUNCHER_VERSION} ({build_stamp() or 'unknown build'})",
            f"Mode: {'frozen exe' if getattr(sys, 'frozen', False) else 'python source'}",
            f"OS: {self._os_label()}",
            f"CPU: {platform.processor() or 'unknown'} ({os.cpu_count() or '?'} logical cores)",
            f"Game path: {cfg.get('eboot', '') or '(not set)'}",
            f"Game folder: {game_dir or '(not resolved)'}",
            f"Vulkan runtime: {'vulkan-1.dll loaded' if self._vulkan_available() else 'not detected'}",
            "GPU / driver:",
        ]

        gpu_lines = self._gpu_driver_lines()
        if gpu_lines:
            lines.extend(f"  {line}" for line in gpu_lines)
        else:
            lines.append("  (not detected)")

        lines.extend([
            "",
            "Active configuration:",
            json.dumps(cfg, indent=2, ensure_ascii=False),
        ])

        ini = self.load_bbport_ini()
        if ini:
            lines.extend([
                "",
                "bbport.ini snapshot:",
                json.dumps(ini, indent=2, ensure_ascii=False),
            ])

        tail = [str(line) for line in getattr(self, "_log_history", [])[-200:]]
        if not tail and LOG_FILE.is_file():
            try:
                tail = LOG_FILE.read_text(encoding="utf-8", errors="replace").splitlines()[-200:]
            except OSError:
                tail = []
        if tail:
            lines.extend(["", f"Last {len(tail)} console lines:", *tail])

        return "\n".join(lines)

    @staticmethod
    def _os_label() -> str:
        if os.name == "nt":
            try:
                version = sys.getwindowsversion()
                return f"Windows {version.major}.{version.minor}.{version.build}"
            except Exception:
                pass
        return platform.platform()

    @staticmethod
    def _vulkan_available() -> bool:
        try:
            lib = ctypes.CDLL("vulkan-1.dll")
            handle = getattr(lib, "_handle", None)
            if os.name == "nt" and handle:
                ctypes.windll.kernel32.FreeLibrary(ctypes.c_void_p(handle))
            return True
        except OSError:
            return False

    def _gpu_driver_lines(self) -> list[str]:
        lines = []
        if os.name == "nt":
            try:
                import winreg

                base = r"SYSTEM\CurrentControlSet\Control\Class\{4d36e968-e325-11ce-bfc1-08002be10318}"
                with winreg.OpenKey(winreg.HKEY_LOCAL_MACHINE, base) as root:
                    index = 0
                    while True:
                        try:
                            subkey = winreg.EnumKey(root, index)
                        except OSError:
                            break
                        index += 1
                        if not subkey.isdigit():
                            continue
                        try:
                            with winreg.OpenKey(root, subkey) as adapter:
                                desc = str(winreg.QueryValueEx(adapter, "DriverDesc")[0]).strip()
                                version = str(winreg.QueryValueEx(adapter, "DriverVersion")[0]).strip()
                                if desc:
                                    lines.append(f"{desc} \u2014 driver {version or 'unknown'}")
                        except OSError:
                            continue
            except Exception:
                pass

        if not lines:
            chosen = str(self.settings.get("gpu", "") or "Auto (Vulkan default)")
            lines.append(f"{chosen} (driver version unavailable)")
        return lines

    def open_log_file(self):
        target = LOG_FILE if LOG_FILE.is_file() else LOG_DIR
        if not target.exists():
            QMessageBox.information(
                self,
                "Engine Log",
                f"No engine log found yet.\n\nExpected location:\n{LOG_FILE}",
            )
            return
        try:
            os.startfile(str(target))
        except OSError as ex:
            QMessageBox.critical(self, "Engine Log", f"Could not open the log:\n{ex}")

    def save_log(self):
        if not self._log_history:
            QMessageBox.information(self, "Console Log", "There is no console output to save yet.")
            return
        default_name = str(APP_DIR / f"bblauncher_console_{datetime.now():%Y%m%d_%H%M%S}.log")
        path, _ = QFileDialog.getSaveFileName(
            self,
            "Save Console Output",
            default_name,
            "Log files (*.log);;Text files (*.txt);;All files (*)",
        )
        if not path:
            return
        try:
            Path(path).write_text("\n".join(self._log_history) + "\n", encoding="utf-8")
            self.log(f"[LOG] Console output saved to: {path}")
        except OSError as ex:
            QMessageBox.critical(self, "Save Console Log", f"Could not save the log:\n{ex}")

    def on_log_filter_changed(self, _index=None):
        self._log_filter = self.cmb_log_filter.currentData() or "hide_vk"
        self.save_settings()
        self.apply_log_filter()

    def on_log_autoscroll_changed(self, _state=None):
        self.save_settings()
        if self.chk_autoscroll.isChecked():
            self.log_view.moveCursor(QTextCursor.End)

    def _line_visible(self, line: str) -> bool:
        text = str(line)
        if self._log_filter == "all":
            return True
        if self._log_filter == "hide_vk":
            return VK_NOISE not in text

        low = text.lower()
        if VK_NOISE in text:
            return False
        prefixes = ("[warn", "[error", "[pre-launch", "[cache", "[diag", "process exited", "launch error", "traceback")
        if low.startswith(prefixes):
            return True
        return any(word in low for word in ("error", "warning", "failed", "fatal", "exception", "crash"))

    def apply_log_filter(self):
        if not hasattr(self, "log_view"):
            return
        visible = [str(line) for line in self._log_history if self._line_visible(line)]
        self.hidden_count = len(self._log_history) - len(visible)
        self.log_view.clear()
        if visible:
            self.log_view.setPlainText("\n".join(visible))
        if hasattr(self, "chk_autoscroll") and self.chk_autoscroll.isChecked():
            self.log_view.moveCursor(QTextCursor.End)

    def update_cache_info(self):
        if not hasattr(self, "lbl_cache_info"):
            return
        cache_dir = self._user_dir() / "cache"
        files = []
        if cache_dir.is_dir():
            try:
                files = [path for path in cache_dir.rglob("*") if path.is_file()]
            except OSError:
                files = []
        total = 0
        for path in files:
            try:
                total += path.stat().st_size
            except OSError:
                pass
        if files:
            text = f"Shader cache: {self._format_size(total)} \u00b7 {len(files)} files"
        else:
            text = "Shader cache: empty"
        self.lbl_cache_info.setText(text)
        self.lbl_cache_info.setToolTip(str(cache_dir))
        if hasattr(self, "btn_cache"):
            self.btn_cache.setEnabled(bool(files))
            self.btn_cache.setToolTip(f"Delete compiled shaders from:\n{cache_dir}")

    @staticmethod
    def _format_size(size: float) -> str:
        value = float(size)
        for unit in ("B", "KB", "MB", "GB"):
            if value < 1024 or unit == "GB":
                return f"{value:.0f} {unit}" if unit == "B" else f"{value:.1f} {unit}"
            value /= 1024
        return f"{value:.1f} GB"

    def load_bbport_ini(self) -> dict:
        ini_path = APP_DIR / "bbport.ini"
        values = {}
        if ini_path.is_file():
            try:
                for line in ini_path.read_text(encoding="utf-8").splitlines():
                    trimmed = line.strip()
                    if trimmed and not trimmed.startswith("#") and "=" in trimmed:
                        k, v = trimmed.split("=", 1)
                        values[k.strip()] = v.strip()
            except OSError:
                pass
        return values

    def save_bbport_ini(self):
        if getattr(self, "_syncing_ini", False):
            return
        if getattr(self, "_suppress_settings_save", False):
            return
        # Native mouse look is mandatory: stick emulation was removed from the port.
        keys_to_update = {"mk_native_mouse": "1"}

        if self._display_built:
            res_raw = self.cmb_res.currentText()
            output_res_val = "1920x1080"
            for candidate in ("1280x720", "1920x1080", "2560x1440", "3840x2160"):
                if candidate in res_raw:
                    output_res_val = candidate
                    break

            aa_choice = self.cmb_aa.currentText()
            upscaler_choice = self.cmb_upscaler.currentText()
            is_fsr = self.chk_fsr.isChecked() and upscaler_choice != "Off"
            is_taa = "TAA" in aa_choice
            is_upscaler_active = is_fsr or is_taa
            effect_game_aa = "0" if is_upscaler_active or "Disable" in aa_choice else "1"

            if is_taa:
                upscaler_val = "taa"
            elif is_fsr:
                upscaler_val = self.cmb_upscaler.currentData() or "fsr31"
                if upscaler_val not in ("fsr31", "fsr4", "fsr411", "dlss"):
                    upscaler_val = "fsr31"
            else:
                upscaler_val = "off"

            prep_choice = self.cmb_prep_workers.currentText()
            prep_workers_val = "auto"
            for label, val in PREP_WORKER_CHOICES:
                if label == prep_choice:
                    prep_workers_val = val
                    break

            keys_to_update.update({
                "upscaler": upscaler_val,
                "preset": self.cmb_preset.currentData() or "0",
                "output_res": output_res_val,
                "effect_game_aa": effect_game_aa,
                "prep_workers": prep_workers_val,
                "gpu": self.cmb_gpu.currentData() or "",
                "live_resolution": self.cmb_live_res.currentData() or "0",
                "sharpen": "1" if self.chk_sharpen.isChecked() else "0",
                "sharpness": f"{self.sl_sharpness.value() / 100:.2f}",
                "jitter": "1" if self.chk_jitter.isChecked() else "0",
                "reactive": "1" if self.chk_reactive.isChecked() else "0",
                "object_motion": "1" if self.chk_object_motion.isChecked() else "0",
                "reactive_scale": f"{self.sl_reactive_scale.value() / 100:.2f}",
                "reactive_threshold": f"{self.sl_reactive_threshold.value() / 100:.2f}",
                "reactive_max": f"{self.sl_reactive_max.value() / 100:.2f}",
                "fsr4_auto_exposure": "1" if self.chk_fsr4_auto_exposure.isChecked() else "0",
                "fsr4_invert_jitter": "1" if self.chk_fsr4_invert_jitter.isChecked() else "0",
                "debug_view": self.cmb_debug_view.currentData() or "0",
                "show_fps": "1" if self.chk_show_fps.isChecked() else "0",
                "show_hud": "1" if self.chk_hud.isChecked() else "0",
                "hud_quadrant": self.cmb_hud_quadrant.currentData() or "0",
                "hud_opacity": f"{self.sl_hud_opacity.value() / 100:.2f}",
                "hud_scale": f"{self.sl_hud_scale.value() / 100:.2f}",
                "model_lod": self.cmb_model_lod.currentData() or "0",
                "live_fps": "1" if (self.chk_fps_patch.isChecked()
                                    and (self.cmb_fps.currentData() or "60") != "30") else "0",
                "target_fps": self.cmb_fps.currentData() or "60",
            })
            for effect_key, chk in self.effect_checks.items():
                keys_to_update[effect_key] = "1" if chk.isChecked() else "0"

        if self._mk_built:
            keys_to_update.update({
                "mk_enabled": "1" if self.chk_mk_enabled.isChecked() else "0",
                "mk_sens_x": f"{self.sl_sens_x.value() / 100:.2f}",
                "mk_sens_y": f"{self.sl_sens_y.value() / 100:.2f}",
                "mk_invert_x": "1" if self.chk_inv_x.isChecked() else "0",
                "mk_invert_y": "1" if self.chk_inv_y.isChecked() else "0",
                "mk_smoothing": f"{self.sl_smoothing.value() / 100:.2f}",
                "mk_responsiveness": f"{self.sl_responsiveness.value() / 100:.2f}",
                "mk_acceleration": f"{self.sl_acceleration.value() / 100:.2f}",
                "mouse_menu": "1" if self.chk_mouse_menu.isChecked() else "0",
            })
            for k, btn in self.keybind_buttons.items():
                keys_to_update[k] = btn.current_binding

        if self._advanced_built:
            keys_to_update["language"] = self.cmb_menu_lang.currentData() or "en"

        if not keys_to_update:
            return
        if self._write_ini_values(keys_to_update):
            changed = self._sync_patches_from_ini(persist=True)
            if not changed and hasattr(self, "home_chips"):
                self.update_home_summary()

    def _write_ini_values(self, keys_to_update: dict) -> bool:
        if not keys_to_update:
            return False
        if getattr(self, "_syncing_ini", False) or getattr(self, "_suppress_settings_save", False):
            return False

        ini_path = APP_DIR / "bbport.ini"
        lines = []
        written = set()
        if ini_path.is_file():
            try:
                for line in ini_path.read_text(encoding="utf-8").splitlines():
                    trimmed = line.strip()
                    if trimmed and not trimmed.startswith("#") and "=" in trimmed:
                        key, _ = trimmed.split("=", 1)
                        key = key.strip()
                        if key in keys_to_update:
                            lines.append(f"{key}={keys_to_update[key]}")
                            written.add(key)
                            continue
                    lines.append(line)
            except OSError:
                pass

        for key, value in keys_to_update.items():
            if key not in written:
                lines.append(f"{key}={value}")

        try:
            ini_path.write_text("\n".join(lines) + "\n", encoding="utf-8")
            self._last_ini_mtime = ini_path.stat().st_mtime
            self.log("[Settings] bbport.ini updated.")
            return True
        except OSError as ex:
            self.log(f"[ERROR] Could not write to {ini_path}: {ex}")
            return False

    def _assign_keybind(self, action_key: str, new_val: str, old_val=None):
        """Set one binding and move any conflicting action off it.

        The old value is supplied by KeybindButton because it has already updated its
        label when the signal arrives; reading current_binding here would see the new
        value and create a duplicate instead of swapping.
        """
        btn = self.keybind_buttons.get(action_key)
        if btn is None:
            return
        if old_val is None:
            old_val = btn.current_binding
        if new_val != "None":
            for other_key, other in self.keybind_buttons.items():
                if other_key == action_key:
                    continue
                if other.current_binding.lower() != new_val.lower():
                    continue
                # The old value may itself belong to a third action; in that case
                # clearing the conflict is safer than creating another duplicate.
                replacement = old_val
                if replacement != "None" and any(
                    key not in (action_key, other_key)
                    and other_btn.current_binding.lower() == replacement.lower()
                    for key, other_btn in self.keybind_buttons.items()
                ):
                    replacement = "None"
                other.set_binding(replacement)
                self.log(f"[Keybinds] Swapped '{new_val}' between {action_key} and {other_key}")
                break
        btn.set_binding(new_val)

    def on_keybind_button_assigned(self, action_key: str, new_val: str, old_val: str):
        if not hasattr(self, "keybind_buttons"):
            return
        self._assign_keybind(action_key, new_val, old_val)
        self.save_bbport_ini()

    def apply_keybind_preset(self, layout: str):
        """Switch the movement keys between the QWERTY (WASD) and AZERTY (ZQSD) layouts."""
        preset = KEYBIND_PRESETS.get(layout)
        if not preset or not hasattr(self, "keybind_buttons"):
            return
        for action_key in ("key.move_forward", "key.move_left",
                           "key.move_backward", "key.move_right"):
            btn = self.keybind_buttons.get(action_key)
            if btn is not None:
                # The preset holds printed labels.  Convert each one to the physical
                # SDL scancode name for this keyboard layout before saving.
                stored = display_name_to_scancode_label(preset[action_key])
                self._assign_keybind(action_key, stored, btn.current_binding)
        self.save_bbport_ini()
        keys = "WASD" if layout == "qwerty" else "ZQSD"
        self.log(f"[Keybinds] Applied {layout.upper()} movement preset ({keys}).")

    def reset_keybinds_to_defaults(self):
        from bblauncher_Utils import DEFAULT_KEYBINDS
        if not hasattr(self, "keybind_buttons"):
            return
        for ini_key, _, def_val in DEFAULT_KEYBINDS:
            if ini_key in self.keybind_buttons:
                self.keybind_buttons[ini_key].set_binding(def_val)
        self.save_bbport_ini()
        self.log("[Keybinds] Reset all keybinds to defaults.")

    def browse_path(self, edit, kind: str):
        start = edit.text().strip() or str(APP_DIR)
        if kind == "dir":
            path = QFileDialog.getExistingDirectory(self, "Select directory", start)
        else:
            path, _ = QFileDialog.getOpenFileName(self, "Select file", start, "All Files (*)")
        if path:
            edit.setText(os.path.normpath(path))
            self.save_settings()

    def save_advanced(self):
        self.save_settings()
        self.save_bbport_ini()
        self.log("[Settings] Advanced settings saved.")

    def open_bbport_ini(self):
        ini_path = APP_DIR / "bbport.ini"
        if not ini_path.is_file():
            QMessageBox.information(self, "bbport.ini", f"File not found:\n{ini_path}")
            return
        try:
            os.startfile(str(ini_path))
        except OSError as ex:
            QMessageBox.critical(self, "bbport.ini", f"Could not open the file:\n{ex}")

    def reload_bbport_ini(self):
        self._last_ini_mtime = 0
        self.sync_bbport_ini_to_ui()
        self.log("[Settings] Reloaded values from bbport.ini.")

    @staticmethod
    def _ini_bool(opts: dict, key: str, default: bool) -> bool:
        value = str(opts.get(key, "1" if default else "0")).strip().lower()
        return value not in ("0", "", "false", "off", "no")

    @staticmethod
    def _ini_percent(opts: dict, key: str, default: float, minimum: int, maximum: int) -> int:
        try:
            value = int(round(float(opts.get(key, str(default))) * 100))
        except (TypeError, ValueError):
            value = int(round(default * 100))
        return max(minimum, min(maximum, value))

    @staticmethod
    def _set_check(chk, checked: bool):
        chk.blockSignals(True)
        chk.setChecked(bool(checked))
        chk.blockSignals(False)

    @staticmethod
    def _set_combo_data(combo, value: str):
        text = str(value).strip()
        idx = combo.findData(text.lower())
        if idx < 0:
            idx = combo.findData(text)
        if idx >= 0 and combo.currentIndex() != idx:
            combo.blockSignals(True)
            combo.setCurrentIndex(idx)
            combo.blockSignals(False)

    @staticmethod
    def _set_slider(slider, label, value: int):
        value = max(slider.minimum(), min(slider.maximum(), value))
        slider.blockSignals(True)
        slider.setValue(value)
        slider.blockSignals(False)
        label.setText(f"{value / 100:.2f}")

    @staticmethod
    def _patch_value_for_state(active_value: str, enabled: bool) -> str:
        if enabled:
            return active_value
        return "1" if active_value == "0" else "0"

    def _set_effect_checkbox(self, key: str, value: str):
        if not self._display_built:
            return
        chk = getattr(self, "effect_checks", {}).get(key)
        if chk is None:
            return
        self._set_check(chk, str(value).strip() == "1")

    def _update_enabled_patches_from_ini(self, ini_opts: dict | None = None) -> bool:
        if ini_opts is None:
            ini_opts = self.load_bbport_ini()
        changed = False
        for name, (key, active_value) in EFFECT_PATCH_MAP.items():
            if key not in ini_opts:
                continue
            effect_on = self._ini_bool(ini_opts, key, active_value == "1")
            enabled = effect_on if active_value == "1" else not effect_on
            if enabled and name not in self.enabled_patches:
                self.enabled_patches.add(name)
                changed = True
            elif not enabled and name in self.enabled_patches:
                self.enabled_patches.discard(name)
                changed = True
        is_live = hasattr(self, "chk_fps_patch") and self.chk_fps_patch.isChecked()
        if is_live:
            for p in list(self.enabled_patches):
                if p in LIVE_FPS_CONFLICTING_PATCHES:
                    self.enabled_patches.discard(p)
                    changed = True
        return changed

    def _sync_patches_from_ini(self, ini_opts: dict | None = None, persist: bool = False) -> bool:
        changed = self._update_enabled_patches_from_ini(ini_opts)
        self.sync_patches_to_ui()
        if changed:
            if persist:
                self.save_settings()
            elif hasattr(self, "home_chips"):
                self.update_home_summary()
        return changed

    def _apply_patch_states_to_ini(self):
        updates = {
            key: self._patch_value_for_state(active_value, name in self.enabled_patches)
            for name, (key, active_value) in EFFECT_PATCH_MAP.items()
        }
        if not updates:
            return
        if self._write_ini_values(updates):
            for key, value in updates.items():
                self._set_effect_checkbox(key, value)

    def _sync_display_from_ini(self, ini_opts: dict):
        ini_upscaler = ini_opts.get("upscaler", "off").strip().lower()
        if ini_upscaler in ("fsr3", "fsr31"):
            upscaler_value = "fsr31"
        elif ini_upscaler == "fsr4":
            upscaler_value = "fsr4"
        elif ini_upscaler == "fsr411":
            upscaler_value = "fsr411"
        elif ini_upscaler == "dlss":
            upscaler_value = "dlss"
        else:
            upscaler_value = "off"
        self._set_combo_data(self.cmb_upscaler, upscaler_value)
        self._set_check(self.chk_fsr, upscaler_value != "off")
        self._set_combo_data(self.cmb_preset, ini_opts.get("preset", "1"))
        self._set_combo_data(self.cmb_live_res, ini_opts.get("live_resolution", "0").strip().lower())
        self._set_combo_data(self.cmb_hud_quadrant, ini_opts.get("hud_quadrant", "0"))
        self._set_combo_data(self.cmb_debug_view, ini_opts.get("debug_view", "0"))
        self._set_combo_data(self.cmb_model_lod, ini_opts.get("model_lod", "0"))

        ini_pw = ini_opts.get("prep_workers", "auto").strip().lower()
        for label, val in PREP_WORKER_CHOICES:
            if val == ini_pw or (ini_pw == "auto" and val == "auto"):
                self.cmb_prep_workers.blockSignals(True)
                self.cmb_prep_workers.setCurrentText(label)
                self.cmb_prep_workers.blockSignals(False)
                break

        ini_res = ini_opts.get("output_res", "").strip()
        if ini_res:
            for idx_res in range(self.cmb_res.count()):
                if ini_res in self.cmb_res.itemText(idx_res):
                    self.cmb_res.blockSignals(True)
                    self.cmb_res.setCurrentIndex(idx_res)
                    self.cmb_res.blockSignals(False)
                    break

        for chk, key, default in (
            (self.chk_sharpen, "sharpen", True),
            (self.chk_jitter, "jitter", True),
            (self.chk_reactive, "reactive", False),
            (self.chk_object_motion, "object_motion", False),
            (self.chk_fsr4_auto_exposure, "fsr4_auto_exposure", False),
            (self.chk_fsr4_invert_jitter, "fsr4_invert_jitter", False),
            (self.chk_show_fps, "show_fps", False),
            (self.chk_hud, "show_hud", True),
        ):
            self._set_check(chk, self._ini_bool(ini_opts, key, default))

        self._set_slider(self.sl_sharpness, self.lbl_sharpness_val, self._ini_percent(ini_opts, "sharpness", 0.30, 0, 200))
        self._set_slider(self.sl_reactive_scale, self.lbl_reactive_scale_val, self._ini_percent(ini_opts, "reactive_scale", 1.00, 0, 1600))
        self._set_slider(self.sl_reactive_threshold, self.lbl_reactive_threshold_val, self._ini_percent(ini_opts, "reactive_threshold", 0.20, 0, 100))
        self._set_slider(self.sl_reactive_max, self.lbl_reactive_max_val, self._ini_percent(ini_opts, "reactive_max", 0.90, 0, 100))
        self._set_slider(self.sl_hud_opacity, self.lbl_hud_opacity_val, self._ini_percent(ini_opts, "hud_opacity", 0.75, 10, 100))
        self._set_slider(self.sl_hud_scale, self.lbl_hud_scale_val, self._ini_percent(ini_opts, "hud_scale", 1.00, 50, 250))

        effect_defaults = {key: default for key, _label, default in EFFECT_CHOICES}
        for effect_key, chk in self.effect_checks.items():
            self._set_check(chk, self._ini_bool(ini_opts, effect_key, effect_defaults.get(effect_key, False)))

    def _sync_mk_from_ini(self, ini_opts: dict):
        self._set_check(self.chk_mk_enabled, self._ini_bool(ini_opts, "mk_enabled", True))
        self._set_check(self.chk_inv_x, self._ini_bool(ini_opts, "mk_invert_x", False))
        self._set_check(self.chk_inv_y, self._ini_bool(ini_opts, "mk_invert_y", False))
        self._set_check(self.chk_mouse_menu, self._ini_bool(ini_opts, "mouse_menu", True))

        self._set_slider(self.sl_sens_x, self.lbl_sens_x_val, self._ini_percent(ini_opts, "mk_sens_x", 3.20, 10, 1000))
        self._set_slider(self.sl_sens_y, self.lbl_sens_y_val, self._ini_percent(ini_opts, "mk_sens_y", 3.26, 10, 1000))
        self._set_slider(self.sl_smoothing, self.lbl_smoothing_val, self._ini_percent(ini_opts, "mk_smoothing", 0.25, 0, 80))
        self._set_slider(self.sl_responsiveness, self.lbl_responsiveness_val, self._ini_percent(ini_opts, "mk_responsiveness", 1.00, 50, 250))
        self._set_slider(self.sl_acceleration, self.lbl_acceleration_val, self._ini_percent(ini_opts, "mk_acceleration", 1.30, 100, 200))

        for k, btn in self.keybind_buttons.items():
            if k in ini_opts:
                btn.set_binding(ini_opts[k])

    def _sync_external_patch_settings(self):
        if not self._patches_built:
            return
        try:
            if not SETTINGS_FILE.is_file():
                return
            mtime = SETTINGS_FILE.stat().st_mtime
        except OSError:
            return
        if mtime == getattr(self, "_last_settings_mtime", 0):
            return
        self._last_settings_mtime = mtime
        try:
            loaded = json.loads(SETTINGS_FILE.read_text(encoding="utf-8"))
        except Exception:
            return
        if not isinstance(loaded, dict):
            return

        external = set(loaded.get("enabled_patches", []) or [])
        changed = False
        for patch in self.all_xml_patches:
            name = str(patch.get("name", ""))
            if not name or name in EFFECT_PATCH_MAP:
                continue
            should_enable = name in external
            if should_enable and name not in self.enabled_patches:
                self.enabled_patches.add(name)
                changed = True
            elif not should_enable and name in self.enabled_patches:
                self.enabled_patches.discard(name)
                changed = True
        if changed:
            self.settings["enabled_patches"] = sorted(self.enabled_patches)
            self.sync_patches_to_ui()
            self.update_home_summary()

    def sync_bbport_ini_to_ui(self):
        self._sync_external_patch_settings()
        if not (self._display_built or self._mk_built or self._advanced_built or self._patches_built):
            return
        ini_path = APP_DIR / "bbport.ini"
        if not ini_path.is_file():
            return
        try:
            mtime = ini_path.stat().st_mtime
            if getattr(self, "_last_ini_mtime", None) == mtime:
                return
            self._last_ini_mtime = mtime
            ini_opts = self.load_bbport_ini()
            self._syncing_ini = True
            try:
                if self._display_built:
                    self._sync_display_from_ini(ini_opts)
                if self._mk_built:
                    self._sync_mk_from_ini(ini_opts)
                if self._advanced_built:
                    self._set_combo_data(self.cmb_menu_lang, ini_opts.get("language", "en").strip().lower())
                if self._patches_built:
                    self._sync_patches_from_ini(ini_opts, persist=True)
            finally:
                self._syncing_ini = False
        except Exception:
            self._syncing_ini = False
        if hasattr(self, "home_chips"):
            self.update_home_summary()

    def on_patch_toggled(self, name: str, state: int):
        is_live = hasattr(self, "chk_fps_patch") and self.chk_fps_patch.isChecked()
        if is_live and name in LIVE_FPS_CONFLICTING_PATCHES:
            if hasattr(self, "patch_checkboxes") and name in self.patch_checkboxes:
                cb = self.patch_checkboxes[name]
                cb.blockSignals(True)
                cb.setChecked(False)
                cb.setEnabled(False)
                cb.setToolTip("Disabled: Conflicts with Live FPS Patching.")
                cb.blockSignals(False)
            self.enabled_patches.discard(name)
            self.update_patch_count_label()
            return

        enabled = bool(state)
        if enabled:
            self.enabled_patches.add(name)
        else:
            self.enabled_patches.discard(name)

        mapped = EFFECT_PATCH_MAP.get(name)
        if mapped:
            key, active_value = mapped
            value = self._patch_value_for_state(active_value, enabled)
            if self._write_ini_values({key: value}):
                self._set_effect_checkbox(key, value)

        self.update_patch_count_label()
        self.save_settings()

    def update_patch_count_label(self):
        if self._patches_built and hasattr(self, "lbl_patch_count"):
            self.lbl_patch_count.setText(f"{len(self.enabled_patches)} / {len(self.all_xml_patches)} enabled")

    def _sync_patches_to_live_fps(self, is_live: bool = None):
        if is_live is None:
            is_live = hasattr(self, "chk_fps_patch") and self.chk_fps_patch.isChecked()
        changed = False
        if is_live:
            for p in list(self.enabled_patches):
                if p in LIVE_FPS_CONFLICTING_PATCHES:
                    self.enabled_patches.discard(p)
                    changed = True
        if getattr(self, "_patches_built", False) and hasattr(self, "patch_checkboxes"):
            for name, cb in self.patch_checkboxes.items():
                if name in LIVE_FPS_CONFLICTING_PATCHES:
                    cb.blockSignals(True)
                    if is_live:
                        cb.setChecked(False)
                        cb.setEnabled(False)
                        cb.setToolTip("Disabled: Conflicts with Live FPS Patching.")
                    else:
                        cb.setEnabled(True)
                        cb.setChecked(name in self.enabled_patches)
                        cb.setToolTip("")
                    cb.blockSignals(False)
            self.update_patch_count_label()
        if changed:
            self.save_settings()

    def sync_patches_to_ui(self):
        if not self._patches_built:
            return
        is_live = hasattr(self, "chk_fps_patch") and self.chk_fps_patch.isChecked()
        for name, cb in self.patch_checkboxes.items():
            cb.blockSignals(True)
            if is_live and name in LIVE_FPS_CONFLICTING_PATCHES:
                cb.setChecked(False)
                cb.setEnabled(False)
                cb.setToolTip("Disabled: Conflicts with Live FPS Patching.")
            else:
                cb.setEnabled(True)
                cb.setChecked(name in self.enabled_patches)
                cb.setToolTip("")
            cb.blockSignals(False)
        self.update_patch_count_label()

    def filter_patches(self):
        if not self._patches_built:
            return
        q = self.txt_patch_search.text().strip().lower()
        selected_cat = self.cmb_patch_cat.currentText()

        self.patch_list_widget.setUpdatesEnabled(False)
        for i in range(self.patch_list_layout.count() - 1):
            widget = self.patch_list_layout.itemAt(i).widget()
            if not widget:
                continue
            w_cat = widget.property("patch_category")
            cat_match = (selected_cat == "All Categories") or (w_cat == selected_cat)
            query_match = not q or (q in widget.property("patch_search_blob"))
            widget.setVisible(cat_match and query_match)
        self.patch_list_widget.setUpdatesEnabled(True)

    def apply_recommended_patches(self):
        self.enabled_patches = {"Skip Intro"}
        self._apply_patch_states_to_ini()
        self.sync_patches_to_ui()
        self.save_settings()
        self.log("[PATCHES] Recommended defaults applied.")

    def log(self, text: str):
        self.log_batch([text])

    def log_batch(self, lines: list[str]):
        if not lines:
            return
        raw_lines = [str(line) for line in lines]
        if self.log_fh:
            try:
                self.log_fh.write("\n".join(raw_lines) + "\n")
                self.log_fh.flush()
            except OSError:
                pass

        self._log_history.extend(raw_lines)
        if len(self._log_history) > 5000:
            del self._log_history[:-5000]

        visible = [line for line in raw_lines if self._line_visible(line)]
        self.hidden_count += len(raw_lines) - len(visible)

        if visible and hasattr(self, "log_view"):
            self.log_view.appendPlainText("\n".join(visible))
            if self.chk_autoscroll.isChecked():
                self.log_view.moveCursor(QTextCursor.End)

    def copy_log(self):
        QApplication.clipboard().setText(self.log_view.toPlainText())

    def clear_log(self):
        self._log_history.clear()
        self.hidden_count = 0
        self.log_view.clear()

    def browse_eboot(self):
        start = self.txt_eboot.text().strip() or str(DEFAULT_GAME if DEFAULT_GAME.exists() else APP_DIR)
        path, _ = QFileDialog.getOpenFileName(
            self, "Select Bloodborne eboot.bin", start, "PS4 Executable (*.bin);;All Files (*)"
        )
        if path:
            self.txt_eboot.setText(os.path.normpath(path))
            self.log(f"Selected executable: {path}")

    def manual_verify_setup(self):
        target = self.txt_eboot.text().strip()
        if not target:
            QMessageBox.critical(self, "Error", "Specify the path to eboot.bin first.")
            return

        game_dir = resolve_game_dir(target)
        if not game_dir:
            QMessageBox.critical(self, "Invalid Path", f"Could not find eboot.bin in:\n{target}")
            return

        self.log("--- Running Pre-Launch Verification Check ---")
        ready, err, details = verify_prelaunch(game_dir)
        self.log_batch([f"  {line}" for line in details])

        if ready:
            self.log("[PRE-LAUNCH] All checks passed.")
            QMessageBox.information(self, "Verification Passed", "System ready.\n\n" + "\n".join(details))
        else:
            self.log(f"[PRE-LAUNCH ERROR] {err}")
            QMessageBox.critical(self, "Verification Failed", err)

    def _user_dir(self) -> Path:
        value = self.txt_user_dir.text().strip() if self._advanced_built else str(self.settings.get("user_dir", "")).strip()
        base = Path(value) if value else Path("user")
        if not base.is_absolute():
            base = APP_DIR / base
        return base

    def reset_shader_cache(self):
        import shutil

        if self.worker and self.worker.isRunning():
            QMessageBox.warning(self, "Busy", "Terminate game execution before resetting the shader cache.")
            return

        self.save_settings()
        cache_dir = self._user_dir() / "cache"
        files = [p for p in cache_dir.rglob("*") if p.is_file()] if cache_dir.is_dir() else []
        if not files:
            self.update_cache_info()
            QMessageBox.information(self, "Cache Clear", "The shader cache is already empty.")
            return

        confirm = QMessageBox.question(
            self,
            "Reset Shader Cache",
            f"Delete {len(files)} compiled shader cache files in {cache_dir}?",
            QMessageBox.Yes | QMessageBox.No,
        )
        if confirm == QMessageBox.Yes:
            try:
                shutil.rmtree(cache_dir)
                self.log(f"[CACHE] Shader cache purged ({len(files)} files removed).")
            except OSError as ex:
                QMessageBox.critical(self, "Error", f"Could not purge cache directory: {ex}")
            self.update_cache_info()

    def blocked_by_running_instance(self) -> bool:
        found = running_instances()
        if not found:
            return False
        guard = get_bb_guard()
        lines = "\n".join(guard.describe(i) for i in found)
        self.log(f"[PRE-LAUNCH] bbport.exe is already running ({lines.replace(chr(10), '; ')})")
        reply = QMessageBox.warning(
            self,
            "Bloodborne is already running",
            f"bbport.exe is still running:\n\n{lines}\n\nTerminate it now and continue?",
            QMessageBox.Yes | QMessageBox.No,
        )
        if reply != QMessageBox.Yes:
            self.log("[PRE-LAUNCH] Launch cancelled: close the running bbport.exe first.")
            return True
        survivors = [i.pid for i in found if not guard.terminate(i.pid)]
        if survivors:
            self.log(f"[PRE-LAUNCH ERROR] Could not terminate PID {', '.join(map(str, survivors))}")
            QMessageBox.critical(
                self,
                "Cannot terminate bbport.exe",
                f"PID {', '.join(map(str, survivors))} did not exit.\nEnd it in Task Manager, then launch again.",
            )
            return True
        self.log("[PRE-LAUNCH] Old bbport.exe terminated.")
        return False

    def launch_game(self):
        if self.worker and self.worker.isRunning():
            return

        target = self.txt_eboot.text().strip()
        if not target:
            self.update_path_state()
            self.log("[PRE-LAUNCH] Select eboot.bin or the game folder first.")
            self.txt_eboot.setFocus()
            return

        game_dir = resolve_game_dir(target)
        if not game_dir:
            self.update_path_state()
            self.log(f"[PRE-LAUNCH] eboot.bin not found in: {target}")
            self.txt_eboot.setFocus()
            return

        if self.blocked_by_running_instance():
            return

        self.save_settings()
        self.save_bbport_ini()

        cfg = self.current_config()
        ready, err, details = verify_prelaunch(game_dir)
        if not ready:
            self.refresh_setup_status()
            for line in details or []:
                self.log(f"  {line}")
            first = next((line.strip() for line in str(err or "").splitlines() if line.strip()), "Setup is not ready.")
            self.log(f"[PRE-LAUNCH ERROR] {first}")
            return

        env = os.environ.copy()
        if not getattr(sys, "frozen", False):
            env["PYTHON"] = sys.executable
        env["BB_GAME_DIR"] = str(game_dir)
        fps_rate = cfg["fps"]
        # 30 is the native PS4 path in both modes: no FPS patch and no live clock hook.
        live_fps = bool(cfg["feat_fps_patch"]) and fps_rate != "30"
        env["BB_FPS"] = fps_rate
        env["BB_LIVE_FPS"] = "1" if live_fps else "0"
        env["BB_MODS_ENABLED"] = "1" if cfg["mods"] else "0"

        aa_mode = cfg.get("anti_aliasing", "Native FXAA (PS4 Default)")
        upscaler_choice = cfg.get("upscaler_model", "Off")
        upscaler_map = {label: value for label, value in UPSCALER_CHOICES}
        if "TAA" in aa_mode:
            env["BB_UPSCALER"] = "taa"
        elif not cfg.get("feat_upscaler", False) or upscaler_choice == "Off":
            env["BB_UPSCALER"] = "off"
        else:
            env["BB_UPSCALER"] = upscaler_map.get(upscaler_choice, "fsr31")

        # In-game overlay configuration menu is permanently enabled
        env.pop("BB_OVERLAY", None)

        # patches.py uses BB_FPS_PATCH=0 as "apply no frame-rate patch at all". It must NOT be
        # derived from the live checkbox: with live off, the static preset ("60/90/Uncap (XML
        # patch)") still has to be applied, and tying the two dropped it, so the game fell back
        # to the native 30 FPS. The "30 (Native PS4)" preset already carries no frame-rate patch
        # on its own, so the launcher never needs to set this switch.
        env.pop("BB_FPS_PATCH", None)

        env.pop("BB_RENDER_RES", None)
        env.pop("BB_OUTPUT_RES", None)
        res_choice = cfg["res"]
        output_res_str = "1920x1080"
        for candidate in ("1280x720", "1920x1080", "2560x1440", "3840x2160"):
            if candidate in res_choice:
                output_res_str = candidate
                break
        env["BB_OUTPUT_RES"] = output_res_str

        win_mode_val = cfg["window_mode"].lower()
        env["BB_WINDOW_MODE"] = win_mode_val
        env["BB_FULLSCREEN"] = "1" if win_mode_val != "windowed" else "0"
        frames_ahead = str(cfg.get("frames_ahead", "")).strip()
        env["BB_FRAMES_AHEAD"] = frames_ahead or "2"
        vblank = str(cfg.get("vblank_hz", "")).strip()
        if live_fps:
            # The live clock owns the frame rate; the presenter loop must not be capped by a
            # static vblank. 0 selects the 480 Hz live base in run.bat and emulator_settings.
            env["BB_VBLANK_HZ"] = "0"
        elif vblank:
            env["BB_VBLANK_HZ"] = vblank
        else:
            env.pop("BB_VBLANK_HZ", None)

        aniso_mapping = {c[0]: c[1] for c in ANISO_CHOICES}
        env["BB_ANISO"] = aniso_mapping.get(cfg["aniso"], "16")

        prep_mapping = {c[0]: c[1] for c in PREP_WORKER_CHOICES}
        prep_val = prep_mapping.get(cfg.get("prep_workers"), "auto")
        if prep_val and prep_val != "auto":
            env["BB_PREP_WORKERS"] = prep_val
        else:
            env.pop("BB_PREP_WORKERS", None)

        chosen_gpu = cfg.get("gpu", "")
        if chosen_gpu and "BB_GPU_ID" not in os.environ:
            gpu_idx = next((idx for idx, _vendor, _kind, name, _best in connected_gpus(APP_DIR) if name == chosen_gpu), None)
            if gpu_idx is not None:
                env["BB_GPU_ID"] = str(gpu_idx)
            else:
                env.pop("BB_GPU_ID", None)

        lang_code = cfg.get("language", "1")
        if lang_code:
            env["BB_LANGUAGE"] = str(lang_code)

        for cfg_key, env_key in (
            ("timeout", "BB_TIMEOUT"),
            ("dmem_mb", "BB_DMEM_MB"),
            ("user_dir", "BB_USER_DIR"),
            ("mods_dir", "BB_MODS_DIR"),
            ("mods_config", "BB_MODS_CONFIG"),
            ("patches_dir", "BB_PATCHES_DIR"),
            ("patches_config", "BB_PATCHES_CONFIG"),
            ("sku", "BB_CONTENT_SKU"),
        ):
            value = str(cfg.get(cfg_key, "")).strip()
            if value:
                env[env_key] = value
            else:
                env.pop(env_key, None)

        active_patches = list(cfg["enabled_patches"])
        if live_fps:
            active_patches = [p for p in active_patches if p not in LIVE_FPS_CONFLICTING_PATCHES]
        if ("Disable" in aa_mode or "TAA" in aa_mode) and "Disable AA" not in active_patches:
            active_patches.append("Disable AA")

        # scripts/patches.py:312 adds the Intel tonemap patch on every Intel CPU regardless of
        # BB_PATCHES, so its UI checkbox cannot turn it off; BB_INTEL_TONEMAP_FIX (patches.py:68)
        # is the only switch. Keep the documented default (on for Intel) and let an explicit
        # environment value win.
        if "BB_INTEL_TONEMAP_FIX" not in os.environ:
            intel = "Intel" in os.environ.get("PROCESSOR_IDENTIFIER", "")
            wanted = "Intel Black Tonemap Fix" in active_patches or intel
            env["BB_INTEL_TONEMAP_FIX"] = "1" if wanted else "0"
            if wanted and "Intel Black Tonemap Fix" not in active_patches:
                active_patches.append("Intel Black Tonemap Fix")

        if active_patches:
            env["BB_PATCHES"] = ";".join(active_patches)
        else:
            env.pop("BB_PATCHES", None)

        rotate_log()
        try:
            self.log_fh = open(LOG_FILE, "w", encoding="utf-8")
        except OSError:
            self.log_fh = None

        self.hidden_count = 0
        self.log_batch([
            "=" * 60,
            f"Starting Bloodborne from: {game_dir}  [{datetime.now():%Y-%m-%d %H:%M:%S}]",
            f"Target FPS: {cfg['fps']} | Res: {res_choice} | Window: {win_mode_val}",
            f"Active XML Patches ({len(active_patches)}): {', '.join(active_patches) if active_patches else 'None'}",
            "=" * 60,
        ])

        extra_args = str(cfg.get("extra_args", "")).strip()
        if os.name == "nt":
            bat_cmd = RUN_BAT.name + (f" {extra_args}" if extra_args else "")
            cmd = ["cmd.exe", "/d", "/c", bat_cmd]
        else:
            cmd = [str(RUN_BAT)]

        self.save_settings()
        self.save_bbport_ini()

        self._stop_requested = False
        self._run_started = time.monotonic()
        self.run_timer.start()
        self.update_run_clock()

        self.worker = ProcessWorker(cmd, env, APP_DIR)
        self.worker.batch_emitted.connect(self.log_batch)
        self.worker.process_finished.connect(self.on_process_finished)
        self.worker.start()

        self.btn_launch.setEnabled(False)
        self.btn_stop.setEnabled(True)
        self.lbl_status.setText("\u25cf Game Running")
        self.lbl_status.setStyleSheet("color: #4ade80; font-size: 11px; padding: 0 20px; background: transparent;")

    def stop_game(self):
        if self._stop_requested:
            return
        self._stop_requested = True
        # Give immediate feedback and make a second click impossible while the tree dies.
        self.btn_stop.setEnabled(False)
        self.log("Force-terminating the game process tree...")
        self._set_run_info("warn", "\u25cf Stopping...")

        worker = self.worker

        def force_stop():
            # taskkill /T needs the cmd.exe wrapper; the bb_guard sweep catches a game
            # process even when the wrapper is already gone.
            if worker is not None:
                worker.terminate_proc()
            else:
                kill_tree(None)

        # Killing a tree can take a moment (taskkill + guard).  Keep the UI responsive and
        # let the worker's process_finished signal finish the status reset.
        threading.Thread(target=force_stop, name="bbport-force-stop", daemon=True).start()

    def on_process_finished(self, rc: int):
        duration = 0
        if self._run_started is not None:
            duration = max(0, int(time.monotonic() - self._run_started))
        self._run_started = None
        self.run_timer.stop()

        self.settings["last_run_time"] = datetime.now().isoformat(timespec="seconds")
        self.settings["last_run_rc"] = rc
        self.settings["last_run_duration"] = duration

        if self.hidden_count:
            self.log(f"({self.hidden_count} console lines hidden by the active filter)")
        self.log(f"Process exited with code: {rc}")

        if self._stop_requested:
            self._stop_requested = False
            # Final sweep for a game process that outlived its cmd.exe wrapper.
            kill_tree(None)
            self.log("[STOP] Process tree terminated.")

        guard = get_bb_guard()
        if guard:
            for instance in running_instances():
                self.log(
                    f"[WARNING] bbport.exe is still running after the game ended "
                    f"({guard.describe(instance)}); the next launch will ask to terminate it."
                )

        if self.log_fh:
            try:
                self.log_fh.close()
            except OSError:
                pass
            self.log_fh = None

        self.btn_launch.setEnabled(self._path_valid)
        self.btn_stop.setEnabled(False)
        self.lbl_status.setText("\u25cf Engine Idle")
        self.lbl_status.setStyleSheet("color: #6a6a78; font-size: 11px; padding: 0 20px; background: transparent;")
        if self._path_valid and self._setup_kind in ("warn", "error"):
            label = "\u25cf Ready (warnings)" if self._setup_kind == "warn" else "\u25cf Setup needs attention"
            self._set_run_info(self._setup_kind, label)
        elif self._path_valid:
            self._set_run_info("idle", "\u25cf Engine Idle")
        else:
            self._set_run_info("empty", "\u25cf Select a game path")

        self.save_settings()
        self.update_last_run()

    def closeEvent(self, event):
        if self.worker and self.worker.isRunning():
            reply = QMessageBox.question(
                self,
                "Quit Confirmation",
                "Bloodborne is actively executing. Terminate process and exit?",
                QMessageBox.Yes | QMessageBox.No,
            )
            if reply != QMessageBox.Yes:
                event.ignore()
                return
            self._stop_requested = True
            self.worker.terminate_proc()
            # Give the reader thread time to observe EOF before the QThread is destroyed.
            self.worker.wait(5000)
        elif self.worker is not None:
            # The wrapper exited but the game may still be alive.
            self.worker.terminate_proc()
        self.save_settings()
        event.accept()


if __name__ == "__main__":
    if not acquire_single_instance():
        app = QApplication(sys.argv)
        install_dialog_styling(app)
        QMessageBox.information(None, "Bloodborne PC Launcher", "The launcher is already open.")
        sys.exit(0)

    app = QApplication(sys.argv)
    install_dialog_styling(app)
    window = BloodborneLauncher()
    window.show()
    sys.exit(app.exec())