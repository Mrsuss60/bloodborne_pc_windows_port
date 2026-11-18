import QtQuick
import QtQuick.Controls

/*  Themed button.

    variant: "primary" | "secondary" | "ghost" | "danger" | "link" | "chip"
*/
Control {
    id: root

    property string text: ""
    property string variant: "secondary"
    property string glyph: ""
    property bool checkable: false
    property bool checked: false
    property int minWidth: 0
    property string toolTipText: ""
    property string fontFamily: ""
    property int fontWeight: -1
    property real fontLetterSpacing: -1

    signal clicked()

    readonly property bool isPrimary: variant === "primary"
    readonly property bool isDanger: variant === "danger"
    readonly property bool isChip: variant === "chip"
    readonly property bool isLink: variant === "link"
    readonly property bool isGhost: variant === "ghost"
    readonly property bool active: checkable ? checked : false
    readonly property bool isHovered: hover.hovered && enabled

    implicitHeight: isChip ? 24 : 30
    implicitWidth: Math.max(minWidth, contentRow.implicitWidth + (isChip ? 22 : 32))
    opacity: enabled ? 1 : 0.4
    scale: press.pressed ? 0.972 : 1

    Behavior on scale { NumberAnimation { duration: Theme.durFast; easing: Theme.easeOut } }
    Behavior on opacity { NumberAnimation { duration: Theme.durFast } }

    HoverHandler { id: hover }

    background: Rectangle {
        radius: root.isChip ? 13 : Theme.radiusSm
        color: {
            if (root.isPrimary)  return root.isHovered ? Qt.lighter(Theme.accent, 1.18) : Theme.accent
            if (root.isDanger)   return root.isHovered ? "#3A1417" : "transparent"
            if (root.isChip)     return root.isHovered ? Theme.surfaceHi : Theme.surfaceAlt
            if (root.isLink)     return "transparent"
            if (root.active)     return Theme.accentSoft
            return root.isHovered ? Theme.surfaceHi : Theme.surfaceAlt
        }
        border.width: root.isLink ? 0 : 1
        border.color: {
            if (root.isPrimary) return root.isHovered ? Theme.accentHi : Qt.darker(Theme.accent, 1.1)
            if (root.isDanger)  return root.isHovered ? Theme.err : "#5A2A2E"
            if (root.isChip)    return root.isHovered ? Theme.borderHi : Theme.border
            if (root.active)    return Theme.accent
            return root.isHovered ? Theme.borderHi : Theme.border
        }

        Behavior on color { ColorAnimation { duration: Theme.durFast } }
        Behavior on border.color { ColorAnimation { duration: Theme.durFast } }

        // primary gets a soft inner glow when hovered
        Rectangle {
            anchors.fill: parent
            anchors.margins: 1
            radius: parent.radius - 1
            visible: root.isPrimary
            gradient: Gradient {
                orientation: Gradient.Vertical
                GradientStop { position: 0.0; color: "#22FFFFFF" }
                GradientStop { position: 0.55; color: "#00FFFFFF" }
            }
        }
    }

    contentItem: Item {
        Row {
            id: contentRow
            anchors.centerIn: parent
            spacing: root.glyph.length > 0 ? 7 : 0

            Text {
                text: root.glyph
                visible: text.length > 0
                anchors.verticalCenter: parent.verticalCenter
                color: root.isPrimary ? "#FFFFFF" : (root.isDanger ? Theme.err : Theme.textMuted)
                font.family: Theme.fontUi
                font.pixelSize: root.isChip ? Theme.fsSmall : Theme.fsBody
            }
            Text {
                text: root.text
                anchors.verticalCenter: parent.verticalCenter
                color: {
                    if (root.isPrimary) return "#FFFFFF"
                    if (root.isDanger)  return Theme.err
                    if (root.active)    return Theme.accentHi
                    if (root.isLink)    return root.isHovered ? Theme.goldHi : Theme.gold
                    return root.isHovered ? Theme.text : (root.isChip ? Theme.textMuted : "#D8D8E0")
                }
                font.family: root.fontFamily.length > 0 ? root.fontFamily : Theme.fontUi
                font.pixelSize: root.isChip ? Theme.fsSmall : Theme.fsBody
                font.weight: root.fontWeight >= 0 ? root.fontWeight : (root.isPrimary ? Font.DemiBold : (root.isChip ? Font.Medium : Font.Medium))
                font.letterSpacing: root.fontLetterSpacing >= 0 ? root.fontLetterSpacing : 0
            }
        }
    }

    MouseArea {
        id: press
        anchors.fill: parent
        enabled: root.enabled
        cursorShape: Qt.PointingHandCursor
        hoverEnabled: false
        onClicked: {
            if (root.checkable)
                root.checked = !root.checked
            root.clicked()
        }
    }

    ToolTip.visible: root.toolTipText.length > 0 && hover.hovered
    ToolTip.text: root.toolTipText
    ToolTip.delay: 600
}
