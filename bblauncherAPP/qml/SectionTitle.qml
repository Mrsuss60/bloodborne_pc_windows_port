import QtQuick
import QtQuick.Layouts
import Launcher 1.0

Item {
    id: root

    property string title: ""
    property string subtitle: ""
    property string eyebrow: ""

    implicitHeight: layout.implicitHeight
    Layout.fillWidth: true

    ColumnLayout {
        id: layout
        anchors.left: parent.left
        anchors.right: parent.right
        spacing: 2

        Text {
            visible: root.eyebrow.length > 0
            text: root.eyebrow.toUpperCase()
            color: Theme.gold
            font.family: Theme.fontUi
            font.pixelSize: Theme.fsMicro
            font.weight: Font.DemiBold
            font.letterSpacing: 1.6
        }

        Text {
            visible: root.title.length > 0
            text: root.title
            color: Theme.text
            font.family: Theme.fontDisplay
            font.pixelSize: Theme.fsTitle
            font.weight: Font.DemiBold
            font.letterSpacing: 0.2
        }

        Text {
            visible: root.subtitle.length > 0
            text: root.subtitle
            color: Theme.textMuted
            font.family: Theme.fontUi
            font.pixelSize: Theme.fsSmall * 1.05
            font.weight: Font.Medium
        }
    }
}
