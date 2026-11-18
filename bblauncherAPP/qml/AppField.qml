import QtQuick
import QtQuick.Controls
import Launcher 1.0

/*  Two-way bound text field backed by a Python shim.
    Drop support is enabled when allowDrop is true (eboot path). */
Item {
    id: root

    property var shim: null
    property string placeholder: shim ? shim.uiPlaceholder : ""
    property bool allowDrop: false
    readonly property bool fieldActiveFocus: field.activeFocus

    signal dropped(string path)
    signal accepted()

    implicitHeight: 30
    implicitWidth: 200
    opacity: enabled ? 1 : 0.45

    Rectangle {
        id: frame
        anchors.fill: parent
        radius: Theme.radiusSm
        color: Theme.surfaceAlt
        border.width: 1
        border.color: field.activeFocus ? Theme.accent : (hover.hovered ? Theme.borderHi : Theme.border)

        Behavior on border.color { ColorAnimation { duration: Theme.durFast } }
        HoverHandler { id: hover }

        TextField {
            id: field
            anchors.fill: parent
            anchors.leftMargin: 10
            anchors.rightMargin: 10
            padding: 0
            verticalAlignment: TextInput.AlignVCenter
            color: Theme.text
            placeholderText: root.placeholder
            placeholderTextColor: Theme.textDim
            selectByMouse: true
            font.family: Theme.fontUi
            font.pixelSize: Theme.fsBody
            background: null
            enabled: root.enabled

            onTextEdited: if (root.shim) root.shim.setText(text)
            onEditingFinished: {
                if (root.shim)
                    root.shim.finishEditing()
                root.accepted()
            }
        }
    }

    Connections {
        target: root.shim
        ignoreUnknownSignals: true

        function onUiChanged() {
            if (!root.shim)
                return
            if (field.text !== root.shim.uiText)
                field.text = root.shim.uiText
        }
        function onFocusRequested() {
            field.forceActiveFocus()
        }
    }

    Component.onCompleted: {
        if (root.shim)
            field.text = root.shim.uiText
    }

    DropArea {
        anchors.fill: parent
        enabled: root.allowDrop
        onDropped: (drop) => {
            if (!drop.hasUrls || drop.urls.length === 0)
                return
            const p = drop.urls[0].toString()
            root.dropped(p)
            drop.acceptProposedAction()
        }
    }
}
