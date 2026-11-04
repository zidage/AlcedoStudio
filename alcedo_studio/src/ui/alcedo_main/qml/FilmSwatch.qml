import QtQuick

// Swatch of a film or print material in the film print browser. The shape
// shows the material, the colours show the stock (FilmPresentation in
// app/film_stock_presentation.hpp). The colours are the identity of a stock,
// so they are the same in each theme.
//
//   photo_negative    tab in two colours: brand colour, then stock colour
//   cinema_negative   dark can with perforations and one stock colour
//   reversal          slide mount, one window band for each colour
//   print_paper       white sheet with a coloured edge
//   cinema_print      clear strip with perforations
//   display_material  glossy sheet
Item {
    id: root

    property string shape: "photo_negative"
    property var colors: []
    property bool large: false
    property real cornerRadius: large ? 6 : 5

    implicitWidth: large ? 44 : 34
    implicitHeight: large ? 30 : 22

    function colorAt(index) {
        if (!root.colors || root.colors.length === 0)
            return "#808080"
        return String(root.colors[Math.min(index, root.colors.length - 1)])
    }

    onShapeChanged: swatchCanvas.requestPaint()
    onColorsChanged: swatchCanvas.requestPaint()

    Canvas {
        id: swatchCanvas
        anchors.fill: parent
        antialiasing: true
        onWidthChanged: requestPaint()
        onHeightChanged: requestPaint()

        function roundRect(ctx, x, y, w, h, r) {
            ctx.beginPath()
            ctx.moveTo(x + r, y)
            ctx.lineTo(x + w - r, y)
            ctx.arcTo(x + w, y, x + w, y + r, r)
            ctx.lineTo(x + w, y + h - r)
            ctx.arcTo(x + w, y + h, x + w - r, y + h, r)
            ctx.lineTo(x + r, y + h)
            ctx.arcTo(x, y + h, x, y + h - r, r)
            ctx.lineTo(x, y + r)
            ctx.arcTo(x, y, x + r, y, r)
            ctx.closePath()
        }

        function perforations(ctx, w, h, ink) {
            const pitch = root.large ? 6 : 5
            const radius = root.large ? 1.1 : 0.9
            const inset = root.large ? 2.5 : 2.2
            ctx.fillStyle = ink
            for (var x = pitch / 2; x < w; x += pitch) {
                ctx.beginPath()
                ctx.arc(x, inset, radius, 0, Math.PI * 2)
                ctx.arc(x, h - inset, radius, 0, Math.PI * 2)
                ctx.fill()
            }
        }

        function hairline(ctx, x, y, w, h, r) {
            roundRect(ctx, x + 0.5, y + 0.5, w - 1, h - 1, Math.max(0, r - 0.5))
            ctx.strokeStyle = Qt.rgba(1, 1, 1, 0.10)
            ctx.lineWidth = 1
            ctx.stroke()
        }

        onPaint: {
            const ctx = getContext("2d")
            const w = width
            const h = height
            ctx.reset()
            if (w <= 0 || h <= 0)
                return
            const r = Math.min(root.cornerRadius, h / 2)

            if (root.shape === "reversal") {
                // Square slide mount, centred, with a window of colour bands.
                const side = h
                const x0 = (w - side) / 2
                const mountRadius = Math.min(root.large ? 4 : 3.5, root.cornerRadius)
                roundRect(ctx, x0, 0, side, side, mountRadius)
                ctx.fillStyle = "#ebe6da"
                ctx.fill()
                // Thin outline: the mount stays visible on a light selected fill.
                roundRect(ctx, x0 + 0.5, 0.5, side - 1, side - 1, Math.max(0, mountRadius - 0.5))
                ctx.strokeStyle = Qt.rgba(0, 0, 0, 0.28)
                ctx.lineWidth = 1
                ctx.stroke()
                const insetY = side * 0.24
                const insetX = side * 0.18
                const wx = x0 + insetX
                const wy = insetY
                const ww = side - insetX * 2
                const wh = side - insetY * 2
                ctx.save()
                roundRect(ctx, wx, wy, ww, wh, 1.5)
                ctx.clip()
                const count = Math.max(1, root.colors ? root.colors.length : 1)
                for (var i = 0; i < count; ++i) {
                    ctx.fillStyle = root.colorAt(i)
                    ctx.fillRect(wx, wy + wh * i / count, ww, wh / count + 0.5)
                }
                ctx.restore()
                return
            }

            ctx.save()
            roundRect(ctx, 0, 0, w, h, r)
            ctx.clip()
            if (root.shape === "cinema_negative") {
                ctx.fillStyle = "#1b1b1b"
                ctx.fillRect(0, 0, w, h)
                ctx.fillStyle = root.colorAt(0)
                ctx.fillRect(w * 0.38, 0, w * 0.62, h)
                perforations(ctx, w, h, Qt.rgba(1, 1, 1, 0.30))
            } else if (root.shape === "print_paper") {
                ctx.fillStyle = "#f1eee6"
                ctx.fillRect(0, 0, w, h)
                const edge = ctx.createLinearGradient(0, 0, w, 0)
                edge.addColorStop(0, root.colorAt(0))
                edge.addColorStop(1, root.colorAt(1))
                ctx.fillStyle = edge
                ctx.fillRect(0, h * 0.74, w, h * 0.26)
            } else if (root.shape === "cinema_print" || root.shape === "display_material") {
                const base = ctx.createLinearGradient(0, 0, w, 0)
                base.addColorStop(0, root.colorAt(0))
                base.addColorStop(1, root.colorAt(1))
                ctx.fillStyle = base
                ctx.fillRect(0, 0, w, h)
                if (root.shape === "cinema_print") {
                    perforations(ctx, w, h, Qt.rgba(0, 0, 0, 0.45))
                } else {
                    const gloss = ctx.createLinearGradient(0, 0, 0, h)
                    gloss.addColorStop(0, Qt.rgba(1, 1, 1, 0.55))
                    gloss.addColorStop(0.45, Qt.rgba(1, 1, 1, 0))
                    ctx.fillStyle = gloss
                    ctx.fillRect(0, 0, w, h)
                }
            } else {
                // photo_negative: brand colour, then the stock colour past a diagonal.
                ctx.fillStyle = root.colorAt(0)
                ctx.fillRect(0, 0, w, h)
                ctx.beginPath()
                ctx.moveTo(w * 0.66, 0)
                ctx.lineTo(w, 0)
                ctx.lineTo(w, h)
                ctx.lineTo(w * 0.46, h)
                ctx.closePath()
                ctx.fillStyle = root.colorAt(1)
                ctx.fill()
            }
            ctx.restore()
            hairline(ctx, 0, 0, w, h, r)
        }
    }
}
