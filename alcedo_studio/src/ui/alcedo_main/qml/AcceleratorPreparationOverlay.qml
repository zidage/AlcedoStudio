import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// OpenCL kernel-compilation overlay shown while the accelerator prepares.
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
            height: acceleratorPreparationContent.implicitHeight + 36
            radius: 14
            color: root.theme ? root.theme.colBgDeep : "#0C0D0F"
            border.width: 0

            ColumnLayout {
                id: acceleratorPreparationContent
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.top: parent.top
                anchors.margins: 20
                spacing: 16

                Label {
                    text: qsTr("Preparing OpenCL")
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
                    indeterminate: true
                    running: appModules.project.acceleratorPreparing
                }

                Label {
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                    horizontalAlignment: Text.AlignHCenter
                    text: appModules.project.acceleratorPreparationStatus.length > 0
                          ? appModules.project.acceleratorPreparationStatus
                          : qsTr("Compiling kernels...")
                    color: root.theme ? root.theme.colTextMuted : appTheme.textMutedColor
                    font.pixelSize: 12
                }
            }
        }
    }
}