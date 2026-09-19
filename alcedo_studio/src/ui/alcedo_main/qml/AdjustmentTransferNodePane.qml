pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts

// Adjustment Transfer pane 2: transferable nodes (Color Grades + the single
// DRT/Post endpoint). Copy mode shows the pane's Select All checkbox and
// Clear action plus a checkbox per row; paste mode reuses the same row
// treatment read-only, with node focus still swapping the item pane.
Item {
    id: pane
    objectName: "adjustmentTransferNodePane"

    // Copy: AdjustmentTransferNodeListModel. Paste: a read-only ListModel
    // with the same role names.
    property var model: null
    property string title: qsTr("Nodes")
    // Qt::CheckState across every transferable node (copy mode only).
    property int allCheckState: Qt.Unchecked
    // Paste mode: no selection chrome; rows only report focus.
    property bool readOnly: false

    signal nodeFocused(string nodeId)
    signal nodeCheckRequested(string nodeId, bool checked)
    signal selectAllRequested(bool checked)
    signal clearRequested()

    implicitWidth: appTheme.editorSidePanelWidth

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        Label {
            Layout.fillWidth: true
            Layout.leftMargin: appTheme.spaceMd
            Layout.rightMargin: appTheme.spaceMd
            Layout.topMargin: appTheme.spaceSm
            Layout.bottomMargin: appTheme.spaceXs
            text: pane.title
            color: appTheme.textMutedColor
            font.family: appTheme.uiFontFamily
            font.pixelSize: appTheme.fontSizeCaption
            font.weight: appTheme.fontWeightStrong
        }

        RowLayout {
            objectName: "transferNodeHeaderControls"
            Layout.fillWidth: true
            Layout.leftMargin: appTheme.spaceSm
            Layout.rightMargin: appTheme.spaceSm
            Layout.bottomMargin: appTheme.spaceXs
            visible: !pane.readOnly
            spacing: appTheme.spaceSm

            ThemeCheckBox {
                id: nodeSelectAll
                objectName: "transferNodeSelectAll"
                Layout.fillWidth: true
                text: qsTr("Select All")
                alwaysPrimaryText: true
                checked: pane.allCheckState === Qt.Checked
                partiallyChecked: pane.allCheckState === Qt.PartiallyChecked
                onToggled: function(nextChecked) {
                    pane.selectAllRequested(nextChecked)
                }
            }

            DialogActionButton {
                objectName: "transferNodeClearButton"
                buttonWidth: appTheme.spaceXl * 3
                buttonHeight: appTheme.iconButtonHitSizeCompact - appTheme.spaceXs
                buttonRadius: appTheme.controlRadiusSmall
                font.pixelSize: appTheme.fontSizeBody
                font.weight: appTheme.fontWeightRegular
                text: qsTr("Clear")
                onClicked: pane.clearRequested()
            }
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
                model: pane.model
                spacing: appTheme.spaceXs
                boundsBehavior: Flickable.StopAtBounds
                reuseItems: true
                keyNavigationEnabled: true
                activeFocusOnTab: true
                Accessible.role: Accessible.List
                Accessible.name: pane.title

                delegate: Item {
                    id: nodeDelegate
                    objectName: "transferNodeDelegate"
                    required property int index
                    required property string nodeId
                    required property string displayName
                    required property int nodeKind
                    required property bool defaultGrade
                    required property int checkState
                    required property bool focused
                    width: ListView.view ? ListView.view.width : 0
                    height: Math.max(appTheme.iconButtonHitSizeCompact,
                                     nodeRowLayout.implicitHeight + appTheme.spaceXs)
                    Accessible.role: Accessible.ListItem
                    Accessible.name: nodeDelegate.displayName
                    Accessible.onPressAction: nodeDelegate.focusRow()

                    // keyNavigationEnabled lands active focus on the current
                    // delegate, so activation keys are handled here.
                    Keys.onPressed: function(event) {
                        if (event.key === Qt.Key_Space && !pane.readOnly) {
                            nodeDelegate.toggleCheck()
                            event.accepted = true
                        } else if (event.key === Qt.Key_Return
                                   || event.key === Qt.Key_Enter
                                   || (event.key === Qt.Key_Space && pane.readOnly)) {
                            nodeDelegate.focusRow()
                            event.accepted = true
                        }
                    }

                    function focusRow() {
                        ListView.view.currentIndex = nodeDelegate.index
                        ListView.view.forceActiveFocus()
                        pane.nodeFocused(nodeDelegate.nodeId)
                    }

                    function toggleCheck() {
                        pane.nodeCheckRequested(
                            nodeDelegate.nodeId,
                            nodeDelegate.checkState !== Qt.Checked)
                    }

                    Rectangle {
                        anchors.fill: parent
                        radius: appTheme.badgeRadius
                        color: nodeDelegate.focused
                               ? appTheme.editorListSelectedFillColor
                               : (nodeMouse.containsMouse
                                  ? appTheme.buttonHoveredFillColor
                                  : "transparent")
                        border.width: (nodeDelegate.ListView.isCurrentItem
                                       && nodeDelegate.ListView.view
                                       && nodeDelegate.ListView.view.activeFocus) ? 1 : 0
                        border.color: appTheme.textColor
                    }

                    MouseArea {
                        id: nodeMouse
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: nodeDelegate.focusRow()
                    }

                    RowLayout {
                        id: nodeRowLayout
                        anchors.fill: parent
                        anchors.leftMargin: appTheme.spaceSm
                        anchors.rightMargin: appTheme.spaceSm
                        spacing: appTheme.spaceSm

                        ThemeCheckBox {
                            objectName: "transferNodeRowCheck"
                            visible: !pane.readOnly
                            activeFocusOnTab: false
                            // Box-only: the row body still owns node focus, so the
                            // checkbox must not fill the row's remaining width.
                            Layout.fillWidth: false
                            Layout.preferredWidth: implicitWidth
                            checked: nodeDelegate.checkState === Qt.Checked
                            partiallyChecked: nodeDelegate.checkState === Qt.PartiallyChecked
                            accessibleText: qsTr("Select all items in %1")
                                            .arg(nodeDelegate.displayName)
                            onToggled: nodeDelegate.toggleCheck()
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
                            wrapMode: Text.Wrap
                        }
                    }
                }
            }
        }
    }
}
