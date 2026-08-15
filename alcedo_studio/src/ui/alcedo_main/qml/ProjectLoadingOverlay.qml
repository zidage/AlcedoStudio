import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Project loading overlay shown while a project is opening or launching.
// `wanted` snaps the overlay on immediately (no fade-in — that flashed the
// empty library after Welcome closed). After load it holds, then fades out
// while the library grid plays its first reveal.
Item {
    id: root
    property var theme: null
    property var host: null
    property Item blurSource: null
    property bool wanted: false
    anchors.fill: parent
    visible: overlayShown
    opacity: 0

    readonly property int fadeOutMs: appTheme.reduceMotion ? 0 : appTheme.motionFoldCloseMs
    readonly property int revealHoldMs: appTheme.reduceMotion ? 0 : appTheme.motionFadeMs
    property bool overlayShown: false

    onWantedChanged: {
        if (wanted) {
            hideSequence.stop()
            opacity = 1
            overlayShown = true
        } else if (overlayShown) {
            hideSequence.restart()
        }
    }

    SequentialAnimation {
        id: hideSequence
        PauseAnimation { duration: root.revealHoldMs }
        ScriptAction {
            script: {
                if (root.host && root.host.revealLibraryAfterProjectLoad)
                    root.host.revealLibraryAfterProjectLoad()
            }
        }
        NumberAnimation {
            target: root
            property: "opacity"
            to: 0
            duration: root.fadeOutMs
            easing.type: Easing.OutCubic
        }
        ScriptAction {
            script: {
                if (!root.wanted && root.opacity <= 0) {
                    root.overlayShown = false
                }
            }
        }
    }

    BlurredOverlay {
        anchors.fill: parent
        blurSource: root.blurSource
        overlayColor: root.theme ? root.theme.colOverlay : Qt.rgba(0, 0, 0, 0.5)

        Rectangle {
            anchors.centerIn: parent
            width: Math.min(parent.width - 36, 420)
            height: projectLoadingContent.implicitHeight + 36
            radius: 14
            color: root.theme ? root.theme.colBgDeep : "#0C0D0F"
            border.width: 0

            ColumnLayout {
                id: projectLoadingContent
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.top: parent.top
                anchors.margins: 20
                spacing: 16

                Label {
                    text: qsTr("Loading Project")
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
                    running: root.host && root.host.projectLoadingOverlayVisible
                }

                Label {
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                    horizontalAlignment: Text.AlignHCenter
                    text: appModules.project.projectLoadingMessage.length > 0
                          ? appModules.project.projectLoadingMessage
                          : qsTr("Preparing library...")
                    color: root.theme ? root.theme.colTextMuted : appTheme.textMutedColor
                    font.pixelSize: 12
                }
            }
        }
    }
}