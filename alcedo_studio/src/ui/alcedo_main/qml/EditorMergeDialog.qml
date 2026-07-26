import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Per-field merge resolution dialog. The transfer service provides the
// conflict values; this component only records the user's visible choices.
Dialog {
    id: root
    objectName: "editorMergeDialog"

    property var conflicts: []
    property color textColor: appTheme.textColor
    property color mutedColor: appTheme.textMutedColor
    property color surfaceColor: appTheme.cardSurfaceColor
    property color borderColor: appTheme.cardBorderColor

    signal mergeRequested(var resolutions)
    signal cancelled()

    modal: true
    title: qsTr("Resolve merge fields")
    width: appTheme.editorMergeDialogWidth

    footer: RowLayout {
        width: parent.width
        spacing: appTheme.spaceSm

        Item { Layout.fillWidth: true }

        DialogActionButton {
            objectName: "editorMergeCancelButton"
            text: qsTr("Cancel")
            kind: "normal"
            buttonWidth: appTheme.spaceXl * 4
            buttonHeight: appTheme.spaceXl * 2 + appTheme.spaceSm
            buttonRadius: appTheme.controlRadiusSmall
            onClicked: root.reject()
        }

        DialogActionButton {
            objectName: "editorMergeAcceptButton"
            text: qsTr("OK")
            kind: "normal"
            buttonWidth: appTheme.spaceXl * 4
            buttonHeight: appTheme.spaceXl * 2 + appTheme.spaceSm
            buttonRadius: appTheme.controlRadiusSmall
            onClicked: root.accept()
        }
    }

    function openPreview(preview) {
        conflicts = preview && preview.conflicts ? preview.conflicts : []
        open()
    }

    onRejected: cancelled()
    onAccepted: {
        var resolutions = []
        for (var index = 0; index < conflictChoices.count; ++index) {
            var choice = conflictChoices.get(index)
            var conflict = choice.conflict
            resolutions.push({
                fieldKey: conflict.fieldKey,
                resolvedValue: choice.useIncoming ? conflict.incomingValue : conflict.currentValue,
                resolvedEnabled: choice.useIncoming
                                  ? Boolean(conflict.incomingEnabled)
                                  : Boolean(conflict.currentEnabled)
            })
        }
        mergeRequested(resolutions)
    }

    ListModel {
        id: conflictChoices
        function resetFrom(source) {
            clear()
            for (var index = 0; index < source.length; ++index) {
                append({conflict: source[index], useIncoming: false})
            }
        }
    }

    onConflictsChanged: conflictChoices.resetFrom(conflicts)

    contentItem: ColumnLayout {
        spacing: appTheme.spaceMd

        Label {
            Layout.fillWidth: true
            text: qsTr("Choose the value that should be published for every changed field.")
            color: root.mutedColor
            wrapMode: Text.WordWrap
            font.family: appTheme.uiFontFamily
            font.pixelSize: appTheme.fontSizeBody
        }

        ListView {
            id: conflictList
            objectName: "editorMergeConflictList"
            Layout.fillWidth: true
            Layout.preferredHeight: Math.min(contentHeight, appTheme.spaceXl * 8)
            clip: true
            model: conflictChoices
            spacing: appTheme.spaceXs

            delegate: Rectangle {
                objectName: "editorMergeConflictRow"
                width: conflictList.width
                height: appTheme.spaceXl * 2
                radius: appTheme.controlRadiusSmall
                color: root.surfaceColor
                border.width: 1
                border.color: root.borderColor

                RowLayout {
                    anchors.fill: parent
                    anchors.margins: appTheme.spaceSm
                    spacing: appTheme.spaceSm

                    Label {
                        Layout.fillWidth: true
                        text: conflict.fieldKey
                        color: root.textColor
                        elide: Text.ElideRight
                        font.family: appTheme.uiFontFamily
                        font.pixelSize: appTheme.fontSizeBody
                    }

                    ComboBox {
                        id: conflictChoice
                        objectName: "editorMergeConflictChoice"
                        Layout.preferredWidth: appTheme.spaceXl * 3
                        model: [qsTr("Current"), qsTr("Incoming")]
                        currentIndex: useIncoming ? 1 : 0
                        activeFocusOnTab: true
                        background: Rectangle {
                            implicitHeight: appTheme.spaceXl + appTheme.spaceSm
                            radius: appTheme.controlRadiusSmall
                            color: appTheme.bgBaseColor
                            border.width: 1
                            border.color: conflictChoice.activeFocus || conflictChoice.hovered
                                          ? root.mutedColor
                                          : root.borderColor
                        }
                        contentItem: Text {
                            leftPadding: appTheme.spaceSm
                            rightPadding: appTheme.spaceMd + appTheme.spaceSm
                            text: conflictChoice.displayText
                            color: root.textColor
                            font.family: appTheme.uiFontFamily
                            font.pixelSize: appTheme.fontSizeBody
                            verticalAlignment: Text.AlignVCenter
                            elide: Text.ElideRight
                        }
                        indicator: Item {
                            x: conflictChoice.width - width - appTheme.spaceSm
                            y: conflictChoice.topPadding
                               + (conflictChoice.availableHeight - height) / 2
                            width: appTheme.spaceSm + appTheme.spaceXs
                            height: appTheme.spaceSm + appTheme.spaceXs
                            Text {
                                anchors.centerIn: parent
                                text: "▾"
                                color: root.mutedColor
                                font.pixelSize: appTheme.fontSizeCaption
                            }
                        }
                        popup: Popup {
                            y: conflictChoice.height + appTheme.spaceXs
                            width: conflictChoice.width
                            implicitHeight: Math.min(contentItem.implicitHeight + appTheme.spaceXs,
                                                     appTheme.spaceXl * 6)
                            padding: appTheme.spaceXs / 2

                            background: Rectangle {
                                radius: appTheme.controlRadiusSmall
                                color: appTheme.bgBaseColor
                                border.width: 1
                                border.color: root.borderColor
                            }

                            contentItem: ListView {
                                clip: true
                                implicitHeight: contentHeight
                                model: conflictChoice.popup.visible
                                       ? conflictChoice.delegateModel : null
                                currentIndex: conflictChoice.highlightedIndex
                            }
                        }
                        delegate: ItemDelegate {
                            id: conflictChoiceDelegate
                            width: conflictChoice.width
                            height: appTheme.spaceXl + appTheme.spaceSm
                            text: modelData
                            highlighted: conflictChoice.highlightedIndex === index
                            background: Rectangle {
                                radius: appTheme.controlRadiusSmall
                                color: conflictChoiceDelegate.highlighted
                                       ? appTheme.editorListSelectedFillColor
                                       : (conflictChoiceDelegate.hovered
                                          ? appTheme.buttonHoveredFillColor
                                          : "transparent")
                            }
                            contentItem: Text {
                                text: conflictChoiceDelegate.text
                                color: conflictChoiceDelegate.highlighted
                                       ? appTheme.editorListSelectedInkColor
                                       : root.textColor
                                font.family: appTheme.uiFontFamily
                                font.pixelSize: appTheme.fontSizeBody
                                elide: Text.ElideRight
                                verticalAlignment: Text.AlignVCenter
                                leftPadding: appTheme.spaceSm
                            }
                        }
                        onActivated: useIncoming = currentIndex === 1
                    }
                }
            }
        }
    }
}
