import QtQuick

// One slot of the shared layer icon strip used by Mask Group rows (and, later,
// Color Grade node cards). Slots are small square icon buttons with a fixed
// order per strip; new layer features append slots instead of resizing rows.
//
// State is ink only (docs/VI/README.md): an active state paints the glyph in
// the foreground color, the idle state in the muted color. No fill, no accent.
// The hit square comes from appTheme.layerIconHitSize, the glyph from
// appTheme.layerIconGlyphSize. Place it in a cell of layerIconHitSize.
IconActionButton {
    id: root

    property bool active: false
    // Tooltip shown while the slot is disabled; empty falls back to actionName.
    property string disabledReason: ""
    property color activeColor: appTheme.textColor
    property color idleColor: appTheme.textMutedColor
    property color hoverColor: appTheme.hoverColor

    compact: true
    stretchInLayout: true
    glyphSizeOverride: appTheme.layerIconGlyphSize
    iconColorDefault: root.active ? root.activeColor : root.idleColor
    iconColorMuted: root.idleColor
    fillIdle: "transparent"
    fillHover: root.hoverColor
    fillPressed: appTheme.buttonPressedFillColor
    toolTipText: enabled || root.disabledReason.length === 0 ? root.actionName
                                                             : root.disabledReason
    focusOnPointerPress: false
}
