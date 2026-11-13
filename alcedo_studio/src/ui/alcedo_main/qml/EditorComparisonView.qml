import QtQuick
import QtQuick.Controls

// Opaque comparison surface over the editor viewport.
//
// Shows one published pair (ComparisonPairPublication::ToVariantMap()) in one of four layouts:
// complete images left/right or top/bottom, or one common canvas revealed by a movable
// left/right or top/bottom divider. Each side has exactly one EditorComparisonCanvas for the
// whole life of a pair, so layout, divider, and swap changes only move and clip items: they
// never reload images or ask for a render.
//
// The pair is shown only when `status` is "ready" and both Image items are Ready. Until then the
// surface shows the loading or error text and no partial pair.
//
// The view owns no comparison state. The owner binds the properties below and writes back the
// divider position from `dividerPositionRequested`.
Item {
    id: root
    objectName: "editorComparisonView"

    // ComparisonPairPublication::ToVariantMap(), or null when no pair is published.
    property var pair: null
    // "idle" | "loading" | "ready" | "failed"
    property string status: "idle"
    property string errorText: ""
    // "complete" | "divider"
    property string displayMode: "divider"
    // "horizontal" (left/right) | "vertical" (top/bottom)
    property string orientation: "horizontal"
    // Divider position in the fitted reference canvas, 0 (left/top edge) to 1 (right/bottom).
    property real dividerPosition: 0.5
    // When true, B is on the left/top and A on the right/bottom.
    property bool swapped: false
    property string aLabel: ""
    property string bLabel: ""

    signal dividerPositionRequested(real position)
    // An Image item could not load its provider image.
    signal imageLoadFailed(string message)

    readonly property bool horizontal: orientation !== "vertical"
    readonly property bool dividerMode: displayMode !== "complete"
    readonly property string aSource: pair ? String(pair.aSource || "") : ""
    readonly property string bSource: pair ? String(pair.bSource || "") : ""
    readonly property bool pairReady: status === "ready" && aCanvas.imageReady
                                      && bCanvas.imageReady
    readonly property bool imageLoadError: aCanvas.imageStatus === Image.Error
                                           || bCanvas.imageStatus === Image.Error
    readonly property real dividerStep: 0.01
    readonly property real dividerPageStep: 0.1

    readonly property color colSurface: appTheme.cardSurfaceColor
    readonly property color colText: appTheme.textColor
    readonly property color colMuted: appTheme.textMutedColor
    readonly property color colDanger: appTheme.dangerColor
    readonly property color colGrip: appTheme.bgBaseColor
    readonly property color colGripActive: appTheme.buttonHoveredFillColor

    function clampPosition(value) {
        return Math.max(0, Math.min(1, value))
    }

    function requestDividerPosition(value) {
        const next = clampPosition(value)
        if (next !== root.dividerPosition)
            root.dividerPositionRequested(next)
    }

    onImageLoadErrorChanged: {
        if (imageLoadError && status === "ready")
            root.imageLoadFailed(qsTr("The comparison images could not be loaded."))
    }

    // The surface covers the viewport photograph and its overlays, and takes every pointer
    // event so that nothing reaches the viewport underneath.
    Rectangle {
        anchors.fill: parent
        color: root.colSurface
    }
    MouseArea {
        anchors.fill: parent
        acceptedButtons: Qt.AllButtons
        hoverEnabled: true
        onWheel: function (wheel) {
            wheel.accepted = true
        }
    }

    // Side captions: the first side is at the top-left, the second at the top-right
    // (left/right) or bottom-left (top/bottom).
    // Inline components do not see the `root` id, so the caption reads appTheme directly.
    component SideCaption: Row {
        id: caption
        property string sideName: ""
        property string sourceLabel: ""
        property real maximumLabelWidth: 0
        spacing: appTheme.spaceSm

        Label {
            text: caption.sideName
            color: appTheme.textMutedColor
            font.family: appTheme.uiFontFamily
            font.pixelSize: appTheme.fontSizeCaption
            font.weight: appTheme.fontWeightStrong
        }
        Label {
            text: caption.sourceLabel
            color: appTheme.textColor
            font.family: appTheme.uiFontFamily
            font.pixelSize: appTheme.fontSizeCaption
            wrapMode: Text.Wrap
            width: Math.max(0, Math.min(implicitWidth, caption.maximumLabelWidth))
        }
    }

    SideCaption {
        id: firstCaption
        objectName: "editorComparisonFirstCaption"
        anchors.left: parent.left
        anchors.top: parent.top
        anchors.margins: appTheme.spaceSm
        sideName: root.swapped ? qsTr("B") : qsTr("A")
        sourceLabel: root.swapped ? root.bLabel : root.aLabel
        maximumLabelWidth: root.width / 2 - appTheme.spaceXl
    }
    SideCaption {
        id: secondCaption
        objectName: "editorComparisonSecondCaption"
        anchors.margins: appTheme.spaceSm
        anchors.right: root.horizontal ? parent.right : undefined
        anchors.left: root.horizontal ? undefined : parent.left
        anchors.top: root.horizontal ? parent.top : undefined
        anchors.bottom: root.horizontal ? undefined : parent.bottom
        layoutDirection: root.horizontal ? Qt.RightToLeft : Qt.LeftToRight
        sideName: root.swapped ? qsTr("A") : qsTr("B")
        sourceLabel: root.swapped ? root.aLabel : root.bLabel
        maximumLabelWidth: root.width / 2 - appTheme.spaceXl
    }

    Item {
        id: stage
        objectName: "editorComparisonStage"
        anchors.fill: parent
        anchors.leftMargin: appTheme.spaceSm
        anchors.rightMargin: appTheme.spaceSm
        anchors.topMargin: firstCaption.height + 2 * appTheme.spaceSm
        anchors.bottomMargin: root.horizontal ? appTheme.spaceSm
                                              : secondCaption.height + 2 * appTheme.spaceSm

        readonly property real gap: appTheme.spaceXs
        // Common divider-mode fit. Both images share the reference extent of the pair.
        readonly property var referencePlacement: root.pair ? root.pair.aPlacement : null
        readonly property real referenceWidth: referencePlacement
                                               ? Number(referencePlacement.referenceWidth) : 0
        readonly property real referenceHeight: referencePlacement
                                                ? Number(referencePlacement.referenceHeight) : 0
        readonly property real fitScale: referenceWidth > 0 && referenceHeight > 0
                                         && width > 0 && height > 0
                                         ? Math.min(width / referenceWidth,
                                                    height / referenceHeight)
                                         : 0
        readonly property real fitX: (width - referenceWidth * fitScale) / 2
        readonly property real fitY: (height - referenceHeight * fitScale) / 2
        readonly property real fitWidth: referenceWidth * fitScale
        readonly property real fitHeight: referenceHeight * fitScale
        // Divider line in stage coordinates.
        readonly property real dividerX: fitX + root.dividerPosition * fitWidth
        readonly property real dividerY: fitY + root.dividerPosition * fitHeight

        // Region of the first (index 0) or second (index 1) side, in stage coordinates.
        function regionRect(index) {
            if (root.dividerMode) {
                if (root.horizontal) {
                    return index === 0 ? Qt.rect(0, 0, dividerX, height)
                                       : Qt.rect(dividerX, 0, width - dividerX, height)
                }
                return index === 0 ? Qt.rect(0, 0, width, dividerY)
                                   : Qt.rect(0, dividerY, width, height - dividerY)
            }
            if (root.horizontal) {
                const w = Math.max(0, (width - gap) / 2)
                return Qt.rect(index === 0 ? 0 : w + gap, 0, w, height)
            }
            const h = Math.max(0, (height - gap) / 2)
            return Qt.rect(0, index === 0 ? 0 : h + gap, width, h)
        }

        // Canvas rectangle relative to its region. In divider mode both canvases cover the whole
        // stage, so both use the same fit and only the region clip differs.
        function canvasRect(index) {
            const region = regionRect(index)
            if (root.dividerMode)
                return Qt.rect(-region.x, -region.y, width, height)
            return Qt.rect(0, 0, region.width, region.height)
        }

        Item {
            id: aRegion
            objectName: "editorComparisonRegionA"
            readonly property int slot: root.swapped ? 1 : 0
            readonly property rect region: stage.regionRect(slot)
            readonly property rect canvasArea: stage.canvasRect(slot)
            x: region.x
            y: region.y
            width: region.width
            height: region.height
            clip: true
            visible: root.pairReady

            EditorComparisonCanvas {
                id: aCanvas
                objectName: "editorComparisonCanvasA"
                x: aRegion.canvasArea.x
                y: aRegion.canvasArea.y
                width: aRegion.canvasArea.width
                height: aRegion.canvasArea.height
                source: root.aSource
                placement: root.pair ? root.pair.aPlacement : null
            }
        }

        Item {
            id: bRegion
            objectName: "editorComparisonRegionB"
            readonly property int slot: root.swapped ? 0 : 1
            readonly property rect region: stage.regionRect(slot)
            readonly property rect canvasArea: stage.canvasRect(slot)
            x: region.x
            y: region.y
            width: region.width
            height: region.height
            clip: true
            visible: root.pairReady

            EditorComparisonCanvas {
                id: bCanvas
                objectName: "editorComparisonCanvasB"
                x: bRegion.canvasArea.x
                y: bRegion.canvasArea.y
                width: bRegion.canvasArea.width
                height: bRegion.canvasArea.height
                source: root.bSource
                placement: root.pair ? root.pair.bPlacement : null
            }
        }

        // Movable divider: a 1 px line across the stage with a grip at its center.
        Item {
            id: divider
            objectName: "editorComparisonDivider"
            readonly property real hit: appTheme.iconButtonHitSizeCompact
            visible: root.dividerMode && root.pairReady
            x: root.horizontal ? stage.dividerX - hit / 2 : 0
            y: root.horizontal ? 0 : stage.dividerY - hit / 2
            width: root.horizontal ? hit : stage.width
            height: root.horizontal ? stage.height : hit
            activeFocusOnTab: visible
            Accessible.role: Accessible.Slider
            Accessible.name: qsTr("Comparison divider")
            Accessible.description: root.horizontal
                                    ? qsTr("Move left or right to reveal A or B.")
                                    : qsTr("Move up or down to reveal A or B.")

            Keys.onPressed: function (event) {
                const step = (event.modifiers & Qt.ShiftModifier) ? root.dividerPageStep
                                                                   : root.dividerStep
                const decrease = root.horizontal ? Qt.Key_Left : Qt.Key_Up
                const increase = root.horizontal ? Qt.Key_Right : Qt.Key_Down
                if (event.key === decrease) {
                    root.requestDividerPosition(root.dividerPosition - step)
                } else if (event.key === increase) {
                    root.requestDividerPosition(root.dividerPosition + step)
                } else if (event.key === Qt.Key_Home) {
                    root.requestDividerPosition(0)
                } else if (event.key === Qt.Key_End) {
                    root.requestDividerPosition(1)
                } else {
                    return
                }
                event.accepted = true
            }

            Rectangle {
                objectName: "editorComparisonDividerLine"
                color: root.colText
                x: root.horizontal ? (parent.width - width) / 2 : 0
                y: root.horizontal ? 0 : (parent.height - height) / 2
                width: root.horizontal ? 1 : parent.width
                height: root.horizontal ? parent.height : 1
            }

            Rectangle {
                id: grip
                readonly property bool active: dividerMouse.containsMouse
                                               || dividerMouse.pressed || divider.activeFocus
                anchors.centerIn: parent
                width: root.horizontal ? divider.hit / 2 : divider.hit
                height: root.horizontal ? divider.hit : divider.hit / 2
                radius: appTheme.controlRadiusSmall
                color: active ? root.colGripActive : root.colGrip
                border.width: 1
                border.color: root.colText
            }

            MouseArea {
                id: dividerMouse
                anchors.fill: parent
                hoverEnabled: true
                cursorShape: root.horizontal ? Qt.SplitHCursor : Qt.SplitVCursor
                preventStealing: true
                function moveTo(mouse) {
                    const point = mapToItem(stage, mouse.x, mouse.y)
                    if (root.horizontal && stage.fitWidth > 0)
                        root.requestDividerPosition((point.x - stage.fitX) / stage.fitWidth)
                    else if (!root.horizontal && stage.fitHeight > 0)
                        root.requestDividerPosition((point.y - stage.fitY) / stage.fitHeight)
                }
                onPressed: function (mouse) {
                    divider.forceActiveFocus()
                    moveTo(mouse)
                }
                onPositionChanged: function (mouse) {
                    if (pressed)
                        moveTo(mouse)
                }
            }
        }
    }

    Label {
        id: statusText
        objectName: "editorComparisonStatusText"
        anchors.centerIn: parent
        width: Math.min(implicitWidth, parent.width - 2 * appTheme.spaceXl)
        horizontalAlignment: Text.AlignHCenter
        wrapMode: Text.Wrap
        visible: !root.pairReady && text.length > 0
        color: root.status === "failed" || root.imageLoadError ? root.colDanger : root.colMuted
        font.family: appTheme.uiFontFamily
        font.pixelSize: appTheme.fontSizeBody
        text: {
            if (root.status === "failed")
                return root.errorText
            if (root.imageLoadError)
                return qsTr("The comparison images could not be loaded.")
            if (root.status === "loading" || root.status === "ready")
                return qsTr("Rendering comparison images")
            return ""
        }
    }
}
