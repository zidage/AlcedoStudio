import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Enum adjustment combo (Phase 6A). Binds to an EditorAdjustmentEnumModel
// (entries: [{value, label}, ...]) and commits one settled transaction on
// selection. The ComboBox follows model.currentIndex for programmatic loads;
// onActivated pushes the user's choice to the model. Visuals use appTheme
// tokens (DESIGN.md): sunken dark field matching AdjustmentField / Widgets
// EditorComboBoxStyle (bgBase).
Item {
    id: root
    objectName: "adjustmentCombo"

    property var model: null
    property bool showResetButton: true
    // Editor panels stay dense (28). Settings and other spacious surfaces can
    // raise this without changing the sunken-field chrome.
    property int controlHeight: 28
    // Panels may expose a stable object name for behavior tests and
    // accessibility tooling without changing the shared wrapper name.
    property string controlObjectName: "adjustmentCombo"
    // Entry key whose text heads each run of entries with the same value (for
    // example Scene / Display). Empty: the popup lists entries without headings.
    property string groupLabelRole: ""

    implicitHeight: comboRow.implicitHeight
    Layout.fillWidth: true

    readonly property color colText: appTheme.textColor
    readonly property color colMuted: appTheme.textMutedColor
    readonly property color colBorder: appTheme.cardBorderColor
    readonly property color colField: appTheme.bgBaseColor
    readonly property color colPopup: appTheme.bgBaseColor
    readonly property color colHover: appTheme.hoverColor
    readonly property color colSelectedFill: appTheme.editorListSelectedFillColor
    readonly property color colSelectedInk: appTheme.editorListSelectedInkColor

    RowLayout {
        id: comboRow
        anchors.fill: parent
        spacing: appTheme.spaceSm

        Label {
            Layout.preferredWidth: 76
            text: root.model ? root.model.label : ""
            color: root.colText
            font.pixelSize: appTheme.fontSizeBody
            wrapMode: Text.Wrap
            visible: root.model && root.model.label && root.model.label.length > 0
            Accessible.name: root.model ? root.model.label : ""
        }

        ComboBox {
            id: combo
            objectName: root.controlObjectName
            Layout.fillWidth: true
            Layout.preferredHeight: Math.max(root.controlHeight, combo.implicitHeight)
            enabled: root.model && root.model.enabled
            model: root.model ? root.model.entries : []
            textRole: "label"
            currentIndex: root.model ? root.model.currentIndex : 0
            activeFocusOnTab: true
            Accessible.role: Accessible.ComboBox
            Accessible.name: root.model ? root.model.label : ""
            onActivated: function (index) {
                if (root.model) {
                    root.model.selectIndex(index)
                }
            }
            Connections {
                target: root.model
                function onCurrentIndexChanged() {
                    if (root.model) {
                        combo.currentIndex = root.model.currentIndex
                    }
                }
                function onEntriesChanged() {
                    if (root.model) {
                        combo.model = root.model.entries
                    }
                }
            }

            // Dark sunken field — Basic style paints a light chrome by default.
            background: Rectangle {
                implicitHeight: root.controlHeight
                radius: appTheme.controlRadiusSmall
                color: root.colField
                border.width: 1
                border.color: combo.activeFocus || combo.hovered
                              ? root.colMuted
                              : root.colBorder
            }

            contentItem: Text {
                leftPadding: appTheme.spaceSm
                rightPadding: appTheme.spaceMd + appTheme.spaceSm
                topPadding: appTheme.spaceXs
                bottomPadding: appTheme.spaceXs
                text: combo.displayText
                font.pixelSize: appTheme.fontSizeBody
                color: combo.enabled ? root.colText : root.colMuted
                verticalAlignment: Text.AlignVCenter
                wrapMode: Text.WordWrap
            }

            indicator: Item {
                x: combo.width - width - appTheme.spaceSm
                y: combo.topPadding + (combo.availableHeight - height) / 2
                width: 12
                height: 12
                Text {
                    anchors.centerIn: parent
                    text: "▾"
                    color: root.colMuted
                    font.pixelSize: appTheme.fontSizeCaption
                    opacity: combo.enabled ? 1.0 : 0.45
                }
            }

            popup: Popup {
                y: combo.height + 2
                width: combo.width
                implicitHeight: Math.min(contentItem.implicitHeight + 2, 240)
                padding: 1

                background: Rectangle {
                    radius: appTheme.controlRadiusSmall
                    color: root.colPopup
                    border.width: 1
                    border.color: root.colBorder
                }

                contentItem: ListView {
                    clip: true
                    implicitHeight: contentHeight
                    model: combo.popup.visible ? combo.delegateModel : null
                    currentIndex: combo.highlightedIndex
                    ScrollIndicator.vertical: ScrollIndicator {}
                }
            }

            delegate: ItemDelegate {
                id: del
                width: ListView.view ? ListView.view.width : combo.width
                // Own the paddings: the style's defaults are tall and wide, and a
                // height that ignores them clips wrapped labels under the next row.
                leftPadding: appTheme.spaceSm
                rightPadding: appTheme.spaceSm
                topPadding: 0
                bottomPadding: 0
                spacing: 0
                implicitHeight: topPadding + bottomPadding + delContent.implicitHeight
                height: implicitHeight
                // ComboBox list models of maps expose role data via modelData.
                text: {
                    if (typeof modelData === "undefined" || modelData === null)
                        return ""
                    if (modelData.label !== undefined)
                        return String(modelData.label)
                    if (combo.textRole && modelData[combo.textRole] !== undefined)
                        return String(modelData[combo.textRole])
                    return ""
                }
                highlighted: combo.highlightedIndex === index
                palette.highlightedText: root.colSelectedInk
                palette.text: root.colText
                font.pixelSize: appTheme.fontSizeBody

                // Fill only the entry row, so a group heading never sits on the
                // selection fill.
                background: Item {
                    Rectangle {
                        anchors.fill: parent
                        anchors.topMargin: delHeading.visible ? delHeading.height : 0
                        color: del.highlighted
                               ? root.colSelectedFill
                               : (del.hovered ? root.colHover : "transparent")
                        radius: 4
                    }
                }

                // Heading of the group this entry starts; empty inside a group.
                readonly property string groupHeading: {
                    if (root.groupLabelRole.length === 0 || typeof modelData === "undefined"
                            || modelData === null)
                        return ""
                    const heading = modelData[root.groupLabelRole]
                    if (heading === undefined)
                        return ""
                    const entries = combo.model
                    if (index > 0 && entries && entries[index - 1]
                            && entries[index - 1][root.groupLabelRole] === heading)
                        return ""
                    return String(heading)
                }

                contentItem: Column {
                    id: delContent
                    spacing: 0

                    Text {
                        id: delHeading
                        visible: del.groupHeading.length > 0
                        width: parent.width
                        text: del.groupHeading
                        color: root.colMuted
                        font.pixelSize: appTheme.fontSizeCaption
                        elide: Text.ElideRight
                        topPadding: appTheme.spaceSm
                        bottomPadding: appTheme.spaceXs
                    }

                    Text {
                        width: parent.width
                        height: Math.max(root.controlHeight, implicitHeight)
                        text: del.text
                        color: del.highlighted ? root.colSelectedInk : root.colText
                        font.pixelSize: appTheme.fontSizeBody
                        wrapMode: Text.WordWrap
                        verticalAlignment: Text.AlignVCenter
                        topPadding: appTheme.spaceXs
                        bottomPadding: appTheme.spaceXs
                    }
                }
            }
        }

        AdjustmentResetButton {
            model: root.model
            visible: root.showResetButton
        }
    }
}
