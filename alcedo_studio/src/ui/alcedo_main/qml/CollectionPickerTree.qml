//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

import QtQuick
import QtQuick.Controls
import QtQuick.Controls.impl
import QtQuick.Layouts
import QtQuick.Shapes
import "CollectionTree.js" as CollectionTree

// Destination tree of the Import Folder dialog (#1078): Library and the ordinary collections,
// with a search field and a New collection button. Mounted folders are not listed. The rows use
// the geometry, icons, and tree helpers of the Collections sidebar (CollectionTree.js).
//
// Search: a match keeps its ancestors visible, every branch is expanded while the field has
// text, and the expansion of the tree returns when the field is cleared. The matched part of a
// name is bold. The destination stays selected while the filter hides it.
Rectangle {
    id: picker
    objectName: "collectionPicker"

    /// The rows of FolderController.folders.
    property var folderRows: []
    /// The owner of CreateFolderIn (FolderController); null hides New collection.
    property var folderController: null
    /// elementId of the destination; 0 is Library. A tree reload gives the rows new ui ids, so
    /// the destination is kept by element id.
    property int selectedElementId: 0
    property string searchText: ""
    property int visibleRowCount: 6

    /// Ui folder id of the destination, -1 while no row has selectedElementId.
    readonly property int selectedFolderId: {
        const row = rowOfElement(selectedElementId)
        return row ? Number(row.folderId) : -1
    }
    readonly property string selectedName: {
        const row = rowOfElement(selectedElementId)
        return row ? displayName(row) : ""
    }

    // elementId -> true for an expanded collection; the Library row has its own flag.
    property var expandedIds: ({})
    property bool libraryExpanded: true
    // elementId of the parent of the draft row of New collection; -1 when none.
    property int draftParentElementId: -1

    readonly property var orderedRows: CollectionTree.orderRows(
        CollectionTree.ordinaryRows(folderRows), false)
    readonly property var searchResult: CollectionTree.searchRows(orderedRows, searchText,
                                                                  displayName)
    readonly property bool searching: searchResult !== null
    readonly property var shownRows: buildShownRows()

    radius: appTheme.controlRadius
    color: appTheme.bgBaseColor
    border.width: 1
    border.color: appTheme.controlBorderColor
    implicitHeight: searchBox.height + 1 + visibleRowCount * appTheme.collectionsTreeRowHeight
                    + appTheme.spaceXs * 2
    clip: true

    function displayName(row) {
        return Number(row.folderId) === 0 ? qsTr("Library") : String(row.name || "")
    }

    function rowOfElement(elementId) {
        for (let i = 0; i < orderedRows.length; ++i) {
            if (Number(orderedRows[i].elementId) === Number(elementId)) {
                return orderedRows[i]
            }
        }
        return null
    }

    function isExpanded(row) {
        if (Number(row.folderId) === 0) {
            return libraryExpanded
        }
        return expandedIds[String(Number(row.elementId))] === true
    }

    function setExpanded(row, expanded) {
        if (Number(row.folderId) === 0) {
            libraryExpanded = expanded
            return
        }
        const next = Object.assign({}, expandedIds)
        if (expanded) {
            next[String(Number(row.elementId))] = true
        } else {
            delete next[String(Number(row.elementId))]
        }
        expandedIds = next
    }

    // Expands every row above @p elementId, so its row is shown.
    function revealElement(elementId) {
        const byFolderId = CollectionTree.rowsByFolderId(orderedRows)
        const row = rowOfElement(elementId)
        if (!row) {
            return
        }
        const next = Object.assign({}, expandedIds)
        let parent = byFolderId[String(Number(row.parentFolderId))]
        let guard = 0
        while (Number(row.folderId) !== 0 && parent && guard < 4096) {
            if (Number(parent.folderId) === 0) {
                libraryExpanded = true
                break
            }
            next[String(Number(parent.elementId))] = true
            parent = byFolderId[String(Number(parent.parentFolderId))]
            ++guard
        }
        expandedIds = next
    }

    function buildShownRows() {
        const byFolderId = CollectionTree.rowsByFolderId(orderedRows)
        const out = []
        for (let i = 0; i < orderedRows.length; ++i) {
            const row = orderedRows[i]
            let shown
            if (searching) {
                shown = searchResult.visible[String(Number(row.folderId))] === true
            } else {
                shown = true
                let parent = Number(row.folderId) === 0
                        ? null : byFolderId[String(Number(row.parentFolderId))]
                let guard = 0
                while (parent && guard < 4096) {
                    if (!isExpanded(parent)) {
                        shown = false
                        break
                    }
                    parent = Number(parent.folderId) === 0
                            ? null : byFolderId[String(Number(parent.parentFolderId))]
                    ++guard
                }
            }
            if (!shown) {
                continue
            }
            out.push(Object.assign({}, row, { rowExpanded: searching || isExpanded(row) }))
        }
        // The draft row of New collection goes after the shown subtree of its parent.
        const parent = draftParentElementId >= 0 ? rowOfElement(draftParentElementId) : null
        let at = parent ? out.findIndex(function(row) {
            return Number(row.elementId) === draftParentElementId
        }) : -1
        if (at >= 0) {
            ++at
            while (at < out.length && Number(out[at].treeDepth) > Number(parent.treeDepth)) {
                ++at
            }
            out.splice(at, 0, { isDraft: true, folderId: -1, elementId: -1,
                                treeDepth: Number(parent.treeDepth) + 1, hasChildren: false })
        }
        return out
    }

    function beginNewCollection() {
        if (!folderController || selectedFolderId < 0) {
            return
        }
        searchText = ""
        searchInput.text = ""
        const parent = rowOfElement(selectedElementId)
        if (parent) {
            setExpanded(parent, true)
            revealElement(selectedElementId)
        }
        draftParentElementId = selectedElementId
    }

    function finishNewCollection(name) {
        const parentElementId = draftParentElementId
        draftParentElementId = -1
        const trimmed = String(name || "").trim()
        const parent = rowOfElement(parentElementId)
        if (trimmed.length === 0 || !parent || !folderController) {
            return
        }
        const created = Number(folderController.CreateFolderIn(Number(parent.folderId), trimmed))
        if (created > 0) {
            selectedElementId = created
            // The row of the new collection comes with the next tree read.
            pendingRevealElementId = created
            revealPendingElement()
        }
    }

    // elementId of a new collection whose row is revealed when it arrives; -1 when none.
    property int pendingRevealElementId: -1
    onOrderedRowsChanged: revealPendingElement()

    function revealPendingElement() {
        if (pendingRevealElementId <= 0 || !rowOfElement(pendingRevealElementId)) {
            return
        }
        const elementId = pendingRevealElementId
        pendingRevealElementId = -1
        revealElement(elementId)
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        // Search field and New collection.
        Item {
            id: searchBox
            Layout.fillWidth: true
            Layout.preferredHeight: 34

            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: appTheme.spaceSm
                anchors.rightMargin: appTheme.spaceXs
                spacing: appTheme.spaceSm

                IconImage {
                    Layout.preferredWidth: 14
                    Layout.preferredHeight: 14
                    Layout.alignment: Qt.AlignVCenter
                    sourceSize: Qt.size(14, 14)
                    source: "qrc:/panel_icons/search.svg"
                    color: appTheme.textMutedColor
                }

                TextInput {
                    id: searchInput
                    objectName: "collectionPickerSearch"
                    Layout.fillWidth: true
                    Layout.alignment: Qt.AlignVCenter
                    clip: true
                    color: appTheme.textColor
                    selectByMouse: true
                    font.family: appTheme.uiFontFamily
                    font.pixelSize: appTheme.fontSizeBody
                    selectedTextColor: appTheme.textColor
                    selectionColor: appTheme.controlHoveredFillColor
                    onTextChanged: picker.searchText = text
                    Keys.onEscapePressed: function(event) {
                        if (text.length > 0) {
                            text = ""
                            event.accepted = true
                        } else {
                            event.accepted = false
                        }
                    }

                    Text {
                        anchors.fill: parent
                        verticalAlignment: Text.AlignVCenter
                        visible: searchInput.text.length === 0
                                 && searchInput.preeditText.length === 0
                        text: qsTr("Search collections")
                        color: appTheme.textMutedColor
                        font: searchInput.font
                        elide: Text.ElideRight
                    }
                }

                Rectangle {
                    id: newCollectionButton
                    objectName: "collectionPickerNewButton"
                    Layout.preferredWidth: 24
                    Layout.preferredHeight: 24
                    Layout.alignment: Qt.AlignVCenter
                    visible: picker.folderController !== null
                    radius: appTheme.controlRadiusSmall
                    color: newCollectionMouse.containsMouse ? appTheme.controlFillColor
                                                            : "transparent"
                    Accessible.role: Accessible.Button
                    Accessible.name: qsTr("New collection")
                    Accessible.onPressAction: picker.beginNewCollection()

                    signal clicked()
                    onClicked: picker.beginNewCollection()

                    IconImage {
                        anchors.centerIn: parent
                        width: 14
                        height: 14
                        sourceSize: Qt.size(14, 14)
                        source: "qrc:/panel_icons/folder-plus.svg"
                        color: newCollectionMouse.containsMouse ? appTheme.textColor
                                                                : appTheme.textMutedColor
                    }

                    MouseArea {
                        id: newCollectionMouse
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: newCollectionButton.clicked()
                    }

                    ToolTip.visible: newCollectionMouse.containsMouse
                    ToolTip.text: qsTr("New collection")
                }
            }
        }

        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: 1
            color: appTheme.dividerColor
        }

        ListView {
            id: pickerList
            objectName: "collectionPickerList"
            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.topMargin: appTheme.spaceXs
            Layout.bottomMargin: appTheme.spaceXs
            clip: true
            model: picker.shownRows
            boundsBehavior: Flickable.StopAtBounds
            Accessible.role: Accessible.Tree
            Accessible.name: qsTr("Import to")
            ScrollBar.vertical: AlcedoScrollBar {}

            delegate: Item {
                id: pickerRow
                required property var modelData
                required property int index
                readonly property bool isDraft: modelData.isDraft === true
                readonly property bool isRoot: !isDraft && Number(modelData.folderId) === 0
                readonly property bool selected: !isDraft
                    && Number(modelData.elementId) === picker.selectedElementId
                objectName: isDraft ? "collectionPickerDraftRow"
                                    : "collectionPickerRow_" + String(modelData.elementId)
                width: pickerList.width - appTheme.spaceXs * 2
                x: appTheme.spaceXs
                height: appTheme.collectionsTreeRowHeight

                Rectangle {
                    anchors.fill: parent
                    radius: appTheme.controlRadiusSmall
                    color: pickerRow.selected ? appTheme.controlHoveredFillColor
                                              : rowMouse.containsMouse ? appTheme.hoverColor
                                                                       : "transparent"
                }

                RowLayout {
                    anchors.fill: parent
                    anchors.leftMargin: appTheme.spaceXs
                                        + Number(pickerRow.modelData.treeDepth)
                                          * appTheme.collectionsTreeIndentStep
                    anchors.rightMargin: appTheme.spaceSm
                    spacing: 0

                    Item {
                        Layout.preferredWidth: 16
                        Layout.preferredHeight: 16
                        Layout.alignment: Qt.AlignVCenter

                        IconImage {
                            anchors.centerIn: parent
                            width: 12
                            height: 12
                            sourceSize: Qt.size(12, 12)
                            visible: !pickerRow.isDraft && pickerRow.modelData.hasChildren === true
                            source: "qrc:/panel_icons/chevron-right.svg"
                            color: appTheme.textMutedColor
                            rotation: pickerRow.modelData.rowExpanded === true ? 90 : 0
                        }
                    }

                    IconImage {
                        Layout.leftMargin: appTheme.spaceXs
                        Layout.rightMargin: appTheme.spaceSm
                        Layout.preferredWidth: 16
                        Layout.preferredHeight: 16
                        Layout.alignment: Qt.AlignVCenter
                        sourceSize: Qt.size(16, 16)
                        source: pickerRow.isRoot ? "qrc:/panel_icons/library.svg"
                                                 : "qrc:/panel_icons/folder.svg"
                        color: pickerRow.selected ? appTheme.textColor : appTheme.textMutedColor
                    }

                    Label {
                        objectName: pickerRow.isDraft
                                    ? "" : "collectionPickerName_" + String(pickerRow.modelData.elementId)
                        Layout.fillWidth: true
                        Layout.minimumWidth: 0
                        Layout.alignment: Qt.AlignVCenter
                        visible: !pickerRow.isDraft
                        elide: Text.ElideRight
                        textFormat: Text.StyledText
                        text: pickerRow.isDraft
                              ? ""
                              : CollectionTree.highlightedName(picker.displayName(pickerRow.modelData),
                                                               picker.searchText)
                        color: appTheme.textColor
                        font.family: appTheme.uiFontFamily
                        font.pixelSize: appTheme.fontSizeTitle
                        font.weight: pickerRow.selected ? appTheme.fontWeightStrong
                                                        : appTheme.fontWeightRegular
                    }

                    // Check mark of the destination.
                    Shape {
                        Layout.preferredWidth: 14
                        Layout.preferredHeight: 14
                        Layout.leftMargin: appTheme.spaceSm
                        Layout.alignment: Qt.AlignVCenter
                        visible: pickerRow.selected
                        preferredRendererType: Shape.CurveRenderer

                        ShapePath {
                            fillColor: "transparent"
                            strokeColor: appTheme.textColor
                            strokeWidth: 2.4 * 14 / 24
                            capStyle: ShapePath.RoundCap
                            joinStyle: ShapePath.RoundJoin
                            scale: Qt.size(14 / 24, 14 / 24)
                            PathSvg { path: "M5 12l5 5L20 7" }
                        }
                    }

                    Label {
                        Layout.leftMargin: appTheme.spaceSm
                        Layout.alignment: Qt.AlignVCenter
                        readonly property real count: pickerRow.isDraft
                                                      ? -1 : Number(pickerRow.modelData.imageCount)
                        visible: !pickerRow.selected && !pickerRow.isDraft && count >= 0
                        text: count >= 0 ? count.toLocaleString(Qt.locale(), "f", 0) : ""
                        color: appTheme.textMutedColor
                        font.family: appTheme.headlineFontFamily
                        font.features: { "tnum": 1 }
                        font.pixelSize: appTheme.fontSizeCaption
                        font.weight: appTheme.fontWeightStrong
                    }

                    // Inline name field of New collection: a plain TextInput in a hairline box.
                    Loader {
                        Layout.fillWidth: true
                        Layout.preferredHeight: appTheme.collectionsTreeRowHeight - 6
                        Layout.alignment: Qt.AlignVCenter
                        active: pickerRow.isDraft
                        visible: active
                        sourceComponent: Rectangle {
                            radius: appTheme.controlRadiusSmall
                            color: appTheme.bgBaseColor
                            border.width: 1
                            border.color: draftInput.activeFocus ? appTheme.textMutedColor
                                                                 : appTheme.dividerColor

                            TextInput {
                                id: draftInput
                                objectName: "collectionPickerDraftField"
                                anchors.fill: parent
                                anchors.leftMargin: appTheme.spaceSm
                                anchors.rightMargin: appTheme.spaceSm
                                verticalAlignment: TextInput.AlignVCenter
                                clip: true
                                selectByMouse: true
                                color: appTheme.textColor
                                font.family: appTheme.uiFontFamily
                                font.pixelSize: appTheme.fontSizeTitle
                                property bool finished: false

                                Text {
                                    anchors.fill: parent
                                    verticalAlignment: Text.AlignVCenter
                                    visible: draftInput.text.length === 0
                                             && draftInput.preeditText.length === 0
                                    text: qsTr("Collection Name")
                                    color: appTheme.textMutedColor
                                    font: draftInput.font
                                    elide: Text.ElideRight
                                }

                                function finish(accept) {
                                    if (finished) {
                                        return
                                    }
                                    finished = true
                                    // The commit reloads the rows, which destroys this editor.
                                    const value = accept ? text : ""
                                    Qt.callLater(picker.finishNewCollection, value)
                                }

                                Component.onCompleted: forceActiveFocus()
                                onAccepted: finish(true)
                                Keys.onEscapePressed: finish(false)
                                onActiveFocusChanged: {
                                    if (!activeFocus) {
                                        finish(true)
                                    }
                                }
                            }
                        }
                    }
                }

                MouseArea {
                    id: rowMouse
                    anchors.fill: parent
                    enabled: !pickerRow.isDraft
                    hoverEnabled: true
                    onClicked: function(mouse) {
                        const start = appTheme.spaceXs + Number(pickerRow.modelData.treeDepth)
                                      * appTheme.collectionsTreeIndentStep
                        if (!picker.searching && pickerRow.modelData.hasChildren === true
                                && mouse.x >= start - 2 && mouse.x <= start + 18) {
                            picker.setExpanded(pickerRow.modelData,
                                               !picker.isExpanded(pickerRow.modelData))
                            return
                        }
                        picker.selectedElementId = Number(pickerRow.modelData.elementId)
                    }
                    onDoubleClicked: {
                        if (!picker.searching && pickerRow.modelData.hasChildren === true) {
                            picker.setExpanded(pickerRow.modelData,
                                               !picker.isExpanded(pickerRow.modelData))
                        }
                    }
                }
            }

            Label {
                objectName: "collectionPickerEmpty"
                anchors.horizontalCenter: parent.horizontalCenter
                y: appTheme.spaceMd
                visible: picker.searching && picker.searchResult.matchCount === 0
                text: qsTr("No matching collections")
                color: appTheme.textMutedColor
                font.family: appTheme.uiFontFamily
                font.pixelSize: appTheme.fontSizeBody
            }
        }
    }
}
