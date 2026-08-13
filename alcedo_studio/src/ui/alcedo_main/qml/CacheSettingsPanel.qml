pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Dialogs
import QtQuick.Layouts

// Settings > Cache. Owns thumbnail-disk-cache pending state, stats, and
// maintenance so SettingDialog stays a section host.
ColumnLayout {
    id: panel
    objectName: "cacheSettingsPanel"

    property var libraryModule: null
    property bool projectReady: false
    property color textColor: appTheme.textColor
    property color mutedTextColor: appTheme.textMutedColor
    property color canvasColor: appTheme.bgBaseColor
    property color dividerColor: appTheme.dividerColor
    property color dangerColor: appTheme.dangerColor
    property string dataFontFamily: appTheme.dataFontFamily

    property bool pendingEnabled: true
    property string pendingRoot: ""
    property int pendingMaxEntries: 10000
    property int pendingJpegQuality: 85
    property string statsSnapshot: ""

    signal messageRequested(string message)

    readonly property bool hasLibrary: libraryModule !== null

    width: parent ? parent.width : implicitWidth
    spacing: 20

    function reloadPending() {
        if (!panel.hasLibrary)
            return
        panel.pendingEnabled = panel.libraryModule.thumbnailDiskCacheEnabled
        panel.pendingRoot = panel.libraryModule.thumbnailDiskCacheRoot
        panel.pendingMaxEntries = panel.libraryModule.thumbnailDiskCacheMaxEntries
        panel.pendingJpegQuality = panel.libraryModule.thumbnailDiskCacheJpegQuality
        cacheEnabledSwitch.checked = panel.pendingEnabled
        maxEntriesSpin.value = panel.pendingMaxEntries
        jpegQualitySpin.value = panel.pendingJpegQuality
        panel.refreshStats()
    }

    function applyPending() {
        if (!panel.hasLibrary)
            return
        if (panel.libraryModule.thumbnailDiskCacheEnabled !== panel.pendingEnabled)
            panel.libraryModule.SetThumbnailDiskCacheEnabled(panel.pendingEnabled)
        if (panel.libraryModule.thumbnailDiskCacheRoot !== panel.pendingRoot)
            panel.libraryModule.SetThumbnailDiskCacheRoot(panel.pendingRoot)
        if (panel.libraryModule.thumbnailDiskCacheMaxEntries !== panel.pendingMaxEntries)
            panel.libraryModule.SetThumbnailDiskCacheMaxEntries(panel.pendingMaxEntries)
        if (panel.libraryModule.thumbnailDiskCacheJpegQuality !== panel.pendingJpegQuality)
            panel.libraryModule.SetThumbnailDiskCacheJpegQuality(panel.pendingJpegQuality)
        panel.refreshStats()
    }

    function refreshStats() {
        panel.statsSnapshot = panel.hasLibrary
                ? panel.libraryModule.thumbnailDiskCacheStats : ""
    }

    function statLineValue(label) {
        const lines = panel.statsSnapshot.split("\n")
        for (let i = 0; i < lines.length; ++i) {
            const line = lines[i]
            if (line.indexOf(label + ": ") === 0)
                return line.substring(label.length + 2)
        }
        return qsTr("Unavailable")
    }

    function hitsMissesValue() {
        const value = panel.statLineValue("Hits")
        const separator = " / Misses: "
        const separatorIndex = value.indexOf(separator)
        if (separatorIndex < 0)
            return value
        return qsTr("%1 / %2").arg(value.substring(0, separatorIndex))
                              .arg(value.substring(separatorIndex + separator.length))
    }

    FolderDialog {
        id: cacheFolderDialog
        title: qsTr("Select Thumbnail Cache Folder")
        onAccepted: panel.pendingRoot = selectedFolder.toString()
    }

    SettingsSection {
        Layout.fillWidth: true
        Layout.topMargin: 26
        Layout.leftMargin: 34
        Layout.rightMargin: 34
        title: qsTr("Thumbnail cache")
        textColor: panel.textColor
        mutedTextColor: panel.mutedTextColor
        dividerColor: panel.dividerColor

        GridLayout {
            Layout.fillWidth: true
            columns: 3
            columnSpacing: 12
            rowSpacing: 12

            CacheMetric {
                Layout.fillWidth: true
                label: qsTr("Entries")
                value: panel.statLineValue("Entries")
            }

            CacheMetric {
                Layout.fillWidth: true
                label: qsTr("Size")
                value: panel.statLineValue("Size")
            }

            CacheMetric {
                Layout.fillWidth: true
                label: qsTr("Hits / misses")
                value: panel.hitsMissesValue()
            }
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
                    text: qsTr("Cache directory")
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
                        text: panel.pendingRoot.length > 0
                              ? panel.pendingRoot
                              : panel.statLineValue("Root")
                        elide: Text.ElideMiddle
                        verticalAlignment: Text.AlignVCenter
                        color: panel.textColor
                        font.family: panel.dataFontFamily
                        font.pixelSize: appTheme.fontSizeBody
                    }
                }
            }

            IconButton {
                buttonWidth: 36
                buttonHeight: 36
                buttonRadius: appTheme.controlRadiusSmall
                iconSize: appTheme.iconOpticalSizeCompact
                kind: "normal"
                bordered: true
                borderColor: appTheme.cardBorderColor
                iconSrc: "qrc:/panel_icons/folder-open.svg"
                tooltipText: qsTr("Select Thumbnail Cache Folder")
                Layout.alignment: Qt.AlignBottom
                onClicked: cacheFolderDialog.open()
            }
        }
    }

    SettingsSection {
        Layout.fillWidth: true
        Layout.leftMargin: 34
        Layout.rightMargin: 34
        title: qsTr("Limits")
        textColor: panel.textColor
        mutedTextColor: panel.mutedTextColor
        dividerColor: panel.dividerColor

        RowLayout {
            Layout.fillWidth: true
            spacing: 16

            Label {
                Layout.preferredWidth: 180
                text: qsTr("Disk cache")
                color: panel.textColor
                font.pixelSize: 15
                font.weight: 600
            }

            Switch {
                id: cacheEnabledSwitch
                checked: panel.pendingEnabled
                onToggled: panel.pendingEnabled = checked
                Accessible.name: qsTr("Disk cache")

                indicator: Rectangle {
                    implicitWidth: 40
                    implicitHeight: 22
                    x: cacheEnabledSwitch.leftPadding
                    y: parent.height / 2 - height / 2
                    radius: height / 2
                    color: cacheEnabledSwitch.checked
                           ? appTheme.editorListSelectedFillColor
                           : appTheme.bgBaseColor
                    border.width: 1
                    border.color: cacheEnabledSwitch.checked
                                  ? appTheme.editorListSelectedFillColor
                                  : appTheme.cardBorderColor

                    Rectangle {
                        x: cacheEnabledSwitch.checked
                           ? parent.width - width - 2 : 2
                        anchors.verticalCenter: parent.verticalCenter
                        width: 16
                        height: 16
                        radius: 8
                        color: cacheEnabledSwitch.checked
                               ? appTheme.editorListSelectedInkColor
                               : appTheme.textMutedColor

                        Behavior on x {
                            NumberAnimation {
                                duration: appTheme.reduceMotion
                                          ? 0 : appTheme.motionFadeMs
                            }
                        }
                    }
                }

                contentItem: Text {
                    text: cacheEnabledSwitch.checked
                          ? qsTr("Enabled") : qsTr("Disabled")
                    color: panel.textColor
                    font.pixelSize: 15
                    font.weight: 600
                    leftPadding: cacheEnabledSwitch.indicator.width + 12
                    verticalAlignment: Text.AlignVCenter
                }
            }
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: 16

            Label {
                Layout.preferredWidth: 180
                text: qsTr("Max entries")
                color: panel.textColor
                font.pixelSize: 15
                font.weight: 600
            }

            SettingsSpinBox {
                id: maxEntriesSpin
                objectName: "settingsCacheMaxEntriesSpin"
                Layout.fillWidth: true
                from: 1
                to: 100000
                stepSize: 500
                value: panel.pendingMaxEntries
                onValueModified: panel.pendingMaxEntries = value
            }
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: 16

            Label {
                Layout.preferredWidth: 180
                text: qsTr("JPEG quality")
                color: panel.textColor
                font.pixelSize: 15
                font.weight: 600
            }

            SettingsSpinBox {
                id: jpegQualitySpin
                objectName: "settingsCacheJpegQualitySpin"
                Layout.fillWidth: true
                from: 1
                to: 100
                stepSize: 5
                value: panel.pendingJpegQuality
                onValueModified: panel.pendingJpegQuality = value
            }
        }
    }

    SettingsSection {
        Layout.fillWidth: true
        Layout.leftMargin: 34
        Layout.rightMargin: 34
        Layout.bottomMargin: 26
        title: qsTr("Maintenance")
        textColor: panel.textColor
        mutedTextColor: panel.mutedTextColor
        dividerColor: panel.dividerColor

        RowLayout {
            Layout.fillWidth: true
            spacing: 12

            DialogActionButton {
                kind: "normal"
                Layout.fillWidth: true
                buttonWidth: 188
                buttonHeight: 42
                text: qsTr("Clear current project")
                enabled: panel.projectReady
                onClicked: {
                    if (panel.hasLibrary)
                        panel.libraryModule.ClearProjectThumbnailDiskCache()
                    panel.refreshStats()
                    panel.messageRequested(qsTr("Current project cache cleared"))
                }
            }

            DialogActionButton {
                kind: "danger"
                Layout.fillWidth: true
                buttonWidth: 148
                buttonHeight: 42
                text: qsTr("Clear all cache")
                onClicked: {
                    if (panel.hasLibrary)
                        panel.libraryModule.ClearAllThumbnailDiskCache()
                    panel.refreshStats()
                    panel.messageRequested(qsTr("All thumbnail cache cleared"))
                }
            }

            DialogActionButton {
                kind: "normal"
                Layout.fillWidth: true
                buttonWidth: 112
                buttonHeight: 42
                text: qsTr("Refresh")
                onClicked: panel.refreshStats()
            }
        }
    }

    Item {
        Layout.fillHeight: true
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

    // Numeric stepper with AdjustmentCombo sunken chrome: type a value, or
    // click / hold the left (−) and right (+) sides, or use the wheel.
    component SettingsSpinBox: SpinBox {
        id: spin
        editable: true
        implicitHeight: 36
        Layout.preferredHeight: 36
        font.pixelSize: appTheme.fontSizeBody
        font.family: appTheme.dataFontFamily
        leftPadding: 36
        rightPadding: 36
        topPadding: 0
        bottomPadding: 0
        wheelEnabled: true

        background: Rectangle {
            implicitHeight: 36
            radius: appTheme.controlRadiusSmall
            color: appTheme.bgBaseColor
            border.width: 1
            border.color: spin.activeFocus || spin.hovered
                          ? appTheme.textMutedColor
                          : appTheme.cardBorderColor
        }

        contentItem: TextInput {
            z: 2
            text: spin.textFromValue(spin.value, spin.locale)
            font: spin.font
            color: spin.enabled ? appTheme.textColor : appTheme.textMutedColor
            selectionColor: appTheme.editorListSelectedFillColor
            selectedTextColor: appTheme.editorListSelectedInkColor
            horizontalAlignment: Qt.AlignHCenter
            verticalAlignment: Qt.AlignVCenter
            readOnly: !spin.editable
            validator: spin.validator
            inputMethodHints: Qt.ImhDigitsOnly
            selectByMouse: true
        }

        down.indicator: Item {
            x: 0
            implicitWidth: 36
            implicitHeight: 36
            height: spin.height
            opacity: spin.enabled && spin.value > spin.from ? 1.0 : 0.45

            Text {
                anchors.centerIn: parent
                text: "−"
                color: spin.down.pressed || spin.down.hovered
                       ? appTheme.textColor
                       : appTheme.textMutedColor
                font.pixelSize: appTheme.fontSizeTitle
            }
        }

        up.indicator: Item {
            x: spin.width - width
            implicitWidth: 36
            implicitHeight: 36
            height: spin.height
            opacity: spin.enabled && spin.value < spin.to ? 1.0 : 0.45

            Text {
                anchors.centerIn: parent
                text: "+"
                color: spin.up.pressed || spin.up.hovered
                       ? appTheme.textColor
                       : appTheme.textMutedColor
                font.pixelSize: appTheme.fontSizeTitle
            }
        }
    }

    component CacheMetric: Rectangle {
        property string label: ""
        property string value: ""

        implicitHeight: 84
        radius: 8
        color: Qt.rgba(panel.canvasColor.r, panel.canvasColor.g, panel.canvasColor.b, 0.62)

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
