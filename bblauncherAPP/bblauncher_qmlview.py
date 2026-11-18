#!/usr/bin/env python3
"""QML view layer for Bloodborne PC Launcher.

The controller in :mod:`bblauncher_app` was written against Qt Widgets.  Rather
than rewriting that battle-tested logic, this module provides lightweight
``QObject`` shims that expose the *same* attribute surface (``setText``,
``isChecked``, ``currentData``, ``blockSignals`` …) so the controller keeps
working untouched, while Qt Quick owns everything the user actually sees.

Naming contract
---------------
Qt widget getters keep their exact names and are plain methods
(``text()``, ``currentText()``, ``currentIndex()``, ``currentData()``,
``value()``, ``minimum()``, ``maximum()``).  Everything QML binds to is a
``Property`` with a distinct ``ui*`` name, so the two APIs never collide.

``blockSignals`` is *soft*: it suppresses only the semantic signals the
controller connects to (``toggled``, ``stateChanged``, ``currentIndexChanged``,
``textChanged`` …).  The ``visualChanged`` notify signal is never suppressed,
because blocking signals on a Qt widget still repaints it while a QML binding
must be told to re-evaluate.
"""

from __future__ import annotations

import re
from pathlib import Path

from PySide6.QtCore import (
    Property,
    QAbstractListModel,
    QModelIndex,
    QObject,
    Qt,
    Signal,
    Slot,
)


# --------------------------------------------------------------------------- #
#  helpers
# --------------------------------------------------------------------------- #

_HEX_TONES = {
    "#4ade80": "ok",
    "#6a9955": "ok",
    "#5bc98c": "ok",
    "#98c379": "ok",
    "#d1b87a": "gold",
    "#e2d7ba": "gold",
    "#dcdcaa": "gold",
    "#e5c07b": "gold",
    "#f0b429": "warn",
    "#e2b96f": "warn",
    "#ffffff": "text",
    "#dcdce0": "text",
    "#c8ccd4": "text",
    "#d8d8e0": "text",
    "#cccccc": "text",
    "#abb2bf": "muted",
    "#92929e": "muted",
    "#9da0b0": "muted",
    "#8c8c9e": "muted",
    "#7b7b88": "muted",
    "#6a6a78": "dim",
    "#6a6a80": "dim",
    "#8b8b98": "dim",
    "#5c6370": "dim",
    "#7c7c88": "dim",
    "#5a5a66": "dim",
    "#7fb08a": "ok",
    "#e0a33c": "warn",
    "#e63946": "accent",
    "#c678dd": "accent",
    "#ef4444": "err",
    "#e5484d": "err",
    "#e65555": "err",
    "#f87171": "err",
    "#f48771": "err",
    "#61afef": "info",
    "#56b6c2": "info",
}

_RGB_RE = re.compile(r"\b(?:color|background-color)\s*:\s*(#[0-9a-fA-F]{6})")


def tone_from_qss(qss: str, fallback: str = "text") -> str:
    """Map a stylesheet's first colour onto one of the QML theme tones."""
    for match in _RGB_RE.finditer(qss or ""):
        tone = _HEX_TONES.get(match.group(1).lower())
        if tone:
            return tone
    return fallback


# --------------------------------------------------------------------------- #
#  base shim
# --------------------------------------------------------------------------- #

