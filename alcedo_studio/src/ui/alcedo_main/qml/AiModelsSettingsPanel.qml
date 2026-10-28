pragma ComponentBehavior: Bound

import QtQuick
import QtQml.Models
import QtQuick.Controls.Basic
import QtQuick.Dialogs
import QtQuick.Layouts

// Settings > AI Models (AI Models plan, AM4): the one place to download, update, remove, and
// activate the external AI models, in the layout of Settings > LUTs.
//
// - The model directory tile (one directory for all AI models, plan P7).
// - "AI Mask": one PackageCard for the AI Mask model set (AiModelPackageService, signed feed).
// - "Content Recognition (CLIP)": the download source, then one PackageCard per CLIP model
//   (ModelDownloadController) with Download, Cancel, Remove, and Activate for the open project
//   (SemanticGenerationController). The active model of the open project has the active border.
// - The download statement (plan section 4) opens before the first download of any model. The
//   About page shows it read-only.
//
// The dialog starts the signed feed check when this page opens; the page never starts one by
// itself except "Check again".
ColumnLayout {
    id: page
    objectName: "aiModelsSettingsPanel"

    // AiModelPackageService (appModules.aiModelPackages).
    property var packageService: null
    // ModelDownloadController (appModules.modelDownload).
    property var downloadController: null
    // SemanticGenerationController (appModules.semanticGeneration).
    property var semanticController: null
    property var interactionPolicy: null
    // True while a project is open; CLIP activation is per project (plan A3).
    property bool projectOpen: false

    readonly property bool packagesEnabled: !!packageService && packageService.enabled
    readonly property var maskRows: packageService ? packageService.packages : []
    readonly property var clipRows: downloadController ? downloadController.models : []
    readonly property bool statementAccepted: !!packageService && packageService.downloadStatementAccepted
    readonly property bool clipDownloadRunning: !!downloadController && downloadController.modelDownloadRunning
    readonly property bool canChangeDownloadSettings: !interactionPolicy
                                                      || interactionPolicy.canChangeModelDownloadSettings
    readonly property bool canChangeSemanticModel: !interactionPolicy
                                                   || interactionPolicy.canChangeSemanticModel
    readonly property string activeModelProfileId: semanticController
                                                   ? String(semanticController.activeModelProfileId || "") : ""
    readonly property string activatingModelProfileId: semanticController
                                                       ? String(semanticController.activatingModelProfileId || "") : ""
    readonly property bool modelActivationRunning: !!semanticController && semanticController.modelActivationRunning

    // The download that waits for the statement: {kind: "aiMask" | "clip", id}.
    property var pendingDownload: null
    // The card whose removal waits for confirmation: "aiMask:<id>" or "clip:<profileId>".
    property string pendingRemovalKey: ""

    spacing: appTheme.spaceMd

    // Every download passes here: before the statement is accepted, the statement opens and
    // the download waits for "Accept and Download" (plan P8).
    function requestDownload(kind, id) {
        if (!statementAccepted) {
            pendingDownload = { kind: kind, id: id }
            statementDialog.openForDownload()
            return
        }
        startDownload(kind, id)
    }

    function startDownload(kind, id) {
        if (kind === "aiMask" && packageService)
            packageService.installPackage(id)
        else if (kind === "clip" && downloadController)
            downloadController.installModel(id)
    }

    function acceptStatement() {
        const download = pendingDownload
        pendingDownload = null
        if (!packageService)
            return
        packageService.acceptDownloadStatement()
        if (download)
            startDownload(download.kind, download.id)
    }

    function declineStatement() {
        pendingDownload = null
    }

    function confirmRemoval() {
        const key = pendingRemovalKey
        pendingRemovalKey = ""
        if (key.startsWith("aiMask:") && packageService)
            packageService.removePackage(key.substring(7))
        else if (key.startsWith("clip:") && downloadController)
            downloadController.removeModel(key.substring(5))
    }

    function refreshInstallations() {
        if (downloadController)
            downloadController.refreshModels()
        if (packageService)
            packageService.refreshInstallations()
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

    function localFileUrl(path) {
        const normalized = String(path || "").replace(/\\/g, "/")
        if (normalized.length === 0)
            return ""
        return normalized.startsWith("/") ? "file://" + normalized : "file:///" + normalized
    }

    // ── AI Mask model set ────────────────────────────────────────────────

    // What the AI Mask set does, in one sentence (the revision Pill shows the version).
    readonly property string maskDescription:
        qsTr("Selects the subject, people, sky, and the objects that you click, box, or draw over.")
    // The project that trains and prepares the AI Mask models.
    readonly property string maskProjectUrl: "https://github.com/zidage/alcedo-ai-mask-lab"

    function maskRevisionText(row) {
        const installedVersion = Number(row.installedModelSetVersion || 0)
        const installed = installedVersion > 0 && (row.installedRevision || "").length > 0
                          ? qsTr("v%1 %2").arg(installedVersion).arg(row.installedRevision) : ""
        const available = row.available && Number(row.modelSetVersion || 0) > 0
                          ? qsTr("v%1 %2").arg(Number(row.modelSetVersion)).arg(row.revision || "") : ""
        if (available.length === 0)
            return installed
        if (installed.length > 0 && installed !== available)
            return qsTr("%1 → %2").arg(installed).arg(available)
        return available
    }

    // License files are in the installed set; before the installation, the name only.
    function maskLicenses(row) {
        const licenses = row.licenses || []
        const installedVersion = Number(row.installedModelSetVersion || 0)
        const installed = installedVersion > 0 && (row.installedRevision || "").length > 0
        const directory = downloadController ? String(downloadController.modelDownloadDirectory || "") : ""
        const result = []
        for (let i = 0; i < licenses.length; ++i) {
            const file = String(licenses[i].file || "")
            result.push({
                name: licenses[i].name,
                url: installed && directory.length > 0 && file.length > 0
                     ? localFileUrl(directory + "/alcedo-ai-mask-v" + installedVersion + "/" + file) : ""
            })
        }
        return result
    }

    function maskStatusText(row) {
        switch (row.status) {
        case "notInstalled": return qsTr("Not installed")
        case "current": return qsTr("Installed and current")
        case "updateAvailable": return qsTr("Update available")
        case "updateRequired": return qsTr("Update required: this version of Alcedo Studio does not support the installed models")
        case "needsNewerApplication": return qsTr("The available models need a newer version of Alcedo Studio")
        case "repairRequired": return qsTr("Repair required: installed files differ from the package")
        case "downloading": return qsTr("Downloading… %1%").arg(Math.round((row.progress || 0) * 100))
        case "verifying": return qsTr("Verifying…")
        case "installing": return qsTr("Installing…")
        case "removing": return qsTr("Removing…")
        case "error": return qsTr("Not completed")
        }
        return ""
    }

    function maskStatusColor(row) {
        switch (row.status) {
        case "current": return appTheme.accentColor
        case "updateAvailable":
        case "updateRequired":
        case "needsNewerApplication":
        case "repairRequired": return appTheme.toneGold
        case "error": return appTheme.dangerColor
        }
        return appTheme.textMutedColor
    }

    function maskActionText(action) {
        switch (action) {
        case "install": return qsTr("Download")
        case "update": return qsTr("Update")
        case "repair": return qsTr("Repair")
        case "retry": return qsTr("Retry")
        }
        return ""
    }

    // ── CLIP models ──────────────────────────────────────────────────────

    // One sentence per CLIP model family, shortened from the description of its publisher
    // (apple/ml-mobileclip README, the jinaai/jina-clip-v2 model card, the SigLIP 2 README of
    // google-research/big_vision), and the GitHub project when the publisher has one.
    function clipFamily(profileId) {
        const id = String(profileId || "")
        if (id.startsWith("mobileclip2"))
            return "mobileclip2"
        if (id.startsWith("jina-clip-v2"))
            return "jina-clip-v2"
        if (id.startsWith("siglip2"))
            return "siglip2"
        return ""
    }

    function clipDescription(profileId) {
        switch (clipFamily(profileId)) {
        case "mobileclip2":
            return qsTr("Fast image-text model from Apple, trained with multi-modal reinforced training.")
        case "jina-clip-v2":
            return qsTr("Multilingual embedding model for text and images from Jina AI, for 89 languages.")
        case "siglip2":
            return qsTr("Multilingual vision-language encoder from Google that improves on SigLIP.")
        }
        return ""
    }

    function clipProjectUrl(profileId) {
        switch (clipFamily(profileId)) {
        case "mobileclip2": return "https://github.com/apple/ml-mobileclip"
        case "siglip2": return "https://github.com/google-research/big_vision"
        }
        return ""
    }

    function clipBackendText(backend) {
        switch (backend) {
        case "onnx_runtime": return qsTr("ONNX Runtime")
        case "native_coreml": return qsTr("Core ML")
        }
        return ""
    }

    function endpointPresetIndex(preset) {
        switch (preset) {
        case "huggingface": return 1
        case "sufy": return 2
        case "custom": return 3
        }
        return 0
    }

    FolderDialog {
        id: modelFolderDialog
        title: qsTr("Choose a folder for the AI models")
        onAccepted: {
            if (page.downloadController)
                page.downloadController.SetModelDownloadDirectory(selectedFolder.toString())
        }
    }

    AiModelDownloadStatementDialog {
        id: statementDialog
        onAcceptRequested: page.acceptStatement()
        onDeclineRequested: page.declineStatement()
    }

    QtObject {
        id: endpointComboModel
        property string label: ""
        property bool enabled: !!page.downloadController && page.canChangeDownloadSettings
        readonly property var entries: [
            { value: "mirror", label: qsTr("HF Mirror") },
            { value: "huggingface", label: qsTr("Hugging Face") },
            { value: "sufy", label: qsTr("Sufy CDN") },
            { value: "custom", label: qsTr("Custom") }
        ]
        property int currentIndex: page.downloadController
                                   ? page.endpointPresetIndex(page.downloadController.modelEndpointPreset) : 0
        function selectIndex(index) {
            const entry = entries[index]
            if (entry && page.downloadController)
                page.downloadController.SetModelEndpointPreset(entry.value)
        }
    }

    // ── Model directory ──────────────────────────────────────────────────
    BentoTile {
        objectName: "aiModelsDirectoryTile"
        Layout.fillWidth: true
        Layout.topMargin: 26
        Layout.leftMargin: 34
        Layout.rightMargin: 34
        contentHeight: directoryColumn.implicitHeight

        ColumnLayout {
            id: directoryColumn
            anchors.fill: parent
            spacing: appTheme.spaceMd

            ColumnLayout {
                Layout.fillWidth: true
                spacing: appTheme.spaceXs

                Label {
                    Layout.fillWidth: true
                    text: qsTr("Model folder")
                    color: appTheme.textColor
                    font.family: appTheme.headlineFontFamily
                    font.pixelSize: appTheme.fontSizeSection
                    font.weight: Font.DemiBold
                }
                Label {
                    Layout.fillWidth: true
                    text: qsTr("The folder that holds the AI Mask models and the content recognition models. Changing it does not move the installed models.")
                    color: appTheme.textMutedColor
                    font.family: appTheme.uiFontFamily
                    font.pixelSize: appTheme.fontSizeBody
                    wrapMode: Text.Wrap
                }
            }

            FolderPathField {
                Layout.fillWidth: true
                controlHeight: 44
                pathPixelSize: appTheme.fontSizeSection
                iconSize: 20
                path: page.downloadController ? page.downloadController.modelDownloadDirectory : ""
                pathFontFamily: appTheme.headlineFontFamily
                browseEnabled: !!page.downloadController && page.canChangeDownloadSettings
                               && !page.clipDownloadRunning
                browseToolTip: qsTr("Change folder…")
                pathLabel.objectName: "aiModelsDirectoryPath"
                browseButton.objectName: "aiModelsChangeFolderButton"
                onBrowseRequested: modelFolderDialog.open()
            }

            RowLayout {
                Layout.fillWidth: true
                spacing: 12

                SystemActionButton {
                    objectName: "aiModelsOpenFolderButton"
                    Layout.fillWidth: true
                    text: Qt.platform.os === "osx" ? qsTr("Open in Finder")
                          : (Qt.platform.os === "windows" ? qsTr("Open in File Explorer")
                                                          : qsTr("Open folder"))
                    enabled: !!page.downloadController
                    onClicked: Qt.openUrlExternally(page.localFileUrl(page.downloadController.modelDownloadDirectory))
                }
                IconButton {
                    objectName: "aiModelsRefreshButton"
                    buttonSize: 44
                    buttonRadius: 10
                    iconSize: 20
                    iconSrc: "qrc:/panel_icons/retry.svg"
                    normalColor: Qt.rgba(1, 1, 1, 0.07)
                    borderColor: Qt.rgba(appTheme.textColor.r, appTheme.textColor.g, appTheme.textColor.b, 0.14)
                    tooltipText: qsTr("Read the installed models again")
                    onClicked: page.refreshInstallations()
                }
            }
        }
    }

    // ── AI Mask ──────────────────────────────────────────────────────────
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
                text: qsTr("AI Mask")
                color: appTheme.textColor
                font.family: appTheme.headlineFontFamily
                font.pixelSize: 18
                font.weight: Font.DemiBold
            }
            // Only a failed check is reported; a running check spins the refresh icon.
            AlertBadge {
                objectName: "aiModelsCheckText"
                Layout.fillWidth: true
                text: page.packagesEnabled && !page.packageService.checking
                      && page.packageService.lastError.length > 0
                      ? qsTr("The AI model check failed: %1").arg(page.packageService.lastError) : ""
            }
            Label {
                objectName: "aiModelsPackagesUnavailable"
                Layout.fillWidth: true
                visible: !page.packagesEnabled
                text: qsTr("AI Mask model downloads are not available in this build.")
                color: appTheme.textMutedColor
                font.family: appTheme.uiFontFamily
                font.pixelSize: appTheme.fontSizeCaption
                wrapMode: Text.Wrap
            }
        }

        IconButton {
            objectName: "aiModelsCheckButton"
            Layout.alignment: Qt.AlignVCenter
            visible: page.packagesEnabled
            buttonSize: 36
            buttonRadius: 10
            iconSize: 18
            iconSrc: "qrc:/panel_icons/retry.svg"
            normalColor: Qt.rgba(1, 1, 1, 0.07)
            borderColor: Qt.rgba(appTheme.textColor.r, appTheme.textColor.g, appTheme.textColor.b, 0.14)
            tooltipText: qsTr("Check again")
            spinning: !!page.packageService && page.packageService.checking
            enabled: !!page.packageService && !page.packageService.checking
            onClicked: page.packageService.checkPackages()
        }
    }

    GridLayout {
        id: maskGrid
        objectName: "aiModelsMaskList"
        Layout.fillWidth: true
        Layout.leftMargin: 34
        Layout.rightMargin: 34
        columns: width >= 600 ? 2 : 1
        columnSpacing: appTheme.spaceMd
        rowSpacing: appTheme.spaceMd

        // Instantiator over the row count, not the row list: cards persist while progress
        // changes, so a Cancel press is not lost to a rebuilt card.
        // An empty cell after a card that is alone in its row, so the AI Mask card keeps the
        // column width and the shape of the CLIP cards.
        Item {
            id: maskRowFiller
            Layout.fillWidth: true
            Layout.preferredWidth: 1
            visible: maskGrid.columns > 1 && page.maskRows.length % 2 === 1
        }

        Instantiator {
            model: page.maskRows.length
            onObjectAdded: function(index, object) {
                object.parent = maskGrid
                // Reparenting appends: the filler stays after the cards.
                maskRowFiller.parent = null
                maskRowFiller.parent = maskGrid
            }
            delegate: PackageCard {
                id: maskCard
                required property int index
                readonly property var row: page.maskRows[index] || ({})
                readonly property string packageId: row.id || ""

                namePrefix: "aiModelsMask"
                cardKey: packageId
                title: row.name || packageId
                subtitle: page.maskDescription
                upstreamUrl: page.maskProjectUrl
                upstreamToolTip: qsTr("Open the AI Mask model project on GitHub")
                pills: [page.maskRevisionText(row), page.formatBytes(row.archiveBytes)]
                licenses: page.maskLicenses(row)
                statusText: page.maskStatusText(row)
                statusColor: page.maskStatusColor(row)
                busy: !!row.busy
                progressIndeterminate: row.status !== "downloading"
                progressPercent: (row.progress || 0) * 100
                errorText: row.error || ""
                actionText: page.maskActionText(row.action || "")
                actionKind: row.action === "retry" ? "normal" : "accent"
                cancelVisible: !!row.cancelable
                removable: !!row.removable
                removeToolTip: qsTr("Remove the AI Mask models")
                confirmingRemoval: page.pendingRemovalKey === "aiMask:" + packageId
                removeQuestion: qsTr("Remove the AI Mask models?")
                removeWarning: qsTr("Without the models, the AI Mask tool cannot make AI Masks until you download them again.")
                removeConfirmEnabled: !!row.removable
                onActionRequested: page.requestDownload("aiMask", maskCard.packageId)
                onCancelRequested: page.packageService.cancelInstall(maskCard.packageId)
                onRemoveRequested: page.pendingRemovalKey = "aiMask:" + maskCard.packageId
                onRemoveKept: page.pendingRemovalKey = ""
                onRemoveConfirmed: page.confirmRemoval()
            }
        }
    }

    // ── Content Recognition (CLIP) ───────────────────────────────────────
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
                text: qsTr("Content Recognition (CLIP)")
                color: appTheme.textColor
                font.family: appTheme.headlineFontFamily
                font.pixelSize: 18
                font.weight: Font.DemiBold
            }
            Label {
                Layout.fillWidth: true
                text: page.projectOpen
                      ? qsTr("Activate a model to use it in the open project.")
                      : qsTr("Open a project to activate a model. Each project has its own active model.")
                color: appTheme.textMutedColor
                font.family: appTheme.uiFontFamily
                font.pixelSize: appTheme.fontSizeCaption
                wrapMode: Text.Wrap
            }
        }

        Label {
            Layout.alignment: Qt.AlignVCenter
            text: qsTr("Source")
            color: appTheme.textMutedColor
            font.family: appTheme.uiFontFamily
            font.pixelSize: appTheme.fontSizeBody
        }
        AdjustmentCombo {
            objectName: "aiModelsEndpointControl"
            controlObjectName: "aiModelsEndpointCombo"
            Layout.alignment: Qt.AlignVCenter
            Layout.preferredWidth: 190
            Layout.fillWidth: false
            controlHeight: 36
            showResetButton: false
            model: endpointComboModel
        }
    }

    TextField {
        id: customEndpointField
        objectName: "aiModelsCustomEndpointField"
        Layout.fillWidth: true
        Layout.leftMargin: 34
        Layout.rightMargin: 34
        Layout.preferredHeight: 40
        visible: !!page.downloadController && page.downloadController.modelEndpointPreset === "custom"
        enabled: !!page.downloadController && page.canChangeDownloadSettings
        text: page.downloadController ? page.downloadController.customModelEndpoint : ""
        placeholderText: qsTr("https://example.com")
        color: appTheme.textColor
        placeholderTextColor: appTheme.textMutedColor
        font.family: appTheme.headlineFontFamily
        font.pixelSize: appTheme.fontSizeBody
        leftPadding: appTheme.spaceMd
        rightPadding: appTheme.spaceMd
        background: Rectangle {
            radius: 10
            color: Qt.rgba(1, 1, 1, 0.10)
            border.width: 1
            border.color: customEndpointField.activeFocus
                          ? appTheme.accentColor
                          : Qt.rgba(appTheme.textColor.r, appTheme.textColor.g, appTheme.textColor.b, 0.12)
        }
        onEditingFinished: {
            if (page.downloadController)
                page.downloadController.SetCustomModelEndpoint(text)
        }
    }

    GridLayout {
        id: clipGrid
        objectName: "aiModelsClipList"
        Layout.fillWidth: true
        Layout.leftMargin: 34
        Layout.rightMargin: 34
        Layout.bottomMargin: 26
        columns: width >= 600 ? 2 : 1
        columnSpacing: appTheme.spaceMd
        rowSpacing: appTheme.spaceMd

        Instantiator {
            model: page.clipRows.length
            onObjectAdded: function(index, object) {
                object.parent = clipGrid
            }
            delegate: PackageCard {
                id: clipCard
                required property int index
                readonly property var row: page.clipRows[index] || ({})
                readonly property string profileId: row.profileId || ""
                readonly property bool installed: !!row.installed
                readonly property bool downloading: !!row.busy
                readonly property bool activating: profileId.length > 0
                                                   && page.activatingModelProfileId === profileId
                readonly property bool isActive: page.projectOpen && installed
                                                 && page.activeModelProfileId.length > 0
                                                 && (page.activeModelProfileId === profileId
                                                     || page.activeModelProfileId === row.modelId)
                readonly property bool needsProject: installed && !isActive && !page.projectOpen

                namePrefix: "aiModelsClip"
                cardKey: profileId
                title: row.displayName || profileId
                subtitle: page.clipDescription(profileId)
                upstreamUrl: page.clipProjectUrl(profileId)
                upstreamToolTip: qsTr("Open the model project on GitHub")
                pills: [row.sizeLabel || "", page.clipBackendText(row.inferenceBackend || "")]
                licenses: (row.licenseName || "").length > 0
                          ? [{ name: row.licenseName, url: row.licenseUrl || "" }] : []
                active: isActive
                busy: downloading || activating
                statusText: {
                    if (activating)
                        return qsTr("Activating…")
                    if (row.status === "canceling")
                        return qsTr("Canceling…")
                    if (downloading)
                        return qsTr("Downloading… %1%").arg(Math.round(Number(row.progress || 0)))
                    if (isActive)
                        return qsTr("Active in this project")
                    if (installed)
                        return qsTr("Installed")
                    return qsTr("Not installed")
                }
                statusColor: isActive ? appTheme.activeModelBorderColor
                                      : (installed ? appTheme.accentColor : appTheme.textMutedColor)
                progressPercent: Number(row.progress || 0)
                progressIndeterminate: activating || Number(row.progress || 0) <= 0
                progressLeadingText: activating ? qsTr("Preparing the model…") : (row.bytesLabel || "")
                progressSpeedText: downloading ? (row.speedLabel || "") : ""
                progressEtaText: downloading ? (row.etaLabel || "") : ""
                errorText: row.error || ""
                messageText: needsProject ? qsTr("Open a project to activate this model.")
                                          : (row.message || "")
                actionText: {
                    if (downloading || activating || isActive)
                        return ""
                    return installed ? qsTr("Activate") : qsTr("Download")
                }
                actionEnabled: installed
                               ? (page.projectOpen && !page.modelActivationRunning && page.canChangeSemanticModel)
                               : (!page.clipDownloadRunning && page.canChangeDownloadSettings)
                cancelVisible: downloading && row.status !== "canceling"
                removable: installed && !activating
                removeEnabled: page.canChangeDownloadSettings
                removeToolTip: qsTr("Remove model")
                confirmingRemoval: page.pendingRemovalKey === "clip:" + profileId
                removeQuestion: qsTr("Remove this model?")
                removeWarning: isActive
                               ? qsTr("It is the active model of this project. After removal, the project has no active model.")
                               : ""
                removeConfirmEnabled: installed && !downloading && page.canChangeDownloadSettings
                onActionRequested: {
                    if (clipCard.installed)
                        page.semanticController.activateModel(clipCard.profileId)
                    else
                        page.requestDownload("clip", clipCard.profileId)
                }
                onCancelRequested: page.downloadController.cancelInstall(clipCard.profileId)
                onRemoveRequested: page.pendingRemovalKey = "clip:" + clipCard.profileId
                onRemoveKept: page.pendingRemovalKey = ""
                onRemoveConfirmed: page.confirmRemoval()
            }
        }
    }

    Item {
        Layout.fillHeight: true
    }
}
