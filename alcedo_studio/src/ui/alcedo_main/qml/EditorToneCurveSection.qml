import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Alcedo.Main 1.0

// Tone Curve section of the Tone panel (#890): channel selector, the curve plot on PlotPane over
// the histogram of the curve input, and the In and Out fields of the selected point. The plot
// layers only report positions; EditorToneCurveModel moves the points and writes them (one settled
// history entry per drag, key move sequence, field edit, removal, and reset).
CollapsibleSection {
    id: section
    objectName: "editorToneCurveGroup"
    title: qsTr("Tone Curve")
    expanded: true

    // EditorToneCurveModel of the panel.
    property var model: null
    property var editorSession: null
    // True while the Tone panel is the visible adjustment panel.
    property bool panelVisible: true
    property bool histogramVisible: true
    property bool fitToPhoto: false

    // EditorCurveHistogramController of the session; absent in test sessions.
    readonly property var histogramSource: editorSession && editorSession.curveHistogram
                                           ? editorSession.curveHistogram : null
    readonly property int channel: model ? model.channel : 0
    readonly property var channelModified: model ? model.channelModified : [false, false, false, false]
    readonly property var channelColors: [appTheme.plotLineColor, appTheme.plotChannelRedColor,
                                          appTheme.plotChannelGreenColor, appTheme.plotChannelBlueColor]
    // Id of the preset whose points the selected curve has; empty for other points.
    readonly property string presetId: model ? model.presetId : "linear"
    readonly property real axisGrey: model ? model.axisGrey : 0.4135
    readonly property real axisStop: model ? model.axisStop : 0.0571
    readonly property int labelInset: appTheme.fontSizeCaption + appTheme.spaceSm
    // Output of the selected curve at the pointer input.
    readonly property real pointerOutput: {
        if (!section.model || !pane.pointerInside)
            return 0
        var dependency = section.model.points
        return section.model.evaluate(section.channel, pane.pointerPosition.x)
    }

    bodyContentHeight: curveBody.implicitHeight + appTheme.spaceSm

    // Menu name of a preset id (EditorToneCurveModel.presets); Custom for an empty id.
    function presetLabel(id) {
        switch (id) {
        case "": return qsTr("Custom")
        case "linear": return qsTr("Linear")
        case "soft_contrast": return qsTr("Soft Contrast")
        case "medium_contrast": return qsTr("Medium Contrast")
        case "strong_contrast": return qsTr("Strong Contrast")
        case "film_like": return qsTr("Film-like")
        case "filmic": return qsTr("Filmic")
        case "kodak_2383": return qsTr("Kodak 2383")
        case "fuji_velvia_50": return qsTr("Velvia 50")
        case "kodak_ektachrome_e100": return qsTr("Ektachrome E100")
        }
        const presets = section.model ? section.model.presets : []
        for (let i = 0; i < presets.length; ++i) {
            if (presets[i].id === id)
                return presets[i].name
        }
        return id
    }

    function applyPreset(id) {
        if (section.model)
            section.model.applyPreset(id)
    }

    function evText(value) {
        const ev = (value - section.axisGrey) / section.axisStop
        if (Math.abs(ev) < 0.005)
            return qsTr("%1 EV").arg("0.00")
        const digits = Math.abs(ev).toFixed(2)
        return qsTr("%1 EV").arg((ev > 0 ? "+" : "−") + digits)
    }

    // Grid lines every 2 EV from 18% grey inside [0, 1]; the line at 18% grey is strong.
    function evTicks(withLabels) {
        var ticks = []
        for (var ev = -8; ev <= 10; ev += 2) {
            const value = section.axisGrey + ev * section.axisStop
            if (value < 0 || value > 1)
                continue
            var label = ""
            if (withLabels)
                label = ev === 0 ? "0" : (ev > 0 ? "+" + ev : "−" + Math.abs(ev))
            ticks.push({ value: value, label: label, strong: ev === 0 })
        }
        return ticks
    }

    function lineOf(channel) {
        if (!section.model)
            return []
        switch (channel) {
        case 1: return section.model.redLine
        case 2: return section.model.greenLine
        case 3: return section.model.blueLine
        default: return section.model.rgbLine
        }
    }

    function fadedColor(color) {
        return Qt.rgba(color.r, color.g, color.b, appTheme.plotInactiveLineOpacity)
    }

    function applyFit() {
        if (section.fitToPhoto) {
            const low = section.axisGrey + (-7) * section.axisStop
            const high = section.axisGrey + 5 * section.axisStop
            pane.setView(Qt.rect(low, low, high - low, high - low))
        } else {
            pane.resetView()
        }
    }

    onFitToPhotoChanged: applyFit()

    // The curve input histogram is counted only while it shows.
    Binding {
        target: section.histogramSource
        property: "active"
        value: section.histogramVisible && section.expanded && section.panelVisible
        when: section.histogramSource !== null
        restoreMode: Binding.RestoreValue
    }
    Component.onDestruction: {
        if (section.histogramSource)
            section.histogramSource.active = false
    }

    headerActions: Component {
        Row {
            spacing: 2

            IconActionButton {
                objectName: "toneCurveHistogramToggle"
                compact: true
                selected: section.histogramVisible
                iconSrc: "qrc:/panel_icons/histogram.svg"
                actionName: qsTr("Histogram")
                onClicked: section.histogramVisible = !section.histogramVisible
            }
            IconActionButton {
                objectName: "toneCurveFitToggle"
                compact: true
                selected: section.fitToPhoto
                iconSrc: "qrc:/panel_icons/fit-range.svg"
                actionName: qsTr("Fit to photo range")
                onClicked: section.fitToPhoto = !section.fitToPhoto
            }
            AdjustmentResetButton {
                objectName: "toneCurveResetButton"
                model: section.model
            }
        }
    }

    ColumnLayout {
        id: curveBody
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.margins: appTheme.spaceXs
        spacing: appTheme.spaceSm

        RowLayout {
            Layout.fillWidth: true
            spacing: appTheme.spaceSm

            SegmentedCardSwitcher {
                id: channelSelector
                objectName: "toneCurveChannelSelector"
                Layout.fillWidth: true
                Layout.preferredWidth: 0
                Layout.horizontalStretchFactor: 1
                enabled: section.controlsEnabled
                entries: [
                    { label: qsTr("RGB"), value: "rgb", channel: 0 },
                    { label: qsTr("R"), value: "red", channel: 1 },
                    { label: qsTr("G"), value: "green", channel: 2 },
                    { label: qsTr("B"), value: "blue", channel: 3 }
                ]
                currentIndex: section.channel
                // A dot before the title of a curve that is not the identity.
                segmentDecoration: Component {
                    Rectangle {
                        readonly property bool modified:
                            entry ? section.channelModified[entry.channel] === true : false
                        objectName: "toneCurveChannelMark_" + (entry ? entry.value : "")
                        visible: modified
                        // No room for the dot of an unchanged curve, so its title stays centered.
                        width: modified ? appTheme.plotChannelMarkSize : 0
                        height: appTheme.plotChannelMarkSize
                        radius: width / 2
                        color: selected ? appTheme.editorListSelectedInkColor : appTheme.textColor
                    }
                }
                onSelected: function(index, value) {
                    if (section.model)
                        section.model.channel = index
                }
            }

            // Preset menu of the selected curve: the name of the preset whose points the curve has,
            // or Custom.
            Rectangle {
                id: presetButton
                objectName: "toneCurvePresetButton"
                readonly property string text: section.presetLabel(section.presetId)
                Layout.fillWidth: true
                Layout.preferredWidth: 0
                Layout.horizontalStretchFactor: 1
                Layout.preferredHeight: channelSelector.implicitHeight
                radius: appTheme.controlRadiusSmall
                color: presetMouse.containsMouse && section.controlsEnabled ? appTheme.hoverColor
                                                                            : appTheme.bgBaseColor
                border.width: 1
                border.color: presetMenu.visible ? appTheme.textMutedColor : appTheme.cardBorderColor
                opacity: section.controlsEnabled ? 1.0 : 0.55

                Text {
                    objectName: "toneCurvePresetName"
                    anchors.left: parent.left
                    anchors.right: presetChevron.left
                    anchors.leftMargin: appTheme.spaceSm
                    anchors.rightMargin: appTheme.spaceXs
                    anchors.verticalCenter: parent.verticalCenter
                    text: presetButton.text
                    elide: Text.ElideRight
                    color: appTheme.textColor
                    font.family: appTheme.uiFontFamily
                    font.pixelSize: appTheme.fontSizeBody
                }
                Text {
                    id: presetChevron
                    anchors.right: parent.right
                    anchors.rightMargin: appTheme.spaceSm
                    anchors.verticalCenter: parent.verticalCenter
                    text: "›"
                    rotation: 90
                    color: appTheme.textMutedColor
                    font.pixelSize: appTheme.fontSizeTitle
                }
                MouseArea {
                    id: presetMouse
                    anchors.fill: parent
                    hoverEnabled: true
                    enabled: section.controlsEnabled && section.model !== null
                    cursorShape: Qt.PointingHandCursor
                    onClicked: presetMenu.open()
                }

                AppContextMenu {
                    id: presetMenu
                    objectName: "toneCurvePresetMenu"
                    x: 0
                    y: presetButton.height + 2

                    PresetMenuItem {
                        presetId: "linear"
                        text: section.presetLabel(presetId)
                        checked: section.presetId === presetId
                        onTriggered: section.applyPreset(presetId)
                    }
                    AppMenuSeparator {}
                    PresetMenuItem {
                        presetId: "soft_contrast"
                        text: section.presetLabel(presetId)
                        checked: section.presetId === presetId
                        onTriggered: section.applyPreset(presetId)
                    }
                    PresetMenuItem {
                        presetId: "medium_contrast"
                        text: section.presetLabel(presetId)
                        checked: section.presetId === presetId
                        onTriggered: section.applyPreset(presetId)
                    }
                    PresetMenuItem {
                        presetId: "strong_contrast"
                        text: section.presetLabel(presetId)
                        checked: section.presetId === presetId
                        onTriggered: section.applyPreset(presetId)
                    }
                    AppMenuSeparator {}
                    PresetMenuItem {
                        presetId: "film_like"
                        text: section.presetLabel(presetId)
                        checked: section.presetId === presetId
                        onTriggered: section.applyPreset(presetId)
                    }
                    PresetMenuItem {
                        presetId: "filmic"
                        text: section.presetLabel(presetId)
                        checked: section.presetId === presetId
                        onTriggered: section.applyPreset(presetId)
                    }
                    AppMenuSeparator {}
                    PresetMenuItem {
                        presetId: "kodak_2383"
                        text: section.presetLabel(presetId)
                        checked: section.presetId === presetId
                        onTriggered: section.applyPreset(presetId)
                    }
                    PresetMenuItem {
                        presetId: "fuji_velvia_50"
                        text: section.presetLabel(presetId)
                        checked: section.presetId === presetId
                        onTriggered: section.applyPreset(presetId)
                    }
                    PresetMenuItem {
                        presetId: "kodak_ektachrome_e100"
                        text: section.presetLabel(presetId)
                        checked: section.presetId === presetId
                        onTriggered: section.applyPreset(presetId)
                    }
                }
            }
        }

        Item {
            id: plotFrame
            Layout.fillWidth: true
            // Square plot rectangle with the EV labels under it.
            Layout.preferredHeight: width + section.labelInset

            PlotPane {
                id: pane
                objectName: "toneCurvePlotPane"
                anchors.fill: parent
                enabled: section.controlsEnabled
                limits: Qt.rect(0, 0, 1, 1)
                insetBottom: section.labelInset
                backgroundColor: appTheme.cardSurfaceColor
                surfaceColor: appTheme.plotSurfaceColor

                onBackgroundPressed: function(position, modifiers) {
                    if (section.model)
                        section.model.clearSelection()
                }

                PlotGridLayer {
                    xTicks: section.evTicks(true)
                    yTicks: section.evTicks(false)
                    color: appTheme.plotGridColor
                    strongColor: appTheme.plotGridStrongColor
                    labelColor: appTheme.plotLabelColor
                    labelFont.family: appTheme.dataFontFamily
                    labelFont.pixelSize: appTheme.fontSizeCaption
                    lineWidth: appTheme.plotGridLineWidth
                }
                PlotHistogramLayer {
                    objectName: "toneCurveHistogramLayer"
                    source: section.histogramSource
                    visible: section.histogramVisible && section.histogramSource !== null
                    channels: section.channel === 1 ? PlotHistogramLayer.Red
                            : section.channel === 2 ? PlotHistogramLayer.Green
                            : section.channel === 3 ? PlotHistogramLayer.Blue
                            : PlotHistogramLayer.All
                    redColor: appTheme.scopeHistogramRedColor
                    greenColor: appTheme.scopeHistogramGreenColor
                    blueColor: appTheme.scopeHistogramBlueColor
                    cutColor: appTheme.plotSurfaceColor
                    edgeWidth: appTheme.plotLineWidth
                }
                PlotPolylineLayer {
                    objectName: "toneCurveIdentityLine"
                    points: [Qt.point(0, 0), Qt.point(1, 1)]
                    color: appTheme.plotReferenceLineColor
                    width: appTheme.plotGridLineWidth
                    dashLength: 4
                    gapLength: 4
                }
                // Faint curves of the other channels that are not the identity.
                PlotPolylineLayer {
                    visible: section.channel !== 0 && section.channelModified[0] === true
                    points: section.model ? section.model.rgbLine : []
                    color: section.fadedColor(section.channelColors[0])
                    width: appTheme.plotLineWidth
                }
                PlotPolylineLayer {
                    visible: section.channel !== 1 && section.channelModified[1] === true
                    points: section.model ? section.model.redLine : []
                    color: section.fadedColor(section.channelColors[1])
                    width: appTheme.plotLineWidth
                }
                PlotPolylineLayer {
                    visible: section.channel !== 2 && section.channelModified[2] === true
                    points: section.model ? section.model.greenLine : []
                    color: section.fadedColor(section.channelColors[2])
                    width: appTheme.plotLineWidth
                }
                PlotPolylineLayer {
                    visible: section.channel !== 3 && section.channelModified[3] === true
                    points: section.model ? section.model.blueLine : []
                    color: section.fadedColor(section.channelColors[3])
                    width: appTheme.plotLineWidth
                }
                PlotPolylineLayer {
                    objectName: "toneCurvePointerLine"
                    visible: pane.pointerInside
                    points: [Qt.point(pane.pointerPosition.x, pane.view.y),
                             Qt.point(pane.pointerPosition.x, pane.view.y + pane.view.height)]
                    color: appTheme.plotPointerLineColor
                    width: appTheme.plotGridLineWidth
                }
                // The curve of the selected channel; a press on it adds a point on the curve.
                PlotPolylineLayer {
                    id: curveLine
                    objectName: "toneCurveLine"
                    interactive: section.controlsEnabled
                    points: section.lineOf(section.channel)
                    color: section.channelColors[section.channel]
                    width: appTheme.plotLineWidth
                    hitRadius: appTheme.plotCurveHitRadius
                    onPressed: function(position, modifiers) {
                        if (section.model)
                            section.model.insertPointOnCurve(position.x)
                    }
                    onMoved: function(position, modifiers) {
                        if (section.model)
                            section.model.updateDrag(position.x, position.y, modifiers)
                    }
                    onReleased: function(position, modifiers) {
                        if (section.model)
                            section.model.finishDrag()
                    }
                }
                PlotHandleLayer {
                    id: handles
                    objectName: "toneCurveHandles"
                    interactive: section.controlsEnabled
                    points: section.model ? section.model.points : []
                    selectedIndex: section.model ? section.model.selectedIndex : -1
                    radius: appTheme.plotHandleRadius
                    ringWidth: appTheme.plotHandleRingWidth
                    outlineWidth: appTheme.plotHandleOutlineWidth
                    color: appTheme.plotHandleColor
                    selectedColor: appTheme.plotHandleColor
                    outlineColor: appTheme.plotHandleOutlineColor
                    hitRadius: appTheme.plotHitRadius
                    onPressed: function(index, position, modifiers) {
                        if (section.model)
                            section.model.beginDrag(index)
                    }
                    onMoved: function(index, position, modifiers) {
                        if (section.model)
                            section.model.updateDrag(position.x, position.y, modifiers)
                    }
                    onReleased: function(index, position, modifiers) {
                        if (section.model)
                            section.model.finishDrag()
                    }
                    onDoubleClicked: function(index) {
                        if (section.model)
                            section.model.removePoint(index)
                    }
                    onKeyPressed: function(index, key, modifiers) {
                        if (section.model)
                            section.model.handleKey(key, modifiers)
                    }
                }
            }

            // In and Out at the pointer, at the top left of the plot.
            Row {
                objectName: "toneCurvePointerReadout"
                x: pane.plotRect.x + appTheme.spaceSm
                y: pane.plotRect.y + appTheme.spaceXs
                visible: pane.pointerInside
                spacing: appTheme.spaceXs

                Text {
                    text: qsTr("In")
                    color: appTheme.plotLabelColor
                    font.family: appTheme.uiFontFamily
                    font.pixelSize: appTheme.fontSizeCaption
                }
                Text {
                    text: section.evText(pane.pointerPosition.x)
                    color: appTheme.textColor
                    font.family: appTheme.dataFontFamily
                    font.pixelSize: appTheme.fontSizeCaption
                }
                Item {
                    width: appTheme.spaceXs
                    height: 1
                }
                Text {
                    text: qsTr("Out")
                    color: appTheme.plotLabelColor
                    font.family: appTheme.uiFontFamily
                    font.pixelSize: appTheme.fontSizeCaption
                }
                Text {
                    text: section.evText(section.pointerOutput)
                    color: appTheme.textColor
                    font.family: appTheme.dataFontFamily
                    font.pixelSize: appTheme.fontSizeCaption
                }
            }
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: appTheme.spaceMd

            PointField {
                objectName: "toneCurveInField"
                Layout.fillWidth: true
                label: qsTr("In")
                available: section.model ? section.model.hasSelection : false
                value: section.model ? section.model.selectedX : 0
                evText: available ? section.evText(value) : ""
                onCommitted: function(value) {
                    if (section.model)
                        section.model.setSelectedXValue(value)
                }
            }
            PointField {
                objectName: "toneCurveOutField"
                Layout.fillWidth: true
                label: qsTr("Out")
                available: section.model ? section.model.hasSelection : false
                value: section.model ? section.model.selectedY : 0
                evText: available ? section.evText(value) : ""
                onCommitted: function(value) {
                    if (section.model)
                        section.model.setSelectedYValue(value)
                }
            }
        }
    }

    // One preset of the menu. The use site binds the name, the check of the preset of the selected
    // curve, and the apply: an inline component has no access to the ids of this file.
    component PresetMenuItem: AppMenuItem {
        property string presetId: ""
        objectName: "toneCurvePreset_" + presetId
        checkable: false
    }

    // Field of one coordinate of the selected point: a plain TextInput in a hairline box and the
    // EV value after it. Enter or focus out writes a changed value.
    component PointField: RowLayout {
        id: field

        property string label: ""
        property real value: 0
        property bool available: false
        property string evText: ""

        signal committed(real value)

        function valueText() {
            return field.available ? field.value.toFixed(4) : ""
        }

        spacing: appTheme.spaceXs

        Text {
            text: field.label
            color: appTheme.textMutedColor
            font.family: appTheme.uiFontFamily
            font.pixelSize: appTheme.fontSizeCaption
        }

        Rectangle {
            Layout.preferredWidth: fieldMetrics.advanceWidth + 2 * appTheme.spaceSm
            Layout.preferredHeight: input.implicitHeight + 2 * appTheme.spaceXs
            color: appTheme.bgBaseColor
            radius: appTheme.controlRadiusSmall
            border.width: 1
            border.color: input.activeFocus ? appTheme.textMutedColor : appTheme.dividerColor
            opacity: field.available ? 1.0 : 0.55

            TextMetrics {
                id: fieldMetrics
                font: input.font
                text: "0.00000"
            }

            TextInput {
                id: input
                objectName: field.objectName + "Input"
                anchors.fill: parent
                anchors.leftMargin: appTheme.spaceSm
                anchors.rightMargin: appTheme.spaceSm
                verticalAlignment: TextInput.AlignVCenter
                readOnly: !field.available
                activeFocusOnTab: field.available
                selectByMouse: true
                clip: true
                color: appTheme.textColor
                selectionColor: appTheme.editorListSelectedFillColor
                selectedTextColor: appTheme.editorListSelectedInkColor
                font.family: appTheme.dataFontFamily
                font.pixelSize: appTheme.fontSizeBody
                validator: DoubleValidator {
                    bottom: 0
                    top: 1
                    decimals: 4
                    notation: DoubleValidator.StandardNotation
                    locale: "C"
                }
                text: field.valueText()
                onEditingFinished: {
                    const number = Number(input.text)
                    if (field.available && isFinite(number))
                        field.committed(number)
                    input.text = Qt.binding(function() { return field.valueText() })
                }
            }
        }

        Text {
            Layout.fillWidth: true
            text: field.evText
            color: appTheme.textMutedColor
            font.family: appTheme.dataFontFamily
            font.pixelSize: appTheme.fontSizeCaption
            elide: Text.ElideRight
        }
    }
}
