import QtQuick
import QtQuick.Controls
import QtQuick.Dialogs
import QtQuick.Layouts
import Alcedo.Main 1.0

// Camera Profile section of the RAW panel (DCP camera profile plan, section 2.2). One combo: the
// default entry (Built-in, or Embedded for a DNG file with an embedded profile) and the DCP
// profiles of the folder that match the camera. A selection commits one settled edit of the
// Develop field camera_profile_file. A stored reference that does not resolve shows its name in
// dangerColor. Without a DCP folder, Choose Folder opens the folder dialog of the Settings page.
Item {
    id: root
    objectName: "editorCameraProfileSection"

    property var theme: null
    property var editorSession: null
    property bool controlsEnabled: true
    // CameraProfileLibraryService (appModules.cameraProfiles); tests pass their own.
    property var library: typeof appModules !== "undefined" && appModules
                          ? appModules.cameraProfiles : null

    readonly property bool hasFolder: !!root.library && root.library.rootPath.length > 0
    readonly property bool scanning: !!root.library && root.library.scanning

    readonly property color colText: theme ? theme.colText : appTheme.textColor
    readonly property color colMuted: theme ? theme.colTextMuted : appTheme.textMutedColor
    readonly property color colCardSurface: theme ? theme.colCardSurface : appTheme.cardSurfaceColor
    readonly property color colCardBorder: theme ? theme.colCardBorder : appTheme.cardBorderColor

    implicitHeight: section.implicitHeight
    implicitWidth: 200

    function loadFromSnapshot(snapshot) {
        if (snapshot)
            profileModel.loadFromSnapshot(snapshot)
    }

    // Called with the dialog's folder URL; exposed for tests that cannot drive the native dialog.
    function useFolder(folder) {
        if (root.library)
            root.library.setRootFolder(folder)
    }

    EditorCameraProfileListModel {
        id: profileModel
        objectName: "rawCameraProfileModel"
        library: root.library
        cameraMake: root.editorSession && root.editorSession.cameraMake !== undefined
                    ? String(root.editorSession.cameraMake) : ""
        cameraModel: root.editorSession && root.editorSession.cameraModel !== undefined
                     ? String(root.editorSession.cameraModel) : ""
        enabled: root.controlsEnabled && !root.scanning
        submitter: root.editorSession
    }

    FolderDialog {
        id: folderDialog
        title: qsTr("Choose the DCP folder")
        onAccepted: root.useFolder(selectedFolder)
    }

    CollapsibleSection {
        id: section
        objectName: "editorAdjustmentGroupShell_raw_camera_profile"
        width: parent.width
        title: qsTr("Camera Profile")
        expanded: true
        controlsEnabled: root.controlsEnabled
        surfaceColor: root.colCardSurface
        disabledSurfaceColor: root.colCardSurface
        borderColor: root.colCardBorder
        textColor: root.colText
        mutedColor: root.colMuted
        hoverColor: theme ? theme.colHover : appTheme.hoverColor
        accentColor: appTheme.editorSliderHandleColor
        bodyContentHeight: profileBody.implicitHeight + appTheme.spaceSm

        ColumnLayout {
            id: profileBody
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: parent.top
            anchors.margins: appTheme.spaceXs
            spacing: appTheme.spaceSm

            AdjustmentCombo {
                objectName: "rawCameraProfileComboRow"
                controlObjectName: "rawCameraProfileCombo"
                Layout.fillWidth: true
                model: profileModel
                warning: profileModel.currentUnresolved
            }

            DialogActionButton {
                objectName: "rawCameraProfileChooseFolderButton"
                visible: !root.hasFolder
                enabled: root.controlsEnabled && !!root.library
                buttonWidth: 132
                buttonHeight: 30
                text: qsTr("Choose Folder")
                onClicked: folderDialog.open()
            }
        }
    }
}
