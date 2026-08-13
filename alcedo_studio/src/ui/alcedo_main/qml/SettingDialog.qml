import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Dialogs
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
    property bool pendingCacheEnabled: appModules.library.thumbnailDiskCacheEnabled
    property string pendingCacheRoot: appModules.library.thumbnailDiskCacheRoot
    property int pendingCacheMaxEntries: appModules.library.thumbnailDiskCacheMaxEntries
    property int pendingCacheJpegQuality: appModules.library.thumbnailDiskCacheJpegQuality
    property string pendingSemanticImportPreference: appModules.semanticGeneration.importPreference
    property string pendingAcceleratorBackend: appModules.project.acceleratorBackend
    property string cacheStatsSnapshot: ""
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
            refreshCacheStats()
        } else if (currentCategory === 3) {
            appModules.semanticGeneration.RefreshAlbumSummary()
        } else if (currentCategory === 4) {
            // Refresh credential/configured state when the Advanced Content
            // Analysis page is shown, so the API-key label and model box reflect
            // any changes made since the last visit.
            appModules.imageAnalysis.RefreshCredentialState()
        }
    }

    FolderDialog {
        id: cacheFolderDialog
        title: qsTr("Select Thumbnail Cache Folder")
        onAccepted: dialog.pendingCacheRoot = selectedFolder.toString()
    }

    function resetPendingValues() {
        pendingThemeIndex = appTheme.currentThemeIndex
        pendingLanguageCode = languageManager.currentLanguageCode
        pendingCacheEnabled = appModules.library.thumbnailDiskCacheEnabled
        pendingCacheRoot = appModules.library.thumbnailDiskCacheRoot
        pendingCacheMaxEntries = appModules.library.thumbnailDiskCacheMaxEntries
        pendingCacheJpegQuality = appModules.library.thumbnailDiskCacheJpegQuality
        pendingSemanticImportPreference = appModules.semanticGeneration.importPreference
        pendingAcceleratorBackend = appModules.project.acceleratorBackend
        refreshCacheStats()
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

    function refreshCacheStats() {
        cacheStatsSnapshot = appModules.library.thumbnailDiskCacheStats
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

    function statLineValue(label) {
        const lines = cacheStatsSnapshot.split("\n")
        for (let i = 0; i < lines.length; ++i) {
            const line = lines[i]
            if (line.indexOf(label + ": ") === 0) {
                return line.substring(label.length + 2)
            }
        }
        return qsTr("Unavailable")
    }

    function hitsMissesValue() {
        const value = statLineValue("Hits")
        const separator = " / Misses: "
        const separatorIndex = value.indexOf(separator)
        if (separatorIndex < 0) {
            return value
        }
        return qsTr("%1 / %2").arg(value.substring(0, separatorIndex))
                              .arg(value.substring(separatorIndex + separator.length))
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
        if (appModules.library.thumbnailDiskCacheEnabled !== pendingCacheEnabled) {
            appModules.library.SetThumbnailDiskCacheEnabled(pendingCacheEnabled)
        }
        if (appModules.library.thumbnailDiskCacheRoot !== pendingCacheRoot) {
            appModules.library.SetThumbnailDiskCacheRoot(pendingCacheRoot)
        }
        if (appModules.library.thumbnailDiskCacheMaxEntries !== pendingCacheMaxEntries) {
            appModules.library.SetThumbnailDiskCacheMaxEntries(pendingCacheMaxEntries)
        }
        if (appModules.library.thumbnailDiskCacheJpegQuality !== pendingCacheJpegQuality) {
            appModules.library.SetThumbnailDiskCacheJpegQuality(pendingCacheJpegQuality)
        }
        if (appModules.semanticGeneration.importPreference !== pendingSemanticImportPreference) {
            appModules.semanticGeneration.SetImportPreference(pendingSemanticImportPreference)
        }
        if (appModules.project.acceleratorBackend !== pendingAcceleratorBackend) {
            if (!appModules.project.SetAcceleratorBackend(pendingAcceleratorBackend)) {
                messageRequested(appModules.project.serviceMessage)
                return
            }
        }
        refreshCacheStats()
        messageRequested(qsTr("Settings applied"))
        close()
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

                                        ComboBox {
                                            Layout.fillWidth: true
                                            Layout.preferredHeight: 44
                                            model: dialog.languageOptions
                                            textRole: "label"
                                            currentIndex: dialog.languageIndexForCode(dialog.pendingLanguageCode)
                                            onActivated: function(index) {
                                                const item = model[index]
                                                if (item) {
                                                    dialog.pendingLanguageCode = item.code
                                                }
                                            }
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

                                        ComboBox {
                                            Layout.fillWidth: true
                                            Layout.preferredHeight: 44
                                            model: appTheme.availableThemes
                                            textRole: "label"
                                            currentIndex: dialog.themeModelIndexForTheme(dialog.pendingThemeIndex)
                                            onActivated: function(index) {
                                                const item = model[index]
                                                if (item) {
                                                    dialog.pendingThemeIndex = item.index
                                                }
                                            }
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
                                spacing: 20

                                SettingsSection {
                                    Layout.fillWidth: true
                                    Layout.topMargin: 26
                                    Layout.leftMargin: 34
                                    Layout.rightMargin: 34
                                    title: qsTr("Current cache")
                                    textColor: dialog.textColor
                                    mutedTextColor: dialog.mutedTextColor
                                    dividerColor: dialog.dividerColor

                                    GridLayout {
                                        Layout.fillWidth: true
                                        columns: 3
                                        columnSpacing: 12
                                        rowSpacing: 12

                                        CacheMetric {
                                            Layout.fillWidth: true
                                            label: qsTr("Entries")
                                            value: dialog.statLineValue("Entries")
                                            textColor: dialog.textColor
                                            mutedTextColor: dialog.mutedTextColor
                                            panelColor: dialog.canvasColor
                                        }

                                        CacheMetric {
                                            Layout.fillWidth: true
                                            label: qsTr("Size")
                                            value: dialog.statLineValue("Size")
                                            textColor: dialog.textColor
                                            mutedTextColor: dialog.mutedTextColor
                                            panelColor: dialog.canvasColor
                                        }

                                        CacheMetric {
                                            Layout.fillWidth: true
                                            label: qsTr("Hits / misses")
                                            value: dialog.hitsMissesValue()
                                            textColor: dialog.textColor
                                            mutedTextColor: dialog.mutedTextColor
                                            panelColor: dialog.canvasColor
                                        }
                                    }

                                }

                                SettingsSection {
                                    Layout.fillWidth: true
                                    Layout.leftMargin: 34
                                    Layout.rightMargin: 34
                                    title: qsTr("Storage")
                                    textColor: dialog.textColor
                                    mutedTextColor: dialog.mutedTextColor
                                    dividerColor: dialog.dividerColor

                                    RowLayout {
                                        Layout.fillWidth: true
                                        spacing: 16

                                        ColumnLayout {
                                            Layout.fillWidth: true
                                            spacing: 8

                                            Label {
                                                text: qsTr("Cache directory")
                                                color: dialog.textColor
                                                font.pixelSize: 15
                                                font.weight: 600
                                            }

                                            Rectangle {
                                                Layout.fillWidth: true
                                                Layout.preferredHeight: 50
                                                radius: 10
                                                color: Qt.rgba(1, 1, 1, 0.10)
                                                border.width: 1
                                                border.color: Qt.rgba(dialog.textColor.r, dialog.textColor.g, dialog.textColor.b, 0.12)

                                                Label {
                                                    anchors.fill: parent
                                                    anchors.leftMargin: 14
                                                    anchors.rightMargin: 14
                                                    text: dialog.pendingCacheRoot.length > 0
                                                          ? dialog.pendingCacheRoot
                                                          : dialog.statLineValue("Root")
                                                    elide: Text.ElideMiddle
                                                    verticalAlignment: Text.AlignVCenter
                                                    color: dialog.textColor
                                                    font.family: dialog.dataFontFamily
                                                    font.pixelSize: 14
                                                }
                                            }
                                        }

                                        Rectangle {
                                            id: browseButton
                                            Layout.preferredWidth: 50
                                            Layout.preferredHeight: 50
                                            Layout.alignment: Qt.AlignBottom
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
                                                width: 23
                                                height: 23
                                                source: "qrc:/panel_icons/folder-open.svg"
                                                sourceSize.width: 23
                                                sourceSize.height: 23
                                                asynchronous: true
                                            }

                                            MouseArea {
                                                id: browseMouse
                                                anchors.fill: parent
                                                hoverEnabled: true
                                                cursorShape: Qt.PointingHandCursor
                                                onClicked: cacheFolderDialog.open()
                                            }
                                        }
                                    }
                                }

                                SettingsSection {
                                    Layout.fillWidth: true
                                    Layout.leftMargin: 34
                                    Layout.rightMargin: 34
                                    title: qsTr("Limits")
                                    textColor: dialog.textColor
                                    mutedTextColor: dialog.mutedTextColor
                                    dividerColor: dialog.dividerColor

                                    RowLayout {
                                        Layout.fillWidth: true
                                        spacing: 16

                                        Label {
                                            Layout.preferredWidth: 180
                                            text: qsTr("Disk cache")
                                            color: dialog.textColor
                                            font.pixelSize: 15
                                            font.weight: 600
                                        }

                                        Switch {
                                            checked: dialog.pendingCacheEnabled
                                            text: checked ? qsTr("Enabled") : qsTr("Disabled")
                                            palette.windowText: dialog.textColor
                                            palette.buttonText: dialog.textColor
                                            palette.highlight: appTheme.accentColor
                                            onToggled: dialog.pendingCacheEnabled = checked
                                        }
                                    }

                                    RowLayout {
                                        Layout.fillWidth: true
                                        spacing: 16

                                        Label {
                                            Layout.preferredWidth: 180
                                            text: qsTr("Max entries")
                                            color: dialog.textColor
                                            font.pixelSize: 15
                                            font.weight: 600
                                        }

                                        SpinBox {
                                            Layout.preferredWidth: 170
                                            from: 1
                                            to: 100000
                                            stepSize: 500
                                            editable: true
                                            value: dialog.pendingCacheMaxEntries
                                            onValueModified: dialog.pendingCacheMaxEntries = value
                                        }
                                    }

                                    RowLayout {
                                        Layout.fillWidth: true
                                        spacing: 16

                                        Label {
                                            Layout.preferredWidth: 180
                                            text: qsTr("JPEG quality")
                                            color: dialog.textColor
                                            font.pixelSize: 15
                                            font.weight: 600
                                        }

                                        Slider {
                                            Layout.fillWidth: true
                                            from: 1
                                            to: 100
                                            stepSize: 1
                                            value: dialog.pendingCacheJpegQuality
                                            onMoved: dialog.pendingCacheJpegQuality = Math.round(value)
                                        }

                                        Label {
                                            Layout.preferredWidth: 36
                                            text: dialog.pendingCacheJpegQuality
                                            color: dialog.textColor
                                            font.family: dialog.dataFontFamily
                                            font.pixelSize: 15
                                            horizontalAlignment: Text.AlignRight
                                        }
                                    }
                                }

                                SettingsSection {
                                    Layout.fillWidth: true
                                    Layout.leftMargin: 34
                                    Layout.rightMargin: 34
                                    Layout.bottomMargin: 26
                                    title: qsTr("Maintenance")
                                    textColor: dialog.textColor
                                    mutedTextColor: dialog.mutedTextColor
                                    dividerColor: dialog.dividerColor

                                    RowLayout {
                                        Layout.fillWidth: true
                                        spacing: 12

                                        Button {
                                            id: clearProjectButton
                                            Layout.preferredHeight: 42
                                            text: qsTr("Clear current project")
                                            enabled: appModules.project.serviceReady
                                            palette.buttonText: dialog.textColor
                                            onClicked: {
                                                appModules.library.ClearProjectThumbnailDiskCache()
                                                dialog.refreshCacheStats()
                                                dialog.messageRequested(qsTr("Current project cache cleared"))
                                            }
                                        }

                                        Button {
                                            id: clearAllButton
                                            Layout.preferredHeight: 42
                                            text: qsTr("Clear all cache")
                                            palette.buttonText: dialog.dangerColor
                                            onClicked: {
                                                appModules.library.ClearAllThumbnailDiskCache()
                                                dialog.refreshCacheStats()
                                                dialog.messageRequested(qsTr("All thumbnail cache cleared"))
                                            }
                                        }

                                        Button {
                                            Layout.preferredHeight: 42
                                            text: qsTr("Refresh")
                                            palette.buttonText: dialog.textColor
                                            onClicked: dialog.refreshCacheStats()
                                        }
                                    }
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

                                            ComboBox {
                                                id: acceleratorCombo
                                                Layout.fillWidth: true
                                                Layout.preferredHeight: 44
                                                model: appModules.project.acceleratorOptions
                                                textRole: "label"
                                                valueRole: "value"
                                                enabled: appModules.project.acceleratorOptions.length > 0
                                                currentIndex: dialog.acceleratorIndexForValue(
                                                                  dialog.pendingAcceleratorBackend)
                                                onActivated: function(index) {
                                                    const options = appModules.project.acceleratorOptions
                                                    if (index >= 0 && index < options.length) {
                                                        dialog.pendingAcceleratorBackend =
                                                            String(options[index].value || "")
                                                    }
                                                }
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

    component CacheMetric: Rectangle {
        property string label: ""
        property string value: ""
        property color textColor: "white"
        property color mutedTextColor: "#999999"
        property color panelColor: "#111214"

        implicitHeight: 84
        radius: 8
        color: Qt.rgba(panelColor.r, panelColor.g, panelColor.b, 0.62)

        ColumnLayout {
            anchors.fill: parent
            anchors.margins: 14
            spacing: 6

            Label {
                Layout.fillWidth: true
                text: label
                color: mutedTextColor
                font.pixelSize: 12
                font.weight: 700
                elide: Text.ElideRight
            }

            Label {
                Layout.fillWidth: true
                text: value
                color: textColor
                font.family: dialog.dataFontFamily
                font.pixelSize: 18
                font.weight: 700
                elide: Text.ElideRight
            }
        }
    }
}
