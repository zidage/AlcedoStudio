import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Effects

ListView {
    id: root
    model: appModules.library.thumbnailModel
    clip: true
    cacheBuffer: 0
    spacing: 8
    // contentY is already a ListView property; restore across library Loader teardown.
    function restoreContentY(y) {
        if (y === undefined || y === null) {
            return
        }
        const minY = originY
        const maxY = originY + Math.max(0, contentHeight - height)
        contentY = Math.max(minY, Math.min(maxY, Number(y)))
    }
    readonly property color rowBg: "transparent"
    readonly property color rowBgSelected: appTheme.selectedTintColor
    readonly property color rowBgHover: appTheme.hoverColor
    readonly property color rowMuted: appTheme.textMutedColor
    readonly property color rowText: appTheme.textColor
    readonly property color rowAccent: appTheme.accentColor
    readonly property color rowDanger: appTheme.dangerColor
    readonly property color rowDangerTint: appTheme.dangerTintColor
    readonly property int dataFontWeight: 500
    readonly property real dataLetterSpacing: -0.2

    property var selectedImagesById: ({})
    property var exportQueueById: ({})

    signal imageSelectionChanged(int elementId, int imageId, string fileName, bool isHdr,
                                 bool selected)
    signal replaceSelection(var items)
    signal imageFocused(var item)
    signal contextMenuRequested(var item, real sceneX, real sceneY)

    function maybeLoadMoreThumbnails() {
        if (!appModules.library.thumbnailModel.hasMore || appModules.library.thumbnailModel.loading) {
            return
        }
        const threshold = Math.max(360, height * 0.5)
        if (contentY >= originY + Math.max(0, contentHeight - height) - threshold) {
            appModules.library.LoadMoreThumbnails()
        }
    }

    Connections {
        target: appModules.library.thumbnailModel
        function onLoadingChanged() {
            if (!appModules.library.thumbnailModel.loading) {
                Qt.callLater(root.maybeLoadMoreThumbnails)
            }
        }
    }

    onContentYChanged: maybeLoadMoreThumbnails()
    onCountChanged: maybeLoadMoreThumbnails()
    onMovementEnded: maybeLoadMoreThumbnails()

    function keyForElement(elementId) {
        return String(Number(elementId))
    }

    function isImageSelected(elementId) {
        return Object.prototype.hasOwnProperty.call(
            selectedImagesById, keyForElement(elementId))
    }

    function isImageQueued(elementId) {
        return Object.prototype.hasOwnProperty.call(
            exportQueueById, keyForElement(elementId))
    }

    function hasMultiSelectModifier(modifiers) {
        return (modifiers & Qt.ShiftModifier) || (modifiers & Qt.ControlModifier)
    }

    delegate: Rectangle {
        required property int elementId
        required property int fileId
        required property int imageId
        required property int folderId
        required property string scopeType
        required property string fileName
        required property string cameraModel
        required property string extension
        required property int iso
        required property string aperture
        required property string focalLength
        required property string captureDate
        required property int rating
        required property bool isHdr
        required property string tags
        required property string accent
        required property string thumbUrl
        required property bool thumbLoading
        required property bool thumbMissingSource
        required property string thumbErrorText
        property string liveThumbUrl: thumbUrl
        onThumbUrlChanged: liveThumbUrl = thumbUrl
        property bool liveThumbLoading: thumbLoading
        onThumbLoadingChanged: liveThumbLoading = thumbLoading
        property bool liveThumbMissingSource: thumbMissingSource
        onThumbMissingSourceChanged: liveThumbMissingSource = thumbMissingSource
        property string liveThumbErrorText: thumbErrorText
        onThumbErrorTextChanged: liveThumbErrorText = thumbErrorText
        property int pinnedElementId: 0
        property int pinnedImageId: 0
        readonly property bool thumbnailReady: liveThumbUrl.length > 0
        readonly property bool thumbnailLoadingState: liveThumbLoading
        readonly property bool thumbnailMissingState: !thumbnailReady && !thumbnailLoadingState && liveThumbMissingSource
        readonly property bool thumbnailErrorState: !thumbnailReady && !thumbnailLoadingState
                                                    && liveThumbErrorText.length > 0
        readonly property bool thumbnailProblemState: thumbnailMissingState || thumbnailErrorState
        readonly property string thumbnailProblemText: liveThumbErrorText.length > 0
                                                       ? liveThumbErrorText
                                                       : qsTr("Source file was moved or deleted")
        readonly property bool thumbnailIdleState: !thumbnailReady && !thumbnailLoadingState
                                                   && !thumbnailProblemState

        function bindThumbnailLifetime() {
            if (pinnedElementId === elementId && pinnedImageId === imageId) {
                return
            }
            if (pinnedElementId !== 0 && pinnedImageId !== 0) {
                appModules.library.SetThumbnailVisible(pinnedElementId, pinnedImageId, false)
            }
            pinnedElementId = elementId
            pinnedImageId = imageId
            liveThumbUrl = thumbUrl
            liveThumbLoading = thumbLoading
            liveThumbMissingSource = thumbMissingSource
            liveThumbErrorText = thumbErrorText
            if (pinnedElementId !== 0 && pinnedImageId !== 0) {
                appModules.library.SetThumbnailVisible(pinnedElementId, pinnedImageId, true)
            }
        }

        Component.onCompleted: bindThumbnailLifetime()
        onElementIdChanged: bindThumbnailLifetime()
        onImageIdChanged: bindThumbnailLifetime()
        Component.onDestruction: {
            if (pinnedElementId !== 0 && pinnedImageId !== 0) {
                appModules.library.SetThumbnailVisible(pinnedElementId, pinnedImageId, false)
            }
        }

        width: ListView.view.width
        height: 116
        radius: appTheme.panelRadius
        color: root.isImageSelected(elementId)
              ? root.rowBgSelected
              : (rowHoverArea.containsMouse ? root.rowBgHover : root.rowBg)
        border.width: root.isImageSelected(elementId) ? 2 : 0
        border.color: root.rowAccent
        Behavior on color { ColorAnimation { duration: 120 } }
        Behavior on border.width { NumberAnimation { duration: 150 } }

        Connections {
            target: appModules.library
            ignoreUnknownSignals: true
            function onThumbnailUpdated(updatedElementId, updatedUrl, loading, missingSource, errorText) {
                if (updatedElementId === elementId) {
                    liveThumbUrl = updatedUrl
                    liveThumbLoading = loading
                    liveThumbMissingSource = missingSource
                    liveThumbErrorText = errorText
                }
            }
        }

        RowLayout {
            anchors.fill: parent
            anchors.margins: 8
            Item {
                Layout.preferredWidth: 132
                Layout.preferredHeight: 88
                Layout.alignment: Qt.AlignVCenter
                clip: true
                Rectangle {
                    anchors.fill: parent
                    radius: 10
                    color: appTheme.bgBaseColor
                    border.width: 2
                    border.color: appTheme.dividerColor
                }
                BusyIndicator {
                    anchors.centerIn: parent
                    width: 28
                    height: 28
                    visible: thumbnailLoadingState
                    running: visible
                }
                Image {
                    id: thumbImage
                    anchors.centerIn: parent
                    width: parent.width - 4
                    height: parent.height - 4
                    source: liveThumbUrl
                    visible: false
                    asynchronous: true
                    fillMode: Image.PreserveAspectFit
                }
                Rectangle {
                    id: thumbMask
                    anchors.fill: thumbImage
                    radius: 8
                    visible: false
                    layer.enabled: true
                }
                MultiEffect {
                    anchors.fill: thumbImage
                    source: thumbImage
                    maskEnabled: true
                    maskSource: thumbMask
                    visible: thumbnailReady
                }
                Rectangle {
                    anchors.top: parent.top
                    anchors.right: parent.right
                    anchors.margins: 6
                    width: 16
                    height: 16
                    radius: 8
                    visible: thumbnailProblemState
                    color: root.rowDangerTint
                    border.width: 1
                    border.color: root.rowDanger
                }
                Column {
                    anchors.centerIn: parent
                    width: parent.width - 12
                    visible: thumbnailProblemState
                    spacing: 2
                    Label {
                        anchors.horizontalCenter: parent.horizontalCenter
                        text: "!"
                        color: root.rowDanger
                        font.family: appTheme.dataFontFamily
                        font.pixelSize: 26
                        font.weight: 700
                    }
                    Label {
                        width: parent.width
                        text: thumbnailProblemText
                        color: root.rowDanger
                        font.family: appTheme.dataFontFamily
                        font.pixelSize: 9
                        font.weight: root.dataFontWeight
                        horizontalAlignment: Text.AlignHCenter
                        wrapMode: Text.Wrap
                        maximumLineCount: 2
                        elide: Text.ElideRight
                    }
                }
                HoverHandler {
                    id: thumbHover
                }
                ToolTip.visible: thumbnailProblemState && thumbHover.hovered
                ToolTip.text: thumbnailProblemText
                ToolTip.delay: 150
            }
            ColumnLayout {
                Layout.fillWidth: true
                Layout.alignment: Qt.AlignVCenter
                spacing: 4
                Label {
                    Layout.fillWidth: true
                    text: fileName
                    color: root.rowText
                    font.family: appTheme.dataFontFamily
                    font.pixelSize: 13
                    font.weight: root.dataFontWeight
                    font.letterSpacing: root.dataLetterSpacing
                    elide: Text.ElideRight
                }
                Label {
                    Layout.fillWidth: true
                    text: qsTr("%1 | %2 | ISO %3 | f/%4 | %5mm")
                        .arg(cameraModel)
                        .arg(extension)
                        .arg(iso)
                        .arg(aperture)
                        .arg(focalLength)
                    color: root.rowMuted
                    font.family: appTheme.dataFontFamily
                    font.pixelSize: 11
                    font.weight: root.dataFontWeight
                    font.letterSpacing: root.dataLetterSpacing
                    elide: Text.ElideRight
                }
                Label {
                    Layout.fillWidth: true
                    text: qsTr("%1 | Tags: %2").arg(captureDate).arg(tags)
                    color: root.rowMuted
                    font.family: appTheme.dataFontFamily
                    font.pixelSize: 10
                    font.weight: root.dataFontWeight
                    font.letterSpacing: root.dataLetterSpacing
                    elide: Text.ElideRight
                }
            }
            Label {
                text: qsTr("%1/5").arg(rating)
                color: root.rowText
                font.family: appTheme.dataFontFamily
                font.pixelSize: 12
                font.weight: 700
                font.letterSpacing: root.dataLetterSpacing
                horizontalAlignment: Text.AlignHCenter
                Layout.alignment: Qt.AlignVCenter
            }
        }

        MouseArea {
            id: rowHoverArea
            anchors.fill: parent
            hoverEnabled: true
            acceptedButtons: Qt.LeftButton | Qt.RightButton
            cursorShape: Qt.PointingHandCursor
            onPressed: function(mouse) {
                if (mouse.button !== Qt.RightButton) {
                    return
                }
                const item = {
                    elementId: elementId,
                    fileId: fileId,
                    imageId: imageId,
                    folderId: folderId,
                    scopeType: scopeType,
                    fileName: fileName,
                    rating: rating,
                    isHdr: isHdr
                }
                root.imageFocused(item)
                const scenePoint = rowHoverArea.mapToItem(null, mouse.x, mouse.y)
                root.contextMenuRequested(item, scenePoint.x, scenePoint.y)
            }
            onClicked: function(mouse) {
                if (mouse.button !== Qt.LeftButton) {
                    return
                }
                const focusedItem = {
                    elementId: elementId,
                    fileId: fileId,
                    imageId: imageId,
                    folderId: folderId,
                    scopeType: scopeType,
                    fileName: fileName,
                    rating: rating,
                    isHdr: isHdr
                }
                root.imageFocused(focusedItem)
                if (root.hasMultiSelectModifier(mouse.modifiers)) {
                    const nextSelected = !root.isImageSelected(elementId)
                    root.imageSelectionChanged(elementId, imageId, fileName, isHdr, nextSelected)
                } else {
                    root.replaceSelection([focusedItem])
                }
            }
            onDoubleClicked: {
                root.imageFocused({
                    elementId: elementId,
                    fileId: fileId,
                    imageId: imageId,
                    folderId: folderId,
                    scopeType: scopeType,
                    fileName: fileName,
                    rating: rating,
                    isHdr: isHdr
                })
                appModules.workspaceRouter.openEditor(elementId, imageId)
            }
        }
    }
}
