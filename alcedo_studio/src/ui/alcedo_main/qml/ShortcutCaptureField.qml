pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import Alcedo.Main 1.0

// Settings > Keyboard capture cell. Click or press Enter/Space to start
// capture, then type the new input: a key chord for chord commands, or press
// and release one modifier for modifier commands. Enter and Escape commit,
// plain Delete selects the unassigned state, Tab cancels and moves focus.
// ShortcutRegistry owns validation, persistence, conflict detection, and the
// effective binding — this control only forwards key events and renders the
// capture session.
Item {
    id: field

    // Row data, bound by the KeyboardSettingsPanel delegate.
    property string commandId: ""
    property int inputKind: 0
    property string bindingText: ""
    property string descriptionText: ""
    property bool assigned: true
    property bool usesDefault: true
    // KeyboardSettingsPanel enforces one active capture across all rows.
    property var captureOwner: null

    // Theme mirrors — SettingDialog passes its palette; appTheme is the default.
    property color textColor: appTheme.textColor
    property color mutedTextColor: appTheme.textMutedColor
    property color accentColor: appTheme.accentColor
    property color fillColor: appTheme.bgBaseColor
    property color borderColor: appTheme.cardBorderColor
    property color dangerColor: appTheme.dangerColor
    property string dataFontFamily: appTheme.dataFontFamily

    // Capture session state (private; exposed read-only for tests and a11y).
    property bool _capturing: false
    property int _pendingKey: 0
    property int _pendingModifiers: 0
    property bool _pendingUnassigned: false
    property int _heldModifiers: 0
    property string _errorCode: ""
    property string _errorText: ""
    property string _conflictScope: ""
    // Re-reads decorateTooltip() after a row commit lands in the registry.
    property int _bindingStamp: 0

    readonly property bool capturing: _capturing
    readonly property string errorText: _errorText
    readonly property string conflictScope: _conflictScope
    // Plain-property mirrors of the attached a11y strings so tests and probes
    // can read them without resolving attached objects.
    readonly property string accessibleName: Accessible.name
    readonly property string accessibleDescription: Accessible.description
    // idle | unassigned | capturing | candidate | conflict | persistenceError
    readonly property string captureState: {
        if (_errorCode === "persistence" && _errorText.length > 0) {
            return "persistenceError"
        }
        if (_capturing) {
            if (_errorText.length > 0) {
                return _errorCode === "conflict" ? "conflict" : "capturing"
            }
            if (_pendingUnassigned || _pendingKey !== 0 || _pendingModifiers !== 0) {
                return "candidate"
            }
            return "capturing"
        }
        return assigned ? "idle" : "unassigned"
    }
    // A committable candidate exists: a chord key, a released modifier bit, or
    // the unassigned marker. Invalid attempts keep the text visible but do not
    // count as candidates.
    readonly property bool _hasCandidate:
        _pendingUnassigned || _pendingKey !== 0 || _pendingModifiers !== 0
    readonly property string displayText: {
        if (!_capturing) {
            return bindingText
        }
        if (_pendingUnassigned) {
            return qsTr("Unassigned")
        }
        if (_pendingKey !== 0) {
            return ShortcutRegistry.candidateText(_pendingKey, _pendingModifiers, 0)
        }
        if (_pendingModifiers !== 0) {
            return ShortcutRegistry.candidateText(0, _pendingModifiers, 1)
        }
        const held = ShortcutRegistry.candidateText(0, _heldModifiers, 0)
        if (held.length > 0) {
            return held
        }
        return inputKind === 1 ? qsTr("Press and release a modifier")
                               : qsTr("Press shortcut")
    }

    function beginCapture() {
        if (_capturing) {
            return
        }
        if (captureOwner) {
            captureOwner.requestCapture(field)
        }
        forceActiveFocus(Qt.OtherFocusReason)
        _capturing = true
        _pendingKey = 0
        _pendingModifiers = 0
        _pendingUnassigned = false
        _heldModifiers = 0
        _errorCode = ""
        _errorText = ""
        _conflictScope = ""
    }

    // Ends the session but keeps the last result visible (persistence errors
    // stay on the row until the next interaction).
    function endCapture() {
        if (!_capturing) {
            return
        }
        _capturing = false
        _pendingKey = 0
        _pendingModifiers = 0
        _pendingUnassigned = false
        _heldModifiers = 0
        if (captureOwner) {
            captureOwner.releaseCapture(field)
        }
    }

    function cancelCapture() {
        _errorCode = ""
        _errorText = ""
        _conflictScope = ""
        endCapture()
    }

    function commitCandidate() {
        if (_pendingUnassigned) {
            applyResult(ShortcutRegistry.clearBinding(commandId))
            return
        }
        if (_hasCandidate) {
            applyResult(ShortcutRegistry.saveCandidate(commandId, _pendingKey, _pendingModifiers,
                                                       _pendingKey !== 0 ? 0 : 1))
            return
        }
        cancelCapture()
    }

    function applyResult(result) {
        if (result.succeeded) {
            cancelCapture()
            return
        }
        _errorCode = String(result.errorCode || "")
        _errorText = String(result.message || "")
        _conflictScope =
                _errorCode === "conflict" && result.conflictingCommandId !== undefined
                ? ShortcutRegistry.scopeForCommand(String(result.conflictingCommandId))
                : ""
        if (_errorCode === "persistence") {
            // The effective binding did not change; end the session and show
            // the write failure without touching the displayed value.
            endCapture()
        }
        // Validation failures keep the session active so the user can retry.
    }

    function clearBinding() {
        applyResult(ShortcutRegistry.clearBinding(commandId))
    }

    function restoreDefault() {
        applyResult(ShortcutRegistry.restoreDefault(commandId))
    }

    function _modifierBitForKey(key) {
        switch (key) {
        case Qt.Key_Shift: return Qt.ShiftModifier
        case Qt.Key_Control: return Qt.ControlModifier
        case Qt.Key_Alt: return Qt.AltModifier
        case Qt.Key_Meta: return Qt.MetaModifier
        default: return 0
        }
    }

    function _isModifierKey(key) {
        return _modifierBitForKey(key) !== 0 || key === Qt.Key_AltGr
    }

    implicitWidth: 190
    implicitHeight: 36
    activeFocusOnTab: true

    Accessible.role: Accessible.Button
    Accessible.name: descriptionText.length > 0
                     ? qsTr("%1 shortcut").arg(descriptionText)
                     : qsTr("Shortcut")
    Accessible.description: {
        if (_capturing) {
            return inputKind === 1
                    ? qsTr("Recording. Press and release Shift, Ctrl, Alt, or Meta to choose it. Enter or Escape saves, Tab cancels, plain Delete clears the binding.")
                    : qsTr("Recording. Type the new keys. Enter or Escape saves, Tab cancels, plain Delete clears the binding.")
        }
        return qsTr("Current shortcut: %1. Press Enter or click to record a new shortcut.")
                .arg(bindingText)
    }
    Accessible.onPressAction: beginCapture()

    // While capturing, no Shortcut (dialog Escape close or a product command)
    // may consume the key. Tab stays unhandled so focus keeps moving.
    Keys.onShortcutOverride: function (event) {
        if (_capturing && event.key !== Qt.Key_Tab && event.key !== Qt.Key_Backtab) {
            event.accepted = true
        }
    }

    Keys.onPressed: function (event) {
        if (!_capturing) {
            if (event.key === Qt.Key_Return || event.key === Qt.Key_Enter
                    || event.key === Qt.Key_Space) {
                beginCapture()
                event.accepted = true
            }
            return
        }
        if (event.isAutoRepeat) {
            event.accepted = true
            return
        }
        if (event.key === Qt.Key_Return || event.key === Qt.Key_Enter
                || event.key === Qt.Key_Escape) {
            commitCandidate()
            event.accepted = true
            return
        }
        // Tab cancels the unsaved candidate and stays unhandled so focus keeps
        // moving down the row's tab order.
        if (event.key === Qt.Key_Tab || event.key === Qt.Key_Backtab) {
            cancelCapture()
            return
        }
        if (field._isModifierKey(event.key)) {
            _heldModifiers |= _modifierBitForKey(event.key)
            event.accepted = true
            return
        }
        if (event.key === Qt.Key_Delete
                && _maskedModifiers(event.modifiers) === 0) {
            _pendingKey = 0
            _pendingModifiers = 0
            _pendingUnassigned = true
            _errorCode = ""
            _errorText = ""
            _conflictScope = ""
            event.accepted = true
            return
        }
        _pendingUnassigned = false
        _pendingKey = event.key
        _pendingModifiers = _maskedModifiers(event.modifiers)
        const verdict = ShortcutRegistry.validateCandidate(commandId, _pendingKey,
                                                           _pendingModifiers, 0)
        if (verdict.succeeded) {
            _errorCode = ""
            _errorText = ""
            _conflictScope = ""
        } else {
            _errorCode = String(verdict.errorCode || "")
            _errorText = String(verdict.message || "")
            _conflictScope =
                    _errorCode === "conflict" && verdict.conflictingCommandId !== undefined
                    ? ShortcutRegistry.scopeForCommand(String(verdict.conflictingCommandId))
                    : ""
        }
        event.accepted = true
    }

    Keys.onReleased: function (event) {
        if (!_capturing) {
            return
        }
        const bit = _modifierBitForKey(event.key)
        if (bit === 0) {
            // Non-modifier releases never leave the field while capturing.
            event.accepted = true
            return
        }
        _heldModifiers &= ~bit
        if (inputKind === 1) {
            // Modifier commands commit the released modifier as the candidate.
            _pendingUnassigned = false
            _pendingKey = 0
            _pendingModifiers = bit
            const verdict = ShortcutRegistry.validateCandidate(commandId, 0, bit, 1)
            if (verdict.succeeded) {
                _errorCode = ""
                _errorText = ""
                _conflictScope = ""
            } else {
                _errorCode = String(verdict.errorCode || "")
                _errorText = String(verdict.message || "")
                _conflictScope =
                        _errorCode === "conflict" && verdict.conflictingCommandId !== undefined
                        ? ShortcutRegistry.scopeForCommand(String(verdict.conflictingCommandId))
                        : ""
            }
        }
        event.accepted = true
    }

    function _maskedModifiers(modifiers) {
        return modifiers & (Qt.ShiftModifier | Qt.ControlModifier
                            | Qt.AltModifier | Qt.MetaModifier)
    }

    // Losing focus ends the capture and drops any unsaved candidate (Tab).
    onActiveFocusChanged: {
        if (!activeFocus && _capturing) {
            cancelCapture()
        }
    }

    Rectangle {
        id: box
        objectName: field.commandId.length > 0 ? "shortcutCaptureBox:" + field.commandId : ""
        anchors.fill: parent
        radius: appTheme.controlRadiusSmall
        color: field.fillColor
        border.width: 1
        border.color: {
            const state = field.captureState
            if (state === "conflict" || state === "persistenceError") {
                return field.dangerColor
            }
            if (field.activeFocus || field._capturing) {
                return Qt.rgba(field.accentColor.r, field.accentColor.g,
                               field.accentColor.b, 0.60)
            }
            return field.borderColor
        }

        Label {
            anchors.fill: parent
            anchors.leftMargin: appTheme.spaceSm
            anchors.rightMargin: appTheme.spaceSm
            verticalAlignment: Text.AlignVCenter
            horizontalAlignment: Text.AlignHCenter
            elide: Text.ElideRight
            text: field.displayText
            color: {
                if (!field._capturing) {
                    return field.assigned ? field.textColor : field.mutedTextColor
                }
                return field._hasCandidate ? field.textColor : field.mutedTextColor
            }
            font.family: field.dataFontFamily
            font.pixelSize: appTheme.fontSizeBody
        }

        MouseArea {
            id: pressArea
            anchors.fill: parent
            hoverEnabled: true
            cursorShape: Qt.PointingHandCursor
            onClicked: field.beginCapture()
        }
    }

    Connections {
        target: ShortcutRegistry
        function onCommandBindingChanged(changedId) {
            if (changedId === field.commandId) {
                field._bindingStamp += 1
            }
        }
    }

    ToolTip.visible: pressArea.containsMouse && ToolTip.text.length > 0
    ToolTip.text: {
        const stamp = field._bindingStamp
        return ShortcutRegistry.decorateTooltip(field.descriptionText, field.commandId)
    }
    ToolTip.delay: 400
}
