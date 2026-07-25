import QtQuick
import QtQuick.Controls
import QtQuick.Effects
import QtQuick.Layouts

Dialog {
    id: dialog
    objectName: "adjustmentTransferDialog"
    font.family: appTheme.uiFontFamily

    parent: Overlay.overlay
    modal: true
    focus: true
    closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
    padding: 0
    width: Math.min(parent ? parent.width - 44 : 720, 720)
    height: Math.min(parent ? parent.height - 56 : 680, 680)
    x: parent ? Math.round((parent.width - width) / 2) : 0
    y: parent ? Math.round((parent.height - height) / 2) : 0

    property string mode: "copy"
    property string pasteStrategy: "paste"
    property string sourceTitle: ""
    property int targetCount: 0
    property var adjustmentRows: []
    property Item blurSource: null
    property real cornerRadius: 0

    signal copyAccepted(var selectedKeys)
    signal pasteAccepted(string strategy)
    signal pasteDiscarded()

    readonly property bool copyMode: mode === "copy"
    readonly property color panelColor: appTheme.toneGraphite
    readonly property color overlayColor: appTheme.bgDeepColor
    readonly property color rowColor: Qt.rgba(1, 1, 1, 0.035)
    readonly property color rowHoverColor: Qt.rgba(1, 1, 1, 0.07)
    readonly property color borderColor: Qt.rgba(1, 1, 1, 0.08)
    readonly property color textColor: appTheme.textColor
    readonly property color mutedTextColor: appTheme.textMutedColor
    readonly property color accentColor: appTheme.accentColor
    readonly property int selectedCount: {
        let count = 0
        for (let i = 0; i < adjustmentRows.length; ++i) {
            if (adjustmentRows[i] && adjustmentRows[i].checked === true) {
                ++count
            }
        }
        return count
    }

    function selectedKeys() {
        const keys = []
        for (let i = 0; i < adjustmentRows.length; ++i) {
            const row = adjustmentRows[i]
            if (row && row.checked === true) {
                keys.push(String(row.key))
            }
        }
        return keys
    }

    function restoreListScroll(contentY) {
        Qt.callLater(function() {
            if (!listView) {
                return
            }
            const minY = listView.originY
            const maxY = Math.max(minY, listView.contentHeight - listView.height)
            listView.contentY = Math.max(minY, Math.min(contentY, maxY))
        })
    }

    function setRowsPreservingScroll(rows) {
        const contentY = listView ? listView.contentY : 0
        adjustmentRows = rows
        restoreListScroll(contentY)
    }

    function setRowChecked(index, checked) {
        if (index < 0 || index >= adjustmentRows.length) {
            return
        }
        const next = adjustmentRows.slice()
        const row = Object.assign({}, next[index])
        row.checked = checked
        next[index] = row
        setRowsPreservingScroll(next)
    }

    function setAllRowsChecked(checked) {
        const next = []
        let changed = false
        for (let i = 0; i < adjustmentRows.length; ++i) {
            const row = Object.assign({}, adjustmentRows[i])
            if (row.checked !== checked) {
                row.checked = checked
                changed = true
            }
            next.push(row)
        }
        if (changed) {
            setRowsPreservingScroll(next)
        }
    }

    function titleText() {
        return copyMode ? qsTr("Copy Adjustments") : qsTr("Paste Adjustments")
    }

    function subtitleText() {
        if (copyMode) {
            return sourceTitle.length > 0
                ? qsTr("Choose the adjustments to copy from %1.").arg(sourceTitle)
                : qsTr("Choose the adjustments to copy.")
        }
        return qsTr("Apply these copied adjustments to %1 selected images?").arg(targetCount)
    }

    Overlay.modal: Item {
        anchors.fill: parent

        Rectangle {
            id: backdropMask
            anchors.fill: parent
            radius: dialog.cornerRadius
            color: "white"
            visible: false
            layer.enabled: true
            layer.smooth: true
        }

        Item {
            anchors.fill: parent
            layer.enabled: true
            layer.smooth: true
            layer.effect: MultiEffect {
                maskEnabled: dialog.cornerRadius > 0
                maskSource: backdropMask
            }

            MultiEffect {
                anchors.fill: parent
                source: dialog.blurSource
                blurEnabled: dialog.blurSource !== null
                blur: 0.72
                blurMax: 72
                saturation: -0.24
                brightness: -0.08
            }

            Rectangle {
                anchors.fill: parent
                color: dialog.overlayColor
                opacity: dialog.blurSource !== null ? 0.55 : 0.9
            }
        }
    }

    background: Rectangle {
        radius: 14
        color: dialog.panelColor
        border.width: 1
        border.color: dialog.borderColor
    }

    contentItem: ColumnLayout {
        anchors.fill: parent
        anchors.margins: 24
        spacing: 16

        ColumnLayout {
            Layout.fillWidth: true
            spacing: 4

            Label {
                Layout.fillWidth: true
                text: dialog.titleText()
                color: dialog.textColor
                font.pixelSize: 24
                font.weight: 600
                elide: Text.ElideRight
            }

            Label {
                Layout.fillWidth: true
                text: dialog.subtitleText()
                color: dialog.mutedTextColor
                font.pixelSize: 12
                wrapMode: Text.WordWrap
            }
        }

        Rectangle {
            Layout.fillWidth: true
            Layout.fillHeight: true
            radius: 10
            color: Qt.rgba(0, 0, 0, 0.12)
            border.width: 1
            border.color: dialog.borderColor
            clip: true

            ScrollView {
                anchors.fill: parent
                anchors.margins: 8
                clip: true
                ScrollBar.horizontal.policy: ScrollBar.AlwaysOff

                ListView {
                    id: listView
                    model: dialog.adjustmentRows
                    spacing: 6
                    boundsBehavior: Flickable.StopAtBounds
                    reuseItems: true

                    delegate: Rectangle {
                        id: rowShell
                        required property int index
                        required property var modelData
                        width: ListView.view ? ListView.view.width : 0
                        height: 58
                        radius: 8
                        color: rowMouse.containsMouse ? dialog.rowHoverColor : dialog.rowColor
                        border.width: 1
                        border.color: dialog.borderColor

                        MouseArea {
                            id: rowMouse
                            anchors.fill: parent
                            hoverEnabled: true
                            onClicked: {
                                if (dialog.copyMode) {
                                    dialog.setRowChecked(rowShell.index, !(rowShell.modelData.checked === true))
                                }
                            }
                        }

                        RowLayout {
                            anchors.fill: parent
                            anchors.leftMargin: 12
                            anchors.rightMargin: 14
                            spacing: 12

                            CheckBox {
                                visible: dialog.copyMode
                                checked: rowShell.modelData.checked === true
                                onClicked: dialog.setRowChecked(rowShell.index, checked)
                            }

                            ColumnLayout {
                                Layout.fillWidth: true
                                spacing: 2

                                Label {
                                    Layout.fillWidth: true
                                    text: rowShell.modelData.label || ""
                                    color: dialog.textColor
                                    font.pixelSize: 13
                                    font.weight: 600
                                    elide: Text.ElideRight
                                }

                                Label {
                                    Layout.fillWidth: true
                                    text: rowShell.modelData.section || ""
                                    color: dialog.mutedTextColor
                                    font.pixelSize: 10
                                    font.weight: 500
                                    elide: Text.ElideRight
                                }
                            }

                            Label {
                                Layout.maximumWidth: Math.max(140, rowShell.width * 0.34)
                                text: rowShell.modelData.value || ""
                                color: dialog.mutedTextColor
                                font.family: appTheme.dataFontFamily
                                font.pixelSize: 12
                                horizontalAlignment: Text.AlignRight
                                elide: Text.ElideMiddle
                            }
                        }
                    }
                }
            }
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: 10

            Label {
                Layout.fillWidth: true
                text: dialog.copyMode
                      ? qsTr("%1 selected").arg(dialog.selectedCount)
                      : qsTr("%1 adjustments").arg(dialog.adjustmentRows.length)
                color: dialog.mutedTextColor
                font.pixelSize: 12
                elide: Text.ElideRight
            }

            Button {
                visible: dialog.copyMode
                text: qsTr("Select all")
                enabled: dialog.adjustmentRows.length > 0
                         && dialog.selectedCount < dialog.adjustmentRows.length
                onClicked: dialog.setAllRowsChecked(true)
            }

            Button {
                visible: dialog.copyMode
                text: qsTr("Unselect all")
                enabled: dialog.selectedCount > 0
                onClicked: dialog.setAllRowsChecked(false)
            }

            Button {
                objectName: "adjustmentTransferCancelButton"
                text: dialog.copyMode ? qsTr("Cancel") : qsTr("No")
                onClicked: {
                    if (!dialog.copyMode) {
                        dialog.pasteDiscarded()
                    }
                    dialog.close()
                }
            }

            Button {
                objectName: "adjustmentTransferAcceptButton"
                text: dialog.copyMode ? qsTr("OK") : qsTr("Yes")
                enabled: !dialog.copyMode || dialog.selectedCount > 0
                onClicked: {
                    if (dialog.copyMode) {
                        dialog.copyAccepted(dialog.selectedKeys())
                    } else {
                        dialog.pasteAccepted("paste")
                    }
                    dialog.close()
                }
            }
        }
    }
}
