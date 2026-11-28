import QtQuick
import QtQuick.Controls
import QtQuick.Controls.impl
import QtQuick.Layouts

// Render-time alert of the editor viewport (#1056, DESIGN.md "Viewport render alert"). The
// viewport blurs and dims (BlurredOverlay), and a centered column shows the photo-off icon, the
// title, one short sentence, Show Details with the full technical text (selectable, Copy, scrolls
// after viewportAlertDetailMaxLines lines), and one or two neutral buttons. The alert knows no
// feature: the caller maps its error to the title, the message and the detail.
Item {
    id: root
    objectName: "viewportRenderAlert"

    // The viewport stage to blur; it must share this item's parent.
    property Item blurSource: null
    property string title: ""
    property string message: ""
    // The raw error text, shown in full under Show Details.
    property string detail: ""
    // The main action: the light button at the right. Empty = no button.
    property string primaryText: ""
    property bool primaryEnabled: true
    // A second action left of the main one. Empty = no button.
    property string secondaryText: ""
    property bool secondaryEnabled: true
    property bool detailsOpen: false

    signal primaryClicked()
    signal secondaryClicked()

    function copyDetail() {
        detailText.selectAll()
        detailText.copy()
        detailText.deselect()
        copiedTimer.restart()
    }

    visible: title.length > 0
    // Each new error starts with its detail closed; the same error rendered again keeps it.
    onDetailChanged: detailsOpen = false

    BlurredOverlay {
        anchors.fill: parent
        blurSource: root.visible ? root.blurSource : null
        overlayColor: appTheme.viewportAlertScrimColor
    }

    // The whole viewport takes the pointer and the wheel, so the photo below does not pan or zoom.
    MouseArea {
        anchors.fill: parent
        acceptedButtons: Qt.AllButtons
        hoverEnabled: true
        onWheel: function(wheel) { wheel.accepted = true }
    }

    ColumnLayout {
        id: column
        anchors.centerIn: parent
        width: Math.min(appTheme.viewportAlertWidth, root.width - 2 * appTheme.spaceLg)
        spacing: 0
        Accessible.role: Accessible.AlertMessage
        Accessible.name: root.title

        IconImage {
            Layout.alignment: Qt.AlignHCenter
            Layout.preferredWidth: appTheme.iconOpticalSize
            Layout.preferredHeight: appTheme.iconOpticalSize
            sourceSize: Qt.size(appTheme.iconSourceSize, appTheme.iconSourceSize)
            source: "qrc:/panel_icons/photo-off.svg"
            color: appTheme.viewportAlertSecondaryTextColor
        }

        Label {
            objectName: "viewportRenderAlertTitle"
            Layout.fillWidth: true
            Layout.topMargin: appTheme.spaceMd
            text: root.title
            color: appTheme.textColor
            horizontalAlignment: Text.AlignHCenter
            wrapMode: Text.Wrap
            font.family: appTheme.headlineFontFamily
            font.pixelSize: appTheme.fontSizeHeadline
            font.weight: appTheme.fontWeightHeading
        }

        Label {
            objectName: "viewportRenderAlertMessage"
            Layout.fillWidth: true
            Layout.topMargin: appTheme.spaceXs
            visible: text.length > 0
            text: root.message
            color: appTheme.viewportAlertSecondaryTextColor
            horizontalAlignment: Text.AlignHCenter
            wrapMode: Text.Wrap
            font.family: appTheme.uiFontFamily
            font.pixelSize: appTheme.fontSizeTitle
        }

        AbstractButton {
            id: detailsToggle
            objectName: "viewportRenderAlertDetailsToggle"
            Layout.alignment: Qt.AlignHCenter
            Layout.topMargin: appTheme.spaceMd
            visible: root.detail.length > 0
            hoverEnabled: true
            padding: 0
            text: root.detailsOpen ? qsTr("Hide Details") : qsTr("Show Details")
            Accessible.name: text
            onClicked: root.detailsOpen = !root.detailsOpen

            readonly property color inkColor: hovered || visualFocus
                                              ? appTheme.textColor
                                              : appTheme.viewportAlertSecondaryTextColor

            contentItem: Row {
                spacing: appTheme.spaceXs

                Label {
                    anchors.verticalCenter: parent.verticalCenter
                    text: detailsToggle.text
                    color: detailsToggle.inkColor
                    font.family: appTheme.uiFontFamily
                    font.pixelSize: appTheme.fontSizeBody
                }

                IconImage {
                    anchors.verticalCenter: parent.verticalCenter
                    width: appTheme.layerIconGlyphSize
                    height: appTheme.layerIconGlyphSize
                    sourceSize: Qt.size(appTheme.iconSourceSizeCompact, appTheme.iconSourceSizeCompact)
                    source: "qrc:/panel_icons/chevron-right.svg"
                    color: detailsToggle.inkColor
                    rotation: root.detailsOpen ? 90 : 0
                    Behavior on rotation {
                        enabled: !appTheme.reduceMotion
                        NumberAnimation { duration: appTheme.motionFadeMs; easing.type: Easing.OutCubic }
                    }
                }
            }
        }

        Rectangle {
            id: detailWell
            objectName: "viewportRenderAlertDetail"
            Layout.fillWidth: true
            Layout.topMargin: appTheme.spaceSm
            Layout.preferredHeight: detailFlick.height + 2 * appTheme.spaceSm
            visible: root.detailsOpen && root.detail.length > 0
            radius: appTheme.controlRadius
            color: appTheme.viewportAlertDetailFillColor
            border.width: 1
            border.color: appTheme.glassStrokeColor

            Flickable {
                id: detailFlick
                anchors.left: parent.left
                anchors.right: copyButton.left
                anchors.top: parent.top
                anchors.leftMargin: appTheme.spaceMd
                anchors.rightMargin: appTheme.spaceXs
                anchors.topMargin: appTheme.spaceSm
                // Whole lines of the laid-out text (the line height of the mono font with its
                // fallbacks), up to viewportAlertDetailMaxLines.
                readonly property real lineHeight: detailText.lineCount > 0
                                                   ? detailText.contentHeight / detailText.lineCount
                                                   : detailText.font.pixelSize
                height: Math.min(detailText.implicitHeight,
                                 Math.ceil(lineHeight * appTheme.viewportAlertDetailMaxLines))
                contentWidth: width
                contentHeight: detailText.implicitHeight
                clip: true
                boundsBehavior: Flickable.StopAtBounds
                readonly property bool overflows: contentHeight > height + 1

                ScrollBar.vertical: AlcedoScrollBar {
                    policy: detailFlick.overflows ? ScrollBar.AlwaysOn : ScrollBar.AlwaysOff
                }

                TextEdit {
                    id: detailText
                    // The scroll bar stands over the right edge of the Flickable.
                    width: detailFlick.width - (detailFlick.overflows ? appTheme.spaceMd : 0)
                    text: root.detail
                    textFormat: TextEdit.PlainText
                    readOnly: true
                    selectByMouse: true
                    wrapMode: TextEdit.WrapAtWordBoundaryOrAnywhere
                    color: appTheme.textColor
                    selectedTextColor: appTheme.bgCanvasColor
                    selectionColor: appTheme.textColor
                    font.family: appTheme.monoFontFamily
                    font.pixelSize: appTheme.fontSizeCaption
                }
            }

            AbstractButton {
                id: copyButton
                objectName: "viewportRenderAlertCopyButton"
                anchors.right: parent.right
                anchors.top: parent.top
                anchors.rightMargin: appTheme.spaceXs
                anchors.topMargin: appTheme.spaceXs
                width: appTheme.layerIconHitSize
                height: appTheme.layerIconHitSize
                hoverEnabled: true
                padding: 0
                Accessible.name: qsTr("Copy")
                ToolTip.visible: hovered || copiedTimer.running
                ToolTip.text: copiedTimer.running ? qsTr("Copied") : qsTr("Copy")
                onClicked: root.copyDetail()

                contentItem: Item {
                    IconImage {
                        anchors.centerIn: parent
                        width: appTheme.layerIconGlyphSize
                        height: appTheme.layerIconGlyphSize
                        sourceSize: Qt.size(appTheme.iconSourceSizeCompact,
                                            appTheme.iconSourceSizeCompact)
                        source: "qrc:/panel_icons/clipboard.svg"
                        color: copyButton.hovered || copyButton.visualFocus
                               ? appTheme.textColor
                               : appTheme.viewportAlertSecondaryTextColor
                    }
                }
            }

            Timer {
                id: copiedTimer
                interval: appTheme.viewportAlertCopiedMs
            }
        }

        RowLayout {
            Layout.alignment: Qt.AlignHCenter
            Layout.topMargin: appTheme.spaceLg
            spacing: appTheme.spaceSm
            visible: root.primaryText.length > 0 || root.secondaryText.length > 0

            DialogActionButton {
                objectName: "viewportRenderAlertSecondaryButton"
                visible: root.secondaryText.length > 0
                kind: "normal"
                enabled: root.secondaryEnabled
                text: root.secondaryText
                buttonWidth: contentItem.implicitWidth + 2 * appTheme.spaceMd
                buttonHeight: appTheme.viewportAlertButtonHeight
                buttonRadius: appTheme.controlRadiusSmall
                font.pixelSize: appTheme.fontSizeBody
                onClicked: root.secondaryClicked()
            }

            DialogActionButton {
                objectName: "viewportRenderAlertPrimaryButton"
                visible: root.primaryText.length > 0
                kind: "light"
                enabled: root.primaryEnabled
                text: root.primaryText
                buttonWidth: contentItem.implicitWidth + 2 * appTheme.spaceMd
                buttonHeight: appTheme.viewportAlertButtonHeight
                buttonRadius: appTheme.controlRadiusSmall
                font.pixelSize: appTheme.fontSizeBody
                onClicked: root.primaryClicked()
            }
        }
    }
}
