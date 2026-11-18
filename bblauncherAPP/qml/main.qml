import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Launcher 1.0

ApplicationWindow {
    id: win

    width: 1260
    height: 797
    minimumWidth: 1260
    maximumWidth: 1260
    minimumHeight: 797
    maximumHeight: 797
    visible: true
    title: "Bloodborne PC Launcher"
    flags: Qt.Window | Qt.FramelessWindowHint
    color: "transparent"

    readonly property int navWidth: 227

    component WinButton: Item {
        id: wb
        property string glyph: ""
        property bool danger: false
        signal clicked()

        width: 46
        height: 38

        Rectangle {
            anchors.fill: parent
            color: hover.hovered ? (wb.danger ? "#C21722" : "#1E1E27") : "transparent"
            Behavior on color { ColorAnimation { duration: Theme.durFast } }

            Text {
                anchors.centerIn: parent
                text: wb.glyph
                color: hover.hovered ? "#FFFFFF" : Theme.textMuted
                font.family: Theme.fontUi
                font.pixelSize: 12
            }
        }

        HoverHandler { id: hover }

        MouseArea {
            anchors.fill: parent
            cursorShape: Qt.PointingHandCursor
            onClicked: wb.clicked()
        }
    }

    // ---------------------------------------------------------------- shell
    Rectangle {
        id: shell
        anchors.fill: parent
        radius: 12
        color: Theme.bg
        border.width: 1
        border.color: win.active ? "#2A2A35" : "#1C1C24"

        opacity: 0
        scale: 0.988

        Behavior on border.color { ColorAnimation { duration: Theme.durSlow } }
        Behavior on opacity { NumberAnimation { duration: 260; easing: Theme.easeOut } }
        Behavior on scale { NumberAnimation { duration: 320; easing: Theme.easeOutQuint } }

        Component.onCompleted: {
            shell.opacity = 1
            shell.scale = 1
        }

        Rectangle {
            anchors.fill: parent
            radius: parent.radius
            gradient: Gradient {
                GradientStop { position: 0.0; color: "#14141A" }
                GradientStop { position: 0.45; color: "#0D0D11" }
                GradientStop { position: 1.0; color: "#0A0A0E" }
            }
        }

        Row {
            anchors.fill: parent
            anchors.margins: 1

            // ------------------------------------------------------ sidebar
            Rectangle {
                id: sidebar
                width: win.navWidth
                height: parent.height
                color: "#101015"
                radius: 11

                ColumnLayout {
                    anchors.fill: parent
                    anchors.topMargin: 18
                    anchors.bottomMargin: 14
                    spacing: 0

                    Column {
                        Layout.leftMargin: 22
                        Layout.rightMargin: 22
                        Layout.fillWidth: true
                        spacing: 3

                        Text {
                            text: "BB LAUNCHER"
                            color: Theme.gold
                            font.family: Theme.fontTitle
                            font.pixelSize: 18
                            font.weight: Font.Bold
                            font.letterSpacing: 2.8
                        }
                        Text {
                            text: "WIN32 / VULKAN PORT"
                            color: Theme.textDim
                            font.family: Theme.fontUi
                            font.pixelSize: Theme.fsMicro
                            font.weight: Font.DemiBold
                            font.letterSpacing: 1.6
                        }
                    }

                    Item { Layout.preferredHeight: 26; Layout.fillWidth: true }

                    NavItem { Layout.fillWidth: true; text: "HOME"; checked: backend.currentTab === 0; onClicked: backend.switchTab(0) }
                    NavItem { Layout.fillWidth: true; text: "PATCHES"; checked: backend.currentTab === 1; onClicked: backend.switchTab(1) }
                    NavItem { Layout.fillWidth: true; text: "DISPLAY & GRAPHICS"; checked: backend.currentTab === 2; onClicked: backend.switchTab(2) }
                    NavItem { Layout.fillWidth: true; text: "MOUSE & KEYBOARD"; checked: backend.currentTab === 3; onClicked: backend.switchTab(3) }
                    NavItem { Layout.fillWidth: true; text: "SAVE MANAGER"; checked: backend.currentTab === 4; onClicked: backend.switchTab(4) }
                    NavItem { Layout.fillWidth: true; text: "ADVANCED SETTINGS"; checked: backend.currentTab === 5; onClicked: backend.switchTab(5) }

                    Item { Layout.fillHeight: true; Layout.fillWidth: true }

                    Rectangle {
                        Layout.leftMargin: 14
                        Layout.rightMargin: 14
                        Layout.fillWidth: true
                        height: 1
                        color: "#1D1D25"
                    }

                    Item { Layout.preferredHeight: 10; Layout.fillWidth: true }

                    RowLayout {
                        Layout.leftMargin: 22
                        Layout.rightMargin: 18
                        Layout.fillWidth: true
                        spacing: 8

                        Rectangle {
                            Layout.alignment: Qt.AlignVCenter
                            width: 7
                            height: 7
                            radius: 3.5
                            color: Theme.toneColor(lbl_status.tone)

                            SequentialAnimation on opacity {
                                running: lbl_status.tone === "accent" || lbl_status.tone === "ok"
                                loops: Animation.Infinite
                                NumberAnimation { to: 0.3; duration: 900; easing: Theme.easeInOut }
                                NumberAnimation { to: 1.0; duration: 900; easing: Theme.easeInOut }
                            }
                        }

                        Text {
                            Layout.fillWidth: true
                            text: lbl_status.uiText.replace(/^[\u25cf\u25cb\u25aa\u2022\u00b7]\s*/, "")
                            color: Theme.toneColor(lbl_status.tone)
                            font.family: Theme.fontUi
                            font.pixelSize: Theme.fsBody
                            font.weight: Font.DemiBold
                            elide: Text.ElideRight
                        }

                        Text {
                            Layout.alignment: Qt.AlignVCenter
                            visible: lbl_run_clock.visible
                            text: lbl_run_clock.uiText
                            color: Theme.toneColor(lbl_status.tone)
                            font.family: Theme.fontUi
                            font.pixelSize: Theme.fsBody
                            font.weight: Font.DemiBold
                        }
                    }

                    Item { Layout.preferredHeight: 5; Layout.fillWidth: true }

                    Text {
                        Layout.leftMargin: 22
                        Layout.rightMargin: 14
                        Layout.fillWidth: true
                        text: lbl_version.uiText
                        color: Theme.textDim
                        font.family: Theme.fontUi
                        font.pixelSize: Theme.fsSmall
                        elide: Text.ElideRight
                    }
                }
            }

            // ------------------------------------------------------ content
            Column {
                width: parent.width - win.navWidth
                height: parent.height

                Item {
                    id: titleBar
                    width: parent.width
                    height: 38

                    DragHandler {
                        target: null
                        acceptedButtons: Qt.LeftButton
                        onActiveChanged: if (active) win.startSystemMove()
                    }

                    Text {
                        anchors.left: parent.left
                        anchors.leftMargin: 20
                        anchors.verticalCenter: parent.verticalCenter
                        text: "Bloodborne PC Windows Launcher   \u00B7   " + lbl_version.uiText
                        color: Theme.textDim
                        font.family: Theme.fontUi
                        font.pixelSize: Theme.fsSmall
                        font.weight: Font.Medium
                    }

                    Row {
                        anchors.right: parent.right
                        anchors.top: parent.top
                        height: parent.height

                        WinButton { glyph: "\u2212"; onClicked: win.showMinimized() }
                        WinButton { glyph: "\u2715"; danger: true; onClicked: backend.requestClose() }
                    }
                }

                AnimatedStack {
                    id: pages
                    width: parent.width
                    height: parent.height - titleBar.height
                    sources: [
                        "tabs/HomeTab.qml",
                        "tabs/PatchesTab.qml",
                        "tabs/DisplayTab.qml",
                        "tabs/MouseKeyboardTab.qml",
                        "tabs/SavesTab.qml",
                        "tabs/AdvancedTab.qml"
                    ]
                    currentIndex: backend.currentTab
                }
            }
        }

        // ------------------------------------------------------ toast
        Rectangle {
            id: toast
            anchors.horizontalCenter: parent.horizontalCenter
            anchors.bottom: parent.bottom
            anchors.bottomMargin: 22
            height: 36
            width: toastText.implicitWidth + 30
            radius: 18
            color: "#F0141418"
            border.width: 1
            border.color: Theme.borderHi
            opacity: 0
            visible: opacity > 0.01

            Behavior on opacity { NumberAnimation { duration: Theme.durMed } }

            Text {
                id: toastText
                anchors.centerIn: parent
                text: ""
                color: Theme.text
                font.family: Theme.fontUi
                font.pixelSize: Theme.fsBody
            }

            function show(message) {
                toastText.text = message
                toast.opacity = 1
                toastTimer.restart()
            }

            Timer {
                id: toastTimer
                interval: 2200
                onTriggered: toast.opacity = 0
            }
        }
    }

    Connections {
        target: backend
        ignoreUnknownSignals: true
        function onToast(message) { toast.show(message) }
    }
}
