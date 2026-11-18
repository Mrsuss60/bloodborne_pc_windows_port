import QtQuick
import QtQuick.Layouts
import QtQuick.Controls
import Launcher 1.0

ScrollPage {
    id: page

    component FieldLabel: Text {
        color: Theme.text
        font.family: Theme.fontUi
        font.pixelSize: Theme.fsSmall * 1.05
        font.weight: Font.Medium
    }

    component PathRow: RowLayout {
        id: pathRow

        property string label: ""
        property var shim: null
        property string which: ""

        spacing: 10

        FieldLabel {
            Layout.preferredWidth: 190
            Layout.alignment: Qt.AlignVCenter
            text: pathRow.label
        }
        AppField {
            Layout.fillWidth: true
            shim: pathRow.shim
        }
        AppButton {
            text: "Browse\u2026"
            onClicked: backend.browseAdvancedPath(pathRow.which)
        }
    }

    SectionTitle {
        eyebrow: "Engine"
        title: "Advanced Settings"
        subtitle: "Launch environment, mod paths and local save data. Blank fields keep the run.bat defaults."
    }

    // ---------------------------------------------------------------- runtime
    Card {
        Layout.fillWidth: true
        title: "Launch & Runtime"
        hint: "Passed to run.bat as environment variables. Blank fields keep the run.bat defaults."

        GridLayout {
            Layout.fillWidth: true
            columns: 4
            columnSpacing: 10
            rowSpacing: 6

            FieldLabel { text: "In-Game Menu Language:" }
            AppCombo { Layout.fillWidth: true; shim: cmb_menu_lang }
            FieldLabel { text: "Content Profile SKU:" }
            AppCombo { Layout.fillWidth: true; shim: cmb_sku }

            FieldLabel { text: "Frame Timeout (seconds):" }
            AppField { Layout.fillWidth: true; shim: txt_timeout }
            FieldLabel { text: "V-Blank Rate (Hz):" }
            AppField { Layout.fillWidth: true; shim: txt_vblank }

            FieldLabel { text: "Frames Ahead:" }
            AppField { Layout.fillWidth: true; shim: txt_frames_ahead }
            FieldLabel { text: "Direct Memory (MiB):" }
            AppField { Layout.fillWidth: true; shim: txt_dmem }

            FieldLabel { text: "Extra run.bat Arguments:" }
            AppField {
                Layout.fillWidth: true
                Layout.columnSpan: 3
                shim: txt_extra_args
            }
        }

        AppCheck {
            Layout.topMargin: 2
            shim: chk_mods
            label: chk_mods.uiText
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: 8

            Item { Layout.fillWidth: true }

            AppButton {
                text: "Save Advanced Settings"
                onClicked: backend.saveAdvancedSettings()
            }
        }
    }

    // ------------------------------------------------------------------ paths
    Card {
        Layout.fillWidth: true
        title: "Mods & External Patches"
        hint: "Blank paths use the defaults inside run.bat (mods/, mods.json, patches/, patches.json)."

        PathRow {
            Layout.fillWidth: true
            label: "Mods Directory:"
            shim: txt_mods_dir
            which: "mods_dir"
        }
        PathRow {
            Layout.fillWidth: true
            label: "Mods Config File:"
            shim: txt_mods_config
            which: "mods_config"
        }
        PathRow {
            Layout.fillWidth: true
            label: "External Patches Directory:"
            shim: txt_patches_dir
            which: "patches_dir"
        }
        PathRow {
            Layout.fillWidth: true
            label: "Patches Config File:"
            shim: txt_patches_config
            which: "patches_config"
        }
    }

    // -------------------------------------------------------------- save data
    Card {
        Layout.fillWidth: true
        title: "Save Data & Launcher Files"
        hint: "User data holds saves and the shader cache. Reload re-reads bbport.ini changed by the game."

        PathRow {
            Layout.fillWidth: true
            label: "User Data Directory:"
            shim: txt_user_dir
            which: "user_dir"
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: 10

            Text {
                Layout.fillWidth: true
                text: lbl_cache_info.uiText
                color: Theme.textMuted
                font.family: Theme.fontUi
                font.pixelSize: Theme.fsSmall * 1.05
                font.weight: Font.Medium
                wrapMode: Text.WordWrap
            }

            AppButton {
                text: "Reset Shader Cache"
                variant: "danger"
                onClicked: backend.resetShaderCache()
            }
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: 8

            Item { Layout.fillWidth: true }

            AppButton {
                text: "Open bbport.ini"
                onClicked: backend.openBbportIni()
            }
            AppButton {
                text: "Reload from bbport.ini"
                onClicked: backend.reloadBbportIni()
            }
        }
    }
}
