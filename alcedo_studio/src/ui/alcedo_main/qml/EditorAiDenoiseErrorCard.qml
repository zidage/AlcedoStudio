import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// The AI Denoise error of the open photo in the center of the viewport (AI Denoise plan, DN7,
// section 2.4). `error` is EditorSessionController.aiDenoiseError: {kind, text}, empty without an
// error. Each kind has Turn Off AI Denoise; a missing model also has Manage (AI Models page).
Rectangle {
    id: root
    objectName: "editorAiDenoiseErrorCard"

    property var error: ({})
    property bool controlsEnabled: true

    signal manageRequested()
    signal turnOffRequested()

    readonly property string kind: error && error.kind !== undefined ? String(error.kind) : ""

    function titleText() {
        switch (root.kind) {
        case "model_not_installed":
            return qsTr("AI Denoise model not installed")
        case "device_too_slow":
            return qsTr("This GPU is too slow for AI Denoise")
        case "sensor_not_supported":
            return qsTr("AI Denoise does not support this sensor")
        default:
            return qsTr("AI Denoise failed")
        }
    }

    visible: root.kind.length > 0
    // As wide as the title and the buttons; the error text wraps.
    width: Math.min(parent ? parent.width - 2 * appTheme.spaceLg : implicitWidth, implicitWidth)
    implicitWidth: Math.max(titleLabel.implicitWidth, buttonRow.implicitWidth)
                   + 2 * appTheme.spaceMd
    implicitHeight: body.implicitHeight + 2 * appTheme.spaceMd
    height: implicitHeight
    radius: appTheme.panelRadius
    color: appTheme.bgDeepColor
    border.width: 1
    border.color: appTheme.cardBorderColor

    // The card takes the pointer input of its area, so the viewport below does not pan.
    MouseArea {
        anchors.fill: parent
        acceptedButtons: Qt.AllButtons
        onWheel: function(wheel) { wheel.accepted = true }
    }

    ColumnLayout {
        id: body
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.margins: appTheme.spaceMd
        spacing: appTheme.spaceSm

        Label {
            id: titleLabel
            objectName: "editorAiDenoiseErrorTitle"
            Layout.fillWidth: true
            text: root.titleText()
            color: appTheme.textColor
            font.pixelSize: appTheme.fontSizeBody
            font.weight: appTheme.fontWeightHeading
            wrapMode: Text.WordWrap
        }

        Label {
            objectName: "editorAiDenoiseErrorText"
            Layout.fillWidth: true
            text: root.error && root.error.text !== undefined ? String(root.error.text) : ""
            visible: text.length > 0
            color: appTheme.textMutedColor
            font.pixelSize: appTheme.fontSizeCaption
            wrapMode: Text.WordWrap
            maximumLineCount: 4
            elide: Text.ElideRight
        }

        RowLayout {
            id: buttonRow
            Layout.topMargin: appTheme.spaceXs
            spacing: appTheme.spaceSm

            DialogActionButton {
                objectName: "editorAiDenoiseManageButton"
                visible: root.kind === "model_not_installed"
                kind: "light"
                text: qsTr("Manage")
                buttonWidth: contentItem.implicitWidth + 2 * appTheme.spaceMd
                buttonHeight: appTheme.iconButtonHitSizeCompact
                buttonRadius: appTheme.controlRadius
                font.pixelSize: appTheme.fontSizeBody
                onClicked: root.manageRequested()
            }

            DialogActionButton {
                objectName: "editorAiDenoiseTurnOffButton"
                kind: root.kind === "model_not_installed" ? "normal" : "light"
                enabled: root.controlsEnabled
                text: qsTr("Turn Off AI Denoise")
                buttonWidth: contentItem.implicitWidth + 2 * appTheme.spaceMd
                buttonHeight: appTheme.iconButtonHitSizeCompact
                buttonRadius: appTheme.controlRadius
                font.pixelSize: appTheme.fontSizeBody
                onClicked: root.turnOffRequested()
            }
        }
    }
}
