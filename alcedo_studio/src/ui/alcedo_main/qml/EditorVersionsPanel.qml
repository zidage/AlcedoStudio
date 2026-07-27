import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Alcedo.Main 1.0

// Named Version refs and their stable graph identities. This panel is kept
// independent from the transaction timeline so each rail destination owns its
// own layout and actions.
Item {
    id: root
    objectName: "editorVersionsPageBody"

    property var theme: null
    property var editorSession: null
    property var historyModel: null
    property bool versionCheckoutEnabled: true
    property string versionCheckoutDisabledReason: ""

    readonly property color colText: theme ? theme.colText : appTheme.textColor
    readonly property color colMuted: theme ? theme.colTextMuted : appTheme.textMutedColor
    readonly property color colCardSurface: theme ? theme.colCardSurface : appTheme.cardSurfaceColor
    readonly property color colCardBorder: theme ? theme.colCardBorder : appTheme.cardBorderColor

    Dialog {
        id: versionNameDialog
        objectName: "editorVersionNameDialog"
        property string editVersionId: ""
        property bool renameMode: false
        modal: true
        title: renameMode ? qsTr("Rename Version") : qsTr("Create Version")
        width: appTheme.editorSidePanelWidth

        footer: RowLayout {
            width: parent.width
            spacing: appTheme.spaceSm

            Item { Layout.fillWidth: true }

            DialogActionButton {
                objectName: "editorVersionCancelButton"
                text: qsTr("Cancel")
                kind: "normal"
                buttonWidth: appTheme.spaceXl * 4
                buttonHeight: appTheme.spaceXl * 2 + appTheme.spaceSm
                buttonRadius: appTheme.controlRadiusSmall
                onClicked: versionNameDialog.reject()
            }

            DialogActionButton {
                objectName: "editorVersionAcceptButton"
                text: qsTr("OK")
                kind: "normal"
                buttonWidth: appTheme.spaceXl * 4
                buttonHeight: appTheme.spaceXl * 2 + appTheme.spaceSm
                buttonRadius: appTheme.controlRadiusSmall
                onClicked: {
                    if (versionNameField.text.trim().length === 0) {
                        versionNameField.forceActiveFocus()
                        return
                    }
                    versionNameDialog.accept()
                }
            }
        }

        onAccepted: {
            var name = versionNameField.text.trim()
            if (!root.historyModel) return
            if (renameMode) {
                root.historyModel.renameVersion(editVersionId, name)
            } else {
                root.historyModel.createRootVersion(name)
            }
        }

        contentItem: ColumnLayout {
            spacing: appTheme.spaceSm

            Label {
                Layout.fillWidth: true
                text: qsTr("Use a stable name for this editable look.")
                color: root.colMuted
                wrapMode: Text.WordWrap
                font.family: appTheme.uiFontFamily
                font.pixelSize: appTheme.fontSizeBody
            }

            TextField {
                id: versionNameField
                objectName: "editorVersionNameField"
                Layout.fillWidth: true
                color: root.colText
                font.family: appTheme.uiFontFamily
                font.pixelSize: appTheme.fontSizeBody
                placeholderText: qsTr("Version name")
                selectByMouse: true
                leftPadding: appTheme.spaceSm
                rightPadding: appTheme.spaceSm
                selectionColor: appTheme.editorListSelectedFillColor
                selectedTextColor: appTheme.editorListSelectedInkColor
                background: Rectangle {
                    implicitHeight: appTheme.spaceXl * 2 + appTheme.spaceSm
                    radius: appTheme.controlRadiusSmall
                    color: appTheme.bgBaseColor
                    border.width: 1
                    border.color: versionNameField.activeFocus
                                  ? root.colText
                                  : root.colCardBorder
                }
            }
        }

        onOpened: {
            versionNameField.selectAll()
            versionNameField.forceActiveFocus()
        }
    }

    function openCreateVersion() {
        if (!root.historyModel) return
        versionNameDialog.renameMode = false
        versionNameDialog.editVersionId = ""
        versionNameField.text = qsTr("Version %1").arg(root.historyModel.versions.count + 1)
        versionNameDialog.open()
    }

    function openRenameVersion(versionId, displayName) {
        versionNameDialog.renameMode = true
        versionNameDialog.editVersionId = versionId
        versionNameField.text = displayName
        versionNameDialog.open()
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: appTheme.spaceLg
        spacing: appTheme.spaceMd

        RowLayout {
            objectName: "editorVersionsPanelHeader"
            Layout.fillWidth: true
            spacing: appTheme.spaceSm

            ColumnLayout {
                Layout.fillWidth: true
                spacing: appTheme.spaceXs

                Label {
                    objectName: "editorHistoryVersionsPanelTitle"
                    Layout.fillWidth: true
                    text: qsTr("Versions")
                    color: root.colText
                    font.family: appTheme.uiFontFamily
                    font.pixelSize: appTheme.fontSizeSection
                    font.weight: appTheme.fontWeightHeading
                }

                Label {
                    Layout.fillWidth: true
                    text: qsTr("%1 named looks").arg(root.historyModel && root.historyModel.versions
                                                       ? root.historyModel.versions.count : 0)
                    color: root.colMuted
                    font.family: appTheme.dataFontFamily
                    font.pixelSize: appTheme.fontSizeCaption
                }
            }

            IconActionButton {
                objectName: "editorCreateVersionButton"
                compact: true
                enabled: root.versionCheckoutEnabled && root.editorSession
                         && root.editorSession.canEdit
                iconSrc: "qrc:/panel_icons/plus.svg"
                iconColorDefault: root.colText
                iconColorMuted: root.colMuted
                fillIdle: root.colCardSurface
                fillHover: appTheme.buttonHoveredFillColor
                fillPressed: appTheme.buttonPressedFillColor
                fillSelected: appTheme.buttonSelectedFillColor
                focusRingColor: root.colText
                actionName: qsTr("Create Version")
                onClicked: root.openCreateVersion()
            }
        }

        RowLayout {
            Layout.fillWidth: true

            Label {
                Layout.fillWidth: true
                text: root.versionCheckoutEnabled
                      ? qsTr("Named looks")
                      : (root.versionCheckoutDisabledReason.length > 0
                         ? root.versionCheckoutDisabledReason
                         : qsTr("Version checkout is unavailable"))
                color: root.colMuted
                elide: Text.ElideRight
                font.family: appTheme.uiFontFamily
                font.pixelSize: appTheme.fontSizeCaption
            }
        }

        Item {
            objectName: "editorVersionsListWell"
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true

            Rectangle {
                anchors.fill: parent
                radius: appTheme.controlRadiusSmall
                color: appTheme.bgBaseColor
                border.width: 1
                border.color: root.colCardBorder
            }

            ListView {
                id: versionList
                objectName: "editorVersionsList"
                anchors.fill: parent
                anchors.margins: appTheme.spaceXs
                clip: true
                spacing: appTheme.spaceSm
                model: root.historyModel ? root.historyModel.versions : null

                delegate: Rectangle {
                    id: versionCard
                    objectName: "editorVersionCard"
                    property string versionId: model.versionId
                    property string displayName: model.displayName
                    property string versionHead: model.headCommitHash
                    property bool versionActive: Boolean(model.active)
                    property color selectionOutlineColor: versionActive
                                                          ? root.colText : root.colCardBorder
                    width: versionList.width
                    height: versionActive ? appTheme.spaceXl * 5 : appTheme.spaceXl * 4
                    radius: appTheme.controlRadiusSmall
                    color: root.colCardSurface
                    border.width: 1
                    // Version selection is intentionally outline-only. The
                    // dark card remains visible behind the white intent line.
                    border.color: selectionOutlineColor

                    MouseArea {
                        anchors.fill: parent
                        z: 0
                        enabled: root.versionCheckoutEnabled && !versionActive
                        onClicked: root.historyModel.checkoutVersion(versionId)
                    }

                    ColumnLayout {
                        anchors.left: parent.left
                        anchors.right: actionRow.left
                        anchors.verticalCenter: parent.verticalCenter
                        anchors.leftMargin: appTheme.spaceMd
                        anchors.rightMargin: appTheme.spaceXs
                        spacing: appTheme.spaceXs

                        RowLayout {
                            Layout.fillWidth: true
                            spacing: appTheme.spaceSm

                            Label {
                                objectName: "editorVersionTitle"
                                Layout.fillWidth: true
                                text: displayName
                                color: root.colText
                                elide: Text.ElideRight
                                font.family: appTheme.uiFontFamily
                                font.pixelSize: appTheme.fontSizeTitle
                                font.weight: appTheme.fontWeightStrong
                            }

                            Rectangle {
                                visible: versionActive
                                implicitWidth: currentBadgeLabel.implicitWidth + appTheme.spaceSm
                                implicitHeight: currentBadgeLabel.implicitHeight + appTheme.spaceXs
                                radius: appTheme.badgeRadius
                                color: "transparent"
                                border.width: 1
                                border.color: root.colText

                                Label {
                                    id: currentBadgeLabel
                                    anchors.centerIn: parent
                                    text: qsTr("CURRENT HEAD")
                                    color: root.colText
                                    font.family: appTheme.dataFontFamily
                                    font.pixelSize: appTheme.fontSizeCaption
                                    font.weight: appTheme.fontWeightStrong
                                }
                            }
                        }

                        Label {
                            Layout.fillWidth: true
                            objectName: "editorVersionSubtitle"
                            text: versionActive
                                  ? qsTr("Checked out")
                                  : qsTr("Head %1").arg(versionHead.length > 0
                                                         ? versionHead.slice(0, 8)
                                                         : qsTr("image root"))
                            color: root.colMuted
                            elide: Text.ElideRight
                            font.family: appTheme.uiFontFamily
                            font.pixelSize: appTheme.fontSizeCaption
                        }

                        Label {
                            Layout.fillWidth: true
                            visible: versionHead.length > 0
                            text: qsTr("Commit %1").arg(versionHead.slice(0, 8))
                            color: root.colMuted
                            elide: Text.ElideRight
                            font.family: appTheme.dataFontFamily
                            font.pixelSize: appTheme.fontSizeCaption
                        }
                    }

                    Row {
                        id: actionRow
                        anchors.right: parent.right
                        anchors.verticalCenter: parent.verticalCenter
                        anchors.rightMargin: appTheme.spaceXs
                        spacing: appTheme.spaceXs
                        z: 2

                        IconActionButton {
                            objectName: "editorRenameVersionButton"
                            compact: true
                            enabled: root.versionCheckoutEnabled && root.editorSession
                                     && root.editorSession.canEdit
                            iconSrc: "qrc:/panel_icons/edit.svg"
                            iconColorDefault: root.colText
                            iconColorMuted: root.colMuted
                            fillIdle: root.colCardSurface
                            fillHover: appTheme.buttonHoveredFillColor
                            fillPressed: appTheme.buttonPressedFillColor
                            fillSelected: appTheme.buttonSelectedFillColor
                            focusRingColor: root.colText
                            actionName: qsTr("Rename Version")
                            onClicked: root.openRenameVersion(versionId, displayName)
                        }

                        IconActionButton {
                            objectName: "editorRemoveVersionButton"
                            compact: true
                            enabled: root.versionCheckoutEnabled && !versionActive
                                     && root.historyModel && root.historyModel.versions.count > 1
                                     && root.editorSession && root.editorSession.canEdit
                            iconSrc: "qrc:/panel_icons/stop.svg"
                            iconColorDefault: root.colText
                            iconColorMuted: root.colMuted
                            fillIdle: root.colCardSurface
                            fillHover: appTheme.buttonHoveredFillColor
                            fillPressed: appTheme.buttonPressedFillColor
                            fillSelected: appTheme.buttonSelectedFillColor
                            focusRingColor: root.colText
                            actionName: qsTr("Remove Version")
                            onClicked: root.historyModel.removeVersion(versionId)
                        }
                    }
                }

                Label {
                    anchors.centerIn: parent
                    width: parent.width - appTheme.spaceMd
                    visible: root.historyModel && root.historyModel.versions
                              && root.historyModel.versions.count === 0
                    text: root.versionCheckoutEnabled
                          ? qsTr("No versions yet")
                          : (root.versionCheckoutDisabledReason.length > 0
                             ? root.versionCheckoutDisabledReason
                             : qsTr("Version checkout is unavailable"))
                    color: root.colMuted
                    wrapMode: Text.WordWrap
                    horizontalAlignment: Text.AlignHCenter
                    font.family: appTheme.uiFontFamily
                    font.pixelSize: appTheme.fontSizeBody
                }
            }
        }
    }
}
