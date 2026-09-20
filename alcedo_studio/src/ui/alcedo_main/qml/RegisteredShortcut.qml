import QtQuick
import Alcedo.Main 1.0

// Window-context Shortcut bound to a ShortcutRegistry command. The registry
// owns the sequence list, scope, and repeat policy; the caller owns the action.
//
// - commandId: registry command id (e.g. "library.saveProject").
// - activeScope: the input scope that currently owns dispatch for this surface
//   (e.g. "workspace.library"). The shortcut stays inert until activeScope
//   equals the command's registered scope.
// - commandEnabled: caller-side action gate (availability, dialog state).
// - suppressWhileEditing: editable text controls keep native key behavior
//   (select all, undo, arrows, delete) while they own focus.
Item {
    id: root

    property string commandId: ""
    property string activeScope: ""
    property bool commandEnabled: true
    property bool suppressWhileEditing: true
    // Bumped on commandBindingChanged so the sequence/repeat bindings re-read
    // the registry without an imperative reassign.
    property int _bindingStamp: 0

    signal activated()

    readonly property string commandScope: commandId.length > 0
                                           ? ShortcutRegistry.scopeForCommand(commandId)
                                           : ""
    // Introspection of the resolved inner Shortcut for tests and tooltips.
    readonly property alias shortcutEnabled: innerShortcut.enabled
    readonly property alias sequences: innerShortcut.sequences
    readonly property alias autoRepeat: innerShortcut.autoRepeat

    // Live read of the owning window's active focus item via the Window
    // attached property — never a cached focus flag.
    function editableFocusActive() {
        var item = root.Window.activeFocusItem
        while (item) {
            // TextInput/TextField/TextEdit/TextArea (and editable spin/combo
            // content items) all expose insert/selectAll and readOnly.
            if (typeof item.insert === "function" && typeof item.selectAll === "function"
                    && item.readOnly !== undefined) {
                return true
            }
            item = item.parent
        }
        return false
    }

    Shortcut {
        id: innerShortcut
        sequences: {
            var stamp = root._bindingStamp
            return root.commandId.length > 0
                    ? ShortcutRegistry.keySequenceTexts(root.commandId) : []
        }
        autoRepeat: {
            var stamp = root._bindingStamp
            return root.commandId.length > 0
                    && ShortcutRegistry.commandAutoRepeat(root.commandId)
        }
        enabled: root.commandEnabled && root.commandScope.length > 0
                 && root.activeScope === root.commandScope
                 && !(root.suppressWhileEditing && root.editableFocusActive())
        onActivated: root.activated()
    }

    Connections {
        target: ShortcutRegistry
        function onCommandBindingChanged(changedId) {
            if (changedId === root.commandId)
                root._bindingStamp += 1
        }
    }
}
