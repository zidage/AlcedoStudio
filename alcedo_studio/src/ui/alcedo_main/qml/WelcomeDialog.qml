import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Dialogs
import QtQuick.Layouts
import QtQuick.Effects

Dialog {
    id: dialog
    font.family: appTheme.uiFontFamily

    parent: Overlay.overlay
    modal: true
    focus: visible
    closePolicy: Popup.NoAutoClose
    padding: 0
    width: parent ? parent.width : 0
    height: parent ? parent.height : 0
    x: 0
    y: 0

    property Item blurSource: null
    property var recentProjects: []
    property var languageOptions: []
    property var acceleratorOptions: []
    property int currentLanguageIndex: 0
    property string currentAcceleratorBackend: ""
    property string acceleratorWarning: ""
    property bool acceleratorWarningShown: false
    property string serviceMessage: ""
    property string headlineFontFamily: appTheme.headlineFontFamily
    readonly property string dataFontFamily: appTheme.dataFontFamily
    property color primaryAccent: "#6D93B7"
    property color secondaryAccent: "#9FC7D8"
    property color textColor: "#F5F1EA"
    property color mutedTextColor: "#B6B0A7"
    property color panelColor: "#1C1C1D"
    property color panelBorderColor: Qt.rgba(1, 1, 1, 0.08)
    property color overlayColor: Qt.rgba(11 / 255, 12 / 255, 14 / 255, 0.60)
    property color baseColor: "#111214"
    property color exitColor: "#D3D0CB"
    // Match the host window's corner radius so the modal backdrop doesn't bleed past rounded corners.
    property real cornerRadius: 0
    property bool showAllRecent: false
    property int currentPage: 0
    property string projectName: qsTr("Untitled Project")
    property string storageLocation: ""
    readonly property int collapsedRecentCount: 4

    signal loadRequested()
    signal createRequested(string projectName, string storageLocation)
    signal exitRequested()
    signal languageRequested(string languageCode)
    signal acceleratorRequested(string backend)
    signal acceleratorWarningAcknowledged()
    signal recentProjectRequested(string projectPath)

    onVisibleChanged: {
        if (visible) {
            showAllRecent = false
            currentPage = 0
            pager.currentIndex = 0
            if (projectName.length === 0) {
                projectName = qsTr("Untitled Project")
            }
            maybeShowAcceleratorWarning()
        }
    }

    onAcceleratorWarningChanged: {
        if (visible) {
            maybeShowAcceleratorWarning()
        }
    }

    FolderDialog {
        id: projectFolderDialog
        title: qsTr("Select Project Storage Location")
        onAccepted: dialog.storageLocation = selectedFolder.toString()
    }

    Dialog {
        id: acceleratorWarningDialog
        parent: Overlay.overlay
        modal: true
        focus: true
        closePolicy: Popup.CloseOnEscape
        standardButtons: Dialog.Ok
        title: qsTr("CUDA unavailable")
        width: Math.min((parent ? parent.width : 560) - 72, 520)
        x: parent ? Math.round((parent.width - width) / 2) : 0
        y: parent ? Math.round((parent.height - height) / 2) : 0
        onClosed: dialog.acceleratorWarningAcknowledged()

        Overlay.modal: Item {
            anchors.fill: parent

            Rectangle {
                id: warningBackdropMask
                anchors.fill: parent
                radius: dialog.cornerRadius
                color: "white"
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
                    maskSource: warningBackdropMask
                }

                MultiEffect {
                    anchors.fill: parent
                    source: dialog.blurSource
                    blurEnabled: true
                    blur: 0.72
                    blurMax: 72
                    saturation: -0.24
                    brightness: -0.10
                }

                Rectangle {
                    anchors.fill: parent
                    color: Qt.rgba(11 / 255, 12 / 255, 14 / 255, 0.66)
                }
            }

            MouseArea {
                anchors.fill: parent
                hoverEnabled: true
            }
        }

        background: Rectangle {
            radius: 14
            color: Qt.rgba(dialog.panelColor.r, dialog.panelColor.g, dialog.panelColor.b, 0.98)
            border.width: 1
            border.color: dialog.panelBorderColor
        }

        contentItem: Label {
            text: dialog.acceleratorWarning
            wrapMode: Text.WordWrap
            color: dialog.textColor
            font.family: dialog.font.family
            font.pixelSize: 14
            lineHeight: 1.2
        }
    }

    function acceleratorIndexForValue(value) {
        for (let i = 0; i < acceleratorOptions.length; ++i) {
            if (acceleratorOptions[i].value === value) {
                return i
            }
        }
        return acceleratorOptions.length > 0 ? 0 : -1
    }

    function maybeShowAcceleratorWarning() {
        if (acceleratorWarning.length > 0 && !acceleratorWarningShown) {
            acceleratorWarningShown = true
            Qt.callLater(function() {
                if (dialog.visible && acceleratorWarning.length > 0) {
                    acceleratorWarningDialog.open()
                }
            })
        }
    }

    function relativeTimeLabel(lastOpenedMs) {
        const value = Number(lastOpenedMs)
        if (!isFinite(value) || value <= 0) {
            return qsTr("Opened recently")
        }

        const deltaMinutes = Math.max(0, Math.floor((Date.now() - value) / 60000))
        if (deltaMinutes < 1) {
            return qsTr("Opened just now")
        }
        if (deltaMinutes < 60) {
            return qsTr("Opened %n minute(s) ago", "", deltaMinutes)
        }

        const deltaHours = Math.floor(deltaMinutes / 60)
        if (deltaHours < 24) {
            return qsTr("Opened %n hour(s) ago", "", deltaHours)
        }

        const deltaDays = Math.floor(deltaHours / 24)
        if (deltaDays === 1) {
            return qsTr("Opened yesterday")
        }
        if (deltaDays < 7) {
            return qsTr("Opened %n day(s) ago", "", deltaDays)
        }
        if (deltaDays < 14) {
            return qsTr("Opened last week")
        }
        return qsTr("Opened %n day(s) ago", "", deltaDays)
    }

    Overlay.modal: Item {
        anchors.fill: parent

        // Hidden rounded-rect used as the alpha mask for the entire backdrop
        // (blur + dim). White pixels keep, transparent pixels cut away.
        Rectangle {
            id: backdropMask
            anchors.fill: parent
            radius: dialog.cornerRadius
            color: "white"
            visible: false
            layer.enabled: true
            layer.smooth: true
        }

        // Wrap blur + dim in a layer-rendered Item, then mask the layer.
        // This reliably clips both effects to the rounded shape.
        Item {
            id: maskedBackdrop
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
                blurEnabled: true
                blur: 0.72
                blurMax: 72
                saturation: -0.24
                brightness: -0.08
            }

            Rectangle {
                anchors.fill: parent
                color: dialog.overlayColor
            }
        }

        MouseArea {
            anchors.fill: parent
            hoverEnabled: true
        }
    }

    background: Rectangle {
        radius: 0
        color: "transparent"
    }

    contentItem: Item {
        implicitWidth: dialog.width
        implicitHeight: dialog.height

        Rectangle {
            id: shell
            anchors.centerIn: parent
            width: Math.min(parent.width - 56, 980)
            height: Math.min(parent.height - 72, 560)
            radius: 22
            color: Qt.rgba(dialog.panelColor.r, dialog.panelColor.g, dialog.panelColor.b, 0.90)
            border.width: 1
            border.color: dialog.panelBorderColor

            SwipeView {
                id: pager
                anchors.fill: parent
                interactive: false
                clip: true
                currentIndex: dialog.currentPage
                onCurrentIndexChanged: dialog.currentPage = currentIndex

                Item {
                    RowLayout {
                        anchors.fill: parent
                        anchors.margins: 22
                        spacing: 20

                        Item {
                            Layout.fillWidth: true
                            Layout.fillHeight: true
                            Layout.preferredWidth: shell.width * 0.44

                            ColumnLayout {
                                anchors.fill: parent
                                spacing: 18

                                ColumnLayout {
                                    Layout.fillWidth: true
                                    spacing: 0

                                    Row {
                                        spacing: 10

                                        Label {
                                            text: qsTr("Alcedo")
                                            color: dialog.primaryAccent
                                            font.family: dialog.headlineFontFamily
                                            font.pixelSize: 44
                                            font.weight: 800
                                        }

                                        Label {
                                            text: qsTr("Studio")
                                            color: dialog.textColor
                                            font.family: dialog.headlineFontFamily
                                            font.pixelSize: 44
                                            font.weight: 800
                                        }
                                    }
                                }

                                Item {
                                    Layout.preferredHeight: 2
                                }

                                Button {
                                    id: loadButton
                                    Layout.fillWidth: true
                                    Layout.preferredHeight: 64
                                    text: qsTr("Load Project")
                                    icon.source: "qrc:/panel_icons/import.svg"
                                    icon.width: 22
                                    icon.height: 22
                                    icon.color: dialog.textColor
                                    display: AbstractButton.TextBesideIcon
                                    font.family: dialog.font.family
                                    font.pixelSize: 23
                                    font.weight: 700
                                    leftPadding: 24
                                    rightPadding: 24
                                    spacing: 14
                                    Material.foreground: dialog.textColor
                                    onClicked: dialog.loadRequested()

                                    background: Rectangle {
                                        radius: 20
                                        color: loadButton.down
                                               ? Qt.darker(dialog.primaryAccent, 1.18)
                                               : (loadButton.hovered
                                                  ? Qt.lighter(dialog.primaryAccent, 1.06)
                                                  : dialog.primaryAccent)
                                        border.width: 1
                                        border.color: Qt.rgba(dialog.secondaryAccent.r, dialog.secondaryAccent.g, dialog.secondaryAccent.b, 0.16)
                                    }

                                    contentItem: RowLayout {
                                        spacing: loadButton.spacing

                                        Item {
                                            Layout.preferredWidth: 30
                                            Layout.preferredHeight: 30

                                            Image {
                                                id: loadIconSource
                                                anchors.centerIn: parent
                                                width: loadButton.icon.width
                                                height: loadButton.icon.height
                                                source: loadButton.icon.source
                                                visible: false
                                                asynchronous: true
                                            }

                                            MultiEffect {
                                                anchors.fill: loadIconSource
                                                source: loadIconSource
                                                colorization: 1.0
                                                colorizationColor: loadButton.icon.color
                                            }
                                        }

                                        Label {
                                            Layout.fillWidth: true
                                            text: loadButton.text
                                            color: dialog.textColor
                                            font: loadButton.font
                                            verticalAlignment: Text.AlignVCenter
                                        }

                                    }
                                }

                                Button {
                                    id: createButton
                                    Layout.fillWidth: true
                                    Layout.preferredHeight: 64
                                    text: qsTr("Create Project")
                                    display: AbstractButton.TextBesideIcon
                                    font.family: dialog.font.family
                                    font.pixelSize: 23
                                    font.weight: 700
                                    leftPadding: 24
                                    rightPadding: 24
                                    spacing: 14
                                    Material.foreground: dialog.textColor
                                    onClicked: pager.currentIndex = 1

                                    background: Rectangle {
                                        radius: 20
                                        color: createButton.down
                                               ? Qt.rgba(1, 1, 1, 0.14)
                                               : (createButton.hovered
                                                  ? Qt.rgba(1, 1, 1, 0.10)
                                                  : Qt.rgba(dialog.panelColor.r, dialog.panelColor.g, dialog.panelColor.b, 0.80))
                                        border.width: 1
                                        border.color: Qt.rgba(dialog.textColor.r, dialog.textColor.g, dialog.textColor.b, 0.16)
                                    }

                                    contentItem: RowLayout {
                                        spacing: createButton.spacing

                                        Rectangle {
                                            Layout.preferredWidth: 30
                                            Layout.preferredHeight: 30
                                            radius: 15
                                            color: Qt.rgba(dialog.secondaryAccent.r, dialog.secondaryAccent.g, dialog.secondaryAccent.b, 0.90)

                                            Label {
                                                anchors.centerIn: parent
                                                text: "\u2192"
                                                color: dialog.baseColor
                                                font.family: dialog.headlineFontFamily
                                                font.pixelSize: 22
                                                font.weight: 800
                                            }
                                        }

                                        Label {
                                            Layout.fillWidth: true
                                            text: createButton.text
                                            color: dialog.textColor
                                            font: createButton.font
                                            verticalAlignment: Text.AlignVCenter
                                        }
                                    }
                                }

                                Label {
                                    Layout.fillWidth: true
                                    visible: dialog.serviceMessage.length > 0
                                    text: dialog.serviceMessage
                                    wrapMode: Text.WordWrap
                                    color: dialog.mutedTextColor
                                    font.family: dialog.font.family
                                    font.pixelSize: 13
                                    lineHeight: 1.2
                                }

                                ColumnLayout {
                                    Layout.fillWidth: true
                                    spacing: 8
                                    visible: dialog.acceleratorOptions.length > 0

                                    Label {
                                        Layout.fillWidth: true
                                        text: qsTr("Acceleration")
                                        color: dialog.mutedTextColor
                                        font.family: dialog.font.family
                                        font.pixelSize: 12
                                        font.weight: 700
                                    }

                                    ComboBox {
                                        id: acceleratorCombo
                                        Layout.fillWidth: true
                                        Layout.preferredHeight: 42
                                        model: dialog.acceleratorOptions
                                        textRole: "label"
                                        valueRole: "value"
                                        currentIndex: dialog.acceleratorIndexForValue(
                                                          dialog.currentAcceleratorBackend)
                                        font.family: dialog.font.family
                                        font.pixelSize: 14
                                        onActivated: function(index) {
                                            const item = dialog.acceleratorOptions[index]
                                            if (item) {
                                                dialog.acceleratorRequested(item.value)
                                            }
                                        }
                                        Material.background: Qt.rgba(dialog.panelColor.r, dialog.panelColor.g, dialog.panelColor.b, 0.92)
                                        Material.foreground: dialog.textColor
                                    }
                                }

                                Item {
                                    Layout.fillHeight: true
                                }

                                RowLayout {
                                    Layout.fillWidth: true
                                    spacing: 12

                                    ComboBox {
                                        id: languageCombo
                                        Layout.preferredWidth: 168
                                        model: dialog.languageOptions
                                        textRole: "label"
                                        currentIndex: dialog.currentLanguageIndex
                                        font.family: dialog.font.family
                                        onActivated: function(index) {
                                            const item = model[index]
                                            if (item) {
                                                dialog.languageRequested(item.code)
                                            }
                                        }
                                        Material.background: Qt.rgba(dialog.panelColor.r, dialog.panelColor.g, dialog.panelColor.b, 0.92)
                                        Material.foreground: dialog.textColor
                                    }

                                    Item {
                                        Layout.fillWidth: true
                                    }

                                    Button {
                                        id: exitButton
                                        text: qsTr("Exit Application")
                                        flat: true
                                        font.family: dialog.font.family
                                        font.pixelSize: 16
                                        font.weight: 600
                                        Material.foreground: exitButton.hovered
                                                             ? dialog.textColor
                                                             : dialog.exitColor
                                        onClicked: dialog.exitRequested()

                                        contentItem: RowLayout {
                                            spacing: 8

                                            Label {
                                                text: "\u21AA"
                                                color: exitButton.Material.foreground
                                                font.pixelSize: 18
                                            }

                                            Label {
                                                text: exitButton.text
                                                color: exitButton.Material.foreground
                                                font: exitButton.font
                                            }
                                        }
                                    }
                                }
                            }
                        }

                        Rectangle {
                            Layout.fillWidth: true
                            Layout.fillHeight: true
                            Layout.preferredWidth: shell.width * 0.50
                            radius: 18
                            color: Qt.rgba(0, 0, 0, 0.12)
                            border.width: 1
                            border.color: Qt.rgba(1, 1, 1, 0.04)

                            ColumnLayout {
                                anchors.fill: parent
                                anchors.margins: 18
                                spacing: 14

                                RowLayout {
                                    Layout.fillWidth: true

                                    Label {
                                        text: qsTr("Recent Projects")
                                        color: dialog.textColor
                                        font.family: dialog.font.family
                                        font.pixelSize: 24
                                        font.weight: 800
                                    }

                                    Item {
                                        Layout.fillWidth: true
                                    }

                                    Button {
                                        visible: dialog.recentProjects.length > dialog.collapsedRecentCount
                                        flat: true
                                        text: dialog.showAllRecent ? qsTr("Collapse") : qsTr("View All")
                                        font.family: dialog.font.family
                                        font.pixelSize: 14
                                        font.weight: 600
                                        Material.foreground: dialog.primaryAccent
                                        onClicked: dialog.showAllRecent = !dialog.showAllRecent
                                    }
                                }

                                Loader {
                                    Layout.fillWidth: true
                                    active: dialog.recentProjects.length === 0
                                    visible: active
                                    Layout.fillHeight: active

                                    sourceComponent: Item {
                                        Column {
                                            anchors.top: parent.top
                                            anchors.left: parent.left
                                            anchors.right: parent.right
                                            spacing: 8

                                            Label {
                                                text: qsTr("No recent projects yet")
                                                color: dialog.textColor
                                                font.family: dialog.font.family
                                                font.pixelSize: 20
                                                font.weight: 700
                                            }

                                            Label {
                                                text: qsTr("Projects you open or create here will appear in this list.")
                                                color: dialog.mutedTextColor
                                                font.family: dialog.font.family
                                                font.pixelSize: 14
                                                wrapMode: Text.WordWrap
                                            }
                                        }
                                    }
                                }

                                ListView {
                                    id: recentList
                                    Layout.fillWidth: true
                                    visible: dialog.recentProjects.length > 0
                                    Layout.fillHeight: visible
                                    Layout.alignment: Qt.AlignTop
                                    clip: true
                                    boundsBehavior: Flickable.StopAtBounds
                                    spacing: 12
                                    model: dialog.showAllRecent
                                           ? dialog.recentProjects
                                           : dialog.recentProjects.slice(0, dialog.collapsedRecentCount)

                                    delegate: Item {
                                        required property string name
                                        required property string path
                                        required property string folderPath
                                        required property double lastOpenedMs

                                        width: ListView.view.width
                                        height: 72
                                        readonly property bool hovered: rowMouse.containsMouse

                                        Rectangle {
                                            anchors.fill: parent
                                            radius: 18
                                            color: hovered
                                                   ? Qt.rgba(1, 1, 1, 0.055)
                                                   : Qt.rgba(0, 0, 0, 0.22)
                                            border.width: 1
                                            border.color: hovered
                                                          ? Qt.rgba(dialog.textColor.r, dialog.textColor.g, dialog.textColor.b, 0.08)
                                                          : Qt.rgba(1, 1, 1, 0.04)

                                            RowLayout {
                                                anchors.fill: parent
                                                anchors.leftMargin: 18
                                                anchors.rightMargin: 18
                                                spacing: 14

                                                Image {
                                                    Layout.preferredWidth: 24
                                                    Layout.preferredHeight: 24
                                                    source: "qrc:/panel_icons/project-file.svg"
                                                    sourceSize.width: 24
                                                    sourceSize.height: 24
                                                    asynchronous: true
                                                    opacity: rowMouse.containsMouse ? 0.96 : 0.72
                                                }

                                                ColumnLayout {
                                                    Layout.fillWidth: true
                                                    spacing: 2

                                                    Label {
                                                        Layout.fillWidth: true
                                                        text: name
                                                        elide: Text.ElideRight
                                                        color: dialog.textColor
                                                        font.family: dialog.dataFontFamily
                                                        font.pixelSize: 17
                                                        font.weight: 700
                                                    }

                                                    Label {
                                                        Layout.fillWidth: true
                                                        text: dialog.relativeTimeLabel(lastOpenedMs)
                                                        color: dialog.mutedTextColor
                                                        font.family: dialog.dataFontFamily
                                                        font.pixelSize: 12
                                                        elide: Text.ElideRight
                                                    }

                                                    Label {
                                                        Layout.fillWidth: true
                                                        text: folderPath
                                                        color: Qt.rgba(dialog.mutedTextColor.r, dialog.mutedTextColor.g, dialog.mutedTextColor.b, 0.74)
                                                        font.family: dialog.dataFontFamily
                                                        font.pixelSize: 11
                                                        elide: Text.ElideMiddle
                                                    }
                                                }
                                            }
                                        }

                                        MouseArea {
                                            id: rowMouse
                                            anchors.fill: parent
                                            hoverEnabled: true
                                            cursorShape: Qt.PointingHandCursor
                                            onClicked: dialog.recentProjectRequested(path)
                                        }
                                    }
                                }
                            }
                        }
                    }
                }

                Item {
                    ColumnLayout {
                        anchors.fill: parent
                        spacing: 0

                        Rectangle {
                            Layout.fillWidth: true
                            Layout.preferredHeight: 126
                            color: "transparent"

                            RowLayout {
                                anchors.fill: parent
                                anchors.leftMargin: 36
                                anchors.rightMargin: 36
                                spacing: 18

                                Button {
                                    id: headerBackButton
                                    Layout.preferredWidth: 42
                                    Layout.preferredHeight: 42
                                    flat: true
                                    text: "\u2190"
                                    font.family: dialog.headlineFontFamily
                                    font.pixelSize: 31
                                    Material.foreground: headerBackButton.hovered ? dialog.textColor : dialog.mutedTextColor
                                    onClicked: pager.currentIndex = 0
                                }

                                ColumnLayout {
                                    Layout.fillWidth: true
                                    spacing: 6

                                    Label {
                                        text: qsTr("New Project")
                                        color: dialog.textColor
                                        font.family: dialog.headlineFontFamily
                                        font.pixelSize: 32
                                        font.weight: 800
                                    }

                                    Label {
                                        text: qsTr("Configure your workspace settings.")
                                        color: dialog.mutedTextColor
                                        font.family: dialog.font.family
                                        font.pixelSize: 18
                                    }
                                }
                            }
                        }

                        Rectangle {
                            Layout.fillWidth: true
                            Layout.preferredHeight: 1
                            color: Qt.rgba(dialog.textColor.r, dialog.textColor.g, dialog.textColor.b, 0.08)
                        }

                        ColumnLayout {
                            Layout.fillWidth: true
                            Layout.fillHeight: true
                            Layout.leftMargin: 36
                            Layout.rightMargin: 36
                            Layout.topMargin: 34
                            spacing: 24

                            ColumnLayout {
                                Layout.fillWidth: true
                                spacing: 12

                                Label {
                                    text: qsTr("Project Name")
                                    color: dialog.textColor
                                    font.family: dialog.font.family
                                    font.pixelSize: 18
                                    font.weight: 700
                                }

                                TextField {
                                    id: projectNameField
                                    Layout.fillWidth: true
                                    Layout.preferredHeight: 56
                                    text: dialog.projectName
                                    selectByMouse: true
                                    font.family: dialog.dataFontFamily
                                    font.pixelSize: 19
                                    color: dialog.textColor
                                    onTextChanged: dialog.projectName = text
                                    Material.foreground: dialog.textColor
                                    Material.accent: dialog.primaryAccent
                                    background: Rectangle {
                                        radius: 10
                                        color: Qt.rgba(1, 1, 1, 0.10)
                                        border.width: 1
                                        border.color: projectNameField.activeFocus
                                                      ? Qt.rgba(dialog.primaryAccent.r, dialog.primaryAccent.g, dialog.primaryAccent.b, 0.62)
                                                      : Qt.rgba(dialog.textColor.r, dialog.textColor.g, dialog.textColor.b, 0.12)
                                    }
                                }
                            }

                            ColumnLayout {
                                Layout.fillWidth: true
                                spacing: 12

                                Label {
                                    text: qsTr("Storage Location")
                                    color: dialog.textColor
                                    font.family: dialog.font.family
                                    font.pixelSize: 18
                                    font.weight: 700
                                }

                                RowLayout {
                                    Layout.fillWidth: true
                                    spacing: 16

                                    Rectangle {
                                        id: storageLocationField
                                        Layout.fillWidth: true
                                        Layout.preferredHeight: 56
                                        radius: 10
                                        color: Qt.rgba(1, 1, 1, 0.10)
                                        border.width: 1
                                        border.color: Qt.rgba(dialog.textColor.r, dialog.textColor.g, dialog.textColor.b, 0.12)

                                        Label {
                                            anchors.fill: parent
                                            anchors.leftMargin: 16
                                            anchors.rightMargin: 16
                                            text: dialog.storageLocation.length > 0
                                                  ? dialog.storageLocation
                                                  : qsTr("Select a parent folder...")
                                            elide: Text.ElideMiddle
                                            verticalAlignment: Text.AlignVCenter
                                            color: dialog.storageLocation.length > 0
                                                   ? dialog.textColor
                                                   : dialog.mutedTextColor
                                            font.family: dialog.dataFontFamily
                                            font.pixelSize: 19
                                        }
                                    }

                                    Rectangle {
                                        id: browseButton
                                        Layout.preferredWidth: 56
                                        Layout.preferredHeight: 56
                                        radius: 10
                                        color: browseMouse.pressed
                                               ? Qt.rgba(1, 1, 1, 0.06)
                                               : (browseMouse.containsMouse
                                                  ? Qt.rgba(1, 1, 1, 0.12)
                                                  : Qt.rgba(1, 1, 1, 0.07))
                                        border.width: 1
                                        border.color: Qt.rgba(dialog.textColor.r, dialog.textColor.g, dialog.textColor.b, 0.14)

                                        Image {
                                            anchors.centerIn: parent
                                            width: 24
                                            height: 24
                                            source: "qrc:/panel_icons/folder-open.svg"
                                            sourceSize.width: 24
                                            sourceSize.height: 24
                                            asynchronous: true
                                        }

                                        MouseArea {
                                            id: browseMouse
                                            anchors.fill: parent
                                            hoverEnabled: true
                                            cursorShape: Qt.PointingHandCursor
                                            onClicked: projectFolderDialog.open()
                                        }
                                    }
                                }
                            }

                            Label {
                                Layout.fillWidth: true
                                visible: dialog.serviceMessage.length > 0
                                text: dialog.serviceMessage
                                wrapMode: Text.WordWrap
                                color: dialog.mutedTextColor
                                font.family: dialog.font.family
                                font.pixelSize: 13
                            }
                        }

                        Rectangle {
                            Layout.fillWidth: true
                            Layout.preferredHeight: 1
                            color: Qt.rgba(dialog.textColor.r, dialog.textColor.g, dialog.textColor.b, 0.08)
                        }

                        RowLayout {
                            Layout.fillWidth: true
                            Layout.preferredHeight: 104
                            Layout.leftMargin: 36
                            Layout.rightMargin: 36
                            spacing: 18

                            Item {
                                Layout.fillWidth: true
                            }

                            Button {
                                id: submitCreateButton
                                Layout.preferredWidth: 230
                                Layout.preferredHeight: 58
                                topPadding: 0
                                bottomPadding: 0
                                leftPadding: 0
                                rightPadding: 0
                                enabled: dialog.projectName.trim().length > 0
                                         && dialog.storageLocation.length > 0
                                text: qsTr("Create Project")
                                font.family: dialog.font.family
                                font.pixelSize: 17
                                font.weight: 800
                                Material.foreground: dialog.textColor
                                onClicked: dialog.createRequested(dialog.projectName.trim(),
                                                                  dialog.storageLocation)
                                background: Rectangle {
                                    radius: 10
                                    color: submitCreateButton.enabled
                                           ? (submitCreateButton.down
                                              ? Qt.darker(dialog.primaryAccent, 1.16)
                                              : (submitCreateButton.hovered
                                                 ? Qt.lighter(dialog.primaryAccent, 1.06)
                                                 : dialog.primaryAccent))
                                           : Qt.rgba(dialog.primaryAccent.r, dialog.primaryAccent.g, dialog.primaryAccent.b, 0.42)
                                    border.width: 1
                                    border.color: Qt.rgba(dialog.secondaryAccent.r, dialog.secondaryAccent.g, dialog.secondaryAccent.b, 0.16)
                                }

                                contentItem: Label {
                                    text: submitCreateButton.text
                                    horizontalAlignment: Text.AlignHCenter
                                    verticalAlignment: Text.AlignVCenter
                                    color: submitCreateButton.Material.foreground
                                    font: submitCreateButton.font
                                }
                            }
                        }
                    }
                }
            }

        }
    }
}
