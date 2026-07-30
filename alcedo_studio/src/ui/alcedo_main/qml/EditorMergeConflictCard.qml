pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Controls.impl
import QtQuick.Layouts

// One merge conflict. The two candidate wells expose the parsed JSON fields;
// the lower strip mirrors the exact value that will be committed.
Rectangle {
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
    readonly property color statusColor: root.resolved
                                         ? root.mergedColor : appTheme.mergeCurrentColor

    signal choiceSelected(string choice)

    objectName: "editorMergeConflictRow"
    implicitHeight: appTheme.spaceXl * 13
    height: implicitHeight
    radius: appTheme.controlRadius
    color: root.surfaceColor
    border.width: 1
    border.color: root.borderColor
    clip: true

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

    function formatFieldMeta(key) {
        var raw = String(key || "")
        var parts = raw.split("/")
        var stageIndex = parts.length > 1 ? Number(parts[1]) : -1
        var stageNames = [qsTr("Image Loading"), qsTr("Geometry Adjustment"),
                          qsTr("To Working Space"), qsTr("Basic Adjustment"),
                          qsTr("Color Adjustment"), qsTr("Detail Adjustment"),
                          qsTr("Output Transform")]
        if (stageIndex >= 0 && stageIndex < stageNames.length)
            return stageNames[stageIndex]
        return qsTr("Adjustment parameter")
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

    function iconSource(key) {
        var raw = String(key || "").split("/")[0].toLowerCase()
        if (raw === "exposure")
            return "qrc:/history_icons/sun-medium.svg"
        if (raw === "contrast")
            return "qrc:/history_icons/contrast.svg"
        if (raw === "hls")
            return "qrc:/history_icons/swatch-book.svg"
        if (raw === "saturation" || raw === "vibrance")
            return "qrc:/history_icons/droplets.svg"
        if (raw === "color_temp")
            return "qrc:/history_icons/thermometer.svg"
        if (raw === "crop_rotate")
            return "qrc:/history_icons/crop.svg"
        if (raw === "odt" || raw === "cst")
            return "qrc:/history_icons/monitor.svg"
        return "qrc:/history_icons/sliders-horizontal.svg"
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        Rectangle {
            objectName: "editorMergeConflictHeader"
            Layout.fillWidth: true
            Layout.preferredHeight: appTheme.spaceXl * 2 + appTheme.spaceXs
            color: root.surfaceColor

            RowLayout {
                anchors.fill: parent
                anchors.margins: appTheme.spaceSm
                spacing: appTheme.spaceSm

                Rectangle {
                    Layout.preferredWidth: appTheme.iconButtonHitSizeCompact - appTheme.spaceSm
                    Layout.preferredHeight: appTheme.iconButtonHitSizeCompact - appTheme.spaceSm
                    radius: appTheme.controlRadiusSmall
                    color: appTheme.bgBaseColor
                    border.width: 1
                    border.color: root.borderColor

                    ColorImage {
                        anchors.centerIn: parent
                        width: appTheme.iconOpticalSizeCompact
                        height: appTheme.iconOpticalSizeCompact
                        source: root.iconSource(root.fieldKey)
                        sourceSize.width: appTheme.iconSourceSizeCompact
                        sourceSize.height: appTheme.iconSourceSizeCompact
                        color: root.mutedColor
                        fillMode: Image.PreserveAspectFit
                    }
                }

                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: 0

                    Label {
                        Layout.fillWidth: true
                        text: root.formatFieldTitle(root.fieldKey)
                        color: root.textColor
                        elide: Text.ElideRight
                        font.family: appTheme.uiFontFamily
                        font.pixelSize: appTheme.fontSizeTitle
                        font.weight: appTheme.fontWeightStrong
                    }

                    Label {
                        Layout.fillWidth: true
                        text: root.formatFieldMeta(root.fieldKey)
                              + "  ·  " + root.fieldKey
                        color: root.mutedColor
                        elide: Text.ElideRight
                        font.family: appTheme.monoFontFamily
                        font.pixelSize: appTheme.fontSizeCaption
                    }
                }

                Rectangle {
                    Layout.preferredWidth: statusLabel.implicitWidth + appTheme.spaceMd
                    Layout.preferredHeight: appTheme.spaceXl + appTheme.spaceXs
                    radius: appTheme.badgeRadius
                    color: root.resolved
                           ? appTheme.mergeIncomingFillColor : appTheme.mergeCurrentFillColor
                    border.width: 1
                    border.color: root.statusColor

                    RowLayout {
                        anchors.fill: parent
                        anchors.leftMargin: appTheme.spaceSm
                        anchors.rightMargin: appTheme.spaceSm
                        spacing: appTheme.spaceXs

                        Rectangle {
                            Layout.preferredWidth: appTheme.spaceXs
                            Layout.preferredHeight: appTheme.spaceXs
                            radius: width / 2
                            color: root.statusColor
                        }

                        Label {
                            id: statusLabel
                            text: root.resolved ? qsTr("Resolved") : qsTr("Conflict")
                            color: root.statusColor
                            font.family: appTheme.uiFontFamily
                            font.pixelSize: appTheme.fontSizeCaption
                            font.weight: appTheme.fontWeightStrong
                        }
                    }
                }
            }
        }

        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: 1
            color: root.borderColor
        }

        RowLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.leftMargin: appTheme.spaceSm
            Layout.rightMargin: appTheme.spaceSm
            Layout.topMargin: appTheme.spaceSm
            Layout.bottomMargin: appTheme.spaceSm
            spacing: appTheme.spaceSm

            Rectangle {
                id: currentValueCard
                objectName: "editorMergeCurrentValueCard"
                Layout.fillWidth: true
                Layout.fillHeight: true
                color: currentChoiceButton.selected
                       ? appTheme.mergeCurrentFillColor : appTheme.bgBaseColor
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
                        spacing: appTheme.spaceXs

                        Label {
                            Layout.fillWidth: true
                            text: qsTr("CURRENT")
                            color: appTheme.mergeCurrentColor
                            font.family: appTheme.uiFontFamily
                            font.pixelSize: appTheme.fontSizeCaption
                            font.weight: appTheme.fontWeightStrong
                        }

                        Rectangle {
                            Layout.preferredWidth: appTheme.spaceXl * 3
                            Layout.preferredHeight: appTheme.spaceXl + appTheme.spaceXs
                            radius: appTheme.badgeRadius
                            color: appTheme.cardSurfaceColor
                            border.width: 1
                            border.color: root.borderColor

                            Label {
                                anchors.centerIn: parent
                                text: qsTr("ours")
                                color: root.mutedColor
                                font.family: appTheme.monoFontFamily
                                font.pixelSize: appTheme.fontSizeCaption
                            }
                        }
                    }

                    EditorMergeJsonValue {
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        value: root.conflict.currentValue
                        textColor: root.textColor
                        mutedColor: root.mutedColor
                        maxEntries: 3
                    }

                    EditorMergeChoiceButton {
                        id: currentChoiceButton
                        objectName: "editorMergeCurrentChoiceButton"
                        choiceKind: "current"
                        selected: root.choice === "current"
                        label: qsTr("Keep Current")
                        accessibleLabel: qsTr("Use Current value for %1")
                                          .arg(root.formatFieldTitle(root.fieldKey))
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
                       ? appTheme.mergeIncomingFillColor : appTheme.bgBaseColor
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
                        spacing: appTheme.spaceXs

                        Label {
                            Layout.fillWidth: true
                            text: qsTr("INCOMING")
                            color: appTheme.mergeIncomingColor
                            font.family: appTheme.uiFontFamily
                            font.pixelSize: appTheme.fontSizeCaption
                            font.weight: appTheme.fontWeightStrong
                        }

                        Rectangle {
                            Layout.preferredWidth: appTheme.spaceXl * 3
                            Layout.preferredHeight: appTheme.spaceXl + appTheme.spaceXs
                            radius: appTheme.badgeRadius
                            color: appTheme.cardSurfaceColor
                            border.width: 1
                            border.color: root.borderColor

                            Label {
                                anchors.centerIn: parent
                                text: qsTr("theirs")
                                color: root.mutedColor
                                font.family: appTheme.monoFontFamily
                                font.pixelSize: appTheme.fontSizeCaption
                            }
                        }
                    }

                    EditorMergeJsonValue {
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        value: root.conflict.incomingValue
                        textColor: root.textColor
                        mutedColor: root.mutedColor
                        maxEntries: 3
                    }

                    EditorMergeChoiceButton {
                        id: incomingChoiceButton
                        objectName: "editorMergeConflictChoice"
                        choiceKind: "incoming"
                        selected: root.choice === "incoming"
                        label: qsTr("Use Incoming")
                        accessibleLabel: qsTr("Use Incoming value for %1")
                                          .arg(root.formatFieldTitle(root.fieldKey))
                        onChoiceSelected: (selectedChoice) => root.choiceSelected(selectedChoice)
                    }
                }
            }
        }

        Rectangle {
            objectName: "editorMergeResolvedValueCard"
            Layout.fillWidth: true
            Layout.leftMargin: appTheme.spaceSm
            Layout.rightMargin: appTheme.spaceSm
            Layout.bottomMargin: appTheme.spaceSm
            Layout.preferredHeight: appTheme.spaceXl * 3
            color: root.resolved
                   ? (root.choice === "current"
                      ? appTheme.mergeCurrentFillColor : appTheme.mergeIncomingFillColor)
                   : appTheme.bgBaseColor
            radius: appTheme.controlRadiusSmall
            border.width: 1
            border.color: root.resolved ? root.mergedColor : root.borderColor

            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: appTheme.spaceSm
                anchors.rightMargin: appTheme.spaceSm
                spacing: appTheme.spaceSm

                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: 0

                    Label {
                        Layout.fillWidth: true
                        text: qsTr("MERGED RESULT")
                        color: root.resolved ? root.mergedColor : root.mutedColor
                        font.family: appTheme.uiFontFamily
                        font.pixelSize: appTheme.fontSizeCaption
                        font.weight: appTheme.fontWeightStrong
                    }

                    Label {
                        id: mergedValueLabel
                        objectName: "editorMergeResolvedValue"
                        Layout.fillWidth: true
                        text: root.resolved
                              ? root.formatValue(root.mergedValue)
                              : qsTr("Choose Current or Incoming")
                        color: root.resolved ? root.textColor : root.mutedColor
                        elide: Text.ElideRight
                        font.family: appTheme.monoFontFamily
                        font.pixelSize: appTheme.fontSizeBody
                    }
                }

                Label {
                    Layout.preferredWidth: appTheme.spaceXl * 5
                    text: root.resolved ? qsTr("Ready to commit") : qsTr("Pending")
                    color: root.resolved ? root.mergedColor : root.mutedColor
                    horizontalAlignment: Text.AlignRight
                    font.family: appTheme.uiFontFamily
                    font.pixelSize: appTheme.fontSizeCaption
                    font.weight: appTheme.fontWeightStrong
                }
            }
        }
    }
}
