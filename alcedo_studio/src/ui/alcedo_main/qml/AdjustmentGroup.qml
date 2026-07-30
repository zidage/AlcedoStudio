import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Collapsible adjustment group (Phase 6A). A thin adjustment-specific wrapper
// around the shared CollapsibleSection: it forwards the fold contract (title,
// expanded, controlsEnabled, driveFoldProgress) and adds an optional group-
// reset affordance in the header that resets every model in `models`. Surface
// colors default to the editor card family via appTheme (DESIGN.md). Panel
// content is injected through the default property alias into the section body.
Item {
    id: root
    objectName: "adjustmentGroup"

    property string title: ""
    property bool expanded: true
    property bool controlsEnabled: true
    /// Optional list of EditorAdjustment*Model instances; the group-reset button
    /// calls reset() on each. When empty, no group-reset button is shown.
    property var models: []

    signal toggled(bool expanded)

    // Forward content into the CollapsibleSection body.
    default property alias contentData: section.contentData

    implicitHeight: section.implicitHeight
    Layout.fillWidth: true

    CollapsibleSection {
        id: section
        anchors.fill: parent
        title: root.title
        expanded: root.expanded
        controlsEnabled: root.controlsEnabled
        onToggled: function (exp) {
            root.toggled(exp)
        }
    }

    // Group-reset affordance overlaid at the top-right of the header. Resets
    // every model in `models` (one settled transaction each).
    IconActionButton {
        id: groupReset
        objectName: "adjustmentGroupReset"
        visible: root.models && root.models.length > 0 && root.controlsEnabled
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.rightMargin: appTheme.spaceXs
        anchors.topMargin: 0
        compact: true
        iconSrc: "qrc:/panel_icons/reset.svg"
        actionName: root.title.length > 0 ? qsTr("Reset %1").arg(root.title) : qsTr("Reset group")
        onClicked: {
            if (!root.models) {
                return
            }
            for (var i = 0; i < root.models.length; ++i) {
                if (root.models[i]) {
                    root.models[i].reset()
                }
            }
        }
    }
}