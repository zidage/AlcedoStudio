pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts

// Settings > About, laid out as bento tiles like Settings > Local Content Recognition (#824).
// Hosted as the last StackLayout child of SettingDialog.qml, inside its ScrollView.
// - Row 1: the Alcedo Studio wordmark of the main window with the copyright and the license,
//   the Sponsor, GitHub, and Docs buttons, and the Version tile (CountTile) with the build.
// - Row 2: Open Log Folder (the only log control of Settings), the AI model download statement
//   (read-only), and the third-party licenses, which open as a list in the same tile.
// - Row 3: the acknowledgements and components of README.md "Acknowledgments", one row per
//   subject with its source links, and the trademark notice. AboutAcknowledgementsTest checks
//   that every link of that README section is on this page.
// Software updates live in UpdatesSettingsPanel.qml.
ColumnLayout {
    id: page
    objectName: "aboutPage"

    property var updateService: null
    // LogFolderController (appModules.logFolder): opens the log directory in the file manager.
    property var logFolder: null
    // Collapsed by default so the long license list is not front and center.
    property bool licensesExpanded: false

    readonly property string appVersion: appTheme.appVersion
    readonly property string docsUrl: "https://zidage.github.io/AlcedoStudio_docs/"
    readonly property string repoUrl: "https://github.com/zidage/AlcedoStudio"
    readonly property string sponsorUrl: "https://github.com/zidage/AlcedoStudio/discussions/44"

    // README.md "Acknowledgments": one entry per subject. `detail` is shown in parentheses after
    // the link name.
    readonly property var acknowledgements: [
        { subject: qsTr("Film simulation LUTs"), links: [
            { name: "JanLohse/spectral_film_lut", url: "https://github.com/JanLohse/spectral_film_lut" },
            { name: "andreavolpato/spektrafilm", url: "https://github.com/andreavolpato/spektrafilm" } ] },
        { subject: qsTr("Film Print spectral simulation"), links: [
            { name: "andreavolpato/spektrafilm", url: "https://github.com/andreavolpato/spektrafilm" },
            { name: "DhaliwalX/fotufilm-engine", url: "https://github.com/DhaliwalX/fotufilm-engine" } ] },
        { subject: qsTr("Film Print film data"), links: [
            { name: "spectral_film_lut", url: "https://github.com/JanLohse/spectral_film_lut" },
            { name: "fotufilm Astia 100F", detail: "CC BY-SA 4.0",
              url: "https://github.com/DhaliwalX/fotufilm-engine" } ] },
        { subject: qsTr("AI Mask point, path, and box selection"), links: [
            { name: "SAM 2.1", detail: "Hiera tiny", url: "https://github.com/facebookresearch/sam2" } ] },
        { subject: qsTr("AI Mask Subject matting"), links: [
            { name: "S3OD", url: "https://github.com/KupynOrest/s3od" },
            { name: "s3od-synth weights", url: "https://huggingface.co/okupyn/s3od-synth" },
            { name: "DINOv3", url: "https://github.com/facebookresearch/dinov3" } ] },
        { subject: qsTr("Camera color matrices"), links: [
            { name: "rawtoaces-data", url: "https://github.com/AcademySoftwareFoundation/rawtoaces-data" } ] },
        { subject: qsTr("Neural demosaicing"), links: [
            { name: "mgharbi/demosaicnet", url: "https://github.com/mgharbi/demosaicnet" },
            { name: "Gharbi et al., 2016", url: "https://groups.csail.mit.edu/graphics/demosaicnet/" } ] },
        { subject: qsTr("Inpaint-opposed highlight reconstruction"), links: [
            { name: "darktable",
              url: "https://github.com/darktable-org/darktable/blob/master/src/iop/hlreconstruct/opposed.c" },
            { name: "RawTherapee",
              url: "https://github.com/RawTherapee/RawTherapee/blob/dev/rtengine/hilite_recon.cc" } ] },
        { subject: qsTr("RCD demosaicing"), links: [
            { name: "LuisSR/RCD-Demosaicing", url: "https://github.com/LuisSR/RCD-Demosaicing" } ] },
        { subject: qsTr("OpenDRT"), links: [
            { name: "jedypod/open-display-transform",
              url: "https://github.com/jedypod/open-display-transform" } ] },
        { subject: qsTr("ACES 2.0 output transforms"), links: [
            { name: "aces-aswf/aces-core", url: "https://github.com/aces-aswf/aces-core" } ] },
        { subject: qsTr("Film grain"), links: [
            { name: "Realistic Film Grain Rendering", detail: "IPOL 2017",
              url: "https://doi.org/10.5201/ipol.2017.192" } ] }
    ]

    // The last bullet of README.md "Acknowledgments": the libraries by role.
    readonly property var components: [
        { subject: qsTr("RAW decoding"), links: [ { name: "LibRaw", url: "https://www.libraw.org/" } ] },
        { subject: qsTr("Lens correction"), links: [ { name: "Lensfun", url: "https://lensfun.github.io/" } ] },
        { subject: qsTr("Metadata"), links: [ { name: "Exiv2", url: "https://exiv2.org/" } ] },
        { subject: qsTr("Image IO"), links: [
            { name: "OpenImageIO", url: "https://github.com/AcademySoftwareFoundation/OpenImageIO" },
            { name: "OpenCV", url: "https://opencv.org/" } ] },
        { subject: qsTr("Color management"), links: [
            { name: "OpenColorIO", url: "https://opencolorio.org/" } ] },
        { subject: qsTr("Catalog database"), links: [ { name: "DuckDB", url: "https://duckdb.org/" } ] },
        { subject: qsTr("UI"), links: [
            { name: "Qt", url: "https://www.qt.io/" },
            { name: "QuickQanava", url: "https://github.com/cneben/QuickQanava" } ] }
    ]

    width: parent ? parent.width : implicitWidth
    spacing: appTheme.spaceMd

    function openUrl(url) {
        Qt.openUrlExternally(url)
    }

    // ── Bento row 1: identity + version ───────────────────────────────────
    RowLayout {
        Layout.fillWidth: true
        Layout.topMargin: 26
        Layout.leftMargin: 34
        Layout.rightMargin: 34
        spacing: appTheme.spaceMd

        BentoTile {
            objectName: "aboutIdentityTile"
            Layout.fillWidth: true
            Layout.fillHeight: true
            contentHeight: identityColumn.implicitHeight

            ColumnLayout {
                id: identityColumn
                anchors.fill: parent
                spacing: 0

                // The wordmark of the main window and the welcome dialog: the brand mark keeps
                // its heading weight (the one text above 600 on the Settings pages).
                Row {
                    objectName: "aboutWordmark"
                    Accessible.role: Accessible.StaticText
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

                GridLayout {
                    Layout.fillWidth: true
                    Layout.topMargin: 14
                    columns: 2
                    columnSpacing: appTheme.spaceLg
                    rowSpacing: 6

                    MetaKey { text: qsTr("Copyright") }
                    MetaValue {
                        objectName: "aboutCopyrightLabel"
                        text: "© 2026 Yurun Zi"
                    }
                    MetaKey { text: qsTr("License") }
                    MetaValue {
                        objectName: "aboutLicenseLabel"
                        text: qsTr("GPL-3.0-only, with an additional permission under GPLv3 section 7 for combining and distributing the required NVIDIA CUDA components.")
                    }
                }

                Item {
                    Layout.fillHeight: true
                    Layout.minimumHeight: appTheme.spaceLg
                }

                RowLayout {
                    Layout.alignment: Qt.AlignRight
                    spacing: appTheme.spaceSm

                    DialogActionButton {
                        objectName: "aboutSponsorButton"
                        buttonWidth: Math.max(96, implicitContentWidth + 32)
                        buttonHeight: 34
                        iconSource: "qrc:/panel_icons/heart.svg"
                        text: qsTr("Sponsor")
                        onClicked: page.openUrl(page.sponsorUrl)
                    }
                    DialogActionButton {
                        objectName: "aboutGitHubButton"
                        buttonWidth: Math.max(96, implicitContentWidth + 32)
                        buttonHeight: 34
                        iconSource: "qrc:/panel_icons/github.svg"
                        text: qsTr("GitHub")
                        onClicked: page.openUrl(page.repoUrl)
                    }
                    DialogActionButton {
                        objectName: "aboutDocsButton"
                        kind: "light"
                        buttonWidth: Math.max(96, implicitContentWidth + 32)
                        buttonHeight: 34
                        iconSource: "qrc:/panel_icons/book.svg"
                        text: qsTr("Docs")
                        onClicked: page.openUrl(page.docsUrl)
                    }
                }
            }
        }

        CountTile {
            objectName: "aboutVersionTile"
            Layout.fillHeight: true
            label: qsTr("Version")
            valueText: page.appVersion.length > 0 ? page.appVersion : qsTr("Unavailable")
            detailText: page.updateService
                        ? qsTr("Build %1").arg(page.updateService.currentBuild)
                        : ""
            countLabel.objectName: "aboutVersionLabel"
            exactCountLabel.objectName: "aboutBuildLabel"
        }
    }

    // ── Bento row 2: log folder, statement, licenses ──────────────────────
    BentoTile {
        objectName: "aboutResourcesTile"
        Layout.fillWidth: true
        Layout.leftMargin: 34
        Layout.rightMargin: 34
        contentHeight: resourcesColumn.implicitHeight

        ColumnLayout {
            id: resourcesColumn
            anchors.fill: parent
            spacing: 0

            SettingRow {
                label: qsTr("Logs")

                DialogActionButton {
                    objectName: "aboutOpenLogFolderButton"
                    buttonWidth: Math.max(96, implicitContentWidth + 36)
                    buttonHeight: 34
                    text: qsTr("Open Log Folder")
                    enabled: page.logFolder !== null
                    onClicked: page.logFolder.openLogFolder()
                }
            }

            Divider {}

            // AI models: the download statement of the external AI models (AI Models plan,
            // section 4), read-only. Settings > AI Models still shows it for acceptance before
            // the first download.
            SettingRow {
                label: qsTr("AI model download statement")

                DialogActionButton {
                    objectName: "aboutAiModelStatementLink"
                    buttonWidth: Math.max(96, implicitContentWidth + 36)
                    buttonHeight: 34
                    text: qsTr("View")
                    onClicked: aiModelStatement.openReadOnly()
                }
            }

            Divider {}

            // The list is baked in from the third_party_licenses/ folder (distinct libraries, with
            // combined license notes where a library ships several license files).
            SettingRow {
                label: qsTr("Third-party licenses")

                DialogActionButton {
                    objectName: "aboutLicensesToggle"
                    buttonWidth: Math.max(96, implicitContentWidth + 36)
                    buttonHeight: 34
                    text: page.licensesExpanded ? qsTr("Collapse") : qsTr("View all")
                    onClicked: page.licensesExpanded = !page.licensesExpanded
                }
            }

            GridLayout {
                objectName: "aboutLicenseList"
                Layout.fillWidth: true
                visible: page.licensesExpanded
                columns: 2
                columnSpacing: 24
                rowSpacing: 0

                Repeater {
                    model: licenseModel

                    delegate: Item {
                        id: licenseEntry

                        required property string name
                        required property string license

                        Layout.fillWidth: true
                        Layout.preferredWidth: 100
                        implicitHeight: licenseRow.implicitHeight + 14

                        RowLayout {
                            id: licenseRow
                            anchors.left: parent.left
                            anchors.right: parent.right
                            anchors.verticalCenter: parent.verticalCenter
                            spacing: appTheme.spaceMd

                            Label {
                                Layout.fillWidth: true
                                text: licenseEntry.name
                                color: appTheme.textColor
                                font.family: appTheme.uiFontFamily
                                font.pixelSize: appTheme.fontSizeBody
                                font.weight: appTheme.fontWeightRegular
                                elide: Text.ElideRight
                            }
                            Label {
                                text: licenseEntry.license
                                color: appTheme.textMutedColor
                                font.family: appTheme.headlineFontFamily
                                font.pixelSize: appTheme.fontSizeBody
                                font.weight: appTheme.fontWeightRegular
                            }
                        }

                        Rectangle {
                            anchors.left: parent.left
                            anchors.right: parent.right
                            anchors.bottom: parent.bottom
                            height: 1
                            color: appTheme.dividerColor
                        }
                    }
                }
            }
        }
    }

    // ── Bento row 3: acknowledgements ─────────────────────────────────────
    BentoTile {
        objectName: "aboutAcknowledgementsTile"
        Layout.fillWidth: true
        Layout.leftMargin: 34
        Layout.rightMargin: 34
        Layout.bottomMargin: 26
        contentHeight: acknowledgementsColumn.implicitHeight

        ColumnLayout {
            id: acknowledgementsColumn
            anchors.fill: parent
            spacing: 0

            TileTitle { text: qsTr("Acknowledgements") }

            Repeater {
                model: page.acknowledgements

                delegate: AcknowledgementRow {}
            }

            TileTitle {
                Layout.topMargin: 18
                text: qsTr("Components")
            }

            Repeater {
                model: page.components

                delegate: AcknowledgementRow {}
            }

            Divider {
                Layout.topMargin: appTheme.spaceMd
            }

            Label {
                objectName: "aboutTrademarkNotice"
                Layout.fillWidth: true
                Layout.topMargin: appTheme.spaceMd
                text: qsTr("ACES is a trademark of the Academy of Motion Picture Arts and Sciences (A.M.P.A.S.). Alcedo Studio is an independent project and is not affiliated with, certified, or endorsed by A.M.P.A.S., Academy Software Foundation, Jed Smith, or any other upstream authors mentioned above. These names are used solely to identify the respective technologies. Film stock names such as Kodak and Fujifilm are trademarks of their owners and identify the films that Film Print simulates.")
                color: appTheme.textMutedColor
                font.family: appTheme.uiFontFamily
                font.pixelSize: appTheme.fontSizeBody
                font.weight: appTheme.fontWeightRegular
                wrapMode: Text.WordWrap
                lineHeight: 1.3
            }
        }
    }

    Item { Layout.fillHeight: true }

    AiModelDownloadStatementDialog {
        id: aiModelStatement
        objectName: "aboutAiModelStatementDialog"
    }

    ListModel {
        id: licenseModel

        ListElement { name: "ACES core (Academy)"; license: "Academy ACES terms" }
        ListElement { name: "ACES Looks (Pekka Riikonen)"; license: "BSD-style" }
        ListElement { name: "darktable"; license: "GPL-3.0" }
        ListElement { name: "DuckDB"; license: "MIT" }
        ListElement { name: "easy_profiler"; license: "MIT" }
        ListElement { name: "ed25519 (Orson Peters)"; license: "Zlib" }
        ListElement { name: "Eigen"; license: "MPL-2.0" }
        ListElement { name: "Exiv2"; license: "GPL-2.0+" }
        ListElement { name: "glad"; license: "MIT" }
        ListElement { name: "GoogleTest"; license: "BSD-3-Clause" }
        ListElement { name: "Google Highway"; license: "Apache-2.0 OR BSD-3-Clause" }
        ListElement { name: "Lensfun"; license: "GPL-3.0 / LGPL-3.0" }
        ListElement { name: "Lensfun data"; license: "CC-BY-SA-3.0" }
        ListElement { name: "LibRaw"; license: "CDDL-1.0 / LGPL-2.1" }
        ListElement { name: "Little-CMS (lcms2)"; license: "MIT" }
        ListElement { name: "MurmurHash3"; license: "Public Domain" }
        ListElement { name: "nlohmann/json"; license: "MIT" }
        ListElement { name: "OpenColorIO"; license: "BSD-3-Clause" }
        ListElement { name: "OpenCV"; license: "Apache-2.0" }
        ListElement { name: "OpenImageIO"; license: "Apache-2.0" }
        ListElement { name: "Qt"; license: "LGPL-2.1 / LGPL-3.0" }
        ListElement { name: "RawTherapee"; license: "GPL-3.0" }
        ListElement { name: "RCD-Demosaicing (LuisSR)"; license: "GPL-3.0" }
        ListElement { name: "stduuid"; license: "MIT" }
        ListElement { name: "utfcpp"; license: "Boost Software License 1.0" }
        ListElement { name: "uuid_v4 (Crashoz)"; license: "MIT" }
        ListElement { name: "xxHash (Yann Collet)"; license: "BSD-2-Clause" }
    }

    component TileTitle: Label {
        Layout.fillWidth: true
        Layout.bottomMargin: 6
        color: appTheme.textColor
        font.family: appTheme.headlineFontFamily
        font.pixelSize: appTheme.fontSizeSection
        font.weight: appTheme.fontWeightStrong
        elide: Text.ElideRight
    }

    component MetaKey: Label {
        color: appTheme.textMutedColor
        font.family: appTheme.uiFontFamily
        font.pixelSize: appTheme.fontSizeBody
        font.weight: appTheme.fontWeightRegular
        Layout.alignment: Qt.AlignTop
    }

    component MetaValue: Label {
        Layout.fillWidth: true
        color: appTheme.textColor
        font.family: appTheme.uiFontFamily
        font.pixelSize: appTheme.fontSizeBody
        font.weight: appTheme.fontWeightRegular
        wrapMode: Text.WordWrap
        lineHeight: 1.2
    }

    // A row of the resources tile: the name at the left, the control at the right.
    component SettingRow: RowLayout {
        id: settingRow

        property string label: ""

        Layout.fillWidth: true
        Layout.topMargin: appTheme.spaceSm
        Layout.bottomMargin: appTheme.spaceSm
        spacing: appTheme.spaceLg

        Label {
            Layout.fillWidth: true
            text: settingRow.label
            color: appTheme.textColor
            font.family: appTheme.uiFontFamily
            font.pixelSize: appTheme.fontSizeTitle
            font.weight: appTheme.fontWeightStrong
            elide: Text.ElideRight
        }
    }

    component Divider: Rectangle {
        Layout.fillWidth: true
        implicitHeight: 1
        color: appTheme.dividerColor
    }

    // One subject of the acknowledgements: the subject at the left (one line), its source links
    // at the right. A link opens its URL; the line above every row but the first divides them.
    component AcknowledgementRow: Item {
        id: ackRow

        required property int index
        required property var modelData

        Layout.fillWidth: true
        implicitHeight: ackLayout.implicitHeight + 16

        Rectangle {
            visible: ackRow.index > 0
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: parent.top
            height: 1
            color: appTheme.dividerColor
        }

        RowLayout {
            id: ackLayout
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.verticalCenter: parent.verticalCenter
            spacing: appTheme.spaceMd

            Label {
                Layout.preferredWidth: 236
                Layout.alignment: Qt.AlignTop
                text: ackRow.modelData.subject
                color: appTheme.textColor
                font.family: appTheme.uiFontFamily
                font.pixelSize: appTheme.fontSizeBody
                font.weight: appTheme.fontWeightRegular
                elide: Text.ElideRight
            }

            Flow {
                Layout.fillWidth: true
                spacing: appTheme.spaceMd

                Repeater {
                    model: ackRow.modelData.links

                    delegate: SourceLink {}
                }
            }
        }
    }

    component SourceLink: Item {
        id: link

        required property var modelData
        readonly property string detail: modelData.detail !== undefined ? modelData.detail : ""
        readonly property color inkColor: linkMouse.containsMouse ? appTheme.textColor
                                                                  : appTheme.textMutedColor

        implicitWidth: linkRow.implicitWidth
        implicitHeight: linkRow.implicitHeight
        Accessible.role: Accessible.Link
        Accessible.name: modelData.name

        Row {
            id: linkRow

            Label {
                text: link.modelData.name
                color: link.inkColor
                font.family: appTheme.headlineFontFamily
                font.pixelSize: appTheme.fontSizeBody
                font.weight: appTheme.fontWeightRegular
                font.underline: linkMouse.containsMouse
            }
            Label {
                visible: link.detail.length > 0
                text: " (" + link.detail + ")"
                color: link.inkColor
                opacity: 0.6
                font.family: appTheme.headlineFontFamily
                font.pixelSize: appTheme.fontSizeBody
                font.weight: appTheme.fontWeightRegular
            }
            Label {
                text: " ↗"
                color: link.inkColor
                font.family: appTheme.headlineFontFamily
                font.pixelSize: appTheme.fontSizeBody
                font.weight: appTheme.fontWeightRegular
            }
        }

        MouseArea {
            id: linkMouse
            anchors.fill: parent
            hoverEnabled: true
            cursorShape: Qt.PointingHandCursor
            onClicked: page.openUrl(link.modelData.url)
        }
    }
}
