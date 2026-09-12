pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Dialogs
import QtQuick.Layouts

// Settings > Cache — project Mask (Mix) cache. The open project owns
// disposable R8 coverage files under a dedicated namespace; this panel is
// separate from the thumbnail disk cache. Pending fields apply through
// ProjectModule (ProjectService facade); nothing touches storage directly.
ColumnLayout {
    id: panel
    objectName: "projectMaskCacheSettingsPanel"

    property var projectModule: null
    property bool projectReady: false
    property color textColor: appTheme.textColor
    property color mutedTextColor: appTheme.textMutedColor
    property color dividerColor: appTheme.dividerColor
    property color dangerColor: appTheme.dangerColor
    property string dataFontFamily: appTheme.dataFontFamily

    property string pendingRoot: ""
    property string pendingRetention: "keep"

    signal messageRequested(string message)

    readonly property var cacheState: projectModule ? projectModule.maskCacheState : ({})
    readonly property bool hasProject: panel.projectReady
                                       && panel.cacheState.available === true
    readonly property bool rootPending: panel.pendingRoot
                                        !== String(panel.cacheState.chosenRoot || "")
    readonly property string applyErrorText: String(panel.cacheState.applyError || "")
    readonly property string lastErrorText: String(panel.cacheState.lastError || "")

    width: parent ? parent.width : implicitWidth
    spacing: 20

    function reloadPending() {
        if (panel.projectModule)
            panel.projectModule.RefreshMaskCacheState()
        if (!panel.hasProject)
            return
        panel.pendingRoot = String(panel.cacheState.chosenRoot || "")
        panel.pendingRetention = String(panel.cacheState.retention || "keep")
    }

    function applyPending() {
        if (!panel.hasProject || !panel.projectModule)
            return
        if (panel.rootPending) {
            if (!panel.projectModule.ApplyMaskCacheRoot(panel.pendingRoot)) {
                panel.messageRequested(
                            String(panel.projectModule.maskCacheState.applyError
                                   || qsTr("Mask cache folder was not changed")))
                return
            }
            panel.messageRequested(qsTr("Mask cache folder updated"))
        }
        if (panel.pendingRetention !== String(panel.cacheState.retention || "keep")) {
            if (!panel.projectModule.ApplyMaskCacheRetention(panel.pendingRetention)) {
                panel.messageRequested(
                            String(panel.projectModule.maskCacheState.applyError
                                   || qsTr("Mask cache retention was not changed")))
                return
            }
        }
        panel.projectModule.RefreshMaskCacheState()
    }

    function formatBytes(count) {
        const value = Number(count || 0)
        if (value < 1024)
            return qsTr("%1 B").arg(value)
        if (value < 1024 * 1024)
            return qsTr("%1 KB").arg((value / 1024).toFixed(1))
        if (value < 1024 * 1024 * 1024)
            return qsTr("%1 MB").arg((value / (1024 * 1024)).toFixed(1))
        return qsTr("%1 GB").arg((value / (1024 * 1024 * 1024)).toFixed(1))
    }

    function rootDisplayText() {
        if (panel.pendingRoot.length > 0)
            return panel.pendingRoot
        return qsTr("Project metadata folder")
    }

    FolderDialog {
        id: maskCacheFolderDialog
        title: qsTr("Select Mask Cache Folder")
        onAccepted: panel.pendingRoot = selectedFolder.toString()
    }

    QtObject {
        id: retentionModel
        property string label: ""
        property bool enabled: panel.hasProject
        readonly property var entries: [
            { "label": qsTr("Keep cache files"), "value": "keep" },
            { "label": qsTr("Delete on project close"), "value": "deleteOnProjectClose" }
        ]
        property int currentIndex: panel.pendingRetention === "deleteOnProjectClose" ? 1 : 0
        function selectIndex(index) {
            const item = entries[index]
            if (item)
                panel.pendingRetention = item.value
        }
    }

    SettingsSection {
        Layout.fillWidth: true
        Layout.leftMargin: 34
        Layout.rightMargin: 34
        title: qsTr("Project Mask cache")
        textColor: panel.textColor
        mutedTextColor: panel.mutedTextColor
        dividerColor: panel.dividerColor

        Label {
            Layout.fillWidth: true
            text: qsTr("Disposable coverage cache for Mask results on this project's photos. "
                       + "Brush strokes, Mask parameters, history, and RAW files are stored "
                       + "with the project and are never kept here.")
            color: panel.mutedTextColor
            font.pixelSize: appTheme.fontSizeBody
            wrapMode: Text.WordWrap
        }

        GridLayout {
            Layout.fillWidth: true
            columns: 3
            columnSpacing: 12
            rowSpacing: 12

            CacheMetric {
                Layout.fillWidth: true
                label: qsTr("Project")
                value: panel.hasProject ? String(panel.cacheState.projectName || "")
                                        : qsTr("No project")
            }

            CacheMetric {
                Layout.fillWidth: true
                label: qsTr("Files")
                value: panel.hasProject ? String(panel.cacheState.fileCount || 0) : "0"
            }

            CacheMetric {
                Layout.fillWidth: true
                label: qsTr("Size")
                value: panel.hasProject ? panel.formatBytes(panel.cacheState.byteCount) : "0 B"
            }
        }

        Label {
            Layout.fillWidth: true
            visible: panel.hasProject
            text: qsTr("Project UUID: %1").arg(String(panel.cacheState.projectUuid || ""))
            color: panel.mutedTextColor
            font.family: panel.dataFontFamily
            font.pixelSize: appTheme.fontSizeCaption
            elide: Text.ElideMiddle
        }

        Label {
            id: maskCacheStatusLabel
            objectName: "maskCacheStatusLabel"
            Layout.fillWidth: true
            visible: panel.hasProject
                     && (panel.cacheState.dirty === true
                         || Number(panel.cacheState.pendingWrites || 0) > 0
                         || panel.lastErrorText.length > 0
                         || panel.applyErrorText.length > 0)
            text: {
                if (panel.applyErrorText.length > 0)
                    return panel.applyErrorText
                if (panel.lastErrorText.length > 0)
                    return panel.lastErrorText
                return qsTr("%1 pending write%2").arg(Number(panel.cacheState.pendingWrites || 0))
                        .arg(Number(panel.cacheState.pendingWrites || 0) === 1 ? "" : "s")
            }
            color: (panel.applyErrorText.length > 0 || panel.lastErrorText.length > 0)
                   ? panel.dangerColor : panel.mutedTextColor
            font.pixelSize: appTheme.fontSizeBody
            wrapMode: Text.WordWrap
        }
    }

    SettingsSection {
        Layout.fillWidth: true
        Layout.leftMargin: 34
        Layout.rightMargin: 34
        title: qsTr("Storage")
        textColor: panel.textColor
        mutedTextColor: panel.mutedTextColor
        dividerColor: panel.dividerColor

        RowLayout {
            Layout.fillWidth: true
            spacing: 16

            ColumnLayout {
                Layout.fillWidth: true
                spacing: 8

                Label {
                    text: qsTr("Cache folder")
                    color: panel.textColor
                    font.pixelSize: 15
                    font.weight: 600
                }

                Rectangle {
                    Layout.fillWidth: true
                    Layout.preferredHeight: 36
                    radius: appTheme.controlRadiusSmall
                    color: appTheme.bgBaseColor
                    border.width: 1
                    border.color: appTheme.cardBorderColor

                    Label {
                        anchors.fill: parent
                        anchors.leftMargin: appTheme.spaceSm
                        anchors.rightMargin: appTheme.spaceSm
                        text: panel.hasProject ? panel.rootDisplayText()
                                               : qsTr("No project")
                        elide: Text.ElideMiddle
                        verticalAlignment: Text.AlignVCenter
                        color: panel.textColor
                        font.family: panel.dataFontFamily
                        font.pixelSize: appTheme.fontSizeBody
                    }
                }

                Label {
                    Layout.fillWidth: true
                    visible: panel.hasProject
                             && String(panel.cacheState.effectiveRoot || "").length > 0
                    text: String(panel.cacheState.effectiveRoot || "")
                    elide: Text.ElideMiddle
                    color: panel.mutedTextColor
                    font.family: panel.dataFontFamily
                    font.pixelSize: appTheme.fontSizeCaption
                }
            }

            IconButton {
                objectName: "maskCacheRootChooseButton"
                buttonWidth: 36
                buttonHeight: 36
                buttonRadius: appTheme.controlRadiusSmall
                iconSize: appTheme.iconOpticalSizeCompact
                kind: "normal"
                bordered: true
                borderColor: appTheme.cardBorderColor
                iconSrc: "qrc:/panel_icons/folder-open.svg"
                tooltipText: qsTr("Select Mask Cache Folder")
                enabled: panel.hasProject
                Layout.alignment: Qt.AlignBottom
                onClicked: maskCacheFolderDialog.open()
            }

            IconButton {
                objectName: "maskCacheRootDefaultButton"
                buttonWidth: 36
                buttonHeight: 36
                buttonRadius: appTheme.controlRadiusSmall
                iconSize: appTheme.iconOpticalSizeCompact
                kind: "normal"
                bordered: true
                borderColor: appTheme.cardBorderColor
                iconSrc: "qrc:/panel_icons/reset.svg"
                tooltipText: qsTr("Use the project metadata folder")
                enabled: panel.hasProject && panel.pendingRoot.length > 0
                Layout.alignment: Qt.AlignBottom
                onClicked: panel.pendingRoot = ""
            }
        }

        Label {
            id: maskCacheRootWarning
            objectName: "maskCacheRootWarning"
            Layout.fillWidth: true
            visible: panel.hasProject && panel.rootPending
            text: qsTr("Applying moves new cache writes to this folder and deletes this "
                       + "project's cache files under the previous folder.")
            color: panel.dangerColor
            font.pixelSize: appTheme.fontSizeBody
            wrapMode: Text.WordWrap
        }
    }

    SettingsSection {
        Layout.fillWidth: true
        Layout.leftMargin: 34
        Layout.rightMargin: 34
        title: qsTr("Retention")
        textColor: panel.textColor
        mutedTextColor: panel.mutedTextColor
        dividerColor: panel.dividerColor

        RowLayout {
            Layout.fillWidth: true
            spacing: 16

            Label {
                Layout.preferredWidth: 180
                text: qsTr("On project close")
                color: panel.textColor
                font.pixelSize: 15
                font.weight: 600
            }

            AdjustmentCombo {
                objectName: "maskCacheRetentionControl"
                controlObjectName: "maskCacheRetentionCombo"
                Layout.fillWidth: true
                controlHeight: 36
                showResetButton: false
                model: retentionModel
            }
        }
    }

    SettingsSection {
        Layout.fillWidth: true
        Layout.leftMargin: 34
        Layout.rightMargin: 34
        title: qsTr("Maintenance")
        textColor: panel.textColor
        mutedTextColor: panel.mutedTextColor
        dividerColor: panel.dividerColor

        Label {
            id: maskCacheClearHint
            objectName: "maskCacheClearHint"
            Layout.fillWidth: true
            text: qsTr("Deletes only this project's disposable Mask coverage files. "
                       + "Brush strokes, Mask parameters, history, and RAW files are kept; "
                       + "the cache is rebuilt on next open.")
            color: panel.mutedTextColor
            font.pixelSize: appTheme.fontSizeBody
            wrapMode: Text.WordWrap
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: 12

            DialogActionButton {
                objectName: "maskCacheClearButton"
                kind: "normal"
                Layout.fillWidth: true
                buttonWidth: 188
                buttonHeight: 42
                text: qsTr("Clear Mask cache")
                enabled: panel.hasProject
                onClicked: {
                    if (panel.projectModule
                            && panel.projectModule.ClearMaskCache()) {
                        panel.messageRequested(qsTr("Mask cache cleared"))
                    } else if (panel.projectModule) {
                        panel.messageRequested(
                                    String(panel.projectModule.maskCacheState.applyError
                                           || qsTr("Mask cache was not cleared")))
                    }
                }
            }

            DialogActionButton {
                kind: "normal"
                Layout.fillWidth: true
                buttonWidth: 112
                buttonHeight: 42
                text: qsTr("Refresh")
                onClicked: {
                    if (panel.projectModule)
                        panel.projectModule.RefreshMaskCacheState()
                }
            }
        }
    }

    component SettingsSection: ColumnLayout {
        property string title: ""
        property color textColor: appTheme.textColor
        property color mutedTextColor: appTheme.textMutedColor
        property color dividerColor: appTheme.dividerColor

        spacing: 14

        Label {
            Layout.fillWidth: true
            text: title
            color: textColor
            font.pixelSize: 18
            font.weight: 800
        }

        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: 1
            color: dividerColor
        }
    }

    component CacheMetric: Rectangle {
        property string label: ""
        property string value: ""

        implicitHeight: 84
        radius: 8
        color: Qt.rgba(appTheme.bgBaseColor.r, appTheme.bgBaseColor.g,
                       appTheme.bgBaseColor.b, 0.62)

        ColumnLayout {
            anchors.fill: parent
            anchors.margins: 14
            spacing: 6

            Label {
                Layout.fillWidth: true
                text: label
                color: panel.mutedTextColor
                font.pixelSize: 12
                font.weight: 700
                elide: Text.ElideRight
            }

            Label {
                Layout.fillWidth: true
                text: value
                color: panel.textColor
                font.family: panel.dataFontFamily
                font.pixelSize: 18
                font.weight: 700
                elide: Text.ElideRight
            }
        }
    }
}
