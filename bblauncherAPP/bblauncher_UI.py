#!/usr/bin/env python3
"""User Interface layout, lazy tab builders, and stylesheet for Bloodborne PC Launcher."""

import ctypes
import os
from pathlib import Path
from PySide6.QtCore import QEvent, QObject, Qt, Signal
from PySide6.QtGui import QCursor, QKeyEvent, QMouseEvent
from PySide6.QtWidgets import (
    QAbstractScrollArea,
    QAbstractSlider,
    QAbstractSpinBox,
    QCheckBox,
    QComboBox,
    QDialog,
    QFrame,
    QGridLayout,
    QHBoxLayout,
    QLabel,
    QLineEdit,
    QPlainTextEdit,
    QPushButton,
    QScrollArea,
    QSizePolicy,
    QSlider,
    QStackedWidget,
    QVBoxLayout,
    QWidget,
)

from bblauncher_Utils import (
    AA_CHOICES,
    ANISO_CHOICES,
    ASYNC_PIPELINE_CHOICES,
    CONTENT_SKU_CHOICES,
    DEBUG_VIEW_CHOICES,
    DEFAULT_GAME,
    EFFECT_CHOICES,
    FPS_LIVE_CHOICES,
    FPS_XML_CHOICES,
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
    UPSCALER_CHOICES,
    WINDOW_MODE_CHOICES,
    build_stamp,
    connected_gpus,
    scancode_display_name,
    win_scan_to_scancode_name,
)


class KeybindButton(QPushButton):
    key_assigned = Signal(str, str, str)  # (action_key, stored_binding, previous_storage)

    def __init__(self, action_key: str, binding_text: str, parent=None,
                 keyboard_only=False, display_text=None):
        if display_text is None:
            display_text = scancode_display_name(binding_text)
        super().__init__(display_text, parent)
        self.action_key = action_key
        # current_binding is always the SDL scancode name written to bbport.ini.
        self.current_binding = binding_text
        # display_binding is what the active keyboard layout prints on that key.
        self.display_binding = display_text
        self.listening = False
        self.keyboard_only = keyboard_only
        self._pending_shift = None
        self.setObjectName("KeybindBtn")
        self.setFixedHeight(30)
        self.setFocusPolicy(Qt.StrongFocus)
        if keyboard_only:
            self.setToolTip("Keyboard-only system hotkey: press a key to assign.")
        self.clicked.connect(self.start_listening)
        self.update_style()

    def update_style(self):
        if self.listening:
            self.setText("< Press Key >" if self.keyboard_only else "< Press Key / Click >")
        else:
            self.setText(self.display_binding if self.display_binding else "None")
        self.setProperty("listening", "true" if self.listening else "false")
        self.style().unpolish(self)
        self.style().polish(self)

    def set_binding(self, text: str, display_text=None):
        self.current_binding = text
        self.display_binding = (scancode_display_name(text)
                                if display_text is None else display_text)
        self.listening = False
        self.update_style()

    def start_listening(self):
        self.listening = True
        self._pending_shift = None
        self.update_style()
        self.grabKeyboard()
        self.grabMouse()

    def stop_listening(self):
        if self.listening:
            self.listening = False
            self.releaseKeyboard()
            self.releaseMouse()
            self.update_style()

    def keyPressEvent(self, event: QKeyEvent):
        if not self.listening:
            super().keyPressEvent(event)
            return

        key = event.key()

        # Shift alone binds on release; Shift held through a click becomes a mouse chord instead.
        if key == Qt.Key_Shift:
            self._pending_shift = "Right Shift" if event.nativeScanCode() == 0x36 else "Left Shift"
            return
        if key == Qt.Key_Meta:
            return

        if key == Qt.Key_Escape:
            self.stop_listening()
            return
        if key == Qt.Key_Delete:
            previous = self.current_binding
            self.stop_listening()
            self.current_binding = "None"
            self.display_binding = "None"
            self.update_style()
            self.key_assigned.emit(self.action_key, "None", previous)
            return

        name, label = self._event_to_binding(event)
        if name == "Unknown":
            return
        previous = self.current_binding
        self.stop_listening()
        self.current_binding = name
        self.display_binding = label
        self.update_style()
        self.key_assigned.emit(self.action_key, name, previous)

    def keyReleaseEvent(self, event: QKeyEvent):
        if self.listening and event.key() == Qt.Key_Shift and not event.isAutoRepeat() and self._pending_shift:
            name = self._pending_shift
            previous = self.current_binding
            self.stop_listening()
            self.current_binding = name
            self.display_binding = name
            self.update_style()
            self.key_assigned.emit(self.action_key, name, previous)
            return
        super().keyReleaseEvent(event)

    def mousePressEvent(self, event: QMouseEvent):
        if not self.listening:
            super().mousePressEvent(event)
            return

        # Overlay/HUD/cheat/capture hotkeys are keyboard-only.  Keep listening instead of
        # turning a stray click into a binding.
        if self.keyboard_only:
            return

        self._pending_shift = None
        btn = event.button()
        modifiers = event.modifiers()
        is_shift = bool(modifiers & Qt.ShiftModifier)

        btn_map = {
            Qt.LeftButton: "Shift + Mouse Left" if is_shift else "Mouse Left",
            Qt.RightButton: "Shift + Mouse Right" if is_shift else "Mouse Right",
            Qt.MiddleButton: "Mouse Middle",
            Qt.BackButton: "Mouse X1",
            Qt.ForwardButton: "Mouse X2",
        }

        name = btn_map.get(btn, None)
        if name:
            previous = self.current_binding
            self.stop_listening()
            self.current_binding = name
            self.display_binding = name
            self.update_style()
            self.key_assigned.emit(self.action_key, name, previous)
        else:
            self.stop_listening()

    def _event_to_binding(self, event: QKeyEvent):
        """Return (storage_name, display_label) for a key press.

        Storage uses the physical SDL scancode name, so AZERTY and QWERTY bind the
        same key positions.  The label is looked up through the active layout.
        """
        key = event.key()
        scan_code = int(event.nativeScanCode()) & 0xFF
        stored = win_scan_to_scancode_name(scan_code)
        text = event.text()
        if stored and (Qt.Key_A <= key <= Qt.Key_Z
                       or Qt.Key_0 <= key <= Qt.Key_9
                       or (len(text) == 1 and text.isprintable())):
            return stored, scancode_display_name(stored)

        name = self._qt_key_to_name(key)
        if name != "Unknown":
            return name, name
        return "Unknown", ""

    def _qt_key_to_name(self, key: int) -> str:
        key_map = {
            Qt.Key_Space: "Space",
            Qt.Key_Return: "Return",
            Qt.Key_Enter: "Return",
            Qt.Key_Tab: "Tab",
            Qt.Key_Backspace: "Backspace",
            Qt.Key_Left: "Left",
            Qt.Key_Right: "Right",
            Qt.Key_Up: "Up",
            Qt.Key_Down: "Down",
            Qt.Key_PageUp: "PageUp",
            Qt.Key_PageDown: "PageDown",
            Qt.Key_Home: "Home",
            Qt.Key_End: "End",
            Qt.Key_Insert: "Insert",
            Qt.Key_CapsLock: "CapsLock",
            Qt.Key_NumLock: "Numlock",
            Qt.Key_Print: "PrintScreen",
            Qt.Key_ScrollLock: "ScrollLock",
            Qt.Key_Pause: "Pause",
            Qt.Key_Alt: "Left Alt",
            Qt.Key_Control: "Left Ctrl",
        }
        if key in key_map:
            return key_map[key]
        if Qt.Key_F1 <= key <= Qt.Key_F24:
            return f"F{key - Qt.Key_F1 + 1}"
        if Qt.Key_A <= key <= Qt.Key_Z:
            return chr(key)
        if Qt.Key_0 <= key <= Qt.Key_9:
            return chr(key)
        return "Unknown"




class PathDropEdit(QLineEdit):
    """Path field that accepts a dropped eboot.bin, game folder, or plain path text."""

    path_dropped = Signal(str)

    def __init__(self, text: str = "", parent=None):
        super().__init__(text, parent)
        self.setAcceptDrops(True)

    @staticmethod
    def _accepted(mime) -> bool:
        return mime.hasUrls() or mime.hasText()

    def dragEnterEvent(self, event):
        if self._accepted(event.mimeData()):
            event.acceptProposedAction()
        else:
            super().dragEnterEvent(event)

    def dragMoveEvent(self, event):
        if self._accepted(event.mimeData()):
            event.acceptProposedAction()
        else:
            super().dragMoveEvent(event)

    def dropEvent(self, event):
        mime = event.mimeData()
        if mime.hasUrls():
            for url in mime.urls():
                if url.isLocalFile():
                    self.path_dropped.emit(url.toLocalFile())
                    event.acceptProposedAction()
                    return
        if mime.hasText():
            self.path_dropped.emit(mime.text().strip())
            event.acceptProposedAction()
            return
        super().dropEvent(event)


class NoWheelSlider(QSlider):
    """Slider that leaves wheel scrolling to the surrounding page."""

    def wheelEvent(self, event):
        event.ignore()


class WheelGuard(QObject):
    """Keep mouse-wheel scrolling from changing settings controls.

    Qt lets QComboBox/QSlider/QAbstractSpinBox consume wheel events even when
    the cursor merely happens to be over them, which silently changes values
    while the user is scrolling a settings page.  Forward those events to the
    nearest scroll area so the page scrolls instead.
    """

    VALUE_WIDGETS = (QComboBox, QAbstractSlider, QAbstractSpinBox)

    def eventFilter(self, obj, event):
        if event.type() != QEvent.Wheel or not isinstance(obj, self.VALUE_WIDGETS):
            return False

        parent = obj.parentWidget()
        while parent is not None and not isinstance(parent, QAbstractScrollArea):
            parent = parent.parentWidget()
        if parent is not None:
            delta = event.pixelDelta().y()
            if not delta:
                delta = int(event.angleDelta().y() / 120 * 60)
            if delta:
                bar = parent.verticalScrollBar()
                bar.setValue(bar.value() - delta)
        return True



def apply_dwm_styling(hwnd: int):
    if os.name != "nt" or not hwnd:
        return
    try:
        dwmapi = ctypes.windll.dwmapi
        dark = ctypes.c_int(1)
        dwmapi.DwmSetWindowAttribute(hwnd, 20, ctypes.byref(dark), ctypes.sizeof(dark))
        corner = ctypes.c_int(2)
        dwmapi.DwmSetWindowAttribute(hwnd, 33, ctypes.byref(corner), ctypes.sizeof(corner))
        caption_color = ctypes.c_int(0x00131111)
        dwmapi.DwmSetWindowAttribute(hwnd, 35, ctypes.byref(caption_color), ctypes.sizeof(caption_color))
    except Exception:
        pass


