pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Dialogs
import QtQuick.Layouts

// Settings > LUTs, laid out as bento tiles: the LUT library folder (open,
// change, refresh), the total LUT count, the film simulation / custom split,
// and the official LUT packages as store cards (download, update, repair,
// cancel, retry, remove). "Change folder" moves the library into an empty folder and
// switches to a folder that already holds files (LutLibraryService::
// RootChangeMigrates). The dialog starts the signed package check when
// Settings opens; this page never starts one by itself except "Check again".
//
// Data (counts, sizes, revisions, paths) is set in Manrope
// (appTheme.headlineFontFamily); IBM Plex Sans is being retired.
ColumnLayout {
    id: page
    objectName: "lutSettingsPanel"

    property var library: null
    property var packageService: null
    property color textColor: appTheme.textColor
    property color mutedTextColor: appTheme.textMutedColor
    property color dividerColor: appTheme.dividerColor
    property color dangerColor: appTheme.dangerColor
    property string dataFontFamily: appTheme.headlineFontFamily

    readonly property bool libraryBusy: !!library && library.busy
    readonly property bool packagesEnabled: !!packageService && packageService.enabled
    readonly property var packageRows: packageService ? packageService.packages : []
    readonly property int totalCount: library ? library.entryCount : 0
    readonly property int filmCount: library ? Math.min(library.filmSimulationCount, totalCount) : 0
    readonly property int customCount: totalCount - filmCount

    // A folder chosen in the dialog, waiting for confirmation.
    property string pendingFolder: ""
    property bool pendingMigrate: false
    property string pendingPath: ""
    property string pendingError: ""
    // A request the library refused to start (another operation runs).
    property string actionError: ""
    // Package whose removal waits for confirmation on its card.
    property string pendingRemovalId: ""

    spacing: appTheme.spaceMd

    function chooseFolder() {
        actionError = ""
        rootFolderDialog.open()
    }

    // Called with the dialog's folder URL; exposed for tests that cannot drive
    // the native folder dialog. The library decides between moving and switching.
    function reviewFolder(folder) {
        if (!library)
            return
        const check = library.checkRootChange(folder)
        pendingFolder = folder
        pendingMigrate = check.migrate
        pendingPath = check.path
        pendingError = check.error
    }

    function clearPendingFolder() {
        pendingFolder = ""
        pendingPath = ""
        pendingError = ""
    }

    function confirmPackageRemoval() {
        if (!packageService || pendingRemovalId.length === 0)
            return
        packageService.removePackage(pendingRemovalId)
        pendingRemovalId = ""
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
        case "removePackage": return qsTr("Removing a LUT package…")
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
        case "removing": return qsTr("Removing…")
        case "installed": return qsTr("Installed")
        case "error": return qsTr("Not completed")
        }
        return ""
    }

    function packageStatusColor(row) {
        switch (row.status) {
        case "current":
        case "installed": return appTheme.accentColor
        case "updateAvailable":
        case "repairRequired": return appTheme.toneGold
        case "error": return page.dangerColor
        }
        return page.mutedTextColor
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

    // GitHub repository ("owner/name") of the project a package is derived from.
    function packageUpstream(packageId) {
        switch (packageId) {
        case "spectral_film_lut": return "JanLohse/spectral_film_lut"
        case "spektrafilm_lut": return "andreavolpato/spektrafilm"
        }
        return ""
    }

    function packageRevisionText(row) {
        const installed = row.installedRevision || ""
        const available = row.revision || ""
        // A package the feed does not list has only its installed revision.
        if (available.length === 0)
            return installed
        if (installed.length > 0 && installed !== available)
            return qsTr("%1 → %2").arg(installed).arg(available)
        return available
    }

    FolderDialog {
        id: rootFolderDialog
        title: qsTr("Choose a folder for the LUT library")
        onAccepted: page.reviewFolder(selectedFolder.toString())
    }

    // ── Bento row 1: library folder + total ───────────────────────────────
    RowLayout {
        Layout.fillWidth: true
        Layout.topMargin: 26
        Layout.leftMargin: 34
        Layout.rightMargin: 34
        spacing: appTheme.spaceMd

        BentoTile {
            objectName: "lutSettingsLibraryTile"
            Layout.fillWidth: true
            Layout.fillHeight: true
            contentHeight: libraryColumn.implicitHeight

            ColumnLayout {
                id: libraryColumn
                anchors.fill: parent
                spacing: appTheme.spaceMd

                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: appTheme.spaceXs

                    Label {
                        Layout.fillWidth: true
                        text: qsTr("LUT library")
                        color: page.textColor
                        font.family: appTheme.headlineFontFamily
                        font.pixelSize: appTheme.fontSizeHeadline
                        font.weight: Font.DemiBold
                    }
                    Label {
                        Layout.fillWidth: true
                        text: qsTr("The folder that holds every LUT available for color grading.")
                        color: page.mutedTextColor
                        font.family: appTheme.uiFontFamily
                        font.pixelSize: appTheme.fontSizeBody
                        wrapMode: Text.Wrap
                    }
                }

                // The folder button asks for a folder; RootChangeMigrates decides
                // between moving the library and switching to the folder.
                FolderPathField {
                    Layout.fillWidth: true
                    controlHeight: 44
                    pathPixelSize: appTheme.fontSizeSection
                    iconSize: 20
                    iconSource: "qrc:/panel_icons/transfer-out.svg"
                    path: page.library ? page.library.rootPath : ""
                    textColor: page.textColor
                    mutedTextColor: page.mutedTextColor
                    pathFontFamily: page.dataFontFamily
                    browseEnabled: !!page.library && !page.libraryBusy
                    browseToolTip: qsTr("Change folder…")
                    pathLabel.objectName: "lutSettingsRootPath"
                    browseButton.objectName: "lutSettingsChangeFolderButton"
                    onBrowseRequested: page.chooseFolder()
                }

                RowLayout {
                    Layout.fillWidth: true
                    spacing: 12

                    SystemActionButton {
                        objectName: "lutSettingsOpenFolderButton"
                        Layout.fillWidth: true
                        text: Qt.platform.os === "osx" ? qsTr("Open in Finder")
                              : (Qt.platform.os === "windows" ? qsTr("Open in File Explorer")
                                                              : qsTr("Open folder"))
                        enabled: !!page.library
                        onClicked: {
                            page.actionError = ""
                            page.library.openRootDirectory()
                        }
                    }
                    IconButton {
                        objectName: "lutSettingsRefreshButton"
                        buttonSize: 44
                        buttonRadius: 10
                        iconSize: 20
                        iconSrc: "qrc:/panel_icons/retry.svg"
                        normalColor: Qt.rgba(1, 1, 1, 0.07)
                        borderColor: Qt.rgba(page.textColor.r, page.textColor.g, page.textColor.b, 0.14)
                        tooltipText: qsTr("Refresh")
                        spinning: page.libraryBusy
                        enabled: !!page.library && !page.libraryBusy
                        onClicked: {
                            page.actionError = page.library.refresh()
                                    ? "" : qsTr("Another LUT library operation is running. Try again when it finishes.")
                        }
                    }
                }

                // Confirmation of a chosen folder: the operation is shown before it starts.
                Rectangle {
                    objectName: "lutSettingsFolderConfirmation"
                    Layout.fillWidth: true
                    visible: page.pendingFolder.length > 0
                    implicitHeight: confirmColumn.implicitHeight + appTheme.spaceMd * 2
                    radius: appTheme.controlRadius
                    color: appTheme.bgBaseColor
                    border.width: 1
                    border.color: page.pendingError.length > 0 ? page.dangerColor : appTheme.accentColor

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
                            objectName: "lutSettingsPendingDescription"
                            Layout.fillWidth: true
                            visible: page.pendingError.length === 0
                            text: page.pendingMigrate
                                  ? qsTr("The folder is empty, so the library moves into it. Every file is copied and verified first. The current folder stays in use until the copy is complete; then the copied files are removed from it.")
                                  : qsTr("The folder already holds files, so its LUTs become the library. The %n LUT(s) in the current folder are not moved and stay where they are.",
                                         "", page.totalCount)
                            color: page.mutedTextColor
                            font.family: appTheme.uiFontFamily
                            font.pixelSize: appTheme.fontSizeCaption
                            wrapMode: Text.Wrap
                        }
                        AlertBadge {
                            objectName: "lutSettingsPendingError"
                            Layout.fillWidth: true
                            text: page.pendingError
                        }
                        RowLayout {
                            spacing: appTheme.spaceSm
                            DialogActionButton {
                                objectName: "lutSettingsConfirmFolderButton"
                                visible: page.pendingError.length === 0
                                kind: "accent"
                                buttonWidth: 132
                                buttonHeight: 34
                                text: page.pendingMigrate ? qsTr("Move library") : qsTr("Use folder")
                                enabled: !page.libraryBusy
                                onClicked: page.confirmPendingFolder()
                            }
                            DialogActionButton {
                                objectName: "lutSettingsCancelFolderButton"
                                buttonWidth: 112
                                buttonHeight: 34
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
                        buttonWidth: 100
                        buttonHeight: 34
                        text: qsTr("Cancel")
                        onClicked: page.library.cancelOperation()
                    }
                }

                AlertBadge {
                    objectName: "lutSettingsLibraryError"
                    Layout.fillWidth: true
                    readonly property string message: page.actionError.length > 0
                                                      ? page.actionError
                                                      : (page.library ? page.library.lastError : "")
                    text: message
                }

                Label {
                    objectName: "lutSettingsKeptFilesText"
                    Layout.fillWidth: true
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
        }

        BentoTile {
            objectName: "lutSettingsTotalTile"
            Layout.preferredWidth: 190
            Layout.fillHeight: true
            contentHeight: totalColumn.implicitHeight

            ColumnLayout {
                id: totalColumn
                anchors.fill: parent
                spacing: 0

                Label {
                    Layout.fillWidth: true
                    horizontalAlignment: Text.AlignHCenter
                    text: qsTr("Total")
                    color: page.mutedTextColor
                    font.family: page.dataFontFamily
                    font.pixelSize: appTheme.fontSizeSection
                    font.weight: Font.Medium
                }
                Item {
                    Layout.fillHeight: true
                }
                // Centered, so the gaps left and right of the number are equal.
                Label {
                    objectName: "lutSettingsTotalCount"
                    Layout.fillWidth: true
                    horizontalAlignment: Text.AlignHCenter
                    text: String(page.totalCount)
                    color: page.textColor
                    font.family: page.dataFontFamily
                    font.pixelSize: 72
                    font.weight: Font.ExtraLight
                    font.features: { "tnum": 1 }
                    fontSizeMode: Text.HorizontalFit
                    minimumPixelSize: 32
                    elide: Text.ElideNone
                }
                Item {
                    Layout.fillHeight: true
                }
            }
        }
    }

    // ── Bento row 2: film simulation / custom split ───────────────────────
    BentoTile {
        objectName: "lutSettingsCompositionTile"
        Layout.fillWidth: true
        Layout.leftMargin: 34
        Layout.rightMargin: 34
        contentHeight: compositionColumn.implicitHeight

        ColumnLayout {
            id: compositionColumn
            anchors.fill: parent
            spacing: appTheme.spaceMd

            RowLayout {
                Layout.fillWidth: true
                spacing: appTheme.spaceXl

                Label {
                    Layout.fillWidth: true
                    text: qsTr("Categories")
                    color: page.textColor
                    font.family: appTheme.uiFontFamily
                    font.pixelSize: appTheme.fontSizeSection
                    font.weight: appTheme.fontWeightHeading
                }
                LegendItem {
                    objectName: "lutSettingsFilmCount"
                    swatch: appTheme.accentColor
                    label: qsTr("Film simulation")
                    count: page.filmCount
                }
                LegendItem {
                    objectName: "lutSettingsCustomCount"
                    swatch: appTheme.toneMist
                    label: qsTr("Custom")
                    count: page.customCount
                }
            }

            Rectangle {
                id: compositionTrack
                Layout.fillWidth: true
                implicitHeight: 10
                radius: height / 2
                color: appTheme.bgBaseColor
                clip: true

                Row {
                    anchors.fill: parent
                    spacing: page.filmCount > 0 && page.customCount > 0 ? 3 : 0

                    Rectangle {
                        height: parent.height
                        width: page.totalCount > 0
                               ? (compositionTrack.width - parent.spacing) * page.filmCount / page.totalCount : 0
                        radius: height / 2
                        color: appTheme.accentColor
                    }
                    Rectangle {
                        height: parent.height
                        width: page.totalCount > 0
                               ? (compositionTrack.width - parent.spacing) * page.customCount / page.totalCount : 0
                        radius: height / 2
                        color: appTheme.toneMist
                        opacity: 0.7
                    }
                }
            }
        }
    }

    // ── Official LUT packages ─────────────────────────────────────────────
    RowLayout {
        Layout.fillWidth: true
        Layout.topMargin: appTheme.spaceLg
        Layout.leftMargin: 34
        Layout.rightMargin: 34
        spacing: appTheme.spaceMd

        ColumnLayout {
            Layout.fillWidth: true
            spacing: 2

            Label {
                Layout.fillWidth: true
                text: qsTr("Official LUT Packages")
                color: page.textColor
                font.family: appTheme.headlineFontFamily
                font.pixelSize: 18
                font.weight: Font.DemiBold
            }
            // Only a failed check is reported; a running check spins the refresh icon.
            AlertBadge {
                objectName: "lutSettingsCheckText"
                Layout.fillWidth: true
                text: page.packagesEnabled && !page.packageService.checking
                      && page.packageService.lastError.length > 0
                      ? qsTr("The package check failed: %1").arg(page.packageService.lastError) : ""
            }
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
        }

        IconButton {
            objectName: "lutSettingsCheckButton"
            Layout.alignment: Qt.AlignVCenter
            visible: page.packagesEnabled
            buttonSize: 36
            buttonRadius: 10
            iconSize: 18
            iconSrc: "qrc:/panel_icons/retry.svg"
            normalColor: Qt.rgba(1, 1, 1, 0.07)
            borderColor: Qt.rgba(page.textColor.r, page.textColor.g, page.textColor.b, 0.14)
            tooltipText: qsTr("Check again")
            spinning: !!page.packageService && page.packageService.checking
            enabled: !!page.packageService && !page.packageService.checking
            onClicked: page.packageService.checkPackages()
        }
    }

    GridLayout {
        id: packageGrid
        objectName: "lutSettingsPackageList"
        Layout.fillWidth: true
        Layout.leftMargin: 34
        Layout.rightMargin: 34
        Layout.bottomMargin: 26
        // Installed packages are listed (and removable) even without the feed.
        visible: page.packagesEnabled || page.packageRows.length > 0
        columns: width >= 600 ? 2 : 1
        columnSpacing: appTheme.spaceMd
        rowSpacing: appTheme.spaceMd

        // Instantiator over the row count, not the row list: cards persist while
        // progress changes, so a Cancel press is not lost to a rebuilt card.
        Instantiator {
            model: page.packageRows.length
            onObjectAdded: function(index, object) {
                object.parent = packageGrid
            }
            delegate: PackageCard {
                required property int index
                panel: page
                row: page.packageRows[index] || ({})
            }
        }
    }

    Item {
        Layout.fillHeight: true
    }

    component BentoTile: Rectangle {
        // Children are placed in `content`, inset by the tile padding.
        default property alias content: tileContent.data
        property real contentHeight: 0

        implicitHeight: contentHeight + 22 * 2
        radius: 14
        color: appTheme.cardSurfaceColor
        border.width: 1
        border.color: appTheme.cardBorderColor

        Item {
            id: tileContent
            anchors.fill: parent
            anchors.margins: 22
        }
    }

    // A wide button that hands the folder to the system: the file manager's own
    // icon (image://alcedo-system-icon) with a bundled folder icon as fallback.
    component SystemActionButton: Rectangle {
        id: systemButton

        property string text: ""

        signal clicked()

        implicitHeight: 44
        Layout.preferredHeight: 44
        radius: 10
        opacity: enabled ? 1.0 : 0.45
        color: systemMouse.pressed
               ? Qt.rgba(1, 1, 1, 0.06)
               : (systemMouse.containsMouse ? Qt.rgba(1, 1, 1, 0.12) : Qt.rgba(1, 1, 1, 0.07))
        border.width: 1
        border.color: Qt.rgba(appTheme.textColor.r, appTheme.textColor.g, appTheme.textColor.b, 0.14)

        Row {
            anchors.centerIn: parent
            spacing: 10

            Item {
                anchors.verticalCenter: parent.verticalCenter
                width: 20
                height: 20

                Image {
                    id: systemIcon
                    anchors.fill: parent
                    source: "image://alcedo-system-icon/file-manager"
                    sourceSize.width: 40
                    sourceSize.height: 40
                    smooth: true
                    mipmap: true
                }
                Image {
                    anchors.fill: parent
                    // The provider answers 1x1 when the system has no icon.
                    visible: systemIcon.status !== Image.Ready || systemIcon.implicitWidth <= 1
                    source: "qrc:/panel_icons/folder-open.svg"
                    sourceSize.width: 20
                    sourceSize.height: 20
                }
            }
            Label {
                anchors.verticalCenter: parent.verticalCenter
                text: systemButton.text
                color: appTheme.textColor
                font.family: appTheme.uiFontFamily
                font.pixelSize: appTheme.fontSizeSection
                font.weight: appTheme.fontWeightStrong
            }
        }

        MouseArea {
            id: systemMouse
            anchors.fill: parent
            hoverEnabled: true
            cursorShape: systemButton.enabled ? Qt.PointingHandCursor : Qt.ArrowCursor
            onClicked: systemButton.clicked()
        }
    }

    component LegendItem: RowLayout {
        id: legend

        property color swatch: "transparent"
        property string label: ""
        property int count: 0

        spacing: appTheme.spaceSm

        Rectangle {
            Layout.alignment: Qt.AlignVCenter
            implicitWidth: 8
            implicitHeight: 8
            radius: 4
            color: legend.swatch
        }
        Label {
            text: legend.label
            color: appTheme.textMutedColor
            font.family: appTheme.uiFontFamily
            font.pixelSize: appTheme.fontSizeBody
        }
        Label {
            text: String(legend.count)
            color: appTheme.textColor
            font.family: appTheme.headlineFontFamily
            font.pixelSize: appTheme.fontSizeSection
            font.weight: Font.DemiBold
        }
    }

    component Pill: Rectangle {
        id: pill

        property string text: ""

        visible: text.length > 0
        implicitWidth: pillLabel.implicitWidth + appTheme.spaceMd * 2
        implicitHeight: pillLabel.implicitHeight + appTheme.spaceXs * 2 + 2
        radius: height / 2
        color: appTheme.bgBaseColor
        border.width: 1
        border.color: appTheme.cardBorderColor

        Label {
            id: pillLabel
            anchors.centerIn: parent
            text: pill.text
            color: appTheme.textColor
            font.family: appTheme.headlineFontFamily
            font.pixelSize: appTheme.fontSizeBody
            font.weight: Font.Medium
        }
    }

    component PackageCard: Rectangle {
        id: packageCard

        // Inline components do not see the enclosing ids; the delegate passes the page.
        required property var panel
        property var row: ({})
        readonly property string packageId: row.id || ""
        readonly property string action: row.action || ""
        readonly property bool busy: !!row.busy
        readonly property bool confirmingRemoval: panel.pendingRemovalId === packageId
        readonly property string upstream: panel.packageUpstream(packageId)

        objectName: "lutSettingsPackage:" + packageId
        Layout.fillWidth: true
        // Equal preferred widths split the columns evenly.
        Layout.preferredWidth: 1
        // Near-square store card; grows when the content needs more room.
        Layout.preferredHeight: Math.max(cardColumn.implicitHeight + 22 * 2, width * 0.72)
        radius: 14
        color: appTheme.cardSurfaceColor
        border.width: 1
        border.color: packageCard.busy ? appTheme.accentColor : appTheme.cardBorderColor

        ColumnLayout {
            id: cardColumn
            anchors.fill: parent
            anchors.margins: 22
            spacing: appTheme.spaceMd

            RowLayout {
                Layout.fillWidth: true
                spacing: appTheme.spaceMd

                ColumnLayout {
                    Layout.fillWidth: true
                    Layout.alignment: Qt.AlignTop
                    spacing: appTheme.spaceXs

                    Label {
                        objectName: "lutSettingsPackageName:" + packageCard.packageId
                        Layout.fillWidth: true
                        text: packageCard.row.name || packageCard.packageId
                        color: appTheme.textColor
                        font.family: appTheme.headlineFontFamily
                        font.pixelSize: 28
                        font.weight: Font.DemiBold
                        lineHeight: 1.1
                        wrapMode: Text.Wrap
                        maximumLineCount: 3
                        elide: Text.ElideRight
                    }
                    Label {
                        Layout.fillWidth: true
                        visible: packageCard.upstream.length > 0
                        text: qsTr("Based on %1").arg(packageCard.upstream)
                        color: appTheme.textMutedColor
                        font.family: appTheme.headlineFontFamily
                        font.pixelSize: appTheme.fontSizeBody
                        elide: Text.ElideRight
                    }
                }

                IconButton {
                    objectName: "lutSettingsPackageUpstream:" + packageCard.packageId
                    Layout.alignment: Qt.AlignTop
                    visible: packageCard.upstream.length > 0
                    buttonSize: 36
                    buttonRadius: 10
                    iconSize: 18
                    iconSrc: "qrc:/panel_icons/github.svg"
                    normalColor: Qt.rgba(1, 1, 1, 0.07)
                    borderColor: Qt.rgba(appTheme.textColor.r, appTheme.textColor.g, appTheme.textColor.b, 0.14)
                    tooltipText: qsTr("View the original project on GitHub")
                    onClicked: Qt.openUrlExternally("https://github.com/" + packageCard.upstream)
                }
            }

            Flow {
                Layout.fillWidth: true
                spacing: appTheme.spaceSm

                Pill {
                    text: qsTr("%n LUT(s)", "", Number(packageCard.row.fileCount || 0))
                }
                Pill {
                    text: packageCard.panel.packageRevisionText(packageCard.row)
                }
                Pill {
                    text: packageCard.panel.formatBytes(packageCard.row.archiveBytes)
                }
            }

            Item {
                Layout.fillHeight: true
            }

            AlertBadge {
                objectName: "lutSettingsPackageError:" + packageCard.packageId
                Layout.fillWidth: true
                text: packageCard.row.error || ""
            }

            // Removal confirmation: the package's LUTs leave the library.
            ColumnLayout {
                objectName: "lutSettingsPackageRemoveConfirm:" + packageCard.packageId
                Layout.fillWidth: true
                visible: packageCard.confirmingRemoval
                spacing: appTheme.spaceSm

                Label {
                    Layout.fillWidth: true
                    text: qsTr("Remove this package? Its LUTs leave the library. Favorites are kept.")
                    color: appTheme.textColor
                    font.family: appTheme.uiFontFamily
                    font.pixelSize: appTheme.fontSizeCaption
                    wrapMode: Text.Wrap
                }
                AlertBadge {
                    objectName: "lutSettingsPackageRemoveWarning:" + packageCard.packageId
                    text: qsTr("Photos that have these LUTs applied lose that look until the package is installed again.")
                }
                RowLayout {
                    Layout.fillWidth: true
                    spacing: appTheme.spaceSm

                    Item {
                        Layout.fillWidth: true
                    }
                    DialogActionButton {
                        objectName: "lutSettingsPackageRemoveKeep:" + packageCard.packageId
                        buttonWidth: 100
                        buttonHeight: 34
                        text: qsTr("Keep")
                        onClicked: packageCard.panel.pendingRemovalId = ""
                    }
                    DialogActionButton {
                        objectName: "lutSettingsPackageRemoveConfirmButton:" + packageCard.packageId
                        kind: "danger"
                        buttonWidth: 100
                        buttonHeight: 34
                        enabled: !packageCard.panel.libraryBusy && !!packageCard.row.removable
                        text: qsTr("Remove")
                        onClicked: packageCard.panel.confirmPackageRemoval()
                    }
                }
            }

            ThemedProgressBar {
                Layout.fillWidth: true
                visible: packageCard.busy
                active: packageCard.busy
                indeterminate: packageCard.row.status !== "downloading"
                progressValue: (packageCard.row.progress || 0) * 100
            }

            RowLayout {
                Layout.fillWidth: true
                visible: !packageCard.confirmingRemoval
                spacing: appTheme.spaceSm

                Rectangle {
                    Layout.alignment: Qt.AlignVCenter
                    visible: !packageCard.busy
                    implicitWidth: 8
                    implicitHeight: 8
                    radius: 4
                    color: packageCard.panel.packageStatusColor(packageCard.row)
                }
                Label {
                    objectName: "lutSettingsPackageStatus:" + packageCard.packageId
                    Layout.fillWidth: true
                    Layout.alignment: Qt.AlignVCenter
                    text: packageCard.panel.packageStatusText(packageCard.row)
                    color: appTheme.textColor
                    font.family: appTheme.uiFontFamily
                    font.pixelSize: appTheme.fontSizeBody
                    wrapMode: Text.Wrap
                }
                DialogActionButton {
                    objectName: "lutSettingsPackageAction:" + packageCard.packageId
                    visible: packageCard.action.length > 0
                    kind: packageCard.action === "retry" ? "normal" : "accent"
                    buttonWidth: 116
                    buttonHeight: 34
                    text: packageCard.panel.packageActionText(packageCard.action)
                    onClicked: packageCard.panel.packageService.installPackage(packageCard.packageId)
                }
                // The shared trash glyph, in the same chrome as the card's GitHub action.
                IconButton {
                    objectName: "lutSettingsPackageRemove:" + packageCard.packageId
                    Layout.alignment: Qt.AlignVCenter
                    visible: !!packageCard.row.removable && !packageCard.busy
                    enabled: !packageCard.panel.libraryBusy
                    buttonSize: 34
                    buttonRadius: 10
                    iconSize: 18
                    iconSrc: "qrc:/panel_icons/trash.svg"
                    normalColor: Qt.rgba(1, 1, 1, 0.07)
                    borderColor: Qt.rgba(appTheme.textColor.r, appTheme.textColor.g,
                                         appTheme.textColor.b, 0.14)
                    tooltipText: qsTr("Remove package")
                    onClicked: packageCard.panel.pendingRemovalId = packageCard.packageId
                }
                DialogActionButton {
                    objectName: "lutSettingsPackageCancel:" + packageCard.packageId
                    visible: !!packageCard.row.cancelable
                    buttonWidth: 100
                    buttonHeight: 34
                    text: qsTr("Cancel")
                    onClicked: packageCard.panel.packageService.cancelInstall(packageCard.packageId)
                }
            }
        }
    }
}
