import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "CollectionTree.js" as CollectionTree

// Library / inspector right-click menu. Visuals come from the shared
// AppContextMenu family (DESIGN.md "Context menus"); this file owns the
// Rating and "Add to" sub-menus and the dynamic action rows.
AppContextMenu {
    id: root
    objectName: "imageContextMenu"
    property var actions: []
    property int currentRating: 0
    property bool ratingEnabled: false
    // FolderController.folders rows; the "Add to" list shows them as a tree.
    property var collectionRows: []
    property int currentCollectionId: 0
    property bool addToEnabled: false
    signal actionRequested(string actionId)
    signal ratingRequested(int rating)
    signal exportQueueRequested()
    signal collectionRequested(int folderId)

    // Rating, "Add to", separator; the dynamic actions follow.
    readonly property int dynamicActionOffset: 3

    function starText(value) {
        let text = ""
        const rating = Math.max(0, Math.min(5, Number(value)))
        for (let i = 1; i <= 5; ++i) {
            text += i <= rating ? "★" : "☆"
        }
        return text
    }

    AppContextMenu {
        id: ratingMenu
        title: qsTr("Rating")
        enabled: root.ratingEnabled

        AppMenuItem {
            text: qsTr("Unrated")
            checkable: true
            checked: root.currentRating === 0
            onTriggered: root.ratingRequested(0)
        }

        AppMenuSeparator {}

        Instantiator {
            model: [1, 2, 3, 4, 5]
            delegate: AppMenuItem {
                readonly property int ratingValue: Number(modelData)
                text: root.starText(ratingValue)
                checkable: true
                checked: root.currentRating === ratingValue
                onTriggered: root.ratingRequested(ratingValue)
            }
            onObjectAdded: function(index, object) {
                ratingMenu.insertItem(index + 2, object)
            }
            onObjectRemoved: function(index, object) {
                ratingMenu.removeItem(object)
            }
        }
    }

    // "Add to": the export queue first, then a separator and a bounded, scrollable
    // collection tree, so a project with many collections keeps a short menu.
    AppContextMenu {
        id: addToMenu
        objectName: "imageContextAddToMenu"
        title: qsTr("Add to")
        enabled: root.addToEnabled

        property string filterText: ""
        readonly property var orderedRows: CollectionTree.orderRows(root.collectionRows, false)
        readonly property var shownRows: CollectionTree.filterRows(orderedRows, filterText)
        // The root row is not a target; it only anchors the tree.
        readonly property int collectionCount: Math.max(0, orderedRows.length - 1)
        readonly property bool filterVisible:
            collectionCount >= appTheme.collectionMenuFilterThreshold

        onAboutToShow: filterText = ""

        AppMenuItem {
            objectName: "imageContextAddToExportQueue"
            text: qsTr("Export Queue")
            onTriggered: {
                addToMenu.dismiss()
                root.exportQueueRequested()
            }
        }

        AppMenuSeparator {}

        Item {
            id: collectionPicker
            objectName: "imageContextCollectionPicker"
            implicitWidth: 240
            implicitHeight: pickerColumn.implicitHeight

            ColumnLayout {
                id: pickerColumn
                anchors.left: parent.left
                anchors.right: parent.right
                spacing: appTheme.spaceXs

                // A plain TextInput in a hairline box: no style-provided field chrome.
                Rectangle {
                    objectName: "imageContextCollectionFilter"
                    Layout.fillWidth: true
                    Layout.preferredHeight: appTheme.collectionRowHeight
                    visible: addToMenu.filterVisible
                    radius: appTheme.controlRadiusSmall
                    color: appTheme.bgBaseColor
                    border.width: 1
                    border.color: filterField.activeFocus ? appTheme.textMutedColor
                                                          : appTheme.dividerColor

                    TextInput {
                        id: filterField
                        anchors.fill: parent
                        anchors.leftMargin: appTheme.spaceSm
                        anchors.rightMargin: appTheme.spaceSm
                        verticalAlignment: TextInput.AlignVCenter
                        clip: true
                        text: addToMenu.filterText
                        selectByMouse: true
                        color: appTheme.textColor
                        font.family: appTheme.uiFontFamily
                        font.pixelSize: appTheme.fontSizeBody
                        selectedTextColor: appTheme.textColor
                        selectionColor: Qt.rgba(appTheme.textMutedColor.r, appTheme.textMutedColor.g,
                                                appTheme.textMutedColor.b, 0.6)
                        onTextEdited: addToMenu.filterText = text

                        Text {
                            anchors.fill: parent
                            verticalAlignment: Text.AlignVCenter
                            visible: filterField.text.length === 0
                                     && filterField.preeditText.length === 0
                            text: qsTr("Filter collections")
                            color: appTheme.textMutedColor
                            font: filterField.font
                            elide: Text.ElideRight
                        }
                    }
                }

                ListView {
                    id: collectionList
                    objectName: "imageContextCollectionList"
                    Layout.fillWidth: true
                    Layout.preferredHeight: Math.max(1, Math.min(count,
                        appTheme.collectionMenuMaxRows)) * appTheme.collectionRowHeight
                    visible: addToMenu.collectionCount > 0 && count > 0
                    clip: true
                    boundsBehavior: Flickable.StopAtBounds
                    model: addToMenu.shownRows
                    ScrollBar.vertical: ScrollBar {
                        policy: collectionList.count > appTheme.collectionMenuMaxRows
                                ? ScrollBar.AlwaysOn : ScrollBar.AsNeeded
                    }

                    delegate: Rectangle {
                        id: collectionRow
                        required property var modelData
                        readonly property bool isRoot: Number(modelData.folderId) === 0
                        readonly property bool targetEnabled: !isRoot
                            && Number(modelData.folderId) !== root.currentCollectionId
                        objectName: "imageContextCollection_" + String(modelData.elementId)
                        width: ListView.view.width
                        height: appTheme.collectionRowHeight
                        radius: appTheme.controlRadiusSmall
                        color: targetEnabled && rowMouse.containsMouse ? appTheme.hoverColor
                                                                       : "transparent"

                        Label {
                            anchors.fill: parent
                            // Text edge of AppMenuItem (padding, state gutter, spacing), then
                            // the tree indent.
                            anchors.leftMargin: appTheme.spaceSm * 2 + appTheme.spaceLg
                                                + Math.max(0, Number(collectionRow.modelData.treeDepth) - 1)
                                                  * appTheme.collectionIndentStep
                            anchors.rightMargin: appTheme.spaceSm
                            verticalAlignment: Text.AlignVCenter
                            elide: Text.ElideRight
                            text: collectionRow.isRoot ? qsTr("全部图片")
                                                       : String(collectionRow.modelData.name)
                            color: collectionRow.targetEnabled ? appTheme.textColor
                                                               : appTheme.textMutedColor
                            font.pixelSize: appTheme.fontSizeBody
                            font.weight: appTheme.fontWeightRegular
                        }

                        MouseArea {
                            id: rowMouse
                            anchors.fill: parent
                            hoverEnabled: true
                            enabled: collectionRow.targetEnabled
                            onClicked: {
                                const folderId = Number(collectionRow.modelData.folderId)
                                addToMenu.dismiss()
                                root.collectionRequested(folderId)
                            }
                        }
                    }
                }

                Label {
                    objectName: "imageContextCollectionEmpty"
                    Layout.fillWidth: true
                    Layout.preferredHeight: appTheme.collectionRowHeight
                    visible: !collectionList.visible
                    leftPadding: appTheme.spaceSm * 2 + appTheme.spaceLg
                    verticalAlignment: Text.AlignVCenter
                    text: addToMenu.collectionCount === 0 ? qsTr("No collections yet")
                                                          : qsTr("No matching collections")
                    color: appTheme.textMutedColor
                    font.pixelSize: appTheme.fontSizeBody
                    font.weight: appTheme.fontWeightRegular
                }
            }
        }
    }

    AppMenuSeparator {}

    Instantiator {
        model: root.actions
        delegate: AppMenuItem {
            readonly property var actionData: modelData
            objectName: actionData && actionData.id
                        ? "imageContextAction_" + String(actionData.id)
                        : ""
            text: actionData && actionData.label ? actionData.label : ""
            enabled: !(actionData && actionData.enabled === false)
            onTriggered: root.actionRequested(actionData && actionData.id ? actionData.id : "")
        }
        onObjectAdded: function(index, object) {
            root.insertItem(index + root.dynamicActionOffset, object)
        }
        onObjectRemoved: function(index, object) {
            root.removeItem(object)
        }
    }
}
