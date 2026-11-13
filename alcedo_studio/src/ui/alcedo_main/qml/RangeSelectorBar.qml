import QtQuick
import QtQuick.Layouts

// Two-handle range selector on a filled track (Mask ranges MR4, #564). The selected interval is
// framed. Outside it the bar is dimmed with the soft transition of the kernels: the dimming fades
// in over `softness` (value units) beyond each handle. Drag a handle to move one end, drag the
// interval body to move both, double-click to select everything (resetRequested). A drag moves
// by the cursor delta from the press, so a press alone changes nothing; Shift drags at 1/10
// speed. A focused handle steps with Left/Right, and the wheel over a handle steps it (`stepped`,
// Shift = coarse); the owner sizes and settles the steps. With wraps, the interval can cross the
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
    // Soft transition outside the handles, in value units (0: hard edge).
    property real softness: 0

    signal began()
    signal moved(real first, real second)
    signal finished()
    signal resetRequested()
    // part: "first" or "second"; steps: signed count; coarse: Shift held.
    signal stepped(string part, int steps, bool coarse)

    // Handle values while a drag is open; the owner values otherwise.
    property bool _dragging: false
    property real _dragFirst: from
    property real _dragSecond: to
    // "first", "second", "body", or "".
    property string _dragPart: ""
    property real _pressX: 0
    property real _pressFirst: from
    property real _pressSecond: to
    // Shift (1/10 speed) at the last anchor of the open drag, and the last cursor x.
    property bool _fine: false
    property real _lastX: 0
    // Handle that keys step: "first" or "second".
    property string focusedPart: "first"
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
    activeFocusOnTab: true
    Accessible.role: Accessible.Slider
    Accessible.name: bar.accessibleName

    Keys.onLeftPressed: function (event) {
        bar.stepped(bar.focusedPart, -1, (event.modifiers & Qt.ShiftModifier) !== 0)
        event.accepted = true
    }
    Keys.onRightPressed: function (event) {
        bar.stepped(bar.focusedPart, 1, (event.modifiers & Qt.ShiftModifier) !== 0)
        event.accepted = true
    }
    // Tab moves from the first handle to the second, then out of the bar.
    Keys.onTabPressed: function (event) {
        if (bar.focusedPart === "first") {
            bar.focusedPart = "second"
            event.accepted = true
        } else {
            event.accepted = false
        }
    }
    Keys.onBacktabPressed: function (event) {
        if (bar.focusedPart === "second") {
            bar.focusedPart = "first"
            event.accepted = true
        } else {
            event.accepted = false
        }
    }
    onActiveFocusChanged: {
        if (activeFocus && !_dragging && !_focusFromPress)
            focusedPart = "first"
        _focusFromPress = false
    }
    property bool _focusFromPress: false

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
    function smoothStep(e0, e1, v) {
        if (e1 <= e0)
            return v < e0 ? 0 : 1
        var t = Math.max(0, Math.min(1, (v - e0) / (e1 - e0)))
        return t * t * (3 - 2 * t)
    }
    // Weight of value v as the kernels compute it for the shown interval and softness.
    function weightAt(v) {
        if (fullRange)
            return 1
        var a = shownFirst
        var b = shownSecond
        if (!wraps)
            return smoothStep(a - softness, a, v) * (1 - smoothStep(b, b + softness, v))
        var offset = v - a
        if (offset < 0)
            offset += span
        var w = circularWidth(a, b)
        var d = offset <= w ? 0 : Math.min(offset - w, span - offset)
        return 1 - smoothStep(0, softness, d)
    }
    // Cursor delta since the anchor, in value units (1/10 with Shift).
    function deltaTo(x) {
        return (x - _pressX) / trackWidth * span * (_fine ? 0.1 : 1)
    }
    // Shift changed during the drag: anchor at the values of the previous cursor position, so the
    // handle does not jump and the move since then runs at the new speed.
    function setFine(fine, x) {
        if (fine === _fine)
            return
        _pressX = x
        _pressFirst = _dragFirst
        _pressSecond = _dragSecond
        _fine = fine
    }
    function dragTo(x) {
        var a = _pressFirst
        var b = _pressSecond
        var delta = deltaTo(x)
        if (_dragPart === "first" || _dragPart === "second") {
            // A handle moves by the delta from its press value, not to the cursor position.
            var v = Math.max(from, Math.min(to, (_dragPart === "first" ? _pressFirst
                                                                         : _pressSecond) + delta))
        }
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
        a = Math.max(from, Math.min(to, a))
        b = Math.max(from, Math.min(to, b))
        if (a === _dragFirst && b === _dragSecond)
            return
        _dragFirst = a
        _dragSecond = b
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

    // Dimmed outside of the selected interval: each pixel column is dimmed by 1 - weight, so the
    // soft transition beyond each handle is drawn as the kernels weight it.
    Canvas {
        id: dimming
        x: fill.x
        y: fill.y
        width: fill.width
        height: fill.height
        visible: !bar.fullRange
        readonly property color dimColor: appTheme.overlayColor
        readonly property var paintKey: [bar.shownFirst, bar.shownSecond, bar.softness,
                                         bar.fullRange, width, height, dimColor]
        onPaintKeyChanged: requestPaint()
        onPaint: {
            var ctx = getContext("2d")
            ctx.clearRect(0, 0, width, height)
            if (bar.fullRange)
                return
            var c = dimColor
            for (var px = 0; px < Math.ceil(width); ++px) {
                var dim = 1 - bar.weightAt(bar.valueAt(bar.trackX + px + 0.5))
                if (dim <= 0.002)
                    continue
                ctx.fillStyle = Qt.rgba(c.r, c.g, c.b, c.a * dim)
                ctx.fillRect(px, 0, 1, height)
            }
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
            readonly property bool keyFocus: bar.activeFocus
                                             && bar.focusedPart === (index === 0 ? "first" : "second")
            x: (index === 0 ? bar.firstX : bar.secondX) - width / 2
            y: (bar.height - height) / 2
            width: bar.handleWidth
            height: bar.trackHeight + 8
            radius: 2
            color: appTheme.editorSliderHandleColor
            border.width: keyFocus ? 2 : 1
            border.color: keyFocus ? appTheme.textColor : appTheme.editorSliderHandleBorderColor
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
            if (part === "first" || part === "second")
                bar.focusedPart = part
            if (!bar.activeFocus) {
                bar._focusFromPress = true
                bar.forceActiveFocus(Qt.MouseFocusReason)
            }
            bar._dragPart = part
            bar._fine = (mouse.modifiers & Qt.ShiftModifier) !== 0
            bar._pressX = mouse.x
            bar._lastX = mouse.x
            bar._pressFirst = bar.first
            bar._pressSecond = bar.second
            bar._dragFirst = bar.first
            bar._dragSecond = bar.second
            bar.lockScroll(true)
            bar._dragging = true
            bar.began()
        }
        onPositionChanged: function (mouse) {
            if (!bar._dragging)
                return
            bar.setFine((mouse.modifiers & Qt.ShiftModifier) !== 0, bar._lastX)
            bar.dragTo(mouse.x)
            bar._lastX = mouse.x
        }
        // The wheel over a handle steps it; elsewhere the wheel scrolls the panel.
        onWheel: function (wheel) {
            var part = bar._dragging ? "" : bar.partAt(wheel.x)
            var delta = wheel.angleDelta.y !== 0 ? wheel.angleDelta.y : wheel.angleDelta.x
            if ((part !== "first" && part !== "second") || delta === 0) {
                wheel.accepted = false
                return
            }
            _wheelRest += delta
            var steps = _wheelRest > 0 ? Math.floor(_wheelRest / 120) : Math.ceil(_wheelRest / 120)
            if (steps !== 0) {
                _wheelRest -= steps * 120
                bar.focusedPart = part
                bar.stepped(part, steps, (wheel.modifiers & Qt.ShiftModifier) !== 0)
            }
            wheel.accepted = true
        }
        property int _wheelRest: 0
        onReleased: endDrag()
        onCanceled: {
            resetOnRelease = false
            endDrag()
        }
        onDoubleClicked: resetOnRelease = true
    }
}
