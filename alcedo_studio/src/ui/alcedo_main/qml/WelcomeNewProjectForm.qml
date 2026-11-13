import QtQuick
import QtQuick.Controls
import QtQuick.Dialogs
import QtQuick.Layouts

// New-project form in the right column of the welcome surface. It keeps the
// fields and the validation of the previous WelcomeDialog page 1: a project
// name and a storage folder chosen through FolderDialog. Back restores the
// previous right column content; Create reports createRequested(folder, name).
ColumnLayout {
    id: root

    property string serviceMessage: ""
    // False while a project load runs (one load at a time).
    property bool actionsEnabled: true
    property string projectName: qsTr("Untitled Project")
    property string storageLocation: ""
    readonly property bool canCreate: root.actionsEnabled
                                      && root.projectName.trim().length > 0
                                      && root.storageLocation.length > 0
    readonly property alias nameField: projectNameField

    signal createRequested(string storageLocation, string projectName)
    signal backRequested()

    spacing: appTheme.spaceLg

    function reset() {
        root.projectName = qsTr("Untitled Project")
        projectNameField.text = root.projectName
    }

    function requestCreate() {
        if (root.canCreate)
            root.createRequested(root.storageLocation, root.projectName.trim())
    }

    FolderDialog {
        id: projectFolderDialog
        title: qsTr("Select Project Storage Location")
        onAccepted: root.storageLocation = selectedFolder.toString()
    }

    RowLayout {
        Layout.fillWidth: true
        spacing: appTheme.spaceMd

        WelcomeActionButton {
            objectName: "welcomeFormBackButton"
            kind: "quiet"
            compact: true
            text: qsTr("Back")
            onClicked: root.backRequested()
        }

        ColumnLayout {
            Layout.fillWidth: true
            spacing: appTheme.spaceXs / 2

            Label {
                Layout.fillWidth: true
                text: qsTr("New Project")
                color: appTheme.textColor
                font.family: appTheme.uiFontFamily
                font.pixelSize: appTheme.fontSizeHeadline
                font.weight: appTheme.fontWeightHeading
                wrapMode: Text.Wrap
                Accessible.role: Accessible.Heading
            }

            Label {
                Layout.fillWidth: true
                text: qsTr("Configure your workspace settings.")
                color: appTheme.textMutedColor
                font.family: appTheme.uiFontFamily
                font.pixelSize: appTheme.fontSizeBody
                font.weight: appTheme.fontWeightRegular
                wrapMode: Text.Wrap
            }
        }
    }

    Rectangle {
        Layout.fillWidth: true
        Layout.preferredHeight: 1
        color: appTheme.dividerColor
    }

    ColumnLayout {
        Layout.fillWidth: true
        spacing: appTheme.spaceSm

        Label {
            text: qsTr("Project Name")
            color: appTheme.textColor
            font.family: appTheme.uiFontFamily
            font.pixelSize: appTheme.fontSizeTitle
            font.weight: appTheme.fontWeightStrong
        }

        TextField {
            id: projectNameField
            objectName: "welcomeProjectNameField"
            Layout.fillWidth: true
            Layout.preferredHeight: appTheme.iconButtonHitSize
            text: root.projectName
            selectByMouse: true
            leftPadding: appTheme.spaceMd
            rightPadding: appTheme.spaceMd
            color: appTheme.textColor
            selectionColor: appTheme.editorListSelectedFillColor
            selectedTextColor: appTheme.editorListSelectedInkColor
            placeholderTextColor: appTheme.textMutedColor
            font.family: appTheme.uiFontFamily
            font.pixelSize: appTheme.fontSizeSection
            Accessible.name: qsTr("Project Name")
            onTextChanged: root.projectName = text
            onAccepted: root.requestCreate()
            background: Rectangle {
                radius: appTheme.controlRadiusSmall
                color: appTheme.bgBaseColor
                border.width: 1
                border.color: projectNameField.activeFocus ? appTheme.textMutedColor : appTheme.cardBorderColor
            }
        }
    }

    ColumnLayout {
        Layout.fillWidth: true
        spacing: appTheme.spaceSm

        Label {
            text: qsTr("Storage Location")
            color: appTheme.textColor
            font.family: appTheme.uiFontFamily
            font.pixelSize: appTheme.fontSizeTitle
            font.weight: appTheme.fontWeightStrong
        }

        FolderPathField {
            Layout.fillWidth: true
            path: root.storageLocation
            placeholderText: qsTr("Select a parent folder...")
            controlHeight: appTheme.iconButtonHitSize
            pathPixelSize: appTheme.fontSizeSection
            iconSize: appTheme.iconOpticalSizeCompact
            textColor: appTheme.textColor
            mutedTextColor: appTheme.textMutedColor
            pathFontFamily: appTheme.uiFontFamily
            browseToolTip: qsTr("Select Project Storage Location")
            onBrowseRequested: projectFolderDialog.open()
        }
    }

    Label {
        Layout.fillWidth: true
        visible: root.serviceMessage.length > 0
        text: root.serviceMessage
        color: appTheme.textMutedColor
        font.family: appTheme.uiFontFamily
        font.pixelSize: appTheme.fontSizeBody
        font.weight: appTheme.fontWeightRegular
        wrapMode: Text.Wrap
    }

    Item {
        Layout.fillHeight: true
    }

    WelcomeActionButton {
        objectName: "welcomeCreateProjectButton"
        Layout.fillWidth: true
        kind: "primary"
        text: qsTr("Create Project")
        enabled: root.canCreate
        onClicked: root.requestCreate()
    }
}
