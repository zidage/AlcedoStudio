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
    property int targetCount: 0
    // Read-only package summary rows for Paste mode. Never edited in QML.
    property var adjustmentRows: []
    // AdjustmentTransferDialogModel owned by AdjustmentTransferController.
    // C++ owns every checked state; QML sends commands with stable identities.
    property var dialogModel: null
    property Item blurSource: null
    property real cornerRadius: 0
    property var expandedSections: ({})
    property int expandedSectionsRevision: 0

    signal copyAccepted()
    signal pasteAccepted(string strategy)
    signal pasteDiscarded()

    readonly property bool copyMode: mode === "copy"
    readonly property string selectedSourceVersionId:
        dialogModel ? String(dialogModel.selectedVersionId || "") : ""
    readonly property var displayRows: {
        const revision = expandedSectionsRevision
        return buildDisplayRows()
    }

    function sectionExpanded(section, ordinal) {
        if (expandedSections[section] !== undefined) {
            return expandedSections[section] === true
        }
        return ordinal < 2
    }

    function toggleSection(section, ordinal) {
        const next = Object.assign({}, expandedSections)
        next[section] = !sectionExpanded(section, ordinal)
        expandedSections = next
        ++expandedSectionsRevision
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
                label: row.label,
                value: row.value
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
        return copyMode ? qsTr("Copy Adjustments") : qsTr("Paste Adjustments")
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
        color: "transparent"
        radius: appTheme.panelRadius
        clip: true

        ColumnLayout {
            anchors.fill: parent
            spacing: 0

            // ── Header bar ──────────────────────────────────────────────────
            Rectangle {
                Layout.fillWidth: true
                Layout.preferredHeight: appTheme.iconButtonHitSize + appTheme.spaceMd
                color: "transparent"

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

                // Copy pane 1: source Versions.
                Rectangle {
                    Layout.preferredWidth: dialog.copyMode ? appTheme.editorSidePanelWidth : 0
                    Layout.fillHeight: true
                    visible: dialog.copyMode
                    color: "transparent"

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
                                objectName: "adjustmentTransferVersionList"
                                anchors.fill: parent
                                anchors.margins: appTheme.spaceXs
                                model: dialog.dialogModel ? dialog.dialogModel.versions : null
                                spacing: appTheme.spaceXs
                                boundsBehavior: Flickable.StopAtBounds
                                reuseItems: true
                                keyNavigationEnabled: true
                                currentIndex: -1

                                delegate: Item {
                                    id: versionDelegate
                                    required property int index
                                    required property string versionId
                                    required property string displayName
                                    required property var updatedAt
                                    required property bool active
                                    required property bool selected
                                    width: ListView.view ? ListView.view.width : 0
                                    height: appTheme.iconButtonHitSize + appTheme.spaceSm
                                    activeFocusOnTab: true
                                    Accessible.role: Accessible.ListItem
                                    Accessible.name: versionDelegate.displayName

                                    function pick() {
                                        if (dialog.dialogModel) {
                                            dialog.dialogModel.SelectVersion(versionDelegate.versionId)
                                        }
                                    }

                                    Keys.onPressed: function(event) {
                                        if (event.key === Qt.Key_Space || event.key === Qt.Key_Return
                                                || event.key === Qt.Key_Enter) {
                                            versionDelegate.pick()
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
                                                text: versionDelegate.displayName.slice(0, 1).toUpperCase()
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
                                                text: versionDelegate.displayName
                                                color: appTheme.textColor
                                                font.family: appTheme.uiFontFamily
                                                font.pixelSize: appTheme.fontSizeBody
                                                font.weight: appTheme.fontWeightStrong
                                                elide: Text.ElideRight
                                            }

                                            Label {
                                                Layout.fillWidth: true
                                                text: versionDelegate.active
                                                      ? qsTr("Active · %1").arg(dialog.versionTimeText(
                                                              versionDelegate.updatedAt))
                                                      : dialog.versionTimeText(
                                                            versionDelegate.updatedAt)
                                                color: versionDelegate.active
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
                                        onClicked: versionDelegate.pick()
                                    }
                                }
                            }
                        }
                    }
                }

                // Copy pane 2: transferable nodes.
                Rectangle {
                    Layout.preferredWidth: dialog.copyMode ? appTheme.editorSidePanelWidth : 0
                    Layout.fillHeight: true
                    visible: dialog.copyMode
                    color: "transparent"

                    ColumnLayout {
                        anchors.fill: parent
                        spacing: 0

                        Label {
                            Layout.fillWidth: true
                            Layout.leftMargin: appTheme.spaceMd
                            Layout.rightMargin: appTheme.spaceMd
                            Layout.topMargin: appTheme.spaceSm
                            Layout.bottomMargin: appTheme.spaceSm
                            text: qsTr("Transferable Nodes")
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
                                id: nodeList
                                objectName: "adjustmentTransferNodeList"
                                anchors.fill: parent
                                anchors.margins: appTheme.spaceXs
                                model: dialog.dialogModel ? dialog.dialogModel.nodes : null
                                spacing: appTheme.spaceXs
                                boundsBehavior: Flickable.StopAtBounds
                                reuseItems: true
                                keyNavigationEnabled: true
                                currentIndex: -1

                                delegate: Item {
                                    id: nodeDelegate
                                    required property int index
                                    required property string nodeId
                                    required property string displayName
                                    required property int nodeKind
                                    required property bool defaultGrade
                                    required property int checkState
                                    required property bool focused
                                    width: ListView.view ? ListView.view.width : 0
                                    height: appTheme.iconButtonHitSizeCompact
                                    activeFocusOnTab: true
                                    Accessible.role: Accessible.ListItem
                                    Accessible.name: nodeDelegate.displayName

                                    function focus() {
                                        if (dialog.dialogModel) {
                                            dialog.dialogModel.FocusNode(nodeDelegate.nodeId)
                                        }
                                    }

                                    function toggleCheck() {
                                        if (dialog.dialogModel) {
                                            dialog.dialogModel.SetNodeChecked(
                                                nodeDelegate.nodeId,
                                                nodeDelegate.checkState !== Qt.Checked)
                                        }
                                    }

                                    Keys.onPressed: function(event) {
                                        if (event.key === Qt.Key_Space) {
                                            nodeDelegate.toggleCheck()
                                            event.accepted = true
                                        } else if (event.key === Qt.Key_Return
                                                   || event.key === Qt.Key_Enter) {
                                            nodeDelegate.focus()
                                            event.accepted = true
                                        }
                                    }

                                    Rectangle {
                                        anchors.fill: parent
                                        radius: appTheme.badgeRadius
                                        color: nodeDelegate.focused
                                               ? appTheme.editorListSelectedFillColor
                                               : (nodeMouse.containsMouse
                                                  ? appTheme.buttonHoveredFillColor
                                                  : "transparent")
                                        border.width: nodeDelegate.activeFocus ? 1 : 0
                                        border.color: appTheme.accentColor
                                    }

                                    MouseArea {
                                        id: nodeMouse
                                        anchors.fill: parent
                                        hoverEnabled: true
                                        cursorShape: Qt.PointingHandCursor
                                        onClicked: nodeDelegate.focus()
                                    }

                                    RowLayout {
                                        anchors.fill: parent
                                        anchors.leftMargin: appTheme.spaceSm
                                        anchors.rightMargin: appTheme.spaceSm
                                        spacing: appTheme.spaceSm

                                        Rectangle {
                                            Layout.preferredWidth: appTheme.iconOpticalSizeCompact
                                            Layout.preferredHeight: appTheme.iconOpticalSizeCompact
                                            radius: appTheme.badgeRadius
                                            color: nodeDelegate.checkState !== Qt.Unchecked
                                                   ? appTheme.accentColor
                                                   : "transparent"
                                            border.width: 1
                                            border.color: nodeDelegate.checkState !== Qt.Unchecked
                                                          ? appTheme.accentColor
                                                          : appTheme.cardBorderColor

                                            Label {
                                                anchors.centerIn: parent
                                                visible: nodeDelegate.checkState !== Qt.Unchecked
                                                text: nodeDelegate.checkState === Qt.PartiallyChecked
                                                      ? "–" : "✓"
                                                color: "#FFFFFF"
                                                font.family: appTheme.uiFontFamily
                                                font.pixelSize: appTheme.fontSizeCaption
                                                font.weight: appTheme.fontWeightHeading
                                            }

                                            MouseArea {
                                                anchors.fill: parent
                                                cursorShape: Qt.PointingHandCursor
                                                onClicked: nodeDelegate.toggleCheck()
                                            }
                                        }

                                        Label {
                                            Layout.fillWidth: true
                                            text: nodeDelegate.displayName
                                            color: nodeDelegate.focused
                                                   ? appTheme.editorListSelectedInkColor
                                                   : appTheme.textColor
                                            font.family: appTheme.uiFontFamily
                                            font.pixelSize: appTheme.fontSizeBody
                                            font.weight: nodeDelegate.defaultGrade
                                                         ? appTheme.fontWeightStrong
                                                         : appTheme.fontWeightRegular
                                            elide: Text.ElideRight
                                        }
                                    }
                                }
                            }
                        }
                    }
                }

                // Copy pane 3 / Paste summary pane.
                Rectangle {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    color: "transparent"

                    ColumnLayout {
                        anchors.fill: parent
                        spacing: 0

                        Rectangle {
                            Layout.fillWidth: true
                            Layout.preferredHeight: appTheme.iconButtonHitSize
                            color: "transparent"

                            RowLayout {
                                anchors.fill: parent
                                anchors.leftMargin: appTheme.spaceLg
                                anchors.rightMargin: appTheme.spaceLg
                                spacing: appTheme.spaceSm

                                Label {
                                    Layout.fillWidth: true
                                    text: dialog.copyMode ? qsTr("Transferable Items")
                                                          : qsTr("Parameters to Paste")
                                    color: appTheme.textMutedColor
                                    font.family: appTheme.uiFontFamily
                                    font.pixelSize: appTheme.fontSizeCaption
                                    font.weight: appTheme.fontWeightStrong
                                }
                            }
                        }

                        Label {
                            Layout.fillWidth: true
                            Layout.leftMargin: appTheme.spaceLg
                            Layout.rightMargin: appTheme.spaceLg
                            Layout.bottomMargin: appTheme.spaceXs
                            visible: dialog.copyMode && dialog.dialogModel
                                     && String(dialog.dialogModel.errorText || "").length > 0
                            text: dialog.dialogModel ? String(dialog.dialogModel.errorText || "")
                                                     : ""
                            color: appTheme.dangerColor
                            font.family: appTheme.uiFontFamily
                            font.pixelSize: appTheme.fontSizeCaption
                            wrapMode: Text.WordWrap
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

                            // Copy mode: focused-node item rows with C++-owned checked state.
                            ListView {
                                id: itemList
                                objectName: "adjustmentTransferItemList"
                                anchors.fill: parent
                                anchors.margins: appTheme.spaceXs
                                visible: dialog.copyMode
                                model: dialog.copyMode && dialog.dialogModel
                                       ? dialog.dialogModel.items : null
                                boundsBehavior: Flickable.StopAtBounds
                                reuseItems: true
                                keyNavigationEnabled: true
                                currentIndex: -1

                                delegate: Item {
                                    id: itemDelegate
                                    required property int index
                                    required property string itemKey
                                    required property string displayName
                                    required property string displayValue
                                    required property int itemSection
                                    required property int itemKind
                                    required property bool checked
                                    required property bool enabled
                                    width: ListView.view ? ListView.view.width : 0
                                    height: appTheme.iconButtonHitSizeCompact
                                    activeFocusOnTab: itemDelegate.enabled
                                    Accessible.role: Accessible.CheckBox
                                    Accessible.name: itemDelegate.itemKind === 3
                                                     ? qsTr("Transfer all masks in this node")
                                                     : itemDelegate.displayName
                                    Accessible.checkable: itemDelegate.enabled
                                    Accessible.checked: itemDelegate.checked

                                    function toggle() {
                                        if (dialog.dialogModel && itemDelegate.enabled) {
                                            dialog.dialogModel.SetItemChecked(
                                                dialog.dialogModel.focusedNodeId,
                                                itemDelegate.itemKey,
                                                !itemDelegate.checked)
                                        }
                                    }

                                    Accessible.onToggleAction: toggle()
                                    Keys.onPressed: function(event) {
                                        if (event.key === Qt.Key_Space || event.key === Qt.Key_Return
                                                || event.key === Qt.Key_Enter) {
                                            itemDelegate.toggle()
                                            event.accepted = true
                                        }
                                    }

                                    Rectangle {
                                        anchors.fill: parent
                                        radius: appTheme.badgeRadius
                                        color: itemMouse.containsMouse && itemDelegate.enabled
                                               ? appTheme.buttonHoveredFillColor
                                               : "transparent"
                                        border.width: itemDelegate.activeFocus ? 1 : 0
                                        border.color: appTheme.accentColor
                                    }

                                    RowLayout {
                                        anchors.fill: parent
                                        anchors.leftMargin: appTheme.spaceSm
                                        anchors.rightMargin: appTheme.spaceSm
                                        spacing: appTheme.spaceSm

                                        Rectangle {
                                            Layout.preferredWidth: appTheme.iconOpticalSizeCompact
                                            Layout.preferredHeight: appTheme.iconOpticalSizeCompact
                                            radius: appTheme.badgeRadius
                                            opacity: itemDelegate.enabled ? 1.0 : 0.4
                                            color: itemDelegate.checked
                                                   ? appTheme.accentColor
                                                   : "transparent"
                                            border.width: 1
                                            border.color: itemDelegate.checked
                                                          ? appTheme.accentColor
                                                          : appTheme.cardBorderColor

                                            Label {
                                                anchors.centerIn: parent
                                                visible: itemDelegate.checked
                                                text: "✓"
                                                color: "#FFFFFF"
                                                font.family: appTheme.uiFontFamily
                                                font.pixelSize: appTheme.fontSizeCaption
                                                font.weight: appTheme.fontWeightHeading
                                            }
                                        }

                                        Label {
                                            Layout.fillWidth: true
                                            text: itemDelegate.displayName
                                            color: itemDelegate.enabled
                                                   ? appTheme.textColor : appTheme.textMutedColor
                                            font.family: appTheme.uiFontFamily
                                            font.pixelSize: appTheme.fontSizeBody
                                            elide: Text.ElideRight
                                        }

                                        Label {
                                            Layout.maximumWidth: itemDelegate.width / 3
                                            text: itemDelegate.displayValue
                                            color: itemDelegate.enabled && itemDelegate.checked
                                                   ? appTheme.textColor : appTheme.textMutedColor
                                            font.family: appTheme.dataFontFamily
                                            font.pixelSize: appTheme.fontSizeCaption
                                            horizontalAlignment: Text.AlignRight
                                            elide: Text.ElideMiddle
                                        }
                                    }

                                    MouseArea {
                                        id: itemMouse
                                        anchors.fill: parent
                                        enabled: itemDelegate.enabled
                                        hoverEnabled: true
                                        cursorShape: itemDelegate.enabled
                                                     ? Qt.PointingHandCursor : Qt.ArrowCursor
                                        onClicked: itemDelegate.toggle()
                                    }
                                }
                            }

                            // Paste mode: read-only grouped package summary.
                            ListView {
                                id: pasteList
                                objectName: "adjustmentTransferPasteList"
                                anchors.fill: parent
                                anchors.margins: appTheme.spaceXs
                                visible: !dialog.copyMode
                                model: dialog.copyMode ? null : dialog.displayRows
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
                                                     : pasteParameterDelegate
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
                color: "transparent"

                RowLayout {
                    anchors.fill: parent
                    anchors.leftMargin: appTheme.spaceLg
                    anchors.rightMargin: appTheme.spaceLg
                    spacing: appTheme.spaceSm

                    Item {
                        Layout.fillWidth: true
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
                        enabled: dialog.copyMode
                                 ? (dialog.dialogModel && dialog.dialogModel.canCopy === true)
                                 : true
                        onClicked: {
                            if (dialog.copyMode) {
                                dialog.copyAccepted()
                            } else {
                                dialog.pasteAccepted(dialog.pasteStrategy)
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
        id: pasteParameterDelegate

        Item {
            id: parameterRoot
            property var rowData: ({})
            Accessible.role: Accessible.ListItem
            Accessible.name: String(rowData.label || "")

            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: appTheme.spaceSm
                anchors.rightMargin: appTheme.spaceSm
                spacing: appTheme.spaceSm

                Label {
                    Layout.fillWidth: true
                    text: String(parameterRoot.rowData.label || "")
                    color: appTheme.textColor
                    font.family: appTheme.uiFontFamily
                    font.pixelSize: appTheme.fontSizeBody
                    elide: Text.ElideRight
                }

                Label {
                    Layout.maximumWidth: parameterRoot.width / 3
                    text: String(parameterRoot.rowData.value || "")
                    color: appTheme.textMutedColor
                    font.family: appTheme.dataFontFamily
                    font.pixelSize: appTheme.fontSizeCaption
                    horizontalAlignment: Text.AlignRight
                    elide: Text.ElideMiddle
                }
            }
        }
    }
}
