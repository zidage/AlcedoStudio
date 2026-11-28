//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

import QtQuick
import QtQuick.Controls
import QtQuick.Controls.impl
import QtQuick.Effects
import QtQuick.Layouts

// Import Folder dialog (#1078): the user chooses between Import into Collection and Mount Folder
// for one folder, or imports a Library drop. It opens after the system folder dialog and starts
// the folder scan (ImportExportHandler.folderScan) at once; the count line fills in while the
// tree is read.
//
// - Header: folder name, full path, close button.
// - Count line: photo count, one toggle chip per file type with its count, the subfolder count,
//   and the volume of the folder.
// - The two choice cards (ImportFolderChoiceCards, shared with Settings, #1077).
// - Destination: Import has the collection tree with search (CollectionPickerTree); Mount shows
//   the volume, which the user cannot change.
// - Footer: Remember my choice, Cancel, and the main button.
//
// The dialog reports the choice; the owner (ImportFolderFlow) runs it. Closing the dialog any
// other way cancels the scan.
Popup {
    id: dialog
    objectName: "importFolderDialog"

    property var theme: null
    property Item blurSource: null
    /// FolderImportScanModel (ImportExportHandler.folderScan).
    property var scan: null
    /// FolderController: the rows of the destination tree and New collection.
    property var folderController: null
    /// "import" or "mount".
    property string selectedAction: "import"
    property bool remember: false
    /// False for a drop of several items or of files: there is no one folder to mount.
    property bool mountAvailable: true
    /// True for a Library drop (openForDrop).
    property bool dropMode: false
    /// The folder of openForFolder, or the one dropped folder; the folder that Mount takes.
    property string sourceUrl: ""

    readonly property bool scanning: scan ? scan.scanning === true : false
    readonly property bool scanFinished: scan ? scan.scanFinished === true : false
    readonly property bool folderValid: scan ? scan.folderValid !== false : true
    readonly property var categoryCounts: scan && scan.categoryCounts ? scan.categoryCounts
                                                                     : [0, 0, 0, 0, 0, 0]
    readonly property int allowedCategories: scan ? Number(scan.allowedCategories) : 0
    // Mount reads every supported file of the folder, whatever the type chips say.
    readonly property int mountFileCount: {
        let total = 0
        for (let category = 0; category < 5; ++category) {
            total += Number(categoryCounts[category] || 0)
        }
        return total
    }
    readonly property bool mountChosen: selectedAction === "mount"
    readonly property int photoCount: mountChosen ? mountFileCount
                                                  : (scan ? Number(scan.allowedFileCount) : 0)
    readonly property string folderPath: scan ? String(scan.folderPath || "") : ""
    readonly property string folderName: {
        const parts = folderPath.split(/[\\/]/).filter(function(part) { return part.length > 0 })
        return parts.length > 0 ? parts[parts.length - 1] : folderPath
    }
    readonly property string volumeName: scan ? String(scan.volumeName || "") : ""
    readonly property string volumeMountPoint: scan ? String(scan.volumeMountPoint || "") : ""
    readonly property alias destination: picker

    // ImportFileCategory order: Raw, Jpeg, Tiff, Png, OpenExr. Other is never imported.
    readonly property var categoryEntries: [
        { category: 0, label: qsTr("RAW") },
        { category: 1, label: qsTr("JPEG") },
        { category: 2, label: qsTr("TIFF") },
        { category: 3, label: qsTr("PNG") },
        { category: 4, label: qsTr("OpenEXR") }
    ]

    signal importConfirmed(int folderId, bool remember)
    signal mountConfirmed(string folderUrl, bool remember)

    modal: true
    focus: true
    closePolicy: Popup.CloseOnEscape
    anchors.centerIn: parent
    width: parent ? Math.min(parent.width - appTheme.spaceXl * 2, 600) : 600
    height: parent ? Math.min(parent.height - appTheme.spaceXl * 2, implicitHeight) : implicitHeight
    padding: 0

    property bool _confirmed: false

    function grouped(value) {
        return Number(value).toLocaleString(Qt.locale(), "f", 0)
    }

    /// Opens the dialog for @p folderUrl with the card @p action selected and the collection
    /// @p destinationFolderId (ui id) as the Import destination, and starts the scan.
    function openForFolder(folderUrl, action, destinationFolderId) {
        _confirmed = false
        dropMode = false
        mountAvailable = true
        sourceUrl = String(folderUrl)
        prepare(action, destinationFolderId)
        open()
        if (scan) {
            scan.Start(sourceUrl)
        }
    }

    /// Opens the dialog for a Library drop into the collection @p destinationFolderId. Mount is
    /// available when the drop is one folder.
    function openForDrop(urls, destinationFolderId, singleFolder) {
        _confirmed = false
        dropMode = true
        mountAvailable = singleFolder === true
        sourceUrl = singleFolder === true && urls.length > 0 ? String(urls[0]) : ""
        prepare("import", destinationFolderId)
        open()
        if (scan) {
            scan.StartSources(urls)
        }
    }

    function prepare(action, destinationFolderId) {
        selectedAction = action === "mount" && mountAvailable ? "mount" : "import"
        remember = false
        picker.searchText = ""
        picker.draftParentElementId = -1
        // The destination by element id: the rows get new ui ids on a tree reload.
        const rows = folderController && folderController.folders ? folderController.folders : []
        let elementId = 0
        for (let i = 0; i < rows.length; ++i) {
            if (Number(rows[i].folderId) === Number(destinationFolderId)
                    && Number(rows[i].mountId || 0) === 0) {
                elementId = Number(rows[i].elementId)
                break
            }
        }
        picker.selectedElementId = elementId
        picker.revealElement(elementId)
    }

    function confirm() {
        if (!confirmEnabled()) {
            return
        }
        _confirmed = true
        const action = selectedAction
        const folderId = picker.selectedFolderId
        const rememberChoice = remember
        close()
        if (action === "mount") {
            mountConfirmed(sourceUrl, rememberChoice)
        } else {
            importConfirmed(folderId, rememberChoice)
        }
    }

    function confirmEnabled() {
        if (!scanFinished || photoCount <= 0) {
            return false
        }
        return mountChosen ? mountAvailable && sourceUrl.length > 0 : picker.selectedFolderId >= 0
    }

    function statusText() {
        if (!folderValid) {
            return dropMode ? qsTr("The dropped items could not be read.")
                            : qsTr("The folder could not be read.")
        }
        if (scanFinished && photoCount === 0) {
            return dropMode ? qsTr("No files found in the dropped items.")
                            : qsTr("No files found in this folder.")
        }
        return ""
    }

    onClosed: {
        if (!_confirmed && scan) {
            scan.Cancel()
        }
        _confirmed = false
    }

    Overlay.modal: Item {
        anchors.fill: parent

        MultiEffect {
            anchors.fill: parent
            source: dialog.blurSource
            visible: dialog.blurSource !== null
            blurEnabled: true
            blur: 0.6
            blurMax: 64
            saturation: -0.2
        }

        Rectangle {
            anchors.fill: parent
            color: dialog.theme && dialog.theme.colOverlay !== undefined ? dialog.theme.colOverlay
                                                                         : appTheme.overlayColor
        }

        MouseArea { anchors.fill: parent; hoverEnabled: true }
    }

    background: Rectangle {
        radius: appTheme.panelRadius
        color: dialog.theme && dialog.theme.colBgPanel !== undefined ? dialog.theme.colBgPanel
                                                                     : appTheme.cardSurfaceColor
        border.width: 1
        border.color: appTheme.glassStrokeColor
    }

    contentItem: ColumnLayout {
        spacing: 0

        ColumnLayout {
            Layout.fillWidth: true
            Layout.margins: appTheme.spaceLg
            spacing: appTheme.spaceMd

            // Header: folder icon, name and path, close button.
            RowLayout {
                Layout.fillWidth: true
                spacing: appTheme.spaceSm

                IconImage {
                    Layout.preferredWidth: 22
                    Layout.preferredHeight: 22
                    Layout.alignment: Qt.AlignTop
                    Layout.topMargin: 2
                    sourceSize: Qt.size(22, 22)
                    source: "qrc:/panel_icons/folder.svg"
                    color: appTheme.textColor
                }

                ColumnLayout {
                    Layout.fillWidth: true
                    Layout.minimumWidth: 0
                    spacing: 2

                    Label {
                        objectName: "importFolderDialogTitle"
                        Layout.fillWidth: true
                        elide: Text.ElideRight
                        text: dialog.folderName
                        color: appTheme.textColor
                        font.family: appTheme.uiFontFamily
                        font.pixelSize: appTheme.fontSizeSection
                        font.weight: appTheme.fontWeightHeading
                    }

                    Label {
                        Layout.fillWidth: true
                        elide: Text.ElideMiddle
                        text: dialog.folderPath
                        color: appTheme.textMutedColor
                        font.family: appTheme.dataFontFamily
                        font.pixelSize: appTheme.fontSizeCaption
                    }
                }

                IconButton {
                    objectName: "importFolderDialogClose"
                    Layout.alignment: Qt.AlignTop
                    iconSrc: "qrc:/panel_icons/close.svg"
                    bordered: false
                    tooltipText: qsTr("Close")
                    onClicked: dialog.close()
                }
            }

            // Count line: photo count, type chips, subfolders, volume.
            Flow {
                objectName: "importFolderDialogCounts"
                Layout.fillWidth: true
                spacing: appTheme.spaceSm

                Label {
                    objectName: "importFolderDialogPhotoCount"
                    text: dialog.grouped(dialog.photoCount)
                    color: appTheme.textColor
                    font.family: appTheme.headlineFontFamily
                    font.features: { "tnum": 1 }
                    font.pixelSize: 26
                    font.weight: appTheme.fontWeightHeading
                }

                Label {
                    height: 26
                    verticalAlignment: Text.AlignBottom
                    bottomPadding: 3
                    rightPadding: appTheme.spaceSm
                    text: dialog.scanning ? qsTr("Scanning...") : qsTr("photos")
                    color: appTheme.textMutedColor
                    font.family: appTheme.uiFontFamily
                    font.pixelSize: appTheme.fontSizeBody
                }

                Repeater {
                    model: dialog.categoryEntries

                    delegate: Rectangle {
                        id: typeChip
                        required property var modelData
                        readonly property int count: Number(dialog.categoryCounts[modelData.category] || 0)
                        readonly property bool allowed: (dialog.allowedCategories
                                                         & (1 << modelData.category)) !== 0
                        // The chips choose the types of Import; Mount reads every type.
                        readonly property bool active: !dialog.mountChosen
                        objectName: "importFolderDialogType_" + modelData.category
                        visible: count > 0
                        height: 26
                        width: chipRow.implicitWidth + appTheme.spaceSm * 2
                        y: 0
                        radius: appTheme.controlRadiusSmall
                        color: allowed && active ? appTheme.controlFillColor : "transparent"
                        border.width: 1
                        border.color: allowed && active ? appTheme.controlBorderColor
                                                        : appTheme.dividerColor
                        Accessible.role: Accessible.CheckBox
                        Accessible.name: modelData.label
                        Accessible.checkable: true
                        Accessible.checked: allowed

                        signal toggled()
                        onToggled: {
                            if (active && dialog.scan) {
                                dialog.scan.SetCategoryAllowed(modelData.category, !allowed)
                            }
                        }

                        Row {
                            id: chipRow
                            anchors.centerIn: parent
                            spacing: appTheme.spaceXs

                            Label {
                                text: typeChip.modelData.label
                                color: typeChip.allowed && typeChip.active ? appTheme.textColor
                                                                           : appTheme.textMutedColor
                                font.family: appTheme.headlineFontFamily
                                font.pixelSize: appTheme.fontSizeCaption
                                font.weight: appTheme.fontWeightStrong
                            }

                            Label {
                                text: dialog.grouped(typeChip.count)
                                color: appTheme.textMutedColor
                                font.family: appTheme.headlineFontFamily
                                font.features: { "tnum": 1 }
                                font.pixelSize: appTheme.fontSizeCaption
                                font.weight: appTheme.fontWeightStrong
                            }
                        }

                        MouseArea {
                            anchors.fill: parent
                            enabled: typeChip.active
                            cursorShape: Qt.PointingHandCursor
                            onClicked: typeChip.toggled()
                        }
                    }
                }

                InfoChip {
                    objectName: "importFolderDialogSubfolders"
                    visible: dialog.scan && Number(dialog.scan.subfolderCount) > 0
                    text: qsTr("%1 subfolders").arg(dialog.grouped(dialog.scan
                                                                    ? dialog.scan.subfolderCount : 0))
                }

                InfoChip {
                    objectName: "importFolderDialogVolume"
                    visible: text.length > 0
                    text: [dialog.volumeName, dialog.volumeMountPoint].filter(function(part) {
                        return part.length > 0
                    }).join(" ")
                }
            }

            ThemedProgressBar {
                Layout.fillWidth: true
                visible: dialog.scanning
                active: dialog.visible && dialog.scanning
                indeterminate: true
            }

            Label {
                objectName: "importFolderDialogStatus"
                Layout.fillWidth: true
                visible: text.length > 0
                wrapMode: Text.Wrap
                text: dialog.statusText()
                color: appTheme.textMutedColor
                font.family: appTheme.uiFontFamily
                font.pixelSize: appTheme.fontSizeBody
            }

            ImportFolderChoiceCards {
                objectName: "importFolderDialogCards"
                Layout.fillWidth: true
                cardObjectNamePrefix: "importFolderDialogCard"
                selectedAction: dialog.selectedAction
                collectionName: picker.selectedName
                mountEnabled: dialog.mountAvailable
                onActionSelected: function(action) {
                    if (action !== "mount" || dialog.mountAvailable) {
                        dialog.selectedAction = action
                    }
                }
            }

            Label {
                Layout.fillWidth: true
                Layout.topMargin: appTheme.spaceXs
                text: dialog.mountChosen ? qsTr("Mount to") : qsTr("Import to")
                color: appTheme.textMutedColor
                font.family: appTheme.uiFontFamily
                font.pixelSize: appTheme.fontSizeBody
                font.weight: appTheme.fontWeightStrong
            }

            CollectionPickerTree {
                id: picker
                objectName: "importFolderDialogDestination"
                Layout.fillWidth: true
                Layout.preferredHeight: implicitHeight
                visible: !dialog.mountChosen
                folderRows: dialog.folderController && dialog.folderController.folders
                            ? dialog.folderController.folders : []
                folderController: dialog.folderController
            }

            // Mount: the volume that holds the folder; the user cannot change it.
            Rectangle {
                objectName: "importFolderDialogMountDestination"
                Layout.fillWidth: true
                Layout.preferredHeight: 38
                visible: dialog.mountChosen
                radius: appTheme.controlRadius
                color: appTheme.bgBaseColor
                border.width: 1
                border.color: appTheme.controlBorderColor

                RowLayout {
                    anchors.fill: parent
                    anchors.leftMargin: appTheme.spaceMd
                    anchors.rightMargin: appTheme.spaceMd
                    spacing: appTheme.spaceSm

                    IconImage {
                        Layout.preferredWidth: 16
                        Layout.preferredHeight: 16
                        Layout.alignment: Qt.AlignVCenter
                        sourceSize: Qt.size(16, 16)
                        source: "qrc:/panel_icons/drive.svg"
                        color: appTheme.textMutedColor
                    }

                    Label {
                        Layout.alignment: Qt.AlignVCenter
                        text: qsTr("Disk")
                        color: appTheme.textColor
                        font.family: appTheme.uiFontFamily
                        font.pixelSize: appTheme.fontSizeTitle
                    }

                    IconImage {
                        Layout.preferredWidth: 12
                        Layout.preferredHeight: 12
                        Layout.alignment: Qt.AlignVCenter
                        sourceSize: Qt.size(12, 12)
                        source: "qrc:/panel_icons/chevron-right.svg"
                        color: appTheme.textMutedColor
                    }

                    Label {
                        Layout.alignment: Qt.AlignVCenter
                        Layout.maximumWidth: 260
                        visible: text.length > 0
                        elide: Text.ElideRight
                        text: dialog.volumeName.length > 0 ? dialog.volumeName
                                                           : dialog.volumeMountPoint
                        color: appTheme.textColor
                        font.family: appTheme.uiFontFamily
                        font.pixelSize: appTheme.fontSizeTitle
                    }

                    Label {
                        Layout.alignment: Qt.AlignVCenter
                        visible: dialog.volumeName.length > 0 && dialog.volumeMountPoint.length > 0
                        text: dialog.volumeMountPoint
                        color: appTheme.textMutedColor
                        font.family: appTheme.headlineFontFamily
                        font.pixelSize: appTheme.fontSizeCaption
                        font.weight: appTheme.fontWeightStrong
                    }

                    Item { Layout.fillWidth: true }
                }
            }
        }

        // Footer: Remember my choice on the left, Cancel and the main button on the right.
        RowLayout {
            Layout.fillWidth: true
            Layout.leftMargin: appTheme.spaceLg
            Layout.rightMargin: appTheme.spaceLg
            Layout.bottomMargin: appTheme.spaceLg
            spacing: appTheme.spaceSm

            ThemeCheckBox {
                objectName: "importFolderDialogRemember"
                Layout.alignment: Qt.AlignVCenter
                text: qsTr("Remember my choice")
                alwaysPrimaryText: true
                checked: dialog.remember
                onToggled: function(checked) { dialog.remember = checked }
            }

            Item { Layout.fillWidth: true }

            DialogActionButton {
                objectName: "importFolderDialogCancel"
                text: qsTr("Cancel")
                kind: "normal"
                buttonWidth: appTheme.spaceXl * 4
                buttonHeight: appTheme.iconButtonHitSizeCompact
                buttonRadius: appTheme.controlRadiusSmall
                font.pixelSize: appTheme.fontSizeTitle
                font.weight: appTheme.fontWeightRegular
                onClicked: dialog.close()
            }

            DialogActionButton {
                objectName: "importFolderDialogConfirm"
                kind: "light"
                enabled: dialog.confirmEnabled()
                text: dialog.mountChosen ? qsTr("Mount %1 photos").arg(dialog.grouped(dialog.photoCount))
                                         : qsTr("Import %1 photos").arg(dialog.grouped(dialog.photoCount))
                buttonWidth: Math.max(appTheme.spaceXl * 6, implicitContentWidth + appTheme.spaceLg * 2)
                buttonHeight: appTheme.iconButtonHitSizeCompact
                buttonRadius: appTheme.controlRadiusSmall
                font.pixelSize: appTheme.fontSizeTitle
                onClicked: dialog.confirm()
            }
        }
    }

    // A fact of the count line: muted text in a hairline box.
    component InfoChip: Rectangle {
        property string text: ""
        height: 26
        width: infoLabel.implicitWidth + appTheme.spaceSm * 2
        radius: appTheme.controlRadiusSmall
        color: "transparent"
        border.width: 1
        border.color: appTheme.dividerColor

        Label {
            id: infoLabel
            anchors.centerIn: parent
            text: parent.text
            color: appTheme.textMutedColor
            font.family: appTheme.headlineFontFamily
            font.features: { "tnum": 1 }
            font.pixelSize: appTheme.fontSizeCaption
            font.weight: appTheme.fontWeightStrong
        }
    }
}
