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
        anchors.topMargin: 14
        anchors.bottomMargin: 18
        spacing: 10

        SectionTitle {
            eyebrow: "Engine"
            title: "Game Engine & Community Patches"
            subtitle: "Toggle XML patches shipped with the port. Changes are written straight to Bloodborne.xml."
        }

        // ---------------------------------------------------------- filters
        Card {
            Layout.fillWidth: true
            padding: 10
            contentSpacing: 0

            RowLayout {
                Layout.fillWidth: true
                spacing: 10

                AppField {
                    Layout.fillWidth: true
                    shim: txt_patch_search
                }

                AppCombo {
                    Layout.preferredWidth: 190
                    shim: cmb_patch_cat
                }

                AppButton {
                    text: "Recommended Defaults"
                    toolTipText: "Reset the patch list to the recommended configuration"
                    onClicked: backend.applyRecommendedPatches()
                }

                Text {
                    text: lbl_patch_count.uiText
                    color: Theme.gold
                    font.family: Theme.fontUi
                    font.pixelSize: Theme.fsSmall
                    font.weight: Font.DemiBold
                    Layout.leftMargin: 4
                }
            }
        }

        // ---------------------------------------------------------- patch list
        Rectangle {
            Layout.fillWidth: true
            Layout.fillHeight: true
            radius: Theme.radiusMd
            color: "#101015"
            border.width: 1
            border.color: Theme.border

            ListView {
                id: patchList
                anchors.fill: parent
                anchors.margins: 8
                clip: true
                model: patchModel
                spacing: 0
                boundsBehavior: Flickable.StopAtBounds
                cacheBuffer: 800

                ScrollBar.vertical: ScrollBar {
                    policy: patchList.contentHeight > patchList.height ? ScrollBar.AsNeeded : ScrollBar.AlwaysOff
                    width: 8
                }

                delegate: Item {
                    id: row

                    required property int index
                    required property string name
                    required property string author
                    required property string category
                    required property string note
                    required property var shim
                    required property var warn
                    required property bool rowVisible

                    width: patchList.width - 2
                    height: rowVisible ? Math.max(46, body.implicitHeight + 18) + 6 : 0
                    visible: rowVisible

                    Rectangle {
                        id: card
                        anchors.fill: parent
                        anchors.bottomMargin: 6
                        radius: Theme.radiusSm
                        color: hover.hovered ? "#191921" : "#14141A"
                        border.width: 1
                        border.color: hover.hovered ? Theme.borderHi : "#1E1E27"

                        Behavior on color { ColorAnimation { duration: Theme.durFast } }
                        Behavior on border.color { ColorAnimation { duration: Theme.durFast } }

                        HoverHandler { id: hover }

                        ColumnLayout {
                            id: body
                            anchors.left: parent.left
                            anchors.right: parent.right
                            anchors.verticalCenter: parent.verticalCenter
                            anchors.leftMargin: 12
                            anchors.rightMargin: 12
                            spacing: 3

                            RowLayout {
                                Layout.fillWidth: true
                                spacing: 8

                                AppCheck {
                                    shim: row.shim
                                    label: row.name
                                    Layout.maximumWidth: body.width * 0.62
                                }

                                Text {
                                    visible: row.warn ? row.warn.visible : false
                                    text: row.warn ? row.warn.uiText : ""
                                    color: Theme.warn
                                    font.family: Theme.fontUi
                                    font.pixelSize: Theme.fsMicro
                                    font.weight: Font.DemiBold
                                }

                                Item { Layout.fillWidth: true }

                                Text {
                                    text: "by " + row.author
                                    color: Theme.textDim
                                    font.family: Theme.fontUi
                                    font.pixelSize: Theme.fsMicro
                                }

                                Text {
                                    text: "[" + row.category + "]"
                                    color: Theme.gold
                                    font.family: Theme.fontUi
                                    font.pixelSize: Theme.fsMicro
                                    font.weight: Font.DemiBold
                                }
                            }

                            Text {
                                Layout.fillWidth: true
                                visible: row.note.length > 0
                                text: row.note
                                color: Theme.textMuted
                                font.family: Theme.fontUi
                                font.pixelSize: Theme.fsMicro
                                wrapMode: Text.WordWrap
                                Layout.leftMargin: 26
                            }
                        }
                    }
                }
            }

            ColumnLayout {
                anchors.centerIn: parent
                width: Math.min(parent.width - 60, 520)
                spacing: 6
                visible: patchModel.visibleCount === 0

                Text {
                    Layout.fillWidth: true
                    horizontalAlignment: Text.AlignHCenter
                    text: patchModel.totalCount === 0
                          ? "No patch files found"
                          : "No patches match the current filter"
                    color: Theme.textDim
                    font.family: Theme.fontUi
                    font.pixelSize: Theme.fsBody
                    font.weight: Font.DemiBold
                }

                Text {
                    Layout.fillWidth: true
                    horizontalAlignment: Text.AlignHCenter
                    text: patchModel.totalCount === 0
                          ? "Drop patch XML files into the patches folder next to run.bat, then reopen the launcher."
                          : "Clear the search box or switch the category back to All Categories."
                    color: Theme.textMuted
                    font.family: Theme.fontUi
                    font.pixelSize: Theme.fsSmall * 1.05
                    font.weight: Font.Medium
                    wrapMode: Text.WordWrap
                }
            }
        }
    }
}
