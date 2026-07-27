import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Alcedo.Main 1.0

// Named Version refs and their stable graph identities. This panel is kept
// independent from the transaction timeline so each rail destination owns its
// own layout and actions.
//
// Naming is an inline draft under the Versions header (Enter commits, Escape
// cancels, focus-loss commits only non-empty changed text). There is no modal
// naming dialog. Active selection is outline-only; removal uses a labeled
// trash action, never a stop-playback glyph.
Item {
    id: root
    objectName: "editorVersionsPageBody"

    property var theme: null
    property var editorSession: null
    property var historyModel: null
    property bool versionCheckoutEnabled: true
    property string versionCheckoutDisabledReason: ""

    // Inline draft state (create + rename share one field).
    property bool draftVisible: false
    property bool draftRenameMode: false
    property string draftVersionId: ""
    property string draftOriginalText: ""
    property bool draftSubmitPending: false
    property real _preservedContentY: 0
    property bool _restoringContentY: false

    readonly property color colText: theme ? theme.colText : appTheme.textColor
    readonly property color colMuted: theme ? theme.colTextMuted : appTheme.textMutedColor
    readonly property color colCardSurface: theme ? theme.colCardSurface : appTheme.cardSurfaceColor
    readonly property color colCardBorder: theme ? theme.colCardBorder : appTheme.cardBorderColor
    readonly property bool canMutateVersions: root.versionCheckoutEnabled && root.editorSession
                                              && root.editorSession.canEdit
                                              && !root.draftSubmitPending
    readonly property real listContentY: versionList ? versionList.contentY : 0

    // Freeze scroll capture BEFORE any model mutation. ListView modelReset can
    // jump contentY to 0 synchronously; if _restoringContentY is still false,
    // onContentYChanged would overwrite _preservedContentY with 0.
    function captureListScroll() {
        if (!versionList)
            return
        if (!root._restoringContentY)
            root._preservedContentY = versionList.contentY
        root._restoringContentY = true
    }

    function restoreListScroll() {
        if (!versionList)
            return
        root._restoringContentY = true
        Qt.callLater(function () {
            if (!versionList)
                return
            var maxY = Math.max(0, versionList.contentHeight - versionList.height)
            versionList.contentY = Math.max(0, Math.min(root._preservedContentY, maxY))
            root._restoringContentY = false
        })
    }

    function openCreateVersion() {
        if (!root.historyModel || root.draftSubmitPending)
            return
        if (root.draftVisible && !root.draftRenameMode) {
            draftFocusTimer.restart()
            return
        }
        root.draftRenameMode = false
        root.draftVersionId = ""
        root.draftOriginalText = qsTr("Version %1").arg(root.historyModel.versions.count + 1)
        versionNameField.text = root.draftOriginalText
        root.draftVisible = true
        draftFocusTimer.restart()
    }

    function openRenameVersion(versionId, displayName) {
        if (root.draftSubmitPending)
            return
        root.draftRenameMode = true
        root.draftVersionId = versionId
        root.draftOriginalText = displayName
        versionNameField.text = displayName
        root.draftVisible = true
        draftFocusTimer.restart()
    }

    function cancelDraft() {
        if (root.draftSubmitPending)
            return
        root.draftVisible = false
        root.draftRenameMode = false
        root.draftVersionId = ""
        root.draftOriginalText = ""
        versionNameField.text = ""
    }

    function commitDraft(requireChanged) {
        if (!root.draftVisible || !root.historyModel || root.draftSubmitPending)
            return
        var name = versionNameField.text.trim()
        if (name.length === 0) {
            cancelDraft()
            return
        }
        if (requireChanged && name === root.draftOriginalText.trim()) {
            cancelDraft()
            return
        }

        // Pending-save lock: keep the field, block duplicate submit.
        root.draftSubmitPending = true
        root.captureListScroll()
        if (root.draftRenameMode) {
            root.historyModel.renameVersion(root.draftVersionId, name)
        } else {
            root.historyModel.createRootVersion(name)
        }
        root.finishDraftAfterSubmit()
    }

    function finishDraftAfterSubmit() {
        // Synchronous paths finish immediately. Async SaveStarted keeps the
        // field open with draftSubmitPending until HistoryOperationFinished.
        if (!root.editorSession) {
            root.draftSubmitPending = false
            root.draftVisible = false
            root.draftRenameMode = false
            root.draftVersionId = ""
            root.draftOriginalText = ""
            versionNameField.text = ""
            root.restoreListScroll()
            return
        }
        var result = root.editorSession.lastHistoryResult || {}
        var action = String(result.action || "")
        var kind = Number(result.kind !== undefined ? result.kind : -1)
        var isVersionAction = action === "createRootVersion" || action === "renameVersion"
        // SaveStarted = 4 (EditorSessionResultKind::SaveStarted). Field stays
        // until a terminal HistoryOperationFinished.
        if (isVersionAction && kind === 4) {
            root.restoreListScroll()
            return
        }
        root.draftSubmitPending = false
        root.draftVisible = false
        root.draftRenameMode = false
        root.draftVersionId = ""
        root.draftOriginalText = ""
        versionNameField.text = ""
        root.restoreListScroll()
    }

    function checkoutVersionPreservingScroll(versionId) {
        if (!root.historyModel)
            return
        root.captureListScroll()
        root.historyModel.checkoutVersion(versionId)
        root.restoreListScroll()
    }

    function removeVersionPreservingScroll(versionId) {
        if (!root.historyModel)
            return
        root.captureListScroll()
        root.historyModel.removeVersion(versionId)
        root.restoreListScroll()
    }

    Timer {
        id: draftFocusTimer
        interval: 0
        onTriggered: {
            if (!root.draftVisible)
                return
            versionNameField.forceActiveFocus()
            versionNameField.selectAll()
        }
    }

    Connections {
        target: root.editorSession
        ignoreUnknownSignals: true
        function onHistoryOperationFinished() {
            if (!root.draftSubmitPending)
                return
            var result = root.editorSession.lastHistoryResult || {}
            var action = String(result.action || "")
            if (action !== "createRootVersion" && action !== "renameVersion")
                return
            var kind = Number(result.kind !== undefined ? result.kind : -1)
            // Keep waiting while a save checkpoint is still running.
            if (kind === 4)
                return
            root.draftSubmitPending = false
            root.draftVisible = false
            root.draftRenameMode = false
            root.draftVersionId = ""
            root.draftOriginalText = ""
            versionNameField.text = ""
            root.restoreListScroll()
        }
    }

    Connections {
        target: root.historyModel && root.historyModel.versions
                ? root.historyModel.versions : null
        ignoreUnknownSignals: true
        // Capture Y while the old layout is still valid, then freeze so a
        // synchronous contentY jump during reset cannot clobber the value.
        function onModelAboutToBeReset() {
            root.captureListScroll()
        }
        function onModelReset() {
            root.restoreListScroll()
        }
        function onCountChanged() {
            // Count may change without a full reset in some paths; still restore
            // if we already froze for an intentional mutation.
            if (root._restoringContentY)
                root.restoreListScroll()
        }
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
                enabled: root.canMutateVersions
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

        // Inline draft row under the Versions header (create + rename).
        Item {
            objectName: "editorVersionDraftRow"
            Layout.fillWidth: true
            Layout.preferredHeight: root.draftVisible
                                    ? (appTheme.spaceXl * 2 + appTheme.spaceSm
                                       + appTheme.spaceMd + appTheme.fontSizeCaption)
                                    : 0
            clip: true
            visible: root.draftVisible || height > 0
            opacity: root.draftVisible ? 1.0 : 0.0

            Behavior on Layout.preferredHeight {
                enabled: !appTheme.reduceMotion
                NumberAnimation {
                    duration: appTheme.reduceMotion ? 0 : appTheme.motionFoldOpenMs
                    easing.type: Easing.OutCubic
                }
            }

            ColumnLayout {
                anchors.fill: parent
                spacing: appTheme.spaceXs

                Label {
                    Layout.fillWidth: true
                    text: root.draftRenameMode ? qsTr("Rename Version") : qsTr("New Version")
                    color: root.colMuted
                    font.family: appTheme.uiFontFamily
                    font.pixelSize: appTheme.fontSizeCaption
                }

                RowLayout {
                    Layout.fillWidth: true
                    spacing: appTheme.spaceSm

                    TextField {
                        id: versionNameField
                        objectName: "editorVersionNameField"
                        Layout.fillWidth: true
                        Layout.preferredHeight: appTheme.spaceXl * 2 + appTheme.spaceSm
                        enabled: root.draftVisible && !root.draftSubmitPending
                        color: root.colText
                        font.family: appTheme.uiFontFamily
                        font.pixelSize: appTheme.fontSizeBody
                        placeholderText: qsTr("Version name")
                        selectByMouse: true
                        leftPadding: appTheme.spaceSm
                        rightPadding: appTheme.spaceSm
                        selectionColor: appTheme.editorListSelectedFillColor
                        selectedTextColor: appTheme.editorListSelectedInkColor
                        Accessible.name: root.draftRenameMode
                                         ? qsTr("Rename Version")
                                         : qsTr("New Version name")
                        background: Rectangle {
                            implicitHeight: appTheme.spaceXl * 2 + appTheme.spaceSm
                            radius: appTheme.controlRadiusSmall
                            color: appTheme.bgBaseColor
                            border.width: 1
                            border.color: versionNameField.activeFocus
                                          ? root.colText
                                          : root.colCardBorder
                            opacity: versionNameField.enabled ? 1.0 : 0.55
                        }

                        onAccepted: root.commitDraft(false)
                        Keys.onEscapePressed: function (event) {
                            root.cancelDraft()
                            event.accepted = true
                        }
                        // Focus loss commits only non-empty *changed* text.
                        // Double-defer so an Accept-button click (which also
                        // steals focus) can run commitDraft(false) first and
                        // close the draft before this path cancels an
                        // unchanged generated name.
                        onEditingFinished: {
                            Qt.callLater(function () {
                                Qt.callLater(function () {
                                    if (!root.draftVisible || root.draftSubmitPending)
                                        return
                                    root.commitDraft(true)
                                })
                            })
                        }
                    }

                    IconActionButton {
                        objectName: "editorVersionAcceptButton"
                        compact: true
                        enabled: root.draftVisible && !root.draftSubmitPending
                                 && versionNameField.text.trim().length > 0
                        iconSrc: "qrc:/panel_icons/plus.svg"
                        iconColorDefault: root.colText
                        iconColorMuted: root.colMuted
                        fillIdle: root.colCardSurface
                        fillHover: appTheme.buttonHoveredFillColor
                        fillPressed: appTheme.buttonPressedFillColor
                        fillSelected: appTheme.buttonSelectedFillColor
                        focusRingColor: root.colText
                        actionName: root.draftRenameMode
                                    ? qsTr("Accept Rename")
                                    : qsTr("Accept New Version")
                        onClicked: root.commitDraft(false)
                    }
                }
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
                // Avoid re-anchoring the viewport to a selected index on data-only
                // updates; outline selection never requires positionViewAtIndex.
                boundsBehavior: Flickable.StopAtBounds

                onContentYChanged: {
                    if (!root._restoringContentY)
                        root._preservedContentY = contentY
                }

                delegate: Rectangle {
                    id: versionCard
                    objectName: "editorVersionCard"
                    property string versionId: model.versionId
                    property string displayName: model.displayName
                    property string versionHead: model.headCommitHash
                    property bool versionActive: Boolean(model.active)
                    // Outline-only active chrome: card surface stays cardSurface;
                    // the 1 px border is the selection signal (white/text color).
                    property color selectionOutlineColor: versionActive
                                                          ? root.colText : root.colCardBorder
                    width: versionList.width
                    height: versionActive ? appTheme.spaceXl * 5 : appTheme.spaceXl * 4
                    radius: appTheme.controlRadiusSmall
                    color: root.colCardSurface
                    border.width: 1
                    border.color: selectionOutlineColor
                    Accessible.role: Accessible.ListItem
                    Accessible.name: displayName
                    Accessible.description: versionActive
                                            ? qsTr("Current head Version")
                                            : qsTr("Named Version")

                    MouseArea {
                        anchors.fill: parent
                        z: 0
                        enabled: root.versionCheckoutEnabled && !versionActive
                                 && !root.draftSubmitPending
                        onClicked: root.checkoutVersionPreservingScroll(versionId)
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
                            enabled: root.canMutateVersions
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
                                     && !root.draftSubmitPending
                            iconSrc: "qrc:/panel_icons/trash.svg"
                            iconColorDefault: root.colText
                            iconColorMuted: root.colMuted
                            fillIdle: root.colCardSurface
                            fillHover: appTheme.buttonHoveredFillColor
                            fillPressed: appTheme.buttonPressedFillColor
                            fillSelected: appTheme.buttonSelectedFillColor
                            focusRingColor: root.colText
                            actionName: qsTr("Remove Version")
                            onClicked: root.removeVersionPreservingScroll(versionId)
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
