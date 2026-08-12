pragma ComponentBehavior: Bound

//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Effects
import QtQuick.Layouts

Dialog {
    id: root
    objectName: "updateOfferDialog"

    property var updates: null
    property Item blurSource: null
    property real cornerRadius: 0

    readonly property int preferredWidth: appTheme.editorSidePanelWidth * 2
    readonly property int preferredHeight: appTheme.editorSidePanelWidthMax
    readonly property bool canInstall: updates && updates.installAllowed
    readonly property bool hasChangelog: updates && updates.changelog.length > 0
    readonly property bool isDownloading: updates && updates.downloading
    readonly property bool isReady: updates && updates.downloadReady
    readonly property bool isError: updates && updates.hasError
    readonly property bool isAvailable: updates && updates.offerAvailable

    parent: Overlay.overlay
    modal: true
    focus: true
    title: ""
    padding: 0
    closePolicy: Popup.CloseOnEscape
    width: Math.min(parent ? parent.width - appTheme.spaceXl * 2 : preferredWidth, preferredWidth)
    height: Math.min(parent ? parent.height - appTheme.spaceXl * 2 : preferredHeight,
                     hasChangelog ? preferredHeight : Math.round(preferredHeight * 0.55))
    x: parent ? Math.round((parent.width - width) / 2) : 0
    y: parent ? Math.round((parent.height - height) / 2) : 0
    footer: Item {
        width: 1
        height: 0
    }

    function primaryAction() {
        if (!updates)
            return
        if (isReady) {
            updates.InstallUpdate()
            return
        }
        if (isAvailable && canInstall) {
            updates.DownloadUpdate()
            return
        }
        close()
    }

    function secondaryAction() {
        if (!updates)
            return
        if (isDownloading)
            return
        if (isAvailable || isReady)
            updates.DeferUpdate()
        close()
    }

    background: Rectangle {
        radius: appTheme.panelRadius
        color: appTheme.cardSurfaceColor
        border.width: 1
        border.color: appTheme.cardBorderColor
    }

    Overlay.modal: Item {
        anchors.fill: parent

        Rectangle {
            id: backdropMask
            anchors.fill: parent
            radius: root.cornerRadius
            color: appTheme.textColor
            visible: false
            layer.enabled: true
            layer.smooth: true
        }

        Item {
            anchors.fill: parent
            layer.enabled: true
            layer.smooth: true
            layer.effect: MultiEffect {
                maskEnabled: root.cornerRadius > 0
                maskSource: backdropMask
            }

            MultiEffect {
                anchors.fill: parent
                source: root.blurSource
                blurEnabled: root.blurSource !== null
                blur: 0.72
                blurMax: 72
                saturation: -0.24
                brightness: -0.08
            }

            Rectangle {
                anchors.fill: parent
                color: appTheme.overlayColor
            }
        }
    }

    contentItem: ColumnLayout {
        spacing: 0

        RowLayout {
            Layout.fillWidth: true
            Layout.leftMargin: appTheme.spaceLg
            Layout.rightMargin: appTheme.spaceLg
            Layout.topMargin: appTheme.spaceLg
            Layout.bottomMargin: appTheme.spaceMd
            spacing: appTheme.spaceMd

            ColumnLayout {
                Layout.fillWidth: true
                spacing: appTheme.spaceXs

                Label {
                    objectName: "updateOfferTitleLabel"
                    Layout.fillWidth: true
                    text: {
                        if (!root.updates)
                            return ""
                        if (root.isError)
                            return qsTr("Update check failed")
                        if (root.isDownloading)
                            return qsTr("Downloading Alcedo Studio %1…").arg(root.updates.availableVersion)
                        if (root.isReady)
                            return qsTr("Alcedo Studio %1 is ready to install.").arg(root.updates.availableVersion)
                        return qsTr("Alcedo Studio %1 is available.").arg(root.updates.availableVersion)
                    }
                    color: appTheme.textColor
                    font.family: appTheme.uiFontFamily
                    font.pixelSize: appTheme.fontSizeSection
                    font.weight: appTheme.fontWeightHeading
                    wrapMode: Text.WordWrap
                    Accessible.name: text
                }

                Label {
                    objectName: "updateOfferErrorLabel"
                    Layout.fillWidth: true
                    visible: root.isError && root.updates && root.updates.errorText.length > 0
                    text: root.updates ? root.updates.errorText : ""
                    color: appTheme.dangerColor
                    font.family: appTheme.uiFontFamily
                    font.pixelSize: appTheme.fontSizeCaption
                    wrapMode: Text.WordWrap
                }

                Label {
                    objectName: "updateOfferInstallRestrictionLabel"
                    Layout.fillWidth: true
                    visible: !root.canInstall && !root.isError
                    text: qsTr("This build can check for updates but cannot install them.")
                    color: appTheme.textMutedColor
                    font.family: appTheme.uiFontFamily
                    font.pixelSize: appTheme.fontSizeCaption
                    wrapMode: Text.WordWrap
                }
            }
        }

        Rectangle {
            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.leftMargin: appTheme.spaceLg
            Layout.rightMargin: appTheme.spaceLg
            visible: root.hasChangelog
            color: appTheme.bgBaseColor
            radius: appTheme.controlRadius
            border.width: 1
            border.color: appTheme.dividerColor
            clip: true

            ScrollView {
                id: changelogScroll
                anchors.fill: parent
                anchors.margins: appTheme.spaceMd
                clip: true
                contentWidth: availableWidth

                Label {
                    objectName: "updateOfferChangelogLabel"
                    width: changelogScroll.availableWidth
                    text: root.updates ? root.updates.changelog : ""
                    color: appTheme.textColor
                    font.family: appTheme.uiFontFamily
                    font.pixelSize: appTheme.fontSizeBody
                    wrapMode: Text.WordWrap
                    lineHeight: 1.35
                }
            }
        }

        Rectangle {
            Layout.fillWidth: true
            Layout.leftMargin: appTheme.spaceLg
            Layout.rightMargin: appTheme.spaceLg
            Layout.topMargin: appTheme.spaceMd
            Layout.preferredHeight: 3
            visible: root.isDownloading
            radius: appTheme.badgeRadius
            color: appTheme.dividerColor

            Rectangle {
                width: parent.width * Math.max(0, Math.min(1, root.updates ? root.updates.progress : 0))
                height: parent.height
                radius: parent.radius
                color: appTheme.accentColor
            }
        }

        RowLayout {
            Layout.fillWidth: true
            Layout.leftMargin: appTheme.spaceLg
            Layout.rightMargin: appTheme.spaceLg
            Layout.topMargin: appTheme.spaceLg
            Layout.bottomMargin: appTheme.spaceLg
            spacing: appTheme.spaceMd

            Item {
                Layout.fillWidth: true
            }

            DialogActionButton {
                visible: !root.isDownloading
                text: {
                    if (root.isError)
                        return qsTr("Close")
                    if (!root.canInstall)
                        return qsTr("Close")
                    return qsTr("Later")
                }
                kind: "normal"
                onClicked: root.secondaryAction()
            }

            DialogActionButton {
                visible: root.canInstall && (root.isAvailable || root.isReady) && !root.isDownloading
                text: root.isReady ? qsTr("Install and restart") : qsTr("Update")
                kind: "accent"
                onClicked: root.primaryAction()
            }
        }
    }

    onClosed: {
        if (updates && (isAvailable || isReady) && !updates.updateDeferred && !isDownloading)
            updates.DeferUpdate()
    }
}
