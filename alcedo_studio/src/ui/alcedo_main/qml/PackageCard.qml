pragma ComponentBehavior: Bound

import QtQuick
import QtQml.Models
import QtQuick.Controls.Basic
import QtQuick.Layouts

// The near-square store card of the settings pages that download packages: Settings > LUTs
// (one card per LUT package) and Settings > AI Models (the AI Mask model set and one card per
// CLIP model). The page owns the data and the actions: it sets the texts and the state, and it
// receives the button presses through the signals. Place the card in a GridLayout.
//
// Object names: the card is `<namePrefix>:<cardKey>`; its parts are
// `<namePrefix><Part>:<cardKey>` with Part = Name, Upstream, License (+ index), Error, Message,
// RemoveConfirm, RemoveWarning, RemoveKeep, RemoveConfirmButton, Status, Action, Remove, Cancel.
Rectangle {
    id: card

    property string namePrefix: "packageCard"
    property string cardKey: ""

    property string title: ""
    // One line below the title (the upstream project, a supported version range, a language).
    property string subtitle: ""
    // Optional link button beside the title (for example the upstream GitHub project).
    property string upstreamUrl: ""
    property string upstreamIconSource: "qrc:/panel_icons/github.svg"
    property string upstreamToolTip: ""

    // Pill row: one pill per non-empty string (count, revision, size).
    property var pills: []
    // License row: one pill per license, {name, url}. A license with a URL opens it on click.
    property var licenses: []

    property string statusText: ""
    property color statusColor: appTheme.textMutedColor
    // A running download, installation, removal, or activation: accent border, progress bar.
    property bool busy: false
    // The model that is active in the open project: activeModelBorderColor border.
    property bool active: false
    property real progressPercent: 0
    property bool progressIndeterminate: true
    property string progressLeadingText: ""
    property string progressSpeedText: ""
    property string progressEtaText: ""

    property string errorText: ""
    // Information without an error (the result of the last action, a requirement).
    property string messageText: ""

    // The main action (Download, Update, Repair, Retry, Activate). Empty hides the button.
    property string actionText: ""
    property string actionKind: "accent"
    property bool actionEnabled: true
    property bool cancelVisible: false
    property bool cancelEnabled: true
    property bool removable: false
    property bool removeEnabled: true
    property string removeToolTip: ""

    // Removal confirmation on the card.
    property bool confirmingRemoval: false
    property string removeQuestion: ""
    property string removeWarning: ""
    property bool removeConfirmEnabled: true

    signal actionRequested()
    signal cancelRequested()
    signal removeRequested()
    signal removeConfirmed()
    signal removeKept()

    objectName: namePrefix + ":" + cardKey
    Layout.fillWidth: true
    // Equal preferred widths split the columns evenly.
    Layout.preferredWidth: 1
    // Near-square store card; grows when the content needs more room.
    Layout.preferredHeight: Math.max(cardColumn.implicitHeight + 22 * 2, width * 0.72)
    radius: 14
    color: appTheme.cardSurfaceColor
    border.width: card.active ? 2 : 1
    border.color: card.active ? appTheme.activeModelBorderColor
                              : (card.busy ? appTheme.accentColor : appTheme.cardBorderColor)

    ColumnLayout {
        id: cardColumn
        anchors.fill: parent
        anchors.margins: 22
        spacing: appTheme.spaceMd

        RowLayout {
            Layout.fillWidth: true
            spacing: appTheme.spaceMd

            ColumnLayout {
                Layout.fillWidth: true
                Layout.alignment: Qt.AlignTop
                spacing: appTheme.spaceXs

                Label {
                    objectName: card.namePrefix + "Name:" + card.cardKey
                    Layout.fillWidth: true
                    text: card.title
                    color: appTheme.textColor
                    font.family: appTheme.headlineFontFamily
                    font.pixelSize: 28
                    font.weight: Font.DemiBold
                    lineHeight: 1.1
                    wrapMode: Text.Wrap
                    maximumLineCount: 3
                    elide: Text.ElideRight
                }
                Label {
                    objectName: card.namePrefix + "Subtitle:" + card.cardKey
                    Layout.fillWidth: true
                    visible: card.subtitle.length > 0
                    text: card.subtitle
                    color: appTheme.textMutedColor
                    font.family: appTheme.headlineFontFamily
                    font.pixelSize: appTheme.fontSizeBody
                    wrapMode: Text.Wrap
                }
            }

            IconButton {
                objectName: card.namePrefix + "Upstream:" + card.cardKey
                Layout.alignment: Qt.AlignTop
                visible: card.upstreamUrl.length > 0
                buttonSize: 36
                buttonRadius: 10
                iconSize: 18
                iconSrc: card.upstreamIconSource
                normalColor: Qt.rgba(1, 1, 1, 0.07)
                borderColor: Qt.rgba(appTheme.textColor.r, appTheme.textColor.g, appTheme.textColor.b, 0.14)
                tooltipText: card.upstreamToolTip
                onClicked: Qt.openUrlExternally(card.upstreamUrl)
            }
        }

        // Instantiator, not Repeater: the pills must exist under an offscreen test window too
        // (see SettingDialog.qml).
        Flow {
            id: pillRow
            Layout.fillWidth: true
            visible: card.pills.length > 0
            spacing: appTheme.spaceSm

            Instantiator {
                model: card.pills
                onObjectAdded: function(index, object) {
                    object.parent = pillRow
                }
                delegate: Pill {
                    required property var modelData
                    text: String(modelData || "")
                }
            }
        }

        // License names; a license with a URL (web page or installed file) opens on click.
        Flow {
            id: licenseRow
            Layout.fillWidth: true
            visible: card.licenses.length > 0
            spacing: appTheme.spaceSm

            Instantiator {
                model: card.licenses
                onObjectAdded: function(index, object) {
                    object.parent = licenseRow
                }
                delegate: Pill {
                    required property int index
                    required property var modelData
                    objectName: card.namePrefix + "License" + index + ":" + card.cardKey
                    text: String(modelData.name || "")
                    url: String(modelData.url || "")
                    toolTip: qsTr("Open the license")
                }
            }
        }

        Item {
            Layout.fillHeight: true
        }

        AlertBadge {
            objectName: card.namePrefix + "Error:" + card.cardKey
            Layout.fillWidth: true
            text: card.errorText
        }

        Label {
            objectName: card.namePrefix + "Message:" + card.cardKey
            Layout.fillWidth: true
            visible: card.messageText.length > 0
            text: card.messageText
            color: appTheme.textMutedColor
            font.family: appTheme.uiFontFamily
            font.pixelSize: appTheme.fontSizeCaption
            wrapMode: Text.Wrap
        }

        ColumnLayout {
            objectName: card.namePrefix + "RemoveConfirm:" + card.cardKey
            Layout.fillWidth: true
            visible: card.confirmingRemoval
            spacing: appTheme.spaceSm

            Label {
                Layout.fillWidth: true
                text: card.removeQuestion
                color: appTheme.textColor
                font.family: appTheme.uiFontFamily
                font.pixelSize: appTheme.fontSizeCaption
                wrapMode: Text.Wrap
            }
            AlertBadge {
                objectName: card.namePrefix + "RemoveWarning:" + card.cardKey
                text: card.removeWarning
            }
            RowLayout {
                Layout.fillWidth: true
                spacing: appTheme.spaceSm

                Item {
                    Layout.fillWidth: true
                }
                DialogActionButton {
                    objectName: card.namePrefix + "RemoveKeep:" + card.cardKey
                    buttonWidth: 100
                    buttonHeight: 34
                    text: qsTr("Keep")
                    onClicked: card.removeKept()
                }
                DialogActionButton {
                    objectName: card.namePrefix + "RemoveConfirmButton:" + card.cardKey
                    kind: "danger"
                    buttonWidth: 100
                    buttonHeight: 34
                    enabled: card.removeConfirmEnabled
                    text: qsTr("Remove")
                    onClicked: card.removeConfirmed()
                }
            }
        }

        ThemedProgressBar {
            Layout.fillWidth: true
            visible: card.busy
            active: card.busy
            indeterminate: card.progressIndeterminate
            progressValue: card.progressPercent
            showDetails: card.progressLeadingText.length > 0 || card.progressSpeedText.length > 0
                         || card.progressEtaText.length > 0
            leadingText: card.progressLeadingText
            speedText: card.progressSpeedText
            etaText: card.progressEtaText
        }

        RowLayout {
            Layout.fillWidth: true
            visible: !card.confirmingRemoval
            spacing: appTheme.spaceSm

            Rectangle {
                Layout.alignment: Qt.AlignVCenter
                visible: !card.busy
                implicitWidth: 8
                implicitHeight: 8
                radius: 4
                color: card.statusColor
            }
            Label {
                objectName: card.namePrefix + "Status:" + card.cardKey
                Layout.fillWidth: true
                Layout.alignment: Qt.AlignVCenter
                text: card.statusText
                color: appTheme.textColor
                font.family: appTheme.uiFontFamily
                font.pixelSize: appTheme.fontSizeBody
                wrapMode: Text.Wrap
            }
            DialogActionButton {
                objectName: card.namePrefix + "Action:" + card.cardKey
                visible: card.actionText.length > 0
                kind: card.actionKind
                buttonWidth: 116
                buttonHeight: 34
                enabled: card.actionEnabled
                text: card.actionText
                onClicked: card.actionRequested()
            }
            // The shared trash glyph, in the same chrome as the upstream link.
            IconButton {
                objectName: card.namePrefix + "Remove:" + card.cardKey
                Layout.alignment: Qt.AlignVCenter
                visible: card.removable && !card.busy
                enabled: card.removeEnabled
                buttonSize: 34
                buttonRadius: 10
                iconSize: 18
                iconSrc: "qrc:/panel_icons/trash.svg"
                normalColor: Qt.rgba(1, 1, 1, 0.07)
                borderColor: Qt.rgba(appTheme.textColor.r, appTheme.textColor.g,
                                     appTheme.textColor.b, 0.14)
                tooltipText: card.removeToolTip
                onClicked: card.removeRequested()
            }
            DialogActionButton {
                objectName: card.namePrefix + "Cancel:" + card.cardKey
                visible: card.cancelVisible
                enabled: card.cancelEnabled
                buttonWidth: 100
                buttonHeight: 34
                text: qsTr("Cancel")
                onClicked: card.cancelRequested()
            }
        }
    }

    component Pill: Rectangle {
        id: pill

        property string text: ""
        property string url: ""
        property string toolTip: ""

        visible: text.length > 0
        implicitWidth: pillLabel.implicitWidth + appTheme.spaceMd * 2
        implicitHeight: pillLabel.implicitHeight + appTheme.spaceXs * 2 + 2
        radius: height / 2
        color: pillMouse.containsMouse ? appTheme.hoverColor : appTheme.bgBaseColor
        border.width: 1
        border.color: appTheme.cardBorderColor

        Label {
            id: pillLabel
            anchors.centerIn: parent
            text: pill.text
            color: appTheme.textColor
            font.family: appTheme.headlineFontFamily
            font.pixelSize: appTheme.fontSizeBody
            font.weight: Font.Medium
        }

        MouseArea {
            id: pillMouse
            anchors.fill: parent
            enabled: pill.url.length > 0
            hoverEnabled: pill.url.length > 0
            cursorShape: pill.url.length > 0 ? Qt.PointingHandCursor : Qt.ArrowCursor
            onClicked: Qt.openUrlExternally(pill.url)
        }

        ToolTip.visible: pill.url.length > 0 && pill.toolTip.length > 0 && pillMouse.containsMouse
        ToolTip.text: pill.toolTip
        ToolTip.delay: 400
    }
}
