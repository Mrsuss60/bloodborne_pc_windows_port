import QtQuick
import QtQuick.Controls
import Launcher 1.0

/*  One keybind capture row.

    Mirrors the old KeybindButton: click to listen, press a key (or mouse
    button) to bind, Escape cancels, Delete clears the binding and Shift on
    its own binds on release.
*/
Item {
    id: root

    property var shim: null
    property string description: ""

    // Shift is only bound when it is released without another key press.
    property int pendingShiftScan: 0

    readonly property bool listening: shim ? shim.listening : false
    readonly property string bindingLabel: shim ? shim.uiDisplay : "None"
    readonly property bool keyboardOnly: shim ? shim.keyboardOnly : false

    implicitHeight: 30
    implicitWidth: 300
    focus: false

    Rectangle {
        id: frame
        anchors.fill: parent
        radius: Theme.radiusSm
        color: root.listening ? Theme.accentSoft
                              : (hover.hovered ? "#191921" : "transparent")
        border.width: 1
        border.color: root.listening ? Theme.accentHi
                                     : (hover.hovered ? Theme.borderHi : "transparent")

        Behavior on color { ColorAnimation { duration: Theme.durFast } }
        Behavior on border.color { ColorAnimation { duration: Theme.durFast } }
    }

    HoverHandler { id: hover }

    Text {
        id: descText
        anchors.left: parent.left
        anchors.leftMargin: 9
        anchors.right: keyCap.left
        anchors.rightMargin: 8
        anchors.verticalCenter: parent.verticalCenter
        text: root.description
        color: Theme.text
        font.family: Theme.fontUi
        font.pixelSize: Theme.fsSmall
        elide: Text.ElideRight
        Behavior on color { ColorAnimation { duration: Theme.durFast } }
    }

    Rectangle {
        id: keyCap
        anchors.right: parent.right
        anchors.rightMargin: 4
        anchors.verticalCenter: parent.verticalCenter
        width: Math.max(152, root.width - 244)
        height: 24
        radius: Theme.radiusSm - 2
        color: root.listening ? Theme.accent : Theme.surfaceAlt
        border.width: 1
        border.color: root.listening ? Theme.accentHi : Theme.border

        Behavior on color { ColorAnimation { duration: Theme.durFast } }
        Behavior on border.color { ColorAnimation { duration: Theme.durFast } }

        Text {
            anchors.centerIn: parent
            width: parent.width - 14
            text: root.bindingLabel
            color: root.listening ? "#FFFFFF" : Theme.text
            horizontalAlignment: Text.AlignHCenter
            elide: Text.ElideRight
            font.family: Theme.fontMono
            font.pixelSize: Theme.fsSmall
            font.weight: root.listening ? Font.DemiBold : Font.Normal

            SequentialAnimation on opacity {
                running: root.listening
                loops: Animation.Infinite
                NumberAnimation { to: 0.55; duration: 640; easing: Theme.easeInOut }
                NumberAnimation { to: 1.0; duration: 640; easing: Theme.easeInOut }
            }
        }
    }

    MouseArea {
        anchors.fill: parent
        cursorShape: Qt.PointingHandCursor
        acceptedButtons: Qt.LeftButton | Qt.RightButton | Qt.MiddleButton
        enabled: root.shim && root.shim.enabled

        onPressed: (mouse) => {
            if (!root.shim)
                return
            if (!root.shim.listening) {
                root.pendingShiftScan = 0
                root.forceActiveFocus()
                root.shim.begin_capture()
                return
            }
            // Keyboard-only hotkeys ignore mouse buttons, like the legacy button.
            if (root.keyboardOnly)
                return
            var shift = (mouse.modifiers & Qt.ShiftModifier) !== 0
            backend.captureMouse(root.shim.actionKey, mouse.button, shift)
        }
    }

    Keys.onPressed: (event) => {
        if (!root.shim || !root.shim.listening)
            return

        if (event.key === Qt.Key_Shift) {
            root.pendingShiftScan = event.nativeScanCode
            event.accepted = true
            return
        }
        if (event.key === Qt.Key_Meta) {
            event.accepted = true
            return
        }
        if (event.key === Qt.Key_Escape) {
            root.pendingShiftScan = 0
            backend.cancelKeybind(root.shim.actionKey)
            event.accepted = true
            return
        }
        if (event.key === Qt.Key_Delete) {
            root.pendingShiftScan = 0
            backend.clearKeybind(root.shim.actionKey)
            event.accepted = true
            return
        }

        root.pendingShiftScan = 0
        backend.captureKey(root.shim.actionKey, event.key,
                           event.nativeScanCode, event.text)
        event.accepted = true
    }

    Keys.onReleased: (event) => {
        if (!root.shim || !root.shim.listening)
            return
        if (event.key === Qt.Key_Shift && !event.isAutoRepeat && root.pendingShiftScan > 0) {
            var scan = root.pendingShiftScan
            root.pendingShiftScan = 0
            backend.captureShift(root.shim.actionKey, scan)
            event.accepted = true
        }
    }

    // Leaving the row while it is armed cancels the capture, like the old
    // focusOutEvent handler.
    onActiveFocusChanged: {
        if (!activeFocus && root.shim && root.shim.listening)
            backend.cancelKeybind(root.shim.actionKey)
    }

    ToolTip.visible: hover.hovered && root.shim && root.shim.tooltip.length > 0
    ToolTip.text: root.shim ? root.shim.tooltip : ""
    ToolTip.delay: 600
}
