#!/usr/bin/env python3
"""Dark-themed replacements for the stock Qt Widgets dialogs.

The launcher UI is Qt Quick and completely dark, but ``QMessageBox`` and
``QInputDialog`` are native Win32 widgets: on Windows they ignore the Qt Quick
palette and open as bright white boxes.  Rather than editing the ~50 existing
call sites, this module subclasses both and is imported under the original
names:

    from bblauncher_dialogs import DarkInputDialog as QInputDialog
    from bblauncher_dialogs import DarkMessageBox as QMessageBox

The subclasses keep the exact static API (``question``/``warning``/
``information``/``critical``/``getText``), so the controller does not change.

Styling has three layers, because each one alone leaves a light patch:

* the Fusion style, since the native Windows style refuses most QSS,
* a dark palette + stylesheet for the client area,
* the DWM dark title bar for the non-client area (Windows 10/11 only).
"""

from __future__ import annotations

import ctypes
import sys

from PySide6.QtCore import QEvent, QObject, Qt
from PySide6.QtGui import QColor, QPalette
from PySide6.QtWidgets import (
    QDialog,
    QInputDialog,
    QLineEdit,
    QMessageBox,
    QStyle,
    QStyleFactory,
)

# Matches qml/Theme.qml so the dialogs feel like part of the launcher.
_BG = "#15151B"
_SURFACE = "#1B1B23"
_FIELD = "#101015"
_BORDER = "#25252F"
_TEXT = "#EDEDF2"
_TEXT_MUTED = "#9A9AA8"
_ACCENT = "#C21722"

DIALOG_QSS = f"""
QDialog, QMessageBox, QInputDialog {{
    background-color: {_BG};
    font-family: "Segoe UI";
    font-size: 12px;
}}
QLabel {{
    background: transparent;
    color: {_TEXT};
}}
QLabel#CardTitle {{
    color: {_TEXT};
    font-size: 15px;
    font-weight: 600;
}}
QLabel:disabled {{
    color: {_TEXT_MUTED};
}}
QPushButton {{
    background-color: {_SURFACE};
    color: {_TEXT};
    font-weight: 600;
    padding: 7px 16px;
    min-width: 78px;
    border: 1px solid {_BORDER};
    border-radius: 6px;
}}
QPushButton:hover {{
    background-color: #2D2D38;
    color: #FFFFFF;
    border-color: #3B3B49;
}}
QPushButton:pressed {{
    background-color: #101015;
}}
QPushButton:default, QPushButton:focus {{
    border-color: {_ACCENT};
}}
QPushButton:disabled {{
    background-color: {_BG};
    color: #5A5A66;
    border-color: #1E1E26;
}}
QLineEdit {{
    background-color: {_FIELD};
    color: {_TEXT};
    border: 1px solid {_BORDER};
    border-radius: 6px;
    padding: 6px 10px;
    selection-background-color: {_ACCENT};
    selection-color: #FFFFFF;
}}
QLineEdit:focus {{
    border-color: {_ACCENT};
}}
QPlainTextEdit, QTextEdit {{
    background-color: {_FIELD};
    color: {_TEXT};
    border: 1px solid {_BORDER};
    border-radius: 6px;
}}
"""


def dark_palette() -> QPalette:
    """Palette used by every dark dialog (covers widgets QSS does not reach)."""
    palette = QPalette()
    roles = {
        QPalette.ColorRole.Window: _BG,
        QPalette.ColorRole.WindowText: _TEXT,
        QPalette.ColorRole.Base: _FIELD,
        QPalette.ColorRole.AlternateBase: _SURFACE,
        QPalette.ColorRole.Text: _TEXT,
        QPalette.ColorRole.Button: _SURFACE,
        QPalette.ColorRole.ButtonText: _TEXT,
        QPalette.ColorRole.BrightText: "#FFFFFF",
        QPalette.ColorRole.Highlight: _ACCENT,
        QPalette.ColorRole.HighlightedText: "#FFFFFF",
        QPalette.ColorRole.ToolTipBase: _SURFACE,
        QPalette.ColorRole.ToolTipText: _TEXT,
        QPalette.ColorRole.PlaceholderText: _TEXT_MUTED,
        QPalette.ColorRole.Link: "#D3B77B",
    }
    for role, color in roles.items():
        palette.setColor(role, QColor(color))
    for role in (QPalette.ColorGroup.Disabled,):
        palette.setColor(role, QPalette.ColorRole.WindowText, QColor("#6C6C7A"))
        palette.setColor(role, QPalette.ColorRole.Text, QColor("#6C6C7A"))
        palette.setColor(role, QPalette.ColorRole.ButtonText, QColor("#6C6C7A"))
    return palette


_fusion_style: QStyle | None = None


def _fusion() -> QStyle | None:
    """Fusion honours QSS; the Windows style does not.  Created lazily, so it
    is never built before QApplication exists."""
    global _fusion_style
    if _fusion_style is None:
        _fusion_style = QStyleFactory.create("Fusion")
    return _fusion_style


def _apply_dark_title_bar(widget) -> None:
    """Paint the native title bar dark on Windows 10/11 (no-op elsewhere)."""
    if sys.platform != "win32":
        return
    try:
        hwnd = int(widget.winId())
    except Exception:
        return
    if not hwnd:
        return
    try:
        dwmapi = ctypes.windll.dwmapi
    except Exception:
        return

    def set_int(attribute: int, value: int) -> int:
        data = ctypes.c_int(value)
        try:
            return int(dwmapi.DwmSetWindowAttribute(
                hwnd, attribute, ctypes.byref(data), ctypes.sizeof(data)
            ))
        except Exception:
            return -1

    # 20 is the current DWMWA_USE_IMMERSIVE_DARK_MODE; 1809 used 19.
    if set_int(20, 1) != 0:
        set_int(19, 1)
    # Windows 11: match the caption to the dialog body and keep white glyphs.
    set_int(35, 0x001B1515)  # DWMWA_CAPTION_COLOR (COLORREF 0x00BBGGRR)
    set_int(36, 0x00F2EDED)  # DWMWA_TEXT_COLOR
    set_int(33, 2)           # DWMWA_WINDOW_CORNER_PREFERENCE: round


