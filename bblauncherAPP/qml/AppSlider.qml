import QtQuick
import QtQuick.Controls
import Launcher 1.0

/*  Slider bound to a Python slider shim.
    The shim stores an integer; `scale` converts it to the displayed float. */
Item {
    id: root

    property var shim: null
    property string caption: ""
    property int scale: 100
    property int decimals: 2
    property string suffix: ""
    property bool signed: false
    property bool showValue: true

    readonly property real value: shim ? shim.uiValue / scale : 0
    readonly property real minimum: shim ? shim.uiMinimum / scale : 0
    readonly property real maximum: shim ? shim.uiMaximum / scale : 1
    readonly property string valueText: {
        var v = root.value
        var s = v.toFixed(root.decimals)
        if (root.signed && v > 0)
            s = "+" + s
        return s + root.suffix
    }

    implicitHeight: 34
    implicitWidth: 220
    opacity: shim && shim.enabled ? 1 : 0.45

    Text {
        id: captionText
        anchors.left: parent.left
        anchors.top: parent.top
        text: root.caption
        color: Theme.text
        font.family: Theme.fontUi
        font.pixelSize: Theme.fsSmall * 1.05
        font.weight: Font.Medium
    }

    Text {
        anchors.right: parent.right
        anchors.top: parent.top
        visible: root.showValue
        text: root.valueText
        color: dragArea.pressed ? Theme.accentHi : Theme.text
        font.family: Theme.fontMono
        font.pixelSize: Theme.fsSmall
        Behavior on color { ColorAnimation { duration: Theme.durFast } }
    }

    Item {
        id: groove
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        height: 18

        readonly property real ratio: root.maximum > root.minimum
                                     ? Math.max(0, Math.min(1, (root.value - root.minimum) / (root.maximum - root.minimum)))
                                     : 0

        Rectangle {
            anchors.verticalCenter: parent.verticalCenter
            width: parent.width
            height: 4
            radius: 2
            color: "#23232C"
        }

        Rectangle {
            anchors.verticalCenter: parent.verticalCenter
            width: Math.max(4, groove.ratio * parent.width)
            height: 4
            radius: 2
            color: dragArea.pressed ? Theme.accentHi : Theme.accent
            Behavior on color { ColorAnimation { duration: Theme.durFast } }
        }

        Rectangle {
            id: handle
            width: dragArea.pressed || hover.hovered ? 16 : 13
            height: width
            radius: width / 2
            x: Math.max(-width / 2, Math.min(groove.width - width / 2, groove.ratio * groove.width - width / 2))
            anchors.verticalCenter: parent.verticalCenter
            color: "#EDEDF2"
            border.width: 2
            border.color: dragArea.pressed ? Theme.accentHi : Theme.accent

            Behavior on width { NumberAnimation { duration: Theme.durFast; easing: Theme.easeOut } }
            Behavior on border.color { ColorAnimation { duration: Theme.durFast } }

            Rectangle {
                anchors.centerIn: parent
                width: parent.width + 12
                height: width
                radius: width / 2
                color: Theme.accentGlow
                opacity: dragArea.pressed || hover.hovered ? 1 : 0
                z: -1
                Behavior on opacity { NumberAnimation { duration: Theme.durMed } }
            }
        }

        HoverHandler { id: hover }

        MouseArea {
            id: dragArea
            anchors.fill: parent
            anchors.margins: -4
            enabled: root.shim && root.shim.enabled
            cursorShape: Qt.PointingHandCursor
            preventStealing: true

            function applyAt(localX) {
                if (!root.shim)
                    return
                // the MouseArea is expanded 4px past the groove on every side
                var px = Math.max(0, Math.min(groove.width, localX - 4))
                var r = groove.width > 0 ? px / groove.width : 0
                var raw = root.minimum + r * (root.maximum - root.minimum)
                root.shim.setValue(Math.round(raw * root.scale))
            }

            onPressed: (mouse) => applyAt(mouse.x)
            onPositionChanged: (mouse) => {
                if (pressed)
                    applyAt(mouse.x)
            }
            onReleased: {
                if (root.shim)
                    root.shim.commit()
            }
        }
    }
}
