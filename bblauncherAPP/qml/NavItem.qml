import QtQuick
import Launcher 1.0

/*  Sidebar navigation row with an animated accent rail. */
Item {
    id: root

    property string text: ""
    property string glyph: ""
    property bool checked: false

    signal clicked()

    implicitHeight: 44
    implicitWidth: 220

    Rectangle {
        id: bg
        anchors.fill: parent
        anchors.leftMargin: 10
        anchors.rightMargin: 10
        radius: Theme.radiusSm
        color: root.checked ? Theme.accentSoft : (hover.hovered ? "#15151C" : "transparent")
        Behavior on color { ColorAnimation { duration: Theme.durFast } }

        HoverHandler { id: hover }

        Rectangle {
            id: rail
            width: 3
            radius: 1.5
            color: Theme.accent
            anchors.left: parent.left
            anchors.verticalCenter: parent.verticalCenter
            height: root.checked ? parent.height - 14 : 0
            opacity: root.checked ? 1 : 0
            Behavior on height { NumberAnimation { duration: Theme.durMed; easing: Theme.easeOutQuint } }
            Behavior on opacity { NumberAnimation { duration: Theme.durMed } }
        }

        Text {
            id: glyphText
            anchors.left: parent.left
            anchors.leftMargin: 14
            anchors.verticalCenter: parent.verticalCenter
            text: root.glyph
            visible: text.length > 0
            color: root.checked ? Theme.accentHi : (hover.hovered ? Theme.text : Theme.textDim)
            font.family: "Segoe UI Symbol"
            font.pixelSize: 15
            Behavior on color { ColorAnimation { duration: Theme.durFast } }
        }

        Text {
            anchors.left: glyphText.visible ? glyphText.right : parent.left
            anchors.leftMargin: glyphText.visible ? 10 : 16
            anchors.right: parent.right
            anchors.rightMargin: 10
            anchors.verticalCenter: parent.verticalCenter
            text: root.text
            elide: Text.ElideRight
            color: root.checked ? Theme.accentHi : (hover.hovered ? Theme.text : Theme.textMuted)
            font.family: Theme.fontNav
            font.pixelSize: 14
            font.weight: root.checked ? Font.Bold : Font.DemiBold
            font.letterSpacing: 0.8
            Behavior on color { ColorAnimation { duration: Theme.durFast } }
        }

        MouseArea {
            anchors.fill: parent
            cursorShape: Qt.PointingHandCursor
            onClicked: root.clicked()
        }
    }
}
