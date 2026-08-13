import QtQuick
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
    property int requestedCategory: 0
    readonly property bool canCompleteSettings: appModules.interactionPolicy.canRunSemanticGeneration
    readonly property bool acceleratorRestartHintVisible:
        pendingAcceleratorBackend.length > 0
        && pendingAcceleratorBackend !== appModules.project.acceleratorBackend

    signal messageRequested(string message)
    signal semanticGenerationBackgroundRequested()

    onVisibleChanged: {
        if (visible) {
            resetPendingValues()
            currentCategory = requestedCategory
        }
    }

    onCurrentCategoryChanged: {
        if (currentCategory === 2) {
            cachePanel.refreshStats()
        } else if (currentCategory === 3) {
            appModules.semanticGeneration.RefreshAlbumSummary()
        } else if (currentCategory === 4) {
            // Refresh credential/configured state when the Advanced Content
            // Analysis page is shown, so the API-key label and model box reflect
            // any changes made since the last visit.
            appModules.imageAnalysis.RefreshCredentialState()
        }
    }

    function resetPendingValues() {
        pendingThemeIndex = appTheme.currentThemeIndex
        pendingLanguageCode = languageManager.currentLanguageCode
        pendingSemanticImportPreference = appModules.semanticGeneration.importPreference
        pendingAcceleratorBackend = appModules.project.acceleratorBackend
        cachePanel.reloadPending()
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

    function currentPageTitle() {
        if (currentCategory === 0) {
            return qsTr("Language")
        }
        if (currentCategory === 1) {
            return qsTr("Theme and color")
        }
        if (currentCategory === 2) {
            return qsTr("Cache")
        }
        if (currentCategory === 3) {
            return qsTr("Local Content Recognition")
        }
        if (currentCategory === 4) {
            return qsTr("Advanced Content Analysis")
        }
        if (currentCategory === 5) {
            return qsTr("Acceleration")
        }
        if (currentCategory === 6) {
            return qsTr("Updates")
        }
        return qsTr("About")
    }

    function currentPageInfoText() {
        if (currentCategory === 3) {
            return qsTr("本地 AI 功能运行在本机 AI 模型上，图片内容不会上传至云端。识别速度和可处理规模取决于你的 CPU、GPU、内存与磁盘性能。\n\n默认推荐使用 SigLIP2 模型：它是当前最均衡的选择，多语言语义理解更稳，适合大多数相册标注和自然语言搜索。\n\n本地模型特点：\nSigLIP2 B/32 256 Multilingual：默认推荐，多语言、质量稳定、适合长期使用。\nMobileCLIP2 S2 English：更轻更快，偏英文场景，适合低配电脑或快速试用。\nJina CLIP v2 INT8 Multilingual：多语言，512px 输入，模型更大，适合需要更细图文语义的场景。\nSigLIP2 Base CoreML macOS：macOS 原生 CoreML 版本，适合 Apple Silicon 设备。\n\nCLIP / SigLIP 这类视觉语言模型会把图像和文字映射到同一语义空间，因此可以理解“海边日落”“人像”“建筑细节”等自然语言概念，并用于生成标签和语义检索。")
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
                            color: dialog.primaryAccent
                            font.family: dialog.headlineFontFamily
                            font.pixelSize: 26
                            font.weight: 800
                            elide: Text.ElideRight
                        }

                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: 8

                            Repeater {
                                model: [
                                    { label: qsTr("Language"), icon: "qrc:/panel_icons/language.svg" },
                                    { label: qsTr("Theme and color"), icon: "qrc:/panel_icons/palette.svg" },
                                    { label: qsTr("Cache"), icon: "qrc:/panel_icons/box.svg" },
                                    { label: qsTr("Local Content Recognition"), icon: "qrc:/panel_icons/search.svg" },
                                    { label: qsTr("Advanced Content Analysis"), icon: "qrc:/panel_icons/flask.svg" },
                                    { label: qsTr("Acceleration"), icon: "qrc:/panel_icons/cpu.svg" },
                                    { label: qsTr("Updates"), icon: "qrc:/panel_icons/update.svg" },
                                    { label: qsTr("About"), icon: "qrc:/panel_icons/aperture.svg" }
                                ]

                                delegate: Rectangle {
                                    required property int index
                                    required property var modelData

                                    Layout.fillWidth: true
                                    Layout.preferredHeight: 48
                                    radius: 6
                                    color: dialog.currentCategory === index
                                           ? Qt.rgba(dialog.primaryAccent.r, dialog.primaryAccent.g, dialog.primaryAccent.b, 0.22)
                                           : (categoryMouse.containsMouse ? dialog.hoverColor : "transparent")

                                    RowLayout {
                                        anchors.fill: parent
                                        anchors.leftMargin: 12
                                        anchors.rightMargin: 12
                                        spacing: 12

                                        Image {
                                            Layout.preferredWidth: 20
                                            Layout.preferredHeight: 20
                                            source: modelData.icon
                                            sourceSize.width: 20
                                            sourceSize.height: 20
                                            asynchronous: true
                                            opacity: dialog.currentCategory === index ? 0.95 : 0.72
                                        }

                                        Label {
                                            Layout.fillWidth: true
                                            text: modelData.label
                                            color: dialog.currentCategory === index ? dialog.textColor : dialog.mutedTextColor
                                            font.pixelSize: 15
                                            font.weight: dialog.currentCategory === index ? 700 : 500
                                            elide: Text.ElideRight
                                        }

                                        Rectangle {
                                            Layout.preferredWidth: 8
                                            Layout.preferredHeight: 8
                                            Layout.alignment: Qt.AlignVCenter
                                            radius: 4
                                            visible: index === 6
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

                        Item {
                            Layout.fillHeight: true
                        }
                    }
                }

                ColumnLayout {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    spacing: 0

                    Item {
                        Layout.fillWidth: true
                        Layout.preferredHeight: 96

                        ColumnLayout {
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
                                    text: dialog.currentPageTitle()
                                    color: dialog.textColor
                                    font.family: dialog.headlineFontFamily
                                    font.pixelSize: 34
                                    font.weight: 800
                                    elide: Text.ElideRight
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
                                            font.pixelSize: 15
                                            font.weight: 600
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
                            }
                        }

                        ScrollView {
                            id: themeScroll
                            contentWidth: availableWidth
                            clip: true

                            ColumnLayout {
                                width: themeScroll.availableWidth
                                spacing: 18

                                SettingsSection {
                                    Layout.fillWidth: true
                                    Layout.topMargin: 26
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
                                            font.pixelSize: 15
                                            font.weight: 600
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
                            }
                        }

                        ScrollView {
                            id: cacheScroll
                            contentWidth: availableWidth
                            clip: true

                            CacheSettingsPanel {
                                id: cachePanel
                                width: cacheScroll.availableWidth
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

                        ScrollView {
                            id: semanticScroll
                            contentWidth: availableWidth
                            clip: true

                            ColumnLayout {
                                width: semanticScroll.availableWidth
                                spacing: 20

                                SemanticGenerationSettingsPanel {
                                    Layout.fillWidth: true
                                    Layout.topMargin: 26
                                    Layout.leftMargin: 34
                                    Layout.rightMargin: 34
                                    Layout.bottomMargin: 26
                                    semanticController: appModules.semanticGeneration
                                    downloadController: appModules.modelDownload
                                    interactionPolicy: appModules.interactionPolicy
                                    importPreference: dialog.pendingSemanticImportPreference
                                    primaryAccent: dialog.primaryAccent
                                    secondaryAccent: dialog.secondaryAccent
                                    textColor: dialog.textColor
                                    mutedTextColor: dialog.mutedTextColor
                                    canvasColor: dialog.canvasColor
                                    dividerColor: dialog.dividerColor
                                    dangerColor: dialog.dangerColor
                                    dataFontFamily: dialog.dataFontFamily
                                    onImportPreferenceRequested: function(preference) {
                                        dialog.pendingSemanticImportPreference = preference
                                    }
                                    onMessageRequested: function(message) {
                                        dialog.messageRequested(message)
                                    }
                                    onBackgroundRequested: {
                                        dialog.semanticGenerationBackgroundRequested()
                                        dialog.close()
                                    }
                                }
                            }
                        }

                        AiProviderSettingsPanel {
                            Layout.fillWidth: true
                            Layout.fillHeight: true
                            width: parent ? parent.width : 0
                            profileController: appModules.aiProviderProfiles
                            analysisController: appModules.imageAnalysis
                            interactionPolicy: appModules.interactionPolicy
                            backgroundSource: dialog.blurSource
                            primaryAccent: dialog.primaryAccent
                            secondaryAccent: dialog.secondaryAccent
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
                                                font.pixelSize: 15
                                                font.weight: 600
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
                                            font.pixelSize: 12
                                            font.weight: 500
                                            lineHeight: 1.25
                                        }

                                        Label {
                                            Layout.fillWidth: true
                                            Layout.leftMargin: 176
                                            visible: appModules.project.acceleratorWarning.length > 0
                                            text: appModules.project.acceleratorWarning
                                            wrapMode: Text.WordWrap
                                            color: dialog.dangerColor
                                            font.pixelSize: 12
                                            font.weight: 500
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
                            id: aboutScroll
                            contentWidth: availableWidth
                            clip: true

                            AboutPage {
                                Layout.fillWidth: true
                                updateService: appModules.updates
                                primaryAccent: dialog.primaryAccent
                                secondaryAccent: dialog.secondaryAccent
                                textColor: dialog.textColor
                                mutedTextColor: dialog.mutedTextColor
                                canvasColor: dialog.canvasColor
                                panelColor: dialog.panelColor
                                dividerColor: dialog.dividerColor
                                dangerColor: dialog.dangerColor
                                headlineFontFamily: dialog.headlineFontFamily
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
                        Layout.preferredHeight: 82
                        Layout.leftMargin: 34
                        Layout.rightMargin: 34
                        spacing: 16

                        Item {
                            Layout.fillWidth: true
                        }

                        Button {
                            id: applyButton
                            Layout.preferredWidth: 168
                            Layout.preferredHeight: 48
                            text: qsTr("Done")
                            enabled: dialog.canCompleteSettings
                            font.pixelSize: 15
                            font.weight: 800
                            palette.buttonText: appTheme.editorListSelectedInkColor
                            onClicked: dialog.applySettings()
                            background: Rectangle {
                                radius: 10
                                color: applyButton.down
                                       ? Qt.darker(dialog.primaryAccent, 1.16)
                                       : (applyButton.hovered
                                          ? Qt.lighter(dialog.primaryAccent, 1.06)
                                          : dialog.primaryAccent)
                                border.width: 1
                                border.color: Qt.rgba(dialog.secondaryAccent.r,
                                                      dialog.secondaryAccent.g,
                                                      dialog.secondaryAccent.b,
                                                      0.18)
                                opacity: applyButton.enabled ? 1.0 : 0.45
                            }
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
            font.pixelSize: 18
            font.weight: 800
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
                font.pixelSize: 14
                font.weight: 800
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
                font.pixelSize: 13
                font.weight: 500
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
