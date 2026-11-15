import QtQuick

// Shared numeric slider (#922): a recessed rail with the handle inside it, an editable value
// field, soft limits, and double-click reset.
//   - The rail covers the hard limits [from, to]. The extra zones outside the soft range
//     [softFrom, softTo] are always hatched.
//   - A drag on the rail starts at the current value (no jump); Shift drags at 1/10 speed. At a
//     soft limit the handle holds for `softDetent` px of pointer travel, then enters the extra
//     zone; the same hold applies on the way back.
//   - Typing, a horizontal drag on the value (1 px = 1 step, Shift x10), and the arrow keys move
//     through the soft limits without a hold.
//   - A click on the value edits it: Enter or focus-out commits, Esc cancels, Up/Down step.
//   - A double-click on the label or the rail emits resetRequested().
// The control never assigns `value`: the owner binds it and applies the edits from the signals.
//   dragStarted / valueEdited... / dragFinished   rail drag and value drag
//   valueEdited                                   arrow keys on the rail
//   valueEntered                                  typed value, Up/Down in the field
//   textRejected                                  typed text that is not a number
// Part object names start with `partName` (the object name unless set): <partName>Rail,
// <partName>RailInput, <partName>Handle, <partName>Value, <partName>ValueInput, ...
Item {
    id: root
    objectName: "alcedoSlider"

    property string partName: objectName
    property string label: ""
    /// Optional explanation: an "i" glyph after the label shows it.
    property string infoText: ""
    property string infoObjectName: partName + "InfoHint"
    /// "inline": label, rail, value in one row. "stacked": label and value above the rail.
    property string layout: "inline"
    property int labelWidth: 160
    property int labelPixelSize: appTheme.fontSizeCaption
    property int labelWeight: Font.Normal
    property int valuePixelSize: appTheme.fontSizeCaption
    property int valueFieldWidth: 64
    /// False hides the value field (a toolbar slider without a number).
    property bool showValue: true
    /// Row height of the inline layout.
    property int inlineHeight: 32
    property int spacing: 12

    property real value: 0
    property real from: 0
    property real to: 1
    property real softFrom: from
    property real softTo: to
    /// Pointer travel in px that a rail drag holds at a soft limit.
    property real softDetent: 10
    property real stepSize: 0.01
    property int precision: 2
    property real defaultValue: 0
    /// The fill runs from the origin to the handle.
    property real origin: from < 0 && to > 0 ? 0 : from
    property bool signedDisplay: from < 0 && to > 0
    property string suffix: ""
    /// Optional horizontal gradient drawn in the rail instead of the fill.
    property Gradient railGradient: null
    property real railGradientOpacity: 0.7
    /// Optional value text: function (value) -> text shown in the value field, function (value)
    /// -> text to edit, and function (text) -> value (NaN when the text is not a value). Used
    /// when the rail unit differs from the shown unit (Kelvin on a position rail).
    property var valueFormatter: null
    property var editFormatter: null
    property var editParser: null
    /// False shows the value in the danger color (the owner rejected the last entry).
    property bool valueValid: true
    /// Optional parent Flickable to lock while a drag runs (beginInputLock / endInputLock, or
    /// `interactive`).
    property var flickable: null

    readonly property bool modified: Math.abs(value - defaultValue) > stepSize * 0.5
    readonly property bool beyondSoftRange: value > softTo + stepSize * 0.5
                                            || value < softFrom - stepSize * 0.5
    /// True when the soft range is narrower than the hard range on that side.
    readonly property bool hasExtraLow: softFrom > from + stepSize * 0.5
    readonly property bool hasExtraHigh: softTo < to - stepSize * 0.5
    readonly property bool dragging: railInput.dragging || valueInputArea.scrubbing
    readonly property bool editing: valueInput.activeFocus

    signal dragStarted()
    signal valueEdited(real value)
    signal dragFinished()
    signal valueEntered(real value)
    signal textRejected(string text)
    signal resetRequested()

    readonly property bool _stacked: layout === "stacked"
    readonly property bool _hasLabel: label.length > 0
    readonly property bool _hasInfo: infoText.length > 0
    readonly property int _valueWidth: showValue ? valueFieldWidth : 0
    readonly property int _valueGap: showValue ? spacing : 0
    readonly property color _text: appTheme.textColor
    readonly property color _muted: appTheme.textMutedColor
    readonly property int _railHeight: 18
    readonly property int _inset: 2
    readonly property int _thumbWidth: 6
    readonly property int _valueHeight: 22
    readonly property int _stackGap: 6
    property bool _flickableLocked: false
    property bool _cancelEdit: false

    implicitWidth: 240
    opacity: enabled ? 1 : 0.45
    implicitHeight: _stacked
                    ? Math.max(labelText.implicitHeight, _valueHeight) + _stackGap + _railHeight
                    : Math.max(inlineHeight, labelText.implicitHeight, _valueHeight)

    function textAlpha(alpha) {
        return Qt.rgba(root._text.r, root._text.g, root._text.b, alpha)
    }

    function clampHard(v) { return Math.max(root.from, Math.min(root.to, v)) }

    // Rail drag with the soft-limit hold. The drag accumulates a raw value in which each extra
    // zone starts `hold` value units past its soft limit; raw values inside a hold map to the
    // soft limit.
    function rawFromValue(v, hold) {
        if (root.hasExtraHigh && v > root.softTo)
            return v + hold
        if (root.hasExtraLow && v < root.softFrom)
            return v - hold
        return v
    }
    function valueFromRaw(r, hold) {
        if (root.hasExtraHigh && r > root.softTo)
            return r <= root.softTo + hold ? root.softTo : r - hold
        if (root.hasExtraLow && r < root.softFrom)
            return r >= root.softFrom - hold ? root.softFrom : r + hold
        return r
    }

    function snap(v) {
        if (root.stepSize > 0)
            v = Math.round(v / root.stepSize) * root.stepSize
        return Number(v.toFixed(root.precision))
    }

    function formatValue(v) {
        if (typeof root.valueFormatter === "function")
            return root.valueFormatter(v)
        var text = Math.abs(v).toFixed(root.precision)
        if (v < 0 && Number(text) !== 0)
            text = "−" + text
        else if (root.signedDisplay && Number(text) > 0)
            text = "+" + text
        return text + root.suffix
    }

    function editText(v) {
        if (typeof root.editFormatter === "function")
            return root.editFormatter(v)
        return Number(v).toFixed(root.precision)
    }

    function parseText(text) {
        if (typeof root.editParser === "function")
            return root.editParser(text)
        var s = text.replace(/−/g, "-").trim()
        if (root.suffix.length > 0 && s.endsWith(root.suffix.trim()))
            s = s.substring(0, s.length - root.suffix.trim().length).trim()
        var n = Number(s)
        return s.length > 0 && isFinite(n) ? n : NaN
    }

    function lockFlickable(lock) {
        if (!root.flickable || lock === root._flickableLocked)
            return
        root._flickableLocked = lock
        if (typeof root.flickable.beginInputLock === "function"
                && typeof root.flickable.endInputLock === "function") {
            if (lock)
                root.flickable.beginInputLock()
            else
                root.flickable.endInputLock()
            return
        }
        root.flickable.interactive = !lock
    }

    function edit(v) {
        v = root.snap(root.clampHard(v))
        if (v !== root.value)
            root.valueEdited(v)
    }

    function commitTyped() {
        var n = root.parseText(valueInput.text)
        if (isNaN(n))
            root.textRejected(valueInput.text)
        else
            root.valueEntered(root.snap(root.clampHard(n)))
    }

    Text {
        id: labelText
        objectName: root.partName + "Label"
        visible: root._hasLabel
        x: 0
        y: root._stacked ? 0 : (root.height - height) / 2
        readonly property real available: (root._stacked
                                           ? Math.max(0, root.width - root._valueWidth
                                                      - root._valueGap)
                                           : root.labelWidth)
                                          - (root._hasInfo ? infoHint.width + 4 : 0)
        width: root._hasInfo ? Math.min(implicitWidth, available) : available
        text: root.label
        color: root._text
        font.family: appTheme.uiFontFamily
        font.pixelSize: root.labelPixelSize
        font.weight: root.labelWeight
        wrapMode: Text.Wrap

        MouseArea {
            anchors.fill: parent
            acceptedButtons: Qt.LeftButton
            onDoubleClicked: root.resetRequested()
        }
    }

    EditorInfoHint {
        id: infoHint
        objectName: root.infoObjectName
        visible: root._hasInfo
        x: labelText.x + labelText.width + 4
        y: labelText.y + (Math.min(labelText.height, labelText.font.pixelSize * 1.4) - height) / 2
        text: root.infoText
    }

    Rectangle {
        id: rail
        objectName: root.partName + "Rail"
        readonly property real travel: Math.max(1, width - 2 * root._inset - root._thumbWidth)

        function positionOf(v) {
            var span = root.to - root.from
            return span > 0 ? (root.clampHard(v) - root.from) / span : 0
        }
        function centerOf(v) {
            return root._inset + root._thumbWidth / 2 + positionOf(v) * travel
        }

        x: root._stacked || !root._hasLabel ? 0 : root.labelWidth + root.spacing
        y: root._stacked ? root.height - height : (root.height - height) / 2
        width: Math.max(root._thumbWidth + 2 * root._inset,
                        root.width - x - root._valueGap - root._valueWidth
                        + (root._stacked ? root._valueGap + root._valueWidth : 0))
        height: root._railHeight
        radius: 5
        activeFocusOnTab: true
        border.width: 1
        border.color: root.dragging ? root.textAlpha(0.18)
                                    : (railInput.containsMouse ? root.textAlpha(0.13)
                                                               : root.textAlpha(0.07))
        gradient: Gradient {
            GradientStop { position: 0.0; color: Qt.darker(appTheme.cardSurfaceColor, 2.4) }
            GradientStop { position: 0.35; color: Qt.darker(appTheme.cardSurfaceColor, 1.7) }
        }
        Accessible.role: Accessible.Slider
        Accessible.name: root.label

        Keys.onPressed: function (event) {
            var direction = 0
            if (event.key === Qt.Key_Left || event.key === Qt.Key_Down)
                direction = -1
            else if (event.key === Qt.Key_Right || event.key === Qt.Key_Up)
                direction = 1
            if (direction === 0)
                return
            var factor = (event.modifiers & Qt.ShiftModifier) ? 10 : 1
            root.edit(root.value + direction * factor * root.stepSize)
            event.accepted = true
        }

        Rectangle {
            visible: root.railGradient !== null
            anchors.fill: parent
            anchors.margins: 1
            radius: parent.radius - 1
            opacity: root.railGradientOpacity
            gradient: root.railGradient
        }

        // Extra zones outside the soft range: hatched from the soft limit to the rail end.
        Repeater {
            model: [
                { name: "ExtraLow", shown: root.hasExtraLow,
                  x0: 1, x1: rail.centerOf(root.softFrom) },
                { name: "ExtraHigh", shown: root.hasExtraHigh,
                  x0: rail.centerOf(root.softTo), x1: rail.width - 1 }
            ]
            delegate: Item {
                id: zone
                required property var modelData
                objectName: root.partName + modelData.name
                visible: modelData.shown
                x: Math.round(modelData.x0)
                y: 1
                width: Math.max(0, Math.round(modelData.x1) - x)
                height: rail.height - 2
                clip: true

                Rectangle {
                    anchors.fill: parent
                    color: Qt.rgba(0, 0, 0, 0.18)
                }
                Repeater {
                    model: Math.ceil((zone.width + zone.height) / 5) + 1
                    Rectangle {
                        required property int index
                        x: index * 5 - zone.height
                        y: -6
                        width: 1
                        height: zone.height * 1.5 + 12
                        rotation: 45
                        transformOrigin: Item.Center
                        color: root.textAlpha(0.10)
                    }
                }
                // Soft limit.
                Rectangle {
                    x: zone.modelData.name === "ExtraLow" ? zone.width - 1 : 0
                    width: 1
                    height: zone.height
                    color: root.textAlpha(0.16)
                }
            }
        }

        // App default.
        Rectangle {
            objectName: root.partName + "DefaultMark"
            visible: root.defaultValue >= root.from && root.defaultValue <= root.to
            x: Math.round(rail.centerOf(root.defaultValue))
            y: 1
            width: 1
            height: parent.height - 2
            color: root.textAlpha(0.22)
        }

        Rectangle {
            id: fill
            objectName: root.partName + "Fill"
            readonly property real handleCenter: rail.centerOf(root.value)
            readonly property real originCenter: rail.centerOf(root.origin)
            visible: root.railGradient === null
            x: Math.min(handleCenter, originCenter)
            y: root._inset
            width: Math.abs(handleCenter - originCenter)
            height: parent.height - 2 * root._inset
            radius: 2
            color: root.textAlpha(0.15)
        }

        Rectangle {
            id: thumb
            objectName: root.partName + "Handle"
            x: rail.centerOf(root.value) - width / 2
            y: root._inset
            width: root._thumbWidth
            height: parent.height - 2 * root._inset
            radius: 2
            color: railInput.containsMouse || root.dragging ? "#ffffff" : Qt.darker(root._text, 1.05)
            border.width: 1
            border.color: Qt.rgba(0, 0, 0, 0.55)

            Behavior on x {
                enabled: !root.dragging && !appTheme.reduceMotion
                NumberAnimation { duration: 120; easing.type: Easing.OutCubic }
            }
        }

        // Keyboard focus ring.
        Rectangle {
            visible: rail.activeFocus
                     && (rail.focusReason === Qt.TabFocusReason
                         || rail.focusReason === Qt.BacktabFocusReason)
            anchors.fill: parent
            anchors.margins: -3
            radius: parent.radius + 3
            color: "transparent"
            border.width: 1
            border.color: root._muted
        }

        MouseArea {
            id: railInput
            objectName: root.partName + "RailInput"
            property bool dragging: false
            property real pressX: 0
            property real lastX: 0
            property real accumulated: 0
            property real hold: 0

            anchors.fill: parent
            hoverEnabled: true
            preventStealing: true
            acceptedButtons: Qt.LeftButton
            cursorShape: Qt.SizeHorCursor

            onPressed: function (mouse) {
                rail.forceActiveFocus(Qt.MouseFocusReason)
                pressX = mouse.x
                lastX = mouse.x
                hold = root.softDetent / rail.travel * (root.to - root.from)
                accumulated = root.rawFromValue(root.clampHard(root.value), hold)
                root.lockFlickable(true)
            }
            onPositionChanged: function (mouse) {
                if (!pressed)
                    return
                if (!dragging) {
                    if (Math.abs(mouse.x - pressX) < 2)
                        return
                    dragging = true
                    root.dragStarted()
                }
                var gain = (mouse.modifiers & Qt.ShiftModifier) ? 0.1 : 1
                var low = root.from - (root.hasExtraLow ? hold : 0)
                var high = root.to + (root.hasExtraHigh ? hold : 0)
                accumulated = Math.max(low, Math.min(high, accumulated + (mouse.x - lastX)
                                                     / rail.travel * (root.to - root.from) * gain))
                lastX = mouse.x
                root.edit(root.valueFromRaw(accumulated, hold))
            }
            onReleased: finish()
            onCanceled: finish()
            onDoubleClicked: root.resetRequested()

            function finish() {
                if (dragging) {
                    dragging = false
                    root.dragFinished()
                }
                root.lockFlickable(false)
            }
        }
    }

    Rectangle {
        id: valueBox
        objectName: root.partName + "Value"
        visible: root.showValue
        x: root.width - width
        y: root._stacked ? Math.max(0, (Math.max(labelText.implicitHeight, root._valueHeight) - height) / 2)
                         : (root.height - height) / 2
        width: root.valueFieldWidth
        height: root._valueHeight
        radius: appTheme.controlRadiusSmall
        color: root.editing || valueInputArea.containsMouse || valueInputArea.scrubbing
               ? appTheme.bgBaseColor : "transparent"
        border.width: 1
        border.color: root.editing ? root._muted
                                   : (valueInputArea.containsMouse || valueInputArea.scrubbing
                                      ? root.textAlpha(0.14) : "transparent")

        Text {
            objectName: root.partName + "ValueText"
            visible: !root.editing
            anchors.fill: parent
            anchors.leftMargin: 6
            anchors.rightMargin: 6
            text: root.formatValue(root.value)
            color: !root.valueValid ? appTheme.dangerColor
                                    : (root.modified ? root._text : root._muted)
            font.family: appTheme.dataFontFamily
            font.pixelSize: root.valuePixelSize
            font.features: { "tnum": 1 }
            horizontalAlignment: Text.AlignRight
            verticalAlignment: Text.AlignVCenter
            elide: Text.ElideLeft
        }

        TextInput {
            id: valueInput
            objectName: root.partName + "ValueInput"
            visible: root.editing
            anchors.fill: parent
            anchors.leftMargin: 6
            anchors.rightMargin: 6
            color: root._text
            selectionColor: Qt.rgba(root._muted.r, root._muted.g, root._muted.b, 0.6)
            selectedTextColor: root._text
            font.family: appTheme.dataFontFamily
            font.pixelSize: root.valuePixelSize
            font.features: { "tnum": 1 }
            horizontalAlignment: TextInput.AlignRight
            verticalAlignment: TextInput.AlignVCenter
            selectByMouse: true
            clip: true
            activeFocusOnTab: true

            onActiveFocusChanged: {
                if (activeFocus) {
                    root._cancelEdit = false
                    text = root.editText(root.value)
                    selectAll()
                } else if (!root._cancelEdit) {
                    root.commitTyped()
                }
            }
            onAccepted: rail.forceActiveFocus(Qt.OtherFocusReason)
            Keys.onEscapePressed: function (event) {
                root._cancelEdit = true
                rail.forceActiveFocus(Qt.OtherFocusReason)
                event.accepted = true
            }
            Keys.onUpPressed: function (event) { step(event, 1) }
            Keys.onDownPressed: function (event) { step(event, -1) }

            function step(event, direction) {
                var factor = (event.modifiers & Qt.ShiftModifier) ? 10 : 1
                var base = root.parseText(text)
                if (isNaN(base))
                    base = root.value
                var v = root.snap(root.clampHard(base + direction * factor * root.stepSize))
                root.valueEntered(v)
                text = root.editText(v)
                selectAll()
                event.accepted = true
            }
        }

        // Drag scrubs the value; a click without movement starts editing.
        MouseArea {
            id: valueInputArea
            objectName: root.partName + "ValueInputArea"
            property bool scrubbing: false
            property real pressX: 0
            property real startValue: 0

            anchors.fill: parent
            visible: !root.editing
            hoverEnabled: true
            preventStealing: true
            acceptedButtons: Qt.LeftButton
            cursorShape: Qt.SizeHorCursor

            onPressed: function (mouse) {
                pressX = mouse.x
                startValue = root.value
                root.lockFlickable(true)
            }
            onPositionChanged: function (mouse) {
                if (!pressed)
                    return
                var dx = mouse.x - pressX
                if (!scrubbing) {
                    if (Math.abs(dx) < 3)
                        return
                    scrubbing = true
                    root.dragStarted()
                }
                var factor = (mouse.modifiers & Qt.ShiftModifier) ? 10 : 1
                root.edit(startValue + Math.round(dx) * root.stepSize * factor)
            }
            onReleased: {
                root.lockFlickable(false)
                if (scrubbing) {
                    scrubbing = false
                    root.dragFinished()
                } else {
                    valueInput.forceActiveFocus(Qt.MouseFocusReason)
                }
            }
            onCanceled: {
                root.lockFlickable(false)
                if (scrubbing) {
                    scrubbing = false
                    root.dragFinished()
                }
            }
        }
    }
}
