import QtQuick
import QtQuick.Controls

// Window caption layer for platforms that draw their own caption (issue 1140).
//
// A modal popup blocks every press outside its own popup item, and that used
// to include the caption buttons and the toolbar drag area: with Settings open
// the window could not be closed, moved, or maximized. Two non-modal popups sit
// above every dialog and keep them usable:
//   captionPopup - minimize / maximize / close in the window's top-right corner;
//   dragPopup    - the toolbar's empty stretch: drag moves the window and a
//                  double-click toggles maximize, as on the toolbar itself.
// Each popup covers only its own rectangle, because a popup takes every press
// inside it. The toolbar's buttons stay clickable without a modal and stay
// blocked under one. QQuickOverlay stops filtering at the popup that holds the
// pressed item, so a press here reaches it before the modal dialog's block.
Item {
    id: root
    property var host: null
    property var theme: null
    property Item toolbar: null
    property bool active: false
    // Width of the window edge strip that belongs to the resize handles.
    property real resizeInset: 0
    // Above every dialog (Settings and Welcome use 30, the editor close check 40).
    readonly property real layerZ: 1000
    // Width the toolbar leaves free at the window's right edge for the buttons.
    readonly property real reservedWidth: root.active ? captionButtons.width : 0

    // The toolbar's top-left corner in window coordinates. mapToItem() is not a
    // binding dependency, so the geometry it depends on is read explicitly.
    readonly property point toolbarOrigin: {
        const bar = root.toolbar
        if (!bar)
            return Qt.point(0, appTheme.spaceMd)
        void [bar.x, bar.y, bar.width, bar.height,
              root.host ? root.host.width : 0, root.host ? root.host.height : 0]
        return bar.mapToItem(null, 0, 0)
    }
    // The toolbar's empty stretch in window coordinates.
    readonly property rect dragRect: root.toolbar && root.active
                                     ? Qt.rect(root.toolbarOrigin.x + root.toolbar.dragStripX,
                                               root.toolbarOrigin.y,
                                               root.toolbar.dragStripWidth,
                                               root.toolbar.height)
                                     : Qt.rect(0, 0, 0, 0)

    Popup {
        id: captionPopup
        objectName: "windowCaptionPopup"
        parent: Overlay.overlay
        visible: root.active
        z: root.layerZ
        modal: false
        dim: false
        focus: false
        closePolicy: Popup.NoAutoClose
        padding: 0
        margins: 0
        background: null
        // The popup leaves the top and right resize strips free; the buttons
        // still paint the full cells (the content does not clip).
        x: (parent ? parent.width : 0) - captionButtons.width
        y: root.resizeInset
        width: captionButtons.width - root.resizeInset
        height: captionButtons.height - root.resizeInset

        contentItem: Item {
            WindowCaptionButtons {
                id: captionButtons
                y: -root.resizeInset
                theme: root.theme
                host: root.host
                topBand: root.toolbarOrigin.y
                toolbarHeight: root.toolbar ? root.toolbar.height
                                            : appTheme.iconButtonHitSizeCompact + appTheme.spaceSm
                inputTopInset: root.resizeInset
                inputRightInset: root.resizeInset
            }
        }
    }

    Popup {
        id: dragPopup
        objectName: "windowCaptionDragStrip"
        parent: Overlay.overlay
        visible: root.active && root.dragRect.width > 0
        z: root.layerZ
        modal: false
        dim: false
        focus: false
        closePolicy: Popup.NoAutoClose
        padding: 0
        margins: 0
        background: null
        x: root.dragRect.x
        y: root.dragRect.y
        width: root.dragRect.width
        height: root.dragRect.height

        contentItem: Item {
            TapHandler {
                acceptedButtons: Qt.LeftButton
                gesturePolicy: TapHandler.DragThreshold
                onDoubleTapped: if (root.host) root.host.toggleMaximizeAnimated()
            }
            DragHandler {
                target: null
                onActiveChanged: if (active && root.host) root.host.startSystemMove()
            }
        }
    }
}
