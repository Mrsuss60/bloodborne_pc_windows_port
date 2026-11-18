import QtQuick
import QtQuick.Controls
import Launcher 1.0

/*  Dropdown bound to a Python combo shim (items: [{text, value}]). */
Item {
    id: root

    property var shim: null
    property string placeholder: "Select..."
    property int popupMaxHeight: 320

    // Timestamp guard: CloseOnPressOutside closes the popup on the press, and
    // the release would otherwise immediately re-open it.
    property double _closedAt: 0

    readonly property int currentIndex: shim ? shim.uiCurrentIndex : -1
    readonly property string currentText: shim ? shim.uiCurrentText : ""
    readonly property var items: shim ? shim.uiItems : []
    readonly property bool opened: pop.opened

    function toggle() {
        if (pop.opened)
            pop.close()
        else if (Date.now() - root._closedAt > 200)
            pop.open()
    }

    implicitHeight: 30
    implicitWidth: 150
    opacity: shim && !shim.enabled ? 0.45 : 1

    Rectangle {
        id: frame
        anchors.fill: parent
        radius: Theme.radiusSm
        color: Theme.surfaceAlt
        border.width: 1
        border.color: pop.opened || hover.hovered ? Theme.accent : Theme.border
        Behavior on border.color { ColorAnimation { duration: Theme.durFast } }

        HoverHandler { id: hover }

        Text {
            id: valueText
            anchors.left: parent.left
            anchors.right: chevron.left
            anchors.verticalCenter: parent.verticalCenter
            anchors.leftMargin: 10
            anchors.rightMargin: 6
            text: root.currentText.length > 0 ? root.currentText : root.placeholder
            color: root.currentText.length > 0 ? Theme.text : Theme.textDim
            elide: Text.ElideRight
            font.family: Theme.fontUi
            font.pixelSize: Theme.fsBody
        }

        Text {
            id: chevron
            anchors.right: parent.right
            anchors.rightMargin: 10
            anchors.verticalCenter: parent.verticalCenter
            text: "\u25BE"
            color: pop.opened ? Theme.gold : Theme.textMuted
            font.pixelSize: 11
            rotation: pop.opened ? 180 : 0
            Behavior on rotation { NumberAnimation { duration: Theme.durMed; easing: Theme.easeOut } }
            Behavior on color { ColorAnimation { duration: Theme.durFast } }
        }

        MouseArea {
            anchors.fill: parent
            cursorShape: Qt.PointingHandCursor
            enabled: root.shim && root.shim.enabled
            onClicked: root.toggle()
        }
    }

    Popup {
        id: pop
        y: root.height + 4
        width: root.width
        padding: 4
        closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
        onClosed: root._closedAt = Date.now()
        transformOrigin: Item.Top
        enter: Transition {
            ParallelAnimation {
                NumberAnimation { property: "opacity"; from: 0; to: 1; duration: Theme.durFast }
                NumberAnimation { property: "scale"; from: 0.97; to: 1; duration: Theme.durMed; easing: Theme.easeOutQuint }
            }
        }
        exit: Transition {
            NumberAnimation { property: "opacity"; from: 1; to: 0; duration: 90 }
        }

        background: Rectangle {
            radius: Theme.radiusSm
            color: Theme.surfaceHi
            border.width: 1
            border.color: Theme.borderHi
        }

        contentItem: ListView {
            id: list
            clip: true
            implicitHeight: Math.min(contentHeight, root.popupMaxHeight)
            model: root.items
            boundsBehavior: Flickable.StopAtBounds
            currentIndex: root.currentIndex
            ScrollBar.vertical: ScrollBar {
                policy: list.contentHeight > list.height ? ScrollBar.AsNeeded : ScrollBar.AlwaysOff
            }

            delegate: Item {
                required property int index
                required property var modelData

                width: list.width
                height: 30

                Rectangle {
                    anchors.fill: parent
                    radius: Theme.radiusSm - 2
                    color: hovered.hovered ? Theme.surface
                           : (index === root.currentIndex ? Theme.accentSoft : "transparent")
                    Behavior on color { ColorAnimation { duration: Theme.durFast } }
                    HoverHandler { id: hovered }
                }

                Text {
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.verticalCenter: parent.verticalCenter
                    anchors.leftMargin: 9
                    anchors.rightMargin: 9
                    text: modelData.text
                    elide: Text.ElideRight
                    color: index === root.currentIndex ? Theme.accentHi : Theme.text
                    font.family: Theme.fontUi
                    font.pixelSize: Theme.fsBody
                }

                MouseArea {
                    anchors.fill: parent
                    cursorShape: Qt.PointingHandCursor
                    onClicked: {
                        if (root.shim)
                            root.shim.setCurrentIndex(index)
                        pop.close()
                    }
                }
            }
        }
    }
}
