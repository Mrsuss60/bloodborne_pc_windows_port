import QtQuick
import QtQuick.Layouts

/*  A themed content panel.

    Children are appended to the internal ColumnLayout, so they may use
    Layout.* attached properties.  Set Layout.fillWidth: true on them.
*/
Rectangle {
    id: root

    property string title: ""
    property string hint: ""
    property int padding: 12
    property int contentSpacing: 6
    property bool interactive: true
    property bool highlighted: false

    default property alias contentData: column.data

    readonly property bool isHovered: hover.hovered

    implicitHeight: column.implicitHeight + padding * 2
    implicitWidth: column.implicitWidth + padding * 2

    radius: Theme.radiusMd
    color: Theme.surface
    border.width: 1
    border.color: (root.isHovered && root.interactive) || root.highlighted
                  ? Theme.borderHi : Theme.border
    clip: false

    Behavior on border.color {
        ColorAnimation { duration: Theme.durMed; easing: Theme.easeOut }
    }

    // very subtle top sheen so flat panels do not look dead
    Rectangle {
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.margins: 1
        height: 1
        radius: 1
        color: (root.isHovered && root.interactive) ? "#14FFFFFF" : "#0AFFFFFF"
        Behavior on color { ColorAnimation { duration: Theme.durMed } }
    }

    HoverHandler { id: hover }

    ColumnLayout {
        id: column
        anchors.fill: parent
        anchors.margins: root.padding
        spacing: root.contentSpacing

        RowLayout {
            Layout.fillWidth: true
            spacing: 8
            visible: root.title.length > 0

            Text {
                text: root.title
                color: Theme.gold
                font.family: Theme.fontUi
                font.pixelSize: Theme.fsBody
                font.weight: Font.DemiBold
            }
            Item { Layout.fillWidth: true }
        }

        Text {
            Layout.fillWidth: true
            visible: root.hint.length > 0
            text: root.hint
            color: Theme.textMuted
            font.family: Theme.fontUi
            font.pixelSize: Theme.fsSmall * 1.05
            font.weight: Font.Medium
            wrapMode: Text.WordWrap
        }
    }
}
