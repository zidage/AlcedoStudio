import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Album inspector page: library overview hero, capture/import/camera/label/rating/lens
// filters, and the active-search filter card. Each field header carries the photo-sort
// arrows and the Group checkbox (InspectorFieldActions) of the library query.
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

        // Drag-flicking is disabled above, which also stops the Flickable from
        // handling wheel events; scroll the panel explicitly. Takes over from
        // children so wheel over stats cards still scrolls.
        WheelHandler {
            acceptedDevices: PointerDevice.Mouse | PointerDevice.TouchPad
            grabPermissions: PointerHandler.CanTakeOverFromItems
                             | PointerHandler.CanTakeOverFromHandlersOfDifferentType
                             | PointerHandler.ApprovesTakeOverByAnything
            onWheel: function (event) {
                var flick = root.contentItem
                var step = event.pixelDelta.y !== 0
                           ? event.pixelDelta.y
                           : event.angleDelta.y / 120 * 48
                var maxY = Math.max(0, flick.contentHeight - flick.height)
                flick.contentY = Math.max(0, Math.min(maxY, flick.contentY - step))
                event.accepted = true
            }
        }

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

            DateFilterSection {
                Layout.fillWidth: true
                title: qsTr("By Capture Date")
                accentColor: appTheme.toneSteel
                model: appModules.stats.dateStats
                selectedLabel: appModules.stats.statsFilterDate
                folderKey: appModules.folders.currentFolderId
                onDayClicked: function(label) { appModules.stats.ToggleStatsFilter("date", label) }
                headerActions: Component {
                    InspectorFieldActions {
                        field: "date"
                        fieldTitle: qsTr("capture time")
                        timeField: true
                        ascendingTitle: qsTr("oldest first")
                        descendingTitle: qsTr("newest first")
                    }
                }
            }

            DateFilterSection {
                objectName: "importDateFilterSection"
                Layout.fillWidth: true
                title: qsTr("By Import Time")
                accentColor: appTheme.toneSteel
                model: appModules.stats.importDateStats
                selectedLabel: appModules.stats.statsFilterImportDate
                folderKey: appModules.folders.currentFolderId
                activityAvailable: false
                onDayClicked: function(label) { appModules.stats.ToggleStatsFilter("import", label) }
                headerActions: Component {
                    InspectorFieldActions {
                        field: "import"
                        fieldTitle: qsTr("import time")
                        timeField: true
                        ascendingTitle: qsTr("oldest first")
                        descendingTitle: qsTr("newest first")
                    }
                }
            }

            StatsCard {
                Layout.fillWidth: true
                title: qsTr("By Camera Model")
                accentColor: appTheme.toneGold
                model: appModules.stats.cameraStats
                selectedLabel: appModules.stats.statsFilterCamera
                displayMode: "chips"
                onBarClicked: function(label) { appModules.stats.ToggleStatsFilter("camera", label) }
                headerActions: Component {
                    InspectorFieldActions {
                        field: "camera"
                        fieldTitle: qsTr("camera model")
                    }
                }
            }

            StatsCard {
                Layout.fillWidth: true
                title: qsTr("By Labels")
                accentColor: appTheme.toneSteel
                model: appModules.stats.labelStats
                selectedLabel: appModules.stats.statsFilterLabel
                displayMode: "chips"
                onBarClicked: function(label) { appModules.stats.ToggleStatsFilter("label", label) }
                headerActions: Component {
                    InspectorFieldActions {
                        field: "label"
                        fieldTitle: qsTr("labels")
                    }
                }
            }

            StarRatingFilter {
                Layout.fillWidth: true
                selectedRating: appModules.stats.statsFilterRating
                accentColor: appTheme.toneGold
                onStarClicked: function(rating) {
                    appModules.stats.ToggleStatsFilter("rating", rating);
                }
                headerActions: Component {
                    InspectorFieldActions {
                        field: "rating"
                        fieldTitle: qsTr("rating")
                        ascendingTitle: qsTr("lowest first")
                        descendingTitle: qsTr("highest first")
                    }
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
                headerActions: Component {
                    InspectorFieldActions {
                        field: "lens"
                        fieldTitle: qsTr("lens")
                    }
                }
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
                        Layout.preferredHeight: Math.max(58, searchFilterColumn.implicitHeight
                                                         + appTheme.spaceMd)
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
                                id: searchFilterColumn
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
                                    wrapMode: Text.Wrap
                                }

                                Label {
                                    Layout.fillWidth: true
                                    text: appModules.search.activeSearchQuery
                                    color: root.mutedTextColor
                                    font.family: appTheme.dataFontFamily
                                    font.pixelSize: 11
                                    font.weight: 500
                                    wrapMode: Text.Wrap
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
