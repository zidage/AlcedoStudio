import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Controls.impl
import QtQuick.Layouts

// Button row under the Library inspector (#731).
//
// Album and Image tabs: "Add to Export Queue (N)" adds the selected images to the export queue,
// clears the selection, and opens the Export tab, where the user changes the settings.
// Export tab: only "Export N Files", at the full row width. More images are added there with the
// + button of the Export Queue section.
RowLayout {
    id: root
    objectName: "libraryInspectorActions"

    // The InspectorPanel that this row acts on: its tab, export settings, and export action.
    property var inspectorPanel: null
    // Album/image selection (SelectionState.qml).
    property var selectionState: null
    property int selectedCount: 0
    property bool backendInteractive: false
    property int exportQueueCount: 0
    // Colour helpers of LibraryWorkspace; the appTheme values apply when it is not set.
    property var theme: null
    // Height of the buttons; LibraryWorkspace gives the row this height.
    readonly property int buttonHeight: 44

    readonly property bool exportPageActive: inspectorPanel ? inspectorPanel.exportPageActive
                                                            : false
    readonly property bool addEnabled: backendInteractive && selectedCount > 0
    readonly property bool exportEnabled: backendInteractive
                                          && appModules.interactionPolicy.canChangeStateAsUser
                                          && !appModules.importExport.exportInFlight
                                          && exportQueueCount > 0
                                          && inspectorPanel !== null
                                          && inspectorPanel.exportSettingsValid

    readonly property color textColor: theme ? theme.colText : appTheme.textColor
    readonly property color secondaryBorderColor: theme ? theme.colButtonSecondaryBorder
                                                        : appTheme.dividerColor
    readonly property int controlRadius: theme ? theme.controlRadius : appTheme.controlRadius

    function secondaryButtonFill(enabled, hovered, pressed) {
        return theme ? theme.secondaryButtonFill(enabled, hovered, pressed)
                     : appTheme.bgBaseColor
    }

    function filledActionFill(enabled, hovered, pressed) {
        return theme ? theme.filledActionFill(enabled, hovered, pressed)
                     : appTheme.editorListSelectedFillColor
    }

    function filledActionInk(enabled) {
        return theme ? theme.filledActionInk(enabled) : appTheme.editorListSelectedInkColor
    }

    function addSelectedToExportQueue() {
        if (!addEnabled || !selectionState)
            return
        appModules.exportQueue.AddTargets(selectionState.currentSelectedItems())
        selectionState.clearSelectedImages()
        if (inspectorPanel)
            inspectorPanel.openExportPage()
    }

    function startExport() {
        if (!exportEnabled)
            return
        inspectorPanel.startExport()
    }

    spacing: 10

    Button {
        id: addSelectedBtn
        objectName: "libraryInspectorAddToExportQueueButton"
        Layout.fillWidth: true
        Layout.preferredHeight: root.buttonHeight
        visible: !root.exportPageActive
        text: qsTr("Add to Export Queue (%1)").arg(root.selectedCount)
        enabled: root.addEnabled
        icon.source: "qrc:/panel_icons/queue-add.svg"
        icon.width: 16
        icon.height: 16
        icon.color: root.textColor
        display: AbstractButton.TextBesideIcon
        background: Rectangle {
            radius: root.controlRadius
            color: root.secondaryButtonFill(addSelectedBtn.enabled,
                                            addSelectedBtn.hovered,
                                            addSelectedBtn.down)
            border.width: 1
            border.color: root.secondaryBorderColor
            Behavior on color {
                enabled: !appTheme.reduceMotion
                ColorAnimation { duration: 120; easing.type: Easing.OutCubic }
            }
        }
        Material.foreground: root.textColor
        onClicked: root.addSelectedToExportQueue()
    }

    Button {
        id: exportQueueBtn
        objectName: "libraryInspectorExportButton"
        Layout.fillWidth: true
        Layout.preferredHeight: root.buttonHeight
        visible: root.exportPageActive
        text: appModules.importExport.exportInFlight
              ? qsTr("Exporting...")
              : root.exportQueueCount === 1
                ? qsTr("Export 1 File")
                : qsTr("Export %1 Files").arg(root.exportQueueCount)
        enabled: root.exportEnabled
        icon.source: "qrc:/panel_icons/export.svg"
        icon.width: 16
        icon.height: 16
        icon.color: root.filledActionInk(exportQueueBtn.enabled)
        display: AbstractButton.TextBesideIcon
        background: Rectangle {
            radius: 8
            color: root.filledActionFill(exportQueueBtn.enabled,
                                         exportQueueBtn.hovered,
                                         exportQueueBtn.down)
            Behavior on color {
                enabled: !appTheme.reduceMotion
                ColorAnimation { duration: 120; easing.type: Easing.OutCubic }
            }
        }
        contentItem: IconLabel {
            spacing: exportQueueBtn.spacing
            mirrored: exportQueueBtn.mirrored
            display: exportQueueBtn.display
            icon: exportQueueBtn.icon
            text: exportQueueBtn.text
            font: exportQueueBtn.font
            color: root.filledActionInk(exportQueueBtn.enabled)
        }
        onClicked: root.startExport()
    }
}
