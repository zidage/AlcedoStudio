import QtQuick
import QtQuick.Controls
import QtQuick.Controls.impl
import QtQuick.Layouts

// Shared structural SVG action used by editor rails, nav segments, and compact
// tool rows. Canonical geometry and a11y rules: DESIGN.md.
//
// Geometry (AppTheme tokens — do not hardcode px here):
//   hit:     iconButtonHitSize / iconButtonHitSizeCompact   (44 / 40)
//   chrome:  hit minus 8                                    (36 / 32)
//   optical: iconOpticalSize / iconOpticalSizeCompact       (22 / 18)
//   source:  iconSourceSize / iconSourceSizeCompact         (24 / 20)
// Compact mode is for rails and dense segmented navigation.
//
// Root is an Item (not a Controls Button). Material Button applies its own
// minimum padding and implicit sizing and stretches icon-only chrome into a
// rectangle — only the SVG stays correct. Explicit square geometry keeps the
// hit target, fill, and radius square in both Column and Layout contexts.
//
// SVG tint uses ColorImage (Button.icon.color engine) so monochrome stroke
// icons pick up theme colors without MultiEffect colorization failures or
// solid-rect alpha-mask stroke thickening.
Item {
    id: control

    property bool compact: false
    // When true, the control fills its parent RowLayout/column cell instead of
    // forcing a square hitSize (legacy segmented rows; default false).
    property bool stretchInLayout: false
    property bool selected: false
    property bool showHoverFill: true
    property bool showFocusRing: true
    property string actionName: ""
    property url iconSrc: ""
    // Optional palette overrides when the parent shell uses a local theme mirror.
    property color iconColorDefault: appTheme.iconColor
    property color iconColorMuted: appTheme.textMutedColor
    // Kept for call-site compatibility. Selection is fill-only (see DESIGN.md);
    // icon color no longer flips to accent when selected — that recolor path
    // used a solid alpha mask and made stroke icons look over-bold.
    property color iconColorSelected: iconColorDefault
    // Phase 4D: opaque idle fill (was "transparent").
    property color fillIdle: appTheme.buttonIdleFillColor
    property color fillHover: appTheme.buttonHoveredFillColor
    property color fillPressed: appTheme.buttonPressedFillColor
    property color fillSelected: appTheme.buttonSelectedFillColor
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

    // Effective square side: callers may still provide an explicit square;
    // otherwise the shared hit token owns pointer and keyboard geometry.
    readonly property real squareSide: {
        if (stretchInLayout) {
            return Math.max(0, Math.min(width, height))
        }
        if (width > 0 && height > 0) {
            return Math.min(width, height)
        }
        if (width > 0) {
            return width
        }
        if (height > 0) {
            return height
        }
        return hitSize
    }
    // The pointer/focus target stays comfortably large while the painted well
    // remains visually quiet. This separation prevents a 40–44 px hit target
    // from reading as a wall of oversized square buttons.
    readonly property real chromeSize: Math.min(
                                           squareSide,
                                           Math.max(opticalSize + 8, squareSide - 8))

    // Selected vs idle is communicated by fillSelected / fillIdle only.
    // Keep a single stroke tint so native SVG weight stays consistent.
    readonly property color _resolvedIconColor: {
        if (!control.enabled) {
            return iconColorMuted
        }
        return iconColorDefault
    }

    readonly property color _fillColor: {
        if (!control.enabled) {
            return control.fillIdle
        }
        if (control.selected) {
            return control.fillSelected
        }
        if (!control.showHoverFill) {
            return control.fillIdle
        }
        if (pressArea.pressed) {
            return control.fillPressed
        }
        if (hover.hovered || pressArea.containsMouse) {
            return control.fillHover
        }
        return control.fillIdle
    }

    signal clicked()

    implicitWidth: stretchInLayout ? 0 : hitSize
    implicitHeight: stretchInLayout ? 0 : hitSize
    Layout.preferredWidth: stretchInLayout ? -1 : hitSize
    Layout.preferredHeight: stretchInLayout ? -1 : hitSize
    Layout.minimumWidth: stretchInLayout ? 0 : hitSize
    Layout.minimumHeight: stretchInLayout ? 0 : hitSize
    Layout.fillWidth: stretchInLayout
    Layout.fillHeight: stretchInLayout

    // Column parents use implicit size when width/height are not set. Pin both
    // axes so the control never paints as a Material-style rectangle.
    Component.onCompleted: {
        if (stretchInLayout) {
            return
        }
        if (width <= 0 && height <= 0) {
            width = hitSize
            height = hitSize
        } else if (width > 0 && height <= 0) {
            height = width
        } else if (height > 0 && width <= 0) {
            width = height
        } else if (Math.abs(width - height) > 0.5) {
            // Both set but not square — force square from the smaller edge so we
            // never exceed a tight rail or nav track.
            const side = Math.min(width, height)
            width = side
            height = side
        }
    }

    activeFocusOnTab: true
    Accessible.role: Accessible.Button
    Accessible.name: control.actionName
    Accessible.description: control.actionName
    Accessible.onPressAction: {
        if (control.enabled) {
            control.clicked()
        }
    }

    Keys.onPressed: function (event) {
        if (!control.enabled) {
            return
        }
        if (event.key === Qt.Key_Space || event.key === Qt.Key_Return
                || event.key === Qt.Key_Enter) {
            control.clicked()
            event.accepted = true
        }
    }

    Rectangle {
        id: bg
        // Center compact chrome inside the larger square hit target.
        width: control.chromeSize
        height: control.chromeSize
        anchors.centerIn: parent
        radius: appTheme.controlRadiusSmall
        color: control._fillColor
        border.width: (control.showFocusRing && control.enabled && control.activeFocus) ? 1 : 0
        border.color: Qt.rgba(control.focusRingColor.r, control.focusRingColor.g,
                              control.focusRingColor.b, 0.60)
    }

    ColorImage {
        anchors.centerIn: parent
        width: control.opticalSize
        height: control.opticalSize
        source: control.iconSrc
        sourceSize.width: control.sourceSize
        sourceSize.height: control.sourceSize
        fillMode: Image.PreserveAspectFit
        smooth: true
        visible: control.iconSrc.toString().length > 0
        color: control._resolvedIconColor
        opacity: control.enabled ? 1.0 : 0.55
    }

    HoverHandler {
        id: hover
    }

    MouseArea {
        id: pressArea
        anchors.fill: parent
        hoverEnabled: true
        enabled: control.enabled
        cursorShape: control.enabled ? Qt.PointingHandCursor : Qt.ArrowCursor
        onClicked: control.clicked()
    }

    ToolTip.visible: (hover.hovered || pressArea.containsMouse) && control.actionName.length > 0
    ToolTip.text: control.actionName
    ToolTip.delay: 400
}
