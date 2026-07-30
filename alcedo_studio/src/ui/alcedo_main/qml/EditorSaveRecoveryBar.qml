import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Alcedo.Main 1.0

// Keeps the last valid editor frame visible while a failed save blocks the
// pending image or Version navigation. The session backend owns the recovery
// state; this component only presents the exact error and its three actions.
Item {
    id: root
    objectName: "editorSaveRecoveryBar"

    property var editorSession: null

    readonly property bool recoveryPending: !!(editorSession
                                               && editorSession.actions
                                               && (editorSession.actions.canRetrySave
                                                   || editorSession.actions.canDiscardAndContinue
                                                   || editorSession.actions.canCancelPendingNavigation))
    readonly property string failureDetail: editorSession
                                            ? String(editorSession.lastError || "")
                                            : ""

    visible: recoveryPending
    implicitHeight: appTheme.spaceXl * 3 + appTheme.spaceSm
    Layout.fillWidth: true
    Layout.preferredHeight: visible ? implicitHeight : 0

    Rectangle {
        anchors.fill: parent
        radius: appTheme.controlRadiusSmall
        color: appTheme.dangerTintColor
        border.width: 1
        border.color: appTheme.dangerColor

        RowLayout {
            anchors.fill: parent
            anchors.leftMargin: appTheme.spaceMd
            anchors.rightMargin: appTheme.spaceMd
            spacing: appTheme.spaceSm

            ColumnLayout {
                Layout.fillWidth: true
                spacing: appTheme.spaceXs

                Label {
                    objectName: "editorRecoveryTitle"
                    Layout.fillWidth: true
                    text: qsTr("Editor save needs attention")
                    color: appTheme.textColor
                    font.family: appTheme.uiFontFamily
                    font.pixelSize: appTheme.fontSizeTitle
                    font.weight: appTheme.fontWeightHeading
                    elide: Text.ElideRight
                }

                Label {
                    objectName: "editorRecoveryDetail"
                    Layout.fillWidth: true
                    text: root.failureDetail.length > 0
                          ? root.failureDetail
                          : qsTr("Editor changes could not be saved.")
                    color: appTheme.textMutedColor
                    font.family: appTheme.uiFontFamily
                    font.pixelSize: appTheme.fontSizeCaption
                    maximumLineCount: 2
                    wrapMode: Text.WordWrap
                    elide: Text.ElideRight
                }
            }

            DialogActionButton {
                objectName: "editorRecoveryRetryButton"
                text: qsTr("Retry Save")
                kind: "warning"
                enabled: !!(root.editorSession && root.editorSession.actions
                            && root.editorSession.actions.canRetrySave)
                buttonWidth: appTheme.spaceXl * 5
                buttonHeight: appTheme.spaceXl * 2 + appTheme.spaceSm
                buttonRadius: appTheme.controlRadiusSmall
                Accessible.name: text
                onClicked: root.editorSession.RetrySave()
            }

            DialogActionButton {
                objectName: "editorRecoveryDiscardButton"
                text: qsTr("Discard and Continue")
                kind: "normal"
                enabled: !!(root.editorSession && root.editorSession.actions
                            && root.editorSession.actions.canDiscardAndContinue)
                buttonWidth: appTheme.spaceXl * 9
                buttonHeight: appTheme.spaceXl * 2 + appTheme.spaceSm
                buttonRadius: appTheme.controlRadiusSmall
                Accessible.name: text
                onClicked: root.editorSession.DiscardAndContinue()
            }

            DialogActionButton {
                objectName: "editorRecoveryCancelButton"
                text: qsTr("Cancel")
                kind: "normal"
                enabled: !!(root.editorSession && root.editorSession.actions
                            && root.editorSession.actions.canCancelPendingNavigation)
                buttonWidth: appTheme.spaceXl * 4
                buttonHeight: appTheme.spaceXl * 2 + appTheme.spaceSm
                buttonRadius: appTheme.controlRadiusSmall
                Accessible.name: text
                onClicked: root.editorSession.CancelPendingNavigation()
            }
        }
    }
}
