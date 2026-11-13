import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Upper area of the welcome surface ("Continue your last project"): the cover
// block and the information block of the previewed project. It binds only to
// WelcomeProjectPreviewAdapter and reports the continue action through
// continueRequested(). States (welcome overview plan, section 3.3):
//   "loading" - skeleton tiles and skeleton value bars; name and path from the
//               recent entry; Continue is disabled until the load completes.
//   "ready"   - cover thumbnails and values.
//   "failed"  - plain tiles, the real error text in dangerColor, Continue disabled.
ColumnLayout {
    id: root

    property var adapter: null
    // Narrow card: the cover block uses welcomeCoverCompactWidth.
    property bool compact: false
    readonly property alias continueButton: continueButtonObj

    readonly property string previewState: root.adapter ? root.adapter.state : "loading"
    readonly property bool valuesVisible: root.previewState === "ready" || root.previewState === "loading"
    readonly property bool valuesLoading: root.previewState === "loading"

    signal continueRequested()

    // Integer with the locale group separators (section 3.2).
    function formatCount(count) {
        return Number(count).toLocaleString(Qt.locale(), "f", 0)
    }

    spacing: appTheme.spaceMd

    Label {
        Layout.fillWidth: true
        text: qsTr("Continue your last project")
        color: appTheme.textColor
        font.family: appTheme.uiFontFamily
        font.pixelSize: appTheme.fontSizeSection
        font.weight: appTheme.fontWeightHeading
        wrapMode: Text.Wrap
        Accessible.role: Accessible.Heading
    }

    // The row is exactly as tall as the cover block. The spacer in the
    // information block has Layout.fillHeight, which a layout passes up to its
    // parents; without the fixed heights the overview takes the height that
    // the recent list needs.
    RowLayout {
        Layout.fillWidth: true
        Layout.fillHeight: false
        Layout.preferredHeight: appTheme.welcomeCoverHeight
        Layout.maximumHeight: appTheme.welcomeCoverHeight
        spacing: appTheme.spaceXl

        WelcomeCoverMosaic {
            objectName: "welcomeCoverMosaic"
            Layout.preferredWidth: root.compact ? appTheme.welcomeCoverCompactWidth : appTheme.welcomeCoverWidth
            Layout.preferredHeight: appTheme.welcomeCoverHeight
            Layout.alignment: Qt.AlignTop
            previewState: root.previewState
            coverItems: root.adapter ? root.adapter.coverItems : []
        }

        ColumnLayout {
            Layout.fillWidth: true
            Layout.minimumWidth: appTheme.welcomeInfoMinWidth
            // Continue Editing ends on the bottom edge of the cover block.
            Layout.fillHeight: false
            Layout.preferredHeight: appTheme.welcomeCoverHeight
            Layout.maximumHeight: appTheme.welcomeCoverHeight
            Layout.alignment: Qt.AlignTop
            spacing: 0

            Label {
                objectName: "welcomeProjectName"
                Layout.fillWidth: true
                text: root.adapter ? root.adapter.projectName : ""
                color: appTheme.textColor
                font.family: appTheme.uiFontFamily
                font.pixelSize: appTheme.fontSizeHeadline
                font.weight: appTheme.fontWeightHeading
                elide: Text.ElideRight
                maximumLineCount: 1
            }

            Label {
                objectName: "welcomeProjectPath"
                Layout.fillWidth: true
                Layout.topMargin: appTheme.spaceXs / 2
                text: root.adapter ? root.adapter.projectPath : ""
                color: appTheme.textMutedColor
                font.family: appTheme.uiFontFamily
                font.pixelSize: appTheme.fontSizeCaption
                font.weight: appTheme.fontWeightRegular
                elide: Text.ElideMiddle
                maximumLineCount: 1
            }

            Label {
                objectName: "welcomePreviewError"
                Layout.fillWidth: true
                Layout.topMargin: appTheme.spaceLg
                visible: root.previewState === "failed"
                text: root.adapter ? root.adapter.errorText : ""
                color: appTheme.dangerColor
                font.family: appTheme.uiFontFamily
                font.pixelSize: appTheme.fontSizeBody
                font.weight: appTheme.fontWeightRegular
                lineHeight: appTheme.lineHeightBody
                lineHeightMode: Text.FixedHeight
                wrapMode: Text.Wrap
                maximumLineCount: 6
                elide: Text.ElideRight
            }

            GridLayout {
                Layout.fillWidth: true
                Layout.topMargin: appTheme.spaceLg
                visible: root.valuesVisible
                columns: 2
                rowSpacing: appTheme.spaceMd
                columnSpacing: appTheme.spaceLg

                OverviewValue {
                    objectName: "welcomePhotoCount"
                    Layout.fillWidth: true
                    Layout.preferredWidth: 1
                    label: qsTr("Photos")
                    value: root.formatCount(root.adapter ? root.adapter.photoCount : 0)
                    loading: root.valuesLoading
                }

                OverviewValue {
                    objectName: "welcomeEditedPhotoCount"
                    Layout.fillWidth: true
                    Layout.preferredWidth: 1
                    label: qsTr("Edited")
                    value: root.formatCount(root.adapter ? root.adapter.editedPhotoCount : 0)
                    loading: root.valuesLoading
                }

                OverviewValue {
                    objectName: "welcomeCaptureRange"
                    Layout.fillWidth: true
                    Layout.columnSpan: 2
                    label: qsTr("Capture dates")
                    value: root.adapter ? root.adapter.captureRangeText : ""
                    valueMuted: !!root.adapter && !root.adapter.hasCaptureRange
                    loading: root.valuesLoading
                }
            }

            Item {
                Layout.fillHeight: true
            }

            WelcomeActionButton {
                id: continueButtonObj
                objectName: "welcomeContinueButton"
                Layout.fillWidth: true
                Layout.topMargin: appTheme.spaceLg
                kind: "primary"
                text: qsTr("Continue Editing")
                enabled: !!root.adapter && root.adapter.continueEnabled
                onClicked: root.continueRequested()
            }
        }
    }

    // One statistic: caption label above a Manrope value. While loading, a
    // skeleton bar takes the place of the value.
    component OverviewValue: ColumnLayout {
        id: valueItem

        property string label: ""
        property string value: ""
        property bool valueMuted: false
        property bool loading: false

        spacing: appTheme.spaceXs / 2
        Accessible.role: Accessible.StaticText
        Accessible.name: valueItem.loading ? valueItem.label : valueItem.label + " " + valueItem.value

        Label {
            Layout.fillWidth: true
            text: valueItem.label
            color: appTheme.textMutedColor
            font.family: appTheme.uiFontFamily
            font.pixelSize: appTheme.fontSizeCaption
            font.weight: appTheme.fontWeightRegular
            elide: Text.ElideRight
            Accessible.ignored: true
        }

        Item {
            Layout.fillWidth: true
            Layout.preferredHeight: appTheme.lineHeightTitle

            SkeletonBlock {
                visible: valueItem.loading
                anchors.verticalCenter: parent.verticalCenter
                width: Math.min(parent.width, appTheme.welcomeInfoMinWidth / 2)
                height: appTheme.fontSizeTitle
                radius: appTheme.badgeRadius / 2
            }

            Label {
                anchors.fill: parent
                visible: !valueItem.loading
                text: valueItem.value
                color: valueItem.valueMuted ? appTheme.textMutedColor : appTheme.textColor
                font.family: appTheme.headlineFontFamily
                font.pixelSize: appTheme.fontSizeTitle
                font.weight: appTheme.fontWeightStrong
                verticalAlignment: Text.AlignVCenter
                elide: Text.ElideRight
                Accessible.ignored: true
            }
        }
    }
}
