import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts

// Import progress overlay. Two measures, each with its own label:
//  - the ring and the large count: photos imported out of the photos expected.
//    Files rejected as not RAW (or failed) leave the expected total, so the
//    total shrinks while the import runs instead of stalling the ring.
//  - the track below: files checked out of all files handed to the import.
// "preparing" registers the files, "reading" reads their metadata, and
// "finalizing" writes the result to the project; the ring spins while the
// amount of remaining work is unknown (start of preparing, finalizing).
Item {
    id: root
    objectName: "importProgressOverlay"
    property var theme: null
    property Item blurSource: null
    anchors.fill: parent

    readonly property var handler: appModules.importExport
    readonly property string phase: handler.importPhase
    readonly property int total: handler.importTotal
    readonly property int completed: handler.importCompleted
    readonly property int failed: handler.importFailed
    readonly property int unsupported: Math.min(handler.importUnsupported, failed)
    readonly property int errors: failed - unsupported
    readonly property int expected: Math.max(0, total - failed)
    readonly property int checked: Math.min(total, completed + failed)
    readonly property bool finalizing: phase === "finalizing"
    readonly property bool progressUnknown: finalizing
                                            || (phase === "preparing" && checked === 0)

    function formatCount(value) {
        return Number(value).toLocaleString(Qt.locale(), 'f', 0)
    }

    function phaseText() {
        if (root.finalizing)
            return qsTr("Saving to the library...")
        if (root.phase === "preparing")
            return qsTr("Preparing files... %1 of %2")
                   .arg(root.formatCount(root.handler.importPrepared))
                   .arg(root.formatCount(root.total))
        return qsTr("Reading photos... %1 of %2 files checked")
               .arg(root.formatCount(root.checked))
               .arg(root.formatCount(root.total))
    }

    BlurredOverlay {
        anchors.fill: parent
        blurSource: root.blurSource
        overlayColor: root.theme ? root.theme.colOverlay : appTheme.overlayColor

        Rectangle {
            anchors.centerIn: parent
            width: Math.min(parent.width - appTheme.spaceXl * 2, 420)
            height: importDialogContent.implicitHeight + appTheme.spaceLg * 2
            radius: appTheme.panelRadius
            color: root.theme ? root.theme.colBgDeep : appTheme.bgDeepColor
            border.width: 0

            ColumnLayout {
                id: importDialogContent
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.top: parent.top
                anchors.margins: appTheme.spaceLg
                spacing: appTheme.spaceMd

                Label {
                    text: qsTr("Importing Photos")
                    font.family: root.theme ? root.theme.headlineFontFamily : appTheme.headlineFontFamily
                    font.pixelSize: appTheme.fontSizeHeadline
                    font.weight: appTheme.fontWeightHeading
                    color: root.theme ? root.theme.colText : appTheme.textColor
                    Layout.alignment: Qt.AlignHCenter
                }

                ImportProgressRing {
                    objectName: "importProgressRing"
                    Layout.alignment: Qt.AlignHCenter
                    Layout.preferredWidth: 160
                    Layout.preferredHeight: 160
                    ringWidth: 14
                    trackColor: root.theme ? root.theme.colHover : appTheme.hoverColor
                    fillColor: root.theme ? root.theme.colAccentPrimary : appTheme.accentColor
                    indeterminate: root.progressUnknown
                    running: root.visible
                    progress: root.expected > 0 ? root.completed / root.expected : 0
                }

                ColumnLayout {
                    Layout.alignment: Qt.AlignHCenter
                    spacing: 0

                    Label {
                        objectName: "importProgressCount"
                        Layout.alignment: Qt.AlignHCenter
                        text: qsTr("%1 / %2").arg(root.formatCount(root.completed))
                                             .arg(root.formatCount(root.expected))
                        font.family: root.theme ? root.theme.dataFontFamily : appTheme.dataFontFamily
                        font.pixelSize: appTheme.fontSizeHeadline
                        font.weight: appTheme.fontWeightStrong
                        color: root.theme ? root.theme.colText : appTheme.textColor
                    }

                    Label {
                        Layout.alignment: Qt.AlignHCenter
                        text: qsTr("photos imported")
                        color: root.theme ? root.theme.colTextMuted : appTheme.textMutedColor
                        font.pixelSize: appTheme.fontSizeCaption
                    }
                }

                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: appTheme.spaceXs

                    Label {
                        objectName: "importProgressPhase"
                        Layout.fillWidth: true
                        wrapMode: Text.Wrap
                        text: root.phaseText()
                        color: root.theme ? root.theme.colTextMuted : appTheme.textMutedColor
                        font.family: appTheme.dataFontFamily
                        font.pixelSize: appTheme.fontSizeCaption
                    }

                    ThemedProgressBar {
                        Layout.fillWidth: true
                        active: root.visible
                        indeterminate: root.finalizing
                        progressValue: root.total > 0 ? root.checked * 100 / root.total : 0
                    }
                }

                Label {
                    Layout.fillWidth: true
                    visible: root.unsupported > 0
                    wrapMode: Text.Wrap
                    horizontalAlignment: Text.AlignHCenter
                    text: qsTr("%1 file(s) skipped because they are not RAW files")
                          .arg(root.formatCount(root.unsupported))
                    color: root.theme ? root.theme.colTextMuted : appTheme.textMutedColor
                    font.pixelSize: appTheme.fontSizeCaption
                }

                Label {
                    Layout.fillWidth: true
                    visible: root.errors > 0
                    wrapMode: Text.Wrap
                    horizontalAlignment: Text.AlignHCenter
                    text: qsTr("%1 file(s) failed").arg(root.formatCount(root.errors))
                    color: root.theme ? root.theme.colDanger : appTheme.dangerColor
                    font.family: root.theme ? root.theme.dataFontFamily : appTheme.dataFontFamily
                    font.pixelSize: appTheme.fontSizeCaption
                }

                Button {
                    id: importCancelButton
                    Layout.alignment: Qt.AlignHCenter
                    visible: !root.finalizing
                    text: qsTr("Cancel")
                    Material.background: root.theme ? root.theme.colDanger : appTheme.dangerColor
                    Material.foreground: root.theme ? root.theme.colText : appTheme.textColor
                    onClicked: root.handler.CancelImport()
                }
            }
        }
    }
}
