pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Dialogs
import QtQuick.Layouts

// Settings > LUTs: the LUT library folder (open, refresh, use another folder,
// move the library) and the two official LUT packages (download, update,
// repair, cancel, retry). The dialog starts the signed package check when
// Settings opens; this page never starts one by itself except "Check again".
ColumnLayout {
    id: page
    objectName: "lutSettingsPanel"

    property var library: null
    property var packageService: null
    property color textColor: appTheme.textColor
    property color mutedTextColor: appTheme.textMutedColor
    property color dividerColor: appTheme.dividerColor
    property color dangerColor: appTheme.dangerColor
    property string dataFontFamily: appTheme.dataFontFamily

    readonly property bool libraryBusy: !!library && library.busy
    readonly property bool packagesEnabled: !!packageService && packageService.enabled
    readonly property var packageRows: packageService ? packageService.packages : []

    // A folder chosen in the dialog, waiting for confirmation.
    property string pendingFolder: ""
    property bool pendingMigrate: false
    property string pendingPath: ""
    property string pendingError: ""
    // A request the library refused to start (another operation runs).
    property string actionError: ""

    spacing: appTheme.spaceXl

    function chooseFolder(migrate) {
        actionError = ""
        rootFolderDialog.migrate = migrate
        rootFolderDialog.open()
    }

    // Called with the dialog's folder URL; exposed for tests that cannot drive
    // the native folder dialog.
    function reviewFolder(folder, migrate) {
        if (!library)
            return
        const check = library.checkRootChoice(folder, migrate)
        pendingFolder = folder
        pendingMigrate = migrate
        pendingPath = check.path
        pendingError = check.error
    }

    function clearPendingFolder() {
        pendingFolder = ""
        pendingPath = ""
        pendingError = ""
    }

    function confirmPendingFolder() {
        if (!library || pendingFolder.length === 0 || pendingError.length > 0)
            return
        const started = pendingMigrate ? library.migrateRoot(pendingFolder)
                                       : library.useRoot(pendingFolder)
        actionError = started ? "" : qsTr("Another LUT library operation is running. Try again when it finishes.")
        clearPendingFolder()
    }

    function operationText(operation) {
        switch (operation) {
        case "load": return qsTr("Loading the LUT library…")
        case "refresh": return qsTr("Refreshing the LUT inventory…")
        case "import": return qsTr("Importing LUTs…")
        case "useRoot": return qsTr("Indexing the LUT folder…")
        case "migrateRoot": return qsTr("Moving the LUT library…")
        case "sourceCleanup": return qsTr("Removing copied files from the previous folder…")
        case "installPackage": return qsTr("Installing a LUT package…")
        case "retirePackageContent": return qsTr("Removing replaced package files…")
        }
        return ""
    }

    function packageStatusText(row) {
        switch (row.status) {
        case "notInstalled": return qsTr("Not installed")
        case "current": return qsTr("Installed and current")
        case "updateAvailable": return qsTr("Update available")
        case "repairRequired": return qsTr("Repair required: installed files differ from the package")
        case "checking": return qsTr("Checking…")
        case "downloading": return qsTr("Downloading… %1%").arg(Math.round(row.progress * 100))
        case "verifying": return qsTr("Verifying…")
        case "installing": return qsTr("Installing…")
        case "error": return qsTr("Not completed")
        }
        return ""
    }

    function packageActionText(action) {
        switch (action) {
        case "install": return qsTr("Download")
        case "update": return qsTr("Update")
        case "repair": return qsTr("Repair")
        case "retry": return qsTr("Retry")
        }
        return ""
    }

    function formatBytes(bytes) {
        const value = Number(bytes)
        if (!(value > 0))
            return ""
        if (value >= 1024 * 1024 * 1024)
            return qsTr("%1 GB").arg((value / (1024 * 1024 * 1024)).toFixed(1))
        if (value >= 1024 * 1024)
            return qsTr("%1 MB").arg((value / (1024 * 1024)).toFixed(1))
        return qsTr("%1 KB").arg(Math.max(1, Math.round(value / 1024)))
    }

    function packageDetailText(row) {
        const parts = [qsTr("%n LUT(s)", "", Number(row.fileCount))]
        const size = formatBytes(row.archiveBytes)
        if (size.length > 0)
            parts.push(qsTr("%1 download").arg(size))
        if (row.installedRevision.length > 0 && row.installedRevision !== row.revision)
            parts.push(qsTr("installed %1, available %2").arg(row.installedRevision).arg(row.revision))
        else
            parts.push(qsTr("revision %1").arg(row.revision))
        return parts.join(" · ")
    }

    FolderDialog {
        id: rootFolderDialog
        property bool migrate: false
        title: migrate ? qsTr("Choose a new folder for the LUT library")
                       : qsTr("Choose an existing LUT folder")
        onAccepted: page.reviewFolder(selectedFolder.toString(), migrate)
    }

    // ── LUT library folder ───────────────────────────────────────────────
    SettingsSection {
        Layout.fillWidth: true
        Layout.topMargin: 26
        Layout.leftMargin: 34
        Layout.rightMargin: 34
        title: qsTr("LUT library")
        textColor: page.textColor
        dividerColor: page.dividerColor

        RowLayout {
            Layout.fillWidth: true
            spacing: 16

            Label {
                Layout.preferredWidth: 160
                Layout.alignment: Qt.AlignTop
                text: qsTr("Folder")
                color: page.textColor
                font.family: appTheme.uiFontFamily
                font.pixelSize: appTheme.fontSizeSection
                font.weight: appTheme.fontWeightStrong
            }

            ColumnLayout {
                Layout.fillWidth: true
                spacing: appTheme.spaceXs

                Label {
                    objectName: "lutSettingsRootPath"
                    Layout.fillWidth: true
                    text: page.library ? page.library.rootPath : ""
                    color: page.textColor
                    font.family: page.dataFontFamily
                    font.pixelSize: appTheme.fontSizeBody
                    wrapMode: Text.WrapAnywhere
                }

                Label {
                    objectName: "lutSettingsInventoryText"
                    Layout.fillWidth: true
                    text: !page.library ? ""
                          : (page.library.inventoryComplete
                             ? qsTr("%n LUT(s) in the inventory", "", page.library.entryCount)
                             : qsTr("%n LUT(s) in the inventory · verification incomplete, refresh to verify",
                                    "", page.library.entryCount))
                    color: page.mutedTextColor
                    font.family: appTheme.uiFontFamily
                    font.pixelSize: appTheme.fontSizeCaption
                    wrapMode: Text.Wrap
                }
            }
        }

        Flow {
            Layout.fillWidth: true
            Layout.leftMargin: 176
            spacing: appTheme.spaceSm

            DialogActionButton {
                objectName: "lutSettingsOpenFolderButton"
                buttonWidth: 150
                buttonHeight: 36
                text: qsTr("Open folder")
                enabled: !!page.library
                onClicked: {
                    page.actionError = ""
                    page.library.openRootDirectory()
                }
            }
            DialogActionButton {
                objectName: "lutSettingsRefreshButton"
                buttonWidth: 150
                buttonHeight: 36
                text: qsTr("Refresh")
                enabled: !!page.library && !page.libraryBusy
                onClicked: {
                    page.actionError = page.library.refresh()
                            ? "" : qsTr("Another LUT library operation is running. Try again when it finishes.")
                }
            }
            DialogActionButton {
                objectName: "lutSettingsUseFolderButton"
                buttonWidth: 150
                buttonHeight: 36
                text: qsTr("Use another folder…")
                enabled: !!page.library && !page.libraryBusy
                onClicked: page.chooseFolder(false)
            }
            DialogActionButton {
                objectName: "lutSettingsMoveButton"
                buttonWidth: 150
                buttonHeight: 36
                text: qsTr("Move library…")
                enabled: !!page.library && !page.libraryBusy
                onClicked: page.chooseFolder(true)
            }
        }

        // Confirmation of a chosen folder: the operation is shown before it starts.
        Rectangle {
            objectName: "lutSettingsFolderConfirmation"
            Layout.fillWidth: true
            Layout.leftMargin: 176
            visible: page.pendingFolder.length > 0
            implicitHeight: confirmColumn.implicitHeight + appTheme.spaceMd * 2
            radius: appTheme.panelRadius
            color: "transparent"
            border.width: 1
            border.color: page.dividerColor

            ColumnLayout {
                id: confirmColumn
                anchors.fill: parent
                anchors.margins: appTheme.spaceMd
                spacing: appTheme.spaceSm

                Label {
                    Layout.fillWidth: true
                    text: page.pendingMigrate ? qsTr("Move the LUT library to this folder?")
                                              : qsTr("Use this folder as the LUT library?")
                    color: page.textColor
                    font.family: appTheme.uiFontFamily
                    font.pixelSize: appTheme.fontSizeBody
                    font.weight: appTheme.fontWeightStrong
                    wrapMode: Text.Wrap
                }
                Label {
                    objectName: "lutSettingsPendingPath"
                    Layout.fillWidth: true
                    text: page.pendingPath
                    color: page.textColor
                    font.family: page.dataFontFamily
                    font.pixelSize: appTheme.fontSizeCaption
                    wrapMode: Text.WrapAnywhere
                }
                Label {
                    Layout.fillWidth: true
                    visible: page.pendingError.length === 0
                    text: page.pendingMigrate
                          ? qsTr("Every file is copied and verified first. The current folder stays in use until the copy is complete; then the copied files are removed from it.")
                          : qsTr("The LUTs in this folder are indexed. The current folder and its files are not changed.")
                    color: page.mutedTextColor
                    font.family: appTheme.uiFontFamily
                    font.pixelSize: appTheme.fontSizeCaption
                    wrapMode: Text.Wrap
                }
                Label {
                    objectName: "lutSettingsPendingError"
                    Layout.fillWidth: true
                    visible: page.pendingError.length > 0
                    text: page.pendingError
                    color: page.dangerColor
                    font.family: appTheme.uiFontFamily
                    font.pixelSize: appTheme.fontSizeCaption
                    wrapMode: Text.Wrap
                }
                RowLayout {
                    spacing: appTheme.spaceSm
                    DialogActionButton {
                        objectName: "lutSettingsConfirmFolderButton"
                        visible: page.pendingError.length === 0
                        kind: "accent"
                        buttonWidth: 150
                        buttonHeight: 36
                        text: page.pendingMigrate ? qsTr("Move library") : qsTr("Use folder")
                        enabled: !page.libraryBusy
                        onClicked: page.confirmPendingFolder()
                    }
                    DialogActionButton {
                        objectName: "lutSettingsCancelFolderButton"
                        buttonWidth: 150
                        buttonHeight: 36
                        text: qsTr("Cancel")
                        onClicked: page.clearPendingFolder()
                    }
                }
            }
        }

        // Running library operation.
        RowLayout {
            objectName: "lutSettingsOperationRow"
            Layout.fillWidth: true
            Layout.leftMargin: 176
            visible: page.libraryBusy
            spacing: appTheme.spaceSm

            ColumnLayout {
                Layout.fillWidth: true
                spacing: appTheme.spaceXs
                Label {
                    objectName: "lutSettingsOperationText"
                    Layout.fillWidth: true
                    text: page.library ? page.operationText(page.library.operation) : ""
                    color: page.textColor
                    font.family: appTheme.uiFontFamily
                    font.pixelSize: appTheme.fontSizeCaption
                    wrapMode: Text.Wrap
                }
                ThemedProgressBar {
                    Layout.fillWidth: true
                    indeterminate: true
                    active: page.libraryBusy
                }
            }
            DialogActionButton {
                objectName: "lutSettingsCancelOperationButton"
                visible: !!page.library && page.library.cancelable
                         && page.library.operation === "migrateRoot"
                buttonWidth: 120
                buttonHeight: 36
                text: qsTr("Cancel")
                onClicked: page.library.cancelOperation()
            }
        }

        Label {
            objectName: "lutSettingsLibraryError"
            Layout.fillWidth: true
            Layout.leftMargin: 176
            readonly property string message: page.actionError.length > 0
                                              ? page.actionError
                                              : (page.library ? page.library.lastError : "")
            visible: message.length > 0
            text: message
            color: page.dangerColor
            font.family: appTheme.uiFontFamily
            font.pixelSize: appTheme.fontSizeCaption
            wrapMode: Text.Wrap
        }

        Label {
            objectName: "lutSettingsKeptFilesText"
            Layout.fillWidth: true
            Layout.leftMargin: 176
            readonly property int keptCount: page.library ? page.library.keptSourcePaths.length : 0
            visible: keptCount > 0
            text: qsTr("%n file(s) changed after copying and stayed in the previous folder: %1", "",
                       keptCount).arg(page.library ? page.library.keptSourcePaths.join(", ") : "")
            color: page.mutedTextColor
            font.family: appTheme.uiFontFamily
            font.pixelSize: appTheme.fontSizeCaption
            wrapMode: Text.Wrap
        }
    }

    // ── Official LUT packages ─────────────────────────────────────────────
    SettingsSection {
        Layout.fillWidth: true
        Layout.leftMargin: 34
        Layout.rightMargin: 34
        Layout.bottomMargin: 26
        title: qsTr("Official LUT packages")
        textColor: page.textColor
        dividerColor: page.dividerColor

        Label {
            objectName: "lutSettingsPackagesUnavailable"
            Layout.fillWidth: true
            visible: !page.packagesEnabled
            text: qsTr("Official LUT package downloads are not available in this build.")
            color: page.mutedTextColor
            font.family: appTheme.uiFontFamily
            font.pixelSize: appTheme.fontSizeCaption
            wrapMode: Text.Wrap
        }

        RowLayout {
            Layout.fillWidth: true
            visible: page.packagesEnabled
            spacing: appTheme.spaceSm

            Label {
                objectName: "lutSettingsCheckText"
                Layout.fillWidth: true
                readonly property bool failed: !!page.packageService
                                               && page.packageService.lastError.length > 0
                text: !page.packageService ? ""
                      : (page.packageService.checking
                         ? qsTr("Checking the signed package list…")
                         : (failed ? qsTr("The package check failed: %1").arg(page.packageService.lastError)
                                   : (page.packageService.checked
                                      ? qsTr("Package list checked. Downloads start only when you choose one.")
                                      : qsTr("The package list has not been checked."))))
                color: failed ? page.dangerColor : page.mutedTextColor
                font.family: appTheme.uiFontFamily
                font.pixelSize: appTheme.fontSizeCaption
                wrapMode: Text.Wrap
            }

            DialogActionButton {
                objectName: "lutSettingsCheckButton"
                buttonWidth: 150
                buttonHeight: 36
                text: qsTr("Check again")
                enabled: !!page.packageService && !page.packageService.checking
                onClicked: page.packageService.checkPackages()
            }
        }

        ColumnLayout {
            id: packageColumn
            objectName: "lutSettingsPackageList"
            Layout.fillWidth: true
            visible: page.packagesEnabled
            spacing: appTheme.spaceSm

            // Instantiator over the row count, not the row list: rows persist while
            // progress changes, so a Cancel press is not lost to a rebuilt row.
            Instantiator {
                model: page.packageRows.length
                onObjectAdded: function(index, object) {
                    object.parent = packageColumn
                }
                delegate: PackageRow {
                    required property int index
                    panel: page
                    row: page.packageRows[index] || ({})
                }
            }
        }
    }

    Item {
        Layout.fillHeight: true
    }

    component PackageRow: Rectangle {
        id: packageRow

        // Inline components do not see the enclosing ids; the delegate passes the page.
        required property var panel
        property var row: ({})
        readonly property string packageId: row.id || ""
        readonly property string action: row.action || ""
        readonly property bool busy: !!row.busy

        objectName: "lutSettingsPackage:" + packageId
        Layout.fillWidth: true
        implicitHeight: rowLayout.implicitHeight + appTheme.spaceMd * 2
        radius: appTheme.panelRadius
        color: "transparent"
        border.width: 1
        border.color: packageRow.panel.dividerColor

        RowLayout {
            id: rowLayout
            anchors.fill: parent
            anchors.margins: appTheme.spaceMd
            spacing: appTheme.spaceMd

            ColumnLayout {
                Layout.fillWidth: true
                spacing: appTheme.spaceXs

                Label {
                    Layout.fillWidth: true
                    text: packageRow.row.name || packageRow.packageId
                    color: packageRow.panel.textColor
                    font.family: appTheme.uiFontFamily
                    font.pixelSize: appTheme.fontSizeBody
                    font.weight: appTheme.fontWeightStrong
                    wrapMode: Text.Wrap
                }
                Label {
                    objectName: "lutSettingsPackageStatus:" + packageRow.packageId
                    Layout.fillWidth: true
                    text: packageRow.panel.packageStatusText(packageRow.row)
                    color: packageRow.panel.textColor
                    font.family: appTheme.uiFontFamily
                    font.pixelSize: appTheme.fontSizeCaption
                    wrapMode: Text.Wrap
                }
                Label {
                    Layout.fillWidth: true
                    text: packageRow.panel.packageDetailText(packageRow.row)
                    color: packageRow.panel.mutedTextColor
                    font.family: packageRow.panel.dataFontFamily
                    font.pixelSize: appTheme.fontSizeCaption
                    wrapMode: Text.Wrap
                }
                ThemedProgressBar {
                    Layout.fillWidth: true
                    visible: packageRow.busy
                    active: packageRow.busy
                    indeterminate: packageRow.row.status !== "downloading"
                    progressValue: (packageRow.row.progress || 0) * 100
                }
                Label {
                    objectName: "lutSettingsPackageError:" + packageRow.packageId
                    Layout.fillWidth: true
                    visible: text.length > 0
                    text: packageRow.row.error || ""
                    color: packageRow.panel.dangerColor
                    font.family: appTheme.uiFontFamily
                    font.pixelSize: appTheme.fontSizeCaption
                    wrapMode: Text.Wrap
                }
            }

            DialogActionButton {
                objectName: "lutSettingsPackageAction:" + packageRow.packageId
                Layout.alignment: Qt.AlignTop
                visible: packageRow.action.length > 0
                kind: packageRow.action === "retry" ? "normal" : "accent"
                buttonWidth: 120
                buttonHeight: 36
                text: packageRow.panel.packageActionText(packageRow.action)
                onClicked: packageRow.panel.packageService.installPackage(packageRow.packageId)
            }
            DialogActionButton {
                objectName: "lutSettingsPackageCancel:" + packageRow.packageId
                Layout.alignment: Qt.AlignTop
                visible: !!packageRow.row.cancelable
                buttonWidth: 120
                buttonHeight: 36
                text: qsTr("Cancel")
                onClicked: packageRow.panel.packageService.cancelInstall(packageRow.packageId)
            }
        }
    }

    component SettingsSection: ColumnLayout {
        id: section

        property string title: ""
        property color textColor: appTheme.textColor
        property color dividerColor: appTheme.dividerColor

        spacing: 14

        Label {
            Layout.fillWidth: true
            text: section.title
            color: section.textColor
            font.family: appTheme.uiFontFamily
            font.pixelSize: appTheme.fontSizeSection
            font.weight: appTheme.fontWeightHeading
        }

        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: 1
            color: section.dividerColor
        }
    }
}
