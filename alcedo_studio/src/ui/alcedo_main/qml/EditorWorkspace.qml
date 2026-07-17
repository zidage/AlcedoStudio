import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts
import Alcedo.Main 1.0

// Unified editor workspace shell. The viewport is a QQuickRhiItem-backed
// presentation surface; the surrounding shell remains QML-owned.
Item {
    id: root
    objectName: "editorWorkspace"

    property var theme: null
    property var workspaceRouter: appModules.workspaceRouter
    property var editorSession: appModules.editorSession

    readonly property color colPanel: theme ? theme.colGlassPanel : "#1C1C1D"
    readonly property color colStroke: theme ? theme.colGlassStroke : Qt.rgba(1, 1, 1, 0.08)
    readonly property color colText: theme ? theme.colText : "#F5F1EA"
    readonly property color colMuted: theme ? theme.colTextMuted : "#AAA59D"
    readonly property color colAccent: theme ? theme.colAccentPrimary : "#457B9D"
    readonly property color colBg: theme ? theme.colBgCanvas : "#111214"
    readonly property color colDeep: theme ? theme.colBgDeep : "#0C0D0F"
    readonly property color colHover: theme ? theme.colHover : Qt.rgba(1, 1, 1, 0.07)
    readonly property int panelRadius: theme ? theme.panelRadius : 12
    readonly property int controlRadius: theme ? theme.controlRadius : 10
    readonly property string headlineFont: theme ? theme.headlineFontFamily : appTheme.headlineFontFamily
    readonly property bool hasImage: editorSession ? editorSession.hasImage : false
    readonly property int focusedElementId: workspaceRouter ? Number(workspaceRouter.elementId) : 0
    readonly property int focusedImageId: workspaceRouter ? Number(workspaceRouter.imageId) : 0

    // Focus order: return-to-library → toolbar → viewport → side panels → filmstrip handle.
    Component.onCompleted: {
        if (!hasImage) {
            emptyStatePrompt.forceActiveFocus()
        } else {
            viewportSlot.forceActiveFocus()
        }
    }

    function returnToLibrary() {
        if (workspaceRouter) {
            workspaceRouter.openLibrary()
        }
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 12

        // ── Editor toolbar ──────────────────────────────────────────────
        Rectangle {
            id: editorToolbar
            objectName: "editorToolbar"
            Layout.fillWidth: true
            Layout.preferredHeight: 48
            radius: root.panelRadius
            color: root.colPanel
            border.width: 1
            border.color: root.colStroke

            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: 12
                anchors.rightMargin: 12
                spacing: 10

                Button {
                    id: backToLibraryButton
                    objectName: "editorBackToLibraryButton"
                    text: qsTr("Library")
                    flat: true
                    activeFocusOnTab: true
                    Material.foreground: root.colText
                    Accessible.name: qsTr("Return to library")
                    onClicked: root.returnToLibrary()
                }

                Rectangle {
                    Layout.preferredWidth: 1
                    Layout.preferredHeight: 22
                    color: root.colStroke
                }

                Label {
                    text: root.hasImage
                          ? qsTr("Editing")
                          : qsTr("Editor")
                    color: root.colMuted
                    font.pixelSize: 12
                    font.weight: 600
                }

                Label {
                    visible: root.hasImage
                    text: qsTr("Element %1").arg(root.focusedElementId)
                    color: root.colText
                    font.pixelSize: 13
                    font.weight: 600
                    elide: Text.ElideRight
                    Layout.fillWidth: true
                }

                Item {
                    Layout.fillWidth: !root.hasImage
                    visible: !root.hasImage
                }

                Label {
                    visible: !root.hasImage
                    text: qsTr("No image selected")
                    color: root.colMuted
                    font.pixelSize: 12
                }
            }
        }

        // ── Main editor body ────────────────────────────────────────────
        RowLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            spacing: 12

            // Left adjustment panel slot (Tone / Look / Geometry placeholders)
            Rectangle {
                id: leftPanelSlot
                objectName: "editorLeftPanelSlot"
                Layout.preferredWidth: 280
                Layout.minimumWidth: 240
                Layout.maximumWidth: 360
                Layout.fillHeight: true
                radius: root.panelRadius
                color: root.colPanel
                border.width: 1
                border.color: root.colStroke
                opacity: root.hasImage ? 1.0 : 0.55

                ColumnLayout {
                    anchors.fill: parent
                    anchors.margins: 14
                    spacing: 10

                    Label {
                        text: qsTr("Adjustments")
                        color: root.colMuted
                        font.pixelSize: 12
                        font.weight: 600
                    }
                    Label {
                        Layout.fillWidth: true
                        wrapMode: Text.WordWrap
                        text: root.hasImage
                              ? qsTr("Tone, Look, Display Transform, Geometry, and RAW Decode panels mount here.")
                              : qsTr("Select an image to enable adjustment controls.")
                        color: root.colMuted
                        font.pixelSize: 12
                    }
                    Item { Layout.fillHeight: true }
                }
            }

            // Center column: viewport + filmstrip
            ColumnLayout {
                Layout.fillWidth: true
                Layout.fillHeight: true
                Layout.minimumWidth: 360
                spacing: 12

                Rectangle {
                    id: viewportSlot
                    objectName: "editorViewportSlot"
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    radius: root.panelRadius
                    color: root.colDeep
                    border.width: 1
                    border.color: root.colStroke
                    clip: true
                    activeFocusOnTab: true
                    focus: true
                    Accessible.role: Accessible.Canvas
                    Accessible.name: qsTr("Editor viewport")
                    Accessible.description: root.hasImage
                                            ? qsTr("Image viewport")
                                            : qsTr("Empty viewport")

                    // No-image empty state — centered localized prompt.
                    Column {
                        id: emptyStatePrompt
                        objectName: "editorEmptyState"
                        anchors.centerIn: parent
                        width: Math.min(parent.width - 48, 420)
                        spacing: 12
                        visible: !root.hasImage
                        activeFocusOnTab: true
                        Accessible.role: Accessible.StaticText
                        Accessible.name: qsTr("Select an image to edit")

                        Label {
                            anchors.horizontalCenter: parent.horizontalCenter
                            text: qsTr("Select an image to edit")
                            font.family: root.headlineFont
                            font.pixelSize: 22
                            font.weight: 700
                            color: root.colText
                            horizontalAlignment: Text.AlignHCenter
                        }
                        Label {
                            width: parent.width
                            wrapMode: Text.WordWrap
                            horizontalAlignment: Text.AlignHCenter
                            text: qsTr("Open an image from the library, or keep this workspace ready for search results.")
                            color: root.colMuted
                            font.pixelSize: 13
                        }
                        Button {
                            anchors.horizontalCenter: parent.horizontalCenter
                            text: qsTr("Back to Library")
                            activeFocusOnTab: true
                            Material.background: root.colAccent
                            Material.foreground: root.colText
                            onClicked: root.returnToLibrary()
                        }
                    }

                    EditorViewportItem {
                        id: editorViewportItem
                        objectName: "editorViewportItem"
                        anchors.fill: parent
                        visible: root.hasImage
                        imageGeneration: root.focusedImageId
                        Accessible.role: Accessible.Canvas
                        Accessible.name: qsTr("Image viewport")
                    }

                    Label {
                        objectName: "editorViewportStatus"
                        anchors.centerIn: parent
                        visible: root.hasImage && !editorViewportItem.presentationAvailable
                        text: qsTr("Preparing image viewport")
                        color: root.colMuted
                        font.pixelSize: 14
                    }
                }

                EditorFilmstrip {
                    id: editorFilmstrip
                    Layout.fillWidth: true
                    Layout.preferredHeight: editorFilmstrip.dockHeight
                    theme: root.theme
                    editorSession: root.editorSession
                    // Phase 1B: empty model; position/count remain defined for handle UI.
                    currentIndex: root.hasImage ? 1 : 0
                    totalCount: root.hasImage ? 1 : 0
                    saveInProgress: false
                }
            }

            // Right inspector / scope / history slot
            Rectangle {
                id: rightPanelSlot
                objectName: "editorRightPanelSlot"
                Layout.preferredWidth: 300
                Layout.minimumWidth: 240
                Layout.maximumWidth: 420
                Layout.fillHeight: true
                radius: root.panelRadius
                color: root.colPanel
                border.width: 1
                border.color: root.colStroke
                opacity: root.hasImage ? 1.0 : 0.55

                ColumnLayout {
                    anchors.fill: parent
                    anchors.margins: 14
                    spacing: 12

                    Label {
                        text: qsTr("Scopes & History")
                        color: root.colMuted
                        font.pixelSize: 12
                        font.weight: 600
                    }
                    Rectangle {
                        Layout.fillWidth: true
                        Layout.preferredHeight: 120
                        radius: 8
                        color: "transparent"
                        border.width: 1
                        border.color: root.colStroke
                        Label {
                            anchors.centerIn: parent
                            text: qsTr("Histogram / Waveform")
                            color: root.colMuted
                            font.pixelSize: 12
                        }
                    }
                    Rectangle {
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        radius: 8
                        color: "transparent"
                        border.width: 1
                        border.color: root.colStroke
                        Label {
                            anchors.centerIn: parent
                            text: qsTr("History / Versions")
                            color: root.colMuted
                            font.pixelSize: 12
                        }
                    }
                }
            }
        }
    }
}
