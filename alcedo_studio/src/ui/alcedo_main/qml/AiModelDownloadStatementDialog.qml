import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts

// The download statement of the external AI models (AI Models plan, section 4, version 1).
// Settings > AI Models opens it before the first download of any model ("Accept and Download"
// and "Cancel") and read-only from its "Download statement" button ("Close"). The page records
// the acceptance through AiModelPackageService::acceptDownloadStatement.
Popup {
    id: root
    objectName: "aiModelDownloadStatementDialog"

    // True: the statement asks for acceptance before a download. False: read-only.
    property bool requestsAcceptance: false

    signal acceptRequested()
    signal declineRequested()

    readonly property string datasetUrl: "https://github.com/xiechenxi99/mass13k"

    function openForDownload() {
        requestsAcceptance = true
        open()
    }

    function openReadOnly() {
        requestsAcceptance = false
        open()
    }

    parent: Overlay.overlay
    modal: true
    focus: true
    closePolicy: Popup.CloseOnEscape
    anchors.centerIn: parent
    width: parent ? Math.min(parent.width - appTheme.spaceXl * 2, 600) : 600
    padding: appTheme.spaceLg

    // Escape or a click outside a requesting statement is a Cancel.
    onClosed: {
        if (requestsAcceptance) {
            requestsAcceptance = false
            declineRequested()
        }
    }

    Overlay.modal: Rectangle {
        color: appTheme.overlayColor
    }

    background: Rectangle {
        radius: appTheme.panelRadius
        color: appTheme.cardSurfaceColor
        border.width: 1
        border.color: appTheme.cardBorderColor
    }

    contentItem: ColumnLayout {
        spacing: appTheme.spaceMd

        Label {
            objectName: "aiModelDownloadStatementTitle"
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            text: qsTr("Before you download")
            color: appTheme.textColor
            font.family: appTheme.headlineFontFamily
            font.pixelSize: appTheme.fontSizeSection
            font.weight: appTheme.fontWeightHeading
        }

        Label {
            objectName: "aiModelDownloadStatementGeneral"
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            text: qsTr("The AI models on this page are not part of the Alcedo Studio installer. Alcedo Studio downloads them to this computer from third-party sites or from the Alcedo download server. Each model has its own license. The license names are on the model cards, and the AI Mask package contains the full license texts. You decide whether the models and their output can be used for your purpose, including commercial use.")
            color: appTheme.textMutedColor
            font.family: appTheme.uiFontFamily
            font.pixelSize: appTheme.fontSizeBody
            lineHeight: 1.2
        }

        Label {
            Layout.fillWidth: true
            text: qsTr("AI Mask")
            color: appTheme.textColor
            font.family: appTheme.uiFontFamily
            font.pixelSize: appTheme.fontSizeBody
            font.weight: appTheme.fontWeightStrong
        }

        Label {
            objectName: "aiModelDownloadStatementAiMask"
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            textFormat: Text.StyledText
            text: qsTr("The Sky model and the people and sky locator are trained by Alcedo Studio. The Sky model is trained on the MaSS13K dataset (%1), which does not state a clear license. For commercial use of the Sky model or its output, contact the authors of the dataset. Alcedo Studio does not guarantee that the models it trains can be used for commercial work. The Subject model (S3OD) is trained on generated images and on labels from other datasets; see the NOTICE file of the package.")
                  .arg("<a href=\"" + root.datasetUrl + "\">" + root.datasetUrl + "</a>")
            linkColor: appTheme.textColor
            color: appTheme.textMutedColor
            font.family: appTheme.uiFontFamily
            font.pixelSize: appTheme.fontSizeBody
            lineHeight: 1.2
            onLinkActivated: function(link) {
                Qt.openUrlExternally(link)
            }

            HoverHandler {
                cursorShape: parent.hoveredLink.length > 0 ? Qt.PointingHandCursor : Qt.ArrowCursor
            }
        }

        Label {
            objectName: "aiModelDownloadStatementConfirmation"
            Layout.fillWidth: true
            visible: root.requestsAcceptance
            wrapMode: Text.WordWrap
            text: qsTr("Select \"Accept and Download\" to confirm that you have read this statement.")
            color: appTheme.textColor
            font.family: appTheme.uiFontFamily
            font.pixelSize: appTheme.fontSizeBody
        }

        Flow {
            Layout.fillWidth: true
            Layout.topMargin: appTheme.spaceSm
            layoutDirection: Qt.RightToLeft
            spacing: appTheme.spaceSm

            DialogActionButton {
                objectName: "aiModelDownloadStatementAcceptButton"
                visible: root.requestsAcceptance
                text: qsTr("Accept and Download")
                kind: "accent"
                buttonWidth: Math.max(appTheme.spaceXl * 5, implicitContentWidth + appTheme.spaceLg * 2)
                buttonHeight: appTheme.iconButtonHitSizeCompact
                buttonRadius: appTheme.controlRadiusSmall
                Accessible.name: text
                onClicked: {
                    // Closing first keeps onClosed from reporting a Cancel.
                    root.requestsAcceptance = false
                    root.close()
                    root.acceptRequested()
                }
            }

            DialogActionButton {
                objectName: "aiModelDownloadStatementCancelButton"
                visible: root.requestsAcceptance
                text: qsTr("Cancel")
                kind: "normal"
                buttonWidth: Math.max(appTheme.spaceXl * 4, implicitContentWidth + appTheme.spaceLg * 2)
                buttonHeight: appTheme.iconButtonHitSizeCompact
                buttonRadius: appTheme.controlRadiusSmall
                Accessible.name: text
                onClicked: root.close()
            }

            DialogActionButton {
                objectName: "aiModelDownloadStatementCloseButton"
                visible: !root.requestsAcceptance
                text: qsTr("Close")
                kind: "normal"
                buttonWidth: Math.max(appTheme.spaceXl * 4, implicitContentWidth + appTheme.spaceLg * 2)
                buttonHeight: appTheme.iconButtonHitSizeCompact
                buttonRadius: appTheme.controlRadiusSmall
                Accessible.name: text
                onClicked: root.close()
            }
        }
    }
}
