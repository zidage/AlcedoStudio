import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts

// Shared structural SVG action used by editor rails, nav segments, and compact
// tool rows. Canonical geometry and a11y rules: DESIGN.md.
//
// Geometry (AppTheme tokens — do not hardcode px here):
//   hit:     iconButtonHitSize / iconButtonHitSizeCompact   (44 / 40)
//   optical: iconOpticalSize / iconOpticalSizeCompact       (32 / 20)
//   source:  iconSourceSize / iconSourceSizeCompact         (32 / 20)
// Compact exception: set compact:true for dense segmented rows only.
Button {
    id: control

    property bool compact: false
    // When true, the control fills its parent RowLayout/column cell instead of
    // forcing a square hitSize (used by segmented adjustment nav).
    property bool stretchInLayout: false
    property bool selected: false
    property bool showHoverFill: true
    property bool showFocusRing: true
    property string actionName: ""
    property url iconSrc: ""
    // Optional palette overrides when the parent shell uses a local theme mirror.
    property color iconColorDefault: appTheme.iconColor
    property color iconColorMuted: appTheme.textMutedColor
    property color iconColorSelected: appTheme.textColor
    property color fillIdle: "transparent"
    property color fillHover: appTheme.hoverColor
    property color fillSelected: appTheme.bgBaseColor
    property color focusRingColor: appTheme.accentColor

    readonly property int hitSize: compact
                                   ? appTheme.iconButtonHitSizeCompact
                                   : appTheme.iconButtonHitSize
    readonly property int opticalSize: compact
                                       ? appTheme.iconOpticalSizeCompact
                                       : appTheme.iconOpticalSize
    readonly property int sourceSize: compact
                                      ? appTheme.iconSourceSizeCompact
                                      : appTheme.iconSourceSize

    implicitWidth: stretchInLayout ? 0 : hitSize
    implicitHeight: stretchInLayout ? 0 : hitSize
    Layout.preferredWidth: stretchInLayout ? -1 : hitSize
    Layout.preferredHeight: stretchInLayout ? -1 : hitSize
    Layout.fillWidth: stretchInLayout
    Layout.fillHeight: stretchInLayout
    flat: true
    padding: 0
    display: AbstractButton.IconOnly
    activeFocusOnTab: true
    hoverEnabled: true

    icon.source: iconSrc
    icon.width: opticalSize
    icon.height: opticalSize
    icon.color: !enabled ? Qt.rgba(iconColorMuted.r, iconColorMuted.g, iconColorMuted.b, 0.55)
                : (selected ? iconColorSelected : iconColorDefault)
    Material.foreground: icon.color

    HoverHandler { id: hover }

    background: Rectangle {
        radius: appTheme.controlRadius
        color: {
            if (!control.enabled) {
                return control.fillIdle
            }
            if (control.selected) {
                return control.fillSelected
            }
            if (!control.showHoverFill) {
                return control.fillIdle
            }
            if (control.down) {
                return Qt.rgba(control.fillHover.r, control.fillHover.g, control.fillHover.b, 0.55)
            }
            if (hover.hovered || control.hovered) {
                return Qt.rgba(control.fillHover.r, control.fillHover.g, control.fillHover.b, 0.30)
            }
            return control.fillIdle
        }
        border.width: (control.showFocusRing && control.enabled && control.activeFocus) ? 1 : 0
        border.color: Qt.rgba(control.focusRingColor.r, control.focusRingColor.g,
                              control.focusRingColor.b, 0.60)
    }

    ToolTip.visible: (hover.hovered || control.hovered) && control.actionName.length > 0
    ToolTip.text: control.actionName
    Accessible.name: control.actionName
    Accessible.role: Accessible.Button
    Accessible.description: control.actionName
}
