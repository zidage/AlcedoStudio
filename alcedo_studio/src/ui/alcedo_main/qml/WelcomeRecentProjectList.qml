import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Lower area of the welcome surface ("Other recent projects"): a sunken
// bgBaseColor well with one row per recent entry. A row shows the project name,
// the folder path, and the relative last-opened time. The caller passes the
// entries without the previewed project, in lastOpenedMs order. Rows are
// disabled while a project load runs (selectionEnabled == false). Enter, Return
// and Space on a focused row select it.
ColumnLayout {
    id: root

    property var projects: []
    property bool selectionEnabled: true

    signal projectRequested(string projectPath)

    spacing: appTheme.spaceMd

    // Moved from the previous WelcomeDialog page 0 without a change.
    function relativeTimeLabel(lastOpenedMs) {
        const value = Number(lastOpenedMs)
        if (!isFinite(value) || value <= 0) {
            return qsTr("Opened recently")
        }

        const deltaMinutes = Math.max(0, Math.floor((Date.now() - value) / 60000))
        if (deltaMinutes < 1) {
            return qsTr("Opened just now")
        }
        if (deltaMinutes < 60) {
            return qsTr("Opened %n minute(s) ago", "", deltaMinutes)
        }

        const deltaHours = Math.floor(deltaMinutes / 60)
        if (deltaHours < 24) {
            return qsTr("Opened %n hour(s) ago", "", deltaHours)
        }

        const deltaDays = Math.floor(deltaHours / 24)
        if (deltaDays === 1) {
            return qsTr("Opened yesterday")
        }
        if (deltaDays < 7) {
            return qsTr("Opened %n day(s) ago", "", deltaDays)
        }
        if (deltaDays < 14) {
            return qsTr("Opened last week")
        }
        return qsTr("Opened %n day(s) ago", "", deltaDays)
    }

    Label {
        Layout.fillWidth: true
        text: qsTr("Other recent projects")
        color: appTheme.textColor
        font.family: appTheme.uiFontFamily
        font.pixelSize: appTheme.fontSizeSection
        font.weight: appTheme.fontWeightHeading
        wrapMode: Text.Wrap
        Accessible.role: Accessible.Heading
    }

    Rectangle {
        Layout.fillWidth: true
        Layout.fillHeight: true
        radius: appTheme.controlRadiusSmall
        color: appTheme.bgBaseColor

        Label {
            anchors.centerIn: parent
            width: parent.width - 2 * appTheme.spaceLg
            visible: recentList.count === 0
            text: qsTr("No other recent projects")
            color: appTheme.textMutedColor
            font.family: appTheme.uiFontFamily
            font.pixelSize: appTheme.fontSizeBody
            font.weight: appTheme.fontWeightRegular
            horizontalAlignment: Text.AlignHCenter
            wrapMode: Text.Wrap
        }

        ListView {
            id: recentList
            objectName: "welcomeRecentProjectList"
            anchors.fill: parent
            anchors.margins: appTheme.spaceXs
            spacing: appTheme.spaceXs
            clip: true
            boundsBehavior: Flickable.StopAtBounds
            activeFocusOnTab: false
            model: root.projects
            ScrollBar.vertical: ScrollBar {
                policy: ScrollBar.AsNeeded
            }

            delegate: AbstractButton {
                id: row

                required property var modelData
                readonly property string projectPath: String(row.modelData.path || "")

                width: ListView.view.width
                implicitHeight: rowContent.implicitHeight + 2 * appTheme.spaceSm
                enabled: root.selectionEnabled
                hoverEnabled: true
                focusPolicy: Qt.StrongFocus
                leftPadding: appTheme.spaceMd
                rightPadding: appTheme.spaceMd
                topPadding: appTheme.spaceSm
                bottomPadding: appTheme.spaceSm
                Accessible.role: Accessible.Button
                Accessible.name: String(row.modelData.name || "") + " " + String(row.modelData.folderPath || "")

                onClicked: root.projectRequested(row.projectPath)
                Keys.onReturnPressed: function(event) {
                    if (row.enabled)
                        row.clicked()
                    event.accepted = true
                }
                Keys.onEnterPressed: function(event) {
                    if (row.enabled)
                        row.clicked()
                    event.accepted = true
                }

                background: Rectangle {
                    radius: appTheme.controlRadiusSmall
                    color: row.down
                           ? appTheme.buttonPressedFillColor
                           : (row.hovered && row.enabled ? appTheme.hoverColor : "transparent")
                    border.width: row.visualFocus ? 1 : 0
                    border.color: appTheme.textMutedColor
                }

                contentItem: RowLayout {
                    id: rowContent
                    spacing: appTheme.spaceMd

                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: appTheme.spaceXs / 2

                        Label {
                            Layout.fillWidth: true
                            text: String(row.modelData.name || "")
                            color: row.enabled ? appTheme.textColor : appTheme.textMutedColor
                            font.family: appTheme.uiFontFamily
                            font.pixelSize: appTheme.fontSizeTitle
                            font.weight: appTheme.fontWeightStrong
                            elide: Text.ElideRight
                            maximumLineCount: 1
                        }

                        Label {
                            Layout.fillWidth: true
                            text: String(row.modelData.folderPath || "")
                            color: appTheme.textMutedColor
                            font.family: appTheme.uiFontFamily
                            font.pixelSize: appTheme.fontSizeCaption
                            font.weight: appTheme.fontWeightRegular
                            elide: Text.ElideMiddle
                            maximumLineCount: 1
                        }
                    }

                    Label {
                        Layout.alignment: Qt.AlignVCenter
                        text: root.relativeTimeLabel(row.modelData.lastOpenedMs)
                        color: appTheme.textMutedColor
                        font.family: appTheme.headlineFontFamily
                        font.pixelSize: appTheme.fontSizeCaption
                        font.weight: appTheme.fontWeightRegular
                        horizontalAlignment: Text.AlignRight
                    }
                }
            }
        }
    }
}
