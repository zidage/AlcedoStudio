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
    readonly property color colText: theme ? theme.colText : appTheme.textColor
    readonly property color colMuted: theme ? theme.colTextMuted : appTheme.textMutedColor
    readonly property color colHover: theme ? theme.colHover : appTheme.hoverColor
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

        RowLayout {
            id: scopeToolbar
            objectName: "editorScopeToolbar"
            Layout.fillWidth: true
            Layout.preferredHeight: appTheme.iconButtonHitSizeCompact
            spacing: appTheme.spaceXs

            Label {
                Layout.fillWidth: true
                text: qsTr("Scopes")
                color: root.colText
                font.family: theme ? theme.dataFontFamily : appTheme.dataFontFamily
                font.pixelSize: appTheme.fontSizeCaption
                font.weight: appTheme.fontWeightStrong
                verticalAlignment: Text.AlignVCenter
            }

            Button {
                id: histogramButton
                objectName: "editorScopeModeHistogram"
                property int viewIndex: 0
                Layout.preferredWidth: 92
                Layout.preferredHeight: appTheme.iconButtonHitSizeCompact
                text: qsTr("Histogram")
                checkable: true
                checked: root.scopeController ? root.scopeController.activeView === viewIndex : true
                enabled: root.controlsEnabled
                Accessible.role: Accessible.Button
                Accessible.name: qsTr("Show histogram")
                onClicked: if (root.scopeController) root.scopeController.activeView = viewIndex

                background: Rectangle {
                    radius: appTheme.controlRadiusSmall
                    color: histogramButton.checked
                           ? root.colSelected
                           : (histogramButton.hovered ? root.colHover : root.colBase)
                    border.width: 1
                    border.color: histogramButton.checked ? root.colSelected : root.colCardBorder
                }
                contentItem: Text {
                    text: histogramButton.text
                    color: histogramButton.checked ? root.colSelectedInk : root.colMuted
                    font.family: theme ? theme.uiFontFamily : appTheme.uiFontFamily
                    font.pixelSize: appTheme.fontSizeCaption
                    font.weight: histogramButton.checked
                                ? appTheme.fontWeightStrong
                                : appTheme.fontWeightRegular
                    horizontalAlignment: Text.AlignHCenter
                    verticalAlignment: Text.AlignVCenter
                }
            }

            Button {
                id: waveformButton
                objectName: "editorScopeModeWaveform"
                property int viewIndex: 1
                Layout.preferredWidth: 92
                Layout.preferredHeight: appTheme.iconButtonHitSizeCompact
                text: qsTr("Waveform")
                checkable: true
                checked: root.scopeController ? root.scopeController.activeView === viewIndex : false
                enabled: root.controlsEnabled
                Accessible.role: Accessible.Button
                Accessible.name: qsTr("Show waveform")
                onClicked: if (root.scopeController) root.scopeController.activeView = viewIndex

                background: Rectangle {
                    radius: appTheme.controlRadiusSmall
                    color: waveformButton.checked
                           ? root.colSelected
                           : (waveformButton.hovered ? root.colHover : root.colBase)
                    border.width: 1
                    border.color: waveformButton.checked ? root.colSelected : root.colCardBorder
                }
                contentItem: Text {
                    text: waveformButton.text
                    color: waveformButton.checked ? root.colSelectedInk : root.colMuted
                    font.family: theme ? theme.uiFontFamily : appTheme.uiFontFamily
                    font.pixelSize: appTheme.fontSizeCaption
                    font.weight: waveformButton.checked
                                ? appTheme.fontWeightStrong
                                : appTheme.fontWeightRegular
                    horizontalAlignment: Text.AlignHCenter
                    verticalAlignment: Text.AlignVCenter
                }
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
