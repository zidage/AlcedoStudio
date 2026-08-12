import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts

// Complete inline update workflow for Settings > About. Startup checks stay
// silent; all release details and actions remain on this page.
Rectangle {
    id: root
    objectName: "aboutUpdateNotice"

    required property var updates
    property bool showWhenUnchecked: true

    readonly property bool enabledUpdates: updates && updates.enabled
    readonly property bool hasOffer: enabledUpdates
                                     && (updates.offerAvailable
                                         || updates.downloading
                                         || updates.downloadReady)
    readonly property bool showRow: updates !== null
                                    && (showWhenUnchecked || !updates.unchecked)

    visible: showRow
    implicitWidth: 200
    implicitHeight: visible ? content.implicitHeight + appTheme.spaceLg * 2 : 0
    radius: appTheme.controlRadius
    color: appTheme.cardSurfaceColor
    border.width: 1
    border.color: updates && updates.hasError
                  ? appTheme.dangerColor
                  : appTheme.cardBorderColor

    function channelPrefix() {
        return (updates && updates.channel && String(updates.channel) !== "stable")
               ? qsTr("Beta channel — ") : ""
    }

    function statusLabel() {
        if (!updates)
            return ""
        if (!updates.enabled)
            return qsTr("Updates are disabled in this build.")
        if (updates.unchecked)
            return channelPrefix() + qsTr("Not checked yet.")
        if (updates.checking)
            return channelPrefix() + qsTr("Checking for updates…")
        if (updates.statusText.length > 0)
            return updates.statusText
        return channelPrefix() + qsTr("Not checked yet.")
    }

    function primaryLabel() {
        if (!updates)
            return ""
        if (updates.checking)
            return qsTr("Checking…")
        if (updates.downloading)
            return qsTr("Cancel download")
        if (updates.downloadReady)
            return qsTr("Install and restart")
        if (updates.offerAvailable && updates.installAllowed)
            return qsTr("Download update")
        return qsTr("Check for updates")
    }

    function runPrimaryAction() {
        if (!updates || !enabledUpdates)
            return
        if (updates.downloading) {
            updates.CancelDownload()
        } else if (updates.downloadReady) {
            updates.InstallUpdate()
        } else if (updates.offerAvailable && updates.installAllowed) {
            updates.DownloadUpdate()
        } else {
            updates.CheckForUpdates()
        }
    }

    ColumnLayout {
        id: content
        anchors.fill: parent
        anchors.margins: appTheme.spaceLg
        spacing: appTheme.spaceMd

        RowLayout {
            Layout.fillWidth: true
            spacing: appTheme.spaceMd

            Rectangle {
                Layout.preferredWidth: appTheme.spaceSm
                Layout.preferredHeight: appTheme.spaceSm
                Layout.alignment: Qt.AlignVCenter
                radius: width / 2
                visible: root.hasOffer
                color: root.updates && root.updates.downloading
                       ? appTheme.backgroundTaskWorkingColor
                       : appTheme.backgroundTaskFinishedColor
                Accessible.name: qsTr("Update available")
                Accessible.role: Accessible.StaticText
            }

            ColumnLayout {
                Layout.fillWidth: true
                Layout.alignment: Qt.AlignVCenter
                spacing: appTheme.spaceXs

                Label {
                    objectName: "aboutUpdateStatusLabel"
                    Layout.fillWidth: true
                    text: root.statusLabel()
                    color: appTheme.textColor
                    font.family: appTheme.uiFontFamily
                    font.pixelSize: appTheme.fontSizeBody
                    font.weight: appTheme.fontWeightStrong
                    wrapMode: Text.WordWrap
                }

                Label {
                    Layout.fillWidth: true
                    visible: root.hasOffer && root.updates
                    text: root.updates
                          ? qsTr("Installed: %1 · Available: %2")
                                .arg(root.updates.currentVersion)
                                .arg(root.updates.availableVersion)
                          : ""
                    color: appTheme.textMutedColor
                    font.family: appTheme.dataFontFamily
                    font.pixelSize: appTheme.fontSizeCaption
                    wrapMode: Text.WordWrap
                }
            }

            DialogActionButton {
                objectName: "aboutUpdateActionButton"
                Layout.alignment: Qt.AlignVCenter
                enabled: root.enabledUpdates && !root.updates.checking
                text: root.primaryLabel()
                kind: root.updates
                      && (root.updates.offerAvailable || root.updates.downloadReady)
                      ? "accent" : "normal"
                onClicked: root.runPrimaryAction()
            }
        }

        Label {
            objectName: "aboutUpdateErrorLabel"
            Layout.fillWidth: true
            visible: root.updates && root.updates.errorText.length > 0
            text: root.updates ? root.updates.errorText : ""
            color: appTheme.dangerColor
            font.family: appTheme.uiFontFamily
            font.pixelSize: appTheme.fontSizeCaption
            font.weight: appTheme.fontWeightRegular
            wrapMode: Text.WordWrap
        }

        Label {
            Layout.fillWidth: true
            visible: root.updates && root.updates.offerAvailable
                     && !root.updates.installAllowed
            text: qsTr("This build can check for updates but cannot install them.")
            color: appTheme.textMutedColor
            font.family: appTheme.uiFontFamily
            font.pixelSize: appTheme.fontSizeCaption
            wrapMode: Text.WordWrap
        }

        ThemedProgressBar {
            objectName: "aboutUpdateProgressBar"
            Layout.fillWidth: true
            visible: root.updates && root.updates.downloading
            active: visible
            showDetails: true
            progressValue: root.updates ? root.updates.progress * 100 : 0
            indeterminate: root.updates && root.updates.progress <= 0
            leadingText: root.updates ? root.updates.downloadedBytesText : ""
            speedText: root.updates ? root.updates.downloadSpeedText : ""
            etaText: root.updates ? root.updates.downloadEtaText : ""
        }

        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: changelogText.implicitHeight + appTheme.spaceMd * 2
            visible: root.updates && root.updates.changelog.length > 0
            color: appTheme.bgBaseColor
            radius: appTheme.controlRadiusSmall
            border.width: 1
            border.color: appTheme.dividerColor

            Label {
                id: changelogText
                objectName: "aboutUpdateChangelogLabel"
                anchors.fill: parent
                anchors.margins: appTheme.spaceMd
                text: root.updates ? root.updates.changelog : ""
                color: appTheme.textColor
                font.family: appTheme.uiFontFamily
                font.pixelSize: appTheme.fontSizeBody
                wrapMode: Text.WordWrap
                lineHeight: 1.35
            }
        }

        DialogActionButton {
            Layout.alignment: Qt.AlignLeft
            visible: root.updates && root.updates.notesUrl.toString().length > 0
            text: qsTr("Open release notes")
            kind: "normal"
            onClicked: root.updates.OpenReleaseNotes()
        }
    }
}
