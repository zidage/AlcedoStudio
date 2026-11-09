import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Album Inspector section card: one rounded bgBaseColor card with a header (fold glyph, title,
// header actions) and, when expanded, the filter content below it inside the same card. A
// collapsed section is the header alone. The title elides at the right so the header actions
// stay on the title row. Only when the header cannot hold the glyph, a short title, and the
// actions together do the actions move to a second header row, as one group at the right.
// Fold motion (DESIGN.md "Motion", fold rules): `expanded` flips at once; foldProgress
// (0 collapsed, 1 expanded) drives the clipped content height, its opacity, and the glyph
// turn. Opening uses motionFoldOpenMs, closing motionFoldCloseMs, both motionEasing;
// reduceMotion makes it a cut. driveFoldProgress() lets tests pin a point of the fold.
Rectangle {
    id: section
    objectName: "inspectorSection"

    property string title: ""
    property bool expanded: true
    /// False: the header has no fold glyph and a click on it does nothing.
    property bool foldable: true
    /// False: the card is the header only (for example By Edit Time).
    property bool hasBody: true
    /// Actions at the right of the title (InspectorFieldActions). A click on them never
    /// toggles the section.
    property Component headerActions: null
    default property alias content: body.data

    readonly property int headerHeight: 36
    readonly property int headerLeftPadding: appTheme.spaceMd
    readonly property int headerRightPadding: 6
    readonly property int glyphWidth: 10
    readonly property int minimumTitleWidth: 56
    readonly property int headerActionsWidth: appTheme.inspectorHeaderActionSize * 3
                                              + appTheme.spaceXs * 2
    readonly property bool stackHeaderActions: headerActions !== null
        && width < headerLeftPadding + glyphWidth + appTheme.spaceSm + minimumTitleWidth
                   + appTheme.spaceSm + headerActionsWidth + headerRightPadding
    readonly property bool bodyShown: hasBody && (expanded || !foldable)
    readonly property alias titleItem: titleLabel

    // ── Fold driver ─────────────────────────────────────────────────────────
    property real foldProgress: bodyShown ? 1 : 0
    property bool foldManualDrive: false
    property bool _motionArmed: false
    property int _foldDuration: appTheme.motionFoldOpenMs
    /// True while the content shows or folds; loaders of the content stay active with it.
    readonly property bool contentLive: bodyShown || foldProgress > 0
    readonly property real bodyFullHeight: hasBody
        ? body.implicitHeight + appTheme.spaceXs + appTheme.spaceMd : 0

    function driveFoldProgress(value) {
        foldManualDrive = true
        foldProgress = Math.max(0, Math.min(1, value))
    }

    function endFoldDrive() {
        foldManualDrive = false
        foldProgress = bodyShown ? 1 : 0
    }

    onBodyShownChanged: {
        _foldDuration = bodyShown ? appTheme.motionFoldOpenMs : appTheme.motionFoldCloseMs
        if (!foldManualDrive) {
            foldProgress = bodyShown ? 1 : 0
        }
    }

    Component.onCompleted: {
        foldProgress = bodyShown ? 1 : 0
        _motionArmed = true
    }

    Behavior on foldProgress {
        enabled: section._motionArmed && !section.foldManualDrive
        NumberAnimation {
            duration: appTheme.reduceMotion ? 0 : section._foldDuration
            easing.type: appTheme.motionEasing
        }
    }

    implicitHeight: cardColumn.implicitHeight
    radius: appTheme.panelRadius
    color: appTheme.bgBaseColor
    border.width: 1
    border.color: appTheme.cardBorderColor

    ColumnLayout {
        id: cardColumn
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        spacing: 0

        Item {
            id: header
            objectName: "inspectorSectionHeader"
            Layout.fillWidth: true
            implicitHeight: section.headerHeight
                            + (section.stackHeaderActions ? appTheme.inspectorHeaderActionSize
                                                            + appTheme.spaceXs : 0)

            MouseArea {
                anchors.fill: parent
                enabled: section.foldable && section.hasBody
                cursorShape: enabled ? Qt.PointingHandCursor : Qt.ArrowCursor
                onClicked: section.expanded = !section.expanded
            }

            RowLayout {
                id: titleRow
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.top: parent.top
                anchors.leftMargin: section.headerLeftPadding
                anchors.rightMargin: section.headerRightPadding
                height: section.headerHeight
                spacing: appTheme.spaceSm

                // One "▸" that turns toward "▾" with the fold progress.
                Label {
                    objectName: "inspectorSectionGlyph"
                    Layout.preferredWidth: section.glyphWidth
                    Layout.alignment: Qt.AlignVCenter
                    horizontalAlignment: Text.AlignHCenter
                    text: section.foldable && section.hasBody ? "▸" : ""
                    rotation: 90 * section.foldProgress
                    color: appTheme.textMutedColor
                    font.pixelSize: 12
                }

                Label {
                    id: titleLabel
                    objectName: "inspectorSectionTitle"
                    Layout.fillWidth: true
                    Layout.alignment: Qt.AlignVCenter
                    text: section.title
                    elide: Text.ElideRight
                    maximumLineCount: 1
                    color: appTheme.textColor
                    font.family: appTheme.uiFontFamily
                    font.pixelSize: appTheme.fontSizeBody
                    font.weight: appTheme.fontWeightStrong
                }

                Loader {
                    objectName: "inspectorSectionActions"
                    active: section.headerActions !== null && !section.stackHeaderActions
                    visible: active
                    sourceComponent: section.headerActions
                    Layout.alignment: Qt.AlignVCenter
                }
            }

            Loader {
                objectName: "inspectorSectionStackedActions"
                anchors.right: parent.right
                anchors.rightMargin: section.headerRightPadding
                anchors.top: titleRow.bottom
                active: section.headerActions !== null && section.stackHeaderActions
                visible: active
                sourceComponent: section.headerActions
            }
        }

        // The content keeps its full size; the clip reveals it with the fold progress.
        Item {
            id: bodyClip
            objectName: "inspectorSectionBodyClip"
            Layout.fillWidth: true
            Layout.preferredHeight: section.bodyFullHeight * section.foldProgress
            visible: section.hasBody && section.foldProgress > 0
            clip: true

            ColumnLayout {
                id: body
                objectName: "inspectorSectionBody"
                x: appTheme.spaceMd
                y: appTheme.spaceXs
                width: bodyClip.width - appTheme.spaceMd * 2
                opacity: section.foldProgress
                spacing: appTheme.spaceSm
            }
        }
    }
}