class Shim(QObject):
    """Common state every visual shim shares."""

    visualChanged = Signal()      # never suppressed — QML bindings depend on it
    visibleChanged = Signal()
    enabledChanged = Signal()
    tooltipChanged = Signal()

    def __init__(self, name: str = "", parent: QObject | None = None):
        super().__init__(parent)
        self.shim_name = name
        self._blocked = False
        self._visible = True
        self._enabled = True
        self._tooltip = ""

    # -- soft signal blocking ----------------------------------------------
    def blockSignals(self, block: bool) -> bool:       # noqa: N802
        previous = self._blocked
        self._blocked = bool(block)
        return previous

    def signalsBlocked(self) -> bool:                  # noqa: N802
        return self._blocked

    def _touch(self) -> None:
        """Tell QML something visual changed, even while signals are blocked."""
        signal = getattr(self, "uiChanged", None)
        if signal is None:
            self.visualChanged.emit()
        else:
            signal.emit()

    # -- properties ---------------------------------------------------------
    def _get_visible(self) -> bool:
        return self._visible

    def _set_visible(self, value: bool) -> None:
        value = bool(value)
        if value != self._visible:
            self._visible = value
            self.visibleChanged.emit()

    visible = Property(bool, _get_visible, _set_visible, notify=visibleChanged)

    def _get_enabled(self) -> bool:
        return self._enabled

    def _set_enabled(self, value: bool) -> None:
        value = bool(value)
        if value != self._enabled:
            self._enabled = value
            self.enabledChanged.emit()

    enabled = Property(bool, _get_enabled, _set_enabled, notify=enabledChanged)

    def _get_tooltip(self) -> str:
        return self._tooltip

    def _set_tooltip(self, value: str) -> None:
        value = str(value or "")
        if value != self._tooltip:
            self._tooltip = value
            self.tooltipChanged.emit()

    tooltip = Property(str, _get_tooltip, _set_tooltip, notify=tooltipChanged)

    # -- Qt-widget compatible methods --------------------------------------
    def setVisible(self, value: bool) -> None:            # noqa: N802
        self._set_visible(value)

    def isVisible(self) -> bool:                          # noqa: N802
        return self._visible

    def setEnabled(self, value: bool) -> None:            # noqa: N802
        self._set_enabled(value)

    def isEnabled(self) -> bool:                          # noqa: N802
        return self._enabled

    def setToolTip(self, value: str) -> None:             # noqa: N802
        self._set_tooltip(value)

    def toolTip(self) -> str:                             # noqa: N802
        return self._tooltip

    # Presentation-only calls that have no meaning outside Qt Widgets.
    def setFixedWidth(self, *a, **k) -> None:             # noqa: N802
        pass

    def setFixedHeight(self, *a, **k) -> None:            # noqa: N802
        pass

    def setFixedSize(self, *a, **k) -> None:              # noqa: N802
        pass

    def setMinimumWidth(self, *a, **k) -> None:           # noqa: N802
        pass

    def setMinimumHeight(self, *a, **k) -> None:          # noqa: N802
        pass

    def setMinimumSize(self, *a, **k) -> None:            # noqa: N802
        pass

    def setMaximumWidth(self, *a, **k) -> None:           # noqa: N802
        pass

    def setMaximumHeight(self, *a, **k) -> None:          # noqa: N802
        pass

    def setMaximumSize(self, *a, **k) -> None:            # noqa: N802
        pass

    def setSizePolicy(self, *a, **k) -> None:             # noqa: N802
        pass

    def setContentsMargins(self, *a, **k) -> None:        # noqa: N802
        pass

    def setSpacing(self, *a, **k) -> None:                # noqa: N802
        pass

    def setAlignment(self, *a, **k) -> None:              # noqa: N802
        pass

    def setWordWrap(self, *a, **k) -> None:               # noqa: N802
        pass

    def setTextFormat(self, *a, **k) -> None:             # noqa: N802
        pass

    def setCursor(self, *a, **k) -> None:                 # noqa: N802
        pass

    def setFocusPolicy(self, *a, **k) -> None:            # noqa: N802
        pass

    def setUpdatesEnabled(self, *a, **k) -> None:         # noqa: N802
        pass

    def setMouseTracking(self, *a, **k) -> None:          # noqa: N802
        pass

    def setAcceptDrops(self, *a, **k) -> None:            # noqa: N802
        pass

    def setAutoFillBackground(self, *a, **k) -> None:     # noqa: N802
        pass

    def setObjectName(self, *a, **k) -> None:             # noqa: N802
        pass

    def adjustSize(self) -> None:                          # noqa: N802
        pass

    def update(self) -> None:
        pass

    def repaint(self) -> None:
        pass

    def deleteLater(self) -> None:                         # noqa: N802
        # the Qt Quick scene owns the visuals; nothing to tear down here
        pass


# --------------------------------------------------------------------------- #
#  label
# --------------------------------------------------------------------------- #

class ShLabel(Shim):
    uiChanged = Signal()
    toneChanged = Signal()

    def __init__(self, text: str = "", name: str = "", parent: QObject | None = None):
        super().__init__(name, parent)
        self._text = str(text)
        self._tone = "text"

    # -- QML ---------------------------------------------------------------
    def _get_ui_text(self) -> str:
        return self._text

    uiText = Property(str, _get_ui_text, notify=uiChanged)

    def _get_tone(self) -> str:
        return self._tone

    def _set_tone(self, value: str) -> None:
        if value != self._tone:
            self._tone = value
            self.toneChanged.emit()

    tone = Property(str, _get_tone, _set_tone, notify=toneChanged)

    # -- controller / Qt API -----------------------------------------------
    def setText(self, value: str) -> None:                 # noqa: N802
        value = str(value)
        if value != self._text:
            self._text = value
            self._touch()

    def text(self) -> str:                                 # noqa: N802
        return self._text

    def getText(self) -> str:                              # noqa: N802
        return self._text

    def setStyleSheet(self, qss: str) -> None:             # noqa: N802
        self._set_tone(tone_from_qss(qss, self._tone))

    def setRichText(self, *a, **k) -> None:                # noqa: N802
        pass


# --------------------------------------------------------------------------- #
#  button
# --------------------------------------------------------------------------- #

