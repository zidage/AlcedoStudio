pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts

// Adjustment Transfer pane 3: transferable items of the focused node.
// Copy mode shows Select All / Clear scoped to the focused node plus one
// ThemeCheckBox row per item; the Masks value stays a single all-or-none
// checkbox. Paste mode renders the same list read-only with no selection
// controls.
Item {
    id: pane
    objectName: "adjustmentTransferItemPane"

    // Copy: AdjustmentTransferItemListModel. Paste: a read-only ListModel
    // with the same role names.
    property var model: null
    property string title: qsTr("Items")
    // Qt::CheckState across the focused node's enabled items (copy mode only).
    property int bulkCheckState: Qt.Unchecked
    // Paste mode: no selection chrome.
    property bool readOnly: false
    // Replay error surfaced by the dialog model (copy mode only).
    property string errorText: ""

    signal itemCheckRequested(string itemKey, bool checked)
    signal selectAllRequested(bool checked)
    signal clearRequested()

    // AdjustmentTransferItemSection value -> product section label, mirroring
    // the Adjustment Stack panel vocabulary.
    function sectionName(sectionValue) {
        switch (Number(sectionValue)) {
        case 0: return qsTr("Node")
        case 1: return qsTr("Tone")
        case 2: return qsTr("Look")
        case 3: return qsTr("LUT")
        case 4: return qsTr("Display Transform")
        case 5: return qsTr("Masks")
        }
        return qsTr("Other")
    }

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
            wrapMode: Text.Wrap
        }

        RowLayout {
            objectName: "transferItemHeaderControls"
            Layout.fillWidth: true
            Layout.leftMargin: appTheme.spaceSm
            Layout.rightMargin: appTheme.spaceSm
            Layout.bottomMargin: appTheme.spaceXs
            visible: !pane.readOnly
            spacing: appTheme.spaceSm

            ThemeCheckBox {
                objectName: "transferItemSelectAll"
                Layout.fillWidth: true
                text: qsTr("Select All")
                alwaysPrimaryText: true
                checked: pane.bulkCheckState === Qt.Checked
                partiallyChecked: pane.bulkCheckState === Qt.PartiallyChecked
                onToggled: function(nextChecked) {
                    pane.selectAllRequested(nextChecked)
                }
            }

            DialogActionButton {
                objectName: "transferItemClearButton"
                buttonWidth: appTheme.spaceXl * 3
                buttonHeight: appTheme.iconButtonHitSizeCompact - appTheme.spaceXs
                buttonRadius: appTheme.controlRadiusSmall
                font.pixelSize: appTheme.fontSizeBody
                font.weight: appTheme.fontWeightRegular
                text: qsTr("Clear")
                onClicked: pane.clearRequested()
            }
        }

        Label {
            id: errorLabel
            objectName: "adjustmentTransferErrorText"
            Layout.fillWidth: true
            Layout.leftMargin: appTheme.spaceMd
            Layout.rightMargin: appTheme.spaceMd
            Layout.bottomMargin: appTheme.spaceXs
            visible: pane.errorText.length > 0
            text: pane.errorText
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
            Layout.bottomMargin: appTheme.spaceSm
            radius: appTheme.controlRadiusSmall
            color: appTheme.bgBaseColor
            border.width: 1
            border.color: appTheme.cardBorderColor
            clip: true

            Label {
                objectName: "adjustmentTransferEmptyHint"
                anchors.centerIn: parent
                visible: itemList.count === 0
                text: qsTr("No transferable adjustments.")
                color: appTheme.textMutedColor
                font.family: appTheme.uiFontFamily
                font.pixelSize: appTheme.fontSizeBody
            }

            ListView {
                id: itemList
                objectName: "adjustmentTransferItemList"
                anchors.fill: parent
                anchors.margins: appTheme.spaceXs
                model: pane.model
                spacing: 0
                boundsBehavior: Flickable.StopAtBounds
                reuseItems: true
                keyNavigationEnabled: true
                activeFocusOnTab: true
                Accessible.role: Accessible.List
                Accessible.name: pane.title

                section.property: "itemSection"
                section.criteria: ViewSection.FullString
                section.delegate: Item {
                    id: sectionHeader
                    required property var section
                    width: ListView.view ? ListView.view.width : 0
                    height: appTheme.iconButtonHitSizeCompact - appTheme.spaceSm

                    RowLayout {
                        anchors.fill: parent
                        anchors.leftMargin: appTheme.spaceSm
                        anchors.rightMargin: appTheme.spaceSm
                        spacing: appTheme.spaceSm

                        Label {
                            text: pane.sectionName(sectionHeader.section)
                            color: appTheme.textMutedColor
                            font.family: appTheme.uiFontFamily
                            font.pixelSize: appTheme.fontSizeCaption
                            font.weight: appTheme.fontWeightStrong
                        }

                        Rectangle {
                            Layout.fillWidth: true
                            Layout.preferredHeight: 1
                            color: appTheme.dividerColor
                        }
                    }
                }

                delegate: pane.readOnly ? readOnlyRow : checkRow
            }
        }
    }

    // Copy mode: the whole row is one ThemeCheckBox; Space/mouse toggles the
    // C++-owned checked state through the pane's itemCheckRequested signal.
    Component {
        id: checkRow

        Item {
            id: checkRowRoot
            objectName: "transferItemDelegate"
            required property int index
            required property string itemKey
            required property string displayName
            required property string displayValue
            required property int itemSection
            required property int itemKind
            required property bool checked
            required property bool enabled
            width: ListView.view ? ListView.view.width : 0
            height: Math.max(appTheme.iconButtonHitSizeCompact, innerCheck.implicitHeight)
            Accessible.role: Accessible.CheckBox
            Accessible.name: checkRowRoot.itemKind === 3
                             ? qsTr("Transfer all masks in this node")
                             : checkRowRoot.displayName
            Accessible.checkable: checkRowRoot.enabled
            Accessible.checked: checkRowRoot.checked
            Accessible.onToggleAction: checkRowRoot.toggle()

            // keyNavigationEnabled lands active focus on the current
            // delegate, so activation keys are handled here.
            Keys.onPressed: function(event) {
                if (event.key === Qt.Key_Space || event.key === Qt.Key_Return
                        || event.key === Qt.Key_Enter) {
                    checkRowRoot.toggle()
                    event.accepted = true
                }
            }

            function toggle() {
                innerCheck.toggle()
            }

            ThemeCheckBox {
                id: innerCheck
                objectName: "transferItemCheckRow"
                anchors.fill: parent
                activeFocusOnTab: false
                // The wrapper row owns the accessible checkbox role.
                Accessible.ignored: true
                checked: checkRowRoot.checked
                enabled: checkRowRoot.enabled
                text: checkRowRoot.displayName
                valueText: checkRowRoot.displayValue
                accessibleText: checkRowRoot.itemKind === 3
                                ? qsTr("Transfer all masks in this node")
                                : checkRowRoot.displayName
                onToggled: function(nextChecked) {
                    checkRowRoot.ListView.view.currentIndex = checkRowRoot.index
                    checkRowRoot.ListView.view.forceActiveFocus()
                    pane.itemCheckRequested(checkRowRoot.itemKey, nextChecked)
                }
            }

            // Keyboard focus ring owned by the ListView, distinct from the
            // component's own activeFocus treatment.
            Rectangle {
                anchors.fill: parent
                radius: appTheme.badgeRadius
                color: "transparent"
                border.width: (checkRowRoot.ListView.isCurrentItem
                               && checkRowRoot.ListView.view
                               && checkRowRoot.ListView.view.activeFocus) ? 1 : 0
                border.color: appTheme.textColor
            }
        }
    }

    // Paste mode: read-only summary row. No checkbox, no selection mutation.
    Component {
        id: readOnlyRow

        Item {
            id: readOnlyRoot
            objectName: "transferPasteItemRow"
            required property int index
            required property string itemKey
            required property string displayName
            required property string displayValue
            required property int itemSection
            required property int itemKind
            required property bool checked
            required property bool enabled
            width: ListView.view ? ListView.view.width : 0
            height: Math.max(appTheme.iconButtonHitSizeCompact,
                             readOnlyRowLayout.implicitHeight + appTheme.spaceXs)
            Accessible.role: Accessible.ListItem
            Accessible.name: readOnlyRoot.itemKind === 3
                             ? qsTr("Transfer all masks in this node")
                             : readOnlyRoot.displayName

            RowLayout {
                id: readOnlyRowLayout
                anchors.fill: parent
                anchors.leftMargin: appTheme.spaceSm
                anchors.rightMargin: appTheme.spaceSm
                spacing: appTheme.spaceSm

                Label {
                    Layout.fillWidth: true
                    text: readOnlyRoot.displayName
                    color: appTheme.textColor
                    font.family: appTheme.uiFontFamily
                    font.pixelSize: appTheme.fontSizeBody
                    wrapMode: Text.Wrap
                }

                Label {
                    Layout.maximumWidth: readOnlyRoot.width / 3
                    text: readOnlyRoot.displayValue
                    color: appTheme.textMutedColor
                    font.family: appTheme.dataFontFamily
                    font.pixelSize: appTheme.fontSizeCaption
                    horizontalAlignment: Text.AlignRight
                    wrapMode: Text.Wrap
                }
            }
        }
    }
}
