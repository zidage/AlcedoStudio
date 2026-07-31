pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Effects
import QtQuick.Layouts

// Merge conflict resolver. The transfer service supplies the two candidate
// values; this component owns only the visible choices and preview state.
// Layout: title + Cancel/Complete on one axis → scrollable conflict rows →
// sticky Use All Current / Incoming aligned to those columns.
Dialog {
    id: root
    objectName: "editorMergeDialog"

    property var conflicts: []
    property color textColor: appTheme.textColor
    property color mutedColor: appTheme.textMutedColor
    property color surfaceColor: appTheme.cardSurfaceColor
    property color borderColor: appTheme.cardBorderColor
    property Item blurSource: null
    property real cornerRadius: 0
    property int resolvedCount: 0

    readonly property int pendingCount: Math.max(0, conflicts.length - resolvedCount)
    readonly property bool allResolved: conflicts.length > 0
                                        && resolvedCount === conflicts.length

    signal mergeRequested(var resolutions)
    signal cancelled()

    parent: Overlay.overlay
    modal: true
    focus: true
    title: ""
    padding: 0
    closePolicy: Popup.CloseOnEscape
    width: Math.min(parent ? parent.width - appTheme.spaceXl * 2
                           : appTheme.editorMergeDialogWidth,
                    appTheme.editorMergeDialogWidth)
    x: parent ? Math.round((parent.width - width) / 2) : 0
    y: parent ? Math.round((parent.height - height) / 2) : 0
    footer: Item {
        width: 1
        height: 0
    }

    background: Rectangle {
        radius: appTheme.panelRadius
        color: root.surfaceColor
        border.width: 1
        border.color: root.borderColor
    }

    Overlay.modal: Item {
        anchors.fill: parent

        Rectangle {
            id: backdropMask
            anchors.fill: parent
            radius: root.cornerRadius
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
                maskEnabled: root.cornerRadius > 0
                maskSource: backdropMask
            }

            MultiEffect {
                anchors.fill: parent
                source: root.blurSource
                blurEnabled: root.blurSource !== null
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
                choice: choice.choice,
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
        spacing: 0

        // Title + Cancel / Complete on one vertical center axis.
        RowLayout {
            id: headerRow
            objectName: "editorMergeHeader"
            Layout.fillWidth: true
            Layout.leftMargin: appTheme.spaceLg
            Layout.rightMargin: appTheme.spaceLg
            Layout.topMargin: appTheme.spaceLg
            Layout.bottomMargin: appTheme.spaceMd
            spacing: appTheme.spaceMd

            Label {
                Layout.fillWidth: true
                Layout.alignment: Qt.AlignVCenter
                text: qsTr("Merge Conflicts")
                color: root.textColor
                elide: Text.ElideRight
                font.family: appTheme.uiFontFamily
                font.pixelSize: appTheme.fontSizeHeadline
                font.weight: appTheme.fontWeightHeading
            }

            Button {
                id: cancelButton
                objectName: "editorMergeCancelButton"
                Layout.alignment: Qt.AlignVCenter
                Layout.preferredWidth: Math.max(implicitWidth, appTheme.spaceXl * 5)
                Layout.preferredHeight: appTheme.spaceXl * 2
                text: qsTr("Cancel")
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
                    radius: appTheme.controlRadius
                    color: cancelButton.down
                           ? appTheme.buttonPressedFillColor
                           : (cancelButton.hovered
                              ? appTheme.buttonHoveredFillColor : "transparent")
                    border.width: cancelButton.hovered ? 1 : 0
                    border.color: root.borderColor
                }
            }

            Button {
                id: completeButton
                objectName: "editorMergeAcceptButton"
                Layout.alignment: Qt.AlignVCenter
                Layout.preferredWidth: Math.max(implicitWidth, appTheme.spaceXl * 7)
                Layout.preferredHeight: appTheme.spaceXl * 2
                enabled: root.allResolved
                text: qsTr("Complete")
                hoverEnabled: true
                activeFocusOnTab: true
                Accessible.name: text
                onClicked: root.accept()

                contentItem: Label {
                    text: completeButton.text
                    color: completeButton.enabled
                           ? appTheme.editorListSelectedInkColor : root.mutedColor
                    font.family: appTheme.uiFontFamily
                    font.pixelSize: appTheme.fontSizeCaption
                    font.weight: appTheme.fontWeightHeading
                    horizontalAlignment: Text.AlignHCenter
                    verticalAlignment: Text.AlignVCenter
                    elide: Text.ElideRight
                }

                background: Rectangle {
                    radius: appTheme.controlRadius
                    color: completeButton.enabled
                           ? (completeButton.down
                              ? appTheme.buttonPressedFillColor
                              : (completeButton.hovered
                                 ? appTheme.buttonHoveredFillColor
                                 : appTheme.editorListSelectedFillColor))
                           : appTheme.disabledSurfaceColor
                    border.width: 1
                    border.color: completeButton.enabled
                                  ? appTheme.editorListSelectedFillColor : root.borderColor
                }
            }
        }

        // Scrollable conflict rows.
        Item {
            objectName: "editorMergeConflictListWell"
            Layout.fillWidth: true
            Layout.preferredHeight: Math.min(Math.max(conflictList.contentHeight
                                                       + appTheme.spaceSm,
                                                       appTheme.spaceXl * 14),
                                             appTheme.spaceXl * 28)
            Layout.leftMargin: appTheme.spaceLg
            Layout.rightMargin: appTheme.spaceLg
            clip: true

            ListView {
                id: conflictList
                objectName: "editorMergeConflictList"
                anchors.fill: parent
                clip: true
                model: conflictChoices
                spacing: appTheme.spaceXl
                boundsBehavior: Flickable.StopAtBounds

                delegate: Item {
                    required property var conflict
                    required property string choice
                    required property int index

                    width: ListView.view ? ListView.view.width : 0
                    height: conflictCard.implicitHeight

                    EditorMergeConflictCard {
                        id: conflictCard
                        width: parent.width
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

        // Sticky bulk actions — centered under the scrollable conflict list.
        RowLayout {
            id: bulkActionRow
            objectName: "editorMergeBulkActionRow"
            Layout.fillWidth: true
            Layout.leftMargin: appTheme.spaceLg
            Layout.rightMargin: appTheme.spaceLg
            Layout.topMargin: appTheme.spaceMd
            Layout.bottomMargin: appTheme.spaceLg
            spacing: appTheme.spaceSm

            Item { Layout.fillWidth: true }

            Button {
                id: useAllCurrentButton
                objectName: "editorMergeUseAllCurrentButton"
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
                    leftPadding: appTheme.spaceMd
                    rightPadding: appTheme.spaceMd
                }

                background: Rectangle {
                    radius: appTheme.controlRadius
                    color: useAllCurrentButton.down
                           ? appTheme.buttonPressedFillColor
                           : (useAllCurrentButton.hovered
                              ? appTheme.mergeCurrentFillColor : appTheme.bgBaseColor)
                    border.width: 1
                    border.color: appTheme.mergeCurrentColor
                }
            }

            Button {
                id: useAllIncomingButton
                objectName: "editorMergeUseAllIncomingButton"
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
                    leftPadding: appTheme.spaceMd
                    rightPadding: appTheme.spaceMd
                }

                background: Rectangle {
                    radius: appTheme.controlRadius
                    color: useAllIncomingButton.down
                           ? appTheme.buttonPressedFillColor
                           : (useAllIncomingButton.hovered
                              ? appTheme.mergeIncomingFillColor : appTheme.bgBaseColor)
                    border.width: 1
                    border.color: appTheme.mergeIncomingColor
                }
            }

            Item { Layout.fillWidth: true }
        }
    }
}
