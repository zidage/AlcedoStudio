pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts

// About page for the settings dialog. Hosted as the last StackLayout child
// inside SettingDialog.qml. Identity, links, and licenses only — software
// updates live in UpdatesSettingsPanel.qml.
ColumnLayout {
    id: page

    property string appVersion: appTheme.appVersion
    property color primaryAccent: appTheme.accentColor
    property color secondaryAccent: appTheme.accentSecondaryColor
    property color textColor: appTheme.textColor
    property color mutedTextColor: appTheme.textMutedColor
    property color canvasColor: appTheme.bgBaseColor
    property color panelColor: appTheme.cardSurfaceColor
    property color dividerColor: appTheme.cardBorderColor
    property color dangerColor: appTheme.dangerColor
    property string headlineFontFamily: appTheme.headlineFontFamily
    property var updateService: null
    readonly property string uiFontFamily: appTheme.uiFontFamily
    readonly property string dataFontFamily: appTheme.dataFontFamily

    // Collapsed by default so the long license list is not front and center.
    property bool licensesExpanded: false

    readonly property string docsUrl: "https://zidage.github.io/AlcedoStudio_docs/"
    readonly property string repoUrl: "https://github.com/zidage/AlcedoStudio"

    width: parent ? parent.width : implicitWidth
    spacing: appTheme.spaceXl

    function openUrl(url) {
        Qt.openUrlExternally(url)
    }

    // 1. Documentation hero — the prominent, "guide the user" element. The docs
    // site is the tutorial for this software, so it gets an accent-tinted card
    // at the very top with a direct call to action.
    Rectangle {
        Layout.fillWidth: true
        Layout.topMargin: 26
        Layout.leftMargin: 34
        Layout.rightMargin: 34
        Layout.preferredHeight: heroInner.implicitHeight + 32
        radius: appTheme.panelRadius
        color: appTheme.cardSurfaceColor
        border.width: 1
        border.color: appTheme.accentSecondaryColor

        RowLayout {
            id: heroInner
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.verticalCenter: parent.verticalCenter
            anchors.margins: 22
            spacing: 18

            Image {
                Layout.preferredWidth: appTheme.iconButtonHitSizeCompact
                Layout.preferredHeight: appTheme.iconButtonHitSizeCompact
                Layout.alignment: Qt.AlignTop
                source: "qrc:/panel_icons/aperture.svg"
                sourceSize.width: appTheme.iconSourceSize
                sourceSize.height: appTheme.iconSourceSize
                asynchronous: true
                opacity: 0.95
            }

            ColumnLayout {
                Layout.fillWidth: true
                spacing: 6

                Label {
                    Layout.fillWidth: true
                    text: qsTr("New to Alcedo Studio?")
                    color: page.textColor
                    font.family: page.headlineFontFamily
                    font.pixelSize: appTheme.fontSizeHeadline
                    font.weight: appTheme.fontWeightHeading
                    wrapMode: Text.WordWrap
                }

                Label {
                    Layout.fillWidth: true
                    text: qsTr("The documentation website is the best place to learn how this software works — how to import, edit, and manage your photos, step by step. It is the tutorial for Alcedo Studio, and it is kept up to date with each release.")
                    color: page.mutedTextColor
                    font.family: page.uiFontFamily
                    font.pixelSize: appTheme.fontSizeBody
                    font.weight: appTheme.fontWeightRegular
                    wrapMode: Text.WordWrap
                    lineHeight: 1.3
                }

                Button {
                    id: docsButton
                    Layout.topMargin: 4
                    Layout.preferredHeight: appTheme.iconButtonHitSizeCompact
                    text: qsTr("Open documentation")
                    font.family: page.uiFontFamily
                    font.pixelSize: appTheme.fontSizeSection
                    font.weight: appTheme.fontWeightHeading
                    onClicked: page.openUrl(page.docsUrl)

                    contentItem: Label {
                        text: docsButton.text
                        color: appTheme.editorListSelectedInkColor
                        font: docsButton.font
                        horizontalAlignment: Text.AlignHCenter
                        verticalAlignment: Text.AlignVCenter
                    }

                    background: Rectangle {
                        radius: appTheme.controlRadius
                        color: docsButton.down
                               ? appTheme.buttonPressedFillColor
                               : (docsButton.hovered
                                  ? appTheme.buttonHoveredFillColor
                                  : appTheme.editorListSelectedFillColor)
                        border.width: 1
                        border.color: appTheme.cardBorderColor
                    }
                }
            }
        }
    }

    // 2. About — version, copyright, license.
    SettingsSection {
        Layout.fillWidth: true
        Layout.leftMargin: 34
        Layout.rightMargin: 34
        title: qsTr("About")
        textColor: page.textColor
        mutedTextColor: page.mutedTextColor
        dividerColor: page.dividerColor

        ColumnLayout {
            Layout.fillWidth: true
            spacing: 10

            Label {
                Layout.fillWidth: true
                text: qsTr("Alcedo Studio")
                color: page.textColor
                font.family: page.headlineFontFamily
                font.pixelSize: 22
                font.weight: 800
            }

            Label {
                Layout.fillWidth: true
                text: qsTr("An open-source RAW photo editor and digital asset management (DAM) project, built for a lightweight, high-performance, industry-compatible photography workflow.")
                color: page.mutedTextColor
                font.pixelSize: 13
                font.weight: 500
                wrapMode: Text.WordWrap
                lineHeight: 1.3
            }

            RowLayout {
                Layout.fillWidth: true
                spacing: 16

                Label {
                    Layout.preferredWidth: 120
                    text: qsTr("Version")
                    color: page.mutedTextColor
                    font.pixelSize: 14
                    font.weight: 600
                }

                Label {
                    objectName: "aboutVersionLabel"
                    Layout.fillWidth: true
                    text: page.updateService
                          ? page.updateService.currentVersion
                          : (page.appVersion.length > 0 ? page.appVersion : qsTr("Unavailable"))
                    color: page.textColor
                    font.family: page.dataFontFamily
                    font.pixelSize: 15
                    font.weight: 700
                }
            }

            Label {
                Layout.fillWidth: true
                text: qsTr("Copyright © 2026 Yurun Zi")
                color: page.textColor
                font.pixelSize: 14
                font.weight: 600
            }

            Label {
                Layout.fillWidth: true
                text: qsTr("Licensed under GPL-3.0-only, with an additional permission under GPLv3 section 7 for combining and distributing the required NVIDIA CUDA components.")
                color: page.mutedTextColor
                font.pixelSize: 12
                font.weight: 500
                wrapMode: Text.WordWrap
                lineHeight: 1.3
            }
        }
    }

    // 3. Links — repo + documentation.
    SettingsSection {
        Layout.fillWidth: true
        Layout.leftMargin: 34
        Layout.rightMargin: 34
        title: qsTr("Links")
        textColor: page.textColor
        mutedTextColor: page.mutedTextColor
        dividerColor: page.dividerColor

        ColumnLayout {
            Layout.fillWidth: true
            spacing: 10

            LinkRow {
                Layout.fillWidth: true
                labelText: qsTr("Documentation (tutorial)")
                subtitle: page.docsUrl
                accent: page.primaryAccent
                textColor: page.textColor
                mutedTextColor: page.mutedTextColor
                dividerColor: page.dividerColor
                dataFontFamily: page.dataFontFamily
                onActivated: page.openUrl(page.docsUrl)
            }

            LinkRow {
                Layout.fillWidth: true
                labelText: qsTr("GitHub repository")
                subtitle: page.repoUrl
                accent: page.primaryAccent
                textColor: page.textColor
                mutedTextColor: page.mutedTextColor
                dividerColor: page.dividerColor
                dataFontFamily: page.dataFontFamily
                onActivated: page.openUrl(page.repoUrl)
            }
        }
    }

    // 4. Acknowledgements — verbatim from README.md.
    SettingsSection {
        Layout.fillWidth: true
        Layout.leftMargin: 34
        Layout.rightMargin: 34
        title: qsTr("Acknowledgements")
        textColor: page.textColor
        mutedTextColor: page.mutedTextColor
        dividerColor: page.dividerColor

        ColumnLayout {
            Layout.fillWidth: true
            spacing: 10

            Label {
                Layout.fillWidth: true
                text: qsTr("Alcedo Studio builds on research, open-source implementations, and community data from the wider imaging ecosystem:")
                color: page.mutedTextColor
                font.pixelSize: 13
                font.weight: 500
                wrapMode: Text.WordWrap
                lineHeight: 1.3
            }

            LinkRow {
                Layout.fillWidth: true
                labelText: qsTr("Film-emulation LUTs — JanLohse/spectral_film_lut")
                subtitle: "https://github.com/JanLohse/spectral_film_lut"
                accent: page.primaryAccent
                textColor: page.textColor
                mutedTextColor: page.mutedTextColor
                dividerColor: page.dividerColor
                dataFontFamily: page.dataFontFamily
                onActivated: page.openUrl("https://github.com/JanLohse/spectral_film_lut")
            }

            LinkRow {
                Layout.fillWidth: true
                labelText: qsTr("Camera color matrices — AcademySoftwareFoundation/rawtoaces-data")
                subtitle: "https://github.com/AcademySoftwareFoundation/rawtoaces-data"
                accent: page.primaryAccent
                textColor: page.textColor
                mutedTextColor: page.mutedTextColor
                dividerColor: page.dividerColor
                dataFontFamily: page.dataFontFamily
                onActivated: page.openUrl("https://github.com/AcademySoftwareFoundation/rawtoaces-data")
            }

            LinkRow {
                Layout.fillWidth: true
                labelText: qsTr("Highlight reconstruction — RawTherapee hilite_recon.cc")
                subtitle: "https://github.com/RawTherapee/RawTherapee/blob/dev/rtengine/hilite_recon.cc"
                accent: page.primaryAccent
                textColor: page.textColor
                mutedTextColor: page.mutedTextColor
                dividerColor: page.dividerColor
                dataFontFamily: page.dataFontFamily
                onActivated: page.openUrl("https://github.com/RawTherapee/RawTherapee/blob/dev/rtengine/hilite_recon.cc")
            }

            LinkRow {
                Layout.fillWidth: true
                labelText: qsTr("RCD demosaic — LuisSR/RCD-Demosaicing")
                subtitle: "https://github.com/LuisSR/RCD-Demosaicing"
                accent: page.primaryAccent
                textColor: page.textColor
                mutedTextColor: page.mutedTextColor
                dividerColor: page.dividerColor
                dataFontFamily: page.dataFontFamily
                onActivated: page.openUrl("https://github.com/LuisSR/RCD-Demosaicing")
            }

            LinkRow {
                Layout.fillWidth: true
                labelText: qsTr("OpenDRT — jedypod/open-display-transform")
                subtitle: "https://github.com/jedypod/open-display-transform/blob/main/display-transforms/opendrt/OpenDRT.dctl"
                accent: page.primaryAccent
                textColor: page.textColor
                mutedTextColor: page.mutedTextColor
                dividerColor: page.dividerColor
                dataFontFamily: page.dataFontFamily
                onActivated: page.openUrl("https://github.com/jedypod/open-display-transform/blob/main/display-transforms/opendrt/OpenDRT.dctl")
            }

            LinkRow {
                Layout.fillWidth: true
                labelText: qsTr("ACES 2.0 — aces-aswf/aces-core")
                subtitle: "https://github.com/aces-aswf/aces-core"
                accent: page.primaryAccent
                textColor: page.textColor
                mutedTextColor: page.mutedTextColor
                dividerColor: page.dividerColor
                dataFontFamily: page.dataFontFamily
                onActivated: page.openUrl("https://github.com/aces-aswf/aces-core")
            }

            LinkRow {
                Layout.fillWidth: true
                labelText: qsTr("Film grain renderer — Newson, Faraj, Galerne, Delon (IPOL 2017)")
                subtitle: "https://doi.org/10.5201/ipol.2017.192"
                accent: page.primaryAccent
                textColor: page.textColor
                mutedTextColor: page.mutedTextColor
                dividerColor: page.dividerColor
                dataFontFamily: page.dataFontFamily
                onActivated: page.openUrl("https://doi.org/10.5201/ipol.2017.192")
            }
        }
    }

    // 5. Third-party licenses — the long one. Collapsible "View all" so it is
    // not front and center; collapsed by default. The list is baked in from the
    // third_party_licenses/ folder (distinct libraries, with combined license
    // notes where a library ships several license files).
    SettingsSection {
        Layout.fillWidth: true
        Layout.leftMargin: 34
        Layout.rightMargin: 34
        Layout.bottomMargin: 26
        title: qsTr("Third-party licenses")
        textColor: page.textColor
        mutedTextColor: page.mutedTextColor
        dividerColor: page.dividerColor

        ColumnLayout {
            Layout.fillWidth: true
            spacing: 10

            Label {
                Layout.fillWidth: true
                text: qsTr("This software bundles open-source third-party libraries. Their licenses are reproduced in the third_party_licenses/ folder of the source tree; the summary below lists each library and its license.")
                color: page.mutedTextColor
                font.pixelSize: 13
                font.weight: 500
                wrapMode: Text.WordWrap
                lineHeight: 1.3
            }

            Rectangle {
                Layout.fillWidth: true
                Layout.preferredHeight: 44
                radius: 10
                color: licensesToggleMouse.containsMouse
                       ? appTheme.buttonHoveredFillColor
                       : appTheme.buttonIdleFillColor
                border.width: 1
                border.color: appTheme.cardBorderColor

                RowLayout {
                    anchors.fill: parent
                    anchors.leftMargin: 14
                    anchors.rightMargin: 14
                    spacing: 8

                    Label {
                        Layout.fillWidth: true
                        text: page.licensesExpanded
                              ? qsTr("Collapse")
                              : qsTr("View all %1 licenses").arg(licenseModel.count)
                        color: page.primaryAccent
                        font.pixelSize: 14
                        font.weight: 700
                    }

                    Label {
                        text: page.licensesExpanded ? "▲" : "▼"
                        color: page.primaryAccent
                        font.pixelSize: 14
                        font.weight: 700
                    }
                }

                MouseArea {
                    id: licensesToggleMouse
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: page.licensesExpanded = !page.licensesExpanded
                }
            }

            ColumnLayout {
                Layout.fillWidth: true
                visible: page.licensesExpanded
                spacing: 8

                Repeater {
                    model: licenseModel

                    delegate: Rectangle {
                        required property string name
                        required property string license

                        Layout.fillWidth: true
                        Layout.preferredHeight: 40
                        radius: 8
                        color: appTheme.bgBaseColor
                        border.width: 1
                        border.color: appTheme.cardBorderColor

                        RowLayout {
                            anchors.fill: parent
                            anchors.leftMargin: 14
                            anchors.rightMargin: 14
                            spacing: 12

                            Label {
                                Layout.fillWidth: true
                                text: name
                                color: page.textColor
                                font.pixelSize: 13
                                font.weight: 600
                                elide: Text.ElideRight
                            }

                            Label {
                                text: license
                                color: page.mutedTextColor
                                font.family: page.dataFontFamily
                                font.pixelSize: 12
                                font.weight: 600
                            }
                        }
                    }
                }
            }
        }
    }

    Item { Layout.fillHeight: true }

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

    component SettingsSection: ColumnLayout {
        id: section

        property string title: ""
        property color textColor: appTheme.textColor
        property color mutedTextColor: appTheme.textMutedColor
        property color dividerColor: appTheme.dividerColor

        spacing: 14

        Label {
            Layout.fillWidth: true
            text: section.title
            color: section.textColor
            font.family: appTheme.uiFontFamily
            font.pixelSize: appTheme.fontSizeSection
            font.weight: appTheme.fontWeightHeading
        }

        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: 1
            color: section.dividerColor
        }
    }

    component LinkRow: Rectangle {
        id: row

        property string labelText: ""
        property string subtitle: ""
        property color accent: appTheme.accentColor
        property color textColor: appTheme.textColor
        property color mutedTextColor: appTheme.textMutedColor
        property color dividerColor: appTheme.dividerColor
        property string dataFontFamily: appTheme.dataFontFamily

        signal activated()

        Layout.fillWidth: true
        Layout.preferredHeight: rowInner.implicitHeight + 18
        radius: 10
        color: linkMouse.containsMouse
               ? appTheme.buttonHoveredFillColor
               : appTheme.buttonIdleFillColor
        border.width: 1
        border.color: row.dividerColor

        RowLayout {
            id: rowInner
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.verticalCenter: parent.verticalCenter
            anchors.leftMargin: 14
            anchors.rightMargin: 14
            spacing: 12

            ColumnLayout {
                Layout.fillWidth: true
                spacing: 2

                Label {
                    Layout.fillWidth: true
                    text: row.labelText
                    color: linkMouse.containsMouse ? row.accent : row.textColor
                    font.pixelSize: 14
                    font.weight: 600
                    elide: Text.ElideRight
                    wrapMode: Text.WordWrap
                    lineHeight: 1.25
                }

                Label {
                    Layout.fillWidth: true
                    visible: row.subtitle.length > 0
                    text: row.subtitle
                    color: row.mutedTextColor
                    font.family: row.dataFontFamily
                    font.pixelSize: 12
                    font.weight: 500
                    elide: Text.ElideMiddle
                }
            }

            Label {
                Layout.preferredWidth: 18
                text: "↗"
                color: row.mutedTextColor
                font.pixelSize: 14
                font.weight: 700
                horizontalAlignment: Text.AlignRight
                opacity: linkMouse.containsMouse ? 1.0 : 0.5
            }
        }

        MouseArea {
            id: linkMouse
            anchors.fill: parent
            hoverEnabled: true
            cursorShape: Qt.PointingHandCursor
            onClicked: row.activated()
        }
    }
}
