import QtQuick
import QtQuick.Controls.impl
import QtQuick.Effects

// The preview slot of a browser tile or list row (LUT browser, preset browser): the
// photo fits the slot, is centered, and has controlRadiusSmall corners (MultiEffect
// mask), like a filmstrip photo. Until the image is ready the slot shows its fill and,
// when placeholderSource is set, that icon centered. The owner gives the URL; the
// preview controller keeps the pixels.
Rectangle {
    id: thumb

    property string previewUrl: ""
    property url placeholderSource: ""
    property color placeholderColor: appTheme.iconColor
    property real placeholderOpacity: 1.0
    // Object name prefix of the parts: <prefix>Placeholder, <prefix>Preview, <prefix>PreviewImage.
    property string namePrefix: ""
    readonly property bool painted: previewUrl.length > 0
                                    && previewImage.status === Image.Ready
                                    && previewImage.implicitWidth > 0
                                    && previewImage.implicitHeight > 0
    // The icon fits small slots (list rows) as well as grid tiles.
    readonly property real placeholderSize: Math.min(appTheme.editorLutTileIconSize,
                                                     Math.round(Math.min(width, height) * 0.6))

    radius: appTheme.controlRadiusSmall
    color: "transparent"

    ColorImage {
        objectName: thumb.namePrefix + "Placeholder"
        anchors.centerIn: parent
        width: thumb.placeholderSize
        height: thumb.placeholderSize
        visible: !thumb.painted && String(thumb.placeholderSource).length > 0
        source: thumb.placeholderSource
        sourceSize.width: appTheme.editorLutTileIconSize * 2
        sourceSize.height: appTheme.editorLutTileIconSize * 2
        color: thumb.placeholderColor
        opacity: thumb.placeholderOpacity
    }

    Item {
        id: previewPhoto
        objectName: thumb.namePrefix + "Preview"
        readonly property real fitScale: thumb.painted
                                         ? Math.min(thumb.width / previewImage.implicitWidth,
                                                    thumb.height / previewImage.implicitHeight)
                                         : 1
        width: thumb.painted ? Math.round(previewImage.implicitWidth * fitScale) : thumb.width
        height: thumb.painted ? Math.round(previewImage.implicitHeight * fitScale) : thumb.height
        x: Math.round((thumb.width - width) / 2)
        y: Math.round((thumb.height - height) / 2)
        visible: thumb.previewUrl.length > 0

        Rectangle {
            id: previewMask
            anchors.fill: parent
            radius: appTheme.controlRadiusSmall
            visible: false
            layer.enabled: true
        }

        Image {
            id: previewImage
            objectName: thumb.namePrefix + "PreviewImage"
            anchors.fill: parent
            source: thumb.previewUrl
            asynchronous: true
            // Every shown preview has its own URL; the controller keeps the pixels.
            cache: false
            fillMode: Image.PreserveAspectFit
            opacity: thumb.painted ? 1 : 0
            layer.enabled: thumb.painted
            layer.effect: MultiEffect {
                maskEnabled: true
                maskSource: previewMask
            }
        }
    }
}
