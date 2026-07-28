import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts
import QtQuick.Effects

// Delete-from-project / remove-from-album confirmation popup. The host
// builds the confirm text and calls openWith(); acceptance is signalled
// back so the host can run the delete and clear pending targets.
Popup {
    id: root
    property var theme: null
    property var host: null
    property Item blurSource: null
    property string confirmText: ""
    signal cancelled()
    signal confirmed()
    modal: true
    focus: true
    closePolicy: Popup.CloseOnEscape
    width: host ? Math.min(host.width - 36, 520) : 520
    height: deleteConfirmContent.implicitHeight + 36
    x: host ? Math.round((host.width - width) / 2) : 0
    y: host ? Math.round((host.height - height) / 2) : 0

    Overlay.modal: Item {
        anchors.fill: parent

        MultiEffect {
            anchors.fill: parent
            source: root.blurSource
            blurEnabled: true
            blur: 0.6
            blurMax: 64
            saturation: -0.2
        }

        Rectangle {
            anchors.fill: parent
            color: root.theme ? root.theme.colOverlay : Qt.rgba(0, 0, 0, 0.5)
        }

        MouseArea { anchors.fill: parent; hoverEnabled: true }
    }

    background: Rectangle {
        radius: 14
        color: root.theme ? root.theme.colBgPanel : "#1C1C1D"
        border.width: 0
    }

    onClosed: {
        root.confirmText = ""
    }

    function openWith(text) {
        root.confirmText = text
        root.open()
    }

    contentItem: ColumnLayout {
        id: deleteConfirmContent
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.margins: 18
        spacing: 12

        Label {
            text: qsTr("Confirm Deletion")
            font.family: root.theme ? root.theme.headlineFontFamily : appTheme.headlineFontFamily
            font.pixelSize: 24
            font.weight: 700
            color: root.theme ? root.theme.colText : appTheme.textColor
        }

        Label {
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            text: root.confirmText.length > 0
                  ? qsTr("%1\nOriginal source files on disk will be kept.")
                        .arg(root.confirmText)
                  : ""
            color: root.theme ? root.theme.colText : appTheme.textColor
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: 10

            Item { Layout.fillWidth: true }

            Button {
                id: deleteCancelButton
                text: qsTr("Cancel")
                Material.background: root.theme ? root.theme.colButtonSecondary : "#3A3F44"
                Material.foreground: root.theme ? root.theme.colText : appTheme.textColor
                onClicked: {
                    root.cancelled()
                    root.close()
                }
            }

            Button {
                id: deleteConfirmButton
                text: qsTr("Delete")
                // Gated by the same policy as the context-menu delete action —
                // stays disabled while the targets are being analyzed.
                enabled: appModules.interactionPolicy.canDeletePendingTargets
                Material.background: root.theme ? root.theme.colDanger : appTheme.dangerColor
                Material.foreground: root.theme ? root.theme.colText : appTheme.textColor
                onClicked: {
                    root.close()
                    root.confirmed()
                }
            }
        }
    }
}