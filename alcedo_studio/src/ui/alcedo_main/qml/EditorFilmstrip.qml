import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Bottom filmstrip dock for EditorWorkspace.
// Phase 7C: the editor filmstrip reuses the library thumbnail model while
// keeping its own horizontal selection and focus surface.
// Motion / surface tokens: DESIGN.md.
Item {
    id: root
    objectName: "editorFilmstrip"

    property var theme: null
    property var editorSession: null
    // InteractionPolicyController is the authoritative save-checkpoint gate.
    property var interactionPolicy: null
    property bool collapsed: editorSession ? editorSession.filmstripCollapsed : false
    property real expandedHeight: editorSession ? editorSession.filmstripExpandedHeight : 128
    readonly property var libraryModule: (typeof appModules !== "undefined" && appModules)
                                         ? appModules.library : null
    readonly property var thumbnailModel: libraryModule ? libraryModule.thumbnailModel : null
    readonly property var workspaceRouter: (typeof appModules !== "undefined" && appModules)
                                           ? appModules.workspaceRouter : null
    readonly property int selectedElementId: editorSession ? Number(editorSession.elementId) : 0
    readonly property int selectedIndex: thumbnailModel && selectedElementId > 0
                                        ? thumbnailModel.rowByElementId(selectedElementId) : -1
    readonly property int currentIndex: selectedIndex >= 0 ? selectedIndex + 1 : 0
    readonly property int totalCount: thumbnailModel ? Number(thumbnailModel.count) : 0
    readonly property bool saving: editorSession
                                    && (String(editorSession.sessionState) === "Saving"
                                        || String(editorSession.sessionState) === "Switching")
    readonly property bool renderBusy: editorSession ? Boolean(editorSession.renderBusy) : false
    readonly property bool discardEligible: editorSession
                                             ? Boolean(editorSession.canDiscardCurrentCommit)
                                             : false
    readonly property int filmstripThumbnailMaxEdge: 512
    property int focusIndex: selectedIndex >= 0 ? selectedIndex : 0
    property bool _listHadFocus: false
    property bool _restoringScroll: false
    property int contextMenuElementId: 0
    readonly property bool selectionEnabled: interactionPolicy
                                             ? Boolean(interactionPolicy.canSelectEditorImage)
                                             : true
    readonly property string selectionDisabledReason: interactionPolicy
                                                     ? String(interactionPolicy.selectEditorImageReason || "")
                                                     : ""
    property bool hasImage: editorSession ? editorSession.hasImage : false

    readonly property real handleHeight: 28
    // dockExpandProgress drives the downward fold (0 collapsed -> 1 expanded).
    // collapsed flips immediately (persisted session state); only the visual
    // height animates so the handle stays stationary and state assertions hold.
    // foldManualDrive + driveFoldProgress() pin intermediate geometry for tests.
    property real dockExpandProgress: 0
    property bool foldManualDrive: false
    property bool _motionArmed: false
    property int _foldDuration: appTheme.motionFoldOpenMs
    readonly property real dockHeight: handleHeight
                                       + (expandedHeight - handleHeight) * dockExpandProgress
    readonly property color colPanel: theme ? theme.colGlassPanel : "#1C1C1D"
    readonly property color colStroke: theme ? theme.colGlassStroke : Qt.rgba(1, 1, 1, 0.08)
    readonly property color colText: theme ? theme.colText : "#F5F1EA"
    readonly property color colMuted: theme ? theme.colTextMuted : "#AAA59D"
    readonly property color colAccent: theme ? theme.colAccentPrimary : "#457B9D"
    readonly property color colHover: theme ? theme.colHover : Qt.rgba(1, 1, 1, 0.07)
    readonly property color colCardSurface: theme ? theme.colCardSurface : "#161719"
    readonly property color colCardBorder: theme ? theme.colCardBorder : Qt.rgba(1, 1, 1, 0.08)
    readonly property int panelRadius: theme ? theme.panelRadius : 12

    function storeFilmstripScroll() {
        if (!_restoringScroll && editorSession && filmstripListView) {
            editorSession.filmstripScrollPosition = Math.max(0, filmstripListView.contentX)
        }
    }

    function restoreFilmstripScroll() {
        if (!editorSession || !filmstripListView) {
            return
        }
        const maxX = Math.max(0, filmstripListView.contentWidth - filmstripListView.width)
        const savedX = Math.max(0, Number(editorSession.filmstripScrollPosition || 0))
        _restoringScroll = true
        filmstripListView.contentX = Math.min(savedX, maxX)
        _restoringScroll = false
    }

    function scheduleScrollRestore() {
        scrollRestoreTimer.restart()
    }

    function restoreFocusAfterFold() {
        if (collapsed) {
            collapseHandle.forceActiveFocus()
        } else if (_listHadFocus) {
            filmstripListView.forceActiveFocus()
        }
    }

    function focusCurrentIndex(index) {
        if (totalCount <= 0) {
            focusIndex = -1
            return
        }
        focusIndex = Math.max(0, Math.min(totalCount - 1, index))
    }

    function moveFocus(delta) {
        if (totalCount <= 0) {
            return
        }
        focusCurrentIndex(focusIndex + delta)
        filmstripListView.forceActiveFocus()
        filmstripListView.positionViewAtIndex(focusIndex, ListView.Contain)
    }

    function activateFocused() {
        activateImage(focusIndex)
    }

    function driveFoldProgress(value) {
        foldManualDrive = true
        dockExpandProgress = Math.max(0, Math.min(1, value))
    }

    function endFoldDrive() {
        foldManualDrive = false
        dockExpandProgress = collapsed ? 0 : 1
    }

    onCollapsedChanged: {
        if (collapsed) {
            _listHadFocus = filmstripListView ? filmstripListView.activeFocus : false
        }
        _foldDuration = collapsed ? appTheme.motionFoldCloseMs : appTheme.motionFoldOpenMs
        if (!foldManualDrive) {
            dockExpandProgress = collapsed ? 0 : 1
        }
        focusRestoreTimer.restart()
    }
    onSelectedIndexChanged: {
        if (selectedIndex >= 0) {
            focusIndex = selectedIndex
        }
        scheduleScrollRestore()
    }
    onTotalCountChanged: {
        if (totalCount <= 0) {
            focusIndex = -1
        } else if (focusIndex < 0 || focusIndex >= totalCount) {
            focusIndex = Math.max(0, Math.min(totalCount - 1, selectedIndex))
        }
        scheduleScrollRestore()
    }
    Component.onCompleted: {
        // Snap to the persisted collapse state on load (no open animation).
        dockExpandProgress = collapsed ? 0 : 1
        _motionArmed = true
    }
    Behavior on dockExpandProgress {
        enabled: root._motionArmed && !root.foldManualDrive
        NumberAnimation {
            duration: appTheme.reduceMotion ? 0 : root._foldDuration
            easing.type: Easing.OutCubic
        }
    }

    Timer {
        id: scrollRestoreTimer
        interval: 0
        repeat: false
        onTriggered: root.restoreFilmstripScroll()
    }

    Timer {
        id: focusRestoreTimer
        interval: 0
        repeat: false
        onTriggered: root.restoreFocusAfterFold()
    }

    signal expandRequested()
    signal collapseRequested()
    signal toggleRequested()
    signal imageActivated(int index)

    function activateImage(index) {
        if (!selectionEnabled || !thumbnailModel || index < 0 || index >= totalCount) {
            return
        }
        const row = thumbnailModel.getItemAt(index)
        if (!row || Number(row.elementId) <= 0 || Number(row.imageId) <= 0) {
            return
        }
        focusCurrentIndex(index)
        filmstripListView.forceActiveFocus()
        if (workspaceRouter && workspaceRouter.openEditor) {
            workspaceRouter.openEditor(Number(row.elementId), Number(row.imageId))
        }
        root.imageActivated(index)
    }

    function openContextMenu(index) {
        if (!thumbnailModel || index < 0 || index >= totalCount) {
            return
        }
        const row = thumbnailModel.getItemAt(index)
        if (!row || Number(row.elementId) !== selectedElementId) {
            return
        }
        contextMenuElementId = Number(row.elementId)
        filmstripMenu.open()
    }

    function discardCurrentCommit() {
        if (contextMenuElementId !== selectedElementId || !discardEligible || !editorSession) {
            return
        }
        if (editorSession.Discard) {
            editorSession.Discard()
        }
    }

    // Layout.preferredHeight binds to dockHeight so collapse releases vertical space
    // to the viewport without destroying the filmstrip identity or model later.
    implicitHeight: dockHeight
    focus: false
    Accessible.role: Accessible.Pane
    Accessible.name: qsTr("Editor filmstrip")
    Accessible.description: collapsed
                            ? qsTr("Collapsed filmstrip handle")
                            : qsTr("Expanded filmstrip dock")

    function toggleCollapsed() {
        if (!editorSession) {
            return
        }
        editorSession.filmstripCollapsed = !editorSession.filmstripCollapsed
        if (editorSession.filmstripCollapsed) {
            collapseRequested()
        } else {
            expandRequested()
        }
        toggleRequested()
    }

    function focusHandle() {
        collapseHandle.forceActiveFocus()
    }

    Rectangle {
        anchors.fill: parent
        radius: root.panelRadius
        color: root.colCardSurface
        border.width: 1
        border.color: root.colCardBorder
        clip: true

        // Persistent focusable handle — remains keyboard- and pointer-accessible
        // when the dock is collapsed so the released height returns to the viewport.
        Item {
            id: collapseHandle
            objectName: "editorFilmstripHandle"
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: parent.top
            height: root.handleHeight
            focus: true
            activeFocusOnTab: true
            Accessible.role: Accessible.Button
            Accessible.name: root.collapsed ? qsTr("Expand filmstrip") : qsTr("Collapse filmstrip")
            Accessible.description: qsTr("Image %1 of %2").arg(Math.max(0, root.currentIndex))
                                                         .arg(Math.max(0, root.totalCount))
            Accessible.onPressAction: root.toggleCollapsed()

            Keys.onPressed: function(event) {
                if (event.key === Qt.Key_Space || event.key === Qt.Key_Return
                        || event.key === Qt.Key_Enter) {
                    root.toggleCollapsed()
                    event.accepted = true
                } else if (event.key === Qt.Key_Up && root.collapsed) {
                    root.toggleCollapsed()
                    event.accepted = true
                } else if (event.key === Qt.Key_Down && !root.collapsed) {
                    root.toggleCollapsed()
                    event.accepted = true
                }
            }

            Rectangle {
                anchors.fill: parent
                color: handleMouse.containsMouse || collapseHandle.activeFocus
                       ? root.colHover
                       : "transparent"
            }

            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: appTheme.spaceMd
                anchors.rightMargin: appTheme.spaceMd
                spacing: appTheme.spaceMd

                Canvas {
                    id: chevron
                    Layout.preferredWidth: 14
                    Layout.preferredHeight: 14
                    Layout.alignment: Qt.AlignVCenter
                    antialiasing: true
                    onPaint: {
                        const ctx = getContext("2d")
                        ctx.clearRect(0, 0, width, height)
                        ctx.strokeStyle = root.colMuted
                        ctx.lineWidth = 1.5
                        ctx.lineCap = "round"
                        ctx.lineJoin = "round"
                        ctx.beginPath()
                        if (root.collapsed) {
                            ctx.moveTo(3, 9)
                            ctx.lineTo(7, 5)
                            ctx.lineTo(11, 9)
                        } else {
                            ctx.moveTo(3, 5)
                            ctx.lineTo(7, 9)
                            ctx.lineTo(11, 5)
                        }
                        ctx.stroke()
                    }
                    Connections {
                        target: root
                        function onCollapsedChanged() { chevron.requestPaint() }
                    }
                }

                Label {
                    Layout.fillWidth: true
                    Layout.alignment: Qt.AlignVCenter
                    text: {
                        if (root.totalCount <= 0) {
                            return qsTr("No images in filmstrip")
                        }
                        return qsTr("%1 / %2").arg(Math.max(1, root.currentIndex)).arg(root.totalCount)
                    }
                    color: root.colText
                    font.pixelSize: appTheme.fontSizeBody
                    font.weight: appTheme.fontWeightStrong
                    elide: Text.ElideRight
                }

                Label {
                    visible: !root.selectionEnabled
                             && root.selectionDisabledReason.length > 0
                    Layout.alignment: Qt.AlignVCenter
                    Layout.maximumWidth: 180
                    text: root.selectionDisabledReason
                    color: root.colAccent
                    font.pixelSize: appTheme.fontSizeCaption
                    elide: Text.ElideRight
                }
            }

            MouseArea {
                id: handleMouse
                anchors.fill: parent
                hoverEnabled: true
                cursorShape: Qt.PointingHandCursor
                onClicked: root.toggleCollapsed()
            }
        }

        // The body is clipped during the fold. The ListView keeps its expanded
        // delegate geometry even at zero visible body height, so thumbnail pins
        // are released only when a delegate is genuinely destroyed.
        Item {
            id: filmstripBody
            objectName: "editorFilmstripBody"
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: collapseHandle.bottom
            anchors.bottom: parent.bottom
            visible: root.dockExpandProgress > 0.001
            opacity: root.dockExpandProgress
            clip: true

            Label {
                anchors.centerIn: parent
                visible: root.totalCount <= 0
                text: qsTr("No images")
                color: root.colMuted
                font.pixelSize: appTheme.fontSizeBody
            }

            ListView {
                id: filmstripListView
                objectName: "editorFilmstripListView"
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.top: parent.top
                anchors.leftMargin: appTheme.spaceSm
                anchors.rightMargin: appTheme.spaceSm
                anchors.topMargin: appTheme.spaceXs
                height: Math.max(1, root.expandedHeight - root.handleHeight
                                    - appTheme.spaceSm - appTheme.spaceXs)
                orientation: ListView.Horizontal
                model: root.thumbnailModel
                spacing: appTheme.spaceSm
                clip: true
                cacheBuffer: 0
                boundsBehavior: Flickable.StopAtBounds
                interactive: true
                focus: true
                activeFocusOnTab: true
                keyNavigationEnabled: false

                onContentXChanged: root.storeFilmstripScroll()
                onContentWidthChanged: root.scheduleScrollRestore()
                onWidthChanged: root.scheduleScrollRestore()
                onCountChanged: root.scheduleScrollRestore()

                Keys.onPressed: function(event) {
                    if (event.key === Qt.Key_Left) {
                        root.moveFocus(-1)
                        event.accepted = true
                    } else if (event.key === Qt.Key_Right) {
                        root.moveFocus(1)
                        event.accepted = true
                    } else if (event.key === Qt.Key_Home) {
                        root.focusCurrentIndex(0)
                        filmstripListView.positionViewAtIndex(root.focusIndex, ListView.Contain)
                        event.accepted = true
                    } else if (event.key === Qt.Key_End) {
                        root.focusCurrentIndex(root.totalCount - 1)
                        filmstripListView.positionViewAtIndex(root.focusIndex, ListView.Contain)
                        event.accepted = true
                    } else if (event.key === Qt.Key_Return || event.key === Qt.Key_Enter
                               || event.key === Qt.Key_Space) {
                        root.activateFocused()
                        event.accepted = true
                    }
                }

                Component.onCompleted: root.scheduleScrollRestore()

                delegate: Item {
                    id: thumbnailDelegate
                    objectName: "editorFilmstripTile"
                    required property int index
                    required property int elementId
                    required property int imageId
                    required property string fileName
                    required property string thumbUrl
                    required property bool thumbLoading
                    required property bool thumbMissingSource
                    required property string thumbErrorText

                    property string liveThumbUrl: thumbUrl
                    property bool liveThumbLoading: thumbLoading
                    property bool liveThumbMissingSource: thumbMissingSource
                    property string liveThumbErrorText: thumbErrorText
                    property int pinnedElementId: 0
                    property int pinnedImageId: 0
                    property int pinnedMaxEdge: 0

                    readonly property bool isSelected: Number(elementId) === root.selectedElementId
                    readonly property bool hasFocusFrame: root.focusIndex === index
                                                               && filmstripListView.activeFocus
                    readonly property bool thumbnailReady: liveThumbUrl.length > 0
                    readonly property bool thumbnailProblemState: !thumbnailReady
                                                                    && !liveThumbLoading
                                                                    && (liveThumbMissingSource
                                                                        || liveThumbErrorText.length > 0)
                    readonly property string thumbnailProblemText: liveThumbErrorText.length > 0
                                                                   ? liveThumbErrorText
                                                                   : qsTr("Source file was moved or deleted")

                    width: Math.max(appTheme.spaceXl * 6, height * 1.55)
                    height: ListView.view ? ListView.view.height : 1
                    Accessible.role: Accessible.ListItem
                    Accessible.name: fileName.length > 0 ? fileName
                                                         : qsTr("Image %1").arg(index + 1)
                    Accessible.description: isSelected
                                            ? qsTr("Current image")
                                            : qsTr("Open image")

                    function releasePinnedThumbnail() {
                        if (pinnedElementId !== 0 && pinnedImageId !== 0 && root.libraryModule) {
                            root.libraryModule.SetThumbnailVisible(pinnedElementId, pinnedImageId,
                                                                   false, pinnedMaxEdge)
                        }
                        pinnedElementId = 0
                        pinnedImageId = 0
                        pinnedMaxEdge = 0
                    }

                    function bindThumbnailLifetime() {
                        liveThumbUrl = thumbUrl
                        liveThumbLoading = thumbLoading
                        liveThumbMissingSource = thumbMissingSource
                        liveThumbErrorText = thumbErrorText
                        if (pinnedElementId === elementId && pinnedImageId === imageId
                                && pinnedMaxEdge === root.filmstripThumbnailMaxEdge) {
                            return
                        }
                        releasePinnedThumbnail()
                        pinnedElementId = Number(elementId)
                        pinnedImageId = Number(imageId)
                        pinnedMaxEdge = root.filmstripThumbnailMaxEdge
                        if (pinnedElementId !== 0 && pinnedImageId !== 0 && root.libraryModule) {
                            root.libraryModule.SetThumbnailVisible(pinnedElementId, pinnedImageId,
                                                                   true, pinnedMaxEdge)
                        }
                    }

                    function releaseThumbnailBinding() {
                        releasePinnedThumbnail()
                    }

                    onThumbUrlChanged: liveThumbUrl = thumbUrl
                    onThumbLoadingChanged: liveThumbLoading = thumbLoading
                    onThumbMissingSourceChanged: liveThumbMissingSource = thumbMissingSource
                    onThumbErrorTextChanged: liveThumbErrorText = thumbErrorText
                    Component.onCompleted: bindThumbnailLifetime()
                    onElementIdChanged: bindThumbnailLifetime()
                    onImageIdChanged: bindThumbnailLifetime()
                    Component.onDestruction: releaseThumbnailBinding()

                    Rectangle {
                        id: tile
                        objectName: "editorFilmstripTileSurface"
                        anchors.fill: parent
                        radius: appTheme.controlRadiusSmall
                        color: thumbnailMouse.containsMouse
                               ? appTheme.hoverColor
                               : (thumbnailDelegate.isSelected
                                  ? appTheme.selectedTintColor : appTheme.cardSurfaceColor)
                        border.width: thumbnailDelegate.isSelected ? 2 : 1
                        border.color: thumbnailDelegate.isSelected
                                     ? appTheme.accentColor : appTheme.cardBorderColor

                        Rectangle {
                            anchors.fill: parent
                            anchors.margins: appTheme.spaceXs
                            radius: appTheme.controlRadiusSmall
                            color: appTheme.bgBaseColor
                            border.width: 1
                            border.color: appTheme.dividerColor

                            Image {
                                id: thumbnailImage
                                anchors.fill: parent
                                anchors.margins: appTheme.spaceXs
                                source: thumbnailDelegate.liveThumbUrl
                                sourceSize.width: root.filmstripThumbnailMaxEdge
                                sourceSize.height: root.filmstripThumbnailMaxEdge
                                asynchronous: true
                                fillMode: Image.PreserveAspectFit
                                visible: thumbnailDelegate.thumbnailReady
                            }

                            BusyIndicator {
                                anchors.centerIn: parent
                                width: appTheme.iconOpticalSize
                                height: appTheme.iconOpticalSize
                                visible: thumbnailDelegate.liveThumbLoading
                                running: visible
                            }

                            Label {
                                anchors.centerIn: parent
                                width: parent.width - appTheme.spaceLg
                                text: thumbnailDelegate.thumbnailProblemState
                                      ? thumbnailDelegate.thumbnailProblemText
                                      : qsTr("No thumbnail")
                                visible: !thumbnailDelegate.thumbnailReady
                                         && !thumbnailDelegate.liveThumbLoading
                                color: thumbnailDelegate.thumbnailProblemState
                                       ? appTheme.dangerColor : appTheme.textMutedColor
                                font.pixelSize: appTheme.fontSizeCaption
                                horizontalAlignment: Text.AlignHCenter
                                wrapMode: Text.WordWrap
                                maximumLineCount: 2
                                elide: Text.ElideRight
                            }
                        }

                        Rectangle {
                            anchors.left: parent.left
                            anchors.right: parent.right
                            anchors.bottom: parent.bottom
                            height: Math.max(appTheme.spaceLg, appTheme.fontSizeBody
                                             + appTheme.spaceXs)
                            color: appTheme.bgDeepColor
                            opacity: 0.92
                            visible: thumbnailDelegate.fileName.length > 0

                            Label {
                                anchors.fill: parent
                                anchors.leftMargin: appTheme.spaceSm
                                anchors.rightMargin: appTheme.spaceSm
                                text: thumbnailDelegate.fileName
                                color: appTheme.textColor
                                font.pixelSize: appTheme.fontSizeCaption
                                elide: Text.ElideRight
                                verticalAlignment: Text.AlignVCenter
                            }
                        }

                        Row {
                            anchors.left: parent.left
                            anchors.top: parent.top
                            anchors.margins: appTheme.spaceSm
                            spacing: appTheme.spaceXs
                            visible: thumbnailDelegate.isSelected
                                     && (root.saving || root.renderBusy)

                            Rectangle {
                                objectName: "editorFilmstripSavingBadge"
                                visible: root.saving
                                width: savingLabel.implicitWidth + appTheme.spaceSm
                                height: savingLabel.implicitHeight + appTheme.spaceXs
                                radius: appTheme.badgeRadius
                                color: appTheme.accentSecondaryColor
                                Label {
                                    id: savingLabel
                                    anchors.centerIn: parent
                                    text: qsTr("Saving")
                                    color: appTheme.textColor
                                    font.pixelSize: appTheme.fontSizeCaption
                                    font.weight: appTheme.fontWeightStrong
                                }
                            }

                            Rectangle {
                                objectName: "editorFilmstripRenderBadge"
                                visible: root.renderBusy
                                width: renderLabel.implicitWidth + appTheme.spaceSm
                                height: renderLabel.implicitHeight + appTheme.spaceXs
                                radius: appTheme.badgeRadius
                                color: appTheme.selectedTintColor
                                Label {
                                    id: renderLabel
                                    anchors.centerIn: parent
                                    text: qsTr("Rendering")
                                    color: appTheme.textColor
                                    font.pixelSize: appTheme.fontSizeCaption
                                    font.weight: appTheme.fontWeightStrong
                                }
                            }
                        }

                        Rectangle {
                            anchors.fill: parent
                            radius: appTheme.controlRadiusSmall
                            color: "transparent"
                            border.width: 2
                            border.color: appTheme.accentSecondaryColor
                            visible: thumbnailDelegate.hasFocusFrame
                        }

                        MouseArea {
                            id: thumbnailMouse
                            anchors.fill: parent
                            acceptedButtons: Qt.LeftButton | Qt.RightButton
                            hoverEnabled: true
                            cursorShape: Qt.PointingHandCursor
                            onClicked: function(mouse) {
                                if (mouse.button === Qt.LeftButton) {
                                    root.activateImage(thumbnailDelegate.index)
                                } else if (mouse.button === Qt.RightButton) {
                                    root.openContextMenu(thumbnailDelegate.index)
                                }
                            }
                        }
                    }

                    Connections {
                        target: root.libraryModule
                        ignoreUnknownSignals: true
                        function onThumbnailUpdated(updatedElementId, updatedUrl, loading,
                                                    missingSource, errorText) {
                            if (Number(updatedElementId) === Number(thumbnailDelegate.elementId)) {
                                thumbnailDelegate.liveThumbUrl = updatedUrl || ""
                                thumbnailDelegate.liveThumbLoading = Boolean(loading)
                                thumbnailDelegate.liveThumbMissingSource = Boolean(missingSource)
                                thumbnailDelegate.liveThumbErrorText = errorText || ""
                            }
                        }
                    }
                }
            }
        }

        Menu {
            id: filmstripMenu
            objectName: "editorFilmstripContextMenu"
            MenuItem {
                objectName: "editorFilmstripDiscardAction"
                text: qsTr("Discard")
                enabled: root.contextMenuElementId === root.selectedElementId
                         && root.discardEligible
                onTriggered: root.discardCurrentCommit()
            }
        }
    }
}
