import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Item {
    id: root

    property string selectedRating: ""
    signal starClicked(string rating)

    readonly property int starCount: 5
    property int hoveredRating: 0
    /// Optional actions beside the title (see StatsCard.headerActions).
    property Component headerActions: null
    readonly property bool stackHeaderActions: headerActions !== null
        && width < titleMetrics.advanceWidth + appTheme.inspectorHeaderActionSize * 6
                   + appTheme.spaceLg * 2

    implicitHeight: ratingColumn.implicitHeight + appTheme.spaceSm

    // Same star inks as the Library card rating: filled stars in textColor, the hover
    // preview in textMutedColor, and the other stars in ratingStarEmptyColor.
    function starColor(starIndex) {
        const r = starIndex + 1;
        if (selectedRating !== "" && r <= Number(selectedRating)) {
            return appTheme.textColor;
        }
        if (hoveredRating > 0 && r <= hoveredRating) {
            return appTheme.textMutedColor;
        }
        return appTheme.ratingStarEmptyColor;
    }

    function starChar(starIndex) {
        const r = starIndex + 1;
        if (selectedRating !== "" && r <= Number(selectedRating)) return "★";
        if (hoveredRating > 0 && r <= hoveredRating) return "★";
        return "☆";
    }

    MouseArea {
        anchors.fill: parent
        acceptedButtons: Qt.RightButton
        cursorShape: Qt.ArrowCursor
        onClicked: function(mouse) {
            if (mouse.button === Qt.RightButton) {
                root.starClicked("");
            }
        }
    }

    ColumnLayout {
        id: ratingColumn
        anchors.left: parent.left
        anchors.right: parent.right
        spacing: 6

        TextMetrics {
            id: titleMetrics
            text: qsTr("BY RATING")
            font.pixelSize: 10
            font.weight: 700
            font.letterSpacing: 1.6
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: appTheme.spaceXs

            Label {
                Layout.fillWidth: true
                text: qsTr("BY RATING")
                color: appTheme.textMutedColor
                font.pixelSize: 10
                font.weight: 700
                font.letterSpacing: 1.6
                wrapMode: Text.Wrap
            }

            Loader {
                active: root.headerActions !== null && !root.stackHeaderActions
                visible: active
                sourceComponent: root.headerActions
            }
        }

        Loader {
            Layout.fillWidth: true
            active: root.headerActions !== null && root.stackHeaderActions
            visible: active
            sourceComponent: root.headerActions
        }

        RowLayout {
            id: starRow
            spacing: 2

            Repeater {
                model: root.starCount

                delegate: Label {
                    required property int index
                    text: root.starChar(index)
                    color: root.starColor(index)
                    font.pixelSize: 16

                    MouseArea {
                        anchors.fill: parent
                        cursorShape: Qt.PointingHandCursor
                        hoverEnabled: true
                        acceptedButtons: Qt.LeftButton
                        onClicked: function(mouse) {
                            if (mouse.button === Qt.LeftButton) {
                                root.starClicked(String(index + 1));
                            }
                        }
                        onEntered: root.hoveredRating = index + 1
                        onExited: root.hoveredRating = 0
                    }
                }
            }

            Item { Layout.fillWidth: true }
        }
    }
}
