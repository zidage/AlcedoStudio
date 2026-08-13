pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts

// Settings > Updates. Owns the auto-update page so About stays identity-only
// and later update options can land here without growing other settings pages.
ColumnLayout {
    id: page
    objectName: "updatesSettingsPanel"

    property var updateService: null
    property color textColor: appTheme.textColor
    property color mutedTextColor: appTheme.textMutedColor
    property color dividerColor: appTheme.cardBorderColor
    property string dataFontFamily: appTheme.dataFontFamily

    width: parent ? parent.width : implicitWidth
    spacing: appTheme.spaceXl

    SettingsSection {
        Layout.fillWidth: true
        Layout.topMargin: 26
        Layout.leftMargin: 34
        Layout.rightMargin: 34
        Layout.bottomMargin: 26
        title: qsTr("Software updates")
        textColor: page.textColor
        mutedTextColor: page.mutedTextColor
        dividerColor: page.dividerColor

        UpdateNotice {
            Layout.fillWidth: true
            updates: page.updateService
            showWhenUnchecked: true
        }
    }

    Item {
        Layout.fillHeight: true
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
}
