import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import Alcedo.Main 1.0

// Histogram and waveform presentation for the unified editor right rail.
// The controller owns cadence and identity filtering; this file owns only
// mode selection, accessibility copy, and theme-token wiring.
Item {
    id: root
    objectName: "editorScopePanel"

    property var theme: null
    property var editorSession: null
    property bool controlsEnabled: true
    readonly property var scopeController: editorSession ? editorSession.scopeController : null
    readonly property bool visualActive: root.visible
                                         && !!root.editorSession
                                         && !!root.editorSession.hasImage
                                         && root.controlsEnabled
    readonly property bool hasSnapshot: root.scopeController
                                        ? root.scopeController.hasSnapshot
                                        : false
    readonly property color colBase: theme ? theme.colBgBase : appTheme.bgBaseColor
    readonly property color colCardBorder: theme ? theme.colCardBorder : appTheme.cardBorderColor
    readonly property color colMuted: theme ? theme.colTextMuted : appTheme.textMutedColor
    readonly property color colSelected: appTheme.editorListSelectedFillColor
    readonly property color colSelectedInk: appTheme.editorListSelectedInkColor
    readonly property color colGrid: theme ? theme.colScopeGrid : appTheme.scopeGridColor
    readonly property color colPlotBorder: theme
                                              ? theme.colScopePlotBorder
                                              : appTheme.scopePlotBorderColor
    readonly property color colHistogramRed: theme
                                               ? theme.colScopeHistogramRed
                                               : appTheme.scopeHistogramRedColor
    readonly property color colHistogramGreen: theme
                                                 ? theme.colScopeHistogramGreen
                                                 : appTheme.scopeHistogramGreenColor
    readonly property color colHistogramBlue: theme
                                                ? theme.colScopeHistogramBlue
                                                : appTheme.scopeHistogramBlueColor

    implicitHeight: appTheme.editorScopeHeight
    Layout.fillWidth: true
    Layout.minimumHeight: appTheme.editorScopeHeightMin

    onVisualActiveChanged: {
        if (root.scopeController) {
            root.scopeController.visualActive = root.visualActive
        }
    }
    onScopeControllerChanged: {
        if (root.scopeController) {
            root.scopeController.visualActive = root.visualActive
        }
    }

    Component.onDestruction: {
        if (root.scopeController) {
            root.scopeController.visualActive = false
        }
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: appTheme.spaceXs

        SlidingIconNav {
            id: scopeModeNav
            objectName: "editorScopeModeNav"
            Layout.fillWidth: true
            Layout.preferredHeight: implicitHeight
            currentKey: root.scopeController && root.scopeController.activeView === 1
                        ? "waveform" : "histogram"
            controlsEnabled: root.controlsEnabled
            trackColor: root.colBase
            trackBorderColor: root.colCardBorder
            idleIconColor: root.colMuted
            selectedFillColor: root.colSelected
            selectedInkColor: root.colSelectedInk
            thumbObjectName: "editorScopeModeNavThumb"
            items: [
                { key: "histogram", icon: "qrc:/panel_icons/histogram.svg",
                  label: qsTr("Show histogram"),
                  itemObjectName: "editorScopeModeHistogram" },
                { key: "waveform", icon: "qrc:/panel_icons/waveform.svg",
                  label: qsTr("Show waveform"),
                  itemObjectName: "editorScopeModeWaveform" }
            ]
            onActivated: key => {
                if (root.scopeController)
                    root.scopeController.activeView = key === "waveform" ? 1 : 0
            }
        }

        Rectangle {
            id: scopePlotWell
            objectName: "editorScopePlotWell"
            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.minimumHeight: appTheme.editorScopeHeightMin - appTheme.iconButtonHitSizeCompact
            radius: appTheme.controlRadiusSmall
            color: root.colBase
            clip: true

            Rectangle {
                objectName: "editorScopePlotTopRule"
                anchors.top: parent.top
                anchors.left: parent.left
                anchors.right: parent.right
                height: 1
                color: root.colPlotBorder
                Accessible.ignored: true
            }

            Rectangle {
                objectName: "editorScopePlotBottomRule"
                anchors.bottom: parent.bottom
                anchors.left: parent.left
                anchors.right: parent.right
                height: 1
                color: root.colPlotBorder
                Accessible.ignored: true
            }

            EditorScopeItem {
                id: scopePlot
                objectName: "editorScopePlot"
                anchors.fill: parent
                controller: root.scopeController
                backgroundColor: root.colBase
                gridColor: root.colGrid
                borderColor: root.colPlotBorder
                histogramRedColor: root.colHistogramRed
                histogramGreenColor: root.colHistogramGreen
                histogramBlueColor: root.colHistogramBlue
                Accessible.ignored: true
            }

            Label {
                id: scopeEmptyLabel
                objectName: "editorScopeEmptyLabel"
                anchors.centerIn: parent
                text: root.editorSession && root.editorSession.hasImage
                      ? qsTr("Reading display scope")
                      : qsTr("Select an image to view scopes")
                color: root.colMuted
                font.family: theme ? theme.uiFontFamily : appTheme.uiFontFamily
                font.pixelSize: appTheme.fontSizeCaption
                visible: !root.hasSnapshot
                Accessible.role: Accessible.StaticText
                Accessible.name: text
            }
        }
    }
}
