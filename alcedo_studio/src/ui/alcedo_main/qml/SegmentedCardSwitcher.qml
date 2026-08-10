import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts

// Shared monochrome segmented switcher: a sunken track (bgBase + hairline
// border) holding equal-width title segments. The selected segment inverts to
// the editor-list selected well (light fill + dark ink); hover tints with the
// theme hover color. Selection is value-led when both `currentValue` and the
// entry value are non-empty, index-led otherwise, mirroring the adjustment-
// panel selectedPath pattern so snapshot load and user select both invalidate
// the highlight.
//
// Per-entry disable: an entry may set `enabled: false` so unsupported choices
// stay visible but non-interactive (e.g. bit depth locked by export format).
// Root `enabled` remains the master gate for the whole track.
//
// Tokens come from `appTheme`; callers can override the theme color properties
// (e.g. editor panels pass their `theme`-derived colors) and geometry. The
// caller owns the model: bind `entries` / `currentValue` / `currentIndex` and
// handle `onSelected` to apply the choice (e.g. `model.selectIndex(index)`).
Rectangle {
    id: root

    property var entries: []
    property string currentValue: ""
    property int currentIndex: -1
    property bool enabled: true

    // Geometry (token-derived; overridable).
    property int segmentHeight: appTheme.iconButtonHitSizeCompact + appTheme.spaceSm
    property int trackInset: appTheme.spaceXs
    property real trackRadius: appTheme.controlRadiusSmall

    // Theme colors (default to appTheme tokens; editor panels override with
    // their `theme`-derived aliases so the switcher matches the panel family).
    property color trackColor: appTheme.bgBaseColor
    property color trackBorderColor: appTheme.cardBorderColor
    property color textColor: appTheme.textColor
    property color hoverColor: appTheme.hoverColor
    property color selectedFillColor: appTheme.editorListSelectedFillColor
    property color selectedInkColor: appTheme.editorListSelectedInkColor

    signal selected(int index, string value)

    implicitHeight: segmentHeight + 2 * trackInset
    implicitWidth: 200
    radius: trackRadius
    color: trackColor
    border.width: 1
    border.color: trackBorderColor
    opacity: enabled ? 1.0 : 0.55

    RowLayout {
        id: cardRow
        objectName: "segmentedCardSwitcherCardRow"
        anchors.fill: parent
        anchors.margins: trackInset
        spacing: 2

        Repeater {
            model: root.entries
            delegate: Rectangle {
                id: segment
                required property var modelData
                required property int index

                // Per-entry disable: entries may set `enabled: false` (e.g. bit
                // depths unsupported by the current export format). Root.enabled
                // remains the master gate for the whole track.
                readonly property bool segmentEnabled: {
                    if (!root.enabled)
                        return false
                    if (!modelData || modelData.enabled === undefined)
                        return true
                    return modelData.enabled !== false
                }

                // Value-led when both sides carry a value, index-led otherwise
                // (matches the adjustment-panel selectedPath highlight rule).
                readonly property bool sel: {
                    var entryVal = modelData && modelData.value !== undefined
                            ? String(modelData.value) : ""
                    var curVal = String(root.currentValue)
                    if (curVal.length > 0 && entryVal.length > 0)
                        return entryVal === curVal
                    return root.currentIndex === index
                }
                readonly property bool hovered: segMouse.containsMouse

                Layout.fillWidth: true
                Layout.fillHeight: true
                activeFocusOnTab: segmentEnabled
                Accessible.role: Accessible.RadioButton
                Accessible.name: modelData ? String(modelData.label || "") : ""
                Accessible.checkable: true
                Accessible.checked: sel
                radius: Math.max(2, root.trackRadius - 2)
                opacity: segmentEnabled ? 1.0 : 0.38
                color: sel ? root.selectedFillColor
                           : (hovered && segmentEnabled ? root.hoverColor : "transparent")
                border.width: activeFocus ? 1 : 0
                border.color: appTheme.accentColor

                Keys.onPressed: function(event) {
                    if (segmentEnabled && (event.key === Qt.Key_Space
                            || event.key === Qt.Key_Return || event.key === Qt.Key_Enter)) {
                        const entryValue = modelData && modelData.value !== undefined
                                ? String(modelData.value) : ""
                        root.selected(index, entryValue)
                        event.accepted = true
                    }
                }

                Label {
                    anchors.centerIn: parent
                    width: parent.width - appTheme.spaceSm
                    horizontalAlignment: Text.AlignHCenter
                    text: modelData ? modelData.label : ""
                    color: sel ? root.selectedInkColor : root.textColor
                    font.family: appTheme.uiFontFamily
                    font.pixelSize: appTheme.fontSizeBody
                    font.weight: sel ? appTheme.fontWeightStrong
                                      : appTheme.fontWeightRegular
                    elide: Text.ElideRight
                }

                MouseArea {
                    id: segMouse
                    anchors.fill: parent
                    hoverEnabled: segment.segmentEnabled
                    cursorShape: segment.segmentEnabled ? Qt.PointingHandCursor
                                                        : Qt.ArrowCursor
                    enabled: segment.segmentEnabled
                    onClicked: {
                        var entryVal = segment.modelData && segment.modelData.value !== undefined
                                ? String(segment.modelData.value) : ""
                        root.selected(segment.index, entryVal)
                    }
                }
            }
        }
    }
}
