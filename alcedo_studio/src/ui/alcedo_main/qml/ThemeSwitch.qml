import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Shared monochrome switch row (DESIGN.md "Shared components"): a label and a
// track. On: light selected well with an ink knob at the end. Off: sunken
// bgBaseColor track with a hairline border and a muted knob at the start.
// Same visual language as ThemeCheckBox; no Material controls.
Item {
    id: root
    objectName: "themeSwitch"

    property bool checked: false
    property string text: ""
    // Accessible name override; falls back to text when empty.
    property string accessibleText: ""
    property bool enabled: true

    signal toggled(bool checked)

    readonly property int trackWidth: appTheme.iconButtonHitSizeCompact
    readonly property int trackHeight: appTheme.iconOpticalSize
    readonly property int knobSize: appTheme.iconOpticalSizeCompact - appTheme.spaceXs / 2
    readonly property real knobInset: (trackHeight - knobSize) / 2

    implicitWidth: row.implicitWidth
    implicitHeight: Math.max(appTheme.iconOpticalSizeCompact + appTheme.spaceXs,
                             row.implicitHeight)
    Layout.fillWidth: true
    activeFocusOnTab: enabled
    Accessible.role: Accessible.CheckBox
    Accessible.name: root.accessibleText.length > 0 ? root.accessibleText : root.text
    Accessible.checkable: true
    Accessible.checked: checked
    Accessible.onToggleAction: toggle()

    function toggle() {
        if (!enabled)
            return
        toggled(!checked)
    }

    Keys.onPressed: function(event) {
        if (event.key === Qt.Key_Space || event.key === Qt.Key_Return
                || event.key === Qt.Key_Enter) {
            toggle()
            event.accepted = true
        }
    }

    Rectangle {
        anchors.fill: parent
        radius: appTheme.badgeRadius
        color: mouseArea.containsMouse && root.enabled
               ? appTheme.buttonHoveredFillColor
               : "transparent"
        border.width: root.activeFocus ? 1 : 0
        border.color: appTheme.textMutedColor
    }

    RowLayout {
        id: row
        anchors.fill: parent
        anchors.leftMargin: appTheme.spaceXs
        anchors.rightMargin: appTheme.spaceXs
        spacing: appTheme.spaceSm
        opacity: root.enabled ? 1.0 : 0.55

        Label {
            Layout.fillWidth: true
            Layout.alignment: Qt.AlignVCenter
            visible: root.text.length > 0
            text: root.text
            color: appTheme.textColor
            font.family: appTheme.uiFontFamily
            font.pixelSize: appTheme.fontSizeBody
            wrapMode: Text.Wrap
        }

        Rectangle {
            id: track
            Layout.preferredWidth: root.trackWidth
            Layout.preferredHeight: root.trackHeight
            Layout.alignment: Qt.AlignVCenter
            radius: height / 2
            color: root.checked ? appTheme.editorListSelectedFillColor : appTheme.bgBaseColor
            border.width: 1
            border.color: root.checked ? appTheme.editorListSelectedFillColor
                                       : appTheme.cardBorderColor

            Rectangle {
                x: root.checked ? track.width - width - root.knobInset : root.knobInset
                anchors.verticalCenter: parent.verticalCenter
                width: root.knobSize
                height: root.knobSize
                radius: width / 2
                color: root.checked ? appTheme.editorListSelectedInkColor
                                    : appTheme.textMutedColor

                Behavior on x {
                    NumberAnimation {
                        duration: appTheme.reduceMotion ? 0 : appTheme.motionFadeMs
                        easing.type: appTheme.motionEasing
                    }
                }
            }
        }
    }

    MouseArea {
        id: mouseArea
        anchors.fill: parent
        enabled: root.enabled
        hoverEnabled: true
        cursorShape: root.enabled ? Qt.PointingHandCursor : Qt.ArrowCursor
        onClicked: root.toggle()
    }
}
