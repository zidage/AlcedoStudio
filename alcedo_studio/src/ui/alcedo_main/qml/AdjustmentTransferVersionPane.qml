pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts

// Adjustment Transfer Copy pane 1: read-only source Version catalog rows.
// Presentational only; the dialog wires versionActivated to the C++ dialog
// model's SelectVersion command. Selection state lives in the row's
// `selected` role; a Version row never carries a checkbox.
Item {
    id: pane
    objectName: "adjustmentTransferVersionPane"

    // AdjustmentTransferVersionListModel from the dialog model.
    property var model: null
    property string title: qsTr("Source Versions")

    signal versionActivated(string versionId)

    function versionTimeText(seconds) {
        if (!seconds || Number(seconds) <= 0) {
            return qsTr("Imported")
        }
        return Qt.formatDateTime(new Date(Number(seconds) * 1000), Locale.ShortFormat)
    }

    implicitWidth: appTheme.editorSidePanelWidthMin

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        Label {
            Layout.fillWidth: true
            Layout.leftMargin: appTheme.spaceMd
            Layout.rightMargin: appTheme.spaceMd
            Layout.topMargin: appTheme.spaceSm
            Layout.bottomMargin: appTheme.spaceSm
            text: pane.title
            color: appTheme.textMutedColor
            font.family: appTheme.uiFontFamily
            font.pixelSize: appTheme.fontSizeCaption
            font.weight: appTheme.fontWeightStrong
        }

        Rectangle {
            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.leftMargin: appTheme.spaceSm
            Layout.rightMargin: appTheme.spaceSm
            Layout.bottomMargin: appTheme.spaceSm
            radius: appTheme.controlRadiusSmall
            color: appTheme.bgBaseColor
            border.width: 1
            border.color: appTheme.cardBorderColor
            clip: true

            ListView {
                id: versionList
                objectName: "adjustmentTransferVersionList"
                anchors.fill: parent
                anchors.margins: appTheme.spaceXs
                model: pane.model
                spacing: appTheme.spaceXs
                boundsBehavior: Flickable.StopAtBounds
                reuseItems: true
                keyNavigationEnabled: true
                activeFocusOnTab: true
                Accessible.role: Accessible.List
                Accessible.name: pane.title

                delegate: Item {
                    id: versionDelegate
                    objectName: "transferVersionDelegate"
                    required property int index
                    required property string versionId
                    required property string displayName
                    required property var updatedAt
                    required property bool active
                    required property bool selected
                    width: ListView.view ? ListView.view.width : 0
                    height: Math.max(appTheme.iconButtonHitSize + appTheme.spaceSm,
                                     versionTextColumn.implicitHeight + appTheme.spaceMd)
                    Accessible.role: Accessible.ListItem
                    Accessible.name: versionDelegate.displayName
                    Accessible.description: versionDelegate.active
                                            ? qsTr("Active Version") : ""
                    Accessible.onPressAction: versionDelegate.pick()

                    // keyNavigationEnabled lands active focus on the current
                    // delegate, so activation keys are handled here.
                    Keys.onPressed: function(event) {
                        if (event.key === Qt.Key_Space || event.key === Qt.Key_Return
                                || event.key === Qt.Key_Enter) {
                            versionDelegate.pick()
                            event.accepted = true
                        }
                    }

                    function pick() {
                        ListView.view.currentIndex = versionDelegate.index
                        ListView.view.forceActiveFocus()
                        pane.versionActivated(versionDelegate.versionId)
                    }

                    Rectangle {
                        anchors.fill: parent
                        radius: appTheme.badgeRadius
                        color: versionDelegate.selected
                               ? appTheme.editorListSelectedFillColor
                               : (versionMouse.containsMouse
                                  ? appTheme.buttonHoveredFillColor
                                  : "transparent")
                        border.width: (versionDelegate.ListView.isCurrentItem
                                       && versionDelegate.ListView.view
                                       && versionDelegate.ListView.view.activeFocus) ? 1 : 0
                        border.color: appTheme.textColor
                    }

                    RowLayout {
                        anchors.fill: parent
                        anchors.leftMargin: appTheme.spaceSm
                        anchors.rightMargin: appTheme.spaceSm
                        spacing: appTheme.spaceSm

                        Rectangle {
                            Layout.preferredWidth: appTheme.iconButtonHitSizeCompact
                            Layout.preferredHeight: appTheme.iconButtonHitSizeCompact
                            radius: appTheme.controlRadiusSmall
                            color: appTheme.cardSurfaceColor

                            Label {
                                anchors.centerIn: parent
                                text: versionDelegate.displayName.slice(0, 1).toUpperCase()
                                color: versionDelegate.selected
                                       ? appTheme.editorListSelectedInkColor
                                       : appTheme.textMutedColor
                                font.family: appTheme.uiFontFamily
                                font.pixelSize: appTheme.fontSizeBody
                                font.weight: appTheme.fontWeightHeading
                            }
                        }

                        ColumnLayout {
                            id: versionTextColumn
                            Layout.fillWidth: true
                            spacing: 0

                            Label {
                                Layout.fillWidth: true
                                text: versionDelegate.displayName
                                color: versionDelegate.selected
                                       ? appTheme.editorListSelectedInkColor
                                       : appTheme.textColor
                                font.family: appTheme.uiFontFamily
                                font.pixelSize: appTheme.fontSizeBody
                                font.weight: appTheme.fontWeightStrong
                                wrapMode: Text.Wrap
                            }

                            RowLayout {
                                Layout.fillWidth: true
                                spacing: appTheme.spaceSm

                                Label {
                                    Layout.fillWidth: true
                                    text: pane.versionTimeText(versionDelegate.updatedAt)
                                    color: versionDelegate.selected
                                           ? appTheme.editorListSelectedInkColor
                                           : appTheme.textMutedColor
                                    font.family: appTheme.dataFontFamily
                                    font.pixelSize: appTheme.fontSizeCaption
                                    wrapMode: Text.Wrap
                                }

                                Label {
                                    visible: versionDelegate.active
                                    text: qsTr("Active")
                                    color: versionDelegate.selected
                                           ? appTheme.editorListSelectedInkColor
                                           : appTheme.textMutedColor
                                    font.family: appTheme.uiFontFamily
                                    font.pixelSize: appTheme.fontSizeCaption
                                    font.weight: appTheme.fontWeightStrong
                                }
                            }
                        }
                    }

                    MouseArea {
                        id: versionMouse
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: versionDelegate.pick()
                    }
                }
            }
        }
    }
}
