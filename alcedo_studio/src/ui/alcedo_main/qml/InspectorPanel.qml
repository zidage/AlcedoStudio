import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Inspector shell (Frontend 3). A low-emphasis horizontal tab strip at the top
// switches the page stack between Album, Image, and Export — modeled on VSCode's
// secondary-sidebar / editor-tab selection: all-caps text labels, equal weight,
// evenly split across the full width (extensible — add a tab and the strip
// redistributes), no icons, no card, no filled background, no rounded
// rectangle. The active tab is white text with a refined text-width underline;
// inactive tabs are muted gray. The whole content tree below swaps with the
// tab (Album stats vs. Image tiles vs. Export settings).
//
// `focusedImage` is the compact focused-image inspection DTO supplied by Main.qml.
// The shell owns only navigation; ImageInspectorPanel / ExportInspectorPanel own
// page layout and edit state.
//
// Top inset: the shell is placed with a 10px outer margin in Main.qml; the tab
// strip's topMargin of 4 lands it at 14px from the panel-card top — matching
// the CollectionsPanel search row so the two side panels read as a symmetric
// pair, with the center viewport header sitting slightly higher.
Item {
    id: root

    property var focusedImage: ({})
    // Phase 2: the interaction-policy controller (forwarded to ImageInspectorPanel
    // so its edit controls can bind enabled to the focused-image policy gates).
    property var interactionPolicy: null
    property var exportQueueState: null
    property var selectionState: null
    property int selectedCount: 0
    property int currentPage: 0  // 0 = Album, 1 = Image, 2 = Export
    signal ratingRequested(int rating)
    signal descriptionSaveRequested(string caption)
    signal ratingReasonSaveRequested(string reasons)
    signal contextMenuRequested(var item, real sceneX, real sceneY)

    readonly property color textColor: appTheme.textColor
    readonly property color mutedTextColor: appTheme.textMutedColor
    readonly property color dividerColor: appTheme.dividerColor

    function withAlpha(color, alpha) {
        return Qt.rgba(color.r, color.g, color.b, alpha)
    }

    function openExportPage() {
        currentPage = 2
    }

    function startExport() {
        if (exportInspectorPage)
            exportInspectorPage.startExport()
    }

    readonly property bool exportPageActive: currentPage === 2
    readonly property bool exportBusy: exportInspectorPage ? exportInspectorPage.exportBusy : false
    readonly property int exportQueueCount: exportInspectorPage
                                            ? exportInspectorPage.exportQueueCount
                                            : (exportQueueState ? exportQueueState.exportQueueCount : 0)
    readonly property bool exportSettingsValid: exportInspectorPage
                                                 ? exportInspectorPage.settingsValid : true

    // Nested inline component (matches the Main.qml CaptionButton pattern;
    // file-level inline components are rejected by this qmlcachegen).
    // Low-emphasis tab: text only, no background/border. Active = white text +
    // underline; inactive = muted text, brightens on hover. Fills its share of
    // the strip width so N tabs redistribute evenly (extensible navbar).
    component InspectorTab: Item {
        id: tab
        property string label: ""
        property bool active: false
        signal clicked()

        implicitWidth: tabLabel.implicitWidth + 28  // minimum; fillWidth stretches it
        implicitHeight: 36

        Label {
            id: tabLabel
            anchors.centerIn: parent
            text: tab.label
            color: tab.active
                   ? appTheme.textColor
                   : (tabMouseArea.containsMouse ? appTheme.textColor : appTheme.textMutedColor)
            font.family: appTheme.uiFontFamily
            font.pixelSize: 11
            font.weight: tab.active ? 700 : 600
            font.letterSpacing: 1.4
            font.capitalization: Font.AllUppercase
        }

        // Underline beneath the active tab — text-width (capped at tab width),
        // centered, project white. Refined rather than a heavy full-bleed bar.
        Rectangle {
            anchors.bottom: parent.bottom
            anchors.bottomMargin: 2
            anchors.horizontalCenter: parent.horizontalCenter
            width: Math.min(parent.width, tabLabel.implicitWidth + 24)
            height: 2
            color: appTheme.textColor
            visible: tab.active
        }

        MouseArea {
            id: tabMouseArea
            anchors.fill: parent
            cursorShape: Qt.PointingHandCursor
            hoverEnabled: true
            onClicked: tab.clicked()
        }
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        // ── Horizontal tab strip: tabs evenly split the full width ──
        RowLayout {
            Layout.fillWidth: true
            Layout.leftMargin: 16
            Layout.rightMargin: 16
            Layout.topMargin: 4
            Layout.bottomMargin: 4
            spacing: 0

            InspectorTab {
                Layout.fillWidth: true
                label: qsTr("Album")
                active: root.currentPage === 0
                onClicked: root.currentPage = 0
            }

            InspectorTab {
                Layout.fillWidth: true
                label: qsTr("Image")
                active: root.currentPage === 1
                onClicked: root.currentPage = 1
            }

            InspectorTab {
                Layout.fillWidth: true
                label: qsTr("Export")
                active: root.currentPage === 2
                onClicked: root.currentPage = 2
            }
        }

        // ── Page stack: the whole content tree swaps with the tab ──
        StackLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            currentIndex: root.currentPage

            AlbumInspectorPanel {
                Layout.fillWidth: true
                Layout.fillHeight: true
            }

            ImageInspectorPanel {
                Layout.fillWidth: true
                Layout.fillHeight: true
                focusedImage: root.focusedImage
                interactionPolicy: root.interactionPolicy
                onRatingRequested: function(rating) {
                    root.ratingRequested(rating)
                }
                onDescriptionSaveRequested: function(caption) {
                    root.descriptionSaveRequested(caption)
                }
                onRatingReasonSaveRequested: function(reasons) {
                    root.ratingReasonSaveRequested(reasons)
                }
                onContextMenuRequested: function(item, sceneX, sceneY) {
                    root.contextMenuRequested(item, sceneX, sceneY)
                }
            }

            ExportInspectorPanel {
                id: exportInspectorPage
                Layout.fillWidth: true
                Layout.fillHeight: true
                exportQueueState: root.exportQueueState
                selectionState: root.selectionState
                selectedCount: root.selectedCount
                pageActive: root.currentPage === 2
            }
        }
    }
}
