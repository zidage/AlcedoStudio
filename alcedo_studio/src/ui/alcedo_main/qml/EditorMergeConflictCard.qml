pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts

// One merge conflict as a three-column comparison: Current | Incoming | Merged.
// The conflict row itself stays flat on the dialog surface; only the three
// comparison panes are cards. Merge tint tokens color labels / value ink and
// selected card borders.
Item {
    id: root

    property var conflict: ({})
    property string choice: ""
    property int rowIndex: -1
    property color textColor: appTheme.textColor
    property color mutedColor: appTheme.textMutedColor
    property color surfaceColor: appTheme.cardSurfaceColor
    property color borderColor: appTheme.cardBorderColor

    readonly property string fieldKey: String(conflict && conflict.fieldKey || "")
    readonly property bool resolved: choice === "current" || choice === "incoming"
    readonly property var mergedValue: choice === "incoming"
                                        ? conflict.incomingValue
                                        : choice === "current" ? conflict.currentValue : null
    readonly property color mergedColor: choice === "incoming"
                                         ? appTheme.mergeIncomingColor
                                         : choice === "current"
                                           ? appTheme.mergeCurrentColor : root.borderColor

    signal choiceSelected(string choice)

    objectName: "editorMergeConflictRow"
    implicitHeight: cardColumn.implicitHeight

    function formatFieldTitle(key) {
        var raw = String(key || "").split("/")[0].toLowerCase()
        if (raw === "hls")
            return qsTr("HSL")
        if (raw === "odt")
            return qsTr("Output Transform")
        if (raw === "raw_decode")
            return qsTr("RAW Decode")
        if (raw === "color_temp")
            return qsTr("Color Temperature")
        if (raw === "crop_rotate")
            return qsTr("Crop & Rotate")
        if (raw.length === 0)
            return qsTr("Adjustment")
        return raw.replace(/[_-]+/g, " ").replace(/\b\w/g, function(letter) {
            return letter.toUpperCase()
        })
    }

    function formatValue(value) {
        if (value === undefined || value === null)
            return qsTr("No value")
        if (typeof value === "boolean")
            return value ? qsTr("On") : qsTr("Off")
        if (typeof value === "number") {
            var rounded = Math.round(value)
            if (Math.abs(value - rounded) < 0.0005)
                return String(rounded)
            return value.toFixed(3).replace(/0+$/, "").replace(/\.$/, "")
        }
        if (typeof value === "string")
            return value.length > 0 ? value : qsTr("Empty")

        try {
            var serialized = JSON.stringify(value)
            if (!serialized || serialized === "undefined")
                return qsTr("No value")
            return serialized.length > 160 ? serialized.slice(0, 157) + "..." : serialized
        } catch (error) {
            return String(value)
        }
    }

    ColumnLayout {
        id: cardColumn
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        spacing: appTheme.spaceMd

        // Card header: CONFLICT / RESOLVED badge + title only.
        RowLayout {
            id: conflictHeader
            objectName: "editorMergeConflictHeader"
            Layout.fillWidth: true
            spacing: appTheme.spaceSm

            Rectangle {
                Layout.preferredWidth: conflictBadgeLabel.implicitWidth + appTheme.spaceMd
                Layout.preferredHeight: appTheme.spaceXl + appTheme.spaceXs
                radius: appTheme.badgeRadius
                color: root.resolved
                       ? appTheme.mergeIncomingFillColor : appTheme.mergeCurrentFillColor
                border.width: 1
                border.color: root.resolved
                              ? appTheme.mergeIncomingColor : appTheme.mergeCurrentColor

                Label {
                    id: conflictBadgeLabel
                    anchors.centerIn: parent
                    text: root.resolved ? qsTr("RESOLVED") : qsTr("CONFLICT")
                    color: root.resolved
                           ? appTheme.mergeIncomingColor : appTheme.mergeCurrentColor
                    font.family: appTheme.uiFontFamily
                    font.pixelSize: appTheme.fontSizeCaption
                    font.weight: appTheme.fontWeightStrong
                }
            }

            Label {
                Layout.fillWidth: true
                text: root.formatFieldTitle(root.fieldKey)
                color: root.textColor
                elide: Text.ElideRight
                font.family: appTheme.uiFontFamily
                font.pixelSize: appTheme.fontSizeTitle
                font.weight: appTheme.fontWeightStrong
            }
        }

        // Three equal comparison columns on the shared dialog surface.
        RowLayout {
            Layout.fillWidth: true
            spacing: appTheme.spaceMd

            // CURRENT (OURS)
            Rectangle {
                id: currentValueCard
                objectName: "editorMergeCurrentValueCard"
                Layout.fillWidth: true
                Layout.fillHeight: true
                implicitHeight: currentColumn.implicitHeight + appTheme.spaceSm * 2
                color: appTheme.bgBaseColor
                radius: appTheme.controlRadiusSmall
                border.width: 1
                border.color: currentChoiceButton.selected
                              ? appTheme.mergeCurrentColor : root.borderColor

                ColumnLayout {
                    id: currentColumn
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.top: parent.top
                    anchors.margins: appTheme.spaceSm
                    spacing: appTheme.spaceSm

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: appTheme.spaceXs

                        Label {
                            Layout.fillWidth: true
                            text: qsTr("CURRENT (OURS)")
                            color: appTheme.mergeCurrentColor
                            elide: Text.ElideRight
                            font.family: appTheme.uiFontFamily
                            font.pixelSize: appTheme.fontSizeCaption
                            font.weight: appTheme.fontWeightStrong
                        }

                        EditorMergeChoiceButton {
                            id: currentChoiceButton
                            objectName: "editorMergeCurrentChoiceButton"
                            Layout.fillWidth: false
                            Layout.preferredWidth: Math.max(implicitWidth, appTheme.spaceXl * 5)
                            compact: true
                            choiceKind: "current"
                            selected: root.choice === "current"
                            label: qsTr("Keep Current")
                            accessibleLabel: qsTr("Use Current value for %1")
                                              .arg(root.formatFieldTitle(root.fieldKey))
                            onChoiceSelected: (selectedChoice) => root.choiceSelected(selectedChoice)
                        }
                    }

                    EditorMergeJsonValue {
                        Layout.fillWidth: true
                        value: root.conflict.currentValue
                        textColor: root.textColor
                        mutedColor: root.mutedColor
                        valueColor: appTheme.mergeCurrentColor
                        maxEntries: 4
                    }
                }
            }

            // INCOMING (THEIRS)
            Rectangle {
                id: incomingValueCard
                objectName: "editorMergeIncomingValueCard"
                Layout.fillWidth: true
                Layout.fillHeight: true
                implicitHeight: incomingColumn.implicitHeight + appTheme.spaceSm * 2
                color: appTheme.bgBaseColor
                radius: appTheme.controlRadiusSmall
                border.width: 1
                border.color: incomingChoiceButton.selected
                              ? appTheme.mergeIncomingColor : root.borderColor

                ColumnLayout {
                    id: incomingColumn
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.top: parent.top
                    anchors.margins: appTheme.spaceSm
                    spacing: appTheme.spaceSm

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: appTheme.spaceXs

                        Label {
                            Layout.fillWidth: true
                            text: qsTr("INCOMING (THEIRS)")
                            color: appTheme.mergeIncomingColor
                            elide: Text.ElideRight
                            font.family: appTheme.uiFontFamily
                            font.pixelSize: appTheme.fontSizeCaption
                            font.weight: appTheme.fontWeightStrong
                        }

                        EditorMergeChoiceButton {
                            id: incomingChoiceButton
                            objectName: "editorMergeConflictChoice"
                            Layout.fillWidth: false
                            Layout.preferredWidth: Math.max(implicitWidth, appTheme.spaceXl * 5)
                            compact: true
                            choiceKind: "incoming"
                            selected: root.choice === "incoming"
                            label: qsTr("Use Incoming")
                            accessibleLabel: qsTr("Use Incoming value for %1")
                                              .arg(root.formatFieldTitle(root.fieldKey))
                            onChoiceSelected: (selectedChoice) => root.choiceSelected(selectedChoice)
                        }
                    }

                    EditorMergeJsonValue {
                        Layout.fillWidth: true
                        value: root.conflict.incomingValue
                        textColor: root.textColor
                        mutedColor: root.mutedColor
                        valueColor: appTheme.mergeIncomingColor
                        maxEntries: 4
                    }
                }
            }

            // MERGED (PREVIEW)
            Rectangle {
                id: resolvedValueCard
                objectName: "editorMergeResolvedValueCard"
                Layout.fillWidth: true
                Layout.fillHeight: true
                implicitHeight: mergedColumn.implicitHeight + appTheme.spaceSm * 2
                color: appTheme.bgBaseColor
                radius: appTheme.controlRadiusSmall
                border.width: 1
                border.color: root.resolved ? root.mergedColor : root.borderColor

                ColumnLayout {
                    id: mergedColumn
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.top: parent.top
                    anchors.margins: appTheme.spaceSm
                    spacing: appTheme.spaceSm

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: appTheme.spaceXs

                        Label {
                            Layout.fillWidth: true
                            text: qsTr("MERGED (PREVIEW)")
                            color: root.resolved ? root.mergedColor : root.mutedColor
                            elide: Text.ElideRight
                            font.family: appTheme.uiFontFamily
                            font.pixelSize: appTheme.fontSizeCaption
                            font.weight: appTheme.fontWeightStrong
                        }

                        Label {
                            visible: !root.resolved
                            text: qsTr("Pending Selection")
                            color: root.mutedColor
                            font.family: appTheme.uiFontFamily
                            font.pixelSize: appTheme.fontSizeCaption
                            font.weight: appTheme.fontWeightStrong
                        }
                    }

                    // Harness asserts this Label's text for pending / resolved
                    // scalars. When resolved, JsonValue paints the structured
                    // preview and this Label stays in-tree (opacity 0) so Find()
                    // still reads the mirrored formatValue().
                    Label {
                        id: mergedValueLabel
                        objectName: "editorMergeResolvedValue"
                        Layout.fillWidth: true
                        Layout.preferredHeight: root.resolved
                                                ? 0 : appTheme.spaceXl * 4
                        opacity: root.resolved ? 0 : 1
                        text: root.resolved
                              ? root.formatValue(root.mergedValue)
                              : qsTr("Waiting for resolution...")
                        color: root.mutedColor
                        horizontalAlignment: Text.AlignHCenter
                        verticalAlignment: Text.AlignVCenter
                        wrapMode: Text.WordWrap
                        font.family: appTheme.uiFontFamily
                        font.pixelSize: appTheme.fontSizeBody
                        clip: true
                    }

                    EditorMergeJsonValue {
                        Layout.fillWidth: true
                        visible: root.resolved
                        value: root.mergedValue
                        textColor: root.textColor
                        mutedColor: root.mutedColor
                        valueColor: root.mergedColor
                        maxEntries: 4
                    }
                }
            }
        }
    }
}
