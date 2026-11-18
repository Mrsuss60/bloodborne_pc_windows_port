import QtQuick
import QtQuick.Layouts
import QtQuick.Controls
import Launcher 1.0

ScrollPage {
    id: page

    component MicroHeading: Text {
        color: Theme.gold
        font.family: Theme.fontUi
        font.pixelSize: Theme.fsMicro
        font.weight: Font.DemiBold
        font.letterSpacing: 1.3
    }

    SectionTitle {
        eyebrow: "Input"
        title: "Mouse & Keyboard"
        subtitle: "Native mouse look with rebindable actions. Bindings are stored as physical key positions."
    }

    Card {
        Layout.fillWidth: true
        title: "Mouse & Keyboard Input Configuration"

        AppCheck {
            shim: chk_mk_enabled
            label: chk_mk_enabled.uiText
        }

        AppSlider { Layout.fillWidth: true; shim: sl_sens_x; caption: "Horizontal Sensitivity:" }
        AppSlider { Layout.fillWidth: true; shim: sl_sens_y; caption: "Vertical Sensitivity:" }
        AppSlider { Layout.fillWidth: true; shim: sl_smoothing; caption: "Mouse Smoothing:" }
        AppSlider { Layout.fillWidth: true; shim: sl_responsiveness; caption: "Responsiveness:" }
        AppSlider { Layout.fillWidth: true; shim: sl_acceleration; caption: "Mouse Acceleration:" }

        GridLayout {
            Layout.fillWidth: true
            Layout.topMargin: 2
            columns: 2
            columnSpacing: 24
            rowSpacing: 6

            AppCheck { shim: chk_inv_x; label: chk_inv_x.uiText }
            AppCheck { shim: chk_inv_y; label: chk_inv_y.uiText }
            AppCheck { shim: chk_mouse_menu; label: chk_mouse_menu.uiText }
        }

        Rectangle {
            Layout.fillWidth: true
            Layout.topMargin: 4
            Layout.bottomMargin: 2
            height: 1
            color: Theme.border
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: 8

            Text {
                text: "Customize Keybinds"
                color: Theme.gold
                font.family: Theme.fontUi
                font.pixelSize: Theme.fsBody
                font.weight: Font.DemiBold
            }

            Item { Layout.fillWidth: true }

            AppButton {
                text: "AZERTY"
                toolTipText: "Set movement keys to ZQSD (AZERTY layout)."
                onClicked: backend.applyKeybindPreset("azerty")
            }
            AppButton {
                text: "QWERTY"
                toolTipText: "Set movement keys to WASD (QWERTY layout)."
                onClicked: backend.applyKeybindPreset("qwerty")
            }
            AppButton {
                text: "Save Input Parameters to bbport.ini"
                onClicked: backend.saveBbportIni()
            }
            AppButton {
                text: "Reset Keybinds to Defaults"
                variant: "danger"
                onClicked: backend.resetKeybinds()
            }
        }

        RowLayout {
            Layout.fillWidth: true
            Layout.topMargin: 2
            spacing: 32

            ColumnLayout {
                Layout.fillWidth: true
                Layout.preferredWidth: 1
                Layout.alignment: Qt.AlignTop
                spacing: 4

                MicroHeading { text: "GAMEPLAY CONTROLS" }

                Repeater {
                    model: gameplayKeybindRows
                    delegate: KeybindRow {
                        required property var modelData
                        Layout.fillWidth: true
                        shim: modelData.shim
                        description: modelData.desc
                    }
                }
            }

            ColumnLayout {
                Layout.fillWidth: true
                Layout.preferredWidth: 1
                Layout.alignment: Qt.AlignTop
                spacing: 4

                MicroHeading { text: "SYSTEM HOTKEYS" }

                Text {
                    Layout.fillWidth: true
                    text: "Overlay, performance HUD, cheat menu and mouse capture. "
                          + "These four are keyboard-only so a mouse attack cannot open a menu."
                    color: Theme.text
                    wrapMode: Text.WordWrap
                }

                Item { Layout.preferredHeight: 2 }

                Repeater {
                    model: systemKeybindRows
                    delegate: KeybindRow {
                        required property var modelData
                        Layout.fillWidth: true
                        shim: modelData.shim
                        description: modelData.desc
                    }
                }
            }
        }
    }
}
