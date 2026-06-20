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
    closePolicy: Popup.CloseOnEscape
    padding: 0
    width: parent ? parent.width : 0
    height: parent ? parent.height : 0
    x: 0
    y: 0

    property Item blurSource: null
    property var languageOptions: []
    property color primaryAccent: "#457B9D"
    property color secondaryAccent: "#9FC7D8"
    property color textColor: "#F5F1EA"
    property color mutedTextColor: "#B6B0A7"
    property color panelColor: "#1C1C1D"
    property color canvasColor: "#111214"
    property color overlayColor: Qt.rgba(11 / 255, 12 / 255, 14 / 255, 0.60)
    property color hoverColor: Qt.rgba(1, 1, 1, 0.07)
    property color dividerColor: Qt.rgba(1, 1, 1, 0.08)
    property color dangerColor: "#D96C75"
    property color panelBorderColor: Qt.rgba(1, 1, 1, 0.08)
    property string headlineFontFamily: appTheme.headlineFontFamily
    readonly property string dataFontFamily: appTheme.dataFontFamily
    property real cornerRadius: 0

    property int currentCategory: 0
    property int pendingThemeIndex: appTheme.currentThemeIndex
    property string pendingLanguageCode: languageManager.currentLanguageCode
    property bool pendingCacheEnabled: albumBackend.thumbnailDiskCacheEnabled
    property string pendingCacheRoot: albumBackend.thumbnailDiskCacheRoot
    property int pendingCacheMaxEntries: albumBackend.thumbnailDiskCacheMaxEntries
    property int pendingCacheJpegQuality: albumBackend.thumbnailDiskCacheJpegQuality
    property string pendingSemanticImportPreference: albumBackend.semanticGenerationController.importPreference
    property string cacheStatsSnapshot: ""
    property int requestedCategory: 0

    signal messageRequested(string message)

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
            albumBackend.semanticGenerationController.RefreshAlbumSummary()
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
        pendingCacheEnabled = albumBackend.thumbnailDiskCacheEnabled
        pendingCacheRoot = albumBackend.thumbnailDiskCacheRoot
        pendingCacheMaxEntries = albumBackend.thumbnailDiskCacheMaxEntries
        pendingCacheJpegQuality = albumBackend.thumbnailDiskCacheJpegQuality
        pendingSemanticImportPreference = albumBackend.semanticGenerationController.importPreference
        refreshCacheStats()
    }

    function refreshCacheStats() {
        cacheStatsSnapshot = albumBackend.thumbnailDiskCacheStats
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

    function applySettings() {
        if (appTheme.currentThemeIndex !== pendingThemeIndex) {
            appTheme.currentThemeIndex = pendingThemeIndex
        }
        if (languageManager.currentLanguageCode !== pendingLanguageCode) {
            languageManager.setLanguage(pendingLanguageCode)
        }
        if (albumBackend.thumbnailDiskCacheEnabled !== pendingCacheEnabled) {
            albumBackend.SetThumbnailDiskCacheEnabled(pendingCacheEnabled)
        }
        if (albumBackend.thumbnailDiskCacheRoot !== pendingCacheRoot) {
            albumBackend.SetThumbnailDiskCacheRoot(pendingCacheRoot)
        }
        if (albumBackend.thumbnailDiskCacheMaxEntries !== pendingCacheMaxEntries) {
            albumBackend.SetThumbnailDiskCacheMaxEntries(pendingCacheMaxEntries)
        }
        if (albumBackend.thumbnailDiskCacheJpegQuality !== pendingCacheJpegQuality) {
            albumBackend.SetThumbnailDiskCacheJpegQuality(pendingCacheJpegQuality)
        }
        if (albumBackend.semanticGenerationController.importPreference !== pendingSemanticImportPreference) {
            albumBackend.semanticGenerationController.SetImportPreference(pendingSemanticImportPreference)
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

    background: Item {}

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
                                    { label: qsTr("AI"), icon: "qrc:/panel_icons/search.svg" }
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

                            Label {
                                text: dialog.currentCategory === 0
                                      ? qsTr("Language")
                                      : (dialog.currentCategory === 1
                                         ? qsTr("Theme and color")
                                         : (dialog.currentCategory === 2
                                            ? qsTr("Cache")
                                            : qsTr("AI")))
                                color: dialog.textColor
                                font.family: dialog.headlineFontFamily
                                font.pixelSize: 34
                                font.weight: 800
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
                                            Material.foreground: dialog.textColor
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
                                            enabled: albumBackend.serviceReady
                                            Material.foreground: dialog.textColor
                                            onClicked: {
                                                albumBackend.ClearProjectThumbnailDiskCache()
                                                dialog.refreshCacheStats()
                                                dialog.messageRequested(qsTr("Current project cache cleared"))
                                            }
                                        }

                                        Button {
                                            id: clearAllButton
                                            Layout.preferredHeight: 42
                                            text: qsTr("Clear all cache")
                                            Material.foreground: dialog.dangerColor
                                            onClicked: {
                                                albumBackend.ClearAllThumbnailDiskCache()
                                                dialog.refreshCacheStats()
                                                dialog.messageRequested(qsTr("All thumbnail cache cleared"))
                                            }
                                        }

                                        Button {
                                            Layout.preferredHeight: 42
                                            text: qsTr("Refresh")
                                            Material.foreground: dialog.textColor
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
                                    semanticController: albumBackend.semanticGenerationController
                                    downloadController: albumBackend.modelDownloadController
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
                                }
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
                            font.pixelSize: 15
                            font.weight: 800
                            Material.foreground: dialog.textColor
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