class ShButton(Shim):
    uiChanged = Signal()
    textChanged = Signal(str)
    checkedChanged = Signal(bool)

    clicked = Signal()
    clickedChecked = Signal(bool)
    toggled = Signal(bool)
    pressed = Signal()
    released = Signal()

    def __init__(self, text: str = "", name: str = "", parent: QObject | None = None):
        super().__init__(name, parent)
        self._text = str(text)
        self._checked = False
        self._checkable = False

    # -- QML ---------------------------------------------------------------
    def _get_ui_text(self) -> str:
        return self._text

    uiText = Property(str, _get_ui_text, notify=uiChanged)

    def _get_checked(self) -> bool:
        return self._checked

    def _set_checked(self, value: bool) -> None:
        value = bool(value)
        if value == self._checked:
            return
        self._checked = value
        self._touch()
        if not self._blocked:
            self.checkedChanged.emit(value)
            self.toggled.emit(value)

    checked = Property(bool, _get_checked, _set_checked, notify=uiChanged)

    def _get_checkable(self) -> bool:
        return self._checkable

    def _set_checkable(self, value: bool) -> None:
        self._checkable = bool(value)
        self._touch()

    checkable = Property(bool, _get_checkable, _set_checkable, notify=uiChanged)

    # -- controller / Qt API -----------------------------------------------
    def setText(self, value: str) -> None:                 # noqa: N802
        value = str(value)
        if value != self._text:
            self._text = value
            self._touch()
            if not self._blocked:
                self.textChanged.emit(value)

    def text(self) -> str:                                 # noqa: N802
        return self._text

    def setCheckable(self, value: bool) -> None:            # noqa: N802
        self._set_checkable(value)

    def isCheckable(self) -> bool:                          # noqa: N802
        return self._checkable

    def setChecked(self, value: bool) -> None:              # noqa: N802
        self._set_checked(value)

    def isChecked(self) -> bool:                            # noqa: N802
        return self._checked

    def click(self) -> None:
        if self._checkable:
            self._set_checked(not self._checked)
        if not self._blocked:
            self.clicked.emit()
            self.clickedChecked.emit(self._checked)

    def setDefault(self, *a, **k) -> None:                  # noqa: N802
        pass

    def setFlat(self, *a, **k) -> None:                     # noqa: N802
        pass

    def setIcon(self, *a, **k) -> None:                     # noqa: N802
        pass


# --------------------------------------------------------------------------- #
#  checkbox
# --------------------------------------------------------------------------- #

class ShCheck(Shim):
    uiChanged = Signal()
    textChanged = Signal(str)
    checkedChanged = Signal(bool)
    stateChanged = Signal(int)

    toggled = Signal(bool)
    clicked = Signal()

    def __init__(self, text: str = "", name: str = "", parent: QObject | None = None):
        super().__init__(name, parent)
        self._text = str(text)
        self._checked = False

    # -- QML ---------------------------------------------------------------
    def _get_ui_text(self) -> str:
        return self._text

    uiText = Property(str, _get_ui_text, notify=uiChanged)

    def _get_checked(self) -> bool:
        return self._checked

    def _set_checked(self, value: bool) -> None:
        value = bool(value)
        if value == self._checked:
            return
        self._checked = value
        self._touch()
        if not self._blocked:
            self.checkedChanged.emit(value)
            self.toggled.emit(value)
            self.stateChanged.emit(2 if value else 0)

    checked = Property(bool, _get_checked, _set_checked, notify=uiChanged)

    # -- controller / Qt API -----------------------------------------------
    def setText(self, value: str) -> None:                 # noqa: N802
        value = str(value)
        if value != self._text:
            self._text = value
            self._touch()
            if not self._blocked:
                self.textChanged.emit(value)

    def text(self) -> str:                                 # noqa: N802
        return self._text

    @Slot(bool)
    def setChecked(self, value: bool) -> None:              # noqa: N802
        self._set_checked(value)

    def isChecked(self) -> bool:                            # noqa: N802
        return self._checked

    def checkState(self) -> int:                            # noqa: N802
        return 2 if self._checked else 0

    def setCheckState(self, value: int) -> None:            # noqa: N802
        self._set_checked(int(value) != 0)

    def setTristate(self, *a, **k) -> None:                  # noqa: N802
        pass

    def click(self) -> None:
        self._set_checked(not self._checked)
        if not self._blocked:
            self.clicked.emit()

    def toggle(self) -> None:                                # noqa: N802
        self.click()


# --------------------------------------------------------------------------- #
#  combo box
# --------------------------------------------------------------------------- #

