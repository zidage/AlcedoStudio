import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Effects

// Confirmation dialog for recursive folder import. openWith() opens the dialog at
// once and starts ImportExportHandler.folderScan on a worker thread; the list and
// the file count fill in while the folder tree is read, so a large tree never
// leaves the window without feedback. Import takes the scanned paths directly in
// C++ (StartFolderImport); closing the dialog any other way cancels the scan.
Popup {
    id: root
    objectName: "folderImportConfirmDialog"
    property var theme: null
    property var host: null
    property Item blurSource: null
    readonly property var scan: appModules.importExport.folderScan
    readonly property bool scanning: scan ? scan.scanning : false
    readonly property bool scanFinished: scan ? scan.scanFinished : false
    readonly property int fileCount: scan ? scan.fileCount : 0
    // Files in the checked categories: the rows of the list and the files that import takes.
    readonly property int allowedFileCount: scan ? scan.allowedFileCount : 0
    readonly property var categoryCounts: scan ? scan.categoryCounts : [0, 0, 0, 0, 0, 0]
    readonly property int allowedCategories: scan ? scan.allowedCategories : 0
    // ImportFileCategory order: Raw, Jpeg, Tiff, Png, OpenExr, Other.
    readonly property var categoryEntries: [
        { category: 0, label: qsTr("RAW") },
        { category: 1, label: qsTr("JPEG") },
        { category: 2, label: qsTr("TIFF") },
        { category: 3, label: qsTr("PNG") },
        { category: 4, label: qsTr("OpenEXR") },
        { category: 5, label: qsTr("Other") }
    ]
    readonly property color textColor: root.theme ? root.theme.colText : appTheme.textColor
    readonly property color mutedColor: root.theme ? root.theme.colTextMuted : appTheme.textMutedColor
    signal confirmed()
    signal cancelled()

    modal: true
    focus: true
    closePolicy: Popup.CloseOnEscape
    anchors.centerIn: parent
    width: parent ? Math.min(parent.width - (appTheme.spaceXl * 2), 560) : 560
    height: parent ? Math.min(parent.height - (appTheme.spaceXl * 3), 520) : 520
    padding: 0

    property bool _confirmed: false

    Overlay.modal: Item {
        anchors.fill: parent

        MultiEffect {
            anchors.fill: parent
            source: root.blurSource
            blurEnabled: true
            blur: 0.6
            blurMax: 64
            saturation: -0.2
        }

        Rectangle {
            anchors.fill: parent
            color: root.theme ? root.theme.colOverlay : appTheme.overlayColor
        }

        MouseArea { anchors.fill: parent; hoverEnabled: true }
    }

    background: Rectangle {
        radius: appTheme.panelRadius
        color: root.theme ? root.theme.colBgPanel : appTheme.cardSurfaceColor
        border.width: 0
    }

    onClosed: {
        if (!root._confirmed) {
            if (root.scan)
                root.scan.Cancel()
            root.cancelled()
        }
        root._confirmed = false
    }

    function openWith(folderUrl) {
        root._confirmed = false
        root.open()
        if (root.scan)
            root.scan.Start(folderUrl)
    }

    function scanSummaryText() {
        if (root.scan && !root.scan.folderValid)
            return qsTr("The folder could not be read.")
        if (root.scanning)
            return qsTr("Scanning folder... %1 file(s) found").arg(root.fileCount.toLocaleString(Qt.locale(), 'f', 0))
        if (root.fileCount === 0)
            return qsTr("No files found in this folder.")
        return qsTr("%1 file(s) found").arg(root.fileCount.toLocaleString(Qt.locale(), 'f', 0))
    }

    contentItem: ColumnLayout {
        id: contentCol
        anchors.fill: parent
        anchors.margins: appTheme.spaceLg
        spacing: appTheme.spaceMd

        Label {
            text: qsTr("Import From Folder")
            font.family: root.theme ? root.theme.headlineFontFamily : appTheme.headlineFontFamily
            font.pixelSize: appTheme.fontSizeSection
            font.weight: appTheme.fontWeightHeading
            color: root.textColor
        }

        Label {
            Layout.fillWidth: true
            wrapMode: Text.WrapAnywhere
            text: root.scan ? root.scan.folderPath : ""
            color: root.mutedColor
            font.pixelSize: appTheme.fontSizeBody
        }

        // Scan status: live count, the folder being read, and a progress track
        // that sweeps until the scan ends.
        ColumnLayout {
            Layout.fillWidth: true
            spacing: appTheme.spaceXs

            Label {
                objectName: "folderImportScanSummary"
                Layout.fillWidth: true
                wrapMode: Text.Wrap
                text: root.scanSummaryText()
                color: root.textColor
                font.family: appTheme.dataFontFamily
                font.pixelSize: appTheme.fontSizeBody
                font.weight: appTheme.fontWeightStrong
            }

            // Single line that changes several times a second: eliding keeps the
            // dialog height stable while the scan moves through the tree.
            Label {
                Layout.fillWidth: true
                visible: root.scanning && root.scan.currentDirectory.length > 0
                elide: Text.ElideMiddle
                text: root.scanning ? root.scan.currentDirectory : ""
                color: root.mutedColor
                font.pixelSize: appTheme.fontSizeCaption
            }

            ThemedProgressBar {
                Layout.fillWidth: true
                visible: root.scanning
                active: root.visible && root.scanning
                indeterminate: true
            }

            Label {
                Layout.fillWidth: true
                text: qsTr("File types")
                color: root.mutedColor
                font.pixelSize: appTheme.fontSizeCaption
                font.weight: appTheme.fontWeightStrong
            }

            // One checkbox per category with its count. Other is counted but never imported.
            Flow {
                objectName: "folderImportCategoryRow"
                Layout.fillWidth: true
                spacing: appTheme.spaceSm

                Repeater {
                    model: root.categoryEntries
                    ThemeCheckBox {
                        required property var modelData
                        objectName: "folderImportCategory_" + modelData.category
                        text: qsTr("%1 (%2)").arg(modelData.label)
                              .arg(Number(root.categoryCounts[modelData.category] || 0)
                                   .toLocaleString(Qt.locale(), 'f', 0))
                        alwaysPrimaryText: true
                        enabled: modelData.category !== 5
                        checked: modelData.category !== 5
                                 && (root.allowedCategories & (1 << modelData.category)) !== 0
                        onToggled: function(nextChecked) {
                            if (root.scan)
                                root.scan.SetCategoryAllowed(modelData.category, nextChecked)
                        }
                    }
                }
            }

            Label {
                Layout.fillWidth: true
                visible: root.scanFinished && root.fileCount > 0
                wrapMode: Text.Wrap
                text: qsTr("Files of other types are skipped.")
                color: root.mutedColor
                font.pixelSize: appTheme.fontSizeCaption
            }
        }

        Rectangle {
            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.minimumHeight: appTheme.spaceXl * 4
            color: root.theme ? root.theme.colBgBase : appTheme.bgBaseColor
            radius: appTheme.controlRadiusSmall
            clip: true

            ListView {
                id: fileListView
                objectName: "folderImportFileList"
                anchors.fill: parent
                anchors.margins: 1
                model: root.scan
                clip: true
                boundsBehavior: Flickable.StopAtBounds
                ScrollBar.vertical: ScrollBar {}

                delegate: Item {
                    required property string fileName
                    required property string relativeDirectory
                    width: fileListView.width
                    height: appTheme.lineHeightBody + appTheme.spaceXs * 2

                    RowLayout {
                        anchors.fill: parent
                        anchors.leftMargin: appTheme.spaceMd
                        anchors.rightMargin: appTheme.spaceMd
                        spacing: appTheme.spaceMd

                        Label {
                            Layout.fillWidth: true
                            elide: Text.ElideMiddle
                            text: fileName
                            color: root.textColor
                            font.pixelSize: appTheme.fontSizeCaption
                        }

                        Label {
                            Layout.maximumWidth: fileListView.width * 0.45
                            visible: relativeDirectory.length > 0
                            elide: Text.ElideLeft
                            text: relativeDirectory
                            color: root.mutedColor
                            font.pixelSize: appTheme.fontSizeCaption
                        }
                    }
                }
            }

            // Placeholder rows until the first batch of files arrives.
            Column {
                anchors.fill: parent
                anchors.margins: appTheme.spaceMd
                spacing: appTheme.spaceSm
                visible: root.scanning && root.fileCount === 0
                clip: true

                Repeater {
                    model: 6
                    SkeletonBlock {
                        required property int index
                        width: parent.width * (index % 2 === 0 ? 0.72 : 0.54)
                        height: appTheme.lineHeightCaption
                        radius: appTheme.controlRadiusSmall
                        animated: root.visible
                    }
                }
            }
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: appTheme.spaceSm

            Item { Layout.fillWidth: true }

            DialogActionButton {
                objectName: "folderImportCancelButton"
                text: qsTr("Cancel")
                kind: "normal"
                buttonWidth: appTheme.spaceXl * 5
                buttonHeight: appTheme.iconButtonHitSizeCompact
                buttonRadius: appTheme.controlRadiusSmall
                font.weight: appTheme.fontWeightRegular
                onClicked: root.close()
            }

            DialogActionButton {
                objectName: "folderImportConfirmButton"
                enabled: root.scanFinished && root.allowedFileCount > 0
                text: root.scanning
                      ? qsTr("Scanning...")
                      : qsTr("Import %1 File(s)").arg(root.allowedFileCount.toLocaleString(Qt.locale(), 'f', 0))
                kind: "accent"
                buttonWidth: appTheme.spaceXl * 9
                buttonHeight: appTheme.iconButtonHitSizeCompact
                buttonRadius: appTheme.controlRadiusSmall
                font.weight: appTheme.fontWeightRegular
                onClicked: {
                    root._confirmed = true
                    root.close()
                    root.confirmed()
                }
            }
        }
    }
}
