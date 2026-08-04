import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts
import QtQuick.Effects
import "util"

SwipeView {
    id: panel

    property var profileController: null       // appModules.aiProviderProfiles
    property var analysisController: null      // appModules.imageAnalysis
    property var interactionPolicy: null
    property color primaryAccent: "#457B9D"
    property color secondaryAccent: "#9FC7D8"
    property color textColor: "#F5F1EA"
    property color mutedTextColor: "#B6B0A7"
    property color canvasColor: "#111214"
    property color dividerColor: Qt.rgba(1, 1, 1, 0.08)
    property color dangerColor: "#D96C75"
    property string dataFontFamily: appTheme.dataFontFamily
    property int dataRevision: 0
    property string editingProfileId: ""
    property string openAiOAuthStatusText: ""
    property bool openAiOAuthStatusIsError: false
    property string codexLoginPollProfileId: ""
    property int codexLoginPollRemaining: 0
    property Item backgroundSource: null
    readonly property bool hasProfilesController: profileController !== null
    readonly property bool hasAnalysis: analysisController !== null
    readonly property bool canChangeProvider: !interactionPolicy
                                               || interactionPolicy.canChangeImageAnalysisProvider
    readonly property var profiles: hasProfilesController ? profileController.profiles : []
    readonly property var templates: hasProfilesController ? profileController.templateOptions : []
    readonly property var editProfile: {
        dataRevision
        return hasProfilesController && editingProfileId.length > 0
                ? profileController.Profile(editingProfileId) : ({})
    }
    readonly property bool isOpenAiOAuthProfile: editProfile
                                                 && (editProfile.driver === "openai_codex_oauth"
                                                     || editProfile.basedOnTemplate === "openai_codex_oauth")
    readonly property var modelOptions: {
        dataRevision
        return hasProfilesController && editingProfileId.length > 0
                ? profileController.ModelOptions(editingProfileId) : []
    }
    signal messageRequested(string message)

    interactive: false
    clip: true
    currentIndex: 0

    Connections {
        target: panel.profileController
        function onProfilesChanged() {
            panel.dataRevision += 1
        }
    }

    Timer {
        id: codexLoginPollTimer
        interval: 3000
        repeat: true
        onTriggered: {
            if (panel.codexLoginPollRemaining <= 0 || panel.codexLoginPollProfileId.length === 0) {
                stop()
                panel.codexLoginPollProfileId = ""
                panel.setOpenAiOAuthStatus(qsTr("Codex login did not finish yet. Complete `codex login`, then use Codex Login again."), true)
                return
            }
            panel.codexLoginPollRemaining -= 1
            if (panel.tryImportOpenAiOAuth(panel.codexLoginPollProfileId, true)) {
                stop()
                panel.codexLoginPollProfileId = ""
            }
        }
    }

    function openEditor(profileId) {
        editingProfileId = profileId
        openAiOAuthStatusText = ""
        openAiOAuthStatusIsError = false
        currentIndex = 1
        tryImportOpenAiOAuth(profileId, true)
    }

    function setOpenAiOAuthStatus(message, isError) {
        openAiOAuthStatusText = message
        openAiOAuthStatusIsError = isError
    }

    function refreshOpenAiOAuthModels(profileId, quiet) {
        if (!hasAnalysis) {
            setOpenAiOAuthStatus(qsTr("Codex OAuth is connected. Open a project to refresh models."), false)
            return
        }
        if (!quiet) {
            setOpenAiOAuthStatus(qsTr("Loading Codex models..."), false)
        }
        analysisController.ValidateConnectionForProfile(profileId)
    }

    function tryImportOpenAiOAuth(profileId, quiet) {
        if (!hasProfilesController || profileId.length === 0) {
            return false
        }
        const profile = profileController.Profile(profileId)
        const isOAuth = profile
                        && (profile.driver === "openai_codex_oauth"
                            || profile.basedOnTemplate === "openai_codex_oauth")
        if (!isOAuth) {
            return false
        }
        if (profile.credentialAvailable) {
            refreshOpenAiOAuthModels(profileId, quiet)
            return true
        }
        if (typeof profileController.ImportCodexAuth !== "function") {
            setOpenAiOAuthStatus(qsTr("This build does not include Codex OAuth import. Rebuild Alcedo Studio."), true)
            return false
        }
        setOpenAiOAuthStatus(qsTr("Checking local Codex login..."), false)
        const err = profileController.ImportCodexAuth(profileId)
        if (err.length > 0) {
            setOpenAiOAuthStatus(err, true)
            if (!quiet) {
                messageRequested(err)
            }
            return false
        }
        setOpenAiOAuthStatus(qsTr("Codex OAuth connected. Loading models..."), false)
        if (!quiet) {
            messageRequested(qsTr("Codex OAuth connected. Loading models..."))
        }
        refreshOpenAiOAuthModels(profileId, true)
        return true
    }

    function languageIndexFor(value) {
        const options = languageModel
        for (let i = 0; i < options.length; ++i) {
            if (options[i].value === value) {
                return i
            }
        }
        return 0
    }

    function setField(field, value) {
        if (!hasProfilesController || editingProfileId.length === 0) {
            return
        }
        if (!canChangeProvider) {
            messageRequested(qsTr("Finish the current AI task before changing provider settings."))
            return
        }
        if (!profileController.SetProfileField(editingProfileId, field, value)) {
            messageRequested(qsTr("The field value could not be saved."))
        }
    }

    function withAlpha(color, alpha) {
        return Qt.rgba(color.r, color.g, color.b, alpha)
    }

    // Reusable rounded-rectangle action button. Primary = blue (confirm) bg with
    // white text, danger = destructive bg with white text, otherwise gray bg with
    // white text. Optional leading SVG icon. Content-sized so labels never elide.
    component AiButton: Button {
        id: aiBtn
        property bool primary: false
        property bool danger: false
        property bool iconOnly: false
        property int iconSize: 16
        property string iconSrc: ""

        Layout.preferredHeight: 40
        font.pixelSize: 14
        font.weight: 700
        Material.foreground: panel.textColor
        hoverEnabled: true
        spacing: 8
        display: aiBtn.iconOnly ? AbstractButton.IconOnly : AbstractButton.TextBesideIcon
        leftPadding: aiBtn.iconOnly ? 0 : 18
        rightPadding: aiBtn.iconOnly ? 0 : 18
        topPadding: aiBtn.iconOnly ? 0 : 8
        bottomPadding: aiBtn.iconOnly ? 0 : 8
        icon.source: aiBtn.iconSrc.length > 0 ? aiBtn.iconSrc : ""
        icon.color: panel.textColor
        icon.width: aiBtn.iconSize
        icon.height: aiBtn.iconSize
        ToolTip.visible: aiBtn.hovered && aiBtn.iconOnly && aiBtn.text.length > 0
        ToolTip.delay: 400
        ToolTip.text: aiBtn.text

        background: Rectangle {
            radius: 10
            color: aiBtn.primary
                   ? (aiBtn.down
                      ? Qt.darker(panel.primaryAccent, 1.16)
                      : (aiBtn.hovered ? Qt.lighter(panel.primaryAccent, 1.06)
                                        : panel.primaryAccent))
                   : aiBtn.danger
                     ? (aiBtn.down
                        ? Qt.darker(panel.dangerColor, 1.16)
                        : (aiBtn.hovered ? Qt.lighter(panel.dangerColor, 1.06)
                                          : panel.dangerColor))
                     : (aiBtn.down
                        ? Qt.rgba(1, 1, 1, 0.06)
                        : (aiBtn.hovered ? Qt.rgba(1, 1, 1, 0.16)
                                          : Qt.rgba(1, 1, 1, 0.10)))
            border.width: 1
            border.color: aiBtn.primary
                          ? Qt.rgba(panel.secondaryAccent.r,
                                    panel.secondaryAccent.g,
                                    panel.secondaryAccent.b, 0.18)
                          : aiBtn.danger
                            ? Qt.rgba(panel.dangerColor.r,
                                      panel.dangerColor.g,
                                      panel.dangerColor.b, 0.30)
                            : Qt.rgba(1, 1, 1, 0.12)
            opacity: aiBtn.enabled ? 1.0 : 0.45
        }
    }

    // Blurred modal backdrop shared by the panel's dialogs — matches the
    // MultiEffect + overlayColor used by SettingDialog.
    component BlurOverlay: Item {
        anchors.fill: parent

        MultiEffect {
            visible: panel.backgroundSource !== null
            anchors.fill: parent
            source: panel.backgroundSource
            blurEnabled: true
            blur: 0.6
            blurMax: 64
            saturation: -0.2
            brightness: -0.08
        }

        Rectangle {
            anchors.fill: parent
            color: appTheme.overlayColor
        }

        MouseArea {
            anchors.fill: parent
            hoverEnabled: true
        }
    }

    readonly property var languageModel: [
        { label: qsTr("Follow app language"), value: "follow" },
        { label: qsTr("English"), value: "en" },
        { label: qsTr("中文"), value: "zh" }
    ]

    Item {
        id: listPage

        ColumnLayout {
            anchors.fill: parent
            anchors.leftMargin: 34
            anchors.rightMargin: 34
            anchors.topMargin: 26
            anchors.bottomMargin: 26
            spacing: 18

            SettingsSection {
                Layout.fillWidth: true
                title: qsTr("Output language")

                ComboBox {
                    Layout.fillWidth: true
                    Layout.preferredHeight: 42
                    enabled: panel.hasProfilesController && panel.canChangeProvider
                    model: panel.languageModel
                    textRole: "label"
                    currentIndex: panel.languageIndexFor(panel.hasProfilesController ? panel.profileController.outputLanguage : "follow")
                    onActivated: function(index) {
                        if (panel.hasProfilesController) {
                            panel.profileController.SetOutputLanguage(model[index].value)
                        }
                    }
                }
            }

            // Provider settings — manual header so the icon-only Add button
            // sits inline with the title. SettingsSection has no header slot,
            // and routing an icon button through a Loader would break the
            // square Layout sizing, so the header is built in place.
            ColumnLayout {
                Layout.fillWidth: true
                Layout.fillHeight: true
                spacing: 12

                RowLayout {
                    Layout.fillWidth: true
                    spacing: 10

                    Label {
                        Layout.fillWidth: true
                        text: qsTr("Provider settings")
                        color: panel.textColor
                        font.pixelSize: 18
                        font.weight: 800
                    }

                    IconButton {
                        buttonSize: 40
                        iconSize: 16
                        kind: "accent"
                        accentColor: panel.primaryAccent
                        bordered: true
                        borderColor: Qt.rgba(panel.secondaryAccent.r,
                                             panel.secondaryAccent.g,
                                             panel.secondaryAccent.b, 0.18)
                        iconSrc: "qrc:/panel_icons/plus.svg"
                        tooltipText: qsTr("Add provider")
                        enabled: panel.hasProfilesController && panel.canChangeProvider
                        onClicked: addDialog.open()
                    }
                }

                Rectangle {
                    Layout.fillWidth: true
                    Layout.preferredHeight: 1
                    color: panel.dividerColor
                }

                Item {
                    Layout.fillWidth: true
                    Layout.fillHeight: true

                    ListView {
                        id: cardList
                        anchors.fill: parent
                        clip: true
                        spacing: 12
                        boundsBehavior: Flickable.StopAtBounds
                        model: panel.profiles

                        delegate: Rectangle {
                            width: ListView.view.width
                            height: 92
                            radius: 8
                            color: modelData.active
                                   ? Qt.rgba(panel.primaryAccent.r, panel.primaryAccent.g, panel.primaryAccent.b, 0.14)
                                   : (cardMouse.containsMouse ? Qt.rgba(1, 1, 1, 0.06) : Qt.rgba(0, 0, 0, 0.16))
                            border.width: 1
                            border.color: modelData.active
                                          ? Qt.rgba(panel.secondaryAccent.r, panel.secondaryAccent.g, panel.secondaryAccent.b, 0.72)
                                          : Qt.rgba(1, 1, 1, 0.08)

                            RowLayout {
                                anchors.fill: parent
                                anchors.leftMargin: 18
                                anchors.rightMargin: 14
                                spacing: 14

                                ColumnLayout {
                                    Layout.fillWidth: true
                                    spacing: 5

                                    Label {
                                        Layout.fillWidth: true
                                        text: modelData.displayName
                                        color: panel.textColor
                                        font.pixelSize: 16
                                        font.weight: 800
                                        elide: Text.ElideRight
                                    }

                                    Label {
                                        Layout.fillWidth: true
                                        text: modelData.driver === "openai_codex_oauth"
                                              ? qsTr("ChatGPT / Codex OAuth")
                                              : modelData.baseUrl
                                        color: panel.mutedTextColor
                                        font.family: panel.dataFontFamily
                                        font.pixelSize: 12
                                        elide: Text.ElideMiddle
                                    }

                                    Label {
                                        Layout.fillWidth: true
                                        text: modelData.modelDisplayName && modelData.modelDisplayName.length > 0
                                              ? modelData.modelDisplayName : modelData.modelId
                                        color: panel.secondaryAccent
                                        font.family: panel.dataFontFamily
                                        font.pixelSize: 11
                                        elide: Text.ElideRight
                                    }
                                }

                                IconButton {
                                    buttonSize: 40
                                    iconSize: 16
                                    kind: modelData.active ? "normal" : "accent"
                                    accentColor: panel.primaryAccent
                                    bordered: true
                                    borderColor: modelData.active
                                                 ? Qt.rgba(1, 1, 1, 0.12)
                                                 : Qt.rgba(panel.secondaryAccent.r,
                                                           panel.secondaryAccent.g,
                                                           panel.secondaryAccent.b, 0.18)
                                    iconSrc: modelData.active
                                             ? "qrc:/panel_icons/stop.svg"
                                             : "qrc:/panel_icons/play.svg"
                                    tooltipText: modelData.active ? qsTr("In use") : qsTr("Use")
                                    enabled: !modelData.active && panel.hasProfilesController
                                             && panel.canChangeProvider
                                    onClicked: panel.profileController.SetActiveProfile(modelData.uuid)
                                }

                                IconButton {
                                    buttonSize: 40
                                    iconSize: 16
                                    kind: "normal"
                                    bordered: true
                                    iconSrc: "qrc:/panel_icons/edit.svg"
                                    tooltipText: qsTr("Edit")
                                    enabled: panel.hasProfilesController && panel.canChangeProvider
                                    onClicked: panel.openEditor(modelData.uuid)
                                }
                            }

                            MouseArea {
                                id: cardMouse
                                anchors.fill: parent
                                hoverEnabled: true
                                acceptedButtons: Qt.NoButton
                            }
                        }
                    }

                    ColumnLayout {
                        anchors.centerIn: parent
                        width: Math.min(parent.width - 40, 420)
                        visible: panel.profiles.length === 0
                        spacing: 8

                        Label {
                            Layout.fillWidth: true
                            text: qsTr("No provider profiles")
                            color: panel.textColor
                            font.pixelSize: 18
                            font.weight: 800
                            horizontalAlignment: Text.AlignHCenter
                        }

                        Label {
                            Layout.fillWidth: true
                            text: qsTr("Use the + button above to create one.")
                            color: panel.mutedTextColor
                            font.pixelSize: 13
                            horizontalAlignment: Text.AlignHCenter
                        }
                    }
                }
            }
        }

        Dialog {
            id: addDialog
            parent: Overlay.overlay
            modal: true
            focus: true
            closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
            x: parent ? Math.round((parent.width - width) / 2) : 0
            y: parent ? Math.round((parent.height - height) / 2) : 0
            width: Math.min((parent ? parent.width : 620) - 72, 560)
            padding: 24

            Overlay.modal: Component { BlurOverlay {} }

            background: Rectangle {
                radius: 14
                color: appTheme.toneGraphite
                border.width: 0
            }

            contentItem: ColumnLayout {
                spacing: 16

                Label {
                    Layout.fillWidth: true
                    text: qsTr("Add provider")
                    color: appTheme.textColor
                    font.family: appTheme.headlineFontFamily
                    font.pixelSize: 22
                    font.weight: 700
                }

                Label {
                    Layout.fillWidth: true
                    text: qsTr("Choose a template to create a profile from.")
                    color: appTheme.textMutedColor
                    font.pixelSize: 13
                    font.weight: 500
                    wrapMode: Text.WordWrap
                }

                Flow {
                    Layout.fillWidth: true
                    spacing: 10

                    Repeater {
                        model: panel.templates
                        delegate: Rectangle {
                            id: chip
                            readonly property string templateId: modelData.templateId
                            readonly property string chipLabel: modelData.label

                            height: 40
                            radius: 20
                            implicitWidth: Math.min(chipLbl.implicitWidth + 36, 280)
                            color: chipHit.containsMouse
                                   ? panel.withAlpha(appTheme.hoverColor, 0.78)
                                   : panel.withAlpha(appTheme.bgBaseColor, 0.62)
                            border.width: 1
                            border.color: panel.withAlpha(appTheme.glassStrokeColor, 0.35)

                            MouseArea {
                                id: chipHit
                                anchors.fill: parent
                                cursorShape: Qt.PointingHandCursor
                                hoverEnabled: true
                                onClicked: {
                                    if (!panel.canChangeProvider) {
                                        panel.messageRequested(qsTr("Finish the current AI task before changing provider settings."))
                                        return
                                    }
                                    const id = panel.profileController.AddProfileFromTemplate(chip.templateId)
                                    addDialog.close()
                                    if (id.length > 0) {
                                        panel.openEditor(id)
                                        if (chip.templateId === "openai_codex_oauth") {
                                            panel.tryImportOpenAiOAuth(id, false)
                                        }
                                    }
                                }
                            }

                            Label {
                                id: chipLbl
                                anchors.centerIn: parent
                                width: Math.min(implicitWidth, 244)
                                text: chip.chipLabel
                                color: appTheme.textColor
                                font.family: appTheme.uiFontFamily
                                font.pixelSize: 13
                                font.weight: 600
                                elide: Text.ElideRight
                                horizontalAlignment: Text.AlignHCenter
                            }
                        }
                    }
                }

                Item { Layout.fillWidth: true }
            }
        }
    }

    Item {
        id: editPage

        ColumnLayout {
            anchors.fill: parent
            spacing: 0

            RowLayout {
                Layout.fillWidth: true
                Layout.preferredHeight: 74
                Layout.leftMargin: 24
                Layout.rightMargin: 28
                spacing: 14

                Button {
                    Layout.preferredWidth: 42
                    Layout.preferredHeight: 42
                    flat: true
                    text: "‹"
                    font.pixelSize: 30
                    Material.foreground: panel.mutedTextColor
                    onClicked: panel.currentIndex = 0
                }

                Label {
                    Layout.fillWidth: true
                    text: panel.editProfile.displayName || qsTr("Provider")
                    color: panel.textColor
                    font.pixelSize: 22
                    font.weight: 800
                    elide: Text.ElideRight
                }
            }

            Rectangle {
                Layout.fillWidth: true
                Layout.preferredHeight: 1
                color: panel.dividerColor
            }

            ScrollView {
                id: editScroll
                Layout.fillWidth: true
                Layout.fillHeight: true
                contentWidth: availableWidth
                clip: true

                ColumnLayout {
                    width: editScroll.availableWidth
                    spacing: 18

                    SettingsSection {
                        Layout.fillWidth: true
                        Layout.topMargin: 20
                        Layout.leftMargin: 34
                        Layout.rightMargin: 34
                        title: qsTr("OpenAI OAuth")
                        visible: panel.isOpenAiOAuthProfile
                        enabled: visible
                        Layout.preferredHeight: visible ? implicitHeight : 0

                        RowLayout {
                            Layout.fillWidth: true
                            spacing: 12

                            AiButton {
                                Layout.preferredHeight: 38
                                primary: true
                                text: qsTr("Use Codex Login")
                                enabled: panel.hasProfilesController && panel.canChangeProvider
                                onClicked: {
                                    panel.tryImportOpenAiOAuth(panel.editingProfileId, false)
                                }
                            }

                            AiButton {
                                Layout.preferredHeight: 38
                                text: qsTr("Open Login")
                                enabled: panel.canChangeProvider
                                onClicked: {
                                    if (typeof panel.profileController.OpenCodexLogin !== "function") {
                                        panel.setOpenAiOAuthStatus(qsTr("This build does not include Codex login launch. Run `codex login` manually."), true)
                                        return
                                    }
                                    const err = panel.profileController.OpenCodexLogin()
                                    if (err.length > 0) {
                                        panel.setOpenAiOAuthStatus(err, true)
                                        panel.messageRequested(err)
                                        return
                                    }
                                    panel.setOpenAiOAuthStatus(qsTr("Codex login started. Complete the browser flow; models will load when login finishes."), false)
                                    panel.codexLoginPollProfileId = panel.editingProfileId
                                    panel.codexLoginPollRemaining = 40
                                    codexLoginPollTimer.restart()
                                }
                            }

                            AiButton {
                                Layout.preferredHeight: 38
                                danger: true
                                text: qsTr("Disconnect")
                                enabled: panel.editProfile.credentialAvailable === true
                                         && panel.canChangeProvider
                                onClicked: {
                                    panel.profileController.DeleteApiKey(panel.editingProfileId)
                                    panel.messageRequested(qsTr("Codex OAuth disconnected"))
                                }
                            }
                        }

                        Label {
                            Layout.fillWidth: true
                            text: panel.openAiOAuthStatusText.length > 0
                                  ? panel.openAiOAuthStatusText
                                  : panel.editProfile.credentialAvailable
                                  ? (panel.editProfile.maskedKeyLabel && panel.editProfile.maskedKeyLabel.length > 0
                                     ? panel.editProfile.maskedKeyLabel : qsTr("Codex OAuth connected"))
                                  : qsTr("No Codex OAuth credential imported")
                            color: panel.openAiOAuthStatusText.length > 0
                                   ? (panel.openAiOAuthStatusIsError ? panel.dangerColor : panel.secondaryAccent)
                                   : panel.editProfile.credentialAvailable ? panel.secondaryAccent : panel.mutedTextColor
                            font.family: panel.dataFontFamily
                            font.pixelSize: 12
                            wrapMode: Text.WordWrap
                        }
                    }

                    SettingsSection {
                        Layout.fillWidth: true
                        Layout.topMargin: 20
                        Layout.leftMargin: 34
                        Layout.rightMargin: 34
                        title: qsTr("API key")
                        visible: panel.editProfile.credentialRequired !== false
                                 && !panel.isOpenAiOAuthProfile
                        enabled: visible
                        Layout.preferredHeight: visible ? implicitHeight : 0

                        TextField {
                            id: keyField
                            Layout.fillWidth: true
                            Layout.preferredHeight: 58
                            echoMode: TextInput.Password
                            placeholderText: qsTr("Paste API key")
                            color: panel.textColor
                            enabled: panel.hasProfilesController && panel.canChangeProvider
                            font.family: panel.dataFontFamily
                            selectByMouse: true
                        }

                        RowLayout {
                            Layout.fillWidth: true
                            spacing: 12

                            AiButton {
                                Layout.preferredHeight: 38
                                primary: true
                                text: qsTr("Save Key")
                                enabled: panel.hasProfilesController && panel.canChangeProvider
                                         && keyField.text.length > 0
                                onClicked: {
                                    const err = panel.profileController.SaveApiKey(panel.editingProfileId, keyField.text)
                                    keyField.text = ""
                                    panel.messageRequested(err.length > 0 ? err : qsTr("API key saved"))
                                }
                            }

                            AiButton {
                                Layout.preferredHeight: 38
                                danger: true
                                text: qsTr("Delete Key")
                                enabled: panel.editProfile.credentialAvailable === true
                                         && panel.canChangeProvider
                                onClicked: {
                                    panel.profileController.DeleteApiKey(panel.editingProfileId)
                                    panel.messageRequested(qsTr("API key deleted"))
                                }
                            }

                            Label {
                                Layout.fillWidth: true
                                text: panel.editProfile.credentialAvailable
                                      ? (panel.editProfile.maskedKeyLabel && panel.editProfile.maskedKeyLabel.length > 0
                                         ? panel.editProfile.maskedKeyLabel : qsTr("Key saved"))
                                      : qsTr("No key saved")
                                color: panel.editProfile.credentialAvailable ? panel.secondaryAccent : panel.mutedTextColor
                                font.family: panel.dataFontFamily
                                font.pixelSize: 12
                                elide: Text.ElideRight
                            }
                        }
                    }

                    SettingsSection {
                        Layout.fillWidth: true
                        Layout.leftMargin: 34
                        Layout.rightMargin: 34
                        title: qsTr("Model")

                        RowLayout {
                            Layout.fillWidth: true
                            spacing: 12

                            SearchComboBox {
                                id: modelCombo
                                Layout.fillWidth: true
                                Layout.preferredHeight: 42
                                enabled: panel.modelOptions.length > 0 && panel.canChangeProvider
                                options: panel.modelOptions
                                selectedValue: panel.editProfile.modelId || ""
                                selectedLabel: panel.editProfile.modelDisplayName || ""
                                valueRole: "modelId"
                                labelRole: "displayName"
                                subtitleRole: "modelId"
                                placeholderText: qsTr("Search models")
                                emptyText: qsTr("No matching models")
                                textColor: panel.textColor
                                mutedTextColor: panel.mutedTextColor
                                accentColor: panel.secondaryAccent
                                focusedBorderColor: Qt.rgba(panel.secondaryAccent.r,
                                                            panel.secondaryAccent.g,
                                                            panel.secondaryAccent.b, 0.62)
                                popupColor: appTheme.toneGraphite
                                dataFontFamily: panel.dataFontFamily
                                onItemSelected: function(item) {
                                    panel.setField("modelId", item.modelId)
                                    panel.setField("modelDisplayName", item.displayName)
                                }
                            }

                            IconButton {
                                buttonSize: modelCombo.height
                                iconSize: 16
                                Layout.alignment: Qt.AlignBottom
                                kind: "normal"
                                bordered: true
                                iconSrc: "qrc:/panel_icons/retry.svg"
                                tooltipText: qsTr("Test & Refresh")
                                enabled: panel.hasAnalysis && panel.canChangeProvider
                                         && panel.editingProfileId.length > 0
                                         && (panel.editProfile.credentialAvailable || panel.editProfile.authType === "none")
                                onClicked: panel.analysisController.ValidateConnectionForProfile(panel.editingProfileId)
                            }
                        }

                        Label {
                            Layout.fillWidth: true
                            visible: panel.hasAnalysis && (panel.analysisController.connectionStatus.length > 0
                                      || panel.analysisController.lastError.length > 0)
                            text: panel.hasAnalysis
                                  ? (panel.analysisController.lastError.length > 0
                                     ? panel.analysisController.lastError
                                     : panel.analysisController.connectionStatus)
                                  : ""
                            color: panel.hasAnalysis && panel.analysisController.lastError.length > 0 ? panel.dangerColor : panel.secondaryAccent
                            font.pixelSize: 12
                            wrapMode: Text.WordWrap
                        }
                    }

                    SettingsSection {
                        Layout.fillWidth: true
                        Layout.leftMargin: 34
                        Layout.rightMargin: 34
                        title: qsTr("Advanced")
                        visible: !panel.isOpenAiOAuthProfile
                        enabled: visible
                        Layout.preferredHeight: visible ? implicitHeight : 0

                        GridLayout {
                            Layout.fillWidth: true
                            columns: panel.width > 760 ? 2 : 1
                            columnSpacing: 12
                            rowSpacing: 10

                            AdvancedField { label: qsTr("Display name"); field: "displayName"; value: panel.editProfile.displayName || "" }
                            AdvancedField { label: qsTr("Provider id"); field: "providerId"; value: panel.editProfile.providerId || "" }
                            AdvancedField { label: qsTr("Driver"); field: "driver"; value: panel.editProfile.driver || "" }
                            AdvancedField { label: qsTr("Base URL"); field: "baseUrl"; value: panel.editProfile.baseUrl || "" }
                            AdvancedField { label: qsTr("Endpoint"); field: "endpoint"; value: panel.editProfile.endpoint || "" }
                            AdvancedField { label: qsTr("Models endpoint"); field: "modelsEndpoint"; value: panel.editProfile.modelsEndpoint || "" }
                            AdvancedField { label: qsTr("Models response data pointer"); field: "modelsResponseDataJsonPointer"; value: panel.editProfile.modelsResponseDataJsonPointer || "" }
                            AdvancedField { label: qsTr("Auth type"); field: "authType"; value: panel.editProfile.authType || "" }
                            AdvancedField { label: qsTr("Credential slot"); field: "credentialSlot"; value: panel.editProfile.credentialSlot || "" }
                            AdvancedField { label: qsTr("Structured output"); field: "structuredOutputMode"; value: panel.editProfile.structuredOutputMode || "" }
                            AdvancedField { label: qsTr("Timeout ms"); field: "timeoutMs"; value: String(panel.editProfile.timeoutMs || 60000); numeric: true }
                            AdvancedField { label: qsTr("Max image bytes"); field: "maxImageBytes"; value: String(panel.editProfile.maxImageBytes || 4194304); numeric: true }
                            AdvancedField { label: qsTr("Recommended rendition"); field: "recommendedRendition"; value: panel.editProfile.recommendedRendition || "preview" }
                        }
                    }

                    RowLayout {
                        Layout.fillWidth: true
                        Layout.leftMargin: 34
                        Layout.rightMargin: 34
                        Layout.bottomMargin: 28
                        spacing: 12

                        AiButton {
                            Layout.preferredHeight: 40
                            text: qsTr("Duplicate")
                            enabled: panel.hasProfilesController && panel.canChangeProvider
                                     && panel.editingProfileId.length > 0
                            onClicked: {
                                const id = panel.profileController.CloneProfile(panel.editingProfileId)
                                if (id.length > 0) {
                                    panel.openEditor(id)
                                }
                            }
                        }

                        AiButton {
                            Layout.preferredHeight: 40
                            danger: true
                            text: qsTr("Delete")
                            enabled: panel.hasProfilesController && panel.canChangeProvider
                                     && panel.editingProfileId.length > 0
                            onClicked: deleteDialog.open()
                        }

                        Item { Layout.fillWidth: true }
                    }
                }
            }
        }

        Dialog {
            id: deleteDialog
            parent: Overlay.overlay
            modal: true
            focus: true
            x: parent ? Math.round((parent.width - width) / 2) : 0
            y: parent ? Math.round((parent.height - height) / 2) : 0
            width: Math.min((parent ? parent.width : 520) - 72, 460)
            padding: 24

            Overlay.modal: Component { BlurOverlay {} }

            background: Rectangle {
                radius: 14
                color: appTheme.toneGraphite
                border.width: 0
            }

            contentItem: ColumnLayout {
                spacing: 14

                Label {
                    Layout.fillWidth: true
                    text: qsTr("Delete provider")
                    color: appTheme.textColor
                    font.family: appTheme.headlineFontFamily
                    font.pixelSize: 20
                    font.weight: 700
                }

                Label {
                    Layout.fillWidth: true
                    text: qsTr("Delete this provider profile?")
                    color: appTheme.textMutedColor
                    font.pixelSize: 13
                    font.weight: 500
                    wrapMode: Text.WordWrap
                }

                CheckBox {
                    id: wipeKeyCheck
                    checked: true
                    text: qsTr("Delete saved key")
                    Material.foreground: appTheme.textColor
                }

                RowLayout {
                    Layout.fillWidth: true
                    spacing: 10

                    Item { Layout.fillWidth: true }

                    AiButton {
                        text: qsTr("Cancel")
                        onClicked: deleteDialog.close()
                    }

                    AiButton {
                        danger: true
                        text: qsTr("Delete")
                        onClicked: {
                            panel.profileController.DeleteProfile(panel.editingProfileId, wipeKeyCheck.checked)
                            deleteDialog.close()
                            panel.editingProfileId = ""
                            panel.currentIndex = 0
                        }
                    }
                }
            }
        }
    }

    component SettingsSection: ColumnLayout {
        property string title: ""
        spacing: 12

        Label {
            Layout.fillWidth: true
            text: title
            color: panel.textColor
            font.pixelSize: 18
            font.weight: 800
        }
        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: 1
            color: panel.dividerColor
        }
    }

    component AdvancedField: ColumnLayout {
        property string label: ""
        property string field: ""
        property string value: ""
        property bool numeric: false

        Layout.fillWidth: true
        Layout.minimumWidth: 220
        spacing: 5

        Label {
            text: label
            color: panel.mutedTextColor
            font.pixelSize: 11
            font.weight: 700
        }

        TextField {
            Layout.fillWidth: true
            Layout.preferredHeight: 40
            text: value
            color: panel.textColor
            font.family: panel.dataFontFamily
            enabled: panel.canChangeProvider
            selectByMouse: true
            onEditingFinished: {
                panel.setField(field, numeric ? parseInt(text, 10) : text)
            }
        }
    }
}