class ShCombo(Shim):
    uiChanged = Signal()
    itemsChanged = Signal()
    currentIndexChanged = Signal(int)
    currentTextChanged = Signal(str)
    currentDataChanged = Signal()

    activated = Signal(int)
    textActivated = Signal(str)

    def __init__(self, name: str = "", parent: QObject | None = None):
        super().__init__(name, parent)
        self._items: list[dict] = []
        self._index = -1

    # -- QML ---------------------------------------------------------------
    def _get_ui_items(self) -> list:
        return self._items

    uiItems = Property("QVariantList", _get_ui_items, notify=uiChanged)

    def _get_ui_current_index(self) -> int:
        return self._index

    uiCurrentIndex = Property(int, _get_ui_current_index, notify=uiChanged)

    def _get_ui_current_text(self) -> str:
        return self.itemText(self._index)

    uiCurrentText = Property(str, _get_ui_current_text, notify=uiChanged)

    # -- items --------------------------------------------------------------
    def addItem(self, text: str, data=None) -> None:        # noqa: N802
        if data is None:
            data = text
        self._items.append({"text": str(text), "value": data})
        if self._index < 0:
            self._index = 0
        self._touch()
        if not self._blocked:
            self.itemsChanged.emit()

    def addItems(self, texts) -> None:                      # noqa: N802
        for text in texts:
            self._items.append({"text": str(text), "value": str(text)})
        if self._index < 0 and self._items:
            self._index = 0
        self._touch()
        if not self._blocked:
            self.itemsChanged.emit()

    def clear(self) -> None:                                # noqa: N802
        self._items = []
        self._index = -1
        self._touch()
        if not self._blocked:
            self.itemsChanged.emit()
            self.currentIndexChanged.emit(-1)
            self.currentTextChanged.emit("")
            self.currentDataChanged.emit()

    def count(self) -> int:                                 # noqa: N802
        return len(self._items)

    def itemText(self, index: int) -> str:                  # noqa: N802
        if 0 <= index < len(self._items):
            return self._items[index]["text"]
        return ""

    def itemData(self, index: int):                         # noqa: N802
        if 0 <= index < len(self._items):
            return self._items[index]["value"]
        return None

    def findData(self, value) -> int:                        # noqa: N802
        for i, item in enumerate(self._items):
            if item["value"] == value:
                return i
        return -1

    def findText(self, text: str) -> int:                    # noqa: N802
        for i, item in enumerate(self._items):
            if item["text"] == text:
                return i
        return -1

    # -- selection ----------------------------------------------------------
    @Slot(int)
    def setCurrentIndex(self, index: int) -> None:          # noqa: N802
        index = int(index)
        if index < -1 or index >= len(self._items):
            index = -1
        if index == self._index:
            return
        old_text = self.itemText(self._index)
        self._index = index
        self._touch()
        if self._blocked:
            return
        self.currentIndexChanged.emit(index)
        new_text = self.itemText(index)
        if new_text != old_text:
            self.currentTextChanged.emit(new_text)
        self.currentDataChanged.emit()
        if index >= 0:
            self.activated.emit(index)
            if new_text != old_text:
                self.textActivated.emit(new_text)

    def currentIndex(self) -> int:                          # noqa: N802
        return self._index

    def currentText(self) -> str:                           # noqa: N802
        return self.itemText(self._index)

    def currentData(self):                                  # noqa: N802
        return self.itemData(self._index)

    def setCurrentText(self, text: str) -> None:            # noqa: N802
        index = self.findText(text)
        if index >= 0:
            self.setCurrentIndex(index)

    def setCurrentData(self, value) -> None:                 # noqa: N802
        index = self.findData(value)
        if index >= 0:
            self.setCurrentIndex(index)

    def setEditable(self, *a, **k) -> None:                  # noqa: N802
        pass

    def setMaxVisibleItems(self, *a, **k) -> None:           # noqa: N802
        pass

    def setSizeAdjustPolicy(self, *a, **k) -> None:          # noqa: N802
        pass

    def setPlaceholderText(self, *a, **k) -> None:           # noqa: N802
        pass


# --------------------------------------------------------------------------- #
#  slider
# --------------------------------------------------------------------------- #