def hide_title_icon(hwnd: int):
    """Remove the (blank) icon from a native title bar by setting WS_EX_DLGMODALFRAME."""
    if os.name != "nt" or not hwnd:
        return
    try:
        from ctypes import wintypes

        user32 = ctypes.WinDLL("user32", use_last_error=True)
        get_long = getattr(user32, "GetWindowLongPtrW", None) or user32.GetWindowLongW
        set_long = getattr(user32, "SetWindowLongPtrW", None) or user32.SetWindowLongW
        get_long.restype = ctypes.c_ssize_t
        get_long.argtypes = [wintypes.HWND, ctypes.c_int]
        set_long.restype = ctypes.c_ssize_t
        set_long.argtypes = [wintypes.HWND, ctypes.c_int, ctypes.c_ssize_t]
        user32.SetWindowPos.argtypes = [
            wintypes.HWND, wintypes.HWND,
            ctypes.c_int, ctypes.c_int, ctypes.c_int, ctypes.c_int, ctypes.c_uint,
        ]

        GWL_EXSTYLE = -20
        WS_EX_DLGMODALFRAME = 0x00000001
        # SWP_NOSIZE | SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED
        flags = 0x0001 | 0x0002 | 0x0004 | 0x0010 | 0x0020

        hwnd = wintypes.HWND(hwnd)
        ex = get_long(hwnd, GWL_EXSTYLE)
        set_long(hwnd, GWL_EXSTYLE, ex | WS_EX_DLGMODALFRAME)
        user32.SetWindowPos(hwnd, None, 0, 0, 0, 0, flags)
    except Exception:
        pass


class DialogStyler(QObject):
    """Applies the dark, icon-free title bar to every dialog (QMessageBox etc.) when it is first shown."""

    def eventFilter(self, obj, event):
        if event.type() == QEvent.Show and isinstance(obj, QDialog) and not obj.property("_dwm_styled"):
            obj.setProperty("_dwm_styled", True)
            hwnd = int(obj.winId())
            hide_title_icon(hwnd)
            apply_dwm_styling(hwnd)
        return False


def install_dialog_styling(app):
    app.setStyleSheet(STYLE_SHEET)
    styler = DialogStyler(app)
    app.installEventFilter(styler)
    app._dialog_styler = styler


STYLE_SHEET = """
QMainWindow, QStackedWidget, QDialog {
    background-color: #111113;
}
QFrame#MainContainer {
    background-color: #111113;
    border: 1px solid #23232a;
}
QFrame#TitleBar {
    background-color: #111113;
    border-bottom: 1px solid #1a1a20;
}
QLabel#TitleBarText {
    color: #6a6a78;
    font-size: 11px;
    font-weight: 600;
    background: transparent;
}
QPushButton#WinBtn, QPushButton#WinCloseBtn {
    background-color: transparent;
    color: #92929e;
    font-family: "Segoe UI Symbol", "Segoe UI", sans-serif;
    font-size: 12px;
    border: none;
    border-radius: 0px;
}
QPushButton#WinBtn:hover {
    background-color: #22222a;
    color: #ffffff;
}
QPushButton#WinCloseBtn:hover {
    background-color: #cc161e;
    color: #ffffff;
}
QWidget {
    color: #e0e0e4;
    font-family: "Segoe UI", sans-serif;
    font-size: 13px;
}
QScrollArea, QScrollArea > QWidget > QWidget, QWidget#ScrollContent, QWidget#HomePage {
    background-color: #111113;
    border: none;
}
QFrame#Sidebar {
    background-color: #16161a;
    border-right: 1px solid #23232a;
}
QPushButton#NavBtn {
    background-color: transparent;
    color: #92929e;
    font-size: 13px;
    font-weight: 600;
    text-align: left;
    padding: 12px 18px;
    border: none;
    border-radius: 6px;
    margin: 3px 10px;
}
QPushButton#NavBtn:hover {
    background-color: #1f1f26;
    color: #ffffff;
}
QPushButton#NavBtn:checked {
    background-color: #271214;
    color: #e63946;
    border-left: 3px solid #e63946;
}
QFrame#Card {
    background-color: #18181d;
    border: 1px solid #24242d;
    border-radius: 8px;
}
QFrame#Card QLabel, QFrame#Card QCheckBox {
    background-color: transparent;
}
QLabel#HeaderTitle {
    color: #f2f2f5;
    font-size: 19px;
    font-weight: 700;
    background-color: transparent;
}
QLabel#CardTitle {
    color: #d1b87a;
    font-size: 13px;
    font-weight: 600;
    background-color: transparent;
}
QLabel#CardHint {
    color: #92929e;
    font-size: 11px;
    background-color: transparent;
}
QLineEdit, QComboBox {
    background-color: #101014;
    color: #ffffff;
    border: 1px solid #2c2c36;
    border-radius: 5px;
    padding: 6px 10px;
}
QLineEdit:focus, QComboBox:focus {
    border: 1px solid #a31d24;
}
QComboBox::drop-down {
    border: none;
    width: 24px;
}
QComboBox QAbstractItemView {
    background-color: #16161b;
    border: 1px solid #2c2c36;
    color: #ffffff;
    selection-background-color: #8b1116;
}
QPushButton#PrimaryBtn {
    background-color: #99141a;
    color: #ffffff;
    font-size: 13px;
    font-weight: 700;
    padding: 10px 18px;
    border: none;
    border-radius: 6px;
}
QPushButton#PrimaryBtn:hover {
    background-color: #b51920;
}
QPushButton#PrimaryBtn:disabled {
    background-color: #3d1416;
    color: #7a5052;
}
QPushButton#SecondaryBtn {
    background-color: #22222a;
    color: #dcdce0;
    font-weight: 600;
    padding: 7px 14px;
    border: 1px solid #2e2e38;
    border-radius: 5px;
}
QPushButton#SecondaryBtn:hover {
    background-color: #2d2d38;
    color: #ffffff;
}
QPushButton#StopBtn {
    background-color: #361718;
    color: #e65555;
    font-weight: 700;
    border: 1px solid #542224;
    border-radius: 6px;
    padding: 10px 18px;
}
QPushButton#StopBtn:hover {
    background-color: #4a1d1f;
}
QPushButton#StopBtn:disabled {
    background-color: #1a1a1e;
    color: #44444c;
    border-color: #222228;
}
QPushButton#ChipBtn {
    background-color: #1b1b22;
    color: #b8b8c4;
    font-size: 11px;
    font-weight: 600;
    padding: 4px 10px;
    border: 1px solid #2c2c36;
    border-radius: 10px;
}
QPushButton#ChipBtn:hover {
    background-color: #24242d;
    border-color: #555562;
    color: #ffffff;
}
QLabel#ChipCaption {
    color: #656573;
    font-size: 10px;
    font-weight: 700;
    letter-spacing: 1px;
    background-color: transparent;
}
QLabel#PathHint {
    color: #7c7c88;
    font-size: 11px;
    background-color: transparent;
}
QLabel#RunInfo {
    color: #92929e;
    font-size: 12px;
    font-weight: 600;
    background-color: transparent;
}
QLabel#LastRun {
    color: #92929e;
    font-size: 11px;
    background-color: transparent;
}
QLabel#VersionLabel {
    color: #4c4c58;
    font-size: 9px;
    padding: 2px 20px 0 20px;
    background-color: transparent;
}
QPushButton#LinkBtn {
    background-color: transparent;
    color: #d1b87a;
    font-size: 11px;
    font-weight: 600;
    border: none;
    padding: 2px 4px;
}
QPushButton#LinkBtn:hover {
    color: #f0d99a;
}
QPushButton#ConsoleToggle {
    background-color: transparent;
    color: #d1b87a;
    font-size: 13px;
    font-weight: 600;
    text-align: left;
    border: none;
    padding: 0px;
}
QPushButton#ConsoleToggle:hover {
    color: #f0d99a;
}
QFrame#Card QLabel#SetupDetails {
    color: #b0b0ba;
    font-family: "Consolas", monospace;
    font-size: 10px;
    background-color: #101014;
    border: 1px solid #24242d;
    border-radius: 5px;
    padding: 8px;
}
QCheckBox#LogChk::indicator:checked {
    background-color: #3a6a4c;
    border: 2px solid #4ade80;
}
QPushButton#KeybindBtn {
    background-color: #1a1a24;
    border: 1px solid #363645;
    border-radius: 4px;
    color: #d1b87a;
    font-weight: 600;
    font-size: 12px;
    padding: 0 10px;
}
QPushButton#KeybindBtn:hover {
    background-color: #242433;
    border-color: #d1b87a;
}
QPushButton#KeybindBtn[listening="true"] {
    background-color: #2b2510;
    border: 2px solid #d1b87a;
    border-radius: 4px;
    color: #ffd700;
    font-weight: 700;
    font-size: 12px;
}
QMessageBox, QDialog {
    background-color: #111113;
}
QMessageBox QLabel {
    background-color: transparent;
    color: #e0e0e4;
}
QMessageBox QPushButton, QDialogButtonBox QPushButton {
    background-color: #22222a;
    color: #dcdce0;
    font-weight: 600;
    padding: 7px 14px;
    min-width: 76px;
    border: 1px solid #2e2e38;
    border-radius: 5px;
}
QMessageBox QPushButton:hover, QDialogButtonBox QPushButton:hover {
    background-color: #2d2d38;
    color: #ffffff;
}
QMessageBox QPushButton:pressed, QDialogButtonBox QPushButton:pressed {
    background-color: #1a1a21;
}
QMessageBox QPushButton:default, QMessageBox QPushButton:focus,
QDialogButtonBox QPushButton:default, QDialogButtonBox QPushButton:focus {
    border: 1px solid #a31d24;
}
QCheckBox {
    spacing: 8px;
    background-color: transparent;
}
QCheckBox::indicator {
    width: 16px;
    height: 16px;
    border: 2px solid #8c8c9a;
    border-radius: 4px;
    background-color: #000000;
}
QCheckBox::indicator:hover {
    border: 2px solid #d1b87a;
}
QCheckBox::indicator:checked {
    background-color: #cc161e;
    border: 2px solid #8c8c9a;
}
QCheckBox::indicator:checked:hover {
    background-color: #e01b24;
    border: 2px solid #d1b87a;
}
QSlider {
    background-color: transparent;
}
QSlider::groove:horizontal {
    height: 4px;
    background: #25252e;
    border-radius: 2px;
}
QSlider::sub-page:horizontal {
    background: #99141a;
    border-radius: 2px;
}
QSlider::handle:horizontal {
    background: #ffffff;
    width: 14px;
    height: 14px;
    margin-top: -5px;
    margin-bottom: -5px;
    border-radius: 7px;
}
QPlainTextEdit {
    background-color: #0b0b0d;
    color: #b0b0b8;
    font-family: "Consolas", monospace;
    font-size: 11px;
    border: 1px solid #1f1f26;
    border-radius: 6px;
}
QScrollBar:vertical {
    border: none;
    background: #111113;
    width: 8px;
    margin: 0px;
}
QScrollBar::handle:vertical {
    background: #2c2c36;
    min-height: 24px;
    border-radius: 4px;
}
QScrollBar::handle:vertical:hover {
    background: #424250;
}
QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical {
    height: 0px;
    background: none;
}
QScrollBar::add-page:vertical, QScrollBar::sub-page:vertical {
    background: none;
}
"""


