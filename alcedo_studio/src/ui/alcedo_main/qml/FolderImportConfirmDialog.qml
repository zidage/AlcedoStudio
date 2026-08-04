import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Effects

// Confirmation dialog for recursive folder import.  Lists the files
// discovered by ImportExportHandler::CollectFolderFiles and asks the user
// to confirm before starting the import.  The host calls openWith(); on
// acceptance the confirmed(filePaths) signal fires so the host can forward
// the paths to StartImport.
Popup {
    id: root
    property var theme: null
    property var host: null
    property Item blurSource: null
    property string folderPath: ""
    property var filePaths: []
    signal confirmed(var filePaths)
    signal cancelled()

    modal: true
    focus: true
    closePolicy: Popup.CloseOnEscape
    anchors.centerIn: parent
    width: parent ? Math.min(parent.width - (appTheme.spaceXl * 2), 560) : 560
    height: parent ? Math.min(parent.height - (appTheme.spaceXl * 3), 520) : 520
    padding: 0

    property bool _confirmed: false

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
            color: root.theme ? root.theme.colOverlay : appTheme.overlayColor
        }

        MouseArea { anchors.fill: parent; hoverEnabled: true }
    }

    background: Rectangle {
        radius: appTheme.panelRadius
        color: root.theme ? root.theme.colBgPanel : appTheme.cardSurfaceColor
        border.width: 0
    }

    onClosed: {
        if (!root._confirmed) {
            root.cancelled()
        }
        root._confirmed = false
    }

    function openWith(folder, paths) {
        root.folderPath = folder
        root.filePaths = paths
        root._confirmed = false
        root.open()
    }

    function fileNameOf(path) {
        var p = String(path)
        var lastSlash = Math.max(p.lastIndexOf('/'), p.lastIndexOf('\\'))
        return lastSlash >= 0 ? p.substring(lastSlash + 1) : p
    }

    contentItem: ColumnLayout {
        id: contentCol
        anchors.fill: parent
        anchors.margins: appTheme.spaceLg
        spacing: appTheme.spaceMd

        Label {
            text: qsTr("Import From Folder")
            font.family: root.theme ? root.theme.headlineFontFamily : appTheme.headlineFontFamily
            font.pixelSize: appTheme.fontSizeSection
            font.weight: appTheme.fontWeightHeading
            color: root.theme ? root.theme.colText : appTheme.textColor
        }

        Label {
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            text: qsTr("%1 file(s) found in:").arg(root.filePaths.length)
                  + "\n" + root.folderPath
            color: root.theme ? root.theme.colTextMuted : appTheme.textMutedColor
            font.pixelSize: appTheme.fontSizeBody
        }

        Rectangle {
            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.minimumHeight: appTheme.spaceXl * 4
            color: root.theme ? root.theme.colBgBase : appTheme.bgBaseColor
            radius: appTheme.controlRadiusSmall
            clip: true

            ListView {
                id: fileListView
                anchors.fill: parent
                anchors.margins: 1
                model: root.filePaths
                clip: true

                delegate: Label {
                    width: fileListView.width
                    leftPadding: appTheme.spaceMd
                    rightPadding: appTheme.spaceMd
                    topPadding: appTheme.spaceXs
                    bottomPadding: appTheme.spaceXs
                    verticalAlignment: Text.AlignVCenter
                    elide: Text.ElideRight
                    text: root.fileNameOf(modelData)
                    color: root.theme ? root.theme.colText : appTheme.textColor
                    font.pixelSize: appTheme.fontSizeCaption
                }
            }
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: appTheme.spaceSm

            Item { Layout.fillWidth: true }

            DialogActionButton {
                objectName: "folderImportCancelButton"
                text: qsTr("Cancel")
                kind: "normal"
                buttonWidth: appTheme.spaceXl * 5
                buttonHeight: appTheme.iconButtonHitSizeCompact
                buttonRadius: appTheme.controlRadiusSmall
                font.weight: appTheme.fontWeightRegular
                onClicked: root.close()
            }

            DialogActionButton {
                objectName: "folderImportConfirmButton"
                enabled: root.filePaths.length > 0
                text: qsTr("Import %1 File(s)").arg(root.filePaths.length)
                kind: "accent"
                buttonWidth: appTheme.spaceXl * 9
                buttonHeight: appTheme.iconButtonHitSizeCompact
                buttonRadius: appTheme.controlRadiusSmall
                font.weight: appTheme.fontWeightRegular
                onClicked: {
                    root._confirmed = true
                    var paths = root.filePaths
                    root.close()
                    root.confirmed(paths)
                }
            }
        }
    }
}
