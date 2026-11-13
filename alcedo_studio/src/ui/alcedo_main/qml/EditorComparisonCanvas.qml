import QtQuick

// One comparison image placed in the full source reference space.
//
// The canvas fits the reference extent of `placement` into its own size (aspect ratio kept,
// centered) and draws the rendered image with one affine transform: fit * render_to_reference.
// The Image item keeps the exact render pixel extent as its local size, so a cropped render
// covers only its crop footprint and a rotated crop covers its source-space quadrilateral.
// Areas without image pixels stay transparent; the parent paints the surface behind them.
//
// The canvas only loads a completed provider image. It does no rendering and owns no
// comparison state.
Item {
    id: root
    objectName: "editorComparisonCanvas"

    // Provider URL of a completed image, or empty.
    property string source: ""
    // ComparisonImagePlacement::ToVariantMap() of the image, or null.
    property var placement: null

    readonly property real referenceWidth: placement ? Number(placement.referenceWidth) : 0
    readonly property real referenceHeight: placement ? Number(placement.referenceHeight) : 0
    readonly property real renderWidth: placement ? Number(placement.renderWidth) : 0
    readonly property real renderHeight: placement ? Number(placement.renderHeight) : 0
    readonly property bool hasPlacement: referenceWidth > 0 && referenceHeight > 0
                                         && renderWidth > 0 && renderHeight > 0

    // Reference-to-canvas fit: canvas = fitScale * reference + (fitX, fitY).
    readonly property real fitScale: hasPlacement && width > 0 && height > 0
                                     ? Math.min(width / referenceWidth, height / referenceHeight)
                                     : 0
    readonly property real fitX: (width - referenceWidth * fitScale) / 2
    readonly property real fitY: (height - referenceHeight * fitScale) / 2
    readonly property real fitWidth: referenceWidth * fitScale
    readonly property real fitHeight: referenceHeight * fitScale

    // fit * render_to_reference, as a 4x4 matrix in row-major order.
    readonly property matrix4x4 imageMatrix: {
        if (!hasPlacement || fitScale <= 0)
            return Qt.matrix4x4(0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1)
        const p = placement
        const s = fitScale
        return Qt.matrix4x4(s * Number(p.m11), s * Number(p.m12), 0, s * Number(p.dx) + fitX,
                            s * Number(p.m21), s * Number(p.m22), 0, s * Number(p.dy) + fitY,
                            0, 0, 1, 0,
                            0, 0, 0, 1)
    }

    readonly property int imageStatus: photo.status
    readonly property bool imageReady: source.length > 0 && photo.status === Image.Ready

    // Canvas position of a reference-space point.
    function mapReferencePoint(x, y) {
        return Qt.point(fitX + x * fitScale, fitY + y * fitScale)
    }

    clip: true

    Image {
        id: photo
        objectName: "editorComparisonCanvasImage"
        x: 0
        y: 0
        width: root.renderWidth
        height: root.renderHeight
        source: root.source
        // The provider serves each operation once; a new pair has new URLs.
        cache: false
        asynchronous: true
        smooth: true
        mipmap: true
        fillMode: Image.Stretch
        visible: root.hasPlacement && root.fitScale > 0
        transform: Matrix4x4 {
            matrix: root.imageMatrix
        }
    }
}
