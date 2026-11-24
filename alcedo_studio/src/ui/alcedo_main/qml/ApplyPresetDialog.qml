pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Dialogs
import QtQuick.Effects
import QtQuick.Layouts
import Alcedo.Main 1.0

// Apply Preset... of the image context menu (edit presets plan 2.3): the shared
// PresetBrowser, Open File..., Cancel, and Apply. The targets are the targets of
// Paste Adjustments. Apply and a double click start the multi-target Paste of the
// selected preset; Open File... applies one preset file one time (R8). The previews
// show the first target: the previews of the editor when the editor holds that photo,
// otherwise the previews of the library photo (plan PS4, step 3).
//
// The browser exists only while the dialog is open, so its tiles watch previews only
// then. The dialog shows no count of presets or targets.
Dialog {
    id: dialog
    objectName: "applyPresetDialog"
    font.family: appTheme.uiFontFamily

    parent: Overlay.overlay
    modal: true
    focus: true
    closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
    padding: 0
    width: Math.min(parent ? parent.width - appTheme.spaceXl * 2
                           : appTheme.editorSidePanelWidthMin
                             + appTheme.editorSidePanelWidth
                             + appTheme.editorSidePanelWidthMax,
                    appTheme.editorSidePanelWidthMin
                    + appTheme.editorSidePanelWidth
                    + appTheme.editorSidePanelWidthMax)
    height: Math.min(parent ? parent.height - appTheme.spaceXl * 2
                            : appTheme.editorSidePanelWidthMax,
                     appTheme.editorSidePanelWidthMax)
    x: parent ? Math.round((parent.width - width) / 2) : 0
    y: parent ? Math.round((parent.height - height) / 2) : 0

    // PresetController (appModules.presets).
    property var presets: null
    // {elementId, imageId} maps of the photos; the first one gives the previews.
    property var targets: []
    // PresetPreviewController of the open photo (appModules.presetPreview).
    property var editorPreview: null
    // PresetPreviewController of a library photo (appModules.presetDialogPreview).
    property var libraryPreview: null
    // EditorSessionController (appModules.editorSession).
    property var editorSession: null
    property Item blurSource: null
    property real cornerRadius: 0
    property string errorText: ""

    // The result of a started apply, for the snackbar.
    signal messageRequested(string message)

    readonly property var firstTarget: targets && targets.length > 0 ? targets[0] : null
    readonly property bool firstTargetInEditor: !!firstTarget && !!editorSession
                                                && editorSession.hasImage === true
                                                && Number(editorSession.elementId)
                                                   === Number(firstTarget.elementId)
    readonly property var previewSource: firstTargetInEditor ? editorPreview : libraryPreview
    readonly property var browser: browserLoader.item
    readonly property string selectedPresetId: browser ? String(browser.selectedPresetId) : ""
    readonly property bool selectedAvailable: {
        if (!browser || !browser.model || selectedPresetId.length === 0)
            return false
        const _revision = browser.dataRevision
        return browser.roleAt(browser.model.rowOfPreset(selectedPresetId),
                              PresetListModel.AvailableRole) === true
    }

    // Start the Paste and close on success; a refusal stays in the dialog.
    function finish(result) {
        if (result && result.success === true) {
            dialog.close()
            if (result.message)
                dialog.messageRequested(String(result.message))
            return
        }
        dialog.errorText = String(result && result.message ? result.message
                                                          : qsTr("The preset cannot be applied"))
    }

    function applySelected() {
        if (!dialog.presets || !dialog.selectedAvailable)
            return
        dialog.finish(dialog.presets.ApplyToTargets(dialog.selectedPresetId, dialog.targets))
    }

    onOpened: {
        dialog.errorText = ""
        // The preset browser reads the folder when it opens (plan D9).
        if (dialog.presets)
            dialog.presets.Refresh()
        if (!dialog.firstTargetInEditor && dialog.libraryPreview && dialog.firstTarget)
            dialog.libraryPreview.showLibraryPhoto(Number(dialog.firstTarget.elementId),
                                                   Number(dialog.firstTarget.imageId))
    }

    Overlay.modal: Item {
        anchors.fill: parent

        Rectangle {
            id: backdropMask
            anchors.fill: parent
            radius: dialog.cornerRadius
            color: appTheme.textColor
            visible: false
            layer.enabled: true
            layer.smooth: true
        }

        Item {
            anchors.fill: parent
            layer.enabled: true
            layer.smooth: true
            layer.effect: MultiEffect {
                maskEnabled: dialog.cornerRadius > 0
                maskSource: backdropMask
            }

            MultiEffect {
                anchors.fill: parent
                source: dialog.blurSource
                blurEnabled: dialog.blurSource !== null
                blur: 0.72
                blurMax: 72
                saturation: -0.24
                brightness: -0.08
            }

            Rectangle {
                anchors.fill: parent
                color: appTheme.overlayColor
            }
        }
    }

    background: Rectangle {
        radius: appTheme.panelRadius
        color: appTheme.cardSurfaceColor
        border.width: 1
        border.color: appTheme.cardBorderColor
    }

    contentItem: Rectangle {
        color: "transparent"
        radius: appTheme.panelRadius
        clip: true

        ColumnLayout {
            anchors.fill: parent
            spacing: 0

            // ── Header bar ──────────────────────────────────────────────────
            Item {
                Layout.fillWidth: true
                Layout.preferredHeight: appTheme.iconButtonHitSize + appTheme.spaceMd

                RowLayout {
                    anchors.fill: parent
                    anchors.leftMargin: appTheme.spaceLg
                    anchors.rightMargin: appTheme.spaceSm
                    spacing: appTheme.spaceSm

                    Label {
                        Layout.fillWidth: true
                        text: qsTr("Apply Preset")
                        color: appTheme.textColor
                        font.family: appTheme.uiFontFamily
                        font.pixelSize: appTheme.fontSizeTitle
                        font.weight: appTheme.fontWeightHeading
                        elide: Text.ElideRight
                    }

                    Item {
                        id: closeButton
                        objectName: "applyPresetCloseButton"
                        Layout.preferredWidth: appTheme.iconButtonHitSize
                        Layout.preferredHeight: appTheme.iconButtonHitSize
                        activeFocusOnTab: true
                        Accessible.role: Accessible.Button
                        Accessible.name: qsTr("Close")
                        Accessible.onPressAction: dialog.reject()

                        Keys.onPressed: function(event) {
                            if (event.key === Qt.Key_Space || event.key === Qt.Key_Return
                                    || event.key === Qt.Key_Enter) {
                                dialog.reject()
                                event.accepted = true
                            }
                        }

                        Rectangle {
                            anchors.centerIn: parent
                            width: appTheme.iconButtonHitSize - appTheme.spaceSm
                            height: width
                            radius: appTheme.controlRadiusSmall
                            color: closeMouse.containsMouse
                                   ? appTheme.buttonHoveredFillColor
                                   : appTheme.buttonIdleFillColor
                            border.width: closeButton.activeFocus ? 1 : 0
                            border.color: Qt.rgba(appTheme.accentColor.r,
                                                  appTheme.accentColor.g,
                                                  appTheme.accentColor.b, 0.60)
                        }

                        Label {
                            anchors.centerIn: parent
                            text: "×"
                            color: closeMouse.containsMouse
                                   ? appTheme.textColor : appTheme.textMutedColor
                            font.family: appTheme.uiFontFamily
                            font.pixelSize: appTheme.fontSizeTitle
                            horizontalAlignment: Text.AlignHCenter
                            verticalAlignment: Text.AlignVCenter
                        }

                        MouseArea {
                            id: closeMouse
                            anchors.fill: parent
                            hoverEnabled: true
                            cursorShape: Qt.PointingHandCursor
                            onClicked: dialog.reject()
                        }
                    }
                }
            }

            AlertBadge {
                objectName: "applyPresetError"
                Layout.fillWidth: true
                Layout.leftMargin: appTheme.spaceLg
                Layout.rightMargin: appTheme.spaceLg
                Layout.bottomMargin: visible ? appTheme.spaceSm : 0
                text: dialog.errorText
            }

            // ── Preset browser ───────────────────────────────────────────────
            Loader {
                id: browserLoader
                Layout.fillWidth: true
                Layout.fillHeight: true
                Layout.leftMargin: appTheme.spaceLg
                Layout.rightMargin: appTheme.spaceLg
                active: dialog.visible

                sourceComponent: PresetBrowser {
                    objectName: "applyPresetBrowser"
                    model: dialog.presets ? dialog.presets.presets : null
                    previewSource: dialog.previewSource
                    viewMode: dialog.presets ? String(dialog.presets.browserViewMode) : "grid"
                    onViewModeRequested: function (mode) {
                        if (dialog.presets)
                            dialog.presets.browserViewMode = mode
                    }
                    onActivated: function (presetId) {
                        dialog.applySelected()
                    }
                    onSelectedPresetIdChanged: dialog.errorText = ""
                }
            }

            // ── Footer bar ───────────────────────────────────────────────────
            Item {
                objectName: "applyPresetFooter"
                Layout.fillWidth: true
                Layout.preferredHeight: appTheme.iconButtonHitSize + appTheme.spaceLg

                RowLayout {
                    anchors.fill: parent
                    anchors.leftMargin: appTheme.spaceLg
                    anchors.rightMargin: appTheme.spaceLg
                    spacing: appTheme.spaceSm

                    DialogActionButton {
                        objectName: "applyPresetOpenFileButton"
                        buttonWidth: contentItem.implicitWidth + 2 * appTheme.spaceLg
                        buttonHeight: appTheme.iconButtonHitSizeCompact
                        buttonRadius: appTheme.controlRadiusSmall
                        font.weight: appTheme.fontWeightRegular
                        text: qsTr("Open File...")
                        enabled: !!dialog.presets
                        onClicked: openFileDialog.open()
                    }

                    Item {
                        Layout.fillWidth: true
                    }

                    DialogActionButton {
                        objectName: "applyPresetCancelButton"
                        buttonWidth: appTheme.spaceXl * 5
                        buttonHeight: appTheme.iconButtonHitSizeCompact
                        buttonRadius: appTheme.controlRadiusSmall
                        font.weight: appTheme.fontWeightRegular
                        text: qsTr("Cancel")
                        onClicked: dialog.close()
                    }

                    DialogActionButton {
                        objectName: "applyPresetApplyButton"
                        kind: "accent"
                        buttonWidth: appTheme.spaceXl * 5
                        buttonHeight: appTheme.iconButtonHitSizeCompact
                        buttonRadius: appTheme.controlRadiusSmall
                        font.weight: appTheme.fontWeightRegular
                        text: qsTr("Apply")
                        enabled: dialog.selectedAvailable
                        onClicked: dialog.applySelected()
                    }
                }
            }
        }
    }

    // One-time apply (R8): the file makes the Versions and does not enter the folder.
    FileDialog {
        id: openFileDialog
        title: qsTr("Apply Preset File")
        fileMode: FileDialog.OpenFile
        nameFilters: [qsTr("Alcedo presets (*.alcpreset)"), qsTr("All Files (*)")]
        onAccepted: {
            if (dialog.presets)
                dialog.finish(dialog.presets.ApplyFileToTargets(selectedFile.toString(),
                                                                dialog.targets))
        }
    }
}
