import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Effects

Popup {
    id: root
    font.family: appTheme.uiFontFamily
    modal: true
    focus: true
    visible: recoveryActive
    closePolicy: Popup.NoAutoClose
    anchors.centerIn: parent
    width: Math.min(parent ? parent.width - 64 : 760, 760)
    height: Math.min(parent ? parent.height - 64 : 740, 740)
    padding: 0

    property bool recoveryActive: false
    property bool recoveryBusy: false
    property bool showImportProgress: false
    property int importCompleted: 0
    property int importTotal: 0
    property int importFailed: 0
    property string recoveryPhase: ""
    property string recoveryStatus: ""
    property string converterPath: ""
    property bool converterPathFromDefault: false
    property var unsupportedFiles: []
    property Item backgroundSource: null

    signal browseRequested()
    signal convertRequested()
    signal exitRequested()

    readonly property color modalColor: appTheme.bgPanelColor
    readonly property color raisedColor: appTheme.bgDeepColor
    readonly property color inputColor: appTheme.bgBaseColor
    readonly property color hoverColor: appTheme.hoverColor
    readonly property color accentColor: appTheme.accentColor
    readonly property color accentHoverColor: appTheme.accentSecondaryColor
    readonly property color grayButtonColor: appTheme.bgBaseColor
    readonly property color grayButtonHoverColor: appTheme.hoverColor
    readonly property color buttonTextColor: "#FFFFFF"
    readonly property color textColor: appTheme.textColor
    readonly property color mutedTextColor: appTheme.textMutedColor
    readonly property color dividerColor: appTheme.dividerColor
    readonly property color strokeColor: appTheme.glassStrokeColor
    readonly property bool showBusyIndicator: root.recoveryBusy && !root.showImportProgress
    readonly property bool macOsUsesFixedConverterPath: Qt.platform.os === "osx"
    readonly property bool converterDetected: root.converterPath.length > 0
    readonly property int converterRowHeight: 34

    function directoryName(path) {
        if (!path || path.length === 0) {
            return qsTr("Unknown directory")
        }
        var normalized = String(path).replace(/\\/g, "/")
        var index = normalized.lastIndexOf("/")
        if (index <= 0) {
            return normalized
        }
        return normalized.substring(0, index)
    }

    Overlay.modal: Item {
        anchors.fill: parent

        MultiEffect {
            visible: root.backgroundSource !== null
            anchors.fill: parent
            source: root.backgroundSource
            blurEnabled: true
            blur: 0.68
            blurMax: 64
            saturation: -0.28
        }

        Rectangle {
            anchors.fill: parent
            color: appTheme.overlayColor
        }

        MouseArea { anchors.fill: parent; hoverEnabled: true }
    }

    background: Rectangle {
        radius: appTheme.panelRadius + 2
        color: root.modalColor
        border.width: 1
        border.color: root.strokeColor
    }

    contentItem: ColumnLayout {
        spacing: 0

        ColumnLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.margins: 26
            Layout.bottomMargin: 22
            spacing: 22

            RowLayout {
                Layout.fillWidth: true
                spacing: 14

                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: 8

                    Label {
                        Layout.fillWidth: true
                        text: qsTr("Nikon High Efficiency RAW conversion required")
                        color: root.textColor
                        font.family: appTheme.headlineFontFamily
                        font.pixelSize: 24
                        font.weight: 800
                        lineHeight: 1.06
                        wrapMode: Text.WordWrap
                    }

                    Label {
                        visible: root.recoveryStatus.length > 0 || root.showBusyIndicator
                        Layout.fillWidth: true
                        text: root.recoveryStatus.length > 0
                              ? root.recoveryStatus
                              : qsTr("Preparing recovery workflow...")
                        color: root.mutedTextColor
                        font.family: appTheme.uiFontFamily
                        font.pixelSize: 13
                        font.weight: 600
                        lineHeight: 1.25
                        wrapMode: Text.WordWrap
                    }
                }

                BusyIndicator {
                    running: root.showBusyIndicator
                    visible: root.showBusyIndicator
                    implicitWidth: 30
                    implicitHeight: 30
                }
            }

            ColumnLayout {
                Layout.fillWidth: true
                spacing: 8

                Label {
                    text: qsTr("Adobe DNG Converter executable")
                    color: root.textColor
                    font.family: appTheme.uiFontFamily
                    font.pixelSize: 13
                    font.weight: 700
                }

                RowLayout {
                    Layout.fillWidth: true
                    Layout.preferredHeight: Math.max(root.converterRowHeight,
                                                     converterPathLabel.implicitHeight + 14)
                    spacing: 8

                    Rectangle {
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        Layout.preferredHeight: root.converterRowHeight
                        radius: appTheme.panelRadius
                        color: root.inputColor
                        border.width: 1
                        border.color: root.strokeColor

                        Label {
                            id: converterPathLabel
                            anchors.fill: parent
                            anchors.leftMargin: 14
                            anchors.rightMargin: 14
                            verticalAlignment: Text.AlignVCenter
                            text: root.converterPath.length > 0
                                  ? root.converterPath
                                  : (root.macOsUsesFixedConverterPath
                                     ? qsTr("Adobe DNG Converter is not installed at /Applications/Adobe DNG Converter.app.")
                                     : qsTr("No converter selected"))
                            color: root.converterPath.length > 0 ? root.textColor : root.mutedTextColor
                            font.family: appTheme.dataFontFamily
                            font.pixelSize: 12
                            wrapMode: Text.Wrap
                        }
                    }

                    Rectangle {
                        id: browseButton
                        visible: !root.macOsUsesFixedConverterPath
                        Layout.preferredWidth: root.converterRowHeight
                        Layout.preferredHeight: root.converterRowHeight
                        Layout.fillHeight: true
                        radius: appTheme.panelRadius
                        color: browseMouse.pressed ? root.hoverColor : root.inputColor
                        border.width: 1
                        border.color: browseMouse.containsMouse ? root.accentColor : root.strokeColor
                        opacity: root.recoveryBusy ? 0.55 : 1.0

                        Image {
                            anchors.centerIn: parent
                            width: 16
                            height: 16
                            source: "qrc:/panel_icons/folder-open.svg"
                            sourceSize.width: 16
                            sourceSize.height: 16
                            fillMode: Image.PreserveAspectFit
                            mipmap: false
                            smooth: false
                        }

                        MouseArea {
                            id: browseMouse
                            anchors.fill: parent
                            enabled: !root.recoveryBusy
                            hoverEnabled: true
                            cursorShape: Qt.PointingHandCursor
                            onClicked: root.browseRequested()
                        }

                        ToolTip.visible: browseMouse.containsMouse
                        ToolTip.text: qsTr("Browse for Adobe DNG Converter")
                    }
                }

                Rectangle {
                    visible: root.converterPathFromDefault
                    Layout.preferredHeight: 24
                    Layout.preferredWidth: defaultPathBadge.implicitWidth + 18
                    radius: appTheme.panelRadius - 2
                    color: Qt.rgba(root.accentColor.r, root.accentColor.g, root.accentColor.b, 0.22)
                    border.width: 1
                    border.color: Qt.rgba(root.accentColor.r, root.accentColor.g, root.accentColor.b, 0.45)

                    Label {
                        id: defaultPathBadge
                        anchors.centerIn: parent
                        text: qsTr("Loaded from default path")
                        color: root.buttonTextColor
                        font.family: appTheme.uiFontFamily
                        font.pixelSize: 11
                        font.weight: 800
                    }
                }
            }

            ColumnLayout {
                Layout.fillWidth: true
                spacing: 10

                RowLayout {
                    Layout.fillWidth: true
                    spacing: 8

                    Label {
                        text: qsTr("Affected files")
                        color: root.textColor
                        font.family: appTheme.uiFontFamily
                        font.pixelSize: 14
                        font.weight: 800
                    }

                    Label {
                        text: qsTr("(%1 items found)").arg(root.unsupportedFiles.length)
                        color: root.mutedTextColor
                        font.family: appTheme.uiFontFamily
                        font.pixelSize: 12
                        font.weight: 600
                    }
                }

                Rectangle {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    Layout.minimumHeight: 250
                    radius: appTheme.panelRadius
                    color: root.raisedColor
                    border.width: 1
                    border.color: root.dividerColor
                    clip: true

                    ColumnLayout {
                        anchors.fill: parent
                        spacing: 0

                        Rectangle {
                            Layout.fillWidth: true
                            Layout.preferredHeight: 42
                            radius: appTheme.panelRadius
                            color: root.inputColor
                            clip: true

                            Rectangle {
                                anchors.left: parent.left
                                anchors.right: parent.right
                                anchors.bottom: parent.bottom
                                height: appTheme.panelRadius
                                color: parent.color
                            }

                            RowLayout {
                                anchors.fill: parent
                                anchors.leftMargin: 18
                                anchors.rightMargin: 18
                                spacing: 16

                                Label {
                                    Layout.preferredWidth: Math.max(180, parent.width * 0.34)
                                    text: qsTr("Filename")
                                    color: root.mutedTextColor
                                    font.family: appTheme.uiFontFamily
                                    font.pixelSize: 12
                                    font.weight: 800
                                }

                                Label {
                                    Layout.fillWidth: true
                                    text: qsTr("Directory")
                                    color: root.mutedTextColor
                                    font.family: appTheme.uiFontFamily
                                    font.pixelSize: 12
                                    font.weight: 800
                                }
                            }
                        }

                        ListView {
                            id: fileList
                            Layout.fillWidth: true
                            Layout.fillHeight: true
                            clip: true
                            model: root.unsupportedFiles
                            boundsBehavior: Flickable.StopAtBounds

                            delegate: Rectangle {
                                id: fileRow
                                required property int index
                                required property var modelData
                                width: fileList.width
                                height: Math.max(40, fileRowLayout.implicitHeight
                                                 + appTheme.spaceXs)
                                color: fileRow.index % 2 === 0 ? Qt.rgba(1, 1, 1, 0.018) : "transparent"

                                Rectangle {
                                    anchors.left: parent.left
                                    anchors.right: parent.right
                                    anchors.bottom: parent.bottom
                                    height: 1
                                    color: root.dividerColor
                                }

                                RowLayout {
                                    id: fileRowLayout
                                    anchors.fill: parent
                                    anchors.leftMargin: 18
                                    anchors.rightMargin: 18
                                    spacing: 16

                                    Label {
                                        Layout.preferredWidth: Math.max(180, parent.width * 0.34)
                                        text: fileRow.modelData.fileName
                                        color: root.textColor
                                        font.family: appTheme.dataFontFamily
                                        font.pixelSize: 12
                                        font.weight: 600
                                        wrapMode: Text.Wrap
                                        verticalAlignment: Text.AlignVCenter
                                    }

                                    Label {
                                        Layout.fillWidth: true
                                        text: root.directoryName(fileRow.modelData.sourcePath)
                                        color: root.mutedTextColor
                                        font.family: appTheme.dataFontFamily
                                        font.pixelSize: 11
                                        wrapMode: Text.Wrap
                                        verticalAlignment: Text.AlignVCenter
                                    }
                                }
                            }

                            ScrollBar.vertical: ScrollBar {
                                policy: ScrollBar.AsNeeded
                                contentItem: Rectangle {
                                    implicitWidth: 5
                                    radius: 3
                                    color: root.accentColor
                                }
                                background: Rectangle {
                                    color: "transparent"
                                }
                            }
                        }
                    }
                }
            }

            ColumnLayout {
                Layout.fillWidth: true
                visible: root.showImportProgress
                spacing: 7

                ProgressBar {
                    Layout.fillWidth: true
                    from: 0
                    to: Math.max(1, root.importTotal)
                    value: root.importCompleted + root.importFailed
                }

                Label {
                    text: qsTr("%1 / %2 converted files reimported, %3 failed")
                          .arg(root.importCompleted)
                          .arg(root.importTotal)
                          .arg(root.importFailed)
                    color: root.mutedTextColor
                    font.family: appTheme.dataFontFamily
                    font.pixelSize: 12
                }
            }
        }

        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: 70
            color: appTheme.bgCanvasColor

            Rectangle {
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.top: parent.top
                height: 1
                color: root.dividerColor
            }

            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: 26
                anchors.rightMargin: 26
                spacing: 12

                Item { Layout.fillWidth: true }

                Button {
                    id: cancelButton
                    enabled: !root.recoveryBusy
                    text: qsTr("Cancel import")
                    implicitWidth: 122
                    implicitHeight: 42
                    font.family: appTheme.uiFontFamily
                    font.pixelSize: 13
                    font.weight: 800
                    onClicked: root.exitRequested()
                    background: Rectangle {
                        radius: appTheme.panelRadius
                        color: cancelButton.down ? Qt.darker(root.grayButtonColor, 1.12)
                                                 : (cancelButton.hovered ? root.grayButtonHoverColor
                                                                         : root.grayButtonColor)
                        border.width: 1
                        border.color: root.strokeColor
                    }
                    contentItem: Label {
                        text: cancelButton.text
                        color: cancelButton.enabled ? root.buttonTextColor : Qt.darker(root.mutedTextColor, 1.25)
                        font: cancelButton.font
                        horizontalAlignment: Text.AlignHCenter
                        verticalAlignment: Text.AlignVCenter
                    }
                }

                Button {
                    id: convertButton
                    text: root.macOsUsesFixedConverterPath
                          ? qsTr("Convert to DNG & Continue")
                          : (root.converterPath.length > 0
                             ? qsTr("Convert to DNG & Continue")
                             : qsTr("Choose converter & Continue"))
                    enabled: !root.recoveryBusy
                             && (!root.macOsUsesFixedConverterPath || root.converterDetected)
                    implicitWidth: 244
                    implicitHeight: 42
                    font.family: appTheme.uiFontFamily
                    font.pixelSize: 13
                    font.weight: 800
                    onClicked: root.convertRequested()
                    background: Rectangle {
                        radius: appTheme.panelRadius
                        color: convertButton.enabled
                               ? (convertButton.down ? appTheme.toneSteel : (convertButton.hovered ? root.accentHoverColor : root.accentColor))
                               : root.inputColor
                    }
                    contentItem: Label {
                        text: convertButton.text
                        color: convertButton.enabled ? root.buttonTextColor : root.mutedTextColor
                        font: convertButton.font
                        horizontalAlignment: Text.AlignHCenter
                        verticalAlignment: Text.AlignVCenter
                    }
                }
            }
        }
    }
}
