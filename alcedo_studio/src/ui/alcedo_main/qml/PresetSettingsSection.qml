pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Dialogs
import QtQuick.Layouts

// Presets tile of Settings > LUTs and Color Management (edit presets plan, section 2.5): the
// preset folder with Browse and Rescan. A new folder is read in place; no file moves. The Rescan
// button spins while an operation of the preset library runs. An error of the last operation
// shows below the folder row, and the stored folder stays.
BentoTile {
    id: tile
    objectName: "presetSettingsSection"

    // PresetLibraryService (appModules.presetLibrary).
    property var library: null
    property color textColor: appTheme.textColor
    property color mutedTextColor: appTheme.textMutedColor
    property string dataFontFamily: appTheme.headlineFontFamily

    readonly property bool busy: !!library && library.busy

    contentHeight: folderColumn.implicitHeight

    // Called with the dialog's folder URL; exposed for tests that cannot drive the native
    // folder dialog.
    function useFolder(folder) {
        if (library)
            library.useRoot(folder)
    }

    FolderDialog {
        id: folderDialog
        title: qsTr("Choose the preset folder")
        onAccepted: tile.useFolder(selectedFolder)
    }

    ColumnLayout {
        id: folderColumn
        anchors.fill: parent
        spacing: appTheme.spaceMd

        Label {
            Layout.fillWidth: true
            text: qsTr("Presets")
            color: tile.textColor
            font.family: appTheme.headlineFontFamily
            font.pixelSize: appTheme.fontSizeSection
            font.weight: Font.DemiBold
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
                pathLabel.objectName: "presetRootPath"
                browseButton.objectName: "presetBrowseButton"
                onBrowseRequested: folderDialog.open()
            }
            IconButton {
                objectName: "presetRescanButton"
                Layout.alignment: Qt.AlignTop
                buttonSize: 44
                buttonRadius: 10
                iconSize: 20
                iconSrc: "qrc:/panel_icons/retry.svg"
                normalColor: Qt.rgba(1, 1, 1, 0.07)
                borderColor: Qt.rgba(tile.textColor.r, tile.textColor.g, tile.textColor.b, 0.14)
                tooltipText: qsTr("Rescan")
                spinning: tile.busy
                enabled: !!tile.library && !tile.busy
                onClicked: tile.library.refresh()
            }
        }

        AlertBadge {
            objectName: "presetFolderError"
            Layout.fillWidth: true
            text: tile.library ? tile.library.lastError : ""
        }
    }
}