class ShSlider(Shim):
    uiChanged = Signal()
    valueChanged = Signal(int)
    sliderMoved = Signal(int)
    rangeChanged = Signal()
    sliderReleased = Signal()

    def __init__(self, name: str = "", parent: QObject | None = None):
        super().__init__(name, parent)
        self._value = 0
        self._minimum = 0
        self._maximum = 100

    # -- QML ---------------------------------------------------------------
    def _get_ui_value(self) -> int:
        return self._value

    uiValue = Property(int, _get_ui_value, notify=uiChanged)

    def _get_ui_minimum(self) -> int:
        return self._minimum

    uiMinimum = Property(int, _get_ui_minimum, notify=uiChanged)

    def _get_ui_maximum(self) -> int:
        return self._maximum

    uiMaximum = Property(int, _get_ui_maximum, notify=uiChanged)

    # -- controller / Qt API -----------------------------------------------
    @Slot(int)
    def setValue(self, value: int) -> None:                 # noqa: N802
        value = int(value)
        value = max(self._minimum, min(self._maximum, value))
        if value == self._value:
            return
        self._value = value
        self._touch()
        if not self._blocked:
            self.valueChanged.emit(value)

    def value(self) -> int:                                 # noqa: N802
        return self._value

    def setMinimum(self, value: int) -> None:               # noqa: N802
        self._minimum = int(value)
        self._touch()
        if not self._blocked:
            self.rangeChanged.emit()
        self.setValue(self._value)

    def minimum(self) -> int:                               # noqa: N802
        return self._minimum

    def setMaximum(self, value: int) -> None:               # noqa: N802
        self._maximum = int(value)
        self._touch()
        if not self._blocked:
            self.rangeChanged.emit()
        self.setValue(self._value)

    def maximum(self) -> int:                               # noqa: N802
        return self._maximum

    def setRange(self, minimum: int, maximum: int) -> None:  # noqa: N802
        self._minimum = int(minimum)
        self._maximum = int(maximum)
        self._touch()
        if not self._blocked:
            self.rangeChanged.emit()
        self.setValue(self._value)

    @Slot()
    def commit(self) -> None:
        """Called from QML when the user lets go of the handle."""
        if not self._blocked:
            self.sliderReleased.emit()

    def setPageStep(self, *a, **k) -> None:                  # noqa: N802
        pass

    def setSingleStep(self, *a, **k) -> None:                # noqa: N802
        pass

    def setTickPosition(self, *a, **k) -> None:              # noqa: N802
        pass

    def setTickInterval(self, *a, **k) -> None:              # noqa: N802
        pass


# --------------------------------------------------------------------------- #
#  line edit
# --------------------------------------------------------------------------- #

class ShEdit(Shim):
    uiChanged = Signal()
    textChanged = Signal(str)
    textEdited = Signal(str)
    placeholderChanged = Signal()
    editingFinished = Signal()
    returnPressed = Signal()
    submitted = Signal()
    pathDropped = Signal(str)
    focusRequested = Signal()

    def __init__(self, text: str = "", name: str = "", parent: QObject | None = None):
        super().__init__(name, parent)
        self._text = str(text)
        self._placeholder = ""
        self._read_only = False

    # -- QML ---------------------------------------------------------------
    def _get_ui_text(self) -> str:
        return self._text

    uiText = Property(str, _get_ui_text, notify=uiChanged)

    def _get_ui_placeholder(self) -> str:
        return self._placeholder

    uiPlaceholder = Property(str, _get_ui_placeholder, notify=uiChanged)

    # -- controller / Qt API -----------------------------------------------
    @Slot(str)
    def setText(self, value: str) -> None:                  # noqa: N802
        value = str(value)
        if value == self._text:
            return
        self._text = value
        self._touch()
        if not self._blocked:
            self.textChanged.emit(value)

    def text(self) -> str:                                  # noqa: N802
        return self._text

    def setPlaceholderText(self, value: str) -> None:       # noqa: N802
        value = str(value)
        if value != self._placeholder:
            self._placeholder = value
            self._touch()
            if not self._blocked:
                self.placeholderChanged.emit()

    def placeholderText(self) -> str:                        # noqa: N802
        return self._placeholder

    def clear(self) -> None:
        self.setText("")

    @Slot()
    def finishEditing(self) -> None:                          # noqa: N802
        """Emitted from QML when the field loses focus / Enter is pressed."""
        if not self._blocked:
            self.editingFinished.emit()

    def setFocus(self) -> None:                              # noqa: N802
        self.focusRequested.emit()

    def setMaxLength(self, *a, **k) -> None:                 # noqa: N802
        pass

    def selectAll(self) -> None:                             # noqa: N802
        pass

    def deselect(self) -> None:                              # noqa: N802
        pass

    def setReadOnly(self, value: bool) -> None:              # noqa: N802
        self._read_only = bool(value)

    def isReadOnly(self) -> bool:                            # noqa: N802
        return self._read_only


# --------------------------------------------------------------------------- #
#  log console
# --------------------------------------------------------------------------- #

