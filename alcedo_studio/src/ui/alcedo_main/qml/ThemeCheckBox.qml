import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Shared monochrome checkbox row matching AdjustmentTransferDialog parameter
// checks: light selected well + dark check when on; hairline empty box when off.
// Label uses textColor when checked / alwaysPrimary, else textMutedColor.
Item {
    id: root
    objectName: "themeCheckBox"

    property bool checked: false
    property string text: ""
    property bool enabled: true
    /// When true, label stays textColor even while unchecked (static captions).
    property bool alwaysPrimaryText: false

    signal toggled(bool checked)

    implicitWidth: row.implicitWidth
    implicitHeight: Math.max(appTheme.iconOpticalSizeCompact + appTheme.spaceXs,
                             row.implicitHeight)
    Layout.fillWidth: true
    activeFocusOnTab: enabled
    Accessible.role: Accessible.CheckBox
    Accessible.name: text
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
        border.color: appTheme.accentColor
        opacity: root.enabled ? 1.0 : 0.55
    }

    RowLayout {
        id: row
        anchors.fill: parent
        anchors.leftMargin: appTheme.spaceXs
        anchors.rightMargin: appTheme.spaceXs
        spacing: appTheme.spaceSm

        Rectangle {
            Layout.preferredWidth: appTheme.iconOpticalSizeCompact
            Layout.preferredHeight: appTheme.iconOpticalSizeCompact
            Layout.alignment: Qt.AlignVCenter
            radius: appTheme.badgeRadius
            color: root.checked
                   ? appTheme.editorListSelectedFillColor
                   : "transparent"
            border.width: 1
            border.color: root.checked
                          ? appTheme.editorListSelectedFillColor
                          : appTheme.cardBorderColor

            Label {
                anchors.centerIn: parent
                visible: root.checked
                text: "✓"
                color: appTheme.editorListSelectedInkColor
                font.family: appTheme.uiFontFamily
                font.pixelSize: appTheme.fontSizeCaption
                font.weight: appTheme.fontWeightHeading
            }
        }

        Label {
            Layout.fillWidth: true
            Layout.alignment: Qt.AlignVCenter
            text: root.text
            color: (root.checked || root.alwaysPrimaryText)
                   ? appTheme.textColor
                   : appTheme.textMutedColor
            font.family: appTheme.uiFontFamily
            font.pixelSize: appTheme.fontSizeBody
            elide: Text.ElideRight
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
