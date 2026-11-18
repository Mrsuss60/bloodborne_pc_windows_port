import QtQuick
import QtQuick.Layouts
import QtQuick.Controls
import Launcher 1.0

Item {
    id: page

    ColumnLayout {
        anchors.fill: parent
        anchors.leftMargin: Theme.gutter
        anchors.rightMargin: Theme.gutter
        anchors.topMargin: 12
        anchors.bottomMargin: 14
        spacing: 8

        // ---------------------------------------------------------- game path
        Card {
            Layout.fillWidth: true
            title: "Bloodborne Game Path"
            contentSpacing: 7

            RowLayout {
                Layout.fillWidth: true
                spacing: 8

                Rectangle {
                    Layout.alignment: Qt.AlignVCenter
                    width: 8
                    height: 8
                    radius: 4
                    color: Theme.toneColor(lbl_path_status.tone)
                    Behavior on color { ColorAnimation { duration: Theme.durMed } }

                    SequentialAnimation on opacity {
                        running: lbl_path_status.tone === "ok"
                        loops: Animation.Infinite
                        NumberAnimation { to: 0.45; duration: 1100; easing: Theme.easeInOut }
                        NumberAnimation { to: 1.0; duration: 1100; easing: Theme.easeInOut }
                    }
                }

                AppField {
                    Layout.fillWidth: true
                    shim: txt_eboot
                    allowDrop: true
                    onDropped: (path) => backend.dropEboot(path)
                }

                AppButton {
                    text: "Browse\u2026"
                    onClicked: backend.browseEboot()
                }
                AppButton {
                    text: "Open Folder"
                    toolTipText: "Open the resolved game folder."
                    onClicked: backend.openGameFolder()
                }
            }

            Text {
                Layout.fillWidth: true
                text: lbl_path_hint.uiText
                color: Theme.toneColor(lbl_path_hint.tone)
                font.family: Theme.fontUi
                font.pixelSize: Theme.fsSmall
                elide: Text.ElideRight
                visible: text.length > 0
            }
        }

        // ---------------------------------------------------------- readiness
        Card {
            Layout.fillWidth: true
            padding: 12
            contentSpacing: 5

            RowLayout {
                Layout.fillWidth: true
                spacing: 8

                Rectangle {
                    Layout.alignment: Qt.AlignVCenter
                    width: 7
                    height: 7
                    radius: 3.5
                    color: Theme.toneColor(lbl_setup_dot.tone)
                    Behavior on color { ColorAnimation { duration: Theme.durMed } }
                }

                Text {
                    Layout.fillWidth: true
                    text: lbl_setup_summary.uiText
                    color: Theme.toneColor(lbl_setup_summary.tone)
                    font.family: Theme.fontUi
                    font.pixelSize: Theme.fsBody
                    font.weight: Font.DemiBold
                    elide: Text.ElideRight
                }

                AppButton {
                    text: "Details"
                    variant: "link"
                    visible: btn_setup_details.visible
                    onClicked: backend.showSetupDetails()
                }
                AppButton {
                    text: "Re-check"
                    onClicked: backend.recheckSetup()
                }
            }

            RowLayout {
                Layout.fillWidth: true
                spacing: 18
                Layout.topMargin: 2

                Text {
                    text: lbl_check_game.uiText
                    color: Theme.toneColor(lbl_check_game.tone)
                    font.family: Theme.fontUi
                    font.pixelSize: Theme.fsSmall
                }
                Text {
                    text: lbl_check_vulkan.uiText
                    color: Theme.toneColor(lbl_check_vulkan.tone)
                    font.family: Theme.fontUi
                    font.pixelSize: Theme.fsSmall
                }
                Text {
                    text: lbl_check_avx.uiText
                    color: Theme.toneColor(lbl_check_avx.tone)
                    font.family: Theme.fontUi
                    font.pixelSize: Theme.fsSmall
                }
                Text {
                    text: lbl_check_fps.uiText
                    color: Theme.toneColor(lbl_check_fps.tone)
                    font.family: Theme.fontUi
                    font.pixelSize: Theme.fsSmall
                }
                Item { Layout.fillWidth: true }
            }
        }

        // ---------------------------------------------------------- launch
        RowLayout {
            Layout.fillWidth: true
            spacing: 10

            AppButton {
                Layout.fillWidth: true
                Layout.preferredHeight: 46
                variant: "primary"
                text: btn_launch.uiText
                enabled: btn_launch.enabled
                fontFamily: Theme.fontNav
                fontWeight: 1000
                fontLetterSpacing: 0.6
                onClicked: backend.launchGame()
            }

            AppButton {
                Layout.preferredWidth: 140
                Layout.preferredHeight: 46
                variant: "danger"
                text: "Stop"
                glyph: "\u25A0"
                enabled: btn_stop.enabled
                onClicked: backend.stopGame()
            }
        }

        Item { Layout.preferredHeight: 8; Layout.fillWidth: true }

        // ---------------------------------------------------------- profile chips
        RowLayout {
            Layout.fillWidth: true
            spacing: 6

            Text {
                text: "LAUNCH PROFILE"
                color: Theme.textDim
                font.family: Theme.fontUi
                font.pixelSize: Theme.fsMicro
                font.weight: Font.DemiBold
                font.letterSpacing: 1.4
            }

            AppButton { variant: "chip"; text: chip_fps.uiText; toolTipText: chip_fps.tooltip; onClicked: backend.switchTab(2) }
            AppButton { variant: "chip"; text: chip_res.uiText; toolTipText: chip_res.tooltip; onClicked: backend.switchTab(2) }
            AppButton { variant: "chip"; text: chip_window.uiText; toolTipText: chip_window.tooltip; onClicked: backend.switchTab(2) }
            AppButton { variant: "chip"; text: chip_upscaler.uiText; toolTipText: chip_upscaler.tooltip; onClicked: backend.switchTab(2) }
            AppButton { variant: "chip"; text: chip_mk.uiText; toolTipText: chip_mk.tooltip; onClicked: backend.switchTab(3) }
            AppButton { variant: "chip"; text: chip_patches.uiText; toolTipText: chip_patches.tooltip; onClicked: backend.switchTab(1) }
            AppButton { variant: "chip"; text: chip_mods.uiText; toolTipText: chip_mods.tooltip; onClicked: backend.switchTab(5) }

            Item { Layout.fillWidth: true }
        }

        // ---------------------------------------------------------- console
        Card {
            Layout.fillWidth: true
            Layout.fillHeight: true
            title: "Engine Console Output"
            padding: 12
            contentSpacing: 6

            Text {
                Layout.fillWidth: true
                text: lbl_last_run.uiText
                color: Theme.toneColor(lbl_last_run.tone)
                font.family: Theme.fontUi
                font.pixelSize: Theme.fsSmall
                elide: Text.ElideRight
            }

            RowLayout {
                Layout.fillWidth: true
                spacing: 6

                AppCombo { Layout.preferredWidth: 172; shim: cmb_log_filter }
                AppCheck { shim: chk_autoscroll; label: "Auto-scroll" }
                Item { Layout.fillWidth: true }
                AppButton { text: "Diagnostics"; toolTipText: "Copy a full diagnostics report to the clipboard"; onClicked: backend.copyDiagnostics() }
                AppButton { text: "Open Log"; onClicked: backend.openLogFile() }
                AppButton { text: "Save Log"; onClicked: backend.saveLog() }
                AppButton { text: "Copy"; onClicked: backend.copyLog() }
                AppButton { text: "Clear"; onClicked: backend.clearLog() }
            }

            LogConsole {
                Layout.fillWidth: true
                Layout.fillHeight: true
                Layout.topMargin: 2
                logShim: log_view
                autoScroll: chk_autoscroll.checked
            }
        }
    }
}