class ShLog(Shim):
    uiChanged = Signal()
    plainTextChanged = Signal()
    linesAppended = Signal(list)
    scrollToEndRequested = Signal()
    cleared = Signal()

    def __init__(self, name: str = "", parent: QObject | None = None):
        super().__init__(name, parent)
        self._plain = ""
        self._max_blocks = 5000

    # -- QML ---------------------------------------------------------------
    def _get_ui_plain(self) -> str:
        return self._plain

    uiPlainText = Property(str, _get_ui_plain, notify=uiChanged)

    # -- controller / Qt API -----------------------------------------------
    def appendPlainText(self, text: str) -> None:           # noqa: N802
        text = str(text)
        if not text:
            return
        self._plain += text + "\n"
        if self._plain.count("\n") > self._max_blocks * 2:
            keep = self._plain.splitlines()[-self._max_blocks:]
            self._plain = "\n".join(keep) + "\n"
        self._touch()
        if not self._blocked:
            self.plainTextChanged.emit()
            self.linesAppended.emit(text.splitlines())

    def setPlainText(self, text: str) -> None:               # noqa: N802
        self._plain = str(text)
        self._touch()
        if not self._blocked:
            self.plainTextChanged.emit()

    def toPlainText(self) -> str:                            # noqa: N802
        return self._plain

    def getPlainText(self) -> str:                           # noqa: N802
        return self._plain

    def clear(self) -> None:
        self._plain = ""
        self._touch()
        if not self._blocked:
            self.cleared.emit()
            self.plainTextChanged.emit()

    def setMaximumBlockCount(self, value: int) -> None:      # noqa: N802
        self._max_blocks = int(value)

    def moveCursor(self, *a, **k) -> None:                   # noqa: N802
        self.scrollToEndRequested.emit()

    def append(self, text: str) -> None:                      # noqa: N802
        self.appendPlainText(text)


# --------------------------------------------------------------------------- #
#  containers (kept for controller compatibility)
# --------------------------------------------------------------------------- #

class ShStack(Shim):
    """The old QStackedWidget; only tracks an index for the controller."""

    currentChanged = Signal(int)

    def __init__(self, name: str = "", parent: QObject | None = None):
        super().__init__(name, parent)
        self._widgets: list[QObject] = []
        self._index = -1

    def addWidget(self, widget) -> int:                       # noqa: N802
        self._widgets.append(widget)
        if self._index < 0:
            self._index = 0
        return len(self._widgets) - 1

    def insertWidget(self, index: int, widget) -> int:        # noqa: N802
        self._widgets.insert(index, widget)
        return index

    def removeWidget(self, widget) -> None:                   # noqa: N802
        if widget in self._widgets:
            self._widgets.remove(widget)

    def indexOf(self, widget) -> int:                          # noqa: N802
        try:
            return self._widgets.index(widget)
        except ValueError:
            return -1

    def count(self) -> int:                                    # noqa: N802
        return len(self._widgets)

    def setCurrentIndex(self, index: int) -> None:             # noqa: N802
        if index != self._index:
            self._index = index
            self.currentChanged.emit(index)

    def currentIndex(self) -> int:                             # noqa: N802
        return self._index

    def widget(self, index: int):                               # noqa: N802
        if 0 <= index < len(self._widgets):
            return self._widgets[index]
        return None


class ShList(Shim):
    """Stand-in for a QVBoxLayout used by the controller's list builders."""

    def __init__(self, name: str = "", parent: QObject | None = None):
        super().__init__(name, parent)
        self._items: list[QObject] = []

    def addWidget(self, widget, *a, **k) -> None:             # noqa: N802
        self._items.append(widget)

    def insertWidget(self, index: int, widget) -> None:
        self._items.insert(index, widget)

    def removeWidget(self, widget) -> None:                    # noqa: N802
        if widget in self._items:
            self._items.remove(widget)

    def takeAt(self, index: int):
        if 0 <= index < len(self._items):
            return _LayoutItem(self._items.pop(index))
        return None

    def count(self) -> int:                                    # noqa: N802
        return len(self._items)

    def itemAt(self, index: int):
        if 0 <= index < len(self._items):
            return _LayoutItem(self._items[index])
        return None

    def addStretch(self, *a, **k) -> None:                     # noqa: N802
        pass

    def addLayout(self, *a, **k) -> None:                      # noqa: N802
        pass

    def setContentsMargins(self, *a, **k) -> None:             # noqa: N802
        pass

    def clear_widgets(self) -> None:
        self._items.clear()


class _LayoutItem:
    """Mimics QLayoutItem for ``layout.itemAt(i).widget()`` calls."""

    def __init__(self, widget: QObject):
        self._widget = widget

    def widget(self):
        return self._widget

    def layout(self):
        return None


# --------------------------------------------------------------------------- #
#  patch list model
# --------------------------------------------------------------------------- #

