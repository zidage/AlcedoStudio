import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Album inspector page: library overview hero, capture/camera/label/lens/rating
// stats, and the active-search filter card. This is the content that used to
// live directly in InspectorPanel.qml before Frontend 3 split the inspector
// into a shell + page stack. The content is intentionally unchanged.
ScrollView {
    id: root
    contentWidth: availableWidth
    readonly property color textColor: appTheme.textColor
    readonly property color mutedTextColor: appTheme.textMutedColor

    function withAlpha(color, alpha) {
        return Qt.rgba(color.r, color.g, color.b, alpha)
    }

    Component.onCompleted: {
        contentItem.interactive = false
    }

    ColumnLayout {
        width: root.availableWidth
        spacing: 0

        // Library Overview hero
        Item {
            Layout.fillWidth: true
            Layout.topMargin: 24
            Layout.leftMargin: 16
            Layout.rightMargin: 16
            Layout.bottomMargin: 4
            implicitHeight: heroCol.implicitHeight

            ColumnLayout {
                id: heroCol
                anchors.left: parent.left
                anchors.right: parent.right
                spacing: 14

                Label {
                    text: qsTr("LIBRARY OVERVIEW")
                    color: root.mutedTextColor
                    font.pixelSize: 10
                    font.weight: 700
                    font.letterSpacing: 1.8
                }

                RowLayout {
                    Layout.fillWidth: true

                    Label {
                        text: qsTr("Total Photos")
                        color: root.mutedTextColor
                        font.family: appTheme.uiFontFamily
                        font.pixelSize: 13
                        font.weight: 400
                        Layout.alignment: Qt.AlignVCenter
                    }

                    Item { Layout.fillWidth: true }

                    Label {
                        text: appModules.stats.totalPhotoCount
                        color: root.textColor
                        font.family: appTheme.headlineFontFamily
                        font.pixelSize: 34
                        font.weight: 300
                        Layout.alignment: Qt.AlignVCenter
                    }
                }

                Label {
                    visible: appModules.library.filterInfo !== ""
                    text: appModules.library.filterInfo
                    color: root.mutedTextColor
                    font.family: appTheme.uiFontFamily
                    font.pixelSize: 11
                    font.weight: 400
                    Layout.topMargin: -6
                }

            }
        }

        // Stats sections
        ColumnLayout {
            Layout.fillWidth: true
            Layout.topMargin: 28
            Layout.leftMargin: 16
            Layout.rightMargin: 16
            Layout.bottomMargin: 20
            spacing: 24

            StatsCard {
                Layout.fillWidth: true
                title: qsTr("By Capture Date")
                accentColor: appTheme.toneSteel
                model: appModules.stats.dateStats
                selectedLabel: appModules.stats.statsFilterDate
                displayMode: "grouped"
                onBarClicked: function(label) { appModules.stats.ToggleStatsFilter("date", label) }
            }

            StatsCard {
                Layout.fillWidth: true
                title: qsTr("By Camera Model")
                accentColor: appTheme.toneGold
                model: appModules.stats.cameraStats
                selectedLabel: appModules.stats.statsFilterCamera
                displayMode: "chips"
                onBarClicked: function(label) { appModules.stats.ToggleStatsFilter("camera", label) }
            }

            StatsCard {
                Layout.fillWidth: true
                title: qsTr("By Labels")
                accentColor: appTheme.toneSteel
                model: appModules.stats.labelStats
                selectedLabel: appModules.stats.statsFilterLabel
                displayMode: "chips"
                onBarClicked: function(label) { appModules.stats.ToggleStatsFilter("label", label) }
            }

            StarRatingFilter {
                Layout.fillWidth: true
                selectedRating: appModules.stats.statsFilterRating
                accentColor: appTheme.toneGold
                onStarClicked: function(rating) {
                    appModules.stats.ToggleStatsFilter("rating", rating);
                }
            }

            StatsCard {
                Layout.fillWidth: true
                title: qsTr("By Lens")
                accentColor: appTheme.toneGold
                model: appModules.stats.lensStats
                selectedLabel: appModules.stats.statsFilterLens
                displayMode: "dots"
                onBarClicked: function(label) { appModules.stats.ToggleStatsFilter("lens", label) }
            }

            Item {
                Layout.fillWidth: true
                visible: appModules.search.activeSearchQuery.length > 0
                implicitHeight: searchFilterCard.implicitHeight

                ColumnLayout {
                    id: searchFilterCard
                    anchors.left: parent.left
                    anchors.right: parent.right
                    spacing: 8

                    Label {
                        text: qsTr("SEARCH FILTER")
                        color: root.mutedTextColor
                        font.pixelSize: 10
                        font.weight: 700
                        font.letterSpacing: 1.6
                    }

                    Rectangle {
                        Layout.fillWidth: true
                        Layout.preferredHeight: 58
                        radius: 6
                        color: root.withAlpha(appTheme.bgBaseColor, 0.62)
                        border.width: 1
                        border.color: root.withAlpha(appTheme.glassStrokeColor, 0.36)

                        RowLayout {
                            anchors.fill: parent
                            anchors.leftMargin: 12
                            anchors.rightMargin: 8
                            spacing: 10

                            Rectangle {
                                Layout.preferredWidth: 9
                                Layout.preferredHeight: 9
                                radius: 4.5
                                color: appTheme.toneGold
                                Layout.alignment: Qt.AlignVCenter
                            }

                            ColumnLayout {
                                Layout.fillWidth: true
                                spacing: 2
                                Layout.alignment: Qt.AlignVCenter

                                Label {
                                    Layout.fillWidth: true
                                    text: qsTr("Filtered by search")
                                    color: root.withAlpha(root.textColor, 0.86)
                                    font.family: appTheme.uiFontFamily
                                    font.pixelSize: 12
                                    font.weight: 700
                                    elide: Text.ElideRight
                                }

                                Label {
                                    Layout.fillWidth: true
                                    text: appModules.search.activeSearchQuery
                                    color: root.mutedTextColor
                                    font.family: appTheme.dataFontFamily
                                    font.pixelSize: 11
                                    font.weight: 500
                                    elide: Text.ElideRight
                                }
                            }

                            ToolButton {
                                id: clearSearchButton
                                Layout.preferredWidth: 28
                                Layout.preferredHeight: 28
                                text: "×"
                                font.pixelSize: 18
                                font.weight: 400
                                onClicked: appModules.search.ClearFuzzySearch()
                                background: Rectangle {
                                    radius: 6
                                    color: clearSearchButton.down
                                           ? root.withAlpha(appTheme.textColor, 0.10)
                                           : (clearSearchButton.hovered
                                              ? root.withAlpha(appTheme.hoverColor, 0.86)
                                              : "transparent")
                                }
                                contentItem: Text {
                                    text: clearSearchButton.text
                                    color: root.withAlpha(root.textColor, 0.76)
                                    font: clearSearchButton.font
                                    horizontalAlignment: Text.AlignHCenter
                                    verticalAlignment: Text.AlignVCenter
                                }
                            }
                        }
                    }
                }
            }
        }
    }
}
