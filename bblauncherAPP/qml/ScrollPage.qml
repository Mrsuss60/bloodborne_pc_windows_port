import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Launcher 1.0

/*  Vertically scrolling page.  Children go into the internal ColumnLayout. */
Item {
    id: root

    property int sideMargin: Theme.gutter
    property int topMargin: 10
    property int bottomMargin: 12
    property int spacing: Theme.gap

    default property alias content: column.data

    Flickable {
        id: flick
        anchors.fill: parent
        contentWidth: width
        contentHeight: column.implicitHeight + root.topMargin + root.bottomMargin
        clip: true
        boundsBehavior: Flickable.StopAtBounds
        maximumFlickVelocity: 3200
        flickDeceleration: 2400

        ScrollBar.vertical: ScrollBar {
            policy: flick.contentHeight > flick.height ? ScrollBar.AsNeeded : ScrollBar.AlwaysOff
            width: 8
            padding: 2
            contentItem: Rectangle {
                implicitWidth: 4
                radius: 2
                color: parent.pressed ? Theme.borderHi : "#3A3A48"
                opacity: parent.policy === ScrollBar.AlwaysOff ? 0 : (parent.active ? 0.9 : 0.5)
                Behavior on opacity { NumberAnimation { duration: Theme.durMed } }
            }
            background: Item {}
        }

        ColumnLayout {
            id: column
            x: root.sideMargin
            y: root.topMargin
            width: flick.width - root.sideMargin * 2
            spacing: root.spacing
        }
    }
}
