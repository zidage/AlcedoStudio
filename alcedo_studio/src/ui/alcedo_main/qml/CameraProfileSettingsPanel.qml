pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Dialogs
import QtQuick.Layouts

// Settings > Camera Profiles (DCP camera profile plan, section 2.1): the DCP folder with Browse
// and Rescan, and the Profiles, Cameras, and Rejected counts of the last completed scan. The
// counts stay while a scan runs; the Rescan button spins. A folder error shows below the folder
// row, and the stored folder stays. The Rejected tile shows only when files were rejected.
ColumnLayout {
    id: page
    objectName: "cameraProfileSettingsPanel"

    property var library: null
    property string languageCode: "en"
    property color textColor: appTheme.textColor
    property color mutedTextColor: appTheme.textMutedColor
    property string dataFontFamily: appTheme.headlineFontFamily

    readonly property bool scanning: !!library && library.scanning
    readonly property bool hasFolder: !!library && library.rootPath.length > 0

    spacing: appTheme.spaceMd

    // Called with the dialog's folder URL; exposed for tests that cannot drive the native
    // folder dialog.
    function useFolder(folder) {
        if (library)
            library.setRootFolder(folder)
    }

    FolderDialog {
        id: folderDialog
        title: qsTr("Choose the DCP folder")
        onAccepted: page.useFolder(selectedFolder)
    }

    RowLayout {
        Layout.fillWidth: true
        Layout.topMargin: 26
        Layout.leftMargin: 34
        Layout.rightMargin: 34
        spacing: appTheme.spaceMd

        BentoTile {
            objectName: "cameraProfileFolderTile"
            Layout.fillWidth: true
            Layout.fillHeight: true
            contentHeight: folderColumn.implicitHeight

            ColumnLayout {
                id: folderColumn
                anchors.fill: parent
                spacing: appTheme.spaceMd

                Label {
                    Layout.fillWidth: true
                    text: qsTr("DCP Folder")
                    color: page.textColor
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
                        path: page.library ? page.library.rootPath : ""
                        textColor: page.textColor
                        mutedTextColor: page.mutedTextColor
                        pathFontFamily: page.dataFontFamily
                        browseEnabled: !!page.library
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
                        borderColor: Qt.rgba(page.textColor.r, page.textColor.g, page.textColor.b, 0.14)
                        tooltipText: qsTr("Rescan")
                        spinning: page.scanning
                        enabled: page.hasFolder && !page.scanning
                        onClicked: page.library.rescan()
                    }
                }

                AlertBadge {
                    objectName: "cameraProfileFolderError"
                    Layout.fillWidth: true
                    text: page.library ? page.library.errorText : ""
                }
            }
        }

        CountTile {
            objectName: "cameraProfileProfilesTile"
            Layout.fillHeight: true
            label: qsTr("Profiles")
            count: page.library ? page.library.profileCount : 0
            compactLargeCounts: true
            languageCode: page.languageCode
            countLabel.objectName: "cameraProfileProfileCount"
        }

        CountTile {
            objectName: "cameraProfileCamerasTile"
            Layout.fillHeight: true
            label: qsTr("Cameras")
            count: page.library ? page.library.cameraCount : 0
            compactLargeCounts: true
            languageCode: page.languageCode
            countLabel.objectName: "cameraProfileCameraCount"
        }

        CountTile {
            objectName: "cameraProfileRejectedTile"
            Layout.fillHeight: true
            visible: count > 0
            label: qsTr("Rejected")
            count: page.library ? page.library.rejectedCount : 0
            compactLargeCounts: true
            languageCode: page.languageCode
            countLabel.objectName: "cameraProfileRejectedCount"
        }
    }

    Item {
        Layout.fillHeight: true
    }
}
