import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Rail of the folded Collections sidebar in the Library (#1028): Search, Background Tasks,
// All Photos, Collections, and Import. A hover on All Photos or Collections shows the
// sidebar over the workspace after collectionsPeekDelayMs; a click on Collections shows it
// at once.
Rectangle {
    id: rail
    objectName: "collectionsRail"

    property var theme
    property bool allPhotosSelected: false
    // True while the pointer is on a rail button that shows the sidebar.
    readonly property bool peekHovered: allPhotosHover.hovered || collectionsHover.hovered

    signal searchRequested()
    signal backgroundTasksRequested()
    signal allPhotosRequested()
    signal peekRequested()
    signal importRequested(var anchorItem)

    radius: appTheme.panelRadius
    color: theme ? theme.colBgPanel : appTheme.bgPanelColor

    onPeekHoveredChanged: {
        if (peekHovered) {
            peekTimer.restart()
        } else {
            peekTimer.stop()
        }
    }

    Timer {
        id: peekTimer
        interval: appTheme.collectionsPeekDelayMs
        onTriggered: {
            if (rail.peekHovered) {
                rail.peekRequested()
            }
        }
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.topMargin: appTheme.spaceSm
        anchors.bottomMargin: appTheme.spaceSm
        spacing: appTheme.spaceXs

        IconActionButton {
            objectName: "collectionsRailSearch"
            Layout.alignment: Qt.AlignHCenter
            compact: true
            actionName: qsTr("Search")
            iconSrc: "qrc:/panel_icons/search.svg"
            onClicked: rail.searchRequested()
        }

        IconActionButton {
            objectName: "collectionsRailBackgroundTasks"
            Layout.alignment: Qt.AlignHCenter
            compact: true
            actionName: qsTr("Background Tasks")
            iconSrc: "qrc:/panel_icons/clock-play.svg"
            onClicked: rail.backgroundTasksRequested()
        }

        Rectangle {
            Layout.alignment: Qt.AlignHCenter
            Layout.preferredWidth: appTheme.spaceXl
            Layout.preferredHeight: 1
            Layout.topMargin: appTheme.spaceXs
            Layout.bottomMargin: appTheme.spaceXs
            color: appTheme.dividerColor
        }

        IconActionButton {
            objectName: "collectionsRailAllPhotos"
            Layout.alignment: Qt.AlignHCenter
            compact: true
            selected: rail.allPhotosSelected
            actionName: qsTr("All Photos")
            iconSrc: "qrc:/panel_icons/image.svg"
            onClicked: rail.allPhotosRequested()

            HoverHandler {
                id: allPhotosHover
            }
        }

        IconActionButton {
            objectName: "collectionsRailCollections"
            Layout.alignment: Qt.AlignHCenter
            compact: true
            selected: !rail.allPhotosSelected
            actionName: qsTr("Collections")
            iconSrc: "qrc:/panel_icons/folder.svg"
            onClicked: rail.peekRequested()

            HoverHandler {
                id: collectionsHover
            }
        }

        Item {
            Layout.fillHeight: true
        }

        // The import entry keeps the light fill of the Import button of the open sidebar.
        IconActionButton {
            id: importButton
            objectName: "collectionsRailImport"
            Layout.alignment: Qt.AlignHCenter
            compact: true
            actionName: qsTr("Import")
            iconSrc: "qrc:/panel_icons/import.svg"
            iconColorDefault: rail.theme ? rail.theme.filledActionInk(true) : appTheme.bgBaseColor
            fillIdle: rail.theme ? rail.theme.filledActionFill(true, false, false)
                                 : appTheme.textColor
            fillHover: rail.theme ? rail.theme.filledActionFill(true, true, false)
                                  : appTheme.textColor
            fillPressed: rail.theme ? rail.theme.filledActionFill(true, true, true)
                                    : appTheme.textColor
            onClicked: rail.importRequested(importButton)
        }
    }
}
