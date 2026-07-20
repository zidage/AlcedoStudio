import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Per-field reset affordance for adjustment controls (Phase 6A). Calls
// `model.reset()`, which restores the default value and commits one settled
// transaction. Wraps the shared IconActionButton so geometry, focus, and a11y
// match the rest of the editor. Uses the reset.svg asset.
Item {
    id: root
    objectName: "adjustmentResetButton"

    // An EditorAdjustment*Model (value/enum/toggle). Reset is a no-op when null.
    property var model: null

    signal clicked()

    implicitWidth: appTheme.iconButtonHitSizeCompact
    implicitHeight: appTheme.iconButtonHitSizeCompact
    Layout.preferredWidth: appTheme.iconButtonHitSizeCompact
    Layout.preferredHeight: appTheme.iconButtonHitSizeCompact

    IconActionButton {
        anchors.centerIn: parent
        compact: true
        enabled: root.model ? root.model.enabled : false
        iconSrc: "qrc:/panel_icons/reset.svg"
        actionName: root.model && root.model.label && root.model.label.length > 0
                    ? qsTr("Reset %1").arg(root.model.label)
                    : qsTr("Reset")
        onClicked: {
            if (root.model) {
                root.model.reset()
            }
            root.clicked()
        }
    }
}