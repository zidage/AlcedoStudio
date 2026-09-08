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
            }

            Text {
                visible: !colorTempModel.supported
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                text: qsTr("White balance is unavailable for this image.")
                color: root.colMuted
                font.pixelSize: appTheme.fontSizeCaption
            }

            SliderRow {
                objectName: "rawCctSlider"
                title: qsTr("Temperature")
                valueText: Math.round(colorTempModel.cct) + " K"
                from: 0
                to: 4096
                value: colorTempModel.cctSliderPos
                rowEnabled: root.controlsEnabled && colorTempModel.supported
                gradientStops: Gradient {
                    orientation: Gradient.Horizontal
                    GradientStop { position: 0.0; color: "#9BD8FF" }
                    GradientStop { position: 0.5; color: "#FFE8B0" }
                    GradientStop { position: 1.0; color: "#FF8A3D" }
                }
                onBegin: function () { colorTempModel.beginCctDrag() }
                onUpdate: function (v) { colorTempModel.updateCctSliderDrag(Math.round(v)) }
                onFinish: function () { colorTempModel.finishCctDrag() }
                onReset: function () { colorTempModel.reset() }
            }

            SliderRow {
                objectName: "rawTintSlider"
                title: qsTr("Tint")
                valueText: String(Math.round(colorTempModel.tint))
                from: -150
                to: 150
                value: colorTempModel.tint
                rowEnabled: root.controlsEnabled && colorTempModel.supported
                gradientStops: Gradient {
                    orientation: Gradient.Horizontal
                    GradientStop { position: 0.0; color: "#49C26D" }
                    GradientStop { position: 0.5; color: "#E6E6E6" }
                    GradientStop { position: 1.0; color: "#A85AE6" }
                }
                onBegin: function () { colorTempModel.beginTintDrag() }
                onUpdate: function (v) { colorTempModel.updateTintDrag(v) }
                onFinish: function () { colorTempModel.finishTintDrag() }
                onReset: function () { colorTempModel.reset() }
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

    component SliderRow: ColumnLayout {
        id: row
        property string title: ""
        property string valueText: ""
        property real from: 0
        property real to: 100
        property real value: 0
        property var gradientStops: null
        property bool rowEnabled: true
        property var onBegin: function () {}
        property var onUpdate: function (v) {}
        property var onFinish: function () {}
        property var onReset: function () {}

        Layout.fillWidth: true
        spacing: 2

        RowLayout {
            Layout.fillWidth: true
            Layout.preferredHeight: appTheme.lineHeightCaption
            spacing: appTheme.spaceSm
            Text {
                Layout.fillWidth: true
                text: row.title
                color: root.colText
                font.pixelSize: appTheme.fontSizeCaption
                elide: Text.ElideRight
            }
            Text {
                text: row.valueText
                color: root.colMuted
                font.pixelSize: appTheme.fontSizeCaption
                font.family: appTheme.dataFontFamily
            }
        }
        EditorMonoSlider {
            from: row.from
            to: row.to
            externalValue: row.value
            enabled: row.rowEnabled
            gradientStops: row.gradientStops
            flickable: root.flickable
            onBegin: function () { row.onBegin() }
            onUpdate: function (v) { row.onUpdate(v) }
            onFinish: function () { row.onFinish() }
            onReset: function () { row.onReset() }
        }
    }
}
