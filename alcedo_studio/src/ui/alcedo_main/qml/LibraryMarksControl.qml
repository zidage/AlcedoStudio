import QtQuick
import QtQuick.Controls
import QtQuick.Controls.impl
import QtQuick.Layouts

// Marks control of the Library inspector (#772, DESIGN.md "Library workspace chrome"): one
// bgBaseColor card above the button row of LibraryInspectorActions. Three rows, with hairlines
// between them, set the marks of the selected photos:
//
// - Rating: five stars. The stars show the rating of all selected photos; when the ratings
//   differ, the stars are hollow and "Mixed" follows them. A click on a star sets that rating on
//   every photo; a click on the star of the common rating clears the rating.
// - Label: the seven color label codes. Filled: all photos have the code. Ring with a dash:
//   some photos have it. Ring: no photo has it. A click on a code that all photos have removes
//   it from all; otherwise it adds it to all. The other codes of each photo stay. The tooltip is
//   the user name of the code.
// - Delete: "Mark" sets the Delete mark on every photo; when all photos have it, the button is
//   the light filled "Marked" and a click clears it.
//
// The summary comes from appModules.library.selectionMarks (the stored marks of every selected
// photo). The control is enabled under the conditions of "Add to Export Queue" while the user
// may change the project state (interactionPolicy.canChangeStateAsUser).
Rectangle {
    id: root
    objectName: "libraryMarksControl"

    // Album/image selection (SelectionState.qml).
    property var selectionState: null
    property int selectedCount: 0
    property bool backendInteractive: false
    // Colour helpers of LibraryWorkspace; the appTheme values apply when it is not set.
    property var theme: null

    // A write was refused or did not start; @p message says why.
    signal messageRequested(string message)
    // The rating of one photo was saved at once (a batch reports through ImageController).
    signal ratingApplied()

    readonly property var marks: appModules.library.selectionMarks
    readonly property int markedCount: marks ? marks.count : 0
    readonly property bool controlsEnabled: backendInteractive && selectedCount > 0
                                            && appModules.interactionPolicy.canChangeStateAsUser
    readonly property int commonRating: marks && markedCount > 0 ? marks.rating : -1
    readonly property bool ratingMixed: marks ? marks.ratingMixed : false
    readonly property var colorLabelCounts: marks ? marks.colorLabelCounts : []
    readonly property bool allDeleteMarked: markedCount > 0 && marks.deleteMarkCount === markedCount
    readonly property var colorLabelNames: appModules.libraryView.colorLabelNames
    readonly property var colorLabelColors: appTheme.colorLabelColors

    readonly property int rowHeight: 40
    readonly property int labelColumnWidth: 64
    readonly property int starPixelSize: 18
    readonly property int codeSize: 18

    readonly property color textColor: theme ? theme.colText : appTheme.textColor
    readonly property color secondaryBorderColor: theme ? theme.colButtonSecondaryBorder
                                                        : appTheme.dividerColor
    readonly property int controlRadius: theme ? theme.controlRadius : appTheme.controlRadius

    function secondaryButtonFill(enabled, hovered, pressed) {
        return theme ? theme.secondaryButtonFill(enabled, hovered, pressed)
                     : appTheme.controlFillColor
    }

    function filledActionFill(enabled, hovered, pressed) {
        return theme ? theme.filledActionFill(enabled, hovered, pressed)
                     : appTheme.editorListSelectedFillColor
    }

    function filledActionInk(enabled) {
        return theme ? theme.filledActionInk(enabled) : appTheme.editorListSelectedInkColor
    }

    // "all", "some", or "none": how many of the selected photos have color label @p code.
    function codeState(code) {
        const count = code < colorLabelCounts.length ? Number(colorLabelCounts[code]) : 0
        if (markedCount > 0 && count === markedCount)
            return "all"
        return count > 0 ? "some" : "none"
    }

    function codeName(code) {
        return colorLabelNames && code < colorLabelNames.length ? String(colorLabelNames[code])
                                                                : ""
    }

    function targets() {
        return selectionState ? selectionState.currentSelectedItems() : []
    }

    // Rating of a click on star @p star: that rating, or 0 for the star of the common rating.
    function ratingForStar(star) {
        return !ratingMixed && star === commonRating ? 0 : star
    }

    function setRating(rating) {
        if (!controlsEnabled)
            return
        const result = appModules.libraryMutations.RateTargetsAsUser(targets(), rating)
        const batch = result && result.batch === true
        const ok = result && (batch ? result.started === true : result.success === true)
        if (!ok) {
            if (result && result.message)
                root.messageRequested(result.message)
            return
        }
        if (!batch)
            root.ratingApplied()
    }

    function toggleColorLabel(code) {
        if (!controlsEnabled)
            return
        const on = codeState(code) !== "all"
        const result = appModules.libraryMutations.SetColorLabelOfTargetsAsUser(targets(), code, on)
        if (!result || result.started !== true) {
            if (result && result.message)
                root.messageRequested(result.message)
        }
    }

    function toggleDeleteMark() {
        if (!controlsEnabled)
            return
        const result = appModules.libraryMutations.SetDeleteMarkOfTargetsAsUser(targets(),
                                                                                !allDeleteMarked)
        if (!result || result.started !== true) {
            if (result && result.message)
                root.messageRequested(result.message)
        }
    }

    implicitHeight: rows.implicitHeight
    radius: appTheme.panelRadius
    color: appTheme.bgBaseColor
    border.width: 1
    border.color: appTheme.cardBorderColor

    // Inline components do not see the ids of this file; the width repeats labelColumnWidth.
    component RowTitle: Label {
        Layout.preferredWidth: 64
        Layout.alignment: Qt.AlignVCenter
        color: appTheme.textMutedColor
        elide: Text.ElideRight
        font.family: appTheme.uiFontFamily
        font.pixelSize: appTheme.fontSizeCaption
        font.weight: 500
    }

    component Hairline: Rectangle {
        Layout.fillWidth: true
        Layout.preferredHeight: 1
        color: appTheme.dividerColor
    }

    ColumnLayout {
        id: rows
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.leftMargin: appTheme.spaceMd
        anchors.rightMargin: appTheme.spaceMd
        spacing: 0

        // ── Rating ──────────────────────────────────────────────────────────
        RowLayout {
            objectName: "libraryMarksRatingRow"
            Layout.fillWidth: true
            Layout.preferredHeight: root.rowHeight
            spacing: appTheme.spaceSm

            RowTitle { text: qsTr("Rating") }

            Row {
                id: starRow
                objectName: "libraryMarksStars"
                Layout.alignment: Qt.AlignVCenter
                spacing: 2
                property int hoveredStar: 0

                Repeater {
                    model: 5

                    Text {
                        id: star
                        required property int index
                        readonly property int value: index + 1
                        readonly property bool previewed: starRow.hoveredStar > 0
                                                          && value <= starRow.hoveredStar
                        readonly property bool filled: !root.ratingMixed
                                                       && value <= root.commonRating
                        objectName: "libraryMarksStar_" + value
                        text: filled || previewed ? "★" : "☆"
                        color: previewed ? appTheme.textMutedColor
                                         : (filled ? appTheme.textColor
                                                   : appTheme.ratingStarEmptyColor)
                        opacity: root.controlsEnabled ? 1 : 0.45
                        font.family: appTheme.dataFontFamily
                        font.pixelSize: root.starPixelSize
                        verticalAlignment: Text.AlignVCenter

                        Accessible.role: Accessible.Button
                        Accessible.name: qsTr("%n star(s)", "", value)
                        Accessible.onPressAction: root.setRating(root.ratingForStar(value))

                        MouseArea {
                            anchors.fill: parent
                            anchors.leftMargin: -1
                            anchors.rightMargin: -1
                            enabled: root.controlsEnabled
                            hoverEnabled: true
                            cursorShape: Qt.PointingHandCursor
                            onEntered: starRow.hoveredStar = star.value
                            onExited: if (starRow.hoveredStar === star.value) starRow.hoveredStar = 0
                            onClicked: root.setRating(root.ratingForStar(star.value))
                            onEnabledChanged: if (!enabled) starRow.hoveredStar = 0
                        }
                    }
                }
            }

            Label {
                objectName: "libraryMarksRatingMixed"
                Layout.alignment: Qt.AlignVCenter
                visible: root.ratingMixed
                text: qsTr("Mixed")
                color: appTheme.textMutedColor
                font.family: appTheme.uiFontFamily
                font.pixelSize: appTheme.fontSizeCaption
                font.weight: 500
            }

            Item { Layout.fillWidth: true }
        }

        Hairline {}

        // ── Color labels ────────────────────────────────────────────────────
        RowLayout {
            objectName: "libraryMarksLabelRow"
            Layout.fillWidth: true
            Layout.preferredHeight: root.rowHeight
            spacing: appTheme.spaceSm

            RowTitle { text: qsTr("Label") }

            Row {
                Layout.alignment: Qt.AlignVCenter
                spacing: 8

                Repeater {
                    model: root.colorLabelColors ? root.colorLabelColors.length : 0

                    Item {
                        id: codeItem
                        required property int index
                        readonly property string markState: root.codeState(index)
                        readonly property color codeColor: root.colorLabelColors[index]

                        objectName: "libraryMarksColorLabel_" + index
                        width: root.codeSize
                        height: root.codeSize
                        opacity: root.controlsEnabled ? 1 : 0.45

                        Accessible.role: Accessible.Button
                        Accessible.name: root.codeName(index)
                        Accessible.checkable: true
                        Accessible.checked: markState === "all"
                        Accessible.onPressAction: root.toggleColorLabel(index)

                        // Filled when all photos have the code; a ring otherwise.
                        Rectangle {
                            anchors.fill: parent
                            radius: width / 2
                            color: codeItem.markState === "all" ? codeItem.codeColor
                                                                : "transparent"
                            border.width: codeItem.markState === "all" ? 0 : 1.5
                            border.color: codeItem.codeColor
                        }

                        // The dash of "some photos have it".
                        Rectangle {
                            objectName: "libraryMarksColorLabelDash_" + codeItem.index
                            anchors.centerIn: parent
                            visible: codeItem.markState === "some"
                            width: 8
                            height: 2
                            radius: 1
                            color: codeItem.codeColor
                        }

                        // Hover ring around the code.
                        Rectangle {
                            anchors.centerIn: parent
                            width: parent.width + 6
                            height: width
                            radius: width / 2
                            color: "transparent"
                            border.width: 1
                            border.color: appTheme.textMutedColor
                            visible: codeHit.containsMouse
                        }

                        MouseArea {
                            id: codeHit
                            anchors.fill: parent
                            anchors.margins: -3
                            enabled: root.controlsEnabled
                            hoverEnabled: true
                            cursorShape: Qt.PointingHandCursor
                            onClicked: root.toggleColorLabel(codeItem.index)
                        }

                        ToolTip.visible: codeHit.containsMouse
                        ToolTip.delay: 500
                        ToolTip.text: root.codeName(index)
                    }
                }
            }

            Item { Layout.fillWidth: true }
        }

        Hairline {}

        // ── Delete mark ─────────────────────────────────────────────────────
        RowLayout {
            objectName: "libraryMarksDeleteRow"
            Layout.fillWidth: true
            Layout.preferredHeight: root.rowHeight
            spacing: appTheme.spaceSm

            RowTitle { text: qsTr("Delete") }

            Button {
                id: deleteMarkButton
                objectName: "libraryMarksDeleteButton"
                readonly property bool marked: root.allDeleteMarked
                Layout.alignment: Qt.AlignVCenter
                Layout.preferredHeight: 28
                leftPadding: 14
                rightPadding: 14
                topPadding: 0
                bottomPadding: 0
                enabled: root.controlsEnabled
                text: marked ? qsTr("Marked") : qsTr("Mark")
                font.family: appTheme.uiFontFamily
                font.pixelSize: appTheme.fontSizeCaption
                font.weight: 600
                Accessible.checkable: true
                Accessible.checked: marked
                background: Rectangle {
                    radius: root.controlRadius
                    color: deleteMarkButton.marked
                           ? root.filledActionFill(deleteMarkButton.enabled,
                                                   deleteMarkButton.hovered,
                                                   deleteMarkButton.down)
                           : root.secondaryButtonFill(deleteMarkButton.enabled,
                                                      deleteMarkButton.hovered,
                                                      deleteMarkButton.down)
                    border.width: deleteMarkButton.marked ? 0 : 1
                    border.color: root.secondaryBorderColor
                    Behavior on color {
                        enabled: !appTheme.reduceMotion
                        ColorAnimation { duration: 120; easing.type: Easing.OutCubic }
                    }
                }
                contentItem: Label {
                    text: deleteMarkButton.text
                    font: deleteMarkButton.font
                    horizontalAlignment: Text.AlignHCenter
                    verticalAlignment: Text.AlignVCenter
                    color: deleteMarkButton.marked
                           ? root.filledActionInk(deleteMarkButton.enabled)
                           : (deleteMarkButton.enabled ? root.textColor
                                                       : appTheme.textMutedColor)
                }
                onClicked: root.toggleDeleteMark()
            }

            Item { Layout.fillWidth: true }
        }
    }
}
