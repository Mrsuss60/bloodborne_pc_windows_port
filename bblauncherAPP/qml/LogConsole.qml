import QtQuick
import QtQuick.Layouts
import Launcher 1.0

/*  Read-only monospace console backed by the Python log shim. */
Item {
    id: root

    property var logShim: null
    property bool autoScroll: true

    Rectangle {
        id: frame
        anchors.fill: parent
        radius: Theme.radiusSm
        color: "#09090C"
        border.width: 1
        border.color: "#1E1E27"
        clip: true

        Flickable {
            id: flick
            anchors.fill: parent
            anchors.margins: 1
            anchors.rightMargin: 9
            contentWidth: width
            contentHeight: Math.max(area.contentHeight, height)
            clip: true
            boundsBehavior: Flickable.StopAtBounds
            flickDeceleration: 2600

            TextEdit {
                id: area
                width: flick.width
                padding: 8
                readOnly: true
                selectByMouse: true
                persistentSelection: true
                textFormat: TextEdit.PlainText
                wrapMode: TextEdit.Wrap
                text: root.logShim ? root.logShim.uiPlainText : ""
                color: "#C9CBD4"
                selectionColor: Qt.rgba(Theme.accent.r, Theme.accent.g, Theme.accent.b, 0.5)
                selectedTextColor: "#FFFFFF"
                font.family: Theme.fontMono
                font.pixelSize: Theme.fsSmall
            }
        }

        // slim, non-interactive scroll indicator
        Rectangle {
            visible: flick.contentHeight > flick.height + 1
            width: 4
            radius: 2
            color: "#3A3A48"
            x: parent.width - 8
            height: Math.max(24, flick.height * (flick.height / flick.contentHeight))
            y: 4 + (flick.height - height - 8) * (flick.contentY / Math.max(1, flick.contentHeight - flick.height))
        }
    }

    Connections {
        target: root.logShim
        ignoreUnknownSignals: true

        function onPlainTextChanged() {
            if (!root.autoScroll)
                return
            Qt.callLater(function () {
                flick.contentY = Math.max(0, flick.contentHeight - flick.height)
            })
        }
    }

    function scrollToEnd() {
        flick.contentY = Math.max(0, flick.contentHeight - flick.height)
    }
}
