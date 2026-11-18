import QtQuick
import QtQuick.Controls
import Launcher 1.0

/*  Checkbox bound to a Python check shim. */
Item {
    id: root

    property var shim: null
    property string label: ""
    property bool checked: shim ? shim.checked : false
    property bool boxEnabled: shim ? shim.enabled : true
    property string toolTipText: shim ? shim.tooltip : ""

    signal toggled(bool checked)

    implicitHeight: Math.max(20, labelText.implicitHeight)
    implicitWidth: box.width + (label.length > 0 ? 8 + labelText.implicitWidth : 0)
    opacity: boxEnabled ? 1 : 0.5

    Rectangle {
        id: box
        width: 18
        height: 18
        anchors.verticalCenter: parent.verticalCenter
        radius: 4
        color: root.checked ? Theme.accent : Theme.surfaceAlt
        border.width: 1
        border.color: root.checked ? Theme.accentHi
                     : (hover.hovered ? Theme.borderHi : "#3A3A48")

        Behavior on color { ColorAnimation { duration: Theme.durFast } }
        Behavior on border.color { ColorAnimation { duration: Theme.durFast } }

        Text {
            anchors.centerIn: parent
            text: "\u2713"
            color: "#FFFFFF"
            font.pixelSize: 12
            font.bold: true
            opacity: root.checked ? 1 : 0
            scale: root.checked ? 1 : 0.6
            Behavior on opacity { NumberAnimation { duration: Theme.durFast } }
            Behavior on scale { NumberAnimation { duration: Theme.durMed; easing: Theme.easeOutQuint } }
        }
    }

    Text {
        id: labelText
        anchors.left: box.right
        anchors.leftMargin: 8
        anchors.verticalCenter: parent.verticalCenter
        width: Math.max(0, root.width - box.width - 8)
        elide: Text.ElideRight
        visible: root.label.length > 0
        text: root.label
        color: hover.hovered && root.boxEnabled ? Theme.text : "#D2D2DA"
        font.family: Theme.fontUi
        font.pixelSize: Theme.fsBody
    }

    HoverHandler { id: hover }

    MouseArea {
        anchors.fill: parent
        cursorShape: root.boxEnabled ? Qt.PointingHandCursor : Qt.ArrowCursor
        enabled: root.boxEnabled

        onClicked: {
            if (!root.shim)
                return
            root.shim.setChecked(!root.shim.checked)
            root.toggled(root.shim.checked)
        }
    }

    ToolTip.visible: toolTipText.length > 0 && hover.hovered
    ToolTip.text: toolTipText
    ToolTip.delay: 500
}
