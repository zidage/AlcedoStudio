import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Import progress overlay. Concentric rings show the progress of each kind of
// file, as the Apple Watch activity rings do: the outer ring counts RAW files
// and the inner ring counts the other image files. A ring is drawn only when
// the import has files of its kind. Below the rings is the total count of
// photos imported out of the photos expected. Files rejected as not supported
// (or failed) leave the expected total of their ring, so the totals shrink
// while the import runs instead of stalling a ring. The rings spin while the
// amount of remaining work is unknown (start of preparing, finalizing).
//
// The kind of a file follows its extension: only the extension is known before
// the import reads the file.
//
// While the import works, a gradient turns around the card outline. The close
// (X) button at the top right cancels the import. The cancel takes effect at
// once: the title changes, the rings and the outline turn gray, and the X is
// disabled. Files already being read finish and the photos imported so far
// are saved; queued files are skipped.
Item {
    id: root
    objectName: "importProgressOverlay"
    property var theme: null
    property Item blurSource: null
    anchors.fill: parent

    readonly property var handler: appModules.importExport
    readonly property string phase: handler.importPhase
    readonly property bool canceling: handler.importCanceling
    readonly property int total: handler.importTotal
    readonly property int completed: handler.importCompleted
    readonly property int failed: handler.importFailed
    readonly property int expected: Math.max(0, total - failed)

    readonly property int rawTotal: Math.min(handler.importRawTotal, total)
    readonly property int rawCompleted: Math.min(handler.importRawCompleted, completed)
    readonly property int rawExpected: Math.max(0, rawTotal - Math.min(handler.importRawFailed,
                                                                       failed))
    readonly property int otherTotal: total - rawTotal
    readonly property int otherCompleted: completed - rawCompleted
    readonly property int otherExpected: Math.max(0, expected - rawExpected)

    readonly property bool finalizing: phase === "finalizing"
    readonly property bool progressUnknown: finalizing
                                            || (phase === "preparing"
                                                && completed + failed === 0)
    readonly property bool canCancel: !canceling && !finalizing

    readonly property real ringsSize: 176
    readonly property real ringWidth: 16
    readonly property real ringGap: 4

    readonly property color textColor: root.theme ? root.theme.colText : appTheme.textColor
    readonly property color mutedColor: root.theme ? root.theme.colTextMuted
                                                   : appTheme.textMutedColor

    function formatCount(value) {
        return Number(value).toLocaleString(Qt.locale(), 'f', 0)
    }

    function title() {
        if (root.canceling)
            return qsTr("Canceling Import")
        if (root.finalizing)
            return qsTr("Saving Photos")
        return qsTr("Importing Photos")
    }

    function ringColor(color) {
        return root.canceling ? root.mutedColor : color
    }

    function trackColor(color) {
        const c = root.ringColor(color)
        return Qt.rgba(c.r, c.g, c.b, 0.2)
    }

    BlurredOverlay {
        anchors.fill: parent
        blurSource: root.blurSource
        overlayColor: root.theme ? root.theme.colOverlay : appTheme.overlayColor

        Rectangle {
            id: importCard
            anchors.centerIn: parent
            width: Math.min(parent.width - appTheme.spaceXl * 2, 320)
            height: importDialogContent.implicitHeight + appTheme.spaceLg * 2
            radius: appTheme.panelRadius
            color: root.theme ? root.theme.colBgDeep : appTheme.bgDeepColor
            border.width: 0

            RotatingTaskBorder {
                objectName: "importProgressOutline"
                anchors.fill: parent
                radius: importCard.radius
                color: root.canceling ? root.mutedColor : appTheme.backgroundTaskWorkingColor
                running: root.visible && !root.canceling
            }

            ColumnLayout {
                id: importDialogContent
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.top: parent.top
                anchors.margins: appTheme.spaceLg
                spacing: appTheme.spaceLg

                RowLayout {
                    Layout.fillWidth: true
                    spacing: appTheme.spaceSm

                    Label {
                        objectName: "importProgressTitle"
                        Layout.fillWidth: true
                        Layout.alignment: Qt.AlignVCenter
                        text: root.title()
                        elide: Text.ElideRight
                        font.family: appTheme.headlineFontFamily
                        font.pixelSize: appTheme.fontSizeTitle
                        font.weight: appTheme.fontWeightStrong
                        color: root.textColor
                    }

                    IconActionButton {
                        objectName: "importCancelButton"
                        Layout.alignment: Qt.AlignVCenter
                        compact: true
                        enabled: root.canCancel
                        iconSrc: "qrc:/panel_icons/close.svg"
                        fillIdle: root.theme ? root.theme.colBgDeep : appTheme.bgDeepColor
                        actionName: qsTr("Cancel import")
                        onClicked: root.handler.CancelImport()
                    }
                }

                Item {
                    Layout.alignment: Qt.AlignHCenter
                    Layout.preferredWidth: root.ringsSize
                    Layout.preferredHeight: root.ringsSize

                    ImportProgressRing {
                        objectName: "importRawRing"
                        anchors.centerIn: parent
                        width: root.ringsSize
                        height: root.ringsSize
                        visible: root.rawTotal > 0
                        ringWidth: root.ringWidth
                        fillColor: root.ringColor(appTheme.importRawRingColor)
                        trackColor: root.trackColor(appTheme.importRawRingColor)
                        indeterminate: root.progressUnknown
                        running: root.visible
                        progress: root.rawExpected > 0 ? root.rawCompleted / root.rawExpected : 0
                        Accessible.role: Accessible.ProgressBar
                        Accessible.name: qsTr("RAW photos: %1 of %2 imported")
                                         .arg(root.rawCompleted).arg(root.rawExpected)
                    }

                    ImportProgressRing {
                        objectName: "importOtherRing"
                        readonly property real side: root.rawTotal > 0
                                                     ? root.ringsSize
                                                       - 2 * (root.ringWidth + root.ringGap)
                                                     : root.ringsSize
                        anchors.centerIn: parent
                        width: side
                        height: side
                        visible: root.otherTotal > 0
                        ringWidth: root.ringWidth
                        fillColor: root.ringColor(appTheme.importOtherRingColor)
                        trackColor: root.trackColor(appTheme.importOtherRingColor)
                        indeterminate: root.progressUnknown
                        running: root.visible
                        progress: root.otherExpected > 0
                                  ? root.otherCompleted / root.otherExpected : 0
                        Accessible.role: Accessible.ProgressBar
                        Accessible.name: qsTr("Other photos: %1 of %2 imported")
                                         .arg(root.otherCompleted).arg(root.otherExpected)
                    }
                }

                Label {
                    objectName: "importProgressCount"
                    Layout.fillWidth: true
                    horizontalAlignment: Text.AlignHCenter
                    text: qsTr("%1 / %2").arg(root.formatCount(root.completed))
                                         .arg(root.formatCount(root.expected))
                    font.family: appTheme.headlineFontFamily
                    font.pixelSize: appTheme.fontSizeHeadline
                    font.weight: appTheme.fontWeightStrong
                    color: root.textColor
                }
            }
        }
    }
}
