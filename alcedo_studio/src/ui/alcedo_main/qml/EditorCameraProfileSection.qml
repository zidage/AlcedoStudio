import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Alcedo.Main 1.0

// Camera Profile section of the RAW panel (DCP camera profile plan, section 2.2). One combo: the
// default entry (Built-in, or Embedded for a DNG file with an embedded profile) and the DCP
// profiles of the folder that match the camera. A selection commits one settled edit of the
// Develop field camera_profile_file. A stored reference that does not resolve shows its name in
// dangerColor. A note under the combo names the reason when it holds only the default entry: no
// DCP folder (set in Settings > LUTs and Color Management), or no profile of this camera.
// Under the combo, the Tone Curve switch turns the profile tone stage of a profile with a
// ProfileToneCurve on or off; each change is one settled edit of camera_profile_file.
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

            // Profile tone stage of the selected DCP profile (dcp_profile_tone_curve_study.md,
            // section 6.4). Film Print runs no stage, so the switch is disabled there and keeps
            // its stored value. The switch follows the echo of its write.
            RowLayout {
                objectName: "rawCameraProfileToneCurveRow"
                Layout.fillWidth: true
                visible: profileModel.toneCurveAvailable
                spacing: appTheme.spaceSm

                Label {
                    Layout.fillWidth: true
                    text: qsTr("Tone Curve")
                    color: root.colText
                    font.pixelSize: appTheme.fontSizeBody
                    Accessible.name: text
                }

                Switch {
                    id: toneCurveSwitch
                    objectName: "rawCameraProfileToneCurveSwitch"
                    enabled: root.controlsEnabled && !root.scanning
                             && !profileModel.filmPrintMethod
                    checked: profileModel.profileTone
                    activeFocusOnTab: true
                    Accessible.role: Accessible.CheckBox
                    Accessible.name: qsTr("Tone Curve")
                    onToggled: {
                        if (!profileModel.submitProfileTone(toneCurveSwitch.checked))
                            toneCurveSwitch.checked = profileModel.profileTone
                    }
                    Connections {
                        target: profileModel
                        function onStoredReferenceChanged() {
                            if (!toneCurveSwitch.pressed)
                                toneCurveSwitch.checked = profileModel.profileTone
                        }
                    }
                }
            }

            // Why the combo holds only the default entry: no DCP folder is set, or the folder has
            // no profile of this camera. The folder is set in Settings.
            Label {
                objectName: "rawCameraProfileNote"
                Layout.fillWidth: true
                visible: text.length > 0
                text: !root.library || root.scanning ? ""
                      : !root.hasFolder
                        ? qsTr("Set a DCP folder in Settings > LUTs and Color Management.")
                        : profileModel.entries.length <= 1
                          ? qsTr("The DCP folder has no profile of this camera.") : ""
                color: root.colMuted
                font.pixelSize: appTheme.fontSizeCaption
                wrapMode: Text.Wrap
            }
        }
    }
}
