pragma ComponentBehavior: Bound
// Copyright 2026 Yurun Zi
// SPDX-License-Identifier: GPL-3.0-only
import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import QtQuick.Dialogs

// One application-owned review. Hiding never changes selection or approves removal.
Dialog {
    id: root
    objectName: "projectMaintenanceDialog"
    property var controller
    property string backupDirectory: ""
    readonly property var findings: controller ? controller.model : null
    readonly property bool applying: controller && controller.busy && !controller.scanning
                                     && controller.state !== "recovery"
    property bool confirming: false
    parent: Overlay.overlay
    modal: true
    focus: true
    padding: 0
    width: Math.min(parent ? parent.width - appTheme.spaceXl * 2 : preferredWidth, preferredWidth)
    readonly property real preferredWidth: applying ? appTheme.editorSidePanelWidth
                                                    : appTheme.editorSidePanelWidth * 3
    height: Math.min(parent ? parent.height - appTheme.spaceXl * 2 : preferredHeight, preferredHeight)
    readonly property real preferredHeight: applying
                                           ? header.implicitHeight + operationProgress.implicitHeight
                                             + appTheme.spaceLg * 3 + 1
                                           : appTheme.editorSidePanelWidthMax * 2
    x: parent ? Math.round((parent.width - width) / 2) : 0
    y: parent ? Math.round((parent.height - height) / 2) : 0
    closePolicy: Popup.CloseOnEscape
    onClosed: confirming = false
    onApplyingChanged: if (applying) open()
    background: Rectangle {
        color: root.applying ? appTheme.bgDeepColor : appTheme.cardSurfaceColor
        radius: appTheme.panelRadius
        border.width: root.applying ? 0 : 1
        border.color: appTheme.cardBorderColor
        RotatingTaskBorder {
            anchors.fill: parent
            radius: parent.radius
            visible: root.applying
            running: root.visible && root.applying
            color: appTheme.backgroundTaskWorkingColor
        }
    }
    component Caption: Label {
        color: appTheme.textMutedColor
        font.family: appTheme.uiFontFamily
        font.pixelSize: appTheme.fontSizeCaption
        wrapMode: Text.Wrap
    }
    component Action: DialogActionButton {
        buttonWidth: implicitContentWidth + appTheme.spaceLg * 2
    }
    FolderDialog {
        id: backupFolder
        title: qsTr("Choose maintenance backup folder")
        onAccepted: root.backupDirectory = selectedFolder.toString()
    }
    contentItem: ColumnLayout {
        spacing: 0
        RowLayout {
            id: header
            Layout.fillWidth: true
            Layout.leftMargin: appTheme.spaceLg
            Layout.rightMargin: appTheme.spaceLg
            Layout.topMargin: appTheme.spaceSm
            Layout.bottomMargin: appTheme.spaceSm
            spacing: appTheme.spaceSm
            Label {
                text: qsTr("Project Maintenance")
                font.family: appTheme.uiFontFamily
                font.pixelSize: appTheme.fontSizeSection
                font.weight: appTheme.fontWeightHeading
                color: appTheme.textColor
                Layout.fillWidth: true
                Layout.alignment: Qt.AlignVCenter
            }
            IconActionButton {
                objectName: "projectMaintenanceCloseButton"
                Layout.alignment: Qt.AlignVCenter
                compact: true
                showFocusRing: false
                fillIdle: "transparent"
                iconSrc: "qrc:/panel_icons/close.svg"
                iconColorDefault: appTheme.iconColor
                iconColorMuted: appTheme.textMutedColor
                actionName: qsTr("Close Project Maintenance")
                onClicked: root.close()
            }
        }
        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: 1
            visible: !root.applying
            color: appTheme.dividerColor
        }
        ColumnLayout {
            visible: !root.applying
            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.margins: appTheme.spaceLg
            spacing: appTheme.spaceMd
            Action {
                objectName: "maintenanceScanButton"
                Layout.alignment: Qt.AlignLeft
                text: root.controller && root.controller.scanning ? qsTr("Cancel scan") : qsTr("Scan project")
                enabled: root.controller && root.controller.available && (!root.controller.busy || root.controller.scanning)
                onClicked: {
                    root.confirming = false
                    if (root.controller.scanning) root.controller.CancelScan()
                    else root.controller.Scan(markChangedSources.checked)
                }
            }
            ThemeCheckBox {
                id: markChangedSources
                objectName: "maintenanceMarkChangedSources"
                Layout.fillWidth: true
                text: qsTr("Save source-change icons on Library thumbnails")
                checked: true
                enabled: root.controller && root.controller.available && !root.controller.busy
                         && !root.confirming
                Accessible.description: qsTr("The review lists detected changes either way. This option saves or clears thumbnail icons when you scan.")
                ToolTip.text: markChangedSources.Accessible.description
                ToolTip.visible: enabled && markHover.hovered
                onToggled: function(checked) { markChangedSources.checked = checked }
                HoverHandler { id: markHover }
            }
            Caption {
                Layout.fillWidth: true
                text: root.controller ? root.controller.projectPath : ""
                wrapMode: Text.WrapAnywhere
            }
            Label {
                Layout.fillWidth: true
                visible: text.length > 0
                text: root.controller ? root.controller.message : ""
                color: appTheme.textMutedColor
                font.family: appTheme.uiFontFamily
                font.pixelSize: appTheme.fontSizeBody
                wrapMode: Text.Wrap
                Accessible.role: Accessible.StaticText
                Accessible.name: text
            }
            AlertBadge { Layout.fillWidth: true; text: root.controller ? root.controller.error : "" }
            RowLayout {
                Layout.fillWidth: true
                spacing: appTheme.spaceMd
                enabled: root.controller && !root.controller.busy && !root.confirming
                AdjustmentCombo {
                    Layout.fillWidth: false
                    Layout.preferredWidth: Math.min(root.availableWidth / 2, appTheme.editorSidePanelWidth)
                    showResetButton: false
                    controlHeight: appTheme.iconButtonHitSizeCompact
                    model: QtObject {
                        readonly property string label: qsTr("Finding filter")
                        readonly property bool enabled: root.controller && !root.controller.busy
                        readonly property int currentIndex: root.findings ? root.findings.filter + 1 : 0
                        readonly property var entries: [
                            {label: qsTr("All findings")}, {label: qsTr("Duplicates")},
                            {label: qsTr("Missing sources")}, {label: qsTr("Source problems")},
                            {label: qsTr("Project problems")}, {label: qsTr("Changed sources")},
                            {label: qsTr("Unverified source history")}
                        ]
                        function selectIndex(index) {
                            if (root.findings) root.findings.filter = index - 1
                        }
                    }
                }
                TextField {
                    Layout.fillWidth: true
                    Layout.preferredHeight: appTheme.iconButtonHitSizeCompact
                    placeholderText: qsTr("Search full path or entry ID")
                    Accessible.name: placeholderText
                    onTextChanged: if (root.findings) root.findings.search = text
                    color: appTheme.textColor
                    placeholderTextColor: appTheme.textMutedColor
                    selectionColor: appTheme.textMutedColor
                    selectedTextColor: appTheme.textColor
                    font.family: appTheme.uiFontFamily
                    font.pixelSize: appTheme.fontSizeBody
                    leftPadding: appTheme.spaceSm
                    rightPadding: appTheme.spaceSm
                    background: Rectangle {
                        color: appTheme.bgBaseColor
                        border.width: 1
                        border.color: appTheme.cardBorderColor
                        radius: appTheme.controlRadiusSmall
                    }
                }
            }
            ListView {
                id: list
                objectName: "maintenanceFindings"
                Layout.fillWidth: true
                Layout.fillHeight: true
                clip: true
                spacing: appTheme.spaceSm
                model: root.findings
                ScrollBar.vertical: ScrollBar {}
                delegate: Rectangle {
                    id: row
                    required property double entryId
                    required property string sourcePath
                    required property int rating
                    required property string editingWork
                    required property int findingKind
                    required property string reason
                    required property var retainedId
                    required property bool removable
                    required property bool selected
                    width: list.width
                    implicitHeight: fields.implicitHeight + appTheme.spaceMd * 2
                    color: appTheme.bgBaseColor
                    radius: appTheme.controlRadiusSmall
                    border.width: 1
                    border.color: selected ? appTheme.textColor : appTheme.cardBorderColor
                    RowLayout {
                        id: fields
                        anchors.fill: parent
                        anchors.margins: appTheme.spaceMd
                        spacing: appTheme.spaceMd
                        ThemeCheckBox {
                            Layout.fillWidth: false
                            checked: row.selected
                            enabled: row.removable && !root.confirming
                            accessibleText: qsTr("Select entry %1").arg(row.entryId)
                            onToggled: function(checked) { root.findings.SetSelected(row.entryId, checked) }
                        }
                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: appTheme.spaceSm
                            Label {
                                Layout.fillWidth: true
                                text: row.sourcePath
                                wrapMode: Text.WrapAnywhere
                                color: appTheme.textColor
                                font.family: appTheme.uiFontFamily
                                font.pixelSize: appTheme.fontSizeBody
                            }
                            Flow {
                                Layout.fillWidth: true
                                spacing: appTheme.spaceMd
                                Caption { text: qsTr("Entry ID: %1").arg(row.entryId) }
                                Caption { text: qsTr("Rating: %1").arg(row.rating) }
                                Caption { text: row.editingWork }
                                Caption {
                                    visible: row.retainedId !== undefined && row.retainedId !== null
                                    text: qsTr("Retained entry: %1").arg(row.retainedId || "")
                                }
                            }
                            Caption {
                                Layout.fillWidth: true
                                visible: row.findingKind === 0
                                text: row.reason
                            }
                            AlertBadge { Layout.fillWidth: true; text: row.findingKind !== 0 ? row.reason : "" }
                        }
                    }
                }
            }
            RowLayout {
                Layout.fillWidth: true
                spacing: appTheme.spaceSm
                enabled: root.controller && !root.controller.busy && !root.confirming
                Action { text: qsTr("Previous"); enabled: root.findings && root.findings.page > 0; onClicked: root.findings.page-- }
                Caption { text: root.findings ? qsTr("Page %1").arg(root.findings.page + 1) : "" }
                Action { text: qsTr("Next"); enabled: root.findings && (root.findings.page + 1) * 100 < root.findings.matchingCount; onClicked: root.findings.page++ }
                Item { Layout.fillWidth: true }
                Caption { text: root.findings ? qsTr("%1 findings").arg(root.findings.matchingCount) : "" }
            }
            Rectangle { Layout.fillWidth: true; Layout.preferredHeight: 1; color: appTheme.dividerColor }
            RowLayout {
                Layout.fillWidth: true
                spacing: appTheme.spaceSm
                enabled: root.controller && !root.controller.busy && !root.confirming
                Action { Layout.fillWidth: true; Layout.preferredWidth: 0; text: qsTr("Select removable duplicates"); onClicked: root.findings.SelectKind(0) }
                Action { Layout.fillWidth: true; Layout.preferredWidth: 0; text: qsTr("Select removable missing entries"); onClicked: root.findings.SelectKind(1) }
                Action { Layout.fillWidth: true; Layout.preferredWidth: 0; text: qsTr("Clear selection"); onClicked: root.findings.ClearSelection() }
            }
            Label {
                Layout.fillWidth: true
                text: root.findings ? qsTr("Selected: %1 entries (including hidden rows)").arg(root.findings.selectedCount) : ""
                color: appTheme.textColor
                font.family: appTheme.uiFontFamily
                font.pixelSize: appTheme.fontSizeBody
                wrapMode: Text.Wrap
                Accessible.name: text
            }
            Caption {
                Layout.fillWidth: true
                text: root.controller && root.controller.backupPath.length > 0
                      ? (root.controller.backupVerified ? qsTr("Verified backup: %1") : qsTr("Backup path: %1")).arg(root.controller.backupPath)
                      : root.backupDirectory.length > 0 ? qsTr("Backup folder: %1").arg(root.backupDirectory)
                                                      : qsTr("Backup folder: project folder")
                wrapMode: Text.WrapAnywhere
            }
            Label {
                Layout.fillWidth: true
                visible: root.confirming
                text: root.findings ? qsTr("Remove %1 duplicate entries and %2 missing entries? Original image files stay untouched. Rated and edited entries are protected. A verified backup is required.").arg(root.findings.selectedDuplicates).arg(root.findings.selectedMissing) : ""
                wrapMode: Text.Wrap
                color: appTheme.textColor
                font.family: appTheme.uiFontFamily
                font.pixelSize: appTheme.fontSizeBody
            }
            RowLayout {
                Layout.fillWidth: true
                spacing: appTheme.spaceSm
                Action { text: qsTr("Choose backup folder"); enabled: root.controller && !root.controller.busy; onClicked: backupFolder.open() }
                Item { Layout.fillWidth: true }
                Action { text: qsTr("Cancel"); visible: root.confirming; onClicked: root.confirming = false }
                Action {
                    objectName: "maintenanceRemoveButton"
                    text: root.confirming ? qsTr("Confirm removal") : qsTr("Remove selected")
                    kind: root.confirming ? "danger" : "normal"
                    enabled: root.findings && root.findings.selectedCount > 0 && root.controller
                             && !root.controller.busy && appModules.interactionPolicy.canChangeStateAsUser
                    ToolTip.text: appModules.interactionPolicy.changeStateAsUserReason
                    ToolTip.visible: hovered && ToolTip.text.length > 0
                    onClicked: {
                        if (!root.confirming) root.confirming = true
                        else { root.controller.ApplySelected(root.backupDirectory); root.confirming = false }
                    }
                }
            }
        }
        ColumnLayout {
            id: operationProgress
            visible: root.applying
            Layout.fillWidth: true
            Layout.margins: appTheme.spaceLg
            spacing: appTheme.spaceLg
            Label {
                Layout.fillWidth: true
                text: root.controller ? root.controller.message : ""
                horizontalAlignment: Text.AlignHCenter
                wrapMode: Text.Wrap
                color: appTheme.textColor
                font.family: appTheme.uiFontFamily
                font.pixelSize: appTheme.fontSizeBody
                Accessible.name: text
            }
            ImportProgressRing {
                objectName: "maintenanceRemovalRing"
                Layout.alignment: Qt.AlignHCenter
                Layout.preferredWidth: appTheme.editorScopeHeightMin
                Layout.preferredHeight: appTheme.editorScopeHeightMin
                ringWidth: appTheme.spaceMd
                fillColor: appTheme.backgroundTaskWorkingColor
                trackColor: appTheme.hoverColor
                running: root.visible && root.applying && !appTheme.reduceMotion
                indeterminate: !root.controller || !root.controller.progressDeterminate
                progress: root.controller && root.controller.totalEntries > 0
                          ? root.controller.processedEntries / root.controller.totalEntries : 0
                Accessible.role: Accessible.ProgressBar
                Accessible.name: qsTr("Project Maintenance progress")
            }
            Label {
                objectName: "maintenanceProcessedCount"
                Layout.fillWidth: true
                text: root.controller ? qsTr("%1 / %2").arg(root.controller.processedEntries).arg(root.controller.totalEntries) : ""
                horizontalAlignment: Text.AlignHCenter
                color: appTheme.textColor
                font.family: appTheme.headlineFontFamily
                font.pixelSize: appTheme.fontSizeHeadline
                font.weight: appTheme.fontWeightStrong
            }
            Caption {
                Layout.fillWidth: true
                text: qsTr("Entries processed")
                horizontalAlignment: Text.AlignHCenter
            }
        }
    }
}
