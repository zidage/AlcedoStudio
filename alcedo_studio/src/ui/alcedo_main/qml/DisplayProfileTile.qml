pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import Alcedo.Main 1.0

// Display profile tile of Settings > LUTs and Color Management (Windows display ICC plan,
// section 3.2): the profile of the monitor that shows the settings window, the conversion
// state of the photo surfaces, and the file of the monitor profile. Read-only; Windows only.
// An unusable profile shows its reason in an alert badge.
BentoTile {
    id: tile
    objectName: "displayProfileTile"

    property color textColor: appTheme.textColor
    property color mutedTextColor: appTheme.textMutedColor
    property string dataFontFamily: appTheme.headlineFontFamily

    readonly property bool supported: Qt.platform.os === "windows"

    contentHeight: profileColumn.implicitHeight

    function stateText(state) {
        switch (state) {
        case DisplayProfileReadout.Applied:
            return qsTr("Applied")
        case DisplayProfileReadout.ManagedByWindows:
            return qsTr("Managed by Windows")
        case DisplayProfileReadout.NoProfile:
            return qsTr("No profile, sRGB")
        case DisplayProfileReadout.Unusable:
            return qsTr("Not applied")
        default:
            return qsTr("No monitor")
        }
    }

    DisplayProfileReadout {
        id: readout
        window: tile.supported ? tile.Window.window : null
    }

    ColumnLayout {
        id: profileColumn
        anchors.fill: parent
        spacing: appTheme.spaceMd

        Label {
            Layout.fillWidth: true
            text: qsTr("Display profile")
            color: tile.textColor
            font.family: appTheme.headlineFontFamily
            font.pixelSize: appTheme.fontSizeSection
            font.weight: Font.DemiBold
        }

        ColumnLayout {
            Layout.fillWidth: true
            spacing: appTheme.spaceXs

            RowLayout {
                Layout.fillWidth: true
                spacing: appTheme.spaceMd

                Label {
                    objectName: "displayProfileDescription"
                    Layout.fillWidth: true
                    text: readout.description
                    color: tile.textColor
                    font.family: tile.dataFontFamily
                    font.pixelSize: appTheme.fontSizeBody
                    elide: Text.ElideRight
                }
                Label {
                    objectName: "displayProfileState"
                    text: tile.stateText(readout.state)
                    color: tile.mutedTextColor
                    font.family: appTheme.uiFontFamily
                    font.pixelSize: appTheme.fontSizeBody
                }
            }
            Label {
                objectName: "displayProfilePath"
                Layout.fillWidth: true
                visible: text.length > 0
                text: readout.profilePath
                color: tile.mutedTextColor
                font.family: tile.dataFontFamily
                font.pixelSize: appTheme.fontSizeCaption
                wrapMode: Text.WrapAnywhere
            }
        }

        AlertBadge {
            objectName: "displayProfileError"
            Layout.fillWidth: true
            text: readout.errorText
        }
    }
}
