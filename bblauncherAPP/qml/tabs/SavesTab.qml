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

    function characterSummary(chars) {
        if (!chars || chars.length === 0)
            return ""
        var parts = []
        for (var i = 0; i < chars.length; ++i) {
            var c = chars[i]
            parts.push(c.name + "  \u00b7  Lv " + c.level)
        }
        return parts.join("     ")
    }

    SectionTitle {
        eyebrow: "Saves"
        title: "Save Game Manager"
        subtitle: "Inspect the active character save, snapshot it, or roll back to an earlier backup."
    }

    // ------------------------------------------------------------ active save
    Card {
        Layout.fillWidth: true
        title: "Active Game Save"
        hint: "The save data Bloodborne loads and writes to during gameplay."

        RowLayout {
            Layout.fillWidth: true
            spacing: 12

            Rectangle {
                Layout.alignment: Qt.AlignTop
                Layout.topMargin: 5
                width: 8
                height: 8
                radius: 4
                color: Theme.toneColor(lbl_active_save_path.tone)
                Behavior on color { ColorAnimation { duration: Theme.durMed } }
            }

            ColumnLayout {
                Layout.fillWidth: true
                spacing: 3

                Text {
                    Layout.fillWidth: true
                    text: lbl_active_save_path.uiText
                    color: Theme.toneColor(lbl_active_save_path.tone)
                    font.family: Theme.fontUi
                    font.pixelSize: Theme.fsBody
                    font.weight: Font.DemiBold
                    elide: Text.ElideMiddle
                }
                Text {
                    Layout.fillWidth: true
                    text: lbl_active_save_meta.uiText
                    color: Theme.textMuted
                    font.family: Theme.fontUi
                    font.pixelSize: Theme.fsSmall * 1.05
                    font.weight: Font.Medium
                    elide: Text.ElideRight
                }
            }
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: 8

            AppButton {
                variant: "primary"
                text: "Create Backup"
                onClicked: backend.createBackup()
            }
            AppButton {
                text: "Open Folder"
                onClicked: backend.openSavesFolder()
            }
            AppButton {
                text: "Export ZIP"
                onClicked: backend.exportActiveSave()
            }
            AppButton {
                text: "Import Save"
                onClicked: backend.importSave()
            }
            Item { Layout.fillWidth: true }
        }

        Text {
            Layout.fillWidth: true
            visible: lbl_no_characters.visible
            text: lbl_no_characters.uiText
            color: Theme.textDim
            font.family: Theme.fontUi
            font.pixelSize: Theme.fsSmall
            wrapMode: Text.WordWrap
        }

        ListView {
            id: characterList
            Layout.fillWidth: true
            Layout.topMargin: 2
            Layout.preferredHeight: charactersModel.count > 0
                                    ? charactersModel.count * 64 - 4 : 0
            visible: charactersModel.count > 0
            clip: true
            interactive: false
            model: charactersModel
            spacing: 4
            boundsBehavior: Flickable.StopAtBounds

            delegate: Rectangle {
                id: charRow

                required property var fields

                width: characterList.width
                height: 60
                radius: Theme.radiusSm
                color: hover.hovered ? "#191921" : "#14141A"
                border.width: 1
                border.color: hover.hovered ? Theme.borderHi : "#1E1E27"

                Behavior on color { ColorAnimation { duration: Theme.durFast } }
                Behavior on border.color { ColorAnimation { duration: Theme.durFast } }

                HoverHandler { id: hover }

                Rectangle {
                    id: portrait
                    anchors.left: parent.left
                    anchors.leftMargin: 9
                    anchors.verticalCenter: parent.verticalCenter
                    width: 44
                    height: 44
                    radius: Theme.radiusSm - 1
                    color: Theme.surfaceAlt
                    border.width: 1
                    border.color: Theme.border
                    clip: true

                    Image {
                        anchors.fill: parent
                        source: charRow.fields.portrait || ""
                        visible: source.toString().length > 0
                        fillMode: Image.PreserveAspectCrop
                        asynchronous: true
                        cache: false
                    }

                    Text {
                        anchors.centerIn: parent
                        visible: !charRow.fields.portrait
                        text: charRow.fields.name ? charRow.fields.name.charAt(0).toUpperCase() : "?"
                        color: Theme.gold
                        font.family: Theme.fontDisplay
                        font.pixelSize: 18
                        font.weight: Font.DemiBold
                    }
                }

                ColumnLayout {
                    anchors.left: portrait.right
                    anchors.leftMargin: 12
                    anchors.right: stats.left
                    anchors.rightMargin: 12
                    anchors.verticalCenter: parent.verticalCenter
                    spacing: 2

                    Text {
                        Layout.fillWidth: true
                        text: charRow.fields.name || "Unknown Hunter"
                        color: Theme.text
                        font.family: Theme.fontUi
                        font.pixelSize: Theme.fsBody
                        font.weight: Font.DemiBold
                        elide: Text.ElideRight
                    }
                    Text {
                        Layout.fillWidth: true
                        text: charRow.fields.location || ""
                        color: Theme.textMuted
                        font.family: Theme.fontUi
                        font.pixelSize: Theme.fsMicro
                        elide: Text.ElideRight
                    }
                }

                Row {
                    id: stats
                    anchors.right: parent.right
                    anchors.rightMargin: 12
                    anchors.verticalCenter: parent.verticalCenter
                    spacing: 18

                    Column {
                        spacing: 1
                        Text {
                            anchors.right: parent.right
                            text: "LEVEL"
                            color: Theme.textDim
                            font.family: Theme.fontUi
                            font.pixelSize: 9
                            font.letterSpacing: 1.1
                        }
                        Text {
                            anchors.right: parent.right
                            text: charRow.fields.level
                            color: Theme.gold
                            font.family: Theme.fontMono
                            font.pixelSize: Theme.fsBody
                        }
                    }
                    Column {
                        spacing: 1
                        Text {
                            anchors.right: parent.right
                            text: "PLAYTIME"
                            color: Theme.textDim
                            font.family: Theme.fontUi
                            font.pixelSize: 9
                            font.letterSpacing: 1.1
                        }
                        Text {
                            anchors.right: parent.right
                            text: charRow.fields.playtime
                            color: Theme.text
                            font.family: Theme.fontMono
                            font.pixelSize: Theme.fsSmall
                        }
                    }
                    Column {
                        spacing: 1
                        Text {
                            anchors.right: parent.right
                            text: "BLOOD ECHOES"
                            color: Theme.textDim
                            font.family: Theme.fontUi
                            font.pixelSize: 9
                            font.letterSpacing: 1.1
                        }
                        Text {
                            anchors.right: parent.right
                            text: charRow.fields.echoes
                            color: Theme.text
                            font.family: Theme.fontMono
                            font.pixelSize: Theme.fsSmall
                        }
                    }
                }
            }
        }
    }

    // ------------------------------------------------------------- backups
    Card {
        Layout.fillWidth: true
        title: "Saved Backups"
        hint: "Restoring any backup will automatically snapshot your current active save first to prevent progress loss."

        RowLayout {
            Layout.fillWidth: true
            spacing: 10

            Text {
                text: lbl_backup_count.uiText
                color: Theme.gold
                font.family: Theme.fontUi
                font.pixelSize: Theme.fsSmall
                font.weight: Font.DemiBold
            }

            Item { Layout.fillWidth: true }

            AppButton {
                text: "Refresh"
                onClicked: backend.refreshBackups()
            }
        }

        Text {
            Layout.fillWidth: true
            visible: lbl_no_backups.visible
            text: lbl_no_backups.uiText
            color: Theme.textDim
            font.family: Theme.fontUi
            font.pixelSize: Theme.fsSmall
            wrapMode: Text.WordWrap
        }

        ListView {
            id: backupList
            Layout.fillWidth: true
            Layout.preferredHeight: backupsModel.count > 0
                                    ? backupsModel.count * 75 - 5 : 0
            visible: backupsModel.count > 0
            clip: true
            interactive: false
            model: backupsModel
            spacing: 5
            boundsBehavior: Flickable.StopAtBounds
            cacheBuffer: 600

            delegate: Rectangle {
                id: backupRow

                required property var fields

                width: backupList.width - 2
                height: 70
                radius: Theme.radiusSm
                color: hover.hovered ? "#191921" : "#14141A"
                border.width: 1
                border.color: hover.hovered ? Theme.borderHi : "#1E1E27"

                Behavior on color { ColorAnimation { duration: Theme.durFast } }
                Behavior on border.color { ColorAnimation { duration: Theme.durFast } }

                HoverHandler { id: hover }

                Rectangle {
                    id: badge
                    anchors.left: parent.left
                    anchors.leftMargin: 10
                    anchors.verticalCenter: parent.verticalCenter
                    width: badgeText.implicitWidth + 16
                    height: 20
                    radius: 10
                    color: Theme.surfaceAlt
                    border.width: 1
                    border.color: backupRow.fields.badgeColor

                    Text {
                        id: badgeText
                        anchors.centerIn: parent
                        text: backupRow.fields.badge
                        color: backupRow.fields.badgeColor
                        font.family: Theme.fontUi
                        font.pixelSize: 9
                        font.weight: Font.DemiBold
                        font.letterSpacing: 0.9
                    }
                }

                ColumnLayout {
                    anchors.left: badge.right
                    anchors.leftMargin: 10
                    anchors.right: rowButtons.left
                    anchors.rightMargin: 10
                    anchors.verticalCenter: parent.verticalCenter
                    spacing: 2

                    Text {
                        Layout.fillWidth: true
                        text: backupRow.fields.label
                        color: Theme.text
                        font.family: Theme.fontUi
                        font.pixelSize: Theme.fsBody
                        font.weight: Font.DemiBold
                        elide: Text.ElideRight
                    }
                    Text {
                        Layout.fillWidth: true
                        text: backupRow.fields.timestamp + "   \u00b7   " + backupRow.fields.size
                        color: Theme.textMuted
                        font.family: Theme.fontUi
                        font.pixelSize: Theme.fsMicro
                        elide: Text.ElideRight
                    }
                    Text {
                        Layout.fillWidth: true
                        visible: text.length > 0
                        text: page.characterSummary(backupRow.fields.characters)
                        color: Theme.textDim
                        font.family: Theme.fontUi
                        font.pixelSize: Theme.fsMicro
                        elide: Text.ElideRight
                    }
                }

                Row {
                    id: rowButtons
                    anchors.right: parent.right
                    anchors.rightMargin: 10
                    anchors.verticalCenter: parent.verticalCenter
                    spacing: 6

                    AppButton {
                        text: "Restore"
                        variant: "chip"
                        onClicked: backend.restoreBackup(backupRow.fields.path)
                    }
                    AppButton {
                        text: "Export"
                        variant: "chip"
                        onClicked: backend.exportBackup(backupRow.fields.path)
                    }
                    AppButton {
                        text: "Delete"
                        variant: "chip"
                        onClicked: backend.deleteBackup(backupRow.fields.path)
                    }
                }
            }
        }
    }

    // ----------------------------------------------------------- safeguards
    Card {
        Layout.fillWidth: true
        title: "Automatic Safeguards"
        hint: "Protection against accidental New Game+ transitions, save file corruption, or lost progress."

        RowLayout {
            Layout.fillWidth: true
            spacing: 10

            AppCheck {
                Layout.alignment: Qt.AlignVCenter
                shim: chk_auto_backup_launch
                label: chk_auto_backup_launch.uiText
            }

            Item { Layout.fillWidth: true }

            FieldLabel {
                Layout.alignment: Qt.AlignVCenter
                text: "Keep last:"
            }
            AppCombo {
                Layout.preferredWidth: 220
                shim: cmb_max_backups
            }
        }

        AppCheck {
            Layout.topMargin: 2
            shim: chk_engine_safepoint
            label: chk_engine_safepoint.uiText
        }
    }

    Item { Layout.preferredHeight: 2 }
}
