import QtQuick
import QtQml.Models
import QtQuick.Controls.Basic
import QtQuick.Layouts
import QtQuick.Effects

Dialog {
    id: dialog
    font.family: appTheme.uiFontFamily

    parent: Overlay.overlay
    modal: true
    focus: visible
    closePolicy: canCompleteSettings ? Popup.CloseOnEscape : Popup.NoAutoClose
    padding: 0
    width: parent ? parent.width : 0
    height: parent ? parent.height : 0
    x: 0
    y: 0

    property Item blurSource: null
    property var languageOptions: []
    property color primaryAccent: appTheme.accentColor
    property color secondaryAccent: appTheme.accentSecondaryColor
    property color textColor: appTheme.textColor
    property color mutedTextColor: appTheme.textMutedColor
    property color panelColor: appTheme.cardSurfaceColor
    property color canvasColor: appTheme.bgBaseColor
    property color overlayColor: appTheme.overlayColor
    property color hoverColor: appTheme.hoverColor
    property color dividerColor: appTheme.dividerColor
    property color dangerColor: appTheme.dangerColor
    property color panelBorderColor: appTheme.cardBorderColor
    property string headlineFontFamily: appTheme.headlineFontFamily
    readonly property string dataFontFamily: appTheme.dataFontFamily
    property real cornerRadius: 0

    property int currentCategory: 0
    property int pendingThemeIndex: appTheme.currentThemeIndex
    property string pendingLanguageCode: languageManager.currentLanguageCode
    property string pendingSemanticImportPreference: appModules.semanticGeneration.importPreference
    property string pendingAcceleratorBackend: appModules.project.acceleratorBackend
    property string pendingLockedNodeMaskAction: appModules.editorBehavior.lockedNodeMaskAction
    property bool pendingSkipDuplicateImports: appModules.importBehavior.skipDuplicateImports
    property string pendingLibraryGridLayout: appModules.libraryView.gridLayout
    // User names of the seven color label codes; an empty entry keeps the color name.
    property var pendingColorLabelNames: Array.from(appModules.libraryView.colorLabelUserNames)
    property int requestedCategory: 0
    // The next opening scrolls the Language / Theme page to Library > Color Labels (the pencil of
    // By Custom Label in the Library Overview).
    property bool revealColorLabelsOnOpen: false
    // Category indices used by callers outside this dialog.
    readonly property int colorLabelsCategory: 0
    readonly property int updatesCategory: 6
    // LUTs and Color Management: the LUT library and the DCP camera profile folder.
    readonly property int lutCategory: 8
    readonly property int aiModelsCategory: 9
    readonly property int aboutCategory: 10
    // The AI Models page checks the signed AI model feed once per Settings opening, when it is
    // first shown (AI Models plan, P6).
    property bool aiModelFeedCheckedThisOpening: false
    readonly property bool canCompleteSettings: appModules.interactionPolicy.canRunSemanticGeneration
    readonly property bool acceleratorRestartHintVisible:
        pendingAcceleratorBackend.length > 0
        && pendingAcceleratorBackend !== appModules.project.acceleratorBackend

    signal messageRequested(string message)
    signal semanticGenerationBackgroundRequested()

    onVisibleChanged: {
        if (visible) {
            resetPendingValues()
            aiModelFeedCheckedThisOpening = false
            currentCategory = requestedCategory
            if (revealColorLabelsOnOpen) {
                revealColorLabelsOnOpen = false
                Qt.callLater(revealColorLabels)
            }
            // Each Settings opening checks the signed LUT package list once; the
            // application never checks it at startup (plan L6B).
            if (appModules.lutPackages)
                appModules.lutPackages.checkPackages()
            // Settings can open on the AI Models page; no category change follows then.
            showAiModelsPage()
        }
    }

    // Scroll the Language / Theme page so that the Color Labels row is at its top.
    function revealColorLabels() {
        const flick = languageScroll.contentItem
        if (!flick || !colorLabelsRow)
            return
        const rowY = colorLabelsRow.mapToItem(flick.contentItem, 0, 0).y
        const maxY = Math.max(0, flick.contentHeight - flick.height)
        flick.contentY = Math.max(0, Math.min(maxY, rowY - 26))
    }

    function showAiModelsPage() {
        if (!visible || currentCategory !== aiModelsCategory)
            return
        if (appModules.modelDownload)
            appModules.modelDownload.refreshModels()
        if (!aiModelFeedCheckedThisOpening && appModules.aiModelPackages) {
            aiModelFeedCheckedThisOpening = true
            appModules.aiModelPackages.checkPackages()
        }
    }

    onCurrentCategoryChanged: {
        if (currentCategory === 2) {
            cachePanel.refreshStats()
        } else if (currentCategory === 3) {
            appModules.semanticGeneration.RefreshAlbumSummary()
        } else if (currentCategory === aiModelsCategory) {
            showAiModelsPage()
        }
    }

    function resetPendingValues() {
        pendingThemeIndex = appTheme.currentThemeIndex
        pendingLanguageCode = languageManager.currentLanguageCode
        pendingSemanticImportPreference = appModules.semanticGeneration.importPreference
        pendingAcceleratorBackend = appModules.project.acceleratorBackend
        pendingLockedNodeMaskAction = appModules.editorBehavior.lockedNodeMaskAction
        pendingSkipDuplicateImports = appModules.importBehavior.skipDuplicateImports
        pendingLibraryGridLayout = appModules.libraryView.gridLayout
        pendingColorLabelNames = Array.from(appModules.libraryView.colorLabelUserNames)
        cachePanel.reloadPending()
    }

    function setPendingColorLabelName(code, name) {
        const names = pendingColorLabelNames.slice()
        names[code] = name
        pendingColorLabelNames = names
    }

    function acceleratorIndexForValue(value) {
        const options = appModules.project.acceleratorOptions
        for (let i = 0; i < options.length; ++i) {
            if (options[i].value === value) {
                return i
            }
        }
        return options.length > 0 ? 0 : -1
    }

    function languageIndexForCode(code) {
        for (let i = 0; i < languageOptions.length; ++i) {
            if (languageOptions[i].code === code) {
                return i
            }
        }
        return 0
    }

    function themeModelIndexForTheme(themeIndex) {
        const themes = appTheme.availableThemes
        for (let i = 0; i < themes.length; ++i) {
            if (themes[i].index === themeIndex) {
                return i
            }
        }
        return 0
    }

    function lockedNodeMaskActionIndex(action) {
        const entries = lockedNodeMaskComboModel.entries
        for (let i = 0; i < entries.length; ++i) {
            if (entries[i].value === action) {
                return i
            }
        }
        return 0
    }

    function libraryGridLayoutIndex(layout) {
        const entries = libraryGridLayoutComboModel.entries
        for (let i = 0; i < entries.length; ++i) {
            if (entries[i].value === layout) {
                return i
            }
        }
        return 0
    }

    function currentPageTitle() {
        if (currentCategory === 0) {
            return qsTr("Language / Theme")
        }
        if (currentCategory === 1) {
            return qsTr("Default Behavior")
        }
        if (currentCategory === 2) {
            return qsTr("Cache")
        }
        if (currentCategory === 3) {
            return qsTr("Local Content Recognition")
        }
        if (currentCategory === 4) {
            return qsTr("Saved AI Keys")
        }
        if (currentCategory === 5) {
            return qsTr("Acceleration")
        }
        if (currentCategory === 6) {
            return qsTr("Updates")
        }
        if (currentCategory === 7) {
            return qsTr("Keyboard")
        }
        if (currentCategory === lutCategory) {
            return qsTr("LUTs and Color Management")
        }
        if (currentCategory === aiModelsCategory) {
            return qsTr("AI Models")
        }
        return qsTr("About")
    }

    function currentPageInfoText() {
        if (currentCategory === 3) {
            return qsTr("The AI model runs on this computer, and no photo is uploaded. The labeling speed depends on the CPU, the GPU, and the disk.")
        }
        if (currentCategory === 4) {
            return qsTr("高级内容识别会通过你配置的 Anthropic / OpenAI 兼容提供商，与指定 agent 交互来识别图像内容并进行评分。数据保留、隐私与合规政策请咨询你使用的 AI 提供商；Alcedo Studio 不会保留任何内容。\n\n这个设置界面的灵感来自 ccswitch，使用方式也类似。")
        }
        return ""
    }

    function applySettings() {
        if (appTheme.currentThemeIndex !== pendingThemeIndex) {
            appTheme.currentThemeIndex = pendingThemeIndex
        }
        if (languageManager.currentLanguageCode !== pendingLanguageCode) {
            languageManager.setLanguage(pendingLanguageCode)
        }
        if (appModules.editorBehavior.lockedNodeMaskAction !== pendingLockedNodeMaskAction) {
            appModules.editorBehavior.setLockedNodeMaskAction(pendingLockedNodeMaskAction)
        }
        appModules.importBehavior.SetSkipDuplicateImports(pendingSkipDuplicateImports)
        if (appModules.libraryView.gridLayout !== pendingLibraryGridLayout) {
            appModules.libraryView.setGridLayout(pendingLibraryGridLayout)
        }
        const storedColorLabelNames = appModules.libraryView.colorLabelUserNames
        for (let code = 0; code < pendingColorLabelNames.length; ++code) {
            const pendingName = String(pendingColorLabelNames[code] || "").trim()
            if (pendingName !== storedColorLabelNames[code]) {
                appModules.libraryView.SetColorLabelName(code, pendingName)
            }
        }
        cachePanel.applyPending()
        if (appModules.semanticGeneration.importPreference !== pendingSemanticImportPreference) {
            appModules.semanticGeneration.SetImportPreference(pendingSemanticImportPreference)
        }
        if (appModules.project.acceleratorBackend !== pendingAcceleratorBackend) {
            if (!appModules.project.SetAcceleratorBackend(pendingAcceleratorBackend)) {
                messageRequested(appModules.project.serviceMessage)
                return
            }
        }
        messageRequested(qsTr("Settings applied"))
        close()
    }

    function mapLabeledEntries(source, valueKey) {
        const src = source || []
        const mapped = []
        for (let i = 0; i < src.length; ++i) {
            mapped.push({
                value: src[i][valueKey],
                label: src[i].label
            })
        }
        return mapped
    }

    QtObject {
        id: languageComboModel
        property string label: ""
        property bool enabled: true
        readonly property var entries: dialog.mapLabeledEntries(dialog.languageOptions, "code")
        property int currentIndex: dialog.languageIndexForCode(dialog.pendingLanguageCode)
        function selectIndex(index) {
            const item = entries[index]
            if (item)
                dialog.pendingLanguageCode = item.value
        }
    }

    QtObject {
        id: themeComboModel
        property string label: ""
        property bool enabled: true
        readonly property var entries: dialog.mapLabeledEntries(appTheme.availableThemes, "index")
        property int currentIndex: dialog.themeModelIndexForTheme(dialog.pendingThemeIndex)
        function selectIndex(index) {
            const item = entries[index]
            if (item)
                dialog.pendingThemeIndex = item.value
        }
    }

    QtObject {
        id: lockedNodeMaskComboModel
        property string label: ""
        property bool enabled: true
        readonly property var entries: [
            { value: "ask", label: qsTr("Ask every time") },
            { value: "newLayer", label: qsTr("Create a new layer, then draw") },
            { value: "currentNode", label: qsTr("Draw on the current node") }
        ]
        property int currentIndex: dialog.lockedNodeMaskActionIndex(dialog.pendingLockedNodeMaskAction)
        function selectIndex(index) {
            const item = entries[index]
            if (item)
                dialog.pendingLockedNodeMaskAction = item.value
        }
    }

    QtObject {
        id: libraryGridLayoutComboModel
        property string label: ""
        property bool enabled: true
        readonly property var entries: [
            { value: "classic", label: qsTr("Classic grid") },
            { value: "justified", label: qsTr("Justified rows") }
        ]
        property int currentIndex: dialog.libraryGridLayoutIndex(dialog.pendingLibraryGridLayout)
        function selectIndex(index) {
            const item = entries[index]
            if (item)
                dialog.pendingLibraryGridLayout = item.value
        }
    }

    QtObject {
        id: acceleratorComboModel
        property string label: ""
        property bool enabled: appModules.project.acceleratorOptions.length > 0
        readonly property var entries: appModules.project.acceleratorOptions || []
        property int currentIndex: dialog.acceleratorIndexForValue(dialog.pendingAcceleratorBackend)
        function selectIndex(index) {
            const options = entries
            if (index >= 0 && index < options.length)
                dialog.pendingAcceleratorBackend = String(options[index].value || "")
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
                blurEnabled: true
                blur: 0.6
                blurMax: 64
                saturation: -0.2
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
            height: Math.min(parent.height - 72, 620)
            radius: 14
            color: Qt.rgba(dialog.panelColor.r, dialog.panelColor.g, dialog.panelColor.b, 0.94)
            border.width: 1
            border.color: dialog.panelBorderColor
            clip: true

            RowLayout {
                anchors.fill: parent
                spacing: 0

                Rectangle {
                    Layout.preferredWidth: 250
                    Layout.fillHeight: true
                    color: Qt.rgba(dialog.canvasColor.r, dialog.canvasColor.g, dialog.canvasColor.b, 0.52)

                    ColumnLayout {
                        anchors.fill: parent
                        anchors.margins: 22
                        spacing: 18

                        Label {
                            Layout.fillWidth: true
                            text: qsTr("Setting")
                            color: dialog.textColor
                            font.family: dialog.headlineFontFamily
                            font.pixelSize: appTheme.fontSizeHeadline
                            font.weight: appTheme.fontWeightStrong
                            wrapMode: Text.Wrap
                        }

                        // Scrolls once the categories outgrow the dialog height.
                        ScrollView {
                            id: categoryNavScroll
                            objectName: "settingsNavScroll"
                            Layout.fillWidth: true
                            Layout.fillHeight: true
                            contentWidth: availableWidth
                            clip: true

                            ColumnLayout {
                                id: categoryNavColumn
                                width: categoryNavScroll.availableWidth
                                spacing: 2

                                // Instantiator, not Repeater: the Repeater's
                                // delegate-model never completes under an
                                // offscreen test window, so nav items would never
                                // materialize for QML tests (and this list is
                                // fixed at eleven entries, so eager creation is
                                // equivalent in production).
                                Instantiator {
                                    model: [
                                        { label: qsTr("Language / Theme"), icon: "qrc:/panel_icons/language.svg" },
                                        { label: qsTr("Default Behavior"), icon: "qrc:/panel_icons/zero-config.svg" },
                                        { label: qsTr("Cache"), icon: "qrc:/panel_icons/box.svg" },
                                        { label: qsTr("Local Content Recognition"), icon: "qrc:/panel_icons/search.svg" },
                                        { label: qsTr("Saved AI Keys"), icon: "qrc:/panel_icons/flask.svg" },
                                        { label: qsTr("Acceleration"), icon: "qrc:/panel_icons/cpu.svg" },
                                        { label: qsTr("Updates"), icon: "qrc:/panel_icons/update.svg" },
                                        { label: qsTr("Keyboard"), icon: "qrc:/panel_icons/keyboard.svg" },
                                        { label: qsTr("LUTs and Color Management"), icon: "qrc:/panel_icons/lut-cube.svg" },
                                        { label: qsTr("AI Models"), icon: "qrc:/panel_icons/sparkles.svg" },
                                        { label: qsTr("About"), icon: "qrc:/panel_icons/aperture.svg" }
                                    ]

                                    onObjectAdded: function(index, object) {
                                        object.parent = categoryNavColumn
                                    }

                                    delegate: Rectangle {
                                        required property int index
                                        required property var modelData

                                        objectName: "settingsNavItem:" + index
                                        Layout.fillWidth: true
                                        Layout.preferredHeight: Math.max(36, categoryNavRow.implicitHeight
                                                                         + appTheme.spaceSm)
                                        radius: 6
                                        // Neutral selection: a light wash of the text color.
                                        color: dialog.currentCategory === index
                                               ? Qt.rgba(dialog.textColor.r, dialog.textColor.g, dialog.textColor.b, 0.08)
                                               : (categoryMouse.containsMouse ? dialog.hoverColor : "transparent")

                                        RowLayout {
                                            id: categoryNavRow
                                            anchors.fill: parent
                                            anchors.leftMargin: 10
                                            anchors.rightMargin: 10
                                            spacing: 10

                                            Image {
                                                Layout.preferredWidth: 16
                                                Layout.preferredHeight: 16
                                                source: modelData.icon
                                                sourceSize.width: 32
                                                sourceSize.height: 32
                                                asynchronous: true
                                                opacity: dialog.currentCategory === index ? 0.95 : 0.72
                                            }

                                            Label {
                                                Layout.fillWidth: true
                                                text: modelData.label
                                                color: dialog.currentCategory === index ? dialog.textColor : dialog.mutedTextColor
                                                font.family: appTheme.uiFontFamily
                                                font.pixelSize: appTheme.fontSizeTitle
                                                font.weight: dialog.currentCategory === index
                                                             ? appTheme.fontWeightStrong
                                                             : appTheme.fontWeightRegular
                                                wrapMode: Text.Wrap
                                            }

                                            Rectangle {
                                                Layout.preferredWidth: 8
                                                Layout.preferredHeight: 8
                                                Layout.alignment: Qt.AlignVCenter
                                                radius: 4
                                                visible: index === dialog.updatesCategory
                                                         && appModules.updates
                                                         && (appModules.updates.updateDeferred
                                                             || appModules.updates.updateAvailable)
                                                color: appTheme.backgroundTaskFinishedColor
                                                Accessible.name: qsTr("Update available")
                                                Accessible.role: Accessible.Indicator
                                            }
                                        }

                                        MouseArea {
                                            id: categoryMouse
                                            anchors.fill: parent
                                            hoverEnabled: true
                                            cursorShape: Qt.PointingHandCursor
                                            onClicked: dialog.currentCategory = index
                                        }
                                    }
                                }
                            }
                        }
                    }
                }

                ColumnLayout {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    spacing: 0

                    Item {
                        Layout.fillWidth: true
                        Layout.preferredHeight: Math.max(72, pageHeaderColumn.implicitHeight
                                                         + appTheme.spaceMd)

                        ColumnLayout {
                            id: pageHeaderColumn
                            anchors.left: parent.left
                            anchors.right: parent.right
                            anchors.verticalCenter: parent.verticalCenter
                            anchors.leftMargin: 34
                            anchors.rightMargin: 34
                            spacing: 5

                            RowLayout {
                                Layout.fillWidth: true
                                spacing: 10

                                Label {
                                    objectName: "settingsPageTitle"
                                    Layout.fillWidth: true
                                    text: dialog.currentPageTitle()
                                    color: dialog.textColor
                                    font.family: dialog.headlineFontFamily
                                    font.pixelSize: appTheme.fontSizeHeadline
                                    font.weight: appTheme.fontWeightStrong
                                    wrapMode: Text.Wrap
                                }

                                InfoBadge {
                                    visible: dialog.currentPageInfoText().length > 0
                                    Layout.alignment: Qt.AlignVCenter
                                    text: dialog.currentPageInfoText()
                                    textColor: dialog.textColor
                                    mutedTextColor: dialog.mutedTextColor
                                    accentColor: dialog.secondaryAccent
                                    panelColor: dialog.panelColor
                                    dividerColor: dialog.dividerColor
                                    dataFontFamily: dialog.dataFontFamily
                                    boundsItem: shell
                                    toolTipWidth: Math.min(420, shell.width - 48)
                                }

                                Item {
                                    Layout.fillWidth: true
                                }
                            }

                        }
                    }

                    Rectangle {
                        Layout.fillWidth: true
                        Layout.preferredHeight: 1
                        color: dialog.dividerColor
                    }

                    StackLayout {
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        currentIndex: dialog.currentCategory

                        ScrollView {
                            id: languageScroll
                            contentWidth: availableWidth
                            clip: true

                            ColumnLayout {
                                width: languageScroll.availableWidth
                                spacing: 18

                                SettingsSection {
                                    Layout.fillWidth: true
                                    Layout.topMargin: 26
                                    Layout.leftMargin: 34
                                    Layout.rightMargin: 34
                                    title: qsTr("Application language")
                                    textColor: dialog.textColor
                                    mutedTextColor: dialog.mutedTextColor
                                    dividerColor: dialog.dividerColor

                                    RowLayout {
                                        Layout.fillWidth: true
                                        spacing: 16

                                        Label {
                                            Layout.preferredWidth: 160
                                            text: qsTr("Language")
                                            color: dialog.textColor
                                            font.pixelSize: appTheme.fontSizeTitle
                                            font.weight: appTheme.fontWeightStrong
                                        }

                                        AdjustmentCombo {
                                            objectName: "settingsLanguageControl"
                                            controlObjectName: "settingsLanguageCombo"
                                            Layout.fillWidth: true
                                            controlHeight: 36
                                            showResetButton: false
                                            model: languageComboModel
                                        }
                                    }
                                }

                                SettingsSection {
                                    Layout.fillWidth: true
                                    Layout.topMargin: 8
                                    Layout.leftMargin: 34
                                    Layout.rightMargin: 34
                                    title: qsTr("Workspace appearance")
                                    textColor: dialog.textColor
                                    mutedTextColor: dialog.mutedTextColor
                                    dividerColor: dialog.dividerColor

                                    RowLayout {
                                        Layout.fillWidth: true
                                        spacing: 16

                                        Label {
                                            Layout.preferredWidth: 160
                                            text: qsTr("Theme")
                                            color: dialog.textColor
                                            font.pixelSize: appTheme.fontSizeTitle
                                            font.weight: appTheme.fontWeightStrong
                                        }

                                        AdjustmentCombo {
                                            objectName: "settingsThemeControl"
                                            controlObjectName: "settingsThemeCombo"
                                            Layout.fillWidth: true
                                            controlHeight: 36
                                            showResetButton: false
                                            model: themeComboModel
                                        }
                                    }
                                }

                                SettingsSection {
                                    Layout.fillWidth: true
                                    Layout.topMargin: 8
                                    Layout.leftMargin: 34
                                    Layout.rightMargin: 34
                                    Layout.bottomMargin: 26
                                    title: qsTr("Library")
                                    textColor: dialog.textColor
                                    mutedTextColor: dialog.mutedTextColor
                                    dividerColor: dialog.dividerColor

                                    ColumnLayout {
                                        Layout.fillWidth: true
                                        spacing: 10

                                        RowLayout {
                                            Layout.fillWidth: true
                                            spacing: 16

                                            Label {
                                                Layout.preferredWidth: 160
                                                text: qsTr("Photo layout")
                                                color: dialog.textColor
                                                font.pixelSize: appTheme.fontSizeTitle
                                                font.weight: appTheme.fontWeightStrong
                                                wrapMode: Text.Wrap
                                            }

                                            AdjustmentCombo {
                                                objectName: "settingsLibraryGridLayoutControl"
                                                controlObjectName: "settingsLibraryGridLayoutCombo"
                                                Layout.fillWidth: true
                                                controlHeight: 36
                                                showResetButton: false
                                                model: libraryGridLayoutComboModel
                                            }
                                        }

                                        Label {
                                            Layout.fillWidth: true
                                            Layout.leftMargin: 176
                                            text: qsTr("Classic grid shows each photo in a card of the same size, with its details below it. Justified rows show the photos in rows of one height that fill the width; each photo keeps its shape, and its details and rating show when the pointer is on it. Grouped photos always use the classic grid.")
                                            wrapMode: Text.WordWrap
                                            color: dialog.mutedTextColor
                                            font.pixelSize: appTheme.fontSizeBody
                                            font.weight: appTheme.fontWeightRegular
                                            lineHeight: 1.25
                                        }

                                        RowLayout {
                                            id: colorLabelsRow
                                            objectName: "settingsColorLabelsRow"
                                            Layout.fillWidth: true
                                            Layout.topMargin: 8
                                            spacing: 16

                                            Label {
                                                Layout.preferredWidth: 160
                                                Layout.alignment: Qt.AlignTop
                                                Layout.topMargin: 8
                                                text: qsTr("Color Labels")
                                                color: dialog.textColor
                                                font.pixelSize: appTheme.fontSizeTitle
                                                font.weight: appTheme.fontWeightStrong
                                                wrapMode: Text.Wrap
                                            }

                                            ColorLabelNamesEditor {
                                                objectName: "settingsColorLabelNames"
                                                Layout.fillWidth: true
                                                Layout.maximumWidth: 520
                                                userNames: dialog.pendingColorLabelNames
                                                defaultNames: appModules.libraryView.colorLabelDefaultNames
                                                onNameEdited: function(code, name) {
                                                    dialog.setPendingColorLabelName(code, name)
                                                }
                                            }
                                        }
                                    }
                                }
                            }
                        }

                        ScrollView {
                            id: defaultBehaviorScroll
                            objectName: "defaultBehaviorSettingsScroll"
                            contentWidth: availableWidth
                            clip: true

                            ColumnLayout {
                                width: defaultBehaviorScroll.availableWidth
                                spacing: 18

                                SettingsSection {
                                    Layout.fillWidth: true
                                    Layout.topMargin: appTheme.spaceXl
                                    Layout.leftMargin: 34
                                    Layout.rightMargin: 34
                                    title: qsTr("Import")
                                    textColor: dialog.textColor
                                    mutedTextColor: dialog.mutedTextColor
                                    dividerColor: dialog.dividerColor

                                    ColumnLayout {
                                        Layout.fillWidth: true
                                        spacing: appTheme.spaceSm
                                        ThemeCheckBox {
                                            objectName: "settingsSkipDuplicateImports"
                                            checked: dialog.pendingSkipDuplicateImports
                                            alwaysPrimaryText: true
                                            text: qsTr("Skip duplicate imports")
                                            onToggled: function(checked) {
                                                dialog.pendingSkipDuplicateImports = checked
                                            }
                                        }
                                        Label {
                                            Layout.fillWidth: true
                                            text: qsTr("Skips repeated imports from the same source path. Files in different folders remain separate.")
                                            wrapMode: Text.Wrap
                                            color: dialog.mutedTextColor
                                            font.family: appTheme.uiFontFamily
                                            font.pixelSize: appTheme.fontSizeBody
                                        }
                                    }
                                }

                                SettingsSection {
                                    Layout.fillWidth: true
                                    Layout.topMargin: 26
                                    Layout.leftMargin: 34
                                    Layout.rightMargin: 34
                                    Layout.bottomMargin: 26
                                    title: qsTr("Masks")
                                    textColor: dialog.textColor
                                    mutedTextColor: dialog.mutedTextColor
                                    dividerColor: dialog.dividerColor

                                    ColumnLayout {
                                        Layout.fillWidth: true
                                        spacing: 10

                                        RowLayout {
                                            Layout.fillWidth: true
                                            spacing: 16

                                            Label {
                                                Layout.preferredWidth: 160
                                                text: qsTr("Adding a Mask on a locked node")
                                                color: dialog.textColor
                                                font.pixelSize: appTheme.fontSizeTitle
                                                font.weight: appTheme.fontWeightStrong
                                                wrapMode: Text.Wrap
                                            }

                                            AdjustmentCombo {
                                                objectName: "settingsLockedNodeMaskControl"
                                                controlObjectName: "settingsLockedNodeMaskCombo"
                                                Layout.fillWidth: true
                                                controlHeight: 36
                                                showResetButton: false
                                                model: lockedNodeMaskComboModel
                                            }
                                        }

                                        Label {
                                            Layout.fillWidth: true
                                            Layout.leftMargin: 176
                                            text: qsTr("The default adjustment node applies Alcedo's default adjustments, so it is locked. A Mask drawn on it also limits those defaults to the masked area. Layers you add yourself are not locked and are never affected by this setting.")
                                            wrapMode: Text.WordWrap
                                            color: dialog.mutedTextColor
                                            font.pixelSize: appTheme.fontSizeBody
                                            font.weight: appTheme.fontWeightRegular
                                            lineHeight: 1.25
                                        }
                                    }
                                }
                            }
                        }

                        ScrollView {
                            id: cacheScroll
                            contentWidth: availableWidth
                            clip: true

                            ColumnLayout {
                                width: cacheScroll.availableWidth
                                spacing: 0

                                CacheSettingsPanel {
                                    id: cachePanel
                                    width: parent.width
                                    libraryModule: appModules.library
                                    projectReady: appModules.project.serviceReady
                                    textColor: dialog.textColor
                                    mutedTextColor: dialog.mutedTextColor
                                    canvasColor: dialog.canvasColor
                                    dividerColor: dialog.dividerColor
                                    dangerColor: dialog.dangerColor
                                    dataFontFamily: dialog.dataFontFamily
                                    onMessageRequested: function(message) {
                                        dialog.messageRequested(message)
                                    }
                                }
                            }
                        }

                        // The page fits the dialog (#762); the scroll view only serves a window
                        // smaller than the dialog's full size.
                        ScrollView {
                            id: semanticScroll
                            objectName: "semanticSettingsScroll"
                            contentWidth: availableWidth
                            clip: true

                            SemanticGenerationSettingsPanel {
                                width: semanticScroll.availableWidth
                                semanticController: appModules.semanticGeneration
                                interactionPolicy: appModules.interactionPolicy
                                projectOpen: appModules.project.serviceReady
                                languageCode: languageManager.effectiveLanguageCode
                                importPreference: dialog.pendingSemanticImportPreference
                                onImportPreferenceRequested: function(preference) {
                                    dialog.pendingSemanticImportPreference = preference
                                }
                                onBackgroundRequested: {
                                    dialog.semanticGenerationBackgroundRequested()
                                    dialog.close()
                                }
                                onAiModelsPageRequested: dialog.currentCategory = dialog.aiModelsCategory
                            }
                        }

                        AiProviderSettingsPanel {
                            Layout.fillWidth: true
                            Layout.fillHeight: true
                            width: parent ? parent.width : 0
                            profileController: appModules.aiProviderProfiles
                            backgroundSource: dialog.blurSource
                            primaryAccent: dialog.primaryAccent
                            secondaryAccent: dialog.secondaryAccent
                            textColor: dialog.textColor
                            mutedTextColor: dialog.mutedTextColor
                            dividerColor: dialog.dividerColor
                            dangerColor: dialog.dangerColor
                            dataFontFamily: dialog.dataFontFamily
                        }

                        ScrollView {
                            id: accelerationScroll
                            contentWidth: availableWidth
                            clip: true

                            ColumnLayout {
                                width: accelerationScroll.availableWidth
                                spacing: 18

                                SettingsSection {
                                    Layout.fillWidth: true
                                    Layout.topMargin: 26
                                    Layout.leftMargin: 34
                                    Layout.rightMargin: 34
                                    Layout.bottomMargin: 26
                                    title: qsTr("Image processing backend")
                                    textColor: dialog.textColor
                                    mutedTextColor: dialog.mutedTextColor
                                    dividerColor: dialog.dividerColor

                                    ColumnLayout {
                                        Layout.fillWidth: true
                                        spacing: 10

                                        RowLayout {
                                            Layout.fillWidth: true
                                            spacing: 16

                                            Label {
                                                Layout.preferredWidth: 160
                                                text: qsTr("Backend")
                                                color: dialog.textColor
                                                font.pixelSize: appTheme.fontSizeTitle
                                                font.weight: appTheme.fontWeightStrong
                                            }

                                            AdjustmentCombo {
                                                id: acceleratorCombo
                                                objectName: "settingsAcceleratorControl"
                                                controlObjectName: "settingsAcceleratorCombo"
                                                Layout.fillWidth: true
                                                controlHeight: 36
                                                showResetButton: false
                                                model: acceleratorComboModel
                                            }
                                        }

                                        Label {
                                            Layout.fillWidth: true
                                            Layout.leftMargin: 176
                                            visible: dialog.acceleratorRestartHintVisible
                                            text: qsTr("Restart Alcedo yourself to apply this backend change.")
                                            wrapMode: Text.WordWrap
                                            color: dialog.mutedTextColor
                                            font.pixelSize: appTheme.fontSizeBody
                                            font.weight: appTheme.fontWeightRegular
                                            lineHeight: 1.25
                                        }

                                        Label {
                                            Layout.fillWidth: true
                                            Layout.leftMargin: 176
                                            visible: appModules.project.acceleratorWarning.length > 0
                                            text: appModules.project.acceleratorWarning
                                            wrapMode: Text.WordWrap
                                            color: dialog.dangerColor
                                            font.pixelSize: appTheme.fontSizeBody
                                            font.weight: appTheme.fontWeightRegular
                                            lineHeight: 1.25
                                        }
                                    }
                                }
                            }
                        }

                        ScrollView {
                            id: updatesScroll
                            contentWidth: availableWidth
                            clip: true

                            UpdatesSettingsPanel {
                                Layout.fillWidth: true
                                updateService: appModules.updates
                                textColor: dialog.textColor
                                mutedTextColor: dialog.mutedTextColor
                                dividerColor: dialog.dividerColor
                                dataFontFamily: dialog.dataFontFamily
                            }
                        }

                        ScrollView {
                            id: keyboardScroll
                            objectName: "keyboardSettingsScroll"
                            contentWidth: availableWidth
                            clip: true

                            KeyboardSettingsPanel {
                                id: keyboardPanel
                                objectName: "keyboardSettingsPanel"
                                width: keyboardScroll.availableWidth
                                textColor: dialog.textColor
                                mutedTextColor: dialog.mutedTextColor
                                accentColor: dialog.primaryAccent
                                canvasColor: dialog.canvasColor
                                dividerColor: dialog.dividerColor
                                panelBorderColor: dialog.panelBorderColor
                                hoverColor: dialog.hoverColor
                                dangerColor: dialog.dangerColor
                                dataFontFamily: dialog.dataFontFamily
                            }
                        }

                        ScrollView {
                            id: lutScroll
                            objectName: "lutSettingsScroll"
                            contentWidth: availableWidth
                            clip: true

                            LutSettingsPanel {
                                width: lutScroll.availableWidth
                                library: appModules.lutLibrary
                                packageService: appModules.lutPackages
                                cameraProfiles: appModules.cameraProfiles
                                textColor: dialog.textColor
                                mutedTextColor: dialog.mutedTextColor
                                dividerColor: dialog.dividerColor
                                dangerColor: dialog.dangerColor
                            }
                        }

                        ScrollView {
                            id: aiModelsScroll
                            objectName: "aiModelsSettingsScroll"
                            contentWidth: availableWidth
                            clip: true

                            AiModelsSettingsPanel {
                                width: aiModelsScroll.availableWidth
                                packageService: appModules.aiModelPackages
                                downloadController: appModules.modelDownload
                                semanticController: appModules.semanticGeneration
                                interactionPolicy: appModules.interactionPolicy
                                projectOpen: appModules.project.serviceReady
                            }
                        }

                        ScrollView {
                            id: aboutScroll
                            objectName: "aboutScroll"
                            contentWidth: availableWidth
                            clip: true

                            AboutPage {
                                Layout.fillWidth: true
                                updateService: appModules.updates
                                logFolder: appModules.logFolder
                            }
                        }
                    }

                    Rectangle {
                        Layout.fillWidth: true
                        Layout.preferredHeight: 1
                        color: dialog.dividerColor
                    }

                    RowLayout {
                        Layout.fillWidth: true
                        Layout.preferredHeight: 64
                        Layout.leftMargin: 34
                        Layout.rightMargin: 34
                        spacing: 16

                        Item {
                            Layout.fillWidth: true
                        }

                        // The main action of the dialog: the light button of neutral surfaces.
                        DialogActionButton {
                            id: applyButton
                            objectName: "settingsDoneButton"
                            kind: "light"
                            buttonWidth: Math.max(120, implicitContentWidth + 36)
                            buttonHeight: 34
                            text: qsTr("Done")
                            enabled: dialog.canCompleteSettings
                            onClicked: dialog.applySettings()
                        }
                    }
                }
            }
        }
    }

    component SettingsSection: ColumnLayout {
        property string title: ""
        property color textColor: "white"
        property color mutedTextColor: "#999999"
        property color dividerColor: Qt.rgba(1, 1, 1, 0.08)

        spacing: 14

        Label {
            Layout.fillWidth: true
            text: title
            color: textColor
            font.pixelSize: appTheme.fontSizeSection
            font.weight: appTheme.fontWeightStrong
        }

        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: 1
            color: dividerColor
        }
    }

    component InfoBadge: Item {
        id: badge

        property string text: ""
        property color textColor: "white"
        property color mutedTextColor: "#999999"
        property color accentColor: "#9FC7D8"
        property color panelColor: "#1C1C1D"
        property color dividerColor: Qt.rgba(1, 1, 1, 0.08)
        property string dataFontFamily: ""
        property Item boundsItem: null
        property int toolTipWidth: 360

        implicitWidth: 24
        implicitHeight: 24

        function popupBounds() {
            const overlay = Overlay.overlay
            if (overlay === null) {
                return Qt.rect(12, 12, 800, 600)
            }
            if (badge.boundsItem !== null) {
                const topLeft = badge.boundsItem.mapToItem(overlay, 0, 0)
                return Qt.rect(topLeft.x + 12, topLeft.y + 12,
                               Math.max(0, badge.boundsItem.width - 24),
                               Math.max(0, badge.boundsItem.height - 24))
            }
            return Qt.rect(12, 12, Math.max(0, overlay.width - 24), Math.max(0, overlay.height - 24))
        }

        function cursorInOverlay() {
            const overlay = Overlay.overlay
            if (overlay === null) {
                return Qt.point(infoMouse.mouseX, infoMouse.mouseY)
            }
            return badge.mapToItem(overlay, infoMouse.mouseX, infoMouse.mouseY)
        }

        function popupX() {
            const bounds = popupBounds()
            const cursor = cursorInOverlay()
            const maxX = bounds.x + bounds.width - infoPopup.width
            let x = cursor.x + 14
            if (x > maxX) {
                x = cursor.x - infoPopup.width - 14
            }
            return Math.max(bounds.x, Math.min(x, Math.max(bounds.x, maxX)))
        }

        function popupY() {
            const bounds = popupBounds()
            const cursor = cursorInOverlay()
            const maxY = bounds.y + bounds.height - infoPopup.height
            let y = cursor.y + 14
            if (y > maxY) {
                y = cursor.y - infoPopup.height - 14
            }
            return Math.max(bounds.y, Math.min(y, Math.max(bounds.y, maxY)))
        }

        Rectangle {
            anchors.fill: parent
            radius: width / 2
            color: "transparent"
            border.width: 1
            border.color: infoMouse.containsMouse
                          ? Qt.rgba(badge.accentColor.r, badge.accentColor.g, badge.accentColor.b, 0.68)
                          : Qt.rgba(badge.textColor.r, badge.textColor.g, badge.textColor.b, 0.22)

            Label {
                anchors.centerIn: parent
                text: "i"
                color: infoMouse.containsMouse ? badge.accentColor : badge.mutedTextColor
                font.family: badge.dataFontFamily
                font.pixelSize: appTheme.fontSizeSection
                font.weight: appTheme.fontWeightStrong
            }
        }

        MouseArea {
            id: infoMouse
            anchors.fill: parent
            hoverEnabled: true
            acceptedButtons: Qt.NoButton
        }

        ToolTip {
            id: infoPopup

            parent: Overlay.overlay
            visible: infoMouse.containsMouse
            delay: 260
            timeout: 12000
            text: badge.text
            x: badge.popupX()
            y: badge.popupY()
            width: Math.max(260, badge.toolTipWidth)
            padding: 12
            contentItem: Label {
                width: Math.max(236, badge.toolTipWidth - 24)
                text: badge.text
                color: badge.textColor
                font.pixelSize: appTheme.fontSizeTitle
                font.weight: appTheme.fontWeightRegular
                wrapMode: Text.WordWrap
                lineHeight: 1.22
            }
            background: Rectangle {
                color: Qt.rgba(badge.panelColor.r, badge.panelColor.g, badge.panelColor.b, 0.98)
                radius: 8
                border.width: 1
                border.color: badge.dividerColor
            }
        }
    }

}