class _DarkFrameFilter(QObject):
    """Applies the dark title bar once the dialog has a native window."""

    def eventFilter(self, obj, event):  # noqa: N802
        if event.type() == QEvent.Type.Show and not obj.property("_bbDarkFrame"):
            obj.setProperty("_bbDarkFrame", True)
            _apply_dark_title_bar(obj)
        return False


def style_dialog(dialog: QDialog, *, dark_title_bar: bool = True) -> QDialog:
    """Apply the launcher's dark theme to any widget dialog and return it."""
    style = _fusion()
    if style is not None:
        dialog.setStyle(style)
    dialog.setPalette(dark_palette())
    dialog.setStyleSheet(DIALOG_QSS)
    if dark_title_bar and not dialog.property("_bbDarkFrameInstalled"):
        dialog.setProperty("_bbDarkFrameInstalled", True)
        dialog.installEventFilter(_DarkFrameFilter(dialog))
    return dialog


class DarkMessageBox(QMessageBox):
    """A QMessageBox with the launcher's dark theme.

    The static methods mirror ``QMessageBox``'s, so existing controller code
    keeps working unchanged.
    """

    def __init__(self, parent=None):
        super().__init__(parent)
        style_dialog(self)
        self.setWindowModality(Qt.WindowModality.ApplicationModal)

    @classmethod
    def _build(
        cls,
        parent,
        title: str,
        text: str,
        icon: QMessageBox.Icon,
        buttons: QMessageBox.StandardButton,
        default_button: QMessageBox.StandardButton,
    ) -> "DarkMessageBox":
        box = cls(parent)
        box.setWindowTitle(str(title))
        box.setText(str(text))
        box.setIcon(icon)
        box.setStandardButtons(buttons)
        if default_button is not None:
            box.setDefaultButton(default_button)
        # Let users select/copy the message text instead of retyping paths.
        box.setTextInteractionFlags(Qt.TextInteractionFlag.TextSelectableByMouse)
        return box

    @staticmethod
    def _run(box: "DarkMessageBox") -> QMessageBox.StandardButton:
        box.exec()
        clicked = box.clickedButton()
        if clicked is None:  # defensive: no button was ever activated
            return QMessageBox.StandardButton.NoButton
        return box.standardButton(clicked)

    @classmethod
    def question(
        cls,
        parent,
        title: str,
        text: str,
        buttons: QMessageBox.StandardButton = QMessageBox.StandardButton.Yes
        | QMessageBox.StandardButton.No,
        defaultButton: QMessageBox.StandardButton = QMessageBox.StandardButton.NoButton,
    ) -> QMessageBox.StandardButton:
        return cls._run(cls._build(
            parent, title, text, QMessageBox.Icon.Question, buttons, defaultButton
        ))

    @classmethod
    def warning(
        cls,
        parent,
        title: str,
        text: str,
        buttons: QMessageBox.StandardButton = QMessageBox.StandardButton.Ok,
        defaultButton: QMessageBox.StandardButton = QMessageBox.StandardButton.NoButton,
    ) -> QMessageBox.StandardButton:
        return cls._run(cls._build(
            parent, title, text, QMessageBox.Icon.Warning, buttons, defaultButton
        ))

    @classmethod
    def information(
        cls,
        parent,
        title: str,
        text: str,
        buttons: QMessageBox.StandardButton = QMessageBox.StandardButton.Ok,
        defaultButton: QMessageBox.StandardButton = QMessageBox.StandardButton.NoButton,
    ) -> QMessageBox.StandardButton:
        return cls._run(cls._build(
            parent, title, text, QMessageBox.Icon.Information, buttons, defaultButton
        ))

    @classmethod
    def critical(
        cls,
        parent,
        title: str,
        text: str,
        buttons: QMessageBox.StandardButton = QMessageBox.StandardButton.Ok,
        defaultButton: QMessageBox.StandardButton = QMessageBox.StandardButton.NoButton,
    ) -> QMessageBox.StandardButton:
        return cls._run(cls._build(
            parent, title, text, QMessageBox.Icon.Critical, buttons, defaultButton
        ))


class DarkInputDialog(QInputDialog):
    """A QInputDialog with the launcher's dark theme."""

    def __init__(self, parent=None):
        super().__init__(parent)
        style_dialog(self)
        self.setWindowModality(Qt.WindowModality.ApplicationModal)

    @classmethod
    def getText(
        cls,
        parent,
        title: str,
        label: str,
        echo: QLineEdit.EchoMode = QLineEdit.EchoMode.Normal,
        text: str = "",
        flags=None,
        inputMethodHints=Qt.InputMethodHint.ImhNone,
    ) -> tuple[str, bool]:
        dialog = cls(parent)
        dialog.setInputMode(QInputDialog.InputMode.TextInput)
        dialog.setWindowTitle(str(title))
        dialog.setLabelText(str(label))
        dialog.setTextEchoMode(echo)
        dialog.setTextValue(str(text))
        if flags is not None:
            dialog.setWindowFlags(flags)
        dialog.setInputMethodHints(inputMethodHints)
        accepted = dialog.exec() == QDialog.DialogCode.Accepted
        return dialog.textValue(), accepted