class PatchModel(QAbstractListModel):
    """Rows for the Patches tab, backed by the controller's check shims."""

    NameRole = Qt.UserRole + 1
    AuthorRole = Qt.UserRole + 2
    CategoryRole = Qt.UserRole + 3
    NoteRole = Qt.UserRole + 4
    ShimRole = Qt.UserRole + 5
    WarnRole = Qt.UserRole + 6
    VisibleRole = Qt.UserRole + 7

    visibleCountChanged = Signal()

    def __init__(self, parent: QObject | None = None):
        super().__init__(parent)
        self._rows: list[dict] = []
        self._query = ""
        self._category = "All Categories"

    def set_patches(self, patches: list[dict], checks: dict, warnings: dict) -> None:
        self.beginResetModel()
        self._rows = []
        for patch in patches:
            name = patch["name"]
            self._rows.append({
                "name": name,
                "author": patch.get("author", ""),
                "category": patch.get("category", ""),
                "note": patch.get("note", ""),
                "blob": f"{name} {patch.get('author', '')} {patch.get('note', '')}".lower(),
                "shim": checks.get(name),
                "warn": warnings.get(name),
                "rowVisible": True,
            })
        self.endResetModel()
        self._recompute()
        self.visibleCountChanged.emit()

    def _recompute(self) -> None:
        before = self._visible_count()
        query = self._query.strip().lower()
        for row in self._rows:
            category_ok = self._category == "All Categories" or row["category"] == self._category
            query_ok = not query or query in row["blob"]
            row["rowVisible"] = category_ok and query_ok
        if self._rows:
            self.dataChanged.emit(
                self.index(0, 0), self.index(len(self._rows) - 1, 0),
                [self.VisibleRole],
            )
        if self._visible_count() != before:
            self.visibleCountChanged.emit()

    def _visible_count(self) -> int:
        return sum(1 for row in self._rows if row["rowVisible"])

    visibleCount = Property(int, _visible_count, notify=visibleCountChanged)

    def _total_count(self) -> int:
        return len(self._rows)

    totalCount = Property(int, _total_count, notify=visibleCountChanged)

    def setFilter(self, query: str, category: str) -> None:   # noqa: N802
        self._query = str(query or "")
        self._category = str(category or "All Categories")
        self._recompute()

    def rowCount(self, parent=QModelIndex()) -> int:           # noqa: N802
        return 0 if parent.isValid() else len(self._rows)

    def roleNames(self) -> dict:                               # noqa: N802
        return {
            self.NameRole: b"name",
            self.AuthorRole: b"author",
            self.CategoryRole: b"category",
            self.NoteRole: b"note",
            self.ShimRole: b"shim",
            self.WarnRole: b"warn",
            self.VisibleRole: b"rowVisible",
        }

    def data(self, index: QModelIndex, role: int = Qt.DisplayRole):
        if not index.isValid() or not (0 <= index.row() < len(self._rows)):
            return None
        row = self._rows[index.row()]
        if role == self.NameRole:
            return row["name"]
        if role == self.AuthorRole:
            return row["author"]
        if role == self.CategoryRole:
            return row["category"]
        if role == self.NoteRole:
            return row["note"]
        if role == self.ShimRole:
            return row["shim"]
        if role == self.WarnRole:
            return row["warn"]
        if role == self.VisibleRole:
            return row["rowVisible"]
        return None


# --------------------------------------------------------------------------- #
#  generic record list (saves, backups)
# --------------------------------------------------------------------------- #

class RecordModel(QAbstractListModel):
    """Unfiltered list of dict rows; QML reads ``fields`` and ``shims``."""

    FieldsRole = Qt.UserRole + 1
    ShimMapRole = Qt.UserRole + 2

    countChanged = Signal()

    def __init__(self, parent: QObject | None = None):
        super().__init__(parent)
        self._rows: list[dict] = []

    def set_records(self, rows: list[dict]) -> None:
        self.beginResetModel()
        self._rows = rows
        self.endResetModel()
        self.countChanged.emit()

    def clear(self) -> None:
        self.set_records([])

    def _get_count(self) -> int:
        return len(self._rows)

    count = Property(int, _get_count, notify=countChanged)

    def rowCount(self, parent=QModelIndex()) -> int:           # noqa: N802
        return 0 if parent.isValid() else len(self._rows)

    def roleNames(self) -> dict:                               # noqa: N802
        return {self.FieldsRole: b"fields", self.ShimMapRole: b"shims"}

    def data(self, index: QModelIndex, role: int = Qt.DisplayRole):
        if not index.isValid() or not (0 <= index.row() < len(self._rows)):
            return None
        row = self._rows[index.row()]
        if role == self.FieldsRole:
            return row
        if role == self.ShimMapRole:
            return row.get("_shims", {})
        return None


# --------------------------------------------------------------------------- #
#  keybind capture
# --------------------------------------------------------------------------- #

