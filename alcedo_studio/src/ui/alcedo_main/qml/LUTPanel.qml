import QtQuick
import QtQuick.Controls
import QtQuick.Controls.impl
import QtQuick.Layouts

// Standalone LUT browser panel (first-class adjustment navbar page).
// Visual identity: DESIGN.md — only appTheme tokens and shared IconActionButton.
// Selection does not rebuild the list or rewrite contentY when the row is visible.
Item {
    id: root
    objectName: "editorLutPanel"

    property var theme: null
    property var editorSession: null
    property var lutModel: null
    property bool controlsEnabled: true

    // ── Semantic colors (theme mirror → appTheme fallbacks only) ───────────
    readonly property color colText: theme ? theme.colText : appTheme.textColor
    readonly property color colMuted: theme ? theme.colTextMuted : appTheme.textMutedColor
    readonly property color colAccent: theme ? theme.colAccentPrimary : appTheme.accentColor
    readonly property color colCardSurface: theme ? theme.colCardSurface : appTheme.cardSurfaceColor
    readonly property color colCardBorder: theme ? theme.colCardBorder : appTheme.cardBorderColor
    readonly property color colBase: theme ? theme.colBgBase : appTheme.bgBaseColor
    readonly property color colHover: theme ? theme.colHover : appTheme.hoverColor
    // Monochrome inverted list selection (DESIGN.md editorList* tokens).
    readonly property color colSelectedFill: appTheme.editorListSelectedFillColor
    readonly property color colSelectedInk: appTheme.editorListSelectedInkColor
    readonly property color colFavoriteIdle: appTheme.editorListFavoriteIdleColor
    readonly property color colFavoriteActive: appTheme.editorListFavoriteActiveColor
    readonly property color colFavoriteIdleOnSelected: appTheme.editorListFavoriteIdleOnSelectedColor
    readonly property color colFavoriteActiveOnSelected: appTheme.editorListFavoriteActiveOnSelectedColor
    readonly property color colInvalid: appTheme.dangerColor
    readonly property color colInvalidTint: appTheme.dangerTintColor
    readonly property color colWarning: appTheme.accentColor
    readonly property color colWarningTint: appTheme.selectedTintColor
    // Type badge: white chip on dark rows; inverts with selected light well.
    readonly property color colBadgeFill: appTheme.editorSliderHandleColor
    readonly property color colBadgeInk: appTheme.editorListSelectedInkColor
    readonly property int panelRadius: theme ? theme.panelRadius : appTheme.panelRadius
    readonly property int controlRadius: theme ? theme.controlRadius : appTheme.controlRadius
    readonly property int controlRadiusSmall: appTheme.controlRadiusSmall
    readonly property int badgeRadius: appTheme.badgeRadius
    // Toolbar: one compact optical/source pair for every SVG action.
    readonly property int toolbarIconOptical: appTheme.iconOpticalSizeCompact
    readonly property int toolbarIconSource: appTheme.iconSourceSizeCompact
    readonly property color toolbarIconColor: appTheme.iconColor
    readonly property int toolbarChrome: Math.max(
        toolbarIconOptical + appTheme.spaceSm,
        appTheme.iconButtonHitSizeCompact - appTheme.spaceSm)
    // Unified row height (None and file rows share the same hit band).
    readonly property int entryRowHeight: appTheme.lineHeightBody
                                          + appTheme.lineHeightCaption
                                          + appTheme.spaceSm
    // Inset of the monochrome selected well from the sunken list track edge
    // (DESIGN.md listRowInset — never flush to the track border).
    readonly property int listRowInset: appTheme.spaceXs
    // Vertical gap between wells (track color shows through).
    readonly property int listRowGap: appTheme.spaceXs

    // Test/introspection surface (scroll contract).
    readonly property alias listView: lutView
    readonly property real listContentY: lutView ? lutView.contentY : 0
    readonly property int entryCount: lutEntries.length

    // ── Selection binding surface ──────────────────────────────────────────
    // CRITICAL: delegates must read this property (not call through a function
    // that only touches lutModel.selectedPath inside). QML does not always
    // track property deps inside JS functions, so selection highlight would
    // stick after workspace re-entry / snapshot load without this alias.
    readonly property string selectedPath: {
        if (!lutModel)
            return ""
        return String(lutModel.selectedPath || "")
    }
    readonly property string selectedPathNormalized: root.normalizePath(root.selectedPath)

    // ── Sort / filter UI state ─────────────────────────────────────────────
    property int sortField: 0        // 0 = Name, 1 = ModifiedTime
    property bool sortAscending: true
    property bool showFavoritesOnly: false

    // Sorted/filtered copy. Rebuilt on catalog / sort / filter / favorites-only
    // changes only — never on selection alone.
    property var lutEntries: []
    property real _pendingRestoreContentY: -1
    property bool _ensureSelectedIfOffscreen: false

    // Sliding monochrome selection chrome (content coordinates). Driven by an
    // explicit NumberAnimation on the chrome item — not Behavior + root binding
    // (that path restarts mid-flight and pays extra binding churn per frame).
    property int selectionWellIndex: -1
    property real selectionChromeOpacity: 0
    property int _selectionChromePrevIndex: -1

    function formatFileSize(bytes) {
        if (!bytes || bytes === 0)
            return ""
        if (bytes >= 1048576)
            return (bytes / 1048576).toFixed(1) + " MB"
        if (bytes >= 1024)
            return (bytes / 1024).toFixed(1) + " KB"
        return bytes + " B"
    }

    function formatCount() {
        var total = 0
        for (var i = 0; i < root.lutEntries.length; ++i) {
            var e = root.lutEntries[i]
            if (e && e.kind === "file")
                total++
        }
        if (total === 0)
            return qsTr("No LUTs found")
        if (total === 1)
            return qsTr("1 LUT found")
        return total + qsTr(" LUTs found")
    }

    /// Normalize path strings for selection compare (slash + case).
    function normalizePath(path) {
        return String(path || "").replace(/\\/g, "/").toLowerCase()
    }

    function indexOfSelectedEntry() {
        var selected = root.selectedPathNormalized
        for (var i = 0; i < root.lutEntries.length; ++i) {
            var entry = root.lutEntries[i]
            if (!entry)
                continue
            if (selected.length === 0 && entry.kind === "none")
                return i
            if (root.normalizePath(entry.path) === selected)
                return i
        }
        return -1
    }

    function selectionWellYForIndex(idx) {
        if (idx < 0)
            return 0
        return idx * (root.entryRowHeight + root.listRowGap)
    }

    /// True when the live catalog already lists this path (or None).
    function modelContainsPath(path) {
        if (!root.lutModel)
            return false
        var want = root.normalizePath(path)
        if (want.length === 0)
            return true
        var entries = root.lutModel.entries
        if (!entries)
            return false
        for (var i = 0; i < entries.length; ++i) {
            var e = entries[i]
            if (e && root.normalizePath(e.path) === want)
                return true
        }
        return false
    }

    /// Drive the sliding selection well. Nearby moves animate from the chrome's
    /// *current* y (not a root target binding). Long jumps snap + fade in.
    function syncSelectionChrome() {
        if (!selectionChrome)
            return
        var idx = root.indexOfSelectedEntry()
        // Same index: keep whatever animation is in flight; do not restart.
        if (idx === root.selectionWellIndex && idx >= 0
                && !selectionChromeSlide.running) {
            root.selectionChromeOpacity = 1
            return
        }
        root.selectionWellIndex = idx
        if (idx < 0) {
            selectionChromeSlide.stop()
            root.selectionChromeOpacity = 0
            root._selectionChromePrevIndex = -1
            return
        }
        var newY = root.selectionWellYForIndex(idx)
        var prev = root._selectionChromePrevIndex
        var fromY = selectionChrome.y
        var dist = Math.abs(newY - fromY)
        var viewH = lutView ? Math.max(lutView.height, 1) : 1
        var longJump = prev < 0 || dist > viewH
        selectionChromeSlide.stop()
        if (appTheme.reduceMotion || longJump) {
            selectionChrome.y = newY
            root.selectionChromeOpacity = 0
            selectionChromeFadeIn.restart()
        } else {
            // Short, linear-ish ease: OutCubic over 200ms on a busy GUI thread
            // (history submit + every row rebinding ink) reads as hitchy.
            selectionChromeSlide.from = fromY
            selectionChromeSlide.to = newY
            selectionChromeSlide.start()
            root.selectionChromeOpacity = 1
        }
        root._selectionChromePrevIndex = idx
    }

    /// True when this entry is the active selection. Callers in bindings must
    /// also touch `root.selectedPath` so the binding re-evaluates on change.
    function isPathSelected(path, kind) {
        // Establish dependency on the alias (not only the model pointer).
        var selected = root.selectedPathNormalized
        if (kind === "none")
            return selected.length === 0
        return root.normalizePath(path) === selected
    }

    /// Load-only: apply snapshot lut path without submitting. Same contract as
    /// Tone/Look loadFromSnapshot — used on image open, workspace re-entry,
    /// undo, and first bind. Always safe to call; no-op when model is null.
    /// Uses property assignment (not setSelectedPath()) so both production and
    /// QML-facing fakes resolve the WRITE without requiring Q_INVOKABLE.
    function loadFromSnapshot(snapshot) {
        if (!root.lutModel)
            return
        if (snapshot === undefined || snapshot === null) {
            root.lutModel.selectedPath = ""
            root.syncSelectionChrome()
            return
        }
        const entry = snapshot.lut !== undefined ? snapshot.lut : snapshot.ocio_lmt
        var path = ""
        if (entry === undefined || entry === null) {
            path = ""
        } else if (typeof entry === "string") {
            path = entry
        } else if (entry.ocio_lmt !== undefined) {
            path = String(entry.ocio_lmt)
        } else if (entry.path !== undefined) {
            path = String(entry.path)
        }
        // Load-only write. Do NOT refresh on every snapshot echo — that rebuilds
        // the catalog, reassigns lutEntries, and makes contentY hitch (抽搐).
        // Refresh only when the path is missing so a missing-current row can
        // materialize.
        root.lutModel.selectedPath = path
        if (path.length > 0 && !root.modelContainsPath(path)
                && typeof root.lutModel.refresh === "function") {
            root.lutModel.refresh(false)
        }
        root.syncSelectionChrome()
    }

    function entriesFingerprint(list) {
        if (!list || !list.length)
            return ""
        var s = ""
        for (var i = 0; i < list.length; ++i) {
            var e = list[i]
            if (!e) {
                s += "?;"
                continue
            }
            s += String(e.kind || "") + "|" + String(e.path || "") + ";"
        }
        return s
    }

    /// Rebuild sorted/filtered entries.
    /// @param ensureSelectedIfOffscreen only for sort/filter/favorites structural
    ///        changes. Selection clicks must pass false / omit — no contentY move.
    function rebuildEntries(ensureSelectedIfOffscreen) {
        var preserveY = lutView ? lutView.contentY : 0
        var source = root.lutModel ? root.lutModel.entries : []
        if (!source || !source.length) {
            if (root.lutEntries.length !== 0)
                root.lutEntries = []
            root._pendingRestoreContentY = -1
            root._ensureSelectedIfOffscreen = false
            root.syncSelectionChrome()
            return
        }

        var filtered = []
        for (var i = 0; i < source.length; ++i) {
            var entry = source[i]
            if (root.showFavoritesOnly && entry.kind === "file") {
                if (!root.lutModel.isFavoritePath(entry.path))
                    continue
            }
            filtered.push(entry)
        }

        if (root.sortField === 0) {
            filtered.sort(function(a, b) {
                if (a.kind === "none" && b.kind !== "none") return -1
                if (a.kind !== "none" && b.kind === "none") return 1
                var cmp = String(a.displayName || "").localeCompare(
                    String(b.displayName || ""), undefined, { sensitivity: "base" })
                return root.sortAscending ? cmp : -cmp
            })
        } else {
            filtered.sort(function(a, b) {
                if (a.kind === "none" && b.kind !== "none") return -1
                if (a.kind !== "none" && b.kind === "none") return 1
                var ak = a.modifiedTimeSortKey || 0
                var bk = b.modifiedTimeSortKey || 0
                return root.sortAscending ? (ak - bk) : (bk - ak)
            })
        }

        // Skip ListView model reassignment when membership/order is unchanged —
        // reassigning the JS array rebuilds delegates and hitches contentY.
        if (root.entriesFingerprint(filtered) === root.entriesFingerprint(root.lutEntries)) {
            root.syncSelectionChrome()
            return
        }

        root._pendingRestoreContentY = preserveY
        root._ensureSelectedIfOffscreen = !!ensureSelectedIfOffscreen
        root.lutEntries = filtered
        Qt.callLater(root.applyScrollAfterRebuild)
    }

    function applyScrollAfterRebuild() {
        if (!lutView)
            return
        var maxY = Math.max(0, lutView.contentHeight - lutView.height)
        if (root._pendingRestoreContentY >= 0) {
            lutView.contentY = Math.max(0, Math.min(root._pendingRestoreContentY, maxY))
            root._pendingRestoreContentY = -1
        }
        root.syncSelectionChrome()
        if (!root._ensureSelectedIfOffscreen)
            return
        root._ensureSelectedIfOffscreen = false
        root.ensureSelectedVisibleIfOffscreen()
    }

    /// Scroll only when the selected row is fully outside the viewport.
    function ensureSelectedVisibleIfOffscreen() {
        if (!lutView || !root.lutModel)
            return
        var idx = root.indexOfSelectedEntry()
        if (idx < 0)
            return
        var item = lutView.itemAtIndex(idx)
        if (item) {
            var top = item.y
            var bottom = item.y + item.height
            var viewTop = lutView.contentY
            var viewBottom = lutView.contentY + lutView.height
            if (bottom <= viewTop)
                lutView.contentY = top
            else if (top >= viewBottom)
                lutView.contentY = Math.max(0, bottom - lutView.height)
            return
        }
        lutView.positionViewAtIndex(idx, ListView.Visible)
    }

    onSortFieldChanged: root.rebuildEntries(true)
    onSortAscendingChanged: root.rebuildEntries(true)
    onShowFavoritesOnlyChanged: root.rebuildEntries(true)
    // Loader / stack may assign lutModel after construction.
    onLutModelChanged: {
        root.rebuildEntries(false)
        // Model late-bind: pull selection from the current session snapshot.
        if (root.lutModel && root.editorSession)
            root.loadFromSnapshot(root.editorSession.adjustmentSnapshot)
    }
    onEditorSessionChanged: {
        if (root.lutModel && root.editorSession)
            root.loadFromSnapshot(root.editorSession.adjustmentSnapshot)
    }

    Connections {
        target: root.lutModel
        function onEntriesChanged() {
            root.rebuildEntries(false)
        }
        function onSelectedPathChanged() {
            // Selection-only: move chrome, never rebuild the list.
            root.syncSelectionChrome()
        }
        function onFavoritePathsChanged() {
            if (root.showFavoritesOnly)
                root.rebuildEntries(true)
        }
    }

    NumberAnimation {
        id: selectionChromeFadeIn
        target: root
        property: "selectionChromeOpacity"
        to: 1
        duration: appTheme.reduceMotion ? 0 : appTheme.motionFadeMs
        easing.type: Easing.OutQuad
    }

    NumberAnimation {
        id: selectionChromeSlide
        target: selectionChrome
        property: "y"
        // Snappier than motionFoldOpenMs: the slide shares the GUI thread with
        // selectPath→submit and N× delegate ink rebinds.
        duration: appTheme.reduceMotion ? 0 : 140
        easing.type: Easing.OutQuad
    }

    Component.onCompleted: {
        root.rebuildEntries(false)
        if (root.lutModel && root.editorSession)
            root.loadFromSnapshot(root.editorSession.adjustmentSnapshot)
    }

    // ── Layout ─────────────────────────────────────────────────────────────

    ColumnLayout {
        anchors.fill: parent
        spacing: appTheme.spaceSm

        Text {
            objectName: "editorLutPanelTitle"
            Layout.fillWidth: true
            text: qsTr("LUT")
            color: root.colText
            font.family: appTheme.uiFontFamily
            font.pixelSize: appTheme.fontSizeSection
            font.weight: appTheme.fontWeightHeading
        }

        // Sunken toolbar track (bgBase inset inside card family).
        Rectangle {
            objectName: "editorLutToolbar"
            Layout.fillWidth: true
            Layout.preferredHeight: root.toolbarChrome + appTheme.spaceXs * 2
            radius: root.controlRadiusSmall
            color: root.colBase
            border.width: 1
            border.color: root.colCardBorder

            RowLayout {
                anchors.fill: parent
                anchors.margins: appTheme.spaceXs / 2
                spacing: appTheme.spaceXs

                // Filter field
                RowLayout {
                    Layout.fillWidth: true
                    Layout.preferredHeight: root.toolbarChrome
                    Layout.leftMargin: appTheme.spaceXs
                    spacing: appTheme.spaceXs

                    ColorImage {
                        Layout.preferredWidth: root.toolbarIconOptical
                        Layout.preferredHeight: root.toolbarIconOptical
                        source: "qrc:/panel_icons/search.svg"
                        sourceSize.width: root.toolbarIconSource
                        sourceSize.height: root.toolbarIconSource
                        color: root.toolbarIconColor
                        opacity: 0.85
                    }

                    Item {
                        Layout.fillWidth: true
                        Layout.fillHeight: true

                        TextInput {
                            id: filterInput
                            objectName: "editorLutFilterInput"
                            anchors.fill: parent
                            verticalAlignment: TextInput.AlignVCenter
                            color: root.colText
                            font.family: appTheme.uiFontFamily
                            font.pixelSize: appTheme.fontSizeCaption
                            font.weight: appTheme.fontWeightRegular
                            clip: true
                            onTextChanged: {
                                if (root.lutModel)
                                    root.lutModel.filterText = text
                            }
                        }

                        Text {
                            anchors.fill: filterInput
                            verticalAlignment: Text.AlignVCenter
                            visible: filterInput.text.length === 0 && !filterInput.activeFocus
                            text: qsTr("Filter LUTs")
                            color: root.colMuted
                            font.family: appTheme.uiFontFamily
                            font.pixelSize: appTheme.fontSizeCaption
                            font.weight: appTheme.fontWeightRegular
                        }
                    }
                }

                // One chrome recipe for every toolbar SVG (optical/source/tint).
                component LutToolbarButton: IconActionButton {
                    compact: true
                    showHoverFill: true
                    showFocusRing: true
                    iconColorDefault: root.toolbarIconColor
                    iconColorMuted: root.colMuted
                    fillIdle: root.colBase
                    fillHover: appTheme.buttonHoveredFillColor
                    fillSelected: appTheme.buttonSelectedFillColor
                    Layout.preferredWidth: root.toolbarChrome
                    Layout.preferredHeight: root.toolbarChrome
                    width: root.toolbarChrome
                    height: root.toolbarChrome
                }

                Item {
                    id: sortBtnHost
                    Layout.preferredWidth: root.toolbarChrome
                    Layout.preferredHeight: root.toolbarChrome
                    width: root.toolbarChrome
                    height: root.toolbarChrome

                    LutToolbarButton {
                        anchors.fill: parent
                        objectName: "editorLutSortButton"
                        iconSrc: "qrc:/panel_icons/sort.svg"
                        actionName: qsTr("Sort options")
                        onClicked: sortPopup.open()
                    }

                    Popup {
                        id: sortPopup
                        objectName: "editorLutSortPopup"
                        y: parent.height + appTheme.spaceXs
                        width: appTheme.editorSidePanelWidthMin - appTheme.spaceLg * 2
                        padding: appTheme.spaceXs
                        closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
                        background: Rectangle {
                            color: root.colCardSurface
                            border.width: 1
                            border.color: root.colCardBorder
                            radius: root.controlRadiusSmall
                        }

                        ColumnLayout {
                            spacing: appTheme.spaceXs / 2

                            Text {
                                Layout.fillWidth: true
                                Layout.leftMargin: appTheme.spaceSm
                                Layout.rightMargin: appTheme.spaceSm
                                Layout.topMargin: appTheme.spaceXs
                                text: qsTr("Sort by")
                                color: root.colMuted
                                font.family: appTheme.uiFontFamily
                                font.pixelSize: appTheme.fontSizeCaption
                                font.weight: appTheme.fontWeightStrong
                            }

                            Repeater {
                                model: [
                                    { label: qsTr("Name"), field: 0 },
                                    { label: qsTr("Modified time"), field: 1 }
                                ]
                                delegate: Rectangle {
                                    Layout.fillWidth: true
                                    Layout.preferredHeight: appTheme.lineHeightBody
                                                             + appTheme.spaceSm
                                    radius: root.badgeRadius
                                    color: root.sortField === modelData.field
                                           ? root.colSelectedFill
                                           : (sortHover.containsMouse
                                              ? appTheme.buttonHoveredFillColor
                                              : root.colCardSurface)

                                    Text {
                                        anchors.left: parent.left
                                        anchors.leftMargin: appTheme.spaceSm
                                        anchors.verticalCenter: parent.verticalCenter
                                        text: modelData.label
                                        color: root.sortField === modelData.field
                                               ? root.colSelectedInk : root.colText
                                        font.family: appTheme.uiFontFamily
                                        font.pixelSize: appTheme.fontSizeCaption
                                        font.weight: root.sortField === modelData.field
                                                     ? appTheme.fontWeightStrong
                                                     : appTheme.fontWeightRegular
                                    }

                                    Text {
                                        anchors.right: parent.right
                                        anchors.rightMargin: appTheme.spaceSm
                                        anchors.verticalCenter: parent.verticalCenter
                                        text: root.sortField === modelData.field
                                              ? (root.sortAscending ? "▲" : "▼") : ""
                                        color: root.colSelectedInk
                                        font.family: appTheme.uiFontFamily
                                        font.pixelSize: appTheme.fontSizeCaption
                                        font.weight: appTheme.fontWeightStrong
                                    }

                                    MouseArea {
                                        id: sortHover
                                        anchors.fill: parent
                                        hoverEnabled: true
                                        cursorShape: Qt.PointingHandCursor
                                        onClicked: {
                                            if (root.sortField === modelData.field) {
                                                root.sortAscending = !root.sortAscending
                                            } else {
                                                root.sortField = modelData.field
                                                root.sortAscending = true
                                            }
                                            sortPopup.close()
                                        }
                                    }
                                }
                            }
                        }
                    }
                }

                LutToolbarButton {
                    objectName: "editorLutFavoritesFilterButton"
                    selected: root.showFavoritesOnly
                    iconSrc: "qrc:/panel_icons/star.svg"
                    actionName: root.showFavoritesOnly
                                ? qsTr("Show all LUTs")
                                : qsTr("Show favorites only")
                    // Same optical size as siblings; gold only when filter is on.
                    iconColorDefault: root.showFavoritesOnly
                                      ? root.colFavoriteActive
                                      : root.toolbarIconColor
                    onClicked: root.showFavoritesOnly = !root.showFavoritesOnly
                }

                Rectangle {
                    Layout.preferredWidth: 1
                    Layout.preferredHeight: appTheme.iconOpticalSizeCompact
                    color: root.colCardBorder
                }

                LutToolbarButton {
                    objectName: "editorLutRefreshButton"
                    iconSrc: "qrc:/panel_icons/retry.svg"
                    actionName: qsTr("Refresh LUT catalog")
                    onClicked: {
                        if (root.lutModel)
                            root.lutModel.refresh(true)
                    }
                }

                LutToolbarButton {
                    objectName: "editorLutOpenFolderButton"
                    Layout.rightMargin: appTheme.spaceXs
                    enabled: root.lutModel ? root.lutModel.canOpenDirectory : false
                    iconSrc: "qrc:/panel_icons/folder-open.svg"
                    actionName: qsTr("Open LUT folder")
                    onClicked: {
                        if (root.lutModel && root.lutModel.directoryPath())
                            Qt.openUrlExternally("file:///" + root.lutModel.directoryPath())
                    }
                }
            }
        }

        // Sunken entry list.
        Rectangle {
            Layout.fillWidth: true
            Layout.fillHeight: true
            radius: root.controlRadiusSmall
            color: root.colBase
            border.width: 1
            border.color: root.colCardBorder
            clip: true

            ListView {
                id: lutView
                objectName: "editorLutListView"
                anchors.fill: parent
                anchors.margins: root.listRowInset
                model: root.lutEntries
                spacing: root.listRowGap
                boundsBehavior: Flickable.StopAtBounds
                flickableDirection: Flickable.VerticalFlick
                pressDelay: 0
                // height is 0 before Layout settles — never assign a negative buffer.
                cacheBuffer: Math.max(0, Math.ceil(height)) * 2
                ScrollBar.vertical: ScrollBar {
                    policy: ScrollBar.AsNeeded
                    padding: 0
                }

                // Single sliding selected well under rows (not per-delegate fill).
                // Parent is contentItem so y scrolls with the list. y is owned by
                // selectionChromeSlide (not a binding) so frames stay independent
                // of root property writes during the move.
                Rectangle {
                    id: selectionChrome
                    objectName: "editorLutSelectionChrome"
                    parent: lutView.contentItem
                    z: 0
                    width: lutView.width
                    height: root.entryRowHeight
                    x: 0
                    y: 0
                    radius: root.badgeRadius
                    color: root.colSelectedFill
                    opacity: root.selectionChromeOpacity
                    visible: root.selectionWellIndex >= 0
                    enabled: false
                    // Cache the well as a texture so each animation frame is a
                    // transform/composite, not a full path rebuild of the fill.
                    layer.enabled: true
                    layer.smooth: true
                }

                // Outer Item owns ListView cell geometry; track margins + row
                // gap keep wells off the sunken track edge.
                delegate: Item {
                    id: entryDelegate
                    required property var modelData
                    // ListView index — keep required so recycling stays stable.
                    required property int index
                    readonly property bool isNone: modelData.kind === "none"
                    readonly property bool isFile: modelData.kind === "file"
                    readonly property bool isMissing: modelData.kind === "missing"
                    // Read root.selectedPath in-binding so highlight tracks
                    // setSelectedPath / workspace re-entry without a list rebuild.
                    readonly property bool entrySelected: {
                        var _dep = root.selectedPath
                        return root.isPathSelected(modelData.path, modelData.kind)
                    }
                    readonly property bool entryValid: modelData.valid !== false
                    readonly property bool entrySelectable: modelData.selectable !== false
                    readonly property string entryPath: String(modelData.path || "")
                    readonly property string entryKey: entryPath.length > 0
                                                      ? entryPath
                                                      : String(modelData.kind || "none")
                    readonly property bool entryFav: {
                        var paths = root.lutModel ? root.lutModel.favoritePaths : []
                        return entryPath.length > 0 && paths && paths.indexOf(entryPath) >= 0
                    }
                    // Ink/badge invert with the sliding well; fill is not painted
                    // on the row (selectionChrome owns the light bar).
                    readonly property bool showSelectedWell: entrySelected && entrySelectable
                    readonly property color titleColor: {
                        if (showSelectedWell)
                            return root.colSelectedInk
                        if (!entryValid)
                            return root.colInvalid
                        return root.colText
                    }
                    readonly property color secondaryColor: {
                        if (showSelectedWell)
                            return root.colSelectedInk
                        if (!entryValid)
                            return root.colInvalid
                        return root.colMuted
                    }
                    readonly property color starColor: {
                        if (showSelectedWell)
                            return entryFav ? root.colFavoriteActiveOnSelected
                                            : root.colFavoriteIdleOnSelected
                        return entryFav ? root.colFavoriteActive : root.colFavoriteIdle
                    }
                    // Type badge: white chip on dark rows; ink chip on selected.
                    readonly property color badgeFill: {
                        if (showSelectedWell)
                            return root.colSelectedInk
                        return root.colBadgeFill
                    }
                    readonly property color badgeTextColor: {
                        if (showSelectedWell)
                            return root.colSelectedFill
                        return root.colBadgeInk
                    }
                    readonly property color badgeBorderColor: {
                        if (showSelectedWell)
                            return root.colSelectedInk
                        return root.colCardBorder
                    }

                    objectName: "editorLutEntry"
                    width: lutView.width
                    height: root.entryRowHeight
                    // Above the sliding selection chrome.
                    z: 1

                    Rectangle {
                        id: entryWell
                        objectName: "editorLutEntryWell"
                        anchors.fill: parent
                        radius: root.badgeRadius
                        color: entryHover.containsMouse && entrySelectable && !showSelectedWell
                               ? appTheme.buttonHoveredFillColor
                               : "transparent"
                        border.width: 0

                        MouseArea {
                            id: entryHover
                            objectName: "editorLutEntryHit"
                            anchors.fill: parent
                            enabled: entrySelectable
                            hoverEnabled: true
                            cursorShape: Qt.PointingHandCursor
                            onClicked: {
                                if (root.lutModel)
                                    root.lutModel.selectPath(entryPath)
                            }
                        }

                        RowLayout {
                            anchors.fill: parent
                            anchors.leftMargin: appTheme.spaceSm
                            anchors.rightMargin: appTheme.spaceSm
                            anchors.topMargin: appTheme.spaceXs / 2
                            anchors.bottomMargin: appTheme.spaceXs / 2
                            spacing: appTheme.spaceXs
                            z: 1

                        // Favorite star: glyph only, no hover well.
                        Item {
                            visible: isFile || isMissing
                            Layout.preferredWidth: root.toolbarIconOptical
                            Layout.preferredHeight: root.toolbarIconOptical

                            ColorImage {
                                anchors.centerIn: parent
                                width: root.toolbarIconOptical
                                height: root.toolbarIconOptical
                                source: "qrc:/panel_icons/star.svg"
                                sourceSize.width: root.toolbarIconSource
                                sourceSize.height: root.toolbarIconSource
                                color: entryDelegate.starColor
                            }
                            MouseArea {
                                id: favStarMouse
                                objectName: "editorLutFavoriteStar"
                                anchors.fill: parent
                                hoverEnabled: true
                                cursorShape: Qt.PointingHandCursor
                                onClicked: {
                                    if (root.lutModel)
                                        root.lutModel.toggleFavoritePath(entryPath)
                                }
                            }
                        }

                        // Spacer keeps "None" title aligned with file titles
                        // when the star column is hidden.
                        Item {
                            visible: isNone
                            Layout.preferredWidth: root.toolbarIconOptical
                            Layout.preferredHeight: root.toolbarIconOptical
                        }

                        ColumnLayout {
                            Layout.fillWidth: true
                            Layout.alignment: Qt.AlignVCenter
                            spacing: 0

                            Text {
                                Layout.fillWidth: true
                                text: modelData.displayName || ""
                                color: entryDelegate.titleColor
                                font.family: appTheme.uiFontFamily
                                font.pixelSize: appTheme.fontSizeBody
                                // Weight stays constant: flipping Strong/Regular on
                                // select forces text re-layout on two rows while the
                                // chrome is sliding (main-thread hitch).
                                font.weight: appTheme.fontWeightRegular
                                elide: Text.ElideRight
                            }

                            Text {
                                Layout.fillWidth: true
                                text: modelData.secondaryText || ""
                                color: entryDelegate.secondaryColor
                                font.family: appTheme.uiFontFamily
                                font.pixelSize: appTheme.fontSizeCaption
                                font.weight: appTheme.fontWeightRegular
                                elide: Text.ElideRight
                                visible: text.length > 0 && !isNone
                            }
                        }

                        Rectangle {
                            visible: isFile && modelData.lutTypeBadge
                                     && modelData.lutTypeBadge.length > 0
                            Layout.preferredHeight: appTheme.lineHeightCaption + appTheme.spaceXs
                            implicitWidth: badgeText.implicitWidth + appTheme.spaceSm
                            radius: root.badgeRadius
                            color: entryDelegate.badgeFill
                            border.width: 1
                            border.color: entryDelegate.badgeBorderColor

                            Text {
                                id: badgeText
                                anchors.centerIn: parent
                                text: modelData.lutTypeBadge || ""
                                color: entryDelegate.badgeTextColor
                                font.family: appTheme.dataFontFamily
                                font.pixelSize: appTheme.fontSizeCaption
                                font.weight: appTheme.fontWeightRegular
                            }
                        }

                        Text {
                            visible: isFile && root.formatFileSize(modelData.fileSize).length > 0
                            text: root.formatFileSize(modelData.fileSize)
                            color: entryDelegate.secondaryColor
                            font.family: appTheme.dataFontFamily
                            font.pixelSize: appTheme.fontSizeCaption
                            font.weight: appTheme.fontWeightRegular
                        }

                        Rectangle {
                            visible: (isMissing || (!entryValid && isFile))
                                     && modelData.statusText && modelData.statusText.length > 0
                            Layout.preferredHeight: appTheme.lineHeightCaption + appTheme.spaceXs
                            implicitWidth: statusText.implicitWidth + appTheme.spaceSm
                            radius: root.badgeRadius
                            color: isMissing ? root.colWarningTint : root.colInvalidTint
                            border.width: 1
                            border.color: isMissing ? root.colWarning : root.colInvalid

                            Text {
                                id: statusText
                                anchors.centerIn: parent
                                text: modelData.statusText || ""
                                color: isMissing ? root.colWarning : root.colInvalid
                                font.family: appTheme.uiFontFamily
                                font.pixelSize: appTheme.fontSizeCaption
                                font.weight: appTheme.fontWeightRegular
                            }
                        }
                        } // RowLayout
                    } // entryWell
                } // entryDelegate
            } // lutView
        } // sunken list track

        RowLayout {
            Layout.fillWidth: true
            Layout.preferredHeight: appTheme.lineHeightCaption
            spacing: appTheme.spaceXs

            Text {
                objectName: "editorLutCountText"
                Layout.fillWidth: true
                text: root.formatCount()
                color: root.colMuted
                font.family: appTheme.uiFontFamily
                font.pixelSize: appTheme.fontSizeCaption
                font.weight: appTheme.fontWeightRegular
            }

            Text {
                objectName: "editorLutStatusText"
                text: root.lutModel ? root.lutModel.statusText : ""
                color: root.colMuted
                font.family: appTheme.uiFontFamily
                font.pixelSize: appTheme.fontSizeCaption
                font.weight: appTheme.fontWeightRegular
                elide: Text.ElideRight
                visible: text.length > 0
            }
        }
    }
}
