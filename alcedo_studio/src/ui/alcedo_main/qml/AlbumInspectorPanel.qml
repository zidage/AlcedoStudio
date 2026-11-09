import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Album inspector page: library overview hero, then one InspectorSection per field (capture,
// import, edit time, camera, label, rating, lens) and the active-search filter card. Each
// section header band carries the photo-sort arrows and the Group action
// (InspectorFieldActions) of the library query.
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
            objectName: "libraryOverviewHero"
            Layout.fillWidth: true
            Layout.topMargin: 24
            Layout.leftMargin: appTheme.spaceMd * 2
            Layout.rightMargin: appTheme.spaceMd * 2
            Layout.bottomMargin: appTheme.spaceXl
            implicitHeight: heroCol.implicitHeight

            ColumnLayout {
                id: heroCol
                anchors.left: parent.left
                anchors.right: parent.right
                spacing: 14

                Label {
                    text: qsTr("Library Overview")
                    color: root.mutedTextColor
                    font.family: appTheme.uiFontFamily
                    font.pixelSize: appTheme.fontSizeBody
                    font.weight: appTheme.fontWeightStrong
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
            }
        }

        // Sections: one rounded card each (InspectorSection).
        ColumnLayout {
            Layout.fillWidth: true
            Layout.leftMargin: appTheme.spaceMd
            Layout.rightMargin: appTheme.spaceMd
            Layout.bottomMargin: appTheme.spaceLg
            spacing: appTheme.spaceSm

            DateFilterSection {
                objectName: "captureDateFilterSection"
                Layout.fillWidth: true
                title: qsTr("By Capture Date")
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

            // Edit time has no filter buckets, only the sort and Group actions. Edited photos
            // group by the day of their last edit; unedited photos form one group.
            InspectorSection {
                objectName: "editTimeInspectorSection"
                Layout.fillWidth: true
                title: qsTr("By Edit Time")
                foldable: false
                hasBody: false
                headerActions: Component {
                    InspectorFieldActions {
                        field: "edited"
                        fieldTitle: qsTr("edit time")
                        timeField: true
                        ascendingTitle: qsTr("oldest first")
                        descendingTitle: qsTr("newest first")
                    }
                }
            }

            InspectorSection {
                objectName: "cameraInspectorSection"
                Layout.fillWidth: true
                title: qsTr("By Camera Model")
                headerActions: Component {
                    InspectorFieldActions {
                        field: "camera"
                        fieldTitle: qsTr("camera model")
                    }
                }

                StatsCard {
                    objectName: "cameraStatsCard"
                    Layout.fillWidth: true
                    model: appModules.stats.cameraStats
                    selectedLabel: appModules.stats.statsFilterCamera
                    displayMode: "chips"
                    searchable: true
                    onBarClicked: function(label) { appModules.stats.ToggleStatsFilter("camera", label) }
                }
            }

            InspectorSection {
                objectName: "labelInspectorSection"
                Layout.fillWidth: true
                title: qsTr("By Labels")
                headerActions: Component {
                    InspectorFieldActions {
                        field: "label"
                        fieldTitle: qsTr("labels")
                    }
                }

                StatsCard {
                    objectName: "labelStatsCard"
                    Layout.fillWidth: true
                    model: appModules.stats.labelStats
                    selectedLabel: appModules.stats.statsFilterLabel
                    displayMode: "chips"
                    searchable: true
                    onBarClicked: function(label) { appModules.stats.ToggleStatsFilter("label", label) }
                }
            }

            InspectorSection {
                objectName: "ratingInspectorSection"
                Layout.fillWidth: true
                title: qsTr("By Rating")
                headerActions: Component {
                    InspectorFieldActions {
                        field: "rating"
                        fieldTitle: qsTr("rating")
                        ascendingTitle: qsTr("lowest first")
                        descendingTitle: qsTr("highest first")
                    }
                }

                StarRatingFilter {
                    Layout.fillWidth: true
                    selectedRating: appModules.stats.statsFilterRating
                    onStarClicked: function(rating) {
                        appModules.stats.ToggleStatsFilter("rating", rating);
                    }
                }
            }

            InspectorSection {
                objectName: "lensInspectorSection"
                Layout.fillWidth: true
                title: qsTr("By Lens")
                headerActions: Component {
                    InspectorFieldActions {
                        field: "lens"
                        fieldTitle: qsTr("lens")
                    }
                }

                StatsCard {
                    objectName: "lensStatsCard"
                    Layout.fillWidth: true
                    model: appModules.stats.lensStats
                    selectedLabel: appModules.stats.statsFilterLens
                    displayMode: "dots"
                    searchable: true
                    onBarClicked: function(label) { appModules.stats.ToggleStatsFilter("lens", label) }
                }
            }

            InspectorSection {
                objectName: "searchFilterInspectorSection"
                Layout.fillWidth: true
                visible: appModules.search.activeSearchQuery.length > 0
                title: qsTr("Search Filter")
                foldable: false

                RowLayout {
                    Layout.fillWidth: true
                    spacing: 10

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

                        Label {
                            Layout.fillWidth: true
                            visible: appModules.search.activeSearchIsSemantic
                            text: qsTr("Natural language")
                            color: root.mutedTextColor
                            font.family: appTheme.uiFontFamily
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
