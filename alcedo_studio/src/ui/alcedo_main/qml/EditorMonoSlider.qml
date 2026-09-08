import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts

// Relative pointer slider used by White Balance and CDL master rows.
// MouseArea owns the drag so Controls.Slider cannot absolute-seek. Optional
// flickable must expose beginInputLock / endInputLock.
Slider {
    id: mono

    property var flickable: null
    property var gradientStops: null
    property var onBegin: function () {}
    property var onUpdate: function (v) {}
    property var onFinish: function () {}
    property var onReset: function () {}
    property real externalValue: 0
    /// Full-track mouse travel maps to this fraction of the value range.
    property real pointerGain: 0.32
    property int handleSize: 22
    property int rowHeight: 32
    property color trackColor: appTheme.editorSliderTrackColor
    property color fillColor: appTheme.editorSliderHandleColor
    property color handleColor: appTheme.editorSliderHandleColor
    property color handleBorderColor: appTheme.editorSliderHandleBorderColor
    property color trackBorderColor: appTheme.cardBorderColor

    property bool _pointerMoved: false
    property real _pressValue: 0
    property real _pressLocalX: 0
    property double _lastClickMs: 0
    property bool _scrollLocked: false
    property bool _handleDragging: false

    from: 0
    to: 1
    stepSize: 1
    live: true
    touchDragThreshold: 0
    snapMode: Slider.SnapAlways
    padding: 0
    leftPadding: 0
    rightPadding: 0
    topPadding: 0
    bottomPadding: 0
    hoverEnabled: false
    Layout.fillWidth: true
    Layout.preferredHeight: mono.rowHeight
    implicitHeight: mono.rowHeight

    Component.onCompleted: value = externalValue

    onExternalValueChanged: {
        if (!_handleDragging)
            value = externalValue
    }

    function lockScroll(lock) {
        if (!flickable)
            return
        if (lock) {
            if (!_scrollLocked && typeof flickable.beginInputLock === "function") {
                flickable.beginInputLock()
                _scrollLocked = true
            }
        } else if (_scrollLocked) {
            if (typeof flickable.endInputLock === "function")
                flickable.endInputLock()
            _scrollLocked = false
        }
    }

    function syncFromExternal() {
        value = externalValue
    }

    function handleCenterX() {
        return mono.leftPadding
                + mono.visualPosition * (mono.availableWidth - mono.handleSize)
                + mono.handleSize * 0.5
    }

    function isNearHandle(localX) {
        return Math.abs(localX - handleCenterX())
                <= Math.max(mono.handleSize * 0.5 + 12, 16)
    }

    function valueFromDragDelta(localX) {
        var trackW = Math.max(1e-6, mono.availableWidth - mono.handleSize)
        var dx = localX - mono._pressLocalX
        var range = mono.to - mono.from
        var v = mono._pressValue + (dx / trackW) * range * mono.pointerGain
        if (mono.stepSize > 0)
            v = Math.round(v / mono.stepSize) * mono.stepSize
        return Math.max(mono.from, Math.min(mono.to, v))
    }

    function finishPointerPress() {
        var now = Date.now()
        var isDouble = !_pointerMoved
                && (now - _lastClickMs) < 350
                && Math.abs(value - _pressValue) <= Math.max(stepSize * 0.5, 1e-9)
        _lastClickMs = now
        lockScroll(false)
        if (isDouble && enabled) {
            onReset()
            syncFromExternal()
        } else if (_handleDragging) {
            onFinish()
            syncFromExternal()
        }
        _handleDragging = false
    }

    background: Rectangle {
        readonly property real edge: mono.handleSize * 0.5
        x: mono.leftPadding + edge
        y: mono.topPadding + mono.availableHeight / 2 - height / 2
        width: Math.max(0, mono.availableWidth - mono.handleSize)
        height: 6
        radius: 3
        color: mono.trackColor
        border.width: mono.gradientStops ? 1 : 0
        border.color: mono.trackBorderColor
        gradient: mono.gradientStops

        Rectangle {
            visible: mono.gradientStops === null
            x: 0
            y: 0
            width: mono.visualPosition * parent.width
            height: parent.height
            radius: 3
            color: mono.fillColor
        }
    }
    handle: Rectangle {
        x: mono.leftPadding + mono.visualPosition * (mono.availableWidth - width)
        y: mono.topPadding + mono.availableHeight / 2 - height / 2
        width: mono.handleSize
        height: mono.handleSize
        radius: width / 2
        color: mono.handleColor
        border.width: 1
        border.color: mono.handleBorderColor
    }

    MouseArea {
        anchors.fill: parent
        enabled: mono.enabled
        preventStealing: true
        hoverEnabled: false
        acceptedButtons: Qt.LeftButton
        cursorShape: mono._handleDragging ? Qt.ClosedHandCursor : Qt.ArrowCursor

        onPressed: function (mouse) {
            mono._pointerMoved = false
            mono._pressValue = mono.value
            mono._pressLocalX = mouse.x
            mono.lockScroll(true)
            if (mono.isNearHandle(mouse.x)) {
                mono._handleDragging = true
                mono.onBegin()
            } else {
                mono._handleDragging = false
            }
            mouse.accepted = true
        }
        onPositionChanged: function (mouse) {
            if (!mono._handleDragging)
                return
            mono._pointerMoved = true
            var v = mono.valueFromDragDelta(mouse.x)
            mono.value = v
            mono.onUpdate(v)
        }
        onReleased: function (/*mouse*/) {
            mono.finishPointerPress()
        }
        onCanceled: {
            if (mono._handleDragging)
                mono.onFinish()
            mono._handleDragging = false
            mono.lockScroll(false)
        }
    }
}
