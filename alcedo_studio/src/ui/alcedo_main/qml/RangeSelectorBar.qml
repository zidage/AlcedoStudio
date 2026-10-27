import QtQuick
import QtQuick.Layouts

// Two-handle range selector on a filled track (Mask ranges MR4). The selected interval is framed
// and the outside is dimmed. Drag a handle to move one end, drag the interval body to move both,
// double-click to select everything (resetRequested). With wraps, the interval can cross the
// track end: first > second selects [first, to] and [from, second] and is drawn in two parts.
// The bar keeps no model state: first/second come from the owner, and a drag reports
// began / moved / finished. Optional flickable must expose beginInputLock / endInputLock.
Item {
    id: bar

    property real from: 0
    property real to: 1
    property real first: from
    property real second: to
    property bool wraps: false
    property real minimumWidth: 0
    // [{position, color}], position in [0, 1].
    property var gradientStops: []
    property var flickable: null
    property string accessibleName: ""
    property int trackHeight: 14
    property int handleWidth: 6

    signal began()
    signal moved(real first, real second)
    signal finished()
    signal resetRequested()

    // Handle values while a drag is open; the owner values otherwise.
    property bool _dragging: false
    property real _dragFirst: from
    property real _dragSecond: to
    // "first", "second", "body", or "".
    property string _dragPart: ""
    property real _pressX: 0
    property real _pressFirst: from
    property real _pressSecond: to
    property bool _scrollLocked: false
    readonly property real shownFirst: _dragging ? _dragFirst : first
    readonly property real shownSecond: _dragging ? _dragSecond : second
    readonly property real span: Math.max(1e-9, to - from)
    readonly property bool fullRange: shownFirst <= from && shownSecond >= to
    readonly property bool wrapped: wraps && shownFirst > shownSecond
    readonly property real trackX: handleWidth / 2
    readonly property real trackWidth: Math.max(1, width - handleWidth)
    readonly property real firstX: xOf(shownFirst)
    readonly property real secondX: xOf(shownSecond)

    implicitHeight: trackHeight + 8
    Layout.fillWidth: true
    Layout.preferredHeight: implicitHeight
    opacity: enabled ? 1.0 : 0.45
    Accessible.role: Accessible.Slider
    Accessible.name: bar.accessibleName

    function xOf(value) {
        return trackX + (value - from) / span * trackWidth
    }
    function valueAt(x) {
        return Math.max(from, Math.min(to, from + (x - trackX) / trackWidth * span))
    }
    function circularWidth(a, b) {
        return b > a ? b - a : b + span - a
    }
    function wrapValue(v) {
        var w = (v - from) % span
        if (w < 0)
            w += span
        return from + w
    }
    function lockScroll(lock) {
        if (!flickable)
            return
        if (lock && !_scrollLocked && typeof flickable.beginInputLock === "function") {
            flickable.beginInputLock()
            _scrollLocked = true
        } else if (!lock && _scrollLocked) {
            if (typeof flickable.endInputLock === "function")
                flickable.endInputLock()
            _scrollLocked = false
        }
    }
    function insideBody(x) {
        if (fullRange)
            return false
        if (wrapped)
            return x >= firstX || x <= secondX
        return x > firstX && x < secondX
    }
    function partAt(x) {
        var grab = Math.max(handleWidth, 8)
        var dFirst = Math.abs(x - firstX)
        var dSecond = Math.abs(x - secondX)
        if (dFirst <= grab || dSecond <= grab) {
            // Coincident handles at a track end: take the one that can move inward.
            if (Math.abs(dFirst - dSecond) < 0.5)
                return x > (firstX + secondX) / 2 || secondX >= xOf(to) ? "first" : "second"
            return dFirst < dSecond ? "first" : "second"
        }
        return insideBody(x) ? "body" : ""
    }
    function dragTo(x) {
        var a = _pressFirst
        var b = _pressSecond
        var v = valueAt(x)
        if (_dragPart === "first") {
            if (wraps) {
                if (circularWidth(v, b) >= minimumWidth || (v <= from && b >= to))
                    a = v >= to ? from : v
            } else {
                a = Math.min(v, b - minimumWidth)
            }
        } else if (_dragPart === "second") {
            if (wraps) {
                if (circularWidth(a, v) >= minimumWidth || (a <= from && v >= to))
                    b = v <= from ? to : v
            } else {
                b = Math.max(v, a + minimumWidth)
            }
        } else if (_dragPart === "body") {
            var delta = (x - _pressX) / trackWidth * span
            if (wraps) {
                a = wrapValue(_pressFirst + delta)
                b = wrapValue(_pressSecond + delta)
                if (b <= from)
                    b = to
            } else {
                delta = Math.max(from - _pressFirst, Math.min(to - _pressSecond, delta))
                a = _pressFirst + delta
                b = _pressSecond + delta
            }
        }
        _dragFirst = Math.max(from, Math.min(to, a))
        _dragSecond = Math.max(from, Math.min(to, b))
        bar.moved(_dragFirst, _dragSecond)
    }

    // Track fill from the owner's stops.
    Canvas {
        id: fill
        x: bar.trackX
        y: (bar.height - bar.trackHeight) / 2
        width: bar.trackWidth
        height: bar.trackHeight
        onPaint: {
            var ctx = getContext("2d")
            ctx.clearRect(0, 0, width, height)
            var stops = bar.gradientStops || []
            if (stops.length === 0)
                return
            var g = ctx.createLinearGradient(0, 0, width, 0)
            for (var i = 0; i < stops.length; ++i)
                g.addColorStop(stops[i].position, stops[i].color)
            ctx.fillStyle = g
            ctx.fillRect(0, 0, width, height)
        }
        onWidthChanged: requestPaint()
        Connections {
            target: bar
            function onGradientStopsChanged() { fill.requestPaint() }
        }
    }

    // Dimmed outside of the selected interval.
    Item {
        x: fill.x
        y: fill.y
        width: fill.width
        height: fill.height
        visible: !bar.fullRange

        Rectangle {
            // Non-wrapped: before first. Wrapped: between second and first.
            x: bar.wrapped ? bar.secondX - bar.trackX : 0
            width: Math.max(0, bar.wrapped ? bar.firstX - bar.secondX : bar.firstX - bar.trackX)
            height: parent.height
            color: appTheme.overlayColor
        }
        Rectangle {
            visible: !bar.wrapped
            x: bar.secondX - bar.trackX
            width: Math.max(0, parent.width - x)
            height: parent.height
            color: appTheme.overlayColor
        }
    }

    Rectangle {
        x: fill.x
        y: fill.y
        width: fill.width
        height: fill.height
        color: "transparent"
        border.width: 1
        border.color: appTheme.cardBorderColor
    }

    // Frame of the selected interval: one part, or two parts when it wraps.
    Rectangle {
        visible: !bar.fullRange
        x: bar.firstX
        y: fill.y - 1
        width: Math.max(0, bar.wrapped ? fill.x + fill.width - bar.firstX
                                       : bar.secondX - bar.firstX)
        height: fill.height + 2
        color: "transparent"
        border.width: 1
        border.color: appTheme.editorSliderHandleColor
    }
    Rectangle {
        visible: !bar.fullRange && bar.wrapped
        x: fill.x
        y: fill.y - 1
        width: Math.max(0, bar.secondX - fill.x)
        height: fill.height + 2
        color: "transparent"
        border.width: 1
        border.color: appTheme.editorSliderHandleColor
    }

    Repeater {
        model: 2
        delegate: Rectangle {
            required property int index
            x: (index === 0 ? bar.firstX : bar.secondX) - width / 2
            y: (bar.height - height) / 2
            width: bar.handleWidth
            height: bar.trackHeight + 8
            radius: 2
            color: appTheme.editorSliderHandleColor
            border.width: 1
            border.color: appTheme.editorSliderHandleBorderColor
        }
    }

    MouseArea {
        anchors.fill: parent
        enabled: bar.enabled
        preventStealing: true
        acceptedButtons: Qt.LeftButton
        cursorShape: bar._dragging ? Qt.ClosedHandCursor : Qt.ArrowCursor

        // The second press of a double-click opens a drag first; the reset runs after it settles.
        property bool resetOnRelease: false

        function endDrag() {
            if (bar._dragging) {
                bar.lockScroll(false)
                bar.finished()
                bar._dragging = false
                bar._dragPart = ""
            }
            if (resetOnRelease) {
                resetOnRelease = false
                bar.resetRequested()
            }
        }

        onPressed: function (mouse) {
            var part = bar.partAt(mouse.x)
            if (part === "")
                return
            bar._dragPart = part
            bar._pressX = mouse.x
            bar._pressFirst = bar.first
            bar._pressSecond = bar.second
            bar._dragFirst = bar.first
            bar._dragSecond = bar.second
            bar.lockScroll(true)
            bar._dragging = true
            bar.began()
        }
        onPositionChanged: function (mouse) {
            if (bar._dragging)
                bar.dragTo(mouse.x)
        }
        onReleased: endDrag()
        onCanceled: {
            resetOnRelease = false
            endDrag()
        }
        onDoubleClicked: resetOnRelease = true
    }
}
