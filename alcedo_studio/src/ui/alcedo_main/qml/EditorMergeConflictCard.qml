import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts

// One Stitch-style conflict card: Current and Incoming are selectable, while
// Merged mirrors the value that will be sent to the transfer service.
Rectangle {
    id: root

    property var conflict: ({})
    property string choice: ""
    property int rowIndex: -1
    property color textColor: appTheme.textColor
    property color mutedColor: appTheme.textMutedColor
    property color surfaceColor: appTheme.cardSurfaceColor
    property color borderColor: appTheme.cardBorderColor

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
    implicitHeight: appTheme.spaceXl * 10
    height: implicitHeight
    radius: appTheme.controlRadiusSmall
    color: root.surfaceColor
    border.width: 1
    border.color: root.borderColor

    function formatFieldTitle(fieldKey) {
        var raw = String(fieldKey || "")
        var operatorName = raw.split("/")[0]
        if (operatorName.length === 0)
            return qsTr("Adjustment")
        return operatorName.replace(/[_-]+/g, " ").replace(/\b\w/g, function(letter) {
            return letter.toUpperCase()
        })
    }

    function formatFieldMeta(fieldKey) {
        var raw = String(fieldKey || "")
        var separator = raw.indexOf("/")
        return separator >= 0
                ? qsTr("Pipeline stage %1").arg(raw.slice(separator + 1))
                : qsTr("Adjustment parameter")
    }

    function formatValue(value) {
        if (value === undefined || value === null)
            return qsTr("No value")
        if (typeof value === "boolean")
            return value ? qsTr("Enabled") : qsTr("Disabled")
        if (typeof value === "number")
            return String(value)
        if (typeof value === "string")
            return value.length > 0 ? value : qsTr("Empty")

        try {
            var serialized = JSON.stringify(value)
            if (!serialized || serialized === "undefined")
                return qsTr("No value")
            return serialized.length > 180 ? serialized.slice(0, 177) + "..." : serialized
        } catch (error) {
            return String(value)
        }
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: appTheme.spaceMd
        spacing: appTheme.spaceSm

        RowLayout {
            Layout.fillWidth: true
            spacing: appTheme.spaceSm

            Rectangle {
                Layout.preferredWidth: appTheme.spaceXl + appTheme.spaceXs
                Layout.preferredHeight: appTheme.spaceXl + appTheme.spaceXs
                radius: appTheme.controlRadiusSmall
                color: appTheme.bgBaseColor
                border.width: 1
                border.color: root.borderColor

                Label {
                    anchors.centerIn: parent
                    text: "!"
                    color: appTheme.mergeCurrentColor
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
                    text: root.formatFieldTitle(root.conflict.fieldKey)
                    color: root.textColor
                    font.family: appTheme.uiFontFamily
                    font.pixelSize: appTheme.fontSizeTitle
                    font.weight: appTheme.fontWeightStrong
                }

                Label {
                    Layout.fillWidth: true
                    text: root.formatFieldMeta(root.conflict.fieldKey)
                          + "  ·  " + String(root.conflict.fieldKey || "")
                    color: root.mutedColor
                    elide: Text.ElideRight
                    font.family: appTheme.monoFontFamily
                    font.pixelSize: appTheme.fontSizeCaption
                }
            }

            Rectangle {
                Layout.preferredWidth: appTheme.spaceXl * 5
                Layout.preferredHeight: appTheme.spaceXl + appTheme.spaceXs
                radius: appTheme.badgeRadius
                color: root.resolved
                       ? appTheme.mergeIncomingFillColor : appTheme.mergeCurrentFillColor
                border.width: 1
                border.color: root.resolved ? root.mergedColor : appTheme.mergeCurrentColor

                Label {
                    anchors.centerIn: parent
                    text: root.resolved ? qsTr("Resolved") : qsTr("Conflict")
                    color: root.resolved ? root.mergedColor : appTheme.mergeCurrentColor
                    font.family: appTheme.uiFontFamily
                    font.pixelSize: appTheme.fontSizeCaption
                    font.weight: appTheme.fontWeightStrong
                }
            }
        }

        RowLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            spacing: appTheme.spaceSm

            Rectangle {
                id: currentValueCard
                objectName: "editorMergeCurrentValueCard"
                Layout.fillWidth: true
                Layout.fillHeight: true
                color: currentChoiceButton.selected
                       ? appTheme.mergeCurrentFillColor : root.surfaceColor
                radius: appTheme.controlRadiusSmall
                border.width: 1
                border.color: currentChoiceButton.selected
                              ? appTheme.mergeCurrentColor : root.borderColor

                ColumnLayout {
                    anchors.fill: parent
                    anchors.margins: appTheme.spaceSm
                    spacing: appTheme.spaceXs

                    RowLayout {
                        Layout.fillWidth: true
                        Label {
                            Layout.fillWidth: true
                            text: qsTr("CURRENT")
                            color: appTheme.mergeCurrentColor
                            font.family: appTheme.uiFontFamily
                            font.pixelSize: appTheme.fontSizeCaption
                            font.weight: appTheme.fontWeightStrong
                        }
                        Label {
                            text: qsTr("ours")
                            color: root.mutedColor
                            font.family: appTheme.monoFontFamily
                            font.pixelSize: appTheme.fontSizeCaption
                        }
                    }

                    Label {
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        text: root.formatValue(root.conflict.currentValue)
                        color: root.textColor
                        wrapMode: Text.Wrap
                        maximumLineCount: 4
                        elide: Text.ElideRight
                        verticalAlignment: Text.AlignVCenter
                        font.family: appTheme.monoFontFamily
                        font.pixelSize: appTheme.fontSizeBody
                    }

                    EditorMergeChoiceButton {
                        id: currentChoiceButton
                        objectName: "editorMergeCurrentChoiceButton"
                        choiceKind: "current"
                        selected: root.choice === "current"
                        label: qsTr("Keep Current")
                        accessibleLabel: qsTr("Use Current value for %1")
                                          .arg(root.formatFieldTitle(root.conflict.fieldKey))
                        onChoiceSelected: (selectedChoice) => root.choiceSelected(selectedChoice)
                    }
                }
            }

            Rectangle {
                Layout.preferredWidth: 1
                Layout.fillHeight: true
                color: root.borderColor
            }

            Rectangle {
                id: incomingValueCard
                objectName: "editorMergeIncomingValueCard"
                Layout.fillWidth: true
                Layout.fillHeight: true
                color: incomingChoiceButton.selected
                       ? appTheme.mergeIncomingFillColor : root.surfaceColor
                radius: appTheme.controlRadiusSmall
                border.width: 1
                border.color: incomingChoiceButton.selected
                              ? appTheme.mergeIncomingColor : root.borderColor

                ColumnLayout {
                    anchors.fill: parent
                    anchors.margins: appTheme.spaceSm
                    spacing: appTheme.spaceXs

                    RowLayout {
                        Layout.fillWidth: true
                        Label {
                            Layout.fillWidth: true
                            text: qsTr("INCOMING")
                            color: appTheme.mergeIncomingColor
                            font.family: appTheme.uiFontFamily
                            font.pixelSize: appTheme.fontSizeCaption
                            font.weight: appTheme.fontWeightStrong
                        }
                        Label {
                            text: qsTr("theirs")
                            color: root.mutedColor
                            font.family: appTheme.monoFontFamily
                            font.pixelSize: appTheme.fontSizeCaption
                        }
                    }

                    Label {
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        text: root.formatValue(root.conflict.incomingValue)
                        color: root.textColor
                        wrapMode: Text.Wrap
                        maximumLineCount: 4
                        elide: Text.ElideRight
                        verticalAlignment: Text.AlignVCenter
                        font.family: appTheme.monoFontFamily
                        font.pixelSize: appTheme.fontSizeBody
                    }

                    EditorMergeChoiceButton {
                        id: incomingChoiceButton
                        // Keep the historical object name so existing harnesses
                        // can still drive a visible incoming choice.
                        objectName: "editorMergeConflictChoice"
                        choiceKind: "incoming"
                        selected: root.choice === "incoming"
                        label: qsTr("Use Incoming")
                        accessibleLabel: qsTr("Use Incoming value for %1")
                                          .arg(root.formatFieldTitle(root.conflict.fieldKey))
                        onChoiceSelected: (selectedChoice) => root.choiceSelected(selectedChoice)
                    }
                }
            }

            Rectangle {
                Layout.preferredWidth: 1
                Layout.fillHeight: true
                color: root.borderColor
            }

            Rectangle {
                objectName: "editorMergeResolvedValueCard"
                Layout.fillWidth: true
                Layout.fillHeight: true
                color: root.resolved
                       ? (root.choice === "current"
                          ? appTheme.mergeCurrentFillColor : appTheme.mergeIncomingFillColor)
                       : root.surfaceColor
                radius: appTheme.controlRadiusSmall
                border.width: 1
                border.color: root.mergedColor

                ColumnLayout {
                    anchors.fill: parent
                    anchors.margins: appTheme.spaceSm
                    spacing: appTheme.spaceXs

                    RowLayout {
                        Layout.fillWidth: true
                        Label {
                            Layout.fillWidth: true
                            text: qsTr("MERGED")
                            color: root.mergedColor
                            font.family: appTheme.uiFontFamily
                            font.pixelSize: appTheme.fontSizeCaption
                            font.weight: appTheme.fontWeightStrong
                        }
                        Label {
                            text: root.resolved
                                  ? (root.choice === "current"
                                     ? qsTr("from Current") : qsTr("from Incoming"))
                                  : qsTr("pending")
                            color: root.mutedColor
                            font.family: appTheme.monoFontFamily
                            font.pixelSize: appTheme.fontSizeCaption
                        }
                    }

                    Label {
                        objectName: "editorMergeResolvedValue"
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        text: root.resolved
                              ? root.formatValue(root.mergedValue)
                              : qsTr("Choose Current or Incoming")
                        color: root.resolved ? root.textColor : root.mutedColor
                        wrapMode: Text.Wrap
                        maximumLineCount: 4
                        elide: Text.ElideRight
                        verticalAlignment: Text.AlignVCenter
                        font.family: appTheme.monoFontFamily
                        font.pixelSize: appTheme.fontSizeBody
                    }

                    Label {
                        Layout.fillWidth: true
                        text: root.resolved ? qsTr("Ready to commit") : qsTr("No value selected")
                        color: root.resolved ? root.mergedColor : root.mutedColor
                        font.family: appTheme.uiFontFamily
                        font.pixelSize: appTheme.fontSizeCaption
                        elide: Text.ElideRight
                    }
                }
            }
        }
    }
}
