pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts

// Merge conflict resolver. The transfer service supplies the two candidate
// values; this component owns only the visible choices and preview state.
Dialog {
    id: root
    objectName: "editorMergeDialog"

    property var conflicts: []
    property color textColor: appTheme.textColor
    property color mutedColor: appTheme.textMutedColor
    property color surfaceColor: appTheme.cardSurfaceColor
    property color borderColor: appTheme.cardBorderColor
    property int resolvedCount: 0

    readonly property int pendingCount: Math.max(0, conflicts.length - resolvedCount)
    readonly property bool allResolved: conflicts.length > 0
                                        && resolvedCount === conflicts.length

    signal mergeRequested(var resolutions)
    signal cancelled()

    modal: true
    title: ""
    width: appTheme.editorMergeDialogWidth
    padding: 0

    background: Rectangle {
        radius: appTheme.panelRadius
        color: root.surfaceColor
        border.width: 1
        border.color: root.borderColor
    }

    function countResolvedChoices() {
        var count = 0
        for (var index = 0; index < conflictChoices.count; ++index) {
            var choice = conflictChoices.get(index).choice
            if (choice === "current" || choice === "incoming")
                ++count
        }
        return count
    }

    function setChoice(index, value) {
        if (index < 0 || index >= conflictChoices.count)
            return
        conflictChoices.setProperty(index, "choice", value)
        root.resolvedCount = root.countResolvedChoices()
    }

    function setAllChoices(value) {
        for (var index = 0; index < conflictChoices.count; ++index)
            conflictChoices.setProperty(index, "choice", value)
        root.resolvedCount = root.countResolvedChoices()
    }

    function resetChoices(source) {
        conflictChoices.clear()
        var entries = source || []
        for (var index = 0; index < entries.length; ++index)
            conflictChoices.append({conflict: entries[index], choice: ""})
        root.resolvedCount = 0
    }

    function openPreview(preview) {
        root.conflicts = preview && preview.conflicts ? preview.conflicts : []
        root.open()
    }

    onConflictsChanged: root.resetChoices(root.conflicts)

    onRejected: root.cancelled()
    onAccepted: {
        if (!root.allResolved)
            return

        var resolutions = []
        for (var index = 0; index < conflictChoices.count; ++index) {
            var choice = conflictChoices.get(index)
            var conflict = choice.conflict
            var useIncoming = choice.choice === "incoming"
            resolutions.push({
                fieldKey: conflict.fieldKey,
                resolvedValue: useIncoming ? conflict.incomingValue : conflict.currentValue,
                resolvedEnabled: useIncoming
                                  ? Boolean(conflict.incomingEnabled)
                                  : Boolean(conflict.currentEnabled)
            })
        }
        root.mergeRequested(resolutions)
    }

    ListModel {
        id: conflictChoices
    }

    contentItem: ColumnLayout {
        id: contentLayout
        spacing: appTheme.spaceMd

        Rectangle {
            objectName: "editorMergeHeader"
            Layout.fillWidth: true
            Layout.preferredHeight: appTheme.spaceXl * 7
            color: root.surfaceColor

            ColumnLayout {
                anchors.fill: parent
                anchors.margins: appTheme.spaceLg
                spacing: appTheme.spaceSm

                RowLayout {
                    Layout.fillWidth: true
                    spacing: appTheme.spaceSm

                    Rectangle {
                        Layout.preferredWidth: appTheme.spaceSm
                        Layout.preferredHeight: appTheme.spaceSm
                        radius: appTheme.spaceSm / 2
                        color: appTheme.mergeCurrentColor
                    }

                    Label {
                        Layout.fillWidth: true
                        text: qsTr("Action Required")
                        color: appTheme.mergeCurrentColor
                        font.family: appTheme.uiFontFamily
                        font.pixelSize: appTheme.fontSizeCaption
                        font.weight: appTheme.fontWeightStrong
                    }

                    Label {
                        text: qsTr("%1 conflicts").arg(root.conflicts.length)
                        color: root.mutedColor
                        font.family: appTheme.monoFontFamily
                        font.pixelSize: appTheme.fontSizeCaption
                    }
                }

                RowLayout {
                    Layout.fillWidth: true
                    spacing: appTheme.spaceMd

                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: appTheme.spaceXs

                        Label {
                            Layout.fillWidth: true
                            text: qsTr("Merge Into Current Version")
                            color: root.textColor
                            font.family: appTheme.uiFontFamily
                            font.pixelSize: appTheme.fontSizeHeadline
                            font.weight: appTheme.fontWeightHeading
                        }

                        Label {
                            Layout.fillWidth: true
                            text: qsTr("Choose a value for every conflicting parameter. The Merged column previews what will be committed.")
                            color: root.mutedColor
                            wrapMode: Text.WordWrap
                            font.family: appTheme.uiFontFamily
                            font.pixelSize: appTheme.fontSizeCaption
                        }
                    }

                    RowLayout {
                        Layout.alignment: Qt.AlignVCenter
                        spacing: appTheme.spaceXs

                        Button {
                            id: useAllCurrentButton
                            objectName: "editorMergeUseAllCurrentButton"
                            Layout.preferredWidth: appTheme.spaceXl * 7
                            Layout.preferredHeight: appTheme.spaceXl * 2
                            text: qsTr("Use All Current")
                            hoverEnabled: true
                            activeFocusOnTab: true
                            Accessible.name: text
                            onClicked: root.setAllChoices("current")

                            contentItem: Label {
                                text: useAllCurrentButton.text
                                color: appTheme.mergeCurrentColor
                                font.family: appTheme.uiFontFamily
                                font.pixelSize: appTheme.fontSizeCaption
                                font.weight: appTheme.fontWeightStrong
                                horizontalAlignment: Text.AlignHCenter
                                verticalAlignment: Text.AlignVCenter
                                elide: Text.ElideRight
                            }

                            background: Rectangle {
                                radius: appTheme.controlRadiusSmall
                                color: useAllCurrentButton.hovered
                                       ? appTheme.buttonHoveredFillColor : root.surfaceColor
                                border.width: 1
                                border.color: appTheme.mergeCurrentColor
                            }
                        }

                        Button {
                            id: useAllIncomingButton
                            objectName: "editorMergeUseAllIncomingButton"
                            Layout.preferredWidth: appTheme.spaceXl * 7
                            Layout.preferredHeight: appTheme.spaceXl * 2
                            text: qsTr("Use All Incoming")
                            hoverEnabled: true
                            activeFocusOnTab: true
                            Accessible.name: text
                            onClicked: root.setAllChoices("incoming")

                            contentItem: Label {
                                text: useAllIncomingButton.text
                                color: appTheme.mergeIncomingColor
                                font.family: appTheme.uiFontFamily
                                font.pixelSize: appTheme.fontSizeCaption
                                font.weight: appTheme.fontWeightStrong
                                horizontalAlignment: Text.AlignHCenter
                                verticalAlignment: Text.AlignVCenter
                                elide: Text.ElideRight
                            }

                            background: Rectangle {
                                radius: appTheme.controlRadiusSmall
                                color: useAllIncomingButton.hovered
                                       ? appTheme.buttonHoveredFillColor : root.surfaceColor
                                border.width: 1
                                border.color: appTheme.mergeIncomingColor
                            }
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

        ListView {
            id: conflictList
            objectName: "editorMergeConflictList"
            Layout.fillWidth: true
            Layout.preferredHeight: Math.min(Math.max(contentHeight, appTheme.spaceXl * 10),
                                             appTheme.spaceXl * 24)
            clip: true
            model: conflictChoices
            spacing: appTheme.spaceSm
            boundsBehavior: Flickable.StopAtBounds

            delegate: Item {
                required property var conflict
                required property string choice
                required property int index

                width: ListView.view ? ListView.view.width : 0
                height: appTheme.spaceXl * 10

                EditorMergeConflictCard {
                    id: conflictCard
                    anchors.fill: parent
                    conflict: parent.conflict
                    choice: parent.choice
                    rowIndex: parent.index
                    textColor: root.textColor
                    mutedColor: root.mutedColor
                    surfaceColor: root.surfaceColor
                    borderColor: root.borderColor
                    onChoiceSelected: (selectedChoice) => root.setChoice(conflictCard.rowIndex,
                                                                          selectedChoice)
                }
            }

            Label {
                anchors.centerIn: parent
                visible: conflictChoices.count === 0
                text: qsTr("No merge conflicts to resolve")
                color: root.mutedColor
                font.family: appTheme.uiFontFamily
                font.pixelSize: appTheme.fontSizeBody
            }
        }
    }

    footer: Rectangle {
        implicitHeight: appTheme.spaceXl * 3
        color: root.surfaceColor

        Rectangle {
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: parent.top
            height: 1
            color: root.borderColor
        }

        RowLayout {
            anchors.fill: parent
            anchors.margins: appTheme.spaceLg
            spacing: appTheme.spaceMd

            Button {
                id: cancelButton
                objectName: "editorMergeCancelButton"
                Layout.preferredWidth: appTheme.spaceXl * 5
                Layout.preferredHeight: appTheme.spaceXl * 2
                text: qsTr("Cancel Merge")
                hoverEnabled: true
                activeFocusOnTab: true
                Accessible.name: text
                onClicked: root.reject()

                contentItem: Label {
                    text: cancelButton.text
                    color: cancelButton.hovered ? root.textColor : root.mutedColor
                    font.family: appTheme.uiFontFamily
                    font.pixelSize: appTheme.fontSizeCaption
                    font.weight: appTheme.fontWeightStrong
                    horizontalAlignment: Text.AlignHCenter
                    verticalAlignment: Text.AlignVCenter
                }

                background: Rectangle {
                    radius: appTheme.controlRadiusSmall
                    color: cancelButton.hovered
                           ? appTheme.buttonHoveredFillColor : root.surfaceColor
                    border.width: 1
                    border.color: root.borderColor
                }
            }

            Item { Layout.fillWidth: true }

            Label {
                objectName: "editorMergeStatusText"
                text: root.allResolved
                      ? qsTr("Ready to merge")
                      : qsTr("%1 conflicts pending resolution").arg(root.pendingCount)
                color: root.allResolved ? appTheme.mergeIncomingColor : root.mutedColor
                font.family: appTheme.uiFontFamily
                font.pixelSize: appTheme.fontSizeCaption
                horizontalAlignment: Text.AlignRight
                verticalAlignment: Text.AlignVCenter
            }

            Button {
                id: completeButton
                objectName: "editorMergeAcceptButton"
                Layout.preferredWidth: appTheme.spaceXl * 7
                Layout.preferredHeight: appTheme.spaceXl * 2
                enabled: root.allResolved
                text: qsTr("Complete Merge")
                hoverEnabled: true
                activeFocusOnTab: true
                Accessible.name: text
                onClicked: root.accept()

                contentItem: Label {
                    text: completeButton.text
                    color: completeButton.enabled ? root.surfaceColor : root.mutedColor
                    font.family: appTheme.uiFontFamily
                    font.pixelSize: appTheme.fontSizeCaption
                    font.weight: appTheme.fontWeightHeading
                    horizontalAlignment: Text.AlignHCenter
                    verticalAlignment: Text.AlignVCenter
                    elide: Text.ElideRight
                }

                background: Rectangle {
                    radius: appTheme.controlRadiusSmall
                    color: completeButton.enabled
                           ? appTheme.mergeIncomingColor : appTheme.disabledSurfaceColor
                    border.width: 1
                    border.color: completeButton.enabled
                                  ? appTheme.mergeIncomingColor : root.borderColor
                }
            }
        }
    }
}
