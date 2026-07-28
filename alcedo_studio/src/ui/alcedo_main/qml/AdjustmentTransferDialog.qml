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

    contentItem: ColumnLayout {
        spacing: 0

        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: appTheme.iconButtonHitSize + appTheme.spaceMd
            color: appTheme.bgPanelColor

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
                        font.pixelSize: appTheme.fontSizeTitle
                        font.weight: appTheme.fontWeightHeading
                        elide: Text.ElideRight
                    }

                    Label {
                        Layout.fillWidth: true
                        visible: dialog.sourceTitle.length > 0
                        text: dialog.sourceTitle
                        color: appTheme.textMutedColor
                        font.pixelSize: appTheme.fontSizeCaption
                        elide: Text.ElideMiddle
                    }
                }

                Button {
                    id: closeButton
                    objectName: "adjustmentTransferCloseButton"
                    Layout.preferredWidth: appTheme.iconButtonHitSize
                    Layout.preferredHeight: appTheme.iconButtonHitSize
                    flat: true
                    hoverEnabled: true
                    Accessible.name: qsTr("Close")
                    onClicked: dialog.reject()

                    contentItem: Label {
                        text: "×"
                        color: closeButton.hovered ? appTheme.textColor : appTheme.textMutedColor
                        font.pixelSize: appTheme.fontSizeTitle
                        horizontalAlignment: Text.AlignHCenter
                        verticalAlignment: Text.AlignVCenter
                    }

                    background: Rectangle {
                        radius: appTheme.controlRadiusSmall
                        color: closeButton.hovered ? appTheme.buttonHoveredFillColor
                                                   : appTheme.buttonIdleFillColor
                    }
                }
            }

            Rectangle {
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.bottom: parent.bottom
                height: 1
                color: appTheme.dividerColor
            }
        }

        RowLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            spacing: 0

            Rectangle {
                Layout.preferredWidth: dialog.copyMode ? appTheme.editorSidePanelWidth : 0
                Layout.fillHeight: true
                visible: dialog.copyMode
                color: appTheme.bgBaseColor

                ColumnLayout {
                    anchors.fill: parent
                    spacing: 0

                    Label {
                        Layout.fillWidth: true
                        Layout.leftMargin: appTheme.spaceMd
                        Layout.rightMargin: appTheme.spaceMd
                        Layout.topMargin: appTheme.spaceSm
                        Layout.bottomMargin: appTheme.spaceSm
                        text: qsTr("SOURCE VERSIONS")
                        color: appTheme.textMutedColor
                        font.family: appTheme.monoFontFamily
                        font.pixelSize: appTheme.fontSizeCaption
                        font.weight: appTheme.fontWeightStrong
                    }

                    ListView {
                        id: versionList
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        Layout.margins: appTheme.spaceSm
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
                                radius: appTheme.controlRadius
                                color: versionDelegate.selected
                                       ? appTheme.editorListSelectedFillColor
                                       : (versionMouse.containsMouse
                                          ? appTheme.buttonHoveredFillColor
                                          : appTheme.buttonIdleFillColor)
                                border.width: versionDelegate.activeFocus ? 1 : 0
                                border.color: appTheme.accentColor
                            }

                            Rectangle {
                                visible: versionDelegate.selected
                                width: 2
                                height: parent.height - appTheme.spaceMd
                                anchors.left: parent.left
                                anchors.verticalCenter: parent.verticalCenter
                                radius: 1
                                color: appTheme.editorListSelectedInkColor
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
                                    color: versionDelegate.selected
                                           ? appTheme.editorListSelectedInkColor
                                           : appTheme.cardSurfaceColor

                                    Label {
                                        anchors.centerIn: parent
                                        text: String(versionDelegate.modelData.displayName
                                                     || "V").slice(0, 1).toUpperCase()
                                        color: versionDelegate.selected
                                               ? appTheme.editorListSelectedFillColor
                                               : appTheme.textMutedColor
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
                                        color: versionDelegate.selected
                                               ? appTheme.editorListSelectedInkColor
                                               : appTheme.textColor
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
                                        color: versionDelegate.selected
                                               ? appTheme.editorListSelectedInkColor
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

                Rectangle {
                    anchors.top: parent.top
                    anchors.right: parent.right
                    anchors.bottom: parent.bottom
                    width: 1
                    color: appTheme.dividerColor
                }
            }

            Rectangle {
                Layout.fillWidth: true
                Layout.fillHeight: true
                color: appTheme.bgDeepColor

                ColumnLayout {
                    anchors.fill: parent
                    spacing: 0

                    Rectangle {
                        Layout.fillWidth: true
                        Layout.preferredHeight: appTheme.iconButtonHitSize
                        color: appTheme.bgPanelColor

                        RowLayout {
                            anchors.fill: parent
                            anchors.leftMargin: appTheme.spaceLg
                            anchors.rightMargin: appTheme.spaceLg
                            spacing: appTheme.spaceSm

                            Label {
                                Layout.fillWidth: true
                                text: dialog.copyMode ? qsTr("PARAMETERS TO COPY")
                                                      : qsTr("PARAMETERS TO PASTE")
                                color: appTheme.textMutedColor
                                font.family: appTheme.monoFontFamily
                                font.pixelSize: appTheme.fontSizeCaption
                                font.weight: appTheme.fontWeightStrong
                            }

                            Button {
                                visible: dialog.copyMode
                                flat: true
                                text: qsTr("Select All")
                                enabled: dialog.adjustmentRows.length > 0
                                         && dialog.selectedCount < dialog.adjustmentRows.length
                                onClicked: dialog.setAllRowsChecked(true)
                            }

                            Label {
                                visible: dialog.copyMode
                                text: "/"
                                color: appTheme.dividerColor
                            }

                            Button {
                                visible: dialog.copyMode
                                flat: true
                                text: qsTr("None")
                                enabled: dialog.selectedCount > 0
                                onClicked: dialog.setAllRowsChecked(false)
                            }
                        }

                        Rectangle {
                            anchors.left: parent.left
                            anchors.right: parent.right
                            anchors.bottom: parent.bottom
                            height: 1
                            color: appTheme.dividerColor
                        }
                    }

                    ListView {
                        id: parameterList
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        Layout.leftMargin: appTheme.spaceLg
                        Layout.rightMargin: appTheme.spaceLg
                        Layout.topMargin: appTheme.spaceSm
                        Layout.bottomMargin: appTheme.spaceSm
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
                        }
                    }
                }
            }
        }

        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: appTheme.iconButtonHitSize + appTheme.spaceLg
            color: appTheme.bgPanelColor

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
                    font.pixelSize: appTheme.fontSizeCaption
                    elide: Text.ElideRight
                }

                DialogActionButton {
                    objectName: "adjustmentTransferCancelButton"
                    buttonWidth: appTheme.editorSidePanelWidthMin / 2
                    buttonHeight: appTheme.iconButtonHitSizeCompact
                    buttonRadius: appTheme.controlRadiusSmall
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
                    buttonWidth: appTheme.editorSidePanelWidthMin
                    buttonHeight: appTheme.iconButtonHitSizeCompact
                    buttonRadius: appTheme.controlRadiusSmall
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

            Rectangle {
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.top: parent.top
                height: 1
                color: appTheme.dividerColor
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
                spacing: appTheme.spaceSm

                Label {
                    text: sectionRoot.rowData.expanded ? "⌄" : "›"
                    color: appTheme.textMutedColor
                    font.pixelSize: appTheme.fontSizeBody
                }

                Label {
                    text: String(sectionRoot.rowData.section || "")
                    color: appTheme.textColor
                    font.pixelSize: appTheme.fontSizeSection
                    font.weight: appTheme.fontWeightHeading
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
                radius: appTheme.controlRadiusSmall
                color: parameterMouse.containsMouse && dialog.copyMode
                       ? appTheme.buttonHoveredFillColor
                       : appTheme.buttonIdleFillColor
                border.width: parameterRoot.activeFocus ? 1 : 0
                border.color: appTheme.accentColor
            }

            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: appTheme.spaceLg
                anchors.rightMargin: appTheme.spaceSm
                spacing: appTheme.spaceSm

                Rectangle {
                    visible: dialog.copyMode
                    Layout.preferredWidth: appTheme.iconOpticalSizeCompact
                    Layout.preferredHeight: appTheme.iconOpticalSizeCompact
                    radius: width / 2
                    color: parameterRoot.rowData.checked
                           ? appTheme.editorListSelectedFillColor
                           : appTheme.bgBaseColor
                    border.width: 1
                    border.color: parameterRoot.rowData.checked
                                  ? appTheme.editorListSelectedFillColor
                                  : appTheme.cardBorderColor

                    Label {
                        anchors.centerIn: parent
                        visible: parameterRoot.rowData.checked
                        text: "✓"
                        color: appTheme.editorListSelectedInkColor
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
                    font.pixelSize: appTheme.fontSizeBody
                    elide: Text.ElideRight
                }

                Label {
                    Layout.maximumWidth: parameterRoot.width / 3
                    text: String(parameterRoot.rowData.value || "")
                    color: appTheme.textColor
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
