pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts

// Renders the QVariant value produced from a merge conflict JSON node as
// readable key/value rows. Objects and arrays are flattened only for display;
// the original QVariant is still passed through unchanged when a choice is
// submitted.
Item {
    id: root

    property var value: null
    property color textColor: appTheme.textColor
    property color mutedColor: appTheme.textMutedColor
    property bool compact: false
    property int maxEntries: 6

    readonly property int rowHeight: compact ? appTheme.lineHeightCaption
                                             : appTheme.lineHeightBody
    readonly property var entries: buildEntries(root.value)

    implicitWidth: appTheme.spaceXl * 8
    implicitHeight: Math.max(appTheme.lineHeightCaption,
                             root.entries.length * root.rowHeight
                             + Math.max(0, root.entries.length - 1) * appTheme.spaceXs)

    function prettyKey(key) {
        var raw = String(key || "")
        if (raw.length === 0)
            return qsTr("Value")
        return raw.replace(/[_-]+/g, " ").replace(/\b\w/g, function(letter) {
            return letter.toUpperCase()
        })
    }

    function formatScalar(source) {
        if (source === undefined || source === null)
            return qsTr("null")
        if (typeof source === "boolean")
            return source ? qsTr("On") : qsTr("Off")
        if (typeof source === "number") {
            if (!isFinite(source))
                return String(source)
            var rounded = Math.round(source)
            if (Math.abs(source - rounded) < 0.0005)
                return String(rounded)
            return source.toFixed(3).replace(/0+$/, "").replace(/\.$/, "")
        }
        if (typeof source === "string")
            return source.length > 0 ? source : qsTr("Empty")
        try {
            var serialized = JSON.stringify(source)
            return serialized && serialized !== "undefined" ? serialized : qsTr("No value")
        } catch (error) {
            return String(source)
        }
    }

    function appendEntries(source, path, depth, output) {
        if (source === undefined || source === null) {
            output.push({key: path.length > 0 ? path : qsTr("Value"),
                         value: formatScalar(source)})
            return
        }

        if (typeof source !== "object") {
            output.push({key: path.length > 0 ? path : qsTr("Value"),
                         value: formatScalar(source)})
            return
        }

        var keys = Object.keys(source)
        if (keys.length === 0) {
            output.push({key: path.length > 0 ? path : qsTr("Value"),
                         value: Array.isArray(source) ? "[]" : "{}"})
            return
        }

        for (var index = 0; index < keys.length; ++index) {
            var key = keys[index]
            var childPath = Array.isArray(source)
                    ? (path.length > 0 ? path + " / " : "") + "[" + key + "]"
                    : (path.length > 0 ? path + " / " : "") + prettyKey(key)
            var child = source[key]
            if (child !== null && child !== undefined && typeof child === "object"
                    && depth < 2) {
                appendEntries(child, childPath, depth + 1, output)
            } else {
                output.push({key: childPath, value: formatScalar(child)})
            }
        }
    }

    function buildEntries(source) {
        var allEntries = []
        appendEntries(source, "", 0, allEntries)
        if (allEntries.length <= root.maxEntries)
            return allEntries

        var visibleCount = Math.max(1, root.maxEntries - 1)
        var visibleEntries = allEntries.slice(0, visibleCount)
        visibleEntries.push({key: "", value: qsTr("+%1 more fields")
                             .arg(allEntries.length - visibleCount)})
        return visibleEntries
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: appTheme.spaceXs

        Repeater {
            model: root.entries

            delegate: RowLayout {
                id: jsonEntryRow
                required property var modelData

                Layout.fillWidth: true
                Layout.preferredHeight: root.rowHeight
                spacing: appTheme.spaceSm

                Label {
                    Layout.preferredWidth: appTheme.spaceXl * 4
                    text: jsonEntryRow.modelData.key
                    color: root.mutedColor
                    elide: Text.ElideRight
                    verticalAlignment: Text.AlignVCenter
                    font.family: appTheme.uiFontFamily
                    font.pixelSize: appTheme.fontSizeCaption
                }

                Label {
                    Layout.fillWidth: true
                    text: jsonEntryRow.modelData.value
                    color: root.textColor
                    elide: Text.ElideRight
                    verticalAlignment: Text.AlignVCenter
                    font.family: appTheme.monoFontFamily
                    font.pixelSize: root.compact ? appTheme.fontSizeCaption
                                                 : appTheme.fontSizeBody
                }
            }
        }
    }
}
