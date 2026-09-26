import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Alcedo.Main 1.0

// Develop-owned White Balance. ColorTemp writes are rejected unless Develop is
// the selected node; this section lives on the RAW Decode page.
Item {
    id: root
    objectName: "editorWhiteBalanceSection"

    property var theme: null
    property var editorSession: null
    property var flickable: null
    property bool controlsEnabled: true

    readonly property color colText: theme ? theme.colText : appTheme.textColor
    readonly property color colMuted: theme ? theme.colTextMuted : appTheme.textMutedColor
    readonly property color colCardSurface: theme ? theme.colCardSurface : appTheme.cardSurfaceColor
    readonly property color colCardBorder: theme ? theme.colCardBorder : appTheme.cardBorderColor
    readonly property color colBase: theme ? theme.colBgBase : appTheme.bgBaseColor

    implicitHeight: section.implicitHeight
    implicitWidth: 200

    function loadFromSnapshot(snapshot) {
        if (!snapshot || snapshot.color_temp === undefined)
            return
        if (colorTempModel.dragActive)
            return
        colorTempModel.loadFromOperatorParams(snapshot.color_temp)
    }

    EditorColorTempModel {
        id: colorTempModel
        objectName: "rawColorTempModel"
        submitter: root.editorSession
        enabled: root.controlsEnabled
    }

    CollapsibleSection {
        id: section
        objectName: "editorAdjustmentGroupShell_raw_wb"
        width: parent.width
        title: qsTr("White Balance")
        expanded: true
        controlsEnabled: root.controlsEnabled
        surfaceColor: root.colCardSurface
        disabledSurfaceColor: root.colCardSurface
        borderColor: root.colCardBorder
        textColor: root.colText
        mutedColor: root.colMuted
        hoverColor: theme ? theme.colHover : appTheme.hoverColor
        accentColor: appTheme.editorSliderHandleColor
        bodyContentHeight: wbBody.implicitHeight + appTheme.spaceSm

        ColumnLayout {
            id: wbBody
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: parent.top
            anchors.margins: appTheme.spaceXs
            spacing: appTheme.spaceSm

            RowLayout {
                Layout.fillWidth: true
                spacing: appTheme.spaceXs
                ModeChip {
                    label: qsTr("As Shot")
                    selected: colorTempModel.modeIndex === 0
                    chipEnabled: root.controlsEnabled && colorTempModel.supported
                    onActivated: colorTempModel.selectMode(0)
                }
                ModeChip {
                    label: qsTr("Custom")
                    selected: colorTempModel.modeIndex === 1
                    chipEnabled: root.controlsEnabled && colorTempModel.supported
                    onActivated: colorTempModel.selectMode(1)
                }
                Item { Layout.fillWidth: true }
                EditorInfoHint {
                    objectName: "rawWhiteBalanceInfoHint"
                    text: qsTr("RAW white balance sets the scene illuminant from the camera's own color data before the image enters the grading color space. As Shot uses the camera's recorded white balance. The Look page white balance is a separate creative adjustment applied after this one.")
                }
            }

            Text {
                visible: !colorTempModel.supported
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                text: qsTr("White balance is unavailable for this image.")
                color: root.colMuted
                font.pixelSize: appTheme.fontSizeCaption
            }

            EditorWhiteBalanceSliders {
                Layout.fillWidth: true
                flickable: root.flickable
                slidersEnabled: root.controlsEnabled && colorTempModel.supported
                textColor: root.colText
                mutedColor: root.colMuted
                temperatureObjectName: "rawCctSlider"
                tintObjectName: "rawTintSlider"
                temperature: colorTempModel.cct
                temperatureSliderPos: colorTempModel.cctSliderPos
                tint: colorTempModel.tint
                onTemperatureDragBegin: colorTempModel.beginCctDrag()
                onTemperatureDragUpdate: function (pos) { colorTempModel.updateCctSliderDrag(pos) }
                onTemperatureDragFinish: colorTempModel.finishCctDrag()
                onTemperatureReset: colorTempModel.reset()
                onTintDragBegin: colorTempModel.beginTintDrag()
                onTintDragUpdate: function (value) { colorTempModel.updateTintDrag(value) }
                onTintDragFinish: colorTempModel.finishTintDrag()
                onTintReset: colorTempModel.reset()
            }
        }
    }

    component ModeChip: Rectangle {
        id: chip
        property string label: ""
        property bool selected: false
        property bool chipEnabled: true
        signal activated()

        implicitWidth: chipLabel.implicitWidth + 20
        implicitHeight: 26
        radius: appTheme.controlRadiusSmall
        color: selected ? appTheme.editorSliderHandleColor : root.colBase
        border.width: 1
        border.color: root.colCardBorder
        opacity: chipEnabled ? 1.0 : 0.45

        Text {
            id: chipLabel
            anchors.centerIn: parent
            text: chip.label
            color: chip.selected ? root.colBase : root.colText
            font.pixelSize: appTheme.fontSizeCaption
        }
        MouseArea {
            anchors.fill: parent
            enabled: chip.chipEnabled
            cursorShape: Qt.PointingHandCursor
            onClicked: chip.activated()
        }
    }
}