class LauncherUI:
    def init_ui(self):
        self.setWindowFlags(
            Qt.FramelessWindowHint
            | Qt.Window
            | Qt.WindowMinimizeButtonHint
        )

        central = QFrame()
        central.setObjectName("MainContainer")
        self.setCentralWidget(central)

        root_layout = QHBoxLayout(central)
        root_layout.setContentsMargins(0, 0, 0, 0)
        root_layout.setSpacing(0)

        # ----------------- SIDEBAR -----------------
        self.sidebar = QFrame()
        self.sidebar.setObjectName("Sidebar")
        self.sidebar.setFixedWidth(220)
        sb_layout = QVBoxLayout(self.sidebar)
        sb_layout.setContentsMargins(0, 18, 0, 16)
        sb_layout.setSpacing(4)

        logo_title = QLabel("BB LAUNCHER")
        logo_title.setStyleSheet(
            "color: #d1b87a; font-size: 15px; font-weight: 800; letter-spacing: 2px; padding: 0 20px; background: transparent;"
        )
        logo_sub = QLabel("WIN32 / VULKAN PORT")
        logo_sub.setStyleSheet(
            "color: #656573; font-size: 10px; font-weight: 600; padding: 0 20px 18px 20px; background: transparent;"
        )
        sb_layout.addWidget(logo_title)
        sb_layout.addWidget(logo_sub)

        self.btn_home = QPushButton("  HOME")
        self.btn_patches = QPushButton("  PATCHES")
        self.btn_display = QPushButton("  DISPLAY && GRAPHICS")
        self.btn_mk = QPushButton("  MOUSE && KEYBOARD")
        self.btn_advanced = QPushButton("  ADVANCED SETTINGS")

        self.nav_buttons = [
            self.btn_home,
            self.btn_patches,
            self.btn_display,
            self.btn_mk,
            self.btn_advanced,
        ]
        for b in self.nav_buttons:
            b.setObjectName("NavBtn")
            b.setCheckable(True)
            sb_layout.addWidget(b)

        self.btn_home.setChecked(True)
        self.btn_home.clicked.connect(lambda: self.switch_tab(0))
        self.btn_patches.clicked.connect(lambda: self.switch_tab(1))
        self.btn_display.clicked.connect(lambda: self.switch_tab(2))
        self.btn_mk.clicked.connect(lambda: self.switch_tab(3))
        self.btn_advanced.clicked.connect(lambda: self.switch_tab(4))

        sb_layout.addStretch()

        self.lbl_status = QLabel("\u25cf Engine Idle")
        self.lbl_status.setStyleSheet("color: #6a6a78; font-size: 11px; padding: 0 20px; background: transparent;")
        sb_layout.addWidget(self.lbl_status)

        build = build_stamp()
        version_text = f"v{LAUNCHER_VERSION}" + (f" \u00b7 build {build}" if build else "")
        self.lbl_version = QLabel(version_text)
        self.lbl_version.setObjectName("VersionLabel")
        sb_layout.addWidget(self.lbl_version)

        root_layout.addWidget(self.sidebar)

        # ----------------- RIGHT PANEL (TITLE BAR + STACK) -----------------
        right_panel = QWidget()
        right_layout = QVBoxLayout(right_panel)
        right_layout.setContentsMargins(0, 0, 0, 0)
        right_layout.setSpacing(0)

        self.title_bar = QFrame()
        self.title_bar.setObjectName("TitleBar")
        self.title_bar.setFixedHeight(36)
        tb_layout = QHBoxLayout(self.title_bar)
        tb_layout.setContentsMargins(24, 0, 0, 0)
        tb_layout.setSpacing(0)

        lbl_win_title = QLabel(
            f"Bloodborne PC Windows Launcher   \u00b7   {version_text}"
        )
        lbl_win_title.setObjectName("TitleBarText")
        tb_layout.addWidget(lbl_win_title)
        tb_layout.addStretch()

        btn_min = QPushButton("\u2212")
        btn_min.setObjectName("WinBtn")
        btn_min.setFixedSize(46, 36)
        btn_min.clicked.connect(self.showMinimized)
        tb_layout.addWidget(btn_min)

        btn_close = QPushButton("\u2715")
        btn_close.setObjectName("WinCloseBtn")
        btn_close.setFixedSize(46, 36)
        btn_close.clicked.connect(self.close)
        tb_layout.addWidget(btn_close)

        right_layout.addWidget(self.title_bar)

        self.stack = QStackedWidget()
        self.stack.addWidget(self.create_home_tab())
        self.patches_placeholder = QWidget()
        self.display_placeholder = QWidget()
        self.mk_placeholder = QWidget()
        self.advanced_placeholder = QWidget()
        self.stack.addWidget(self.patches_placeholder)
        self.stack.addWidget(self.display_placeholder)
        self.stack.addWidget(self.mk_placeholder)
        self.stack.addWidget(self.advanced_placeholder)
        right_layout.addWidget(self.stack, 1)

        root_layout.addWidget(right_panel, 1)

    def _in_drag_zone(self, pos) -> bool:
        if pos.x() < self.sidebar.width():
            return pos.y() <= 72  # sidebar logo area, above the nav buttons
        return pos.y() <= self.title_bar.height()

    def mousePressEvent(self, event):
        if event.button() == Qt.LeftButton and self.windowHandle():
            pos = event.position().toPoint()
            if self._in_drag_zone(pos):
                self.windowHandle().startSystemMove()
                event.accept()
                return
        super().mousePressEvent(event)

    def switch_tab(self, idx: int):
        if idx == 1:
            if hasattr(self, "_build_patches_tab_now"):
                self._build_patches_tab_now()
            if hasattr(self, "_sync_external_patch_settings"):
                self._sync_external_patch_settings()
            if hasattr(self, "_sync_patches_from_ini"):
                self._sync_patches_from_ini()
        elif idx == 2 and hasattr(self, "_build_display_tab_now"):
            self._build_display_tab_now()
        elif idx == 3 and hasattr(self, "_build_mk_tab_now"):
            self._build_mk_tab_now()
        elif idx == 4 and hasattr(self, "_build_advanced_tab_now"):
            self._build_advanced_tab_now()
        for i, btn in enumerate(self.nav_buttons):
            btn.setChecked(i == idx)
        self.stack.setCurrentIndex(idx)
        if idx == 0 and hasattr(self, "update_home_summary"):
            self.update_home_summary()

    def create_home_tab(self) -> QWidget:
        page = QWidget()
        page.setObjectName("HomePage")
        layout = QVBoxLayout(page)
        layout.setContentsMargins(24, 14, 24, 16)
        layout.setSpacing(8)

        # HOME is a fixed dashboard with no scroll area. Its last card stretches
        # into the remaining space so the console is always visible.
        # ---- Game path ----
        card_exec = QFrame()
        card_exec.setObjectName("Card")
        l_exec = QVBoxLayout(card_exec)
        l_exec.setContentsMargins(16, 10, 16, 8)
        l_exec.setSpacing(6)

        row_title = QHBoxLayout()
        row_title.setSpacing(6)
        lbl = QLabel("Bloodborne Game Path")
        lbl.setObjectName("CardTitle")
        row_title.addWidget(lbl)

        self.lbl_path_status = QLabel("\u2022")
        self.lbl_path_status.setFixedWidth(16)
        self.lbl_path_status.setStyleSheet(
            "color: #6a6a78; font-size: 14px; font-weight: 700; background: transparent;"
        )
        row_title.addWidget(self.lbl_path_status)
        row_title.addStretch()
        l_exec.addLayout(row_title)

        row_exec = QHBoxLayout()
        row_exec.setSpacing(8)
        init_eboot = self.settings.get("eboot", "")
        if not init_eboot or not Path(init_eboot).exists():
            if (DEFAULT_GAME / "eboot.bin").exists():
                init_eboot = str(DEFAULT_GAME / "eboot.bin")
            elif (DEFAULT_GAME / "eboot.elf").exists():
                init_eboot = str(DEFAULT_GAME / "eboot.elf")
            else:
                init_eboot = ""
        self.txt_eboot = PathDropEdit(init_eboot)
        self.txt_eboot.setPlaceholderText("Path to decrypted eboot.bin, eboot.elf or game folder...")
        self.txt_eboot.setToolTip("Drag and drop eboot.bin, eboot.elf or the game folder here.")
        self.txt_eboot.textChanged.connect(self.on_eboot_path_changed)
        self.txt_eboot.path_dropped.connect(self.on_eboot_path_dropped)
        row_exec.addWidget(self.txt_eboot, 1)

        btn_browse = QPushButton("Browse...")
        btn_browse.setObjectName("SecondaryBtn")
        btn_browse.clicked.connect(self.browse_eboot)
        row_exec.addWidget(btn_browse)

        btn_open_folder = QPushButton("Open Folder")
        btn_open_folder.setObjectName("SecondaryBtn")
        btn_open_folder.setToolTip("Open the resolved game folder.")
        btn_open_folder.clicked.connect(self.open_game_folder)
        row_exec.addWidget(btn_open_folder)
        l_exec.addLayout(row_exec)

        self.lbl_path_hint = QLabel("")
        self.lbl_path_hint.setObjectName("PathHint")
        self.lbl_path_hint.setWordWrap(False)
        self.lbl_path_hint.setSizePolicy(QSizePolicy.Ignored, QSizePolicy.Preferred)
        l_exec.addWidget(self.lbl_path_hint)
        layout.addWidget(card_exec)

        # ---- Setup readiness ----
        card_ready = QFrame()
        card_ready.setObjectName("Card")
        l_ready = QVBoxLayout(card_ready)
        l_ready.setContentsMargins(16, 8, 16, 8)
        l_ready.setSpacing(4)

        row_ready = QHBoxLayout()
        row_ready.setSpacing(8)
        self.lbl_setup_dot = QLabel("\u25cf")
        self.lbl_setup_dot.setFixedWidth(14)
        self.lbl_setup_dot.setStyleSheet(
            "color: #6a6a78; font-size: 13px; background: transparent;"
        )
        row_ready.addWidget(self.lbl_setup_dot)

        self.lbl_setup_summary = QLabel("Checking setup...")
        self.lbl_setup_summary.setStyleSheet(
            "color: #dcdce0; font-size: 12px; font-weight: 600; background: transparent;"
        )
        self.lbl_setup_summary.setSizePolicy(QSizePolicy.Ignored, QSizePolicy.Preferred)
        row_ready.addWidget(self.lbl_setup_summary, 1)

        self.btn_setup_details = QPushButton("Details")
        self.btn_setup_details.setObjectName("LinkBtn")
        self.btn_setup_details.setCursor(QCursor(Qt.PointingHandCursor))
        self.btn_setup_details.clicked.connect(self.show_setup_details)
        self.btn_setup_details.setVisible(False)
        row_ready.addWidget(self.btn_setup_details)

        self.btn_setup_recheck = QPushButton("Re-check")
        self.btn_setup_recheck.setObjectName("SecondaryBtn")
        self.btn_setup_recheck.clicked.connect(self.refresh_setup_status)
        row_ready.addWidget(self.btn_setup_recheck)
        l_ready.addLayout(row_ready)

        row_checks = QHBoxLayout()
        row_checks.setSpacing(16)
        self.lbl_check_game = QLabel("\u00b7 Game files")
        self.lbl_check_vulkan = QLabel("\u00b7 Vulkan / GPU")
        self.lbl_check_avx = QLabel("\u00b7 AVX")
        self.lbl_check_fps = QLabel("\u00b7 FPS patch")
        for check_label in (self.lbl_check_game, self.lbl_check_vulkan, self.lbl_check_avx,
                            self.lbl_check_fps):
            check_label.setObjectName("SetupCheck")
            check_label.setStyleSheet(
                "color: #5a5a66; font-size: 11px; background: transparent;"
            )
            row_checks.addWidget(check_label)
        row_checks.addStretch()
        l_ready.addLayout(row_checks)

        layout.addWidget(card_ready)

        # ---- Launch ----
        row_actions = QHBoxLayout()
        row_actions.setSpacing(10)

        self.btn_launch = QPushButton("\u25b6  LAUNCH BLOODBORNE")
        self.btn_launch.setObjectName("PrimaryBtn")
        self.btn_launch.setFixedHeight(44)
        self.btn_launch.setEnabled(False)
        self.btn_launch.clicked.connect(self.launch_game)
        row_actions.addWidget(self.btn_launch, 1)

        self.btn_stop = QPushButton("\u25a0  Stop")
        self.btn_stop.setObjectName("StopBtn")
        self.btn_stop.setFixedSize(132, 44)
        self.btn_stop.setEnabled(False)
        self.btn_stop.clicked.connect(self.stop_game)
        row_actions.addWidget(self.btn_stop, 0)
        layout.addLayout(row_actions)

        row_run = QHBoxLayout()
        self.lbl_run_info = QLabel("\u25cf Select a game path")
        self.lbl_run_info.setObjectName("RunInfo")
        row_run.addWidget(self.lbl_run_info)
        row_run.addStretch()
        layout.addLayout(row_run)

        # ---- Launch profile chips ----
        row_profile = QHBoxLayout()
        row_profile.setSpacing(6)
        lbl_profile = QLabel("LAUNCH PROFILE")
        lbl_profile.setObjectName("ChipCaption")
        row_profile.addWidget(lbl_profile)

        self.home_chips = {}
        chip_specs = [
            ("fps", 2, "Target framerate - open Display & Graphics"),
            ("res", 2, "Render resolution - open Display & Graphics"),
            ("window", 2, "Window mode - open Display & Graphics"),
            ("upscaler", 2, "Upscaler - open Display & Graphics"),
            ("mk", 3, "Mouse & Keyboard mode"),
            ("patches", 1, "Enabled patches"),
            ("mods", 4, "Mods and advanced settings"),
        ]
        for key, tab, tip in chip_specs:
            chip = QPushButton("\u2014")
            chip.setObjectName("ChipBtn")
            chip.setCursor(QCursor(Qt.PointingHandCursor))
            chip.setToolTip(tip)
            chip.clicked.connect(lambda _checked=False, t=tab: self.switch_tab(t))
            self.home_chips[key] = chip
            row_profile.addWidget(chip)
        row_profile.addStretch()
        layout.addLayout(row_profile)

        # ---- Run status + console (merged, always visible) ----
        card_console = QFrame()
        card_console.setObjectName("Card")
        l_console = QVBoxLayout(card_console)
        l_console.setContentsMargins(14, 8, 14, 10)
        l_console.setSpacing(6)

        row_console_head = QHBoxLayout()
        row_console_head.setSpacing(8)
        lbl_console = QLabel("Engine Console Output")
        lbl_console.setObjectName("CardTitle")
        row_console_head.addWidget(lbl_console)
        row_console_head.addStretch()
        l_console.addLayout(row_console_head)

        self.lbl_last_run = QLabel("No launch recorded yet.")
        self.lbl_last_run.setObjectName("LastRun")
        self.lbl_last_run.setWordWrap(True)
        l_console.addWidget(self.lbl_last_run)

        row_controls = QHBoxLayout()
        row_controls.setSpacing(6)
        self.cmb_log_filter = QComboBox()
        self.cmb_log_filter.addItem("All output", "all")
        self.cmb_log_filter.addItem("Hide Vulkan warnings", "hide_vk")
        self.cmb_log_filter.addItem("Errors & warnings", "errors")
        self.cmb_log_filter.setFixedWidth(180)
        current_filter = self.settings.get("log_filter", "")
        if current_filter not in ("all", "hide_vk", "errors"):
            current_filter = "hide_vk" if self.settings.get("hide_vk", True) else "all"
        filter_idx = self.cmb_log_filter.findData(current_filter)
        self.cmb_log_filter.setCurrentIndex(filter_idx if filter_idx >= 0 else 0)
        self._log_filter = current_filter
        self.cmb_log_filter.currentIndexChanged.connect(self.on_log_filter_changed)
        row_controls.addWidget(self.cmb_log_filter)

        self.chk_autoscroll = QCheckBox("Auto-scroll")
        self.chk_autoscroll.setObjectName("LogChk")
        self.chk_autoscroll.setChecked(bool(self.settings.get("log_autoscroll", True)))
        self.chk_autoscroll.stateChanged.connect(self.on_log_autoscroll_changed)
        row_controls.addWidget(self.chk_autoscroll)
        row_controls.addStretch()

        self.btn_diag = QPushButton("Copy Diagnostics")
        self.btn_diag.setObjectName("SecondaryBtn")
        self.btn_diag.clicked.connect(self.copy_diagnostics)
        row_controls.addWidget(self.btn_diag)

        for label, slot in (
            ("Open Log", self.open_log_file),
            ("Save Log", self.save_log),
            ("Copy", self.copy_log),
            ("Clear", self.clear_log),
        ):
            btn = QPushButton(label)
            btn.setObjectName("SecondaryBtn")
            btn.clicked.connect(slot)
            row_controls.addWidget(btn)
        l_console.addLayout(row_controls)

        self.log_view = QPlainTextEdit()
        self.log_view.setReadOnly(True)
        self.log_view.setMaximumBlockCount(5000)
        self.log_view.setMinimumHeight(110)
        l_console.addWidget(self.log_view, 1)

        layout.addWidget(card_console, 1)
        return page

    def create_patches_tab(self) -> QWidget:
        w = QWidget()
        w.setObjectName("ScrollContent")
        w.setUpdatesEnabled(False)
        layout = QVBoxLayout(w)
        layout.setContentsMargins(24, 16, 24, 20)
        layout.setSpacing(12)

        header = QLabel("Game Engine & Community Patches")
        header.setObjectName("HeaderTitle")
        layout.addWidget(header)

        card_filter = QFrame()
        card_filter.setObjectName("Card")
        l_filter = QHBoxLayout(card_filter)
        l_filter.setContentsMargins(12, 10, 12, 10)
        l_filter.setSpacing(10)

        self.txt_patch_search = QLineEdit()
        self.txt_patch_search.setPlaceholderText("Filter patches by keyword or author...")
        self.txt_patch_search.textChanged.connect(self.filter_patches)
        l_filter.addWidget(self.txt_patch_search, 1)

        self.cmb_patch_cat = QComboBox()
        self.cmb_patch_cat.addItems([
            "All Categories",
            "Performance & Fixes",
            "Visuals & Camera",
            "Gameplay & Cheats",
            "Framerate & Engine",
            "Resolutions & Grids",
            "Debug & Tools",
        ])
        self.cmb_patch_cat.currentTextChanged.connect(self.filter_patches)
        l_filter.addWidget(self.cmb_patch_cat)

        btn_rec = QPushButton("Recommended Defaults")
        btn_rec.setObjectName("SecondaryBtn")
        btn_rec.clicked.connect(self.apply_recommended_patches)
        l_filter.addWidget(btn_rec)

        self.lbl_patch_count = QLabel(f"{len(self.enabled_patches)} / {len(self.all_xml_patches)} enabled")
        self.lbl_patch_count.setStyleSheet("color: #d1b87a; font-weight: 600; padding: 0 4px;")
        l_filter.addWidget(self.lbl_patch_count)

        layout.addWidget(card_filter)

        scroll = QScrollArea()
        scroll.setWidgetResizable(True)
        self.patch_list_widget = QWidget()
        self.patch_list_widget.setObjectName("ScrollContent")
        self.patch_list_layout = QVBoxLayout(self.patch_list_widget)
        self.patch_list_layout.setContentsMargins(0, 0, 6, 0)
        self.patch_list_layout.setSpacing(6)

        for patch in self.all_xml_patches:
            name = patch["name"]
            author = patch["author"]
            cat = patch["category"]
            note = patch["note"]

            card = QFrame()
            card.setObjectName("Card")
            c_layout = QVBoxLayout(card)
            c_layout.setContentsMargins(12, 10, 12, 10)
            c_layout.setSpacing(4)

            top_row = QHBoxLayout()
            cb = QCheckBox(name.replace("&", "&&"))
            cb.setStyleSheet("font-weight: 700; color: #ffffff;")
            is_live = hasattr(self, "chk_fps_patch") and self.chk_fps_patch.isChecked()
            if is_live and name in LIVE_FPS_CONFLICTING_PATCHES:
                cb.setChecked(False)
                cb.setEnabled(False)
                cb.setToolTip("Disabled: Conflicts with Live FPS Patching.")
            else:
                cb.setChecked(name in self.enabled_patches)
            cb.stateChanged.connect(lambda state, p=name: self.on_patch_toggled(p, state))
            top_row.addWidget(cb, 1)

            meta_lbl = QLabel(f'<span style="color:#7b7b88;">by {author}</span> &nbsp;<span style="color:#d1b87a;font-weight:600;">[{cat}]</span>')
            meta_lbl.setTextFormat(Qt.RichText)
            meta_lbl.setStyleSheet("font-size: 11px;")
            top_row.addWidget(meta_lbl)
            c_layout.addLayout(top_row)

            if note:
                lbl_n = QLabel(note)
                lbl_n.setStyleSheet("color: #92929e; font-size: 11px; padding-left: 26px;")
                lbl_n.setWordWrap(True)
                c_layout.addWidget(lbl_n)

            card.setProperty("patch_search_blob", f"{name} {author} {note}".lower())
            card.setProperty("patch_category", cat)

            self.patch_checkboxes[name] = cb
            self.patch_list_layout.addWidget(card)

        self.patch_list_layout.addStretch()
        scroll.setWidget(self.patch_list_widget)
        layout.addWidget(scroll, 1)

        w.setUpdatesEnabled(True)
        return w

    def fill_gpu_choices(self):
        if not hasattr(self, "cmb_gpu"):
            return
        self.cmb_gpu.blockSignals(True)
        self.cmb_gpu.clear()
        gpus = connected_gpus()
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
    # Shared helpers for the settings pages
    # ------------------------------------------------------------------
    def _create_page(self, title: str):
        w = QWidget()
        w.setObjectName("ScrollContent")
        layout = QVBoxLayout(w)
        layout.setContentsMargins(24, 16, 24, 20)
        layout.setSpacing(14)

        header = QLabel(title)
        header.setObjectName("HeaderTitle")
        layout.addWidget(header)

        scroll = QScrollArea()
        scroll.setWidgetResizable(True)
        scroll.setHorizontalScrollBarPolicy(Qt.ScrollBarAlwaysOff)
        container = QWidget()
        container.setObjectName("ScrollContent")
        c_layout = QVBoxLayout(container)
        c_layout.setContentsMargins(0, 0, 6, 0)
        c_layout.setSpacing(14)
        scroll.setWidget(container)
        layout.addWidget(scroll, 1)
        return w, c_layout

    def _new_card(self, title: str, hint: str = ""):
        card = QFrame()
        card.setObjectName("Card")
        layout = QVBoxLayout(card)
        layout.setContentsMargins(16, 14, 16, 14)
        layout.setSpacing(12)
        lbl = QLabel(title)
        lbl.setObjectName("CardTitle")
        layout.addWidget(lbl)
        if hint:
            hint_lbl = QLabel(hint)
            hint_lbl.setObjectName("CardHint")
            hint_lbl.setWordWrap(True)
            layout.addWidget(hint_lbl)
        return card, layout

    def _make_slider(self, minimum: int, maximum: int, value: int):
        slider = NoWheelSlider(Qt.Horizontal)
        slider.setRange(minimum, maximum)
        slider.setValue(max(minimum, min(maximum, value)))
        label = QLabel(f"{slider.value() / 100:.2f}")
        slider.valueChanged.connect(lambda v, l=label: l.setText(f"{v / 100:.2f}"))
        slider.sliderReleased.connect(self.save_bbport_ini)
        return slider, label

    def _add_slider_row(self, grid, row: int, text: str, slider, value_label):
        grid.addWidget(QLabel(text), row, 0)
        grid.addWidget(slider, row, 1)
        grid.addWidget(value_label, row, 2)

    def _save_footer(self, card_layout):
        row = QHBoxLayout()
        row.addStretch()
        btn = QPushButton("Save to bbport.ini")
        btn.setObjectName("SecondaryBtn")
        btn.clicked.connect(self.save_bbport_ini)
        row.addWidget(btn)
        card_layout.addLayout(row)

    def _path_row(self, grid, row: int, text: str, edit: QLineEdit, kind: str):
        grid.addWidget(QLabel(text), row, 0)
        row_layout = QHBoxLayout()
        row_layout.setSpacing(8)
        row_layout.addWidget(edit, 1)
        btn = QPushButton("Browse...")
        btn.setObjectName("SecondaryBtn")
        btn.clicked.connect(lambda: self.browse_path(edit, kind))
        row_layout.addWidget(btn)
        grid.addLayout(row_layout, row, 1, 1, 2)

    def _populate_fps_combo(self, live: bool, selected: str | None = None):
        """Fill the framerate combo for the live/XML mode, keeping the selected rate."""
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

    def _on_fps_patch_toggled(self, checked: bool):
        self._populate_fps_combo(checked)
        if hasattr(self, "_sync_patches_to_live_fps"):
            self._sync_patches_to_live_fps(checked)
        self.save_settings()
        self.save_bbport_ini()

    # ------------------------------------------------------------------
    # DISPLAY & GRAPHICS
    # ------------------------------------------------------------------
    def create_display_tab(self) -> QWidget:
        w, c_layout = self._create_page("Display & Graphics")
        ini_opts = self.load_bbport_ini()

        # ---- Display & render core ----
        card_disp, l_disp = self._new_card("Display & Render Core")

        grid_disp = QGridLayout()
        grid_disp.setHorizontalSpacing(16)
        grid_disp.setVerticalSpacing(10)

        grid_disp.addWidget(QLabel("Target Framerate:"), 0, 0)
        self.cmb_fps = QComboBox()
        self.cmb_fps.setObjectName("FpsCombo")
        grid_disp.addWidget(self.cmb_fps, 0, 1)

        grid_disp.addWidget(QLabel("Render Resolution:"), 0, 2)
        self.cmb_res = QComboBox()
        self.cmb_res.addItems(RES_CHOICES)
        self.cmb_res.setCurrentText(self.settings.get("res", RES_CHOICES[0]))
        self.cmb_res.currentIndexChanged.connect(lambda: (self.save_settings(), self.save_bbport_ini()))
        grid_disp.addWidget(self.cmb_res, 0, 3)

        grid_disp.addWidget(QLabel("Window Mode:"), 1, 0)
        self.cmb_win_mode = QComboBox()
        self.cmb_win_mode.addItems(WINDOW_MODE_CHOICES)
        self.cmb_win_mode.setCurrentText(self.settings.get("window_mode", "Borderless"))
        grid_disp.addWidget(self.cmb_win_mode, 1, 1)

        grid_disp.addWidget(QLabel("Anisotropic Filtering:"), 1, 2)
        self.cmb_aniso = QComboBox()
        for label, _, _ in ANISO_CHOICES:
            self.cmb_aniso.addItem(label)
        self.cmb_aniso.setCurrentText(self.settings.get("aniso", "2x"))
        grid_disp.addWidget(self.cmb_aniso, 1, 3)

        grid_disp.addWidget(QLabel("Anti-Aliasing:"), 2, 0)
        self.cmb_aa = QComboBox()
        self.cmb_aa.addItems(AA_CHOICES)
        self.cmb_aa.setCurrentText(self.settings.get("anti_aliasing", AA_CHOICES[0]))
        self.cmb_aa.currentIndexChanged.connect(lambda: (self.save_settings(), self.save_bbport_ini()))
        grid_disp.addWidget(self.cmb_aa, 2, 1)

        grid_disp.addWidget(QLabel("Draw Prep Workers:"), 2, 2)
        self.cmb_prep_workers = QComboBox()
        for label, _ in PREP_WORKER_CHOICES:
            self.cmb_prep_workers.addItem(label)
        self.cmb_prep_workers.setCurrentText(self.settings.get("prep_workers", PREP_WORKER_CHOICES[0][0]))
        self.cmb_prep_workers.currentIndexChanged.connect(lambda: (self.save_settings(), self.save_bbport_ini()))
        grid_disp.addWidget(self.cmb_prep_workers, 2, 3)

        grid_disp.addWidget(QLabel("Upscaler Technology:"), 3, 0)
        self.cmb_upscaler = QComboBox()
        for label, value in UPSCALER_CHOICES:
            self.cmb_upscaler.addItem(label, value)
        init_upscaler = self.settings.get("upscaler_model", "Off")
        if not self.settings.get("feat_upscaler", False) and init_upscaler != "Off":
            init_upscaler = "Off"
        upscaler_idx = self.cmb_upscaler.findText(init_upscaler)
        if upscaler_idx >= 0:
            self.cmb_upscaler.setCurrentIndex(upscaler_idx)
        grid_disp.addWidget(self.cmb_upscaler, 3, 1)

        grid_disp.addWidget(QLabel("Upscaler Preset:"), 3, 2)
        self.cmb_preset = QComboBox()
        for label, value in PRESET_CHOICES:
            self.cmb_preset.addItem(label, value)
        preset_idx = self.cmb_preset.findData(ini_opts.get("preset", "1").strip())
        self.cmb_preset.setCurrentIndex(preset_idx if preset_idx >= 0 else 0)
        self.cmb_preset.currentIndexChanged.connect(lambda: self.save_bbport_ini())
        grid_disp.addWidget(self.cmb_preset, 3, 3)

        grid_disp.addWidget(QLabel("GPU Adapter:"), 4, 0)
        self.cmb_gpu = QComboBox()
        self.fill_gpu_choices()
        self.cmb_gpu.currentIndexChanged.connect(lambda: (self.save_settings(), self.save_bbport_ini()))
        grid_disp.addWidget(self.cmb_gpu, 4, 1)

        grid_disp.addWidget(QLabel("Live Resolution:"), 4, 2)
        self.cmb_live_res = QComboBox()
        for label, value in LIVE_RES_CHOICES:
            self.cmb_live_res.addItem(label, value)
        live_idx = self.cmb_live_res.findData(ini_opts.get("live_resolution", "0").strip().lower())
        self.cmb_live_res.setCurrentIndex(live_idx if live_idx >= 0 else 0)
        self.cmb_live_res.currentIndexChanged.connect(lambda: self.save_bbport_ini())
        grid_disp.addWidget(self.cmb_live_res, 4, 3)

        grid_disp.addWidget(QLabel("Async Pipelines:"), 5, 0)
        self.cmb_async_pipelines = QComboBox()
        for label, value in ASYNC_PIPELINE_CHOICES:
            self.cmb_async_pipelines.addItem(label, value)
        cur_async = str(ini_opts.get("async_pipelines", self.settings.get("async_pipelines", "auto"))).strip().lower()
        async_idx = self.cmb_async_pipelines.findData(cur_async)
        self.cmb_async_pipelines.setCurrentIndex(async_idx if async_idx >= 0 else 0)
        self.cmb_async_pipelines.currentIndexChanged.connect(lambda: (self.save_settings(), self.save_bbport_ini()))
        grid_disp.addWidget(self.cmb_async_pipelines, 5, 1)

        grid_disp.addWidget(QLabel("Game Text Language:"), 5, 2)
        self.cmb_lang = QComboBox()
        for label, code in LANGUAGE_CHOICES:
            self.cmb_lang.addItem(label, code)
        cur_lang = self.settings.get("language", "1")
        lang_idx = next((i for i, (_, c) in enumerate(LANGUAGE_CHOICES) if c == cur_lang), 0)
        self.cmb_lang.setCurrentIndex(lang_idx)
        self.cmb_lang.currentIndexChanged.connect(lambda: (self.save_settings(), self.save_bbport_ini()))
        grid_disp.addWidget(self.cmb_lang, 5, 3)

        l_disp.addLayout(grid_disp)

        row_flags = QGridLayout()
        row_flags.setHorizontalSpacing(16)
        row_flags.setVerticalSpacing(8)

        self.chk_fsr = QCheckBox("Enable Temporal Upscaler")
        self.chk_fsr.setChecked(self.settings.get("feat_upscaler", False) and self.cmb_upscaler.currentData() != "off")
        row_flags.addWidget(self.chk_fsr, 0, 0)

        self.chk_fps_patch = QCheckBox("Enable Live FPS Patching (60/90/120/Uncap)")
        self.chk_fps_patch.setChecked(self.settings.get("feat_fps_patch", True))
        row_flags.addWidget(self.chk_fps_patch, 0, 1)
        self._populate_fps_combo(self.chk_fps_patch.isChecked(), self.settings.get("fps", "60"))
        self.chk_fps_patch.toggled.connect(self._on_fps_patch_toggled)
        self.cmb_fps.currentIndexChanged.connect(lambda: (self.save_settings(), self.save_bbport_ini()))

        def on_upscaler_combo_changed():
            active = self.cmb_upscaler.currentData() != "off"
            self.chk_fsr.blockSignals(True)
            self.chk_fsr.setChecked(active)
            self.chk_fsr.blockSignals(False)
            self.save_settings()
            self.save_bbport_ini()

        def on_chk_fsr_toggled(checked):
            self.cmb_upscaler.blockSignals(True)
            if not checked:
                self.cmb_upscaler.setCurrentIndex(self.cmb_upscaler.findData("off"))
            elif self.cmb_upscaler.currentData() == "off":
                self.cmb_upscaler.setCurrentIndex(self.cmb_upscaler.findData("fsr31"))
            self.cmb_upscaler.blockSignals(False)
            self.save_settings()
            self.save_bbport_ini()

        self.cmb_upscaler.currentIndexChanged.connect(on_upscaler_combo_changed)
        self.chk_fsr.toggled.connect(on_chk_fsr_toggled)

        l_disp.addLayout(row_flags)
        self._save_footer(l_disp)
        c_layout.addWidget(card_disp)

        # ---- Upscaler quality ----
        card_q, l_q = self._new_card(
            "Upscaler Quality",
            "Applies while the upscaler is active. The same values are available in the in-game menu.",
        )
        grid_q = QGridLayout()
        grid_q.setHorizontalSpacing(16)
        grid_q.setVerticalSpacing(10)

        self.chk_sharpen = QCheckBox("Enable Sharpening")
        self.chk_sharpen.setChecked(ini_opts.get("sharpen", "1") != "0")
        self.chk_sharpen.stateChanged.connect(lambda: self.save_bbport_ini())
        grid_q.addWidget(self.chk_sharpen, 0, 0)

        self.chk_jitter = QCheckBox("Jitter")
        self.chk_jitter.setChecked(ini_opts.get("jitter", "1") != "0")
        self.chk_jitter.stateChanged.connect(lambda: self.save_bbport_ini())
        grid_q.addWidget(self.chk_jitter, 0, 1)

        self.chk_reactive = QCheckBox("Reactive Mask")
        self.chk_reactive.setChecked(ini_opts.get("reactive", "0") != "0")
        self.chk_reactive.stateChanged.connect(lambda: self.save_bbport_ini())
        grid_q.addWidget(self.chk_reactive, 1, 0)

        self.chk_object_motion = QCheckBox("Object Motion Vectors")
        self.chk_object_motion.setChecked(ini_opts.get("object_motion", "0") != "0")
        self.chk_object_motion.stateChanged.connect(lambda: self.save_bbport_ini())
        grid_q.addWidget(self.chk_object_motion, 1, 1)

        self.chk_fsr4_auto_exposure = QCheckBox("FSR 4 Auto Exposure")
        self.chk_fsr4_auto_exposure.setChecked(ini_opts.get("fsr4_auto_exposure", "0") != "0")
        self.chk_fsr4_auto_exposure.stateChanged.connect(lambda: self.save_bbport_ini())
        grid_q.addWidget(self.chk_fsr4_auto_exposure, 2, 0)

        self.chk_fsr4_invert_jitter = QCheckBox("FSR 4 Invert Jitter")
        self.chk_fsr4_invert_jitter.setChecked(ini_opts.get("fsr4_invert_jitter", "0") != "0")
        self.chk_fsr4_invert_jitter.stateChanged.connect(lambda: self.save_bbport_ini())
        grid_q.addWidget(self.chk_fsr4_invert_jitter, 2, 1)

        l_q.addLayout(grid_q)

        grid_qs = QGridLayout()
        grid_qs.setHorizontalSpacing(16)
        grid_qs.setVerticalSpacing(10)
        self.sl_sharpness, self.lbl_sharpness_val = self._make_slider(
            0, 200, int(float(ini_opts.get("sharpness", "0.30")) * 100)
        )
        self._add_slider_row(grid_qs, 0, "Sharpness:", self.sl_sharpness, self.lbl_sharpness_val)
        self.sl_reactive_scale, self.lbl_reactive_scale_val = self._make_slider(
            0, 1600, int(float(ini_opts.get("reactive_scale", "1.00")) * 100)
        )
        self._add_slider_row(grid_qs, 1, "Reactive Scale:", self.sl_reactive_scale, self.lbl_reactive_scale_val)
        self.sl_reactive_threshold, self.lbl_reactive_threshold_val = self._make_slider(
            0, 100, int(float(ini_opts.get("reactive_threshold", "0.20")) * 100)
        )
        self._add_slider_row(
            grid_qs, 2, "Reactive Threshold:", self.sl_reactive_threshold, self.lbl_reactive_threshold_val
        )
        self.sl_reactive_max, self.lbl_reactive_max_val = self._make_slider(
            0, 100, int(float(ini_opts.get("reactive_max", "0.90")) * 100)
        )
        self._add_slider_row(grid_qs, 3, "Reactive Max:", self.sl_reactive_max, self.lbl_reactive_max_val)
        l_q.addLayout(grid_qs)

        self._save_footer(l_q)
        c_layout.addWidget(card_q)

        # ---- HUD & diagnostics ----
        card_hud, l_hud = self._new_card("HUD & Diagnostics")
        row_hud = QGridLayout()
        row_hud.setHorizontalSpacing(16)
        row_hud.setVerticalSpacing(8)

        self.chk_show_fps = QCheckBox("Show FPS Counter")
        self.chk_show_fps.setChecked(ini_opts.get("show_fps", "0") != "0")
        self.chk_show_fps.stateChanged.connect(lambda: self.save_bbport_ini())
        row_hud.addWidget(self.chk_show_fps, 0, 0)

        self.chk_hud = QCheckBox("Show HUD Frame Diagnostics")
        self.chk_hud.setChecked(ini_opts.get("show_hud", "1") != "0")
        self.chk_hud.stateChanged.connect(lambda: self.save_bbport_ini())
        row_hud.addWidget(self.chk_hud, 0, 1)

        row_hud.addWidget(QLabel("HUD Quadrant:"), 1, 0)
        self.cmb_hud_quadrant = QComboBox()
        for label, value in HUD_QUADRANT_CHOICES:
            self.cmb_hud_quadrant.addItem(label, value)
        hud_q_idx = self.cmb_hud_quadrant.findData(ini_opts.get("hud_quadrant", "0").strip())
        self.cmb_hud_quadrant.setCurrentIndex(hud_q_idx if hud_q_idx >= 0 else 0)
        self.cmb_hud_quadrant.currentIndexChanged.connect(lambda: self.save_bbport_ini())
        row_hud.addWidget(self.cmb_hud_quadrant, 1, 1)

        row_hud.addWidget(QLabel("Debug View:"), 1, 2)
        self.cmb_debug_view = QComboBox()
        for label, value in DEBUG_VIEW_CHOICES:
            self.cmb_debug_view.addItem(label, value)
        debug_idx = self.cmb_debug_view.findData(ini_opts.get("debug_view", "0").strip())
        self.cmb_debug_view.setCurrentIndex(debug_idx if debug_idx >= 0 else 0)
        self.cmb_debug_view.currentIndexChanged.connect(lambda: self.save_bbport_ini())
        row_hud.addWidget(self.cmb_debug_view, 1, 3)
        l_hud.addLayout(row_hud)

        grid_hud = QGridLayout()
        grid_hud.setHorizontalSpacing(16)
        grid_hud.setVerticalSpacing(10)
        self.sl_hud_opacity, self.lbl_hud_opacity_val = self._make_slider(
            10, 100, int(float(ini_opts.get("hud_opacity", "0.75")) * 100)
        )
        self._add_slider_row(grid_hud, 0, "HUD Opacity:", self.sl_hud_opacity, self.lbl_hud_opacity_val)
        self.sl_hud_scale, self.lbl_hud_scale_val = self._make_slider(
            50, 250, int(float(ini_opts.get("hud_scale", "1.00")) * 100)
        )
        self._add_slider_row(grid_hud, 1, "HUD Scale:", self.sl_hud_scale, self.lbl_hud_scale_val)
        l_hud.addLayout(grid_hud)

        self._save_footer(l_hud)
        c_layout.addWidget(card_hud)

        # ---- Effects & model detail ----
        card_fx, l_fx = self._new_card(
            "Effects & Model Detail",
            "Unchecked boxes apply the matching disable patch at the next launch.",
        )
        grid_fx = QGridLayout()
        grid_fx.setHorizontalSpacing(16)
        grid_fx.setVerticalSpacing(8)

        tooltips = {
            "effect_ssr": "Not in the original game; costs performance.",
            "skip_intro": "Skips the intro videos when the game starts.",
            "debug_camera": "Free camera: Cross + L3 in game.",
            "debug_menu": "Needs the game's debug font files.",
            "debug_camera_note": "",
        }
        self.effect_checks = {}
        for i, (key, label, default_on) in enumerate(EFFECT_CHOICES):
            chk = QCheckBox(label)
            chk.setChecked(ini_opts.get(key, "1" if default_on else "0") != "0")
            tip = tooltips.get(key)
            if tip:
                chk.setToolTip(tip)
            chk.stateChanged.connect(lambda: self.save_bbport_ini())
            self.effect_checks[key] = chk
            grid_fx.addWidget(chk, i // 2, i % 2)
        l_fx.addLayout(grid_fx)

        row_lod = QHBoxLayout()
        row_lod.addWidget(QLabel("Model LOD:"))
        self.cmb_model_lod = QComboBox()
        for label, value in MODEL_LOD_CHOICES:
            self.cmb_model_lod.addItem(label, value)
        self.cmb_model_lod.setMaximumWidth(320)
        lod_idx = self.cmb_model_lod.findData(ini_opts.get("model_lod", "0").strip())
        self.cmb_model_lod.setCurrentIndex(lod_idx if lod_idx >= 0 else 0)
        self.cmb_model_lod.currentIndexChanged.connect(lambda: self.save_bbport_ini())
        row_lod.addWidget(self.cmb_model_lod, 1)
        row_lod.addStretch()
        l_fx.addLayout(row_lod)

        l_fx.addWidget(QLabel("Game's own anti-aliasing is controlled by the Anti-Aliasing option above."))
        self._save_footer(l_fx)
        c_layout.addWidget(card_fx)

        c_layout.addStretch()
        return w

    # ------------------------------------------------------------------
    # MOUSE & KEYBOARD
    # ------------------------------------------------------------------
    def create_mk_tab(self) -> QWidget:
        w, c_layout = self._create_page("Mouse & Keyboard")
        ini_opts = self.load_bbport_ini()

        card_mk = QFrame()
        card_mk.setObjectName("Card")
        l_mk = QVBoxLayout(card_mk)
        l_mk.setContentsMargins(16, 14, 16, 14)
        l_mk.setSpacing(12)

        lbl_mk = QLabel("Mouse & Keyboard Input Configuration")
        lbl_mk.setObjectName("CardTitle")
        l_mk.addWidget(lbl_mk)

        self.chk_mk_enabled = QCheckBox("Enable Mouse && Keyboard Mode")
        self.chk_mk_enabled.setChecked(ini_opts.get("mk_enabled", "1") != "0")
        self.chk_mk_enabled.stateChanged.connect(lambda: self.save_bbport_ini())
        l_mk.addWidget(self.chk_mk_enabled)

        grid_mk = QGridLayout()
        grid_mk.setHorizontalSpacing(16)
        grid_mk.setVerticalSpacing(10)

        grid_mk.addWidget(QLabel("Horizontal Sensitivity:"), 0, 0)
        self.sl_sens_x = NoWheelSlider(Qt.Horizontal)
        self.sl_sens_x.setRange(10, 1000)
        self.sl_sens_x.setValue(int(float(ini_opts.get("mk_sens_x", "3.20")) * 100))
        self.lbl_sens_x_val = QLabel(f"{self.sl_sens_x.value() / 100:.2f}")
        self.sl_sens_x.valueChanged.connect(lambda v: self.lbl_sens_x_val.setText(f"{v / 100:.2f}"))
        self.sl_sens_x.sliderReleased.connect(self.save_bbport_ini)
        grid_mk.addWidget(self.sl_sens_x, 0, 1)
        grid_mk.addWidget(self.lbl_sens_x_val, 0, 2)

        grid_mk.addWidget(QLabel("Vertical Sensitivity:"), 1, 0)
        self.sl_sens_y = NoWheelSlider(Qt.Horizontal)
        self.sl_sens_y.setRange(10, 1000)
        self.sl_sens_y.setValue(int(float(ini_opts.get("mk_sens_y", "3.26")) * 100))
        self.lbl_sens_y_val = QLabel(f"{self.sl_sens_y.value() / 100:.2f}")
        self.sl_sens_y.valueChanged.connect(lambda v: self.lbl_sens_y_val.setText(f"{v / 100:.2f}"))
        self.sl_sens_y.sliderReleased.connect(self.save_bbport_ini)
        grid_mk.addWidget(self.sl_sens_y, 1, 1)
        grid_mk.addWidget(self.lbl_sens_y_val, 1, 2)

        grid_mk.addWidget(QLabel("Mouse Smoothing:"), 2, 0)
        self.sl_smoothing = NoWheelSlider(Qt.Horizontal)
        self.sl_smoothing.setRange(0, 80)
        self.sl_smoothing.setValue(int(float(ini_opts.get("mk_smoothing", "0.25")) * 100))
        self.lbl_smoothing_val = QLabel(f"{self.sl_smoothing.value() / 100:.2f}")
        self.sl_smoothing.valueChanged.connect(lambda v: self.lbl_smoothing_val.setText(f"{v / 100:.2f}"))
        self.sl_smoothing.sliderReleased.connect(self.save_bbport_ini)
        grid_mk.addWidget(self.sl_smoothing, 2, 1)
        grid_mk.addWidget(self.lbl_smoothing_val, 2, 2)

        grid_mk.addWidget(QLabel("Responsiveness:"), 3, 0)
        self.sl_responsiveness = NoWheelSlider(Qt.Horizontal)
        self.sl_responsiveness.setRange(50, 250)
        self.sl_responsiveness.setValue(int(float(ini_opts.get("mk_responsiveness", "1.00")) * 100))
        self.lbl_responsiveness_val = QLabel(f"{self.sl_responsiveness.value() / 100:.2f}")
        self.sl_responsiveness.valueChanged.connect(lambda v: self.lbl_responsiveness_val.setText(f"{v / 100:.2f}"))
        self.sl_responsiveness.sliderReleased.connect(self.save_bbport_ini)
        grid_mk.addWidget(self.sl_responsiveness, 3, 1)
        grid_mk.addWidget(self.lbl_responsiveness_val, 3, 2)

        grid_mk.addWidget(QLabel("Mouse Acceleration:"), 4, 0)
        self.sl_acceleration = NoWheelSlider(Qt.Horizontal)
        self.sl_acceleration.setRange(100, 200)
        self.sl_acceleration.setValue(int(float(ini_opts.get("mk_acceleration", "1.30")) * 100))
        self.lbl_acceleration_val = QLabel(f"{self.sl_acceleration.value() / 100:.2f}")
        self.sl_acceleration.valueChanged.connect(lambda v: self.lbl_acceleration_val.setText(f"{v / 100:.2f}"))
        self.sl_acceleration.sliderReleased.connect(self.save_bbport_ini)
        grid_mk.addWidget(self.sl_acceleration, 4, 1)
        grid_mk.addWidget(self.lbl_acceleration_val, 4, 2)

        self.chk_inv_x = QCheckBox("Invert Horizontal Look (X)")
        self.chk_inv_x.setChecked(ini_opts.get("mk_invert_x", "0") != "0")
        self.chk_inv_x.stateChanged.connect(lambda: self.save_bbport_ini())
        grid_mk.addWidget(self.chk_inv_x, 5, 0)

        self.chk_inv_y = QCheckBox("Invert Vertical Look (Y)")
        self.chk_inv_y.setChecked(ini_opts.get("mk_invert_y", "0") != "0")
        self.chk_inv_y.stateChanged.connect(lambda: self.save_bbport_ini())
        grid_mk.addWidget(self.chk_inv_y, 5, 1)

        self.chk_mouse_menu = QCheckBox("Mouse Pointer in Game Menus")
        self.chk_mouse_menu.setToolTip(
            "Hover selects a menu row, left click chooses it, right click goes back, the wheel scrolls."
        )
        self.chk_mouse_menu.setChecked(ini_opts.get("mouse_menu", "1") != "0")
        self.chk_mouse_menu.stateChanged.connect(lambda: self.save_bbport_ini())
        grid_mk.addWidget(self.chk_mouse_menu, 6, 0)

        l_mk.addLayout(grid_mk)

        sep = QFrame()
        sep.setFixedHeight(1)
        sep.setStyleSheet("background-color: #24242d; border: none;")
        l_mk.addWidget(sep)

        row_kb_header = QHBoxLayout()
        lbl_kb = QLabel("Customize Keybinds")
        lbl_kb.setObjectName("CardTitle")
        row_kb_header.addWidget(lbl_kb)
        row_kb_header.addStretch()

        btn_preset_azerty = QPushButton("AZERTY")
        btn_preset_azerty.setObjectName("SecondaryBtn")
        btn_preset_azerty.setToolTip("Set movement keys to ZQSD (AZERTY layout).")
        btn_preset_azerty.clicked.connect(lambda: self.apply_keybind_preset("azerty"))
        row_kb_header.addWidget(btn_preset_azerty)

        btn_preset_qwerty = QPushButton("QWERTY")
        btn_preset_qwerty.setObjectName("SecondaryBtn")
        btn_preset_qwerty.setToolTip("Set movement keys to WASD (QWERTY layout).")
        btn_preset_qwerty.clicked.connect(lambda: self.apply_keybind_preset("qwerty"))
        row_kb_header.addWidget(btn_preset_qwerty)

        btn_save_ini = QPushButton("Save Input Parameters to bbport.ini")
        btn_save_ini.setObjectName("SecondaryBtn")
        btn_save_ini.clicked.connect(self.save_bbport_ini)
        row_kb_header.addWidget(btn_save_ini)

        btn_reset_kb = QPushButton("Reset Keybinds to Defaults")
        btn_reset_kb.setObjectName("SecondaryBtn")
        btn_reset_kb.clicked.connect(self.reset_keybinds_to_defaults)
        row_kb_header.addWidget(btn_reset_kb)
        l_mk.addLayout(row_kb_header)

        kb_columns = QHBoxLayout()
        kb_columns.setSpacing(32)

        # Left: gameplay pad/mouse bindings.
        left_col = QVBoxLayout()
        left_col.setSpacing(6)
        lbl_gameplay = QLabel("Gameplay Controls")
        lbl_gameplay.setStyleSheet(
            "color: #8b8b98; font-size: 11px; font-weight: 600; background: transparent;"
        )
        left_col.addWidget(lbl_gameplay)

        grid_kb = QGridLayout()
        grid_kb.setHorizontalSpacing(16)
        grid_kb.setVerticalSpacing(8)
        grid_kb.setColumnMinimumWidth(0, 240)
        grid_kb.setColumnMinimumWidth(1, 160)
        grid_kb.setColumnStretch(1, 0)

        self.keybind_buttons = {}
        for row, (ini_key, desc, def_val) in enumerate(GAMEPLAY_KEYBINDS):
            lbl_desc = QLabel(desc)
            lbl_desc.setStyleSheet("color: #dcdce0; font-weight: 500; background: transparent;")
            grid_kb.addWidget(lbl_desc, row, 0)

            cur_val = ini_opts.get(ini_key, def_val)
            btn_kb = KeybindButton(ini_key, cur_val)
            btn_kb.key_assigned.connect(self.on_keybind_button_assigned)
            self.keybind_buttons[ini_key] = btn_kb
            grid_kb.addWidget(btn_kb, row, 1)

        left_col.addLayout(grid_kb)
        left_col.addStretch()

        # Right: four keyboard-only system hotkeys.
        right_col = QVBoxLayout()
        right_col.setSpacing(6)
        lbl_system = QLabel("System Hotkeys")
        lbl_system.setStyleSheet(
            "color: #8b8b98; font-size: 11px; font-weight: 600; background: transparent;"
        )
        right_col.addWidget(lbl_system)

        lbl_hint = QLabel(
            "Overlay, performance HUD, cheat menu and mouse capture. "
            "These four are keyboard-only so a mouse attack cannot open a menu."
        )
        lbl_hint.setWordWrap(True)
        lbl_hint.setStyleSheet("color: #8b8b98; font-size: 11px; background: transparent;")
        right_col.addWidget(lbl_hint)

        grid_sys = QGridLayout()
        grid_sys.setHorizontalSpacing(16)
        grid_sys.setVerticalSpacing(8)
        grid_sys.setColumnMinimumWidth(0, 230)
        grid_sys.setColumnMinimumWidth(1, 150)
        grid_sys.setColumnStretch(1, 1)
        for row, (ini_key, desc, def_val) in enumerate(SYSTEM_KEYBINDS):
            lbl_desc = QLabel(desc)
            lbl_desc.setStyleSheet("color: #dcdce0; font-weight: 500; background: transparent;")
            grid_sys.addWidget(lbl_desc, row, 0)

            cur_val = ini_opts.get(ini_key, def_val)
            btn_kb = KeybindButton(ini_key, cur_val, keyboard_only=True)
            btn_kb.key_assigned.connect(self.on_keybind_button_assigned)
            self.keybind_buttons[ini_key] = btn_kb
            grid_sys.addWidget(btn_kb, row, 1)

        right_col.addLayout(grid_sys)
        right_col.addStretch()

        kb_columns.addLayout(left_col, 0)
        kb_columns.addLayout(right_col, 1)
        l_mk.addLayout(kb_columns)

        c_layout.addWidget(card_mk)
        c_layout.addStretch()
        return w

    # ------------------------------------------------------------------
    # ADVANCED SETTINGS
    # ------------------------------------------------------------------
    def create_advanced_tab(self) -> QWidget:
        w, c_layout = self._create_page("Advanced Settings")
        ini_opts = self.load_bbport_ini()
        s = self.settings

        # ---- Launch & runtime ----
        card_rt, l_rt = self._new_card(
            "Launch & Runtime",
            "Passed to run.bat as environment variables. Blank fields keep the run.bat defaults.",
        )
        grid_rt = QGridLayout()
        grid_rt.setHorizontalSpacing(16)
        grid_rt.setVerticalSpacing(10)

        grid_rt.addWidget(QLabel("In-Game Menu Language:"), 0, 0)
        self.cmb_menu_lang = QComboBox()
        for label, value in MENU_LANGUAGE_CHOICES:
            self.cmb_menu_lang.addItem(label, value)
        menu_idx = self.cmb_menu_lang.findData(ini_opts.get("language", "en").strip().lower())
        self.cmb_menu_lang.setCurrentIndex(menu_idx if menu_idx >= 0 else 0)
        self.cmb_menu_lang.currentIndexChanged.connect(lambda: self.save_bbport_ini())
        grid_rt.addWidget(self.cmb_menu_lang, 0, 1)

        grid_rt.addWidget(QLabel("Content Profile SKU:"), 0, 2)
        self.cmb_sku = QComboBox()
        for label, value in CONTENT_SKU_CHOICES:
            self.cmb_sku.addItem(label, value)
        sku_idx = self.cmb_sku.findData(s.get("sku", "full"))
        self.cmb_sku.setCurrentIndex(sku_idx if sku_idx >= 0 else 0)
        self.cmb_sku.currentIndexChanged.connect(lambda: self.save_settings())
        grid_rt.addWidget(self.cmb_sku, 0, 3)

        grid_rt.addWidget(QLabel("Frame Timeout (seconds):"), 1, 0)
        self.txt_timeout = QLineEdit(s.get("timeout", ""))
        self.txt_timeout.setPlaceholderText("0 = run until closed")
        self.txt_timeout.editingFinished.connect(self.save_settings)
        grid_rt.addWidget(self.txt_timeout, 1, 1)

        grid_rt.addWidget(QLabel("V-Blank Rate (Hz):"), 1, 2)
        self.txt_vblank = QLineEdit(s.get("vblank_hz", ""))
        self.txt_vblank.setPlaceholderText("blank = derive from FPS (0 = uncapped)")
        self.txt_vblank.editingFinished.connect(self.save_settings)
        grid_rt.addWidget(self.txt_vblank, 1, 3)

        grid_rt.addWidget(QLabel("Frames Ahead:"), 2, 0)
        self.txt_frames_ahead = QLineEdit(s.get("frames_ahead", "2"))
        self.txt_frames_ahead.setPlaceholderText("engine default: 1")
        self.txt_frames_ahead.editingFinished.connect(self.save_settings)
        grid_rt.addWidget(self.txt_frames_ahead, 2, 1)

        grid_rt.addWidget(QLabel("Direct Memory (MiB):"), 2, 2)
        self.txt_dmem = QLineEdit(s.get("dmem_mb", ""))
        self.txt_dmem.setPlaceholderText("blank = automatic (9152 above 1080p)")
        self.txt_dmem.editingFinished.connect(self.save_settings)
        grid_rt.addWidget(self.txt_dmem, 2, 3)

        grid_rt.addWidget(QLabel("Extra run.bat Arguments:"), 3, 0)
        self.txt_extra_args = QLineEdit(s.get("extra_args", ""))
        self.txt_extra_args.setPlaceholderText('appended to run.bat, e.g. --some-flag "path with spaces"')
        self.txt_extra_args.editingFinished.connect(self.save_settings)
        grid_rt.addWidget(self.txt_extra_args, 3, 1, 1, 3)

        self.chk_mods = QCheckBox("Load mods/ Directory Assets")
        self.chk_mods.setChecked(s.get("mods", False))
        self.chk_mods.stateChanged.connect(lambda: self.save_settings())
        grid_rt.addWidget(self.chk_mods, 4, 0, 1, 2)
        l_rt.addLayout(grid_rt)

        row_save = QHBoxLayout()
        row_save.addStretch()
        btn_save = QPushButton("Save Advanced Settings")
        btn_save.setObjectName("SecondaryBtn")
        btn_save.clicked.connect(self.save_advanced)
        row_save.addWidget(btn_save)
        l_rt.addLayout(row_save)
        c_layout.addWidget(card_rt)

        # ---- Mods & external patches ----
        card_paths, l_paths = self._new_card(
            "Mods & External Patches",
            "Blank paths use the defaults inside run.bat (mods/, mods.json, patches/, patches.json).",
        )
        grid_paths = QGridLayout()
        grid_paths.setHorizontalSpacing(16)
        grid_paths.setVerticalSpacing(10)
        self.txt_mods_dir = QLineEdit(s.get("mods_dir", ""))
        self.txt_mods_dir.setPlaceholderText("default: mods")
        self.txt_mods_dir.editingFinished.connect(self.save_settings)
        self._path_row(grid_paths, 0, "Mods Directory:", self.txt_mods_dir, "dir")

        self.txt_mods_config = QLineEdit(s.get("mods_config", ""))
        self.txt_mods_config.setPlaceholderText("default: mods.json")
        self.txt_mods_config.editingFinished.connect(self.save_settings)
        self._path_row(grid_paths, 1, "Mods Config File:", self.txt_mods_config, "file")

        self.txt_patches_dir = QLineEdit(s.get("patches_dir", ""))
        self.txt_patches_dir.setPlaceholderText("default: patches")
        self.txt_patches_dir.editingFinished.connect(self.save_settings)
        self._path_row(grid_paths, 2, "External Patches Directory:", self.txt_patches_dir, "dir")

        self.txt_patches_config = QLineEdit(s.get("patches_config", ""))
        self.txt_patches_config.setPlaceholderText("default: patches.json")
        self.txt_patches_config.editingFinished.connect(self.save_settings)
        self._path_row(grid_paths, 3, "Patches Config File:", self.txt_patches_config, "file")
        l_paths.addLayout(grid_paths)
        c_layout.addWidget(card_paths)

        # ---- Save data & files ----
        card_files, l_files = self._new_card(
            "Save Data & Launcher Files",
            "User data holds saves and the shader cache. Reload re-reads bbport.ini changed by the game.",
        )
        grid_files = QGridLayout()
        grid_files.setHorizontalSpacing(16)
        grid_files.setVerticalSpacing(10)
        self.txt_user_dir = QLineEdit(s.get("user_dir", ""))
        self.txt_user_dir.setPlaceholderText("default: user")
        self.txt_user_dir.editingFinished.connect(self.save_settings)
        self.txt_user_dir.editingFinished.connect(self.update_cache_info)
        self._path_row(grid_files, 0, "User Data Directory:", self.txt_user_dir, "dir")
        l_files.addLayout(grid_files)

        row_cache = QHBoxLayout()
        row_cache.setSpacing(8)
        self.lbl_cache_info = QLabel("Shader cache: ...")
        self.lbl_cache_info.setObjectName("CardHint")
        row_cache.addWidget(self.lbl_cache_info, 1)
        self.btn_cache = QPushButton("Reset Shader Cache")
        self.btn_cache.setObjectName("SecondaryBtn")
        self.btn_cache.clicked.connect(self.reset_shader_cache)
        row_cache.addWidget(self.btn_cache)
        l_files.addLayout(row_cache)

        row_files = QHBoxLayout()
        row_files.addStretch()
        btn_open = QPushButton("Open bbport.ini")
        btn_open.setObjectName("SecondaryBtn")
        btn_open.clicked.connect(self.open_bbport_ini)
        row_files.addWidget(btn_open)

        btn_reload = QPushButton("Reload from bbport.ini")
        btn_reload.setObjectName("SecondaryBtn")
        btn_reload.clicked.connect(self.reload_bbport_ini)
        row_files.addWidget(btn_reload)
        l_files.addLayout(row_files)
        c_layout.addWidget(card_files)

        c_layout.addStretch()
        self.update_cache_info()
        return w
