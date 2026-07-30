import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts

ColumnLayout {
    id: panel

    property var folderController
    property var theme
    property bool backendInteractive: false
    property int selectedCount: 0
    property var folderRows: []
    property bool sortDescending: false
    property bool draftCollectionVisible: false
    property string activeUtilityTab: "search"
    readonly property var utilityTabs: [
        {
            tabId: "search",
            label: qsTr("Search"),
            iconSource: "qrc:/panel_icons/search.svg"
        },
        {
            tabId: "advanced-analysis",
            label: qsTr("Advanced Content Analysis"),
            iconSource: "qrc:/panel_icons/flask.svg"
        },
        {
            tabId: "background-tasks",
            label: qsTr("Background Tasks"),
            iconSource: "qrc:/panel_icons/clock-play.svg"
        }
    ]
    signal importRequested()
    signal searchRequested()
    signal advancedAnalysisRequested()
    signal backgroundTasksRequested()

    function withAlpha(colorValue, alphaValue) {
        return Qt.rgba(colorValue.r, colorValue.g, colorValue.b, alphaValue)
    }

    function folderSortKey(folder) {
        const fullPath = folder.path ? String(folder.path).toLowerCase() : ""
        const name = folder.name ? String(folder.name).toLowerCase() : ""
        return fullPath.length > 0 ? fullPath : name
    }

    function rebuildFolderRows() {
        const source = folderController && folderController.folders ? folderController.folders : []
        let rootRow = null
        const next = []

        for (let i = 0; i < source.length; ++i) {
            const row = source[i]
            if (!row) {
                continue
            }

            const mapped = {
                folderId: Number(row.folderId),
                elementId: Number(row.elementId),
                name: Number(row.folderId) === 0 ? qsTr("全部图片")
                                                  : (row.name ? String(row.name) : ""),
                depth: Number(row.depth),
                path: row.path ? String(row.path) : "",
                deletable: row.deletable === true
            }

            if (mapped.folderId === 0) {
                rootRow = mapped
                continue
            }

            next.push(mapped)
        }

        next.sort(function(a, b) {
            const left = folderSortKey(a)
            const right = folderSortKey(b)
            if (left === right) {
                return a.folderId - b.folderId
            }
            if (sortDescending) {
                return right < left ? -1 : 1
            }
            return left < right ? -1 : 1
        })

        folderRows = rootRow ? [rootRow].concat(next) : next
    }

    function beginCreateCollection() {
        if (draftCollectionVisible) {
            draftFocusTimer.restart()
            return
        }
        draftCollectionVisible = true
        draftCollectionField.text = ""
        draftFocusTimer.restart()
    }

    function cancelDraftCollection() {
        draftCollectionVisible = false
        draftCollectionField.text = ""
    }

    function commitDraftCollection() {
        if (!draftCollectionVisible) {
            return
        }

        const trimmed = draftCollectionField.text.trim()
        if (trimmed.length === 0) {
            cancelDraftCollection()
            return
        }

        if (!folderController) {
            return
        }
        folderController.CreateFolder(trimmed)
        cancelDraftCollection()
    }

    readonly property var foldersModule: folderController
    readonly property bool hasSelectedCollection: foldersModule
        && Number(foldersModule.currentFolderId) !== 0

    Layout.preferredWidth: 276
    Layout.minimumWidth: 276
    Layout.maximumWidth: 276
    Layout.fillHeight: true
    spacing: 12

    Component.onCompleted: rebuildFolderRows()
    onSortDescendingChanged: rebuildFolderRows()

    Connections {
        target: panel.foldersModule
        ignoreUnknownSignals: true

        function onFoldersChanged() {
            panel.rebuildFolderRows()
        }

        function onFolderSelectionChanged() {
            panel.rebuildFolderRows()
        }
    }

    Timer {
        id: draftFocusTimer
        interval: 0
        onTriggered: draftCollectionField.forceActiveFocus()
    }

    Rectangle {
        Layout.fillWidth: true
        Layout.fillHeight: true
        radius: theme.panelRadius
        color: Qt.darker(theme.colBgPanel, 1.08)
        border.width: 1
        border.color: panel.withAlpha(theme.colText, 0.05)
        clip: true

        ColumnLayout {
            anchors.fill: parent
            anchors.margins: 14
            spacing: 12

            ColumnLayout {
                Layout.fillWidth: true
                spacing: 6

                Repeater {
                    model: panel.utilityTabs

                    delegate: Rectangle {
                        id: utilityTab
                        required property var modelData

                        Layout.fillWidth: true
                        Layout.preferredHeight: 32
                        radius: 8
                        opacity: actionEnabled ? 1.0 : 0.48
                        readonly property bool actionEnabled: modelData.tabId !== "advanced-analysis"
                                                               || (panel.backendInteractive && panel.selectedCount > 0)
                        color: utilityMouse.pressed && actionEnabled
                               ? panel.withAlpha(theme.colHover, 0.34)
                               : (utilityMouse.containsMouse
                                  ? panel.withAlpha(theme.colHover, 0.24)
                                  : "transparent")

                        RowLayout {
                            anchors.fill: parent
                            anchors.leftMargin: 2
                            anchors.rightMargin: 8
                            spacing: 10

                            Image {
                                Layout.preferredWidth: 17
                                Layout.preferredHeight: 17
                                source: modelData.iconSource
                                opacity: utilityTab.actionEnabled ? 1.0 : 0.62
                                sourceSize.width: 17
                                sourceSize.height: 17
                                fillMode: Image.PreserveAspectFit
                                mipmap: false
                                smooth: false
                            }

                            Label {
                                Layout.fillWidth: true
                                text: modelData.label
                                color: panel.withAlpha(theme.colText, utilityTab.actionEnabled ? 0.92 : 0.58)
                                font.family: appTheme.uiFontFamily
                                font.pixelSize: 13
                                font.weight: 600
                                elide: Text.ElideRight
                                verticalAlignment: Text.AlignVCenter
                            }
                        }

                        MouseArea {
                            id: utilityMouse
                            anchors.fill: parent
                            hoverEnabled: true
                            cursorShape: utilityTab.actionEnabled ? Qt.PointingHandCursor : Qt.ArrowCursor
                            onClicked: {
                                if (!utilityTab.actionEnabled) {
                                    return
                                }
                                panel.activeUtilityTab = modelData.tabId
                                if (modelData.tabId === "search") {
                                    panel.searchRequested()
                                } else if (modelData.tabId === "advanced-analysis") {
                                    panel.advancedAnalysisRequested()
                                } else if (modelData.tabId === "background-tasks") {
                                    panel.backgroundTasksRequested()
                                }
                            }
                        }

                        ToolTip.visible: utilityMouse.containsMouse
                                     && modelData.tabId === "advanced-analysis"
                                     && (!panel.backendInteractive || panel.selectedCount <= 0)
                        ToolTip.text: !panel.backendInteractive
                                      ? qsTr("Open a project before running remote analysis.")
                                      : qsTr("Select one or more images for remote analysis.")
                    }
                }
            }

            RowLayout {
                Layout.fillWidth: true
                spacing: 8

                Label {
                    text: qsTr("LOCAL FOLDERS")
                    color: panel.withAlpha(theme.colText, 0.5)
                    font.pixelSize: 11
                    font.letterSpacing: 1.2
                    font.weight: 600
                }

                Item { Layout.fillWidth: true }

                Button {
                    id: sortButton
                    implicitWidth: 28
                    implicitHeight: 28
                    Layout.minimumWidth: 28
                    Layout.preferredWidth: 28
                    Layout.maximumWidth: 28
                    Layout.minimumHeight: 28
                    Layout.preferredHeight: 28
                    Layout.maximumHeight: 28
                    Layout.alignment: Qt.AlignVCenter
                    leftPadding: 0
                    rightPadding: 0
                    topPadding: 0
                    bottomPadding: 0
                    leftInset: 0
                    rightInset: 0
                    topInset: 0
                    bottomInset: 0
                    Material.foreground: theme.colText
                    onClicked: panel.sortDescending = !panel.sortDescending

                    background: Rectangle {
                        implicitWidth: 28
                        implicitHeight: 28
                        radius: width / 2
                        color: sortButton.hovered || panel.sortDescending
                               ? panel.withAlpha(theme.colHover, 0.55)
                               : "transparent"
                    }

                    contentItem: Item {
                        implicitWidth: 28
                        implicitHeight: 28
                        Image {
                            anchors.centerIn: parent
                            width: 16
                            height: 16
                            source: "qrc:/panel_icons/sort.svg"
                            sourceSize.width: 16
                            sourceSize.height: 16
                            fillMode: Image.PreserveAspectFit
                            mipmap: false
                            smooth: false
                            rotation: panel.sortDescending ? 180 : 0

                            Behavior on rotation {
                                NumberAnimation { duration: 160; easing.type: Easing.OutCubic }
                            }
                        }
                    }

                    ToolTip.visible: hovered
                    ToolTip.text: panel.sortDescending ? qsTr("Sorted Z-A") : qsTr("Sorted A-Z")
                }

                Button {
                    id: addCollectionButton
                    implicitWidth: 28
                    implicitHeight: 28
                    Layout.minimumWidth: 28
                    Layout.preferredWidth: 28
                    Layout.maximumWidth: 28
                    Layout.minimumHeight: 28
                    Layout.preferredHeight: 28
                    Layout.maximumHeight: 28
                    Layout.alignment: Qt.AlignVCenter
                    leftPadding: 0
                    rightPadding: 0
                    topPadding: 0
                    bottomPadding: 0
                    leftInset: 0
                    rightInset: 0
                    topInset: 0
                    bottomInset: 0
                    Material.foreground: theme.colText
                    onClicked: panel.beginCreateCollection()

                    background: Rectangle {
                        implicitWidth: 28
                        implicitHeight: 28
                        radius: width / 2
                        color: addCollectionButton.hovered || draftCollectionVisible
                               ? panel.withAlpha(theme.colHover, 0.55)
                               : "transparent"
                    }

                    contentItem: Item {
                        implicitWidth: 28
                        implicitHeight: 28
                        Image {
                            anchors.centerIn: parent
                            width: 16
                            height: 16
                            source: "qrc:/panel_icons/folder-plus.svg"
                            sourceSize.width: 16
                            sourceSize.height: 16
                            fillMode: Image.PreserveAspectFit
                            mipmap: false
                            smooth: false
                        }
                    }

                    ToolTip.visible: hovered
                    ToolTip.text: qsTr("New collection")
                }
            }

            Item {
                Layout.fillWidth: true
                Layout.preferredHeight: draftCollectionVisible ? 78 : 0
                clip: true

                Behavior on Layout.preferredHeight {
                    NumberAnimation { duration: 180; easing.type: Easing.OutCubic }
                }

                ColumnLayout {
                    anchors.fill: parent
                    spacing: 8
                    opacity: draftCollectionVisible ? 1.0 : 0.0

                    Behavior on opacity { NumberAnimation { duration: 120 } }

                    Label {
                        Layout.fillWidth: true
                        text: qsTr("Collection Name")
                        color: panel.withAlpha(theme.colText, 0.90)
                        font.pixelSize: 13
                        font.weight: 700
                    }

                    TextField {
                        id: draftCollectionField
                        Layout.fillWidth: true
                        Layout.preferredHeight: 48
                        selectByMouse: true
                        color: theme.colText
                        font.family: appTheme.dataFontFamily
                        font.pixelSize: 16
                        selectedTextColor: theme.colBgCanvas
                        selectionColor: panel.withAlpha(theme.colAccentSecondary, 0.6)
                        Material.foreground: theme.colText
                        Material.accent: theme.colAccentSecondary
                        background: Rectangle {
                            radius: 10
                            color: panel.withAlpha(theme.colBgBase, 0.72)
                            border.width: 1
                            border.color: draftCollectionField.activeFocus
                                          ? panel.withAlpha(theme.colAccentSecondary, 0.62)
                                          : panel.withAlpha(theme.colText, 0.12)
                        }

                        onAccepted: panel.commitDraftCollection()
                        Keys.onEscapePressed: panel.cancelDraftCollection()
                        onEditingFinished: {
                            if (panel.draftCollectionVisible) {
                                panel.commitDraftCollection()
                            }
                        }
                    }
                }
            }

            Rectangle {
                Layout.fillWidth: true
                Layout.preferredHeight: 1
                color: panel.withAlpha(theme.colText, 0.06)
            }

            ListView {
                id: folderList
                Layout.fillWidth: true
                Layout.fillHeight: true
                clip: true
                spacing: 2
                model: panel.folderRows

                ScrollIndicator.vertical: ScrollIndicator {}

                delegate: Item {
                    required property var modelData

                    width: ListView.view.width
                    height: 52

                    readonly property bool selected: modelData.folderId === Number(panel.foldersModule.currentFolderId)

                    Rectangle {
                        anchors.fill: parent
                        anchors.rightMargin: 2
                        radius: 10
                        color: selected
                               ? panel.withAlpha(theme.colHover, 0.54)
                               : folderMouse.containsMouse
                                 ? panel.withAlpha(theme.colHover, 0.28)
                                 : "transparent"
                        border.width: selected ? 1 : 0
                        border.color: selected ? panel.withAlpha(theme.colText, 0.08) : "transparent"

                        Rectangle {
                            anchors.top: parent.top
                            anchors.bottom: parent.bottom
                            anchors.right: parent.right
                            width: selected ? 2 : 0
                            radius: 1
                            color: theme.colAccentSecondary

                            Behavior on width {
                                NumberAnimation { duration: 130 }
                            }
                        }

                        RowLayout {
                            anchors.fill: parent
                            anchors.leftMargin: 10 + modelData.depth * 14
                            anchors.rightMargin: 10
                            spacing: 10

                            Image {
                                width: 15
                                height: 15
                                source: "qrc:/panel_icons/folder-open.svg"
                                sourceSize.width: 15
                                sourceSize.height: 15
                                fillMode: Image.PreserveAspectFit
                                mipmap: false
                                smooth: false
                            }

                            Label {
                                Layout.fillWidth: true
                                text: modelData.name
                                color: selected ? theme.colText : panel.withAlpha(theme.colText, 0.92)
                                font.pixelSize: 15
                                font.weight: selected ? 600 : 400
                                elide: Text.ElideRight
                            }
                        }

                        MouseArea {
                            id: folderMouse
                            anchors.fill: parent
                            hoverEnabled: true
                            cursorShape: Qt.PointingHandCursor
                            onClicked: panel.foldersModule.SelectFolder(modelData.folderId)
                        }
                    }
                }

                Label {
                    anchors.centerIn: parent
                    visible: folderList.count === 0
                    text: qsTr("No collections yet")
                    color: panel.withAlpha(theme.colText, 0.55)
                    font.pixelSize: 13
                }
            }

            Rectangle {
                Layout.fillWidth: true
                Layout.preferredHeight: hasSelectedCollection ? 38 : 0
                radius: 10
                color: hasSelectedCollection ? panel.withAlpha(theme.colDanger, 0.12) : "transparent"
                border.width: hasSelectedCollection ? 1 : 0
                border.color: hasSelectedCollection ? panel.withAlpha(theme.colDanger, 0.22) : "transparent"

                Behavior on Layout.preferredHeight {
                    NumberAnimation { duration: 160; easing.type: Easing.OutCubic }
                }

                Button {
                    anchors.fill: parent
                    visible: hasSelectedCollection
                    enabled: hasSelectedCollection && backendInteractive
                    text: qsTr("Delete collection")
                    Material.foreground: theme.colText
                    onClicked: panel.foldersModule.DeleteFolder(panel.foldersModule.currentFolderId)
                    background: Item {}

                    contentItem: Label {
                        text: parent.text
                        horizontalAlignment: Text.AlignHCenter
                        verticalAlignment: Text.AlignVCenter
                        color: panel.withAlpha(theme.colText, 0.84)
                        font.pixelSize: 12
                        font.weight: 600
                    }
                }
            }
        }
    }

    Button {
        id: importBtn
        Layout.fillWidth: true
        Layout.preferredHeight: 52
        text: qsTr("Import")
        enabled: backendInteractive
        icon.source: "qrc:/panel_icons/import.svg"
        icon.width: 16
        icon.height: 16
        icon.color: theme.colText
        display: AbstractButton.TextBesideIcon
        Material.foreground: theme.colText
        onClicked: panel.importRequested()

        background: Canvas {
            opacity: importBtn.enabled ? 1.0 : 0.5
            property color gradStart: theme.colAccentPrimary
            property color gradEnd: theme.colAccentSecondary
            onGradStartChanged: requestPaint()
            onGradEndChanged: requestPaint()
            onWidthChanged: requestPaint()
            onHeightChanged: requestPaint()
            onPaint: {
                var ctx = getContext("2d")
                ctx.clearRect(0, 0, width, height)
                var r = 8
                ctx.beginPath()
                ctx.moveTo(r, 0)
                ctx.lineTo(width - r, 0)
                ctx.quadraticCurveTo(width, 0, width, r)
                ctx.lineTo(width, height - r)
                ctx.quadraticCurveTo(width, height, width - r, height)
                ctx.lineTo(r, height)
                ctx.quadraticCurveTo(0, height, 0, height - r)
                ctx.lineTo(0, r)
                ctx.quadraticCurveTo(0, 0, r, 0)
                ctx.closePath()
                var grad = ctx.createLinearGradient(0, height, width, 0)
                grad.addColorStop(0.0, Qt.rgba(gradStart.r, gradStart.g, gradStart.b, 1.0))
                grad.addColorStop(1.0, Qt.rgba(gradEnd.r, gradEnd.g, gradEnd.b, 1.0))
                ctx.fillStyle = grad
                ctx.fill()
            }
        }

        scale: importBtn.hovered && enabled ? 1.03 : 1.0
        Behavior on scale { NumberAnimation { duration: 100; easing.type: Easing.OutCubic } }
    }
}
