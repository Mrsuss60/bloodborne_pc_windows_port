import QtQuick
import Launcher 1.0

Rectangle {
    id: root

    property string text: ""
    property string tone: "muted"     // muted | ok | warn | err | accent | gold

    implicitHeight: 20
    implicitWidth: label.implicitWidth + 16
    radius: 10
    color: Theme.toneSoft(root.tone)
    border.width: 1
    border.color: Qt.rgba(Theme.toneColor(root.tone).r, Theme.toneColor(root.tone).g,
                          Theme.toneColor(root.tone).b, 0.35)

    Text {
        id: label
        anchors.centerIn: parent
        text: root.text
        color: Theme.toneColor(root.tone)
        font.family: Theme.fontUi
        font.pixelSize: Theme.fsMicro
        font.weight: Font.DemiBold
        font.letterSpacing: 0.4
    }
}
