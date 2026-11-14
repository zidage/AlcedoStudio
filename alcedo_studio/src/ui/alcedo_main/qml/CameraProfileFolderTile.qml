pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Dialogs
import QtQuick.Layouts

// Camera profiles tile of Settings > LUTs and Color Management (DCP camera profile plan, section
// 2.1): the DCP folder with Browse and Rescan. The Rescan button spins while a scan runs. A
// folder error shows below the folder row, and the stored folder stays.
BentoTile {
    id: tile
    objectName: "cameraProfileFolderTile"

    property var library: null
    property color textColor: appTheme.textColor
    property color mutedTextColor: appTheme.textMutedColor
    property string dataFontFamily: appTheme.headlineFontFamily

    readonly property bool scanning: !!library && library.scanning
    readonly property bool hasFolder: !!library && library.rootPath.length > 0

    contentHeight: folderColumn.implicitHeight

    // Called with the dialog's folder URL; exposed for tests that cannot drive the native
    // folder dialog.
    function useFolder(folder) {
        if (library)
            library.setRootFolder(folder)
    }

    FolderDialog {
        id: folderDialog
        title: qsTr("Choose the DCP folder")
        onAccepted: tile.useFolder(selectedFolder)
    }

    ColumnLayout {
        id: folderColumn
        anchors.fill: parent
        spacing: appTheme.spaceMd

        ColumnLayout {
            Layout.fillWidth: true
            spacing: appTheme.spaceXs

            Label {
                Layout.fillWidth: true
                text: qsTr("Camera profiles")
                color: tile.textColor
                font.family: appTheme.headlineFontFamily
                font.pixelSize: appTheme.fontSizeSection
                font.weight: Font.DemiBold
            }
            Label {
                Layout.fillWidth: true
                text: qsTr("The folder of the DCP camera profiles that the editor lists for each camera.")
                color: tile.mutedTextColor
                font.family: appTheme.uiFontFamily
                font.pixelSize: appTheme.fontSizeBody
                wrapMode: Text.Wrap
            }
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: 12

            FolderPathField {
                Layout.fillWidth: true
                controlHeight: 44
                pathPixelSize: appTheme.fontSizeSection
                iconSize: 20
                path: tile.library ? tile.library.rootPath : ""
                textColor: tile.textColor
                mutedTextColor: tile.mutedTextColor
                pathFontFamily: tile.dataFontFamily
                browseEnabled: !!tile.library
                browseToolTip: qsTr("Browse…")
                pathLabel.objectName: "cameraProfileRootPath"
                browseButton.objectName: "cameraProfileBrowseButton"
                onBrowseRequested: folderDialog.open()
            }
            IconButton {
                objectName: "cameraProfileRescanButton"
                Layout.alignment: Qt.AlignTop
                buttonSize: 44
                buttonRadius: 10
                iconSize: 20
                iconSrc: "qrc:/panel_icons/retry.svg"
                normalColor: Qt.rgba(1, 1, 1, 0.07)
                borderColor: Qt.rgba(tile.textColor.r, tile.textColor.g, tile.textColor.b, 0.14)
                tooltipText: qsTr("Rescan")
                spinning: tile.scanning
                enabled: tile.hasFolder && !tile.scanning
                onClicked: tile.library.rescan()
            }
        }

        AlertBadge {
            objectName: "cameraProfileFolderError"
            Layout.fillWidth: true
            text: tile.library ? tile.library.errorText : ""
        }
    }
}
