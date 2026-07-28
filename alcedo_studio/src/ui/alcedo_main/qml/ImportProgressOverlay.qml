import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts

// Import progress overlay with determinate ring, count, status, and cancel.
Item {
    id: root
    property var theme: null
    property Item blurSource: null
    anchors.fill: parent

    BlurredOverlay {
        anchors.fill: parent
        blurSource: root.blurSource
        overlayColor: root.theme ? root.theme.colOverlay : Qt.rgba(0, 0, 0, 0.5)

        Rectangle {
            anchors.centerIn: parent
            width: Math.min(parent.width - 36, 420)
            height: importDialogContent.implicitHeight + 36
            radius: 14
            color: root.theme ? root.theme.colBgDeep : "#0C0D0F"
            border.width: 0

            ColumnLayout {
                id: importDialogContent
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.top: parent.top
                anchors.margins: 20
                spacing: 16

                Label {
                    text: qsTr("Importing Photos")
                    font.family: root.theme ? root.theme.headlineFontFamily : appTheme.headlineFontFamily
                    font.pixelSize: 21
                    font.weight: 700
                    color: root.theme ? root.theme.colText : appTheme.textColor
                    Layout.alignment: Qt.AlignHCenter
                }

                ImportProgressRing {
                    Layout.alignment: Qt.AlignHCenter
                    Layout.preferredWidth: 160
                    Layout.preferredHeight: 160
                    ringWidth: 14
                    trackColor: root.theme ? root.theme.colHover : appTheme.hoverColor
                    fillColor: root.theme ? root.theme.colAccentPrimary : appTheme.accentColor
                    progress: appModules.importExport.importTotal > 0
                              ? appModules.importExport.importCompleted / appModules.importExport.importTotal
                              : 0
                }

                Label {
                    Layout.alignment: Qt.AlignHCenter
                    text: qsTr("%1 / %2").arg(appModules.importExport.importCompleted).arg(appModules.importExport.importTotal)
                    font.family: root.theme ? root.theme.dataFontFamily : appTheme.dataFontFamily
                    font.pixelSize: 28
                    font.weight: 600
                    color: root.theme ? root.theme.colText : appTheme.textColor
                }

                Label {
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                    horizontalAlignment: Text.AlignHCenter
                    text: appModules.importExport.importStatus.length > 0
                          ? appModules.importExport.importStatus
                          : qsTr("Preparing...")
                    color: root.theme ? root.theme.colTextMuted : appTheme.textMutedColor
                    font.pixelSize: 12
                }

                Label {
                    Layout.fillWidth: true
                    visible: appModules.importExport.importFailed > 0
                    wrapMode: Text.WordWrap
                    horizontalAlignment: Text.AlignHCenter
                    text: qsTr("%1 file(s) failed").arg(appModules.importExport.importFailed)
                    color: root.theme ? root.theme.colDanger : appTheme.dangerColor
                    font.family: root.theme ? root.theme.dataFontFamily : appTheme.dataFontFamily
                    font.pixelSize: 12
                }

                Button {
                    id: importCancelButton
                    Layout.alignment: Qt.AlignHCenter
                    text: qsTr("Cancel")
                    Material.background: root.theme ? root.theme.colDanger : appTheme.dangerColor
                    Material.foreground: root.theme ? root.theme.colText : appTheme.textColor
                    onClicked: appModules.importExport.CancelImport()
                }
            }
        }
    }
}