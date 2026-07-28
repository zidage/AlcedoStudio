pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Effects
import QtQuick.Layouts

Dialog {
    id: dialog
    objectName: "adjustmentTransferDialog"
    font.family: appTheme.uiFontFamily

    parent: Overlay.overlay
    modal: true
    focus: true
    closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
    padding: 0
    width: Math.min(parent ? parent.width - appTheme.spaceXl * 2
                           : appTheme.editorSidePanelWidthMax + appTheme.editorSidePanelWidth,
                    appTheme.editorSidePanelWidthMax + appTheme.editorSidePanelWidth)
    height: Math.min(parent ? parent.height - appTheme.spaceXl * 2
                            : appTheme.editorSidePanelWidthMax,
                     appTheme.editorSidePanelWidthMax)
    x: parent ? Math.round((parent.width - width) / 2) : 0
    y: parent ? Math.round((parent.height - height) / 2) : 0

    property string mode: "copy"
    property string pasteStrategy: "paste"
    property string sourceTitle: ""
    property string selectedSourceVersionId: ""
    property int targetCount: 0
    property var sourceVersions: []
    property var adjustmentRows: []
    property Item blurSource: null
    property real cornerRadius: 0
    property var expandedSections: ({})
    property int expandedSectionsRevision: 0

    signal copyAccepted(var selectedKeys, string versionId)
    signal pasteAccepted(string strategy)
    signal pasteDiscarded()

    readonly property bool copyMode: mode === "copy"
    readonly property int selectedCount: {
        let count = 0
        for (let index = 0; index < adjustmentRows.length; ++index) {
            if (adjustmentRows[index] && adjustmentRows[index].checked === true) {
                ++count
            }
        }
        return count
    }
    readonly property var displayRows: {
        const revision = expandedSectionsRevision
        return buildDisplayRows()
    }

    function selectedKeys() {
        const keys = []
        for (let index = 0; index < adjustmentRows.length; ++index) {
            const row = adjustmentRows[index]
            if (row && row.checked === true) {
                keys.push(String(row.key))
            }
        }
        return keys
    }

    function restoreListScroll(contentY) {
        Qt.callLater(function() {
            const minY = parameterList.originY
            const maxY = Math.max(minY, parameterList.contentHeight - parameterList.height)
            parameterList.contentY = Math.max(minY, Math.min(contentY, maxY))
        })
    }

    function setRowsPreservingScroll(rows) {
        const contentY = parameterList.contentY
        adjustmentRows = rows
        restoreListScroll(contentY)
    }

    function setRowChecked(index, checked) {
        if (index < 0 || index >= adjustmentRows.length) {
            return
        }
        const next = adjustmentRows.slice()
        const row = Object.assign({}, next[index])
        row.checked = checked
        next[index] = row
        setRowsPreservingScroll(next)
    }

    function setAllRowsChecked(checked) {
        const next = []
        let changed = false
        for (let index = 0; index < adjustmentRows.length; ++index) {
            const row = Object.assign({}, adjustmentRows[index])
            if (row.checked !== checked) {
                row.checked = checked
                changed = true
            }
            next.push(row)
        }
        if (changed) {
            setRowsPreservingScroll(next)
        }
    }

    function sectionExpanded(section, ordinal) {
        if (expandedSections[section] !== undefined) {
            return expandedSections[section] === true
        }
        return ordinal < 2
    }

    function toggleSection(section, ordinal) {
        const contentY = parameterList.contentY
        const next = Object.assign({}, expandedSections)
        next[section] = !sectionExpanded(section, ordinal)
        expandedSections = next
        ++expandedSectionsRevision
        restoreListScroll(contentY)
    }

    function buildDisplayRows() {
        const result = []
        const sections = []
        const grouped = ({})
        for (let index = 0; index < adjustmentRows.length; ++index) {
            const row = adjustmentRows[index] || ({})
            const section = String(row.section || qsTr("Other"))
            if (!grouped[section]) {
                grouped[section] = []
                sections.push(section)
            }
            grouped[section].push({
                kind: "parameter",
                sourceIndex: index,
                key: row.key,
                label: row.label,
                value: row.value,
                checked: row.checked === true
            })
        }
        for (let sectionIndex = 0; sectionIndex < sections.length; ++sectionIndex) {
            const section = sections[sectionIndex]
            const expanded = sectionExpanded(section, sectionIndex)
            result.push({
                kind: "section",
                section: section,
                ordinal: sectionIndex,
                expanded: expanded
            })
            if (expanded) {
                result.push(...grouped[section])
            }
        }
        return result
    }

    function selectSourceVersion(versionRow) {
        if (!versionRow) {
            return
        }
        selectedSourceVersionId = String(versionRow.versionId || "")
        expandedSections = ({})
        ++expandedSectionsRevision
        adjustmentRows = versionRow.items || []
        parameterList.positionViewAtBeginning()
    }

    function versionTimeText(seconds) {
        if (!seconds || Number(seconds) <= 0) {
            return qsTr("Imported")
        }
        return Qt.formatDateTime(new Date(Number(seconds) * 1000), Locale.ShortFormat)
    }

    function titleText() {
        return copyMode ? qsTr("Copy Adjustments") : qsTr("Paste Adjustments")
    }

    function acceptText() {
        if (!copyMode) {
            return qsTr("Paste Adjustments")
        }
        return qsTr("Copy %1 Settings").arg(selectedCount)
    }

    onOpened: {
        expandedSections = ({})
        ++expandedSectionsRevision
    }

    Overlay.modal: Item {
        anchors.fill: parent

        Rectangle {
            id: backdropMask
            anchors.fill: parent
            radius: dialog.cornerRadius
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
                maskEnabled: dialog.cornerRadius > 0
                maskSource: backdropMask
            }

            MultiEffect {
                anchors.fill: parent
                source: dialog.blurSource
                blurEnabled: dialog.blurSource !== null
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

    background: Rectangle {
        radius: appTheme.panelRadius
        color: appTheme.cardSurfaceColor
        border.width: 1
        border.color: appTheme.cardBorderColor
    }

    contentItem: Rectangle {
        color: appTheme.cardSurfaceColor
        radius: appTheme.panelRadius
        clip: true

        ColumnLayout {
            anchors.fill: parent
            spacing: 0

            // ── Header bar ──────────────────────────────────────────────────
            Rectangle {
                Layout.fillWidth: true
                Layout.preferredHeight: appTheme.iconButtonHitSize + appTheme.spaceMd
                color: appTheme.cardSurfaceColor

                RowLayout {
                    anchors.fill: parent
                    anchors.leftMargin: appTheme.spaceLg
                    anchors.rightMargin: appTheme.spaceSm
                    spacing: appTheme.spaceSm

                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: 0

                        Label {
                            Layout.fillWidth: true
                            text: dialog.titleText()
                            color: appTheme.textColor
                            font.family: appTheme.uiFontFamily
                            font.pixelSize: 18
                            font.weight: 800
                            elide: Text.ElideRight
                        }

                        Label {
                            Layout.fillWidth: true
                            visible: dialog.sourceTitle.length > 0
                            text: dialog.sourceTitle
                            color: appTheme.textMutedColor
                            font.family: appTheme.uiFontFamily
                            font.pixelSize: appTheme.fontSizeCaption
                            elide: Text.ElideMiddle
                        }
                    }

                    Item {
                        id: closeButton
                        objectName: "adjustmentTransferCloseButton"
                        Layout.preferredWidth: appTheme.iconButtonHitSize
                        Layout.preferredHeight: appTheme.iconButtonHitSize
                        activeFocusOnTab: true
                        Accessible.role: Accessible.Button
                        Accessible.name: qsTr("Close")
                        Accessible.onPressAction: dialog.reject()

                        Keys.onPressed: function(event) {
                            if (event.key === Qt.Key_Space || event.key === Qt.Key_Return
                                    || event.key === Qt.Key_Enter) {
                                dialog.reject()
                                event.accepted = true
                            }
                        }

                        Rectangle {
                            anchors.centerIn: parent
                            width: appTheme.iconButtonHitSize - appTheme.spaceSm
                            height: width
                            radius: appTheme.controlRadiusSmall
                            color: closeMouse.containsMouse
                                   ? appTheme.buttonHoveredFillColor
                                   : appTheme.buttonIdleFillColor
                            border.width: closeButton.activeFocus ? 1 : 0
                            border.color: Qt.rgba(appTheme.accentColor.r,
                                                  appTheme.accentColor.g,
                                                  appTheme.accentColor.b, 0.60)
                        }

                        Label {
                            anchors.centerIn: parent
                            text: "×"
                            color: closeMouse.containsMouse
                                   ? appTheme.textColor : appTheme.textMutedColor
                            font.family: appTheme.uiFontFamily
                            font.pixelSize: appTheme.fontSizeTitle
                            horizontalAlignment: Text.AlignHCenter
                            verticalAlignment: Text.AlignVCenter
                        }

                        MouseArea {
                            id: closeMouse
                            anchors.fill: parent
                            hoverEnabled: true
                            cursorShape: Qt.PointingHandCursor
                            onClicked: dialog.reject()
                        }
                    }
                }
            }

            // ── Central workspace ─────────────────────────────────────────────
            RowLayout {
                Layout.fillWidth: true
                Layout.fillHeight: true
                spacing: 0

                // Left pane: source versions (copy mode only).
                Rectangle {
                    Layout.preferredWidth: dialog.copyMode ? appTheme.editorSidePanelWidth : 0
                    Layout.fillHeight: true
                    visible: dialog.copyMode
                    color: appTheme.cardSurfaceColor

                    ColumnLayout {
                        anchors.fill: parent
                        spacing: 0

                        Label {
                            Layout.fillWidth: true
                            Layout.leftMargin: appTheme.spaceMd
                            Layout.rightMargin: appTheme.spaceMd
                            Layout.topMargin: appTheme.spaceSm
                            Layout.bottomMargin: appTheme.spaceSm
                            text: qsTr("Source Versions")
                            color: appTheme.textMutedColor
                            font.family: appTheme.uiFontFamily
                            font.pixelSize: appTheme.fontSizeCaption
                            font.weight: appTheme.fontWeightStrong
                        }

                        Rectangle {
                            Layout.fillWidth: true
                            Layout.fillHeight: true
                            Layout.leftMargin: appTheme.spaceSm
                            Layout.rightMargin: appTheme.spaceSm
                            Layout.bottomMargin: appTheme.spaceSm
                            radius: appTheme.controlRadiusSmall
                            color: appTheme.bgBaseColor
                            border.width: 1
                            border.color: appTheme.cardBorderColor
                            clip: true

                            ListView {
                                id: versionList
                                anchors.fill: parent
                                anchors.margins: appTheme.spaceXs
                                model: dialog.sourceVersions
                                spacing: appTheme.spaceXs
                                boundsBehavior: Flickable.StopAtBounds
                                reuseItems: true
                                currentIndex: -1

                                delegate: Item {
                                    id: versionDelegate
                                    required property int index
                                    required property var modelData
                                    width: ListView.view ? ListView.view.width : 0
                                    height: appTheme.iconButtonHitSize + appTheme.spaceSm
                                    activeFocusOnTab: true
                                    Accessible.role: Accessible.ListItem
                                    Accessible.name: String(modelData.displayName || "")

                                    readonly property bool selected:
                                        String(modelData.versionId || "")
                                        === dialog.selectedSourceVersionId

                                    Keys.onPressed: function(event) {
                                        if (event.key === Qt.Key_Space || event.key === Qt.Key_Return
                                                || event.key === Qt.Key_Enter) {
                                            dialog.selectSourceVersion(versionDelegate.modelData)
                                            event.accepted = true
                                        }
                                    }

                                    Rectangle {
                                        anchors.fill: parent
                                        radius: appTheme.badgeRadius
                                        color: versionDelegate.selected
                                               ? "transparent"
                                               : (versionMouse.containsMouse
                                                  ? appTheme.buttonHoveredFillColor
                                                  : "transparent")
                                        border.width: versionDelegate.selected || versionDelegate.activeFocus ? 1 : 0
                                        border.color: versionDelegate.selected
                                                      ? appTheme.textColor
                                                      : appTheme.accentColor
                                    }

                                    RowLayout {
                                        anchors.fill: parent
                                        anchors.leftMargin: appTheme.spaceSm
                                        anchors.rightMargin: appTheme.spaceSm
                                        spacing: appTheme.spaceSm

                                        Rectangle {
                                            Layout.preferredWidth: appTheme.iconButtonHitSizeCompact
                                            Layout.preferredHeight: appTheme.iconButtonHitSizeCompact
                                            radius: appTheme.controlRadiusSmall
                                            color: appTheme.cardSurfaceColor

                                            Label {
                                                anchors.centerIn: parent
                                                text: String(versionDelegate.modelData.displayName
                                                             || "V").slice(0, 1).toUpperCase()
                                                color: versionDelegate.selected
                                                       ? appTheme.textColor
                                                       : appTheme.textMutedColor
                                                font.family: appTheme.uiFontFamily
                                                font.pixelSize: appTheme.fontSizeBody
                                                font.weight: appTheme.fontWeightHeading
                                            }
                                        }

                                        ColumnLayout {
                                            Layout.fillWidth: true
                                            spacing: 0

                                            Label {
                                                Layout.fillWidth: true
                                                text: String(versionDelegate.modelData.displayName || "")
                                                color: appTheme.textColor
                                                font.family: appTheme.uiFontFamily
                                                font.pixelSize: appTheme.fontSizeBody
                                                font.weight: appTheme.fontWeightStrong
                                                elide: Text.ElideRight
                                            }

                                            Label {
                                                Layout.fillWidth: true
                                                text: versionDelegate.modelData.active
                                                      ? qsTr("Active · %1").arg(dialog.versionTimeText(
                                                              versionDelegate.modelData.updatedAt))
                                                      : dialog.versionTimeText(
                                                            versionDelegate.modelData.updatedAt)
                                                color: versionDelegate.modelData.active
                                                       ? appTheme.accentColor
                                                       : appTheme.textMutedColor
                                                font.family: appTheme.dataFontFamily
                                                font.pixelSize: appTheme.fontSizeCaption
                                                elide: Text.ElideRight
                                            }
                                        }
                                    }

                                    MouseArea {
                                        id: versionMouse
                                        anchors.fill: parent
                                        hoverEnabled: true
                                        cursorShape: Qt.PointingHandCursor
                                        onClicked: dialog.selectSourceVersion(versionDelegate.modelData)
                                    }
                                }
                            }
                        }
                    }
                }

                // Right pane: parameters.
                Rectangle {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    color: appTheme.cardSurfaceColor

                    ColumnLayout {
                        anchors.fill: parent
                        spacing: 0

                        Rectangle {
                            Layout.fillWidth: true
                            Layout.preferredHeight: appTheme.iconButtonHitSize
                            color: appTheme.cardSurfaceColor

                            RowLayout {
                                anchors.fill: parent
                                anchors.leftMargin: appTheme.spaceLg
                                anchors.rightMargin: appTheme.spaceLg
                                spacing: appTheme.spaceSm

                                Label {
                                    Layout.fillWidth: true
                                    text: dialog.copyMode ? qsTr("Parameters to Copy")
                                                          : qsTr("Parameters to Paste")
                                    color: appTheme.textMutedColor
                                    font.family: appTheme.uiFontFamily
                                    font.pixelSize: appTheme.fontSizeCaption
                                    font.weight: appTheme.fontWeightStrong
                                }

                                Label {
                                    visible: dialog.copyMode
                                    text: qsTr("Select All")
                                    color: selectAllMouse.containsMouse
                                           ? appTheme.textColor : appTheme.textMutedColor
                                    font.family: appTheme.uiFontFamily
                                    font.pixelSize: appTheme.fontSizeCaption
                                    font.weight: appTheme.fontWeightStrong
                                    opacity: selectAllMouse.enabled ? 1.0 : 0.4

                                    MouseArea {
                                        id: selectAllMouse
                                        anchors.fill: parent
                                        hoverEnabled: true
                                        cursorShape: Qt.PointingHandCursor
                                        enabled: dialog.adjustmentRows.length > 0
                                                 && dialog.selectedCount < dialog.adjustmentRows.length
                                        onClicked: dialog.setAllRowsChecked(true)
                                    }
                                }

                                Label {
                                    visible: dialog.copyMode
                                    text: "·"
                                    color: appTheme.dividerColor
                                    font.family: appTheme.uiFontFamily
                                    font.pixelSize: appTheme.fontSizeCaption
                                }

                                Label {
                                    visible: dialog.copyMode
                                    text: qsTr("None")
                                    color: noneMouse.containsMouse
                                           ? appTheme.textColor : appTheme.textMutedColor
                                    font.family: appTheme.uiFontFamily
                                    font.pixelSize: appTheme.fontSizeCaption
                                    font.weight: appTheme.fontWeightStrong
                                    opacity: noneMouse.enabled ? 1.0 : 0.4

                                    MouseArea {
                                        id: noneMouse
                                        anchors.fill: parent
                                        hoverEnabled: true
                                        cursorShape: Qt.PointingHandCursor
                                        enabled: dialog.selectedCount > 0
                                        onClicked: dialog.setAllRowsChecked(false)
                                    }
                                }
                            }
                        }

                        Rectangle {
                            Layout.fillWidth: true
                            Layout.fillHeight: true
                            Layout.leftMargin: appTheme.spaceSm
                            Layout.rightMargin: appTheme.spaceSm
                            Layout.topMargin: appTheme.spaceSm
                            Layout.bottomMargin: appTheme.spaceSm
                            radius: appTheme.controlRadiusSmall
                            color: appTheme.bgBaseColor
                            border.width: 1
                            border.color: appTheme.cardBorderColor
                            clip: true

                            ListView {
                                id: parameterList
                                anchors.fill: parent
                                anchors.margins: appTheme.spaceXs
                                model: dialog.displayRows
                                boundsBehavior: Flickable.StopAtBounds
                                reuseItems: true

                                delegate: Loader {
                                    id: rowLoader
                                    required property int index
                                    required property var modelData
                                    width: ListView.view ? ListView.view.width : 0
                                    height: modelData.kind === "section"
                                            ? appTheme.iconButtonHitSize
                                            : appTheme.iconButtonHitSizeCompact
                                    sourceComponent: modelData.kind === "section"
                                                     ? sectionDelegate
                                                     : parameterDelegate
                                    onLoaded: item.rowData = modelData
                                    onModelDataChanged: if (item) item.rowData = modelData
                                }
                            }
                        }
                    }
                }
            }

            // ── Footer bar ───────────────────────────────────────────────────
            Rectangle {
                Layout.fillWidth: true
                Layout.preferredHeight: appTheme.iconButtonHitSize + appTheme.spaceLg
                color: appTheme.cardSurfaceColor

                RowLayout {
                    anchors.fill: parent
                    anchors.leftMargin: appTheme.spaceLg
                    anchors.rightMargin: appTheme.spaceLg
                    spacing: appTheme.spaceSm

                    Label {
                        Layout.fillWidth: true
                        text: dialog.copyMode
                              ? qsTr("%1 of %2 settings selected")
                                    .arg(dialog.selectedCount).arg(dialog.adjustmentRows.length)
                              : qsTr("%1 settings · %2 target images")
                                    .arg(dialog.adjustmentRows.length).arg(dialog.targetCount)
                        color: appTheme.textMutedColor
                        font.family: appTheme.uiFontFamily
                        font.pixelSize: appTheme.fontSizeCaption
                        elide: Text.ElideRight
                    }

                    DialogActionButton {
                        objectName: "adjustmentTransferCancelButton"
                        buttonWidth: appTheme.spaceXl * 5
                        buttonHeight: appTheme.iconButtonHitSizeCompact
                        buttonRadius: appTheme.controlRadiusSmall
                        font.weight: appTheme.fontWeightRegular
                        text: qsTr("Cancel")
                        onClicked: {
                            if (!dialog.copyMode) {
                                dialog.pasteDiscarded()
                            }
                            dialog.close()
                        }
                    }

                    DialogActionButton {
                        objectName: "adjustmentTransferAcceptButton"
                        kind: "accent"
                        buttonWidth: appTheme.spaceXl * 7
                        buttonHeight: appTheme.iconButtonHitSizeCompact
                        buttonRadius: appTheme.controlRadiusSmall
                        font.weight: appTheme.fontWeightRegular
                        text: dialog.acceptText()
                        enabled: !dialog.copyMode || dialog.selectedCount > 0
                        onClicked: {
                            if (dialog.copyMode) {
                                dialog.copyAccepted(dialog.selectedKeys(),
                                                    dialog.selectedSourceVersionId)
                            } else {
                                dialog.pasteAccepted("paste")
                            }
                            dialog.close()
                        }
                    }
                }
            }
        }
    }

    Component {
        id: sectionDelegate

        Item {
            id: sectionRoot
            property var rowData: ({})
            activeFocusOnTab: true
            Accessible.role: Accessible.Button
            Accessible.name: qsTr("%1 section").arg(String(rowData.section || ""))
            Accessible.description: rowData.expanded ? qsTr("Expanded") : qsTr("Collapsed")

            Keys.onPressed: function(event) {
                if (event.key === Qt.Key_Space || event.key === Qt.Key_Return
                        || event.key === Qt.Key_Enter) {
                    dialog.toggleSection(String(rowData.section || ""),
                                         Number(rowData.ordinal || 0))
                    event.accepted = true
                }
            }

            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: appTheme.spaceSm
                anchors.rightMargin: appTheme.spaceSm
                spacing: appTheme.spaceSm

                Canvas {
                    id: sectionChevron
                    Layout.preferredWidth: 12
                    Layout.preferredHeight: 12
                    Layout.alignment: Qt.AlignVCenter
                    antialiasing: true
                    onPaint: {
                        const ctx = getContext("2d")
                        ctx.clearRect(0, 0, width, height)
                        ctx.strokeStyle = appTheme.textMutedColor
                        ctx.lineWidth = 1.5
                        ctx.lineCap = "round"
                        ctx.lineJoin = "round"
                        ctx.beginPath()
                        if (sectionRoot.rowData.expanded) {
                            ctx.moveTo(2, 4)
                            ctx.lineTo(6, 8)
                            ctx.lineTo(10, 4)
                        } else {
                            ctx.moveTo(4, 2)
                            ctx.lineTo(8, 6)
                            ctx.lineTo(4, 10)
                        }
                        ctx.stroke()
                    }
                    Component.onCompleted: sectionChevron.requestPaint()
                    Connections {
                        target: sectionRoot
                        function onRowDataChanged() { sectionChevron.requestPaint() }
                    }
                }

                Label {
                    text: String(sectionRoot.rowData.section || "")
                    color: appTheme.textColor
                    font.family: appTheme.uiFontFamily
                    font.pixelSize: appTheme.fontSizeTitle
                    font.weight: appTheme.fontWeightStrong
                }

                Rectangle {
                    Layout.fillWidth: true
                    Layout.preferredHeight: 1
                    color: appTheme.dividerColor
                }
            }

            MouseArea {
                anchors.fill: parent
                cursorShape: Qt.PointingHandCursor
                onClicked: dialog.toggleSection(String(sectionRoot.rowData.section || ""),
                                                Number(sectionRoot.rowData.ordinal || 0))
            }
        }
    }

    Component {
        id: parameterDelegate

        Item {
            id: parameterRoot
            property var rowData: ({})
            activeFocusOnTab: dialog.copyMode
            Accessible.role: Accessible.CheckBox
            Accessible.name: String(rowData.label || "")
            Accessible.checkable: dialog.copyMode
            Accessible.checked: rowData.checked === true

            function toggle() {
                if (dialog.copyMode) {
                    dialog.setRowChecked(Number(rowData.sourceIndex),
                                         !(rowData.checked === true))
                }
            }

            Accessible.onToggleAction: toggle()
            Keys.onPressed: function(event) {
                if (event.key === Qt.Key_Space || event.key === Qt.Key_Return
                        || event.key === Qt.Key_Enter) {
                    toggle()
                    event.accepted = true
                }
            }

            Rectangle {
                anchors.fill: parent
                radius: appTheme.badgeRadius
                color: parameterMouse.containsMouse && dialog.copyMode
                       ? appTheme.buttonHoveredFillColor
                       : "transparent"
                border.width: parameterRoot.activeFocus ? 1 : 0
                border.color: appTheme.accentColor
            }

            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: appTheme.spaceSm
                anchors.rightMargin: appTheme.spaceSm
                spacing: appTheme.spaceSm

                Rectangle {
                    visible: dialog.copyMode
                    Layout.preferredWidth: appTheme.iconOpticalSizeCompact
                    Layout.preferredHeight: appTheme.iconOpticalSizeCompact
                    radius: appTheme.badgeRadius
                    color: parameterRoot.rowData.checked
                           ? appTheme.editorListSelectedFillColor
                           : "transparent"
                    border.width: 1
                    border.color: parameterRoot.rowData.checked
                                  ? appTheme.editorListSelectedFillColor
                                  : appTheme.cardBorderColor

                    Label {
                        anchors.centerIn: parent
                        visible: parameterRoot.rowData.checked
                        text: "✓"
                        color: appTheme.editorListSelectedInkColor
                        font.family: appTheme.uiFontFamily
                        font.pixelSize: appTheme.fontSizeCaption
                        font.weight: appTheme.fontWeightHeading
                    }
                }

                Label {
                    Layout.fillWidth: true
                    text: String(parameterRoot.rowData.label || "")
                    color: parameterRoot.rowData.checked || !dialog.copyMode
                           ? appTheme.textColor
                           : appTheme.textMutedColor
                    font.family: appTheme.uiFontFamily
                    font.pixelSize: appTheme.fontSizeBody
                    elide: Text.ElideRight
                }

                Label {
                    Layout.maximumWidth: parameterRoot.width / 3
                    text: String(parameterRoot.rowData.value || "")
                    color: parameterRoot.rowData.checked
                           ? appTheme.textColor : appTheme.textMutedColor
                    font.family: appTheme.dataFontFamily
                    font.pixelSize: appTheme.fontSizeCaption
                    horizontalAlignment: Text.AlignRight
                    elide: Text.ElideMiddle
                }
            }

            MouseArea {
                id: parameterMouse
                anchors.fill: parent
                enabled: dialog.copyMode
                hoverEnabled: true
                cursorShape: dialog.copyMode ? Qt.PointingHandCursor : Qt.ArrowCursor
                onClicked: parameterRoot.toggle()
            }
        }
    }
}