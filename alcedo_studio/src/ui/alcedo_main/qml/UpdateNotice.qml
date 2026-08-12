import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts

// About-page update row. Missing remote update data reports an error in place;
// it never removes the manual update action.
Rectangle {
    id: root
    objectName: "aboutUpdateNotice"

    required property var updates
    property bool showWhenUnchecked: true

    readonly property bool enabledUpdates: updates && updates.enabled
    readonly property bool hasOffer: enabledUpdates
                                     && (updates.updateDeferred || updates.updateAvailable)
    readonly property bool showRow: updates !== null
                                    && (showWhenUnchecked
                                        || !updates.unchecked)

    signal offerRequested()

    visible: showRow
    implicitWidth: 200
    implicitHeight: visible ? content.implicitHeight + appTheme.spaceLg * 2 : 0
    radius: appTheme.controlRadius
    color: appTheme.cardSurfaceColor
    border.width: 1
    border.color: updates && updates.hasError
                  ? appTheme.dangerColor
                  : appTheme.cardBorderColor

    // Surface the build's update channel only on the idle status lines so the
    // user can tell a test (beta) build from an official (stable) one at a
    // glance. The Available / UpToDate / Error status text already carries the
    // outcome, so it is not prefixed.
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

    function actionLabel() {
        if (updates && updates.checking)
            return qsTr("Checking…")
        if (hasOffer)
            return qsTr("View update")
        return qsTr("Check for updates")
    }

    RowLayout {
        id: content
        anchors.fill: parent
        anchors.margins: appTheme.spaceLg
        spacing: appTheme.spaceMd

        Rectangle {
            Layout.preferredWidth: appTheme.spaceSm
            Layout.preferredHeight: appTheme.spaceSm
            Layout.alignment: Qt.AlignVCenter
            radius: width / 2
            visible: root.hasOffer
            color: appTheme.backgroundTaskFinishedColor
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

            Rectangle {
                Layout.fillWidth: true
                Layout.preferredHeight: appTheme.spaceXs
                visible: root.updates && root.updates.downloading
                radius: appTheme.badgeRadius
                color: appTheme.bgBaseColor

                Rectangle {
                    width: parent.width
                           * Math.max(0, Math.min(1, root.updates ? root.updates.progress : 0))
                    height: parent.height
                    radius: parent.radius
                    color: appTheme.accentColor
                }
            }
        }

        Button {
            id: actionButton
            objectName: "aboutUpdateActionButton"

            Layout.alignment: Qt.AlignVCenter
            Layout.preferredHeight: appTheme.iconButtonHitSizeCompact
            Layout.preferredWidth: Math.max(148,
                                            actionLabel.implicitWidth
                                            + appTheme.spaceLg * 2)
            enabled: root.enabledUpdates
                     && !root.updates.checking
                     && !root.updates.downloading
                     && !root.updates.installing
            text: root.actionLabel()
            onClicked: {
                if (!root.updates || !root.enabledUpdates)
                    return
                if (root.hasOffer) {
                    root.offerRequested()
                    return
                }
                root.updates.CheckForUpdates()
            }

            contentItem: Label {
                id: actionLabel
                text: actionButton.text
                color: {
                    if (!actionButton.enabled)
                        return appTheme.textMutedColor
                    if (actionButton.down || actionButton.hovered)
                        return appTheme.textColor
                    return appTheme.editorListSelectedInkColor
                }
                font.family: appTheme.uiFontFamily
                font.pixelSize: appTheme.fontSizeBody
                font.weight: appTheme.fontWeightStrong
                horizontalAlignment: Text.AlignHCenter
                verticalAlignment: Text.AlignVCenter
            }

            background: Rectangle {
                radius: appTheme.controlRadiusSmall
                color: {
                    if (!actionButton.enabled)
                        return appTheme.bgBaseColor
                    if (actionButton.down)
                        return appTheme.buttonPressedFillColor
                    if (actionButton.hovered)
                        return appTheme.buttonHoveredFillColor
                    return appTheme.editorListSelectedFillColor
                }
                border.width: actionButton.activeFocus ? 1 : 0
                border.color: appTheme.accentColor
            }
        }
    }
}