class ShKeybind(Shim):
    """Mirrors KeybindButton from the widget UI."""

    uiChanged = Signal()
    bindingChanged = Signal()
    displayChanged = Signal()
    listeningChanged = Signal()

    # (action_key, stored_binding, previous_storage)
    key_assigned = Signal(str, str, str)

    def __init__(self, action_key: str, binding: str = "", display: str = "",
                 keyboard_only: bool = False, parent: QObject | None = None):
        super().__init__(action_key, parent)
        self.action_key = action_key
        self._keyboard_only = bool(keyboard_only)
        self._listening = False
        self.current_binding = str(binding)
        self.display_binding = str(display or self._display_for(binding))

    def _display_for(self, binding: str) -> str:
        try:
            from bblauncher_Utils import scancode_display_name
            return scancode_display_name(binding)
        except Exception:
            return str(binding or "None")

    # -- QML ---------------------------------------------------------------
    def _get_ui_binding(self) -> str:
        return self.current_binding

    uiBinding = Property(str, _get_ui_binding, notify=uiChanged)

    def _get_ui_display(self) -> str:
        if self._listening:
            return "< Press Key / Click >" if not self._keyboard_only else "< Press Key >"
        return self.display_binding or "None"

    uiDisplay = Property(str, _get_ui_display, notify=uiChanged)

    def _get_listening(self) -> bool:
        return self._listening

    def _set_listening(self, value: bool) -> None:
        value = bool(value)
        if value != self._listening:
            self._listening = value
            self._touch()
            self.listeningChanged.emit()

    listening = Property(bool, _get_listening, _set_listening, notify=listeningChanged)

    def _get_keyboard_only(self) -> bool:
        return self._keyboard_only

    keyboardOnly = Property(bool, _get_keyboard_only, constant=True)

    def _get_action_key(self) -> str:
        return self.action_key

    actionKey = Property(str, _get_action_key, constant=True)

    # -- API ---------------------------------------------------------------
    def binding(self) -> str:                                # noqa: N802
        return self.current_binding

    def display(self) -> str:                                # noqa: N802
        return self.display_binding

    def set_binding(self, binding: str, display: str | None = None) -> None:
        self.current_binding = str(binding)
        self.display_binding = str(display if display is not None
                                   else self._display_for(binding))
        self._listening = False
        self._touch()
        self.bindingChanged.emit()
        self.displayChanged.emit()
        self.listeningChanged.emit()

    def setBinding(self, binding: str) -> None:              # noqa: N802
        self.set_binding(binding)

    def setText(self, text: str) -> None:                     # noqa: N802
        self.set_binding(text, text)

    def text(self) -> str:                                    # noqa: N802
        return self.display_binding

    def stop_listening(self) -> None:
        if self._listening:
            self._listening = False
            self._touch()
            self.listeningChanged.emit()

    def start_listening(self) -> None:
        self.begin_capture()

    def focusOutEvent(self, *a, **k) -> None:                 # noqa: N802
        self.stop_listening()

    def hideEvent(self, *a, **k) -> None:                     # noqa: N802
        self.stop_listening()

    def cancel(self) -> None:
        self.stop_listening()

    @Slot()
    def begin_capture(self) -> None:
        self._listening = True
        self._touch()
        self.listeningChanged.emit()


# --------------------------------------------------------------------------- #
#  engine binding helpers
# --------------------------------------------------------------------------- #

def export_shims(engine, shims: dict, extra: dict | None = None) -> None:
    """Expose every shim as a named context property for QML bindings."""
    context = engine.rootContext()
    for name, obj in shims.items():
        context.setContextProperty(name, obj)
    for name, obj in (extra or {}).items():
        context.setContextProperty(name, obj)


_QML_TYPES = (
    "Card", "AppButton", "AppField", "AppCombo", "AppCheck",
    "AppSlider", "NavItem", "ScrollPage", "SectionTitle", "Pill",
    "AnimatedStack", "LogConsole", "KeybindRow",
)


def ensure_basic_style() -> None:
    """Force the ``Basic`` Qt Quick Controls style.

    The native Windows style silently ignores the custom ScrollBar and Popup
    delegates in ``qml/``, which breaks scrolling and every dropdown, so this
    is load-bearing rather than cosmetic.  It must run before the first
    control is created; putting it here (next to the components that need it)
    means no entry point can forget it.
    """
    from PySide6.QtQuickControls2 import QQuickStyle

    try:
        if QQuickStyle.name() == "Basic":
            return
    except Exception:
        pass
    QQuickStyle.setStyle("Basic")


def register_qml_types(qml_dir: Path | None = None) -> None:
    """Register Theme as a singleton and every component under ``Launcher 1.0``.

    Registering explicitly (instead of relying on directory discovery) keeps
    the module importable no matter how the app is launched or frozen.
    """
    from PySide6.QtCore import QUrl
    from PySide6.QtQml import qmlRegisterSingletonType, qmlRegisterType

    ensure_basic_style()

    root = Path(qml_dir) if qml_dir is not None else Path(__file__).resolve().parent / "qml"

    qmlRegisterSingletonType(
        QUrl.fromLocalFile(str(root / "Theme.qml")), "Launcher", 1, 0, "Theme"
    )
    for name in _QML_TYPES:
        path = root / f"{name}.qml"
        if path.is_file():
            qmlRegisterType(QUrl.fromLocalFile(str(path)), "Launcher", 1, 0, name)
