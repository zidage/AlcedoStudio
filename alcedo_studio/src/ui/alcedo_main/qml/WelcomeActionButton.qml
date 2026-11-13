import QtQuick
import QtQuick.Controls
import QtQuick.Controls.impl
import QtQuick.Layouts

// Text action of the welcome surface (DESIGN.md "Welcome surface").
//
// kind:
//   "primary"   - light bone fill (editorListSelectedFillColor) with
//                 editorListSelectedInkColor ink; one per visible column.
//   "secondary" - card fill with a 1 px cardBorderColor outline.
//   "quiet"     - no fill until hover; muted text.
// An optional SVG (iconSource) is tinted through ColorImage. Enter and Return
// activate a focused button, as Space does.
Button {
    id: control

    property string kind: "secondary"
    property url iconSource: ""
    property bool compact: false
    // Centered label for full-width primary actions; left-aligned otherwise.
    property bool centered: control.kind === "primary"
    readonly property int iconSize: control.compact
                                    ? appTheme.iconOpticalSizeCompact - appTheme.spaceXs
                                    : appTheme.iconOpticalSizeCompact

    readonly property color inkColor: {
        if (!control.enabled)
            return appTheme.textMutedColor
        if (control.kind === "primary")
            return appTheme.editorListSelectedInkColor
        if (control.kind === "quiet" && !control.hovered && !control.visualFocus)
            return appTheme.textMutedColor
        return appTheme.textColor
    }

    readonly property color fillColor: {
        if (control.kind === "primary") {
            if (!control.enabled)
                return appTheme.bgBaseColor
            return control.down || control.hovered
                    ? Qt.darker(appTheme.editorListSelectedFillColor, 1.06)
                    : appTheme.editorListSelectedFillColor
        }
        if (control.down)
            return appTheme.buttonPressedFillColor
        if (control.hovered && control.enabled)
            return appTheme.buttonHoveredFillColor
        return control.kind === "quiet" ? "transparent" : appTheme.cardSurfaceColor
    }

    implicitHeight: control.compact ? appTheme.iconButtonHitSizeCompact : appTheme.iconButtonHitSize
    implicitWidth: contentRow.implicitWidth + control.leftPadding + control.rightPadding
    Layout.preferredHeight: implicitHeight
    topInset: 0
    bottomInset: 0
    leftInset: 0
    rightInset: 0
    topPadding: 0
    bottomPadding: 0
    leftPadding: control.compact ? appTheme.spaceMd : appTheme.spaceLg
    rightPadding: control.leftPadding
    hoverEnabled: true
    focusPolicy: Qt.StrongFocus
    font.family: appTheme.uiFontFamily
    font.pixelSize: control.kind === "primary"
                    ? appTheme.fontSizeSection
                    : (control.kind === "quiet" ? appTheme.fontSizeBody : appTheme.fontSizeTitle)
    font.weight: control.kind === "primary"
                 ? appTheme.fontWeightHeading
                 : (control.kind === "quiet" ? appTheme.fontWeightRegular : appTheme.fontWeightStrong)

    Accessible.name: control.text

    Keys.onReturnPressed: function(event) {
        if (control.enabled)
            control.clicked()
        event.accepted = true
    }
    Keys.onEnterPressed: function(event) {
        if (control.enabled)
            control.clicked()
        event.accepted = true
    }

    contentItem: Item {
        implicitWidth: contentRow.implicitWidth
        implicitHeight: contentRow.implicitHeight

        RowLayout {
            id: contentRow
            anchors.verticalCenter: parent.verticalCenter
            x: control.centered ? Math.round((parent.width - width) / 2) : 0
            width: control.centered ? implicitWidth : parent.width
            spacing: control.compact ? appTheme.spaceSm : appTheme.spaceMd

            ColorImage {
                visible: control.iconSource.toString().length > 0
                Layout.preferredWidth: control.iconSize
                Layout.preferredHeight: control.iconSize
                source: control.iconSource
                sourceSize.width: appTheme.iconSourceSizeCompact
                sourceSize.height: appTheme.iconSourceSizeCompact
                fillMode: Image.PreserveAspectFit
                smooth: true
                color: control.inkColor
            }

            Label {
                Layout.fillWidth: !control.centered
                text: control.text
                color: control.inkColor
                font: control.font
                elide: Text.ElideRight
                verticalAlignment: Text.AlignVCenter
            }
        }
    }

    background: Rectangle {
        radius: control.compact ? appTheme.controlRadiusSmall : appTheme.controlRadius
        color: control.fillColor
        border.width: control.visualFocus || (control.kind === "secondary") ? 1 : 0
        border.color: control.visualFocus ? appTheme.textMutedColor : appTheme.cardBorderColor
    }
}
