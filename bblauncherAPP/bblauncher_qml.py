#!/usr/bin/env python3
"""Qt Quick entry point for the Bloodborne PC Launcher."""

from __future__ import annotations

import ctypes
import sys
from pathlib import Path

from PySide6.QtCore import QEvent, QObject, QUrl
from PySide6.QtGui import QCloseEvent
from PySide6.QtQml import QQmlApplicationEngine
from PySide6.QtWidgets import QApplication

from bblauncher_dialogs import DarkMessageBox as QMessageBox
from bblauncher_qmlview import ensure_basic_style, register_qml_types

APP_DIR = Path(__file__).resolve().parent


def run() -> int:
    if sys.platform == "win32":
        try:
            ctypes.windll.shell32.SetCurrentProcessExplicitAppUserModelID(
                "Bloodborne.PC.Launcher"
            )
        except Exception:
            pass

    # The native Windows style refuses custom ScrollBar/Popup delegates.
    # register_qml_types() enforces this too; this early call just makes sure
    # the style is set before QApplication exists, as Qt documents.
    ensure_basic_style()

    app = QApplication(sys.argv)
    app.setApplicationName("Bloodborne PC Launcher")

    from bblauncher_app import BloodborneLauncher, get_launcher_icon

    launcher_icon = get_launcher_icon()
    if not launcher_icon.isNull():
        app.setWindowIcon(launcher_icon)

    from bblauncher_Utils import acquire_single_instance

    if not acquire_single_instance():
        QMessageBox.information(
            None, "Bloodborne PC Launcher", "The launcher is already open."
        )
        return 0

    register_qml_types(APP_DIR / "qml")

    controller = BloodborneLauncher()

    engine = QQmlApplicationEngine()
    controller.expose(engine)
    engine.load(QUrl.fromLocalFile(str(APP_DIR / "qml" / "main.qml")))

    roots = engine.rootObjects()
    if not roots:
        QMessageBox.critical(None, "Bloodborne PC Launcher",
                             "The launcher UI failed to load. See bblauncher.log.")
        return 1

    window = roots[0]

    # Keep the original "Bloodborne is running" confirmation by reusing the
    # controller's closeEvent logic, but ask only once per close request.
    state = {"confirmed": False}

    def confirm() -> bool:
        if state["confirmed"]:
            return True
        event = QCloseEvent()
        controller.closeEvent(event)
        if not event.isAccepted():
            return False
        state["confirmed"] = True
        return True

    def request_close() -> None:
        if confirm():
            window.close()

    controller.closeRequested.connect(request_close)

    # Alt+F4 / taskbar close.  PySide6 cannot deliver QQuickCloseEvent to a
    # Python slot ("parameter 0 of type QQuickCloseEvent* cannot be converted"),
    # so the window's closing signal is useless here; filter QEvent.Close on the
    # window instead, which also lets us veto the close while the game runs.
    class _CloseGuard(QObject):
        def eventFilter(self, obj, event):  # noqa: N802
            if event.type() == QEvent.Type.Close and not confirm():
                event.ignore()
                return True
            return False

    _close_guard = _CloseGuard(window)
    window.installEventFilter(_close_guard)

    # Safety net: whatever path ends the process, persist settings once.
    app.aboutToQuit.connect(controller.save_settings)

    return app.exec()


if __name__ == "__main__":
    sys.exit(run())
