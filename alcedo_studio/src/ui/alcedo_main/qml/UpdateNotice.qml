import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts

// Reusable update workflow card. Settings > Updates hosts the full page;
// this card owns status, version compare, actions, progress, and changelog.
Rectangle {
    id: root
    objectName: "updatesNotice"

    required property var updates
    property bool showWhenUnchecked: true

    readonly property bool enabledUpdates: updates && updates.enabled
    readonly property bool hasOffer: enabledUpdates
                                     && (updates.offerAvailable
                                         || updates.downloading
                                         || updates.downloadReady)
    readonly property bool showRow: updates !== null
                                    && (showWhenUnchecked || !updates.unchecked)
    readonly property bool upToDate: enabledUpdates
                                     && !updates.unchecked
                                     && !updates.checking
                                     && !updates.hasError
                                     && !hasOffer
                                     && !updates.installing

    visible: showRow
    implicitWidth: 200
    implicitHeight: visible ? content.implicitHeight + appTheme.spaceLg * 2 : 0
    radius: appTheme.controlRadius
    color: appTheme.cardSurfaceColor
    border.width: 1
    border.color: updates && updates.hasError
                  ? appTheme.dangerColor
                  : appTheme.cardBorderColor

    readonly property string headlineText: {
        if (!updates)
            return ""
        if (!updates.enabled)
            return qsTr("Updates are unavailable")
        if (updates.checking)
            return qsTr("Checking for updates")
        if (updates.downloading)
            return qsTr("Downloading the update")
        if (updates.downloadReady)
            return qsTr("Ready to install")
        if (updates.installing)
            return qsTr("Installing the update")
        if (updates.hasError)
            return qsTr("The last update step failed")
        if (updates.offerAvailable)
            return qsTr("A newer version is available")
        if (updates.unchecked)
            return qsTr("Not checked yet")
        return qsTr("Alcedo Studio is up to date")
    }

    readonly property string nextStepText: {
        if (!updates)
            return ""
        if (!updates.enabled)
            return qsTr("This build was compiled without a signed update feed, so it cannot check or install releases.")
        if (updates.checking)
            return qsTr("Contacting the signed release feed. This usually takes a few seconds.")
        if (updates.downloading)
            return qsTr("The package is downloading now. You can cancel and try again later.")
        if (updates.downloadReady)
            return qsTr("The package is verified. Install will close Alcedo Studio, replace this copy, and reopen.")
        if (updates.installing)
            return qsTr("Alcedo Studio is closing so the installer can replace this copy.")
        if (updates.hasError)
            return qsTr("Review the error below, then check again. A later check can resume a partial download.")
        if (updates.offerAvailable && !updates.installAllowed)
            return qsTr("This development build can list a newer release, but it cannot replace itself. Use a packaged installer to install updates.")
        if (updates.offerAvailable)
            return qsTr("Download the signed package, then install and restart when the file is ready.")
        if (updates.unchecked)
            return qsTr("Check the signed release feed to see whether a newer build is available.")
        return ""
    }

    readonly property string statusDetailText: {
        if (!updates)
            return ""
        if (!updates.enabled)
            return qsTr("Updates are disabled in this build.")
        if (updates.unchecked)
            return qsTr("Not checked yet.")
        if (updates.checking)
            return qsTr("Checking for updates…")
        if (updates.statusText.length > 0)
            return updates.statusText
        return qsTr("Not checked yet.")
    }

    readonly property string primaryLabel: {
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
        if (root.upToDate)
            return qsTr("Check again")
        return qsTr("Check for updates")
    }

    readonly property string availableVersionText: {
        if (!updates)
            return qsTr("Not checked")
        if (updates.availableVersion && String(updates.availableVersion).length > 0)
            return updates.availableVersion
        if (updates.unchecked || updates.checking)
            return qsTr("Not checked")
        return qsTr("Same as installed")
    }

    readonly property color lampColor: {
        if (!updates)
            return appTheme.textMutedColor
        if (updates.hasError)
            return appTheme.backgroundTaskFailedColor
        if (updates.checking || updates.downloading || updates.installing)
            return appTheme.backgroundTaskWorkingColor
        if (root.hasOffer || root.upToDate)
            return appTheme.backgroundTaskFinishedColor
        return appTheme.textMutedColor
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
            spacing: appTheme.spaceSm

            Rectangle {
                Layout.preferredWidth: appTheme.spaceSm
                Layout.preferredHeight: appTheme.spaceSm
                Layout.alignment: Qt.AlignVCenter
                radius: width / 2
                color: root.lampColor
                Accessible.name: root.headlineText
                Accessible.role: Accessible.Indicator
            }

            Label {
                objectName: "updatesStatusLabel"
                Layout.fillWidth: true
                text: root.headlineText
                color: appTheme.textColor
                font.family: appTheme.uiFontFamily
                font.pixelSize: appTheme.fontSizeSection
                font.weight: appTheme.fontWeightHeading
                wrapMode: Text.WordWrap
            }
        }

        Label {
            Layout.fillWidth: true
            visible: root.nextStepText.length > 0
            text: root.nextStepText
            color: appTheme.textMutedColor
            font.family: appTheme.uiFontFamily
            font.pixelSize: appTheme.fontSizeBody
            font.weight: appTheme.fontWeightRegular
            wrapMode: Text.WordWrap
            lineHeight: 1.35
        }

        Label {
            Layout.fillWidth: true
            visible: root.updates
                     && root.updates.statusText.length > 0
                     && root.updates.statusText !== root.headlineText
            text: root.statusDetailText
            color: appTheme.textMutedColor
            font.family: appTheme.uiFontFamily
            font.pixelSize: appTheme.fontSizeCaption
            wrapMode: Text.WordWrap
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: appTheme.spaceMd

            VersionTile {
                Layout.fillWidth: true
                label: qsTr("Installed")
                value: root.updates ? root.updates.currentVersion : qsTr("Unavailable")
            }

            VersionTile {
                Layout.fillWidth: true
                objectName: "updatesAvailableVersionTile"
                label: root.hasOffer ? qsTr("Available") : qsTr("Latest")
                value: root.availableVersionText
                emphasized: root.hasOffer
            }
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: appTheme.spaceSm

            DialogActionButton {
                objectName: "updatesActionButton"
                Layout.alignment: Qt.AlignVCenter
                enabled: root.enabledUpdates && root.updates && !root.updates.checking
                text: root.primaryLabel
                kind: root.updates
                      && (root.updates.offerAvailable || root.updates.downloadReady)
                      ? "accent" : "normal"
                onClicked: root.runPrimaryAction()
            }

            DialogActionButton {
                Layout.alignment: Qt.AlignVCenter
                visible: root.updates && root.updates.notesUrl.toString().length > 0
                text: qsTr("Open release notes")
                kind: "normal"
                onClicked: root.updates.OpenReleaseNotes()
            }

            Item {
                Layout.fillWidth: true
            }
        }

        Label {
            objectName: "updatesErrorLabel"
            Layout.fillWidth: true
            visible: root.updates && root.updates.errorText.length > 0
            text: root.updates ? root.updates.errorText : ""
            color: appTheme.dangerColor
            font.family: appTheme.uiFontFamily
            font.pixelSize: appTheme.fontSizeCaption
            font.weight: appTheme.fontWeightRegular
            wrapMode: Text.WordWrap
        }

        ThemedProgressBar {
            objectName: "updatesProgressBar"
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

        ColumnLayout {
            Layout.fillWidth: true
            visible: root.updates && root.updates.changelog.length > 0
            spacing: appTheme.spaceSm

            Label {
                Layout.fillWidth: true
                text: qsTr("What's new")
                color: appTheme.textColor
                font.family: appTheme.uiFontFamily
                font.pixelSize: appTheme.fontSizeBody
                font.weight: appTheme.fontWeightHeading
            }

            Rectangle {
                Layout.fillWidth: true
                Layout.preferredHeight: changelogText.implicitHeight + appTheme.spaceMd * 2
                color: appTheme.bgBaseColor
                radius: appTheme.controlRadiusSmall
                border.width: 1
                border.color: appTheme.dividerColor

                Label {
                    id: changelogText
                    objectName: "updatesChangelogLabel"
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
        }
    }

    component VersionTile: Rectangle {
        id: tile

        property string label: ""
        property string value: ""
        property bool emphasized: false

        implicitHeight: tileColumn.implicitHeight + appTheme.spaceMd * 2
        radius: appTheme.controlRadiusSmall
        color: appTheme.bgBaseColor
        border.width: 1
        border.color: tile.emphasized ? appTheme.accentSecondaryColor : appTheme.dividerColor

        ColumnLayout {
            id: tileColumn
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.verticalCenter: parent.verticalCenter
            anchors.leftMargin: appTheme.spaceMd
            anchors.rightMargin: appTheme.spaceMd
            spacing: appTheme.spaceXs

            Label {
                Layout.fillWidth: true
                text: tile.label
                color: appTheme.textMutedColor
                font.family: appTheme.uiFontFamily
                font.pixelSize: appTheme.fontSizeCaption
                font.weight: appTheme.fontWeightStrong
            }

            Label {
                Layout.fillWidth: true
                text: tile.value
                color: appTheme.textColor
                font.family: appTheme.dataFontFamily
                font.pixelSize: appTheme.fontSizeBody
                font.weight: appTheme.fontWeightHeading
                wrapMode: Text.WordWrap
            }
        }
    }
}
