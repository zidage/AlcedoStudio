import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Effects

// Welcome surface (DESIGN.md "Welcome surface", welcome overview plan section 3).
// A modal two-column card over the blurred shell:
//   left   - wordmark, description, Open Project…, New Project…; at the bottom
//            the language selector and Quit.
//   right  - "overview": the previewed project (WelcomeProjectOverview) and the
//            other recent projects (WelcomeRecentProjectList);
//            "empty": the first-project well when no recent project exists;
//            "form": the new-project form (WelcomeNewProjectForm).
// The view reads project data only from `adapter` (WelcomeProjectPreviewAdapter)
// and `recentProjects`, and reports every action through a signal. AppDialogs
// routes the signals to appModules.project and the launch controller.
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
    property var adapter: null
    property var recentProjects: []
    property var languageOptions: []
    property int currentLanguageIndex: 0
    property string acceleratorWarning: ""
    property bool acceleratorWarningShown: false
    property string serviceMessage: ""
    property var updateService: null
    // Match the host window's corner radius so the modal backdrop doesn't bleed past rounded corners.
    property real cornerRadius: 0
    // "overview" or "form". The empty layout follows from the adapter state.
    property string rightColumnMode: "overview"
    // serviceMessage reports every project load, including the startup preview.
    // The welcome surface shows it only after the user started an open or create
    // action here: the message then holds the result of that action (for example
    // the error of a failed open). The preview error has its own place.
    property bool launchMessageVisible: false
    readonly property string launchMessage: dialog.launchMessageVisible ? dialog.serviceMessage : ""

    readonly property string previewState: dialog.adapter ? dialog.adapter.state : "empty"
    readonly property bool hasRecentProjects: dialog.previewState !== "empty"
    // One load at a time: rows, Open Project… and New Project… wait for the running load.
    readonly property bool selectionEnabled: dialog.adapter ? dialog.adapter.selectionEnabled : true
    readonly property string previewedPath: dialog.adapter ? dialog.adapter.projectPath : ""
    // The recent list holds every entry except the previewed project.
    readonly property var otherRecentProjects: {
        const recent = dialog.recentProjects || []
        const previewed = dialog.previewedPath
        const others = []
        for (let i = 0; i < recent.length; ++i) {
            if (String(recent[i].path || "") !== previewed)
                others.push(recent[i])
        }
        return others
    }

    signal previewRequested(string projectPath)
    signal continueRequested()
    signal openRequested()
    signal createRequested(string storageLocation, string projectName)
    signal exitRequested()
    signal languageRequested(string languageCode)
    signal acceleratorWarningAcknowledged()

    // The surface closes for an enter-mode launch and opens again when that
    // launch fails, so it keeps the right column mode and the form values.
    onVisibleChanged: {
        if (visible) {
            maybeShowAcceleratorWarning()
        }
    }

    // True from open until the initial control of section 3.7 has focus. At
    // startup Continue Editing is disabled until the preview load starts, so the
    // focus moves to it when the preview state changes.
    property bool initialFocusPending: false

    onOpened: {
        dialog.initialFocusPending = true
        Qt.callLater(dialog.focusInitialControl)
    }
    onRightColumnModeChanged: {
        dialog.initialFocusPending = true
        Qt.callLater(dialog.focusInitialControl)
    }
    onPreviewStateChanged: {
        if (dialog.initialFocusPending)
            Qt.callLater(dialog.focusInitialControl)
    }

    onAcceleratorWarningChanged: {
        if (visible) {
            maybeShowAcceleratorWarning()
        }
    }

    // Section 3.7: Continue Editing when a preview is ready or loading, the
    // project name in the form, New Project… in the empty layout.
    function focusInitialControl() {
        if (!dialog.visible)
            return
        if (dialog.rightColumnMode === "form") {
            dialog.initialFocusPending = false
            newProjectForm.nameField.forceActiveFocus()
            return
        }
        if (!dialog.hasRecentProjects) {
            dialog.initialFocusPending = false
            emptyNewProjectButton.forceActiveFocus()
            return
        }
        if (projectOverview.continueButton.enabled) {
            dialog.initialFocusPending = false
            projectOverview.continueButton.forceActiveFocus()
        } else if (openProjectButton.enabled) {
            openProjectButton.forceActiveFocus()
        }
    }

    function requestPreview(projectPath) {
        dialog.launchMessageVisible = false
        dialog.previewRequested(projectPath)
    }

    function requestContinue() {
        dialog.launchMessageVisible = false
        dialog.continueRequested()
    }

    function requestOpen() {
        dialog.launchMessageVisible = true
        dialog.openRequested()
    }

    function requestCreate(storageLocation, projectName) {
        dialog.launchMessageVisible = true
        dialog.createRequested(storageLocation, projectName)
    }

    function showNewProjectForm() {
        newProjectForm.reset()
        dialog.rightColumnMode = "form"
    }

    Dialog {
        id: acceleratorWarningDialog
        parent: Overlay.overlay
        modal: true
        focus: true
        closePolicy: Popup.CloseOnEscape
        standardButtons: Dialog.Ok
        title: qsTr("CUDA unavailable")
        width: Math.min((parent ? parent.width : appTheme.welcomeCardWidth) - 2 * appTheme.spaceXl,
                        appTheme.welcomeCardWidth / 2)
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
                    blurEnabled: dialog.blurSource !== null
                    blur: 0.6
                    blurMax: 64
                    saturation: -0.2
                    brightness: -0.08
                }

                Rectangle {
                    anchors.fill: parent
                    color: appTheme.overlayColor
                }
            }

            MouseArea {
                anchors.fill: parent
                hoverEnabled: true
            }
        }

        background: Rectangle {
            radius: appTheme.panelRadius
            color: appTheme.cardSurfaceColor
            border.width: 1
            border.color: appTheme.cardBorderColor
        }

        contentItem: Label {
            text: dialog.acceleratorWarning
            wrapMode: Text.WordWrap
            color: appTheme.textColor
            font.family: appTheme.uiFontFamily
            font.pixelSize: appTheme.fontSizeSection
            lineHeight: appTheme.lineHeightSection
            lineHeightMode: Text.FixedHeight
        }
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

    Overlay.modal: Item {
        anchors.fill: parent

        Rectangle {
            id: backdropMask
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
                maskSource: backdropMask
            }

            MultiEffect {
                anchors.fill: parent
                source: dialog.blurSource
                blurEnabled: dialog.blurSource !== null
                blur: 0.6
                blurMax: 64
                saturation: -0.2
                brightness: -0.08
            }

            Rectangle {
                anchors.fill: parent
                color: appTheme.overlayColor
            }
        }

        MouseArea {
            anchors.fill: parent
            hoverEnabled: true
        }
    }

    background: Item {}

    contentItem: Item {
        implicitWidth: dialog.width
        implicitHeight: dialog.height

        Timer {
            interval: 700
            repeat: false
            running: dialog.visible
                     && dialog.updateService
                     && dialog.updateService.enabled
                     && dialog.updateService.unchecked
            onTriggered: dialog.updateService.CheckForUpdates()
        }

        Rectangle {
            id: shell
            objectName: "welcomeCard"
            anchors.centerIn: parent
            width: Math.min(parent.width - 2 * appTheme.spaceXl, appTheme.welcomeCardWidth)
            height: Math.min(parent.height - 2 * appTheme.spaceXl, appTheme.welcomeCardHeight)
            radius: appTheme.panelRadius
            color: appTheme.cardSurfaceColor
            border.width: 1
            border.color: appTheme.cardBorderColor
            clip: true
            Accessible.role: Accessible.Dialog
            Accessible.name: qsTr("Welcome")

            // Section 3.8: when the right column cannot hold the full cover block and
            // the minimum information block, the cover block uses its compact width.
            readonly property bool compact: rightColumn.width < appTheme.welcomeCoverWidth
                                                                + appTheme.spaceXl
                                                                + appTheme.welcomeInfoMinWidth
            readonly property real sidebarWidth: appTheme.welcomeSidebarWidth
            readonly property real columnGap: appTheme.spaceXl + appTheme.spaceXs

            // The items are declared in Tab order (section 3.7): left column
            // actions, the right column, then the language selector and Quit.

            // Left column, upper part: wordmark, description, project actions.
            ColumnLayout {
                id: sidebarTop
                x: appTheme.spaceXl + appTheme.spaceXs
                y: appTheme.spaceXl + appTheme.spaceXs
                width: shell.sidebarWidth - appTheme.spaceXs
                spacing: 0

                Row {
                    Accessible.role: Accessible.Heading
                    Accessible.name: qsTr("Alcedo Studio")

                    Label {
                        text: qsTr("Alcedo")
                        color: appTheme.accentColor
                        font.family: appTheme.headlineFontFamily
                        font.pixelSize: appTheme.fontSizeHeadline
                        font.weight: appTheme.fontWeightHeading
                        Accessible.ignored: true
                    }

                    Label {
                        text: " "
                        font.family: appTheme.headlineFontFamily
                        font.pixelSize: appTheme.fontSizeHeadline
                        Accessible.ignored: true
                    }

                    Label {
                        text: qsTr("Studio")
                        color: appTheme.textColor
                        font.family: appTheme.headlineFontFamily
                        font.pixelSize: appTheme.fontSizeHeadline
                        font.weight: appTheme.fontWeightHeading
                        Accessible.ignored: true
                    }
                }

                Label {
                    Layout.fillWidth: true
                    Layout.topMargin: appTheme.spaceSm
                    text: qsTr("Each project is one .alcd file that holds the photo references, the edit history and the versions.")
                    color: appTheme.textMutedColor
                    font.family: appTheme.uiFontFamily
                    font.pixelSize: appTheme.fontSizeBody
                    font.weight: appTheme.fontWeightRegular
                    lineHeight: appTheme.lineHeightBody
                    lineHeightMode: Text.FixedHeight
                    wrapMode: Text.Wrap
                }

                ColumnLayout {
                    Layout.fillWidth: true
                    Layout.topMargin: appTheme.spaceXl + appTheme.spaceSm
                    visible: dialog.hasRecentProjects
                    spacing: appTheme.spaceSm

                    WelcomeActionButton {
                        id: openProjectButton
                        objectName: "welcomeOpenProjectButton"
                        Layout.fillWidth: true
                        kind: "secondary"
                        iconSource: "qrc:/panel_icons/folder-open.svg"
                        text: qsTr("Open Project…")
                        enabled: dialog.selectionEnabled
                        onClicked: dialog.requestOpen()
                    }

                    WelcomeActionButton {
                        id: newProjectButton
                        objectName: "welcomeNewProjectButton"
                        Layout.fillWidth: true
                        kind: "secondary"
                        iconSource: "qrc:/panel_icons/folder-plus.svg"
                        text: qsTr("New Project…")
                        enabled: dialog.selectionEnabled && dialog.rightColumnMode !== "form"
                        onClicked: dialog.showNewProjectForm()
                    }
                }

                Label {
                    Layout.fillWidth: true
                    Layout.topMargin: appTheme.spaceMd
                    visible: dialog.launchMessage.length > 0 && dialog.rightColumnMode !== "form"
                    text: dialog.launchMessage
                    color: appTheme.textMutedColor
                    font.family: appTheme.uiFontFamily
                    font.pixelSize: appTheme.fontSizeCaption
                    font.weight: appTheme.fontWeightRegular
                    wrapMode: Text.Wrap
                    maximumLineCount: 6
                    elide: Text.ElideRight
                }
            }

            // Right column.
            StackLayout {
                id: rightColumn
                x: sidebarTop.x + shell.sidebarWidth + shell.columnGap - appTheme.spaceXs
                y: appTheme.spaceXl
                width: shell.width - x - appTheme.spaceXl
                height: shell.height - 2 * appTheme.spaceXl
                currentIndex: dialog.rightColumnMode === "form" ? 2 : (dialog.hasRecentProjects ? 0 : 1)

                ColumnLayout {
                    spacing: appTheme.spaceXl + appTheme.spaceXs

                    WelcomeProjectOverview {
                        id: projectOverview
                        Layout.fillWidth: true
                        adapter: dialog.adapter
                        compact: shell.compact
                        onContinueRequested: dialog.requestContinue()
                    }

                    WelcomeRecentProjectList {
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        projects: dialog.otherRecentProjects
                        selectionEnabled: dialog.selectionEnabled
                        onProjectRequested: function(projectPath) {
                            dialog.requestPreview(projectPath)
                        }
                    }
                }

                // Section 3.4: no recent project.
                Rectangle {
                    radius: appTheme.controlRadiusSmall
                    color: appTheme.bgBaseColor

                    ColumnLayout {
                        anchors.centerIn: parent
                        width: Math.min(parent.width - 2 * appTheme.spaceXl, appTheme.welcomeEmptyContentWidth)
                        spacing: appTheme.spaceXl

                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: appTheme.spaceSm

                            Label {
                                Layout.fillWidth: true
                                text: qsTr("Create your first project")
                                color: appTheme.textColor
                                font.family: appTheme.uiFontFamily
                                font.pixelSize: appTheme.fontSizeHeadline
                                font.weight: appTheme.fontWeightHeading
                                wrapMode: Text.Wrap
                                Accessible.role: Accessible.Heading
                            }

                            Label {
                                Layout.fillWidth: true
                                text: qsTr("Choose a folder for the project file, then import your photos. Alcedo does not move or change the original photos.")
                                color: appTheme.textMutedColor
                                font.family: appTheme.uiFontFamily
                                font.pixelSize: appTheme.fontSizeBody
                                font.weight: appTheme.fontWeightRegular
                                lineHeight: appTheme.lineHeightBody
                                lineHeightMode: Text.FixedHeight
                                wrapMode: Text.Wrap
                            }
                        }

                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: appTheme.spaceSm

                            WelcomeActionButton {
                                id: emptyNewProjectButton
                                objectName: "welcomeEmptyNewProjectButton"
                                Layout.fillWidth: true
                                kind: "primary"
                                centered: false
                                iconSource: "qrc:/panel_icons/folder-plus.svg"
                                text: qsTr("New Project…")
                                enabled: dialog.selectionEnabled
                                onClicked: dialog.showNewProjectForm()
                            }

                            WelcomeActionButton {
                                objectName: "welcomeEmptyOpenProjectButton"
                                Layout.fillWidth: true
                                kind: "secondary"
                                iconSource: "qrc:/panel_icons/folder-open.svg"
                                text: qsTr("Open Existing Project…")
                                enabled: dialog.selectionEnabled
                                onClicked: dialog.requestOpen()
                            }
                        }

                        Label {
                            Layout.fillWidth: true
                            visible: dialog.launchMessage.length > 0
                            text: dialog.launchMessage
                            color: appTheme.textMutedColor
                            font.family: appTheme.uiFontFamily
                            font.pixelSize: appTheme.fontSizeCaption
                            font.weight: appTheme.fontWeightRegular
                            wrapMode: Text.Wrap
                        }
                    }
                }

                WelcomeNewProjectForm {
                    id: newProjectForm
                    serviceMessage: dialog.launchMessage
                    actionsEnabled: dialog.selectionEnabled
                    onBackRequested: dialog.rightColumnMode = "overview"
                    onCreateRequested: function(storageLocation, projectName) {
                        dialog.requestCreate(storageLocation, projectName)
                    }
                }
            }

            // Left column, footer: divider, language selector, Quit.
            ColumnLayout {
                x: sidebarTop.x
                width: sidebarTop.width
                anchors.bottom: parent.bottom
                anchors.bottomMargin: appTheme.spaceXl
                spacing: appTheme.spaceMd

                Rectangle {
                    Layout.fillWidth: true
                    Layout.preferredHeight: 1
                    color: appTheme.dividerColor
                }

                RowLayout {
                    Layout.fillWidth: true
                    spacing: appTheme.spaceSm

                    ComboBox {
                        id: languageCombo
                        objectName: "welcomeLanguageCombo"
                        Layout.preferredWidth: shell.sidebarWidth / 2
                        Layout.preferredHeight: appTheme.iconButtonHitSizeCompact
                        model: dialog.languageOptions
                        textRole: "label"
                        currentIndex: dialog.currentLanguageIndex
                        font.family: appTheme.uiFontFamily
                        font.pixelSize: appTheme.fontSizeBody
                        Accessible.name: qsTr("Interface language")
                        onActivated: function(index) {
                            const item = model[index]
                            if (item) {
                                dialog.languageRequested(item.code)
                            }
                        }

                        background: Rectangle {
                            radius: appTheme.controlRadiusSmall
                            color: appTheme.bgBaseColor
                            border.width: 1
                            border.color: languageCombo.visualFocus || languageCombo.hovered
                                          ? appTheme.textMutedColor
                                          : appTheme.cardBorderColor
                        }

                        contentItem: Label {
                            leftPadding: appTheme.spaceMd
                            rightPadding: appTheme.spaceXl + appTheme.spaceSm
                            text: languageCombo.displayText
                            color: appTheme.textColor
                            font: languageCombo.font
                            verticalAlignment: Text.AlignVCenter
                            elide: Text.ElideRight
                        }

                        indicator: Label {
                            x: languageCombo.width - width - appTheme.spaceMd
                            y: Math.round((languageCombo.height - height) / 2)
                            text: "▾"
                            color: appTheme.textMutedColor
                            font.pixelSize: appTheme.fontSizeCaption
                        }

                        popup: Popup {
                            // The welcome dialog sits above the default popup layer.
                            z: dialog.z + 1
                            y: languageCombo.height + appTheme.spaceXs
                            width: languageCombo.width
                            implicitHeight: contentItem.implicitHeight + 2 * appTheme.spaceXs
                            padding: appTheme.spaceXs

                            background: Rectangle {
                                radius: appTheme.controlRadiusSmall
                                color: appTheme.bgBaseColor
                                border.width: 1
                                border.color: appTheme.cardBorderColor
                            }

                            contentItem: ListView {
                                clip: true
                                implicitHeight: contentHeight
                                spacing: appTheme.spaceXs
                                model: languageCombo.popup.visible ? languageCombo.delegateModel : null
                                currentIndex: languageCombo.highlightedIndex
                            }
                        }

                        delegate: ItemDelegate {
                            id: languageOption
                            required property var modelData
                            required property int index
                            width: languageCombo.width - 2 * appTheme.spaceXs
                            height: appTheme.iconButtonHitSizeCompact - appTheme.spaceSm
                            highlighted: languageCombo.highlightedIndex === languageOption.index

                            background: Rectangle {
                                radius: appTheme.controlRadiusSmall
                                color: languageOption.highlighted
                                       ? appTheme.editorListSelectedFillColor
                                       : (languageOption.hovered ? appTheme.hoverColor : "transparent")
                            }

                            contentItem: Label {
                                leftPadding: appTheme.spaceSm
                                text: String(languageOption.modelData.label || "")
                                color: languageOption.highlighted
                                       ? appTheme.editorListSelectedInkColor
                                       : appTheme.textColor
                                font.family: appTheme.uiFontFamily
                                font.pixelSize: appTheme.fontSizeBody
                                verticalAlignment: Text.AlignVCenter
                                elide: Text.ElideRight
                            }
                        }
                    }

                    Item {
                        Layout.fillWidth: true
                    }

                    WelcomeActionButton {
                        objectName: "welcomeQuitButton"
                        kind: "quiet"
                        compact: true
                        text: qsTr("Quit")
                        onClicked: dialog.exitRequested()
                    }
                }
            }
        }
    }
}
