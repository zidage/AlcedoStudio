import QtQuick
import QtQuick.Controls
import QtQuick.Effects
import QtQuick.Layouts

Item {
    id: root

    property var focusedImage: ({})
    signal ratingRequested(int rating)
    signal descriptionSaveRequested(string caption)
    signal ratingReasonSaveRequested(string reasons)
    signal contextMenuRequested(var item, real sceneX, real sceneY)

    readonly property color textColor: appTheme.textColor
    readonly property color mutedTextColor: appTheme.textMutedColor
    readonly property color accentColor: appTheme.accentColor
    readonly property color dividerColor: root.withAlpha(appTheme.textColor, 0.08)
    readonly property color chipBorderColor: root.withAlpha(appTheme.textColor, 0.10)
    readonly property color chipBgColor: root.withAlpha(appTheme.bgBaseColor, 0.52)
    readonly property bool hasFocus: focusedImage && focusedImage.success === true
    readonly property string titleText: focusedImage && focusedImage.title ? String(focusedImage.title) : qsTr("(unnamed)")
    readonly property string capturedAtText: focusedImage && focusedImage.capturedAt ? String(focusedImage.capturedAt) : ""
    readonly property string descriptionText: focusedImage && focusedImage.description ? String(focusedImage.description) : ""
    readonly property string ratingReasonText: focusedImage && focusedImage.ratingReason ? String(focusedImage.ratingReason) : ""
    readonly property int currentRating: Math.max(0, Math.min(5, Number(focusedImage && focusedImage.rating ? focusedImage.rating : 0)))

    property var interactionPolicy: null
    readonly property bool canEditRating: !root.interactionPolicy || root.interactionPolicy.canEditFocusedRating
    readonly property string editDisabledReason: root.interactionPolicy ? root.interactionPolicy.focusedEditReason : ""

    function withAlpha(color, alpha) {
        return Qt.rgba(color.r, color.g, color.b, alpha);
    }

    function tileFor(tileId) {
        const tiles = focusedImage && focusedImage.tiles ? focusedImage.tiles : [];
        for (let i = 0; i < tiles.length; ++i) {
            if (tiles[i] && String(tiles[i].id) === tileId) {
                return tiles[i];
            }
        }
        return ({
                label: "",
                value: "",
                detail: ""
            });
    }

    function trimmedText(value) {
        if (value === undefined || value === null) {
            return "";
        }
        return String(value).trim();
    }

    function nonDashText(value) {
        const text = root.trimmedText(value);
        return text === "\u2014" ? "" : text;
    }

    function exposurePart(index) {
        const raw = root.nonDashText(root.tileFor("exposure").value);
        if (raw.length === 0) {
            return "";
        }
        const parts = raw.split("\u00b7");
        if (index >= parts.length) {
            return "";
        }
        return root.trimmedText(parts[index]);
    }

    function apertureNumber() {
        const aperture = root.exposurePart(0);
        return aperture.indexOf("f/") === 0 ? aperture.substring(2) : aperture;
    }

    function shutterDisplay() {
        return root.exposurePart(1);
    }

    function isoDisplay() {
        return root.nonDashText(root.tileFor("iso").value);
    }

    function fileKind() {
        const name = root.titleText;
        const dot = name.lastIndexOf(".");
        if (dot < 0 || dot >= name.length - 1) {
            return qsTr("RAW");
        }
        const ext = name.substring(dot + 1).toUpperCase();
        const rawExt = {
            "ARW": true,
            "CR2": true,
            "CR3": true,
            "DNG": true,
            "NEF": true,
            "NRW": true,
            "ORF": true,
            "RAF": true,
            "RW2": true,
            "PEF": true,
            "SRW": true,
            "IIQ": true
        };
        return rawExt[ext] ? qsTr("RAW") : ext;
    }

    function metadataChips() {
        const chips = [root.fileKind()];
        const dimensions = root.nonDashText(focusedImage && focusedImage.dimensions);
        const aspectRatio = root.nonDashText(focusedImage && focusedImage.aspectRatio);
        const tags = root.nonDashText(focusedImage && focusedImage.semanticTags);
        if (dimensions.length > 0) {
            chips.push(dimensions);
        }
        if (aspectRatio.length > 0) {
            chips.push(aspectRatio);
        }
        if (focusedImage && focusedImage.isHdr === true) {
            chips.push(qsTr("HDR"));
        } else if (tags.length > 0) {
            chips.push(tags);
        }
        return chips.slice(0, 4);
    }

    function contextMenuItem() {
        const elementId = Number(focusedImage && (focusedImage.elementId || focusedImage.fileId));
        const fileId = Number(focusedImage && (focusedImage.fileId || focusedImage.elementId));
        return {
            elementId: elementId,
            fileId: fileId,
            imageId: Number(focusedImage && focusedImage.imageId),
            folderId: Number(focusedImage && focusedImage.folderId),
            scopeType: focusedImage && focusedImage.scopeType ? String(focusedImage.scopeType) : "",
            fileName: focusedImage && focusedImage.fileName ? String(focusedImage.fileName) : root.titleText,
            rating: root.currentRating,
            isHdr: focusedImage && focusedImage.isHdr === true
        };
    }

    function openContextMenu(anchorItem) {
        if (!root.hasFocus) {
            return;
        }
        const point = anchorItem.mapToItem(null, anchorItem.width, anchorItem.height);
        root.contextMenuRequested(root.contextMenuItem(), point.x, point.y);
    }

    component SectionDivider: Rectangle {
        Layout.fillWidth: true
        Layout.preferredHeight: 1
        color: root.dividerColor
    }

    component SectionTitle: RowLayout {
        id: section
        property string text: ""
        property string iconSource: ""
        property bool accentIcon: false

        Layout.fillWidth: true
        spacing: 7

        AccentSvgIcon {
            visible: section.iconSource.length > 0 && section.accentIcon
            Layout.preferredWidth: 14
            Layout.preferredHeight: 14
            source: section.iconSource
            iconColor: root.accentColor
        }

        Image {
            visible: section.iconSource.length > 0 && !section.accentIcon
            Layout.preferredWidth: 14
            Layout.preferredHeight: 14
            source: section.iconSource
            sourceSize.width: 14
            sourceSize.height: 14
            opacity: 0.82
        }

        Label {
            Layout.fillWidth: true
            text: section.text
            color: root.mutedTextColor
            font.family: appTheme.uiFontFamily
            font.pixelSize: 11
            font.weight: 700
            font.letterSpacing: 1.8
            font.capitalization: Font.AllUppercase
            wrapMode: Text.Wrap
        }
    }

    component AccentSvgIcon: Item {
        id: accentSvgIcon
        property string source: ""
        property color iconColor: root.accentColor

        implicitWidth: 14
        implicitHeight: 14

        Image {
            id: accentSvgSource
            anchors.fill: parent
            source: accentSvgIcon.source
            sourceSize.width: Math.max(1, width)
            sourceSize.height: Math.max(1, height)
            visible: false
        }

        MultiEffect {
            anchors.fill: parent
            source: accentSvgSource
            colorization: 1.0
            colorizationColor: accentSvgIcon.iconColor
        }
    }

    component DetailPair: ColumnLayout {
        id: detailPair
        property string label: ""
        property string value: ""
        property string detail: ""
        property string iconSource: ""

        Layout.fillWidth: true
        spacing: 7

        SectionTitle {
            text: detailPair.label
            iconSource: detailPair.iconSource
        }

        Label {
            Layout.fillWidth: true
            text: root.nonDashText(detailPair.value).length > 0 ? detailPair.value : qsTr("-")
            color: root.textColor
            font.family: appTheme.dataFontFamily
            font.pixelSize: 16
            font.weight: 500
            wrapMode: Text.WordWrap
            maximumLineCount: 2
            elide: Text.ElideRight
        }

        Label {
            Layout.fillWidth: true
            visible: root.nonDashText(detailPair.detail).length > 0
            text: detailPair.detail
            color: root.mutedTextColor
            font.family: appTheme.uiFontFamily
            font.pixelSize: 13
            font.weight: 500
            wrapMode: Text.WordWrap
            maximumLineCount: 2
            elide: Text.ElideRight
        }
    }

    component ExposureValue: RowLayout {
        id: exposureValue
        property string prefix: ""
        property string value: ""
        property string suffix: ""

        Layout.fillWidth: true
        spacing: 2

        Label {
            visible: exposureValue.prefix.length > 0
            text: exposureValue.prefix
            color: root.mutedTextColor
            font.family: appTheme.dataFontFamily
            font.pixelSize: 12
            font.weight: 500
            Layout.alignment: Qt.AlignBottom
            bottomPadding: 3
        }

        Label {
            text: root.nonDashText(exposureValue.value).length > 0 ? exposureValue.value : "-"
            color: root.textColor
            font.family: appTheme.dataFontFamily
            font.pixelSize: 22
            font.weight: 700
            wrapMode: Text.Wrap
            Layout.fillWidth: true
        }

        Label {
            visible: exposureValue.suffix.length > 0
            text: exposureValue.suffix
            color: root.mutedTextColor
            font.family: appTheme.dataFontFamily
            font.pixelSize: 11
            font.weight: 700
            Layout.alignment: Qt.AlignBottom
            bottomPadding: 4
        }
    }

    component MetadataChip: Rectangle {
        id: chip
        property string text: ""

        implicitWidth: chipLabel.implicitWidth + 18
        implicitHeight: 30
        width: implicitWidth
        height: implicitHeight
        radius: 4
        color: root.chipBgColor
        border.width: 1
        border.color: root.chipBorderColor

        Label {
            id: chipLabel
            anchors.centerIn: parent
            text: chip.text
            color: root.mutedTextColor
            font.family: appTheme.dataFontFamily
            font.pixelSize: 13
            font.weight: 600
        }
    }

    component ParagraphLabel: Label {
        Layout.fillWidth: true
        color: root.textColor
        font.family: appTheme.uiFontFamily
        font.pixelSize: 14
        font.weight: 500
        lineHeight: 1.38
        wrapMode: Text.WordWrap
    }

    component StarRatingRow: RowLayout {
        id: stars
        property bool interactive: false
        property int hoverRating: 0
        readonly property int displayRating: hoverRating > 0 ? hoverRating : root.currentRating

        Layout.fillWidth: true
        spacing: 10

        Item {
            id: starGlyphs
            Layout.preferredWidth: 120
            Layout.preferredHeight: 26

            Row {
                anchors.fill: parent
                spacing: 0
                Repeater {
                    model: 5
                    delegate: Label {
                        required property int index

                        width: 24
                        height: 26
                        text: (index + 1) <= stars.displayRating ? "\u2605" : "\u2606"
                        color: (index + 1) <= stars.displayRating ? "#FFB923" : root.mutedTextColor
                        font.family: appTheme.dataFontFamily
                        font.pixelSize: 21
                        horizontalAlignment: Text.AlignHCenter
                        verticalAlignment: Text.AlignVCenter
                    }
                }
            }

            MouseArea {
                anchors.fill: parent
                enabled: stars.interactive && root.canEditRating
                hoverEnabled: true
                cursorShape: Qt.PointingHandCursor
                acceptedButtons: Qt.LeftButton | Qt.RightButton
                function slotAt(x) {
                    return Math.max(1, Math.min(5, Math.floor(x / 24) + 1));
                }
                onPositionChanged: function (mouse) {
                    stars.hoverRating = slotAt(mouse.x);
                }
                onExited: stars.hoverRating = 0
                onClicked: function (mouse) {
                    if (mouse.button === Qt.RightButton) {
                        root.ratingRequested(0);
                        stars.hoverRating = 0;
                    } else {
                        root.ratingRequested(slotAt(mouse.x));
                    }
                }
            }
        }

        Label {
            text: root.currentRating > 0 ? qsTr("%1 / 5").arg(root.currentRating) : qsTr("Unrated")
            color: root.currentRating > 0 ? root.textColor : root.mutedTextColor
            font.family: appTheme.dataFontFamily
            font.pixelSize: 14
            font.weight: 700
            Layout.alignment: Qt.AlignVCenter
        }

        Item {
            Layout.fillWidth: true
        }
    }

    ColumnLayout {
        anchors.fill: parent
        visible: !root.hasFocus
        spacing: 12

        Item {
            Layout.fillHeight: true
        }

        Button {
            Layout.alignment: Qt.AlignHCenter
            Layout.preferredWidth: 44
            Layout.preferredHeight: 44
            flat: true
            display: AbstractButton.IconOnly
            hoverEnabled: false
            focusPolicy: Qt.NoFocus
            icon.source: "qrc:/panel_icons/image.svg"
            icon.width: 40
            icon.height: 40
            icon.color: root.withAlpha(root.mutedTextColor, 0.45)
            background: Item {}
        }

        Label {
            Layout.alignment: Qt.AlignHCenter
            text: qsTr("No Image Focused")
            color: root.textColor
            font.family: appTheme.headlineFontFamily
            font.pixelSize: 16
            font.weight: 600
        }

        Label {
            Layout.alignment: Qt.AlignHCenter
            Layout.maximumWidth: 240
            horizontalAlignment: Text.AlignHCenter
            wrapMode: Text.WordWrap
            text: qsTr("Focus a photo to inspect its camera, lens, exposure, description, and rating here.")
            color: root.mutedTextColor
            font.family: appTheme.uiFontFamily
            font.pixelSize: 12
        }

        Item {
            Layout.fillHeight: true
        }
    }

    ScrollView {
        id: focusedScroll
        anchors.fill: parent
        visible: root.hasFocus
        contentWidth: availableWidth
        clip: true

        ColumnLayout {
            width: focusedScroll.availableWidth
            spacing: 0

            Item {
                Layout.fillWidth: true
                Layout.preferredHeight: Math.max(116, inspectorHeaderColumn.implicitHeight + 34)

                ColumnLayout {
                    id: inspectorHeaderColumn
                    anchors.left: parent.left
                    anchors.right: menuButton.left
                    anchors.top: parent.top
                    anchors.bottom: parent.bottom
                    anchors.leftMargin: 16
                    anchors.rightMargin: 12
                    anchors.topMargin: 16
                    anchors.bottomMargin: 18
                    spacing: 13

                    Label {
                        Layout.fillWidth: true
                        text: root.titleText
                        color: root.textColor
                        font.family: appTheme.headlineFontFamily
                        font.pixelSize: 20
                        font.weight: 800
                        wrapMode: Text.Wrap
                    }

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 12
                        visible: root.capturedAtText.length > 0

                        Image {
                            Layout.preferredWidth: 19
                            Layout.preferredHeight: 19
                            source: "qrc:/panel_icons/calendar.svg"
                            sourceSize.width: 19
                            sourceSize.height: 19
                            opacity: 0.82
                        }

                        Label {
                            Layout.fillWidth: true
                            text: root.capturedAtText
                            color: root.mutedTextColor
                            font.family: appTheme.dataFontFamily
                            font.pixelSize: 13
                            font.weight: 500
                            wrapMode: Text.Wrap
                        }
                    }
                }

                Item {
                    id: menuButton
                    anchors.top: parent.top
                    anchors.right: parent.right
                    anchors.topMargin: 9
                    anchors.rightMargin: 7
                    width: 42
                    height: 42

                    Rectangle {
                        anchors.fill: parent
                        radius: 6
                        color: root.withAlpha(root.textColor, menuMouse.pressed ? 0.12 : (menuHover.hovered ? 0.07 : 0.0))
                    }

                    Label {
                        anchors.centerIn: parent
                        text: "\u22ee"
                        color: menuHover.hovered ? root.textColor : root.mutedTextColor
                        font.family: appTheme.dataFontFamily
                        font.pixelSize: 30
                        font.weight: 700
                    }

                    HoverHandler {
                        id: menuHover
                        enabled: root.hasFocus
                    }

                    MouseArea {
                        id: menuMouse
                        anchors.fill: parent
                        enabled: root.hasFocus
                        cursorShape: Qt.PointingHandCursor
                        onClicked: root.openContextMenu(menuButton)
                    }

                    ToolTip.visible: menuHover.hovered
                    ToolTip.text: qsTr("Image actions")
                }
            }

            SectionDivider {}

            ColumnLayout {
                Layout.fillWidth: true
                Layout.leftMargin: 16
                Layout.rightMargin: 16
                Layout.topMargin: 24
                Layout.bottomMargin: 26
                spacing: 18

                DetailPair {
                    label: root.tileFor("camera").label
                    iconSource: "qrc:/panel_icons/camera.svg"
                    value: root.tileFor("camera").value
                    detail: root.tileFor("camera").detail
                }

                DetailPair {
                    label: root.tileFor("lens").label
                    iconSource: "qrc:/panel_icons/aperture.svg"
                    value: root.tileFor("lens").value
                    detail: root.tileFor("lens").detail
                }
            }

            SectionDivider {
                Layout.leftMargin: 16
                Layout.rightMargin: 16
            }

            ColumnLayout {
                Layout.fillWidth: true
                Layout.leftMargin: 16
                Layout.rightMargin: 16
                Layout.topMargin: 30
                Layout.bottomMargin: 30
                spacing: 14

                SectionTitle {
                    text: qsTr("Exposure")
                }

                RowLayout {
                    Layout.fillWidth: true
                    spacing: 9

                    ExposureValue {
                        prefix: "f/"
                        value: root.apertureNumber()
                    }

                    Rectangle {
                        Layout.preferredWidth: 1
                        Layout.preferredHeight: 28
                        color: root.dividerColor
                    }

                    ExposureValue {
                        value: root.shutterDisplay()
                        suffix: root.shutterDisplay().length > 0 && root.shutterDisplay().charAt(root.shutterDisplay().length - 1) !== "s" ? "s" : ""
                    }

                    Rectangle {
                        Layout.preferredWidth: 1
                        Layout.preferredHeight: 28
                        color: root.dividerColor
                    }

                    ExposureValue {
                        prefix: "ISO"
                        value: root.isoDisplay()
                    }
                }
            }

            SectionDivider {
                Layout.leftMargin: 16
                Layout.rightMargin: 16
            }

            ColumnLayout {
                Layout.fillWidth: true
                Layout.leftMargin: 16
                Layout.rightMargin: 16
                Layout.topMargin: 30
                Layout.bottomMargin: 26
                spacing: 13

                SectionTitle {
                    text: qsTr("Metadata")
                }

                Flow {
                    Layout.fillWidth: true
                    spacing: 10
                    Repeater {
                        model: root.metadataChips()
                        delegate: MetadataChip {
                            required property string modelData

                            text: String(modelData)
                        }
                    }
                }
            }

            RowLayout {
                Layout.fillWidth: true
                Layout.leftMargin: 0
                Layout.rightMargin: 16
                Layout.topMargin: 6
                Layout.bottomMargin: 24
                spacing: 12

                ColumnLayout {
                    Layout.preferredWidth: 24
                    Layout.fillHeight: true
                    spacing: 10

                    AccentSvgIcon {
                        Layout.alignment: Qt.AlignHCenter
                        Layout.preferredWidth: 22
                        Layout.preferredHeight: 22
                        source: "qrc:/panel_icons/sparkles.svg"
                        iconColor: root.accentColor
                    }

                    Rectangle {
                        Layout.alignment: Qt.AlignHCenter
                        Layout.preferredWidth: 3
                        Layout.fillHeight: true
                        color: root.accentColor
                    }
                }

                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: 18

                    SectionTitle {
                        text: qsTr("AI Description")
                    }

                    ParagraphLabel {
                        text: root.descriptionText.length > 0 ? root.descriptionText : qsTr("No description yet.")
                        color: root.descriptionText.length > 0 ? root.textColor : root.mutedTextColor
                        font.italic: root.descriptionText.length === 0
                    }

                    ColumnLayout {
                        Layout.fillWidth: true
                        Layout.topMargin: 14
                        spacing: 10

                        SectionTitle {
                            text: qsTr("AI Comment")
                        }

                        Label {
                            Layout.fillWidth: true
                            text: root.ratingReasonText.length > 0 ? root.ratingReasonText : qsTr("No comment yet.")
                            color: root.ratingReasonText.length > 0 ? root.textColor : root.mutedTextColor
                            font.family: appTheme.uiFontFamily
                            font.pixelSize: 13
                            font.weight: 500
                            font.italic: root.ratingReasonText.length === 0
                            lineHeight: 1.34
                            wrapMode: Text.WordWrap
                        }
                    }

                    ColumnLayout {
                        Layout.fillWidth: true
                        Layout.topMargin: 12
                        spacing: 10

                        SectionTitle {
                            text: qsTr("AI Rating")
                        }

                        StarRatingRow {
                            interactive: true
                        }

                        Label {
                            Layout.fillWidth: true
                            visible: root.editDisabledReason.length > 0
                            text: root.editDisabledReason
                            color: root.mutedTextColor
                            font.pixelSize: 10
                            font.italic: true
                            wrapMode: Text.WordWrap
                        }
                    }
                }
            }
        }
    }
}
