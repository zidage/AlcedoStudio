//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

import QtQuick
import QtQuick.Controls
import QtQuick.Controls.impl
import QtQuick.Layouts
import QtQuick.Shapes

// One choice of Import Folder (#1077, #1078): Import into Collection or Mount Folder. The card
// has a title with a radio mark, a diagram, and two marks: a check for the property the choice
// has and a cross for the property it lacks.
//
// - Import: a one-way arrow from a disk tree into one collection box.
// - Mount: a one-way arrow from a disk tree to a tree of the same shape, with a sync mark on the
//   arrow. There is no arrow from the library back to the disk.
//
// The card only reports activation; the owner (ImportFolderChoiceCards) holds the selection.
Item {
    id: card

    /// "import" or "mount".
    property string action: "import"
    property bool selected: false
    /// Label below the collection box of the Import diagram: the destination name in the Import
    /// Folder dialog. Empty shows the generic word (the Settings page).
    property string collectionName: ""
    /// Height of the drawing of the diagram, without its labels.
    property real drawingHeight: 40

    signal activated()

    readonly property bool mountAction: action === "mount"
    readonly property string title: mountAction ? qsTr("Mount Folder")
                                                : qsTr("Import into Collection")
    readonly property bool hovered: pointer.containsMouse
    readonly property color inkColor: selected ? appTheme.textColor : appTheme.textMutedColor
    readonly property color selectedEdgeColor: appTheme.editorListSelectedFillColor
    readonly property int padding: appTheme.spaceMd

    implicitWidth: 220
    implicitHeight: content.implicitHeight + 2 * padding
    // A disabled card (DESIGN.md "Component states") draws in the muted ink.
    activeFocusOnTab: enabled

    Accessible.role: Accessible.RadioButton
    Accessible.name: title
    Accessible.checkable: true
    Accessible.checked: selected
    Accessible.onPressAction: card.activated()

    Keys.onPressed: function(event) {
        if (event.key === Qt.Key_Space || event.key === Qt.Key_Return
                || event.key === Qt.Key_Enter) {
            card.activated()
            event.accepted = true
        }
    }

    // SVG path of a rounded rectangle, in the 200 x 44 coordinates of the diagram.
    function roundedRect(x, y, w, h, r) {
        return "M" + (x + r) + " " + y + "H" + (x + w - r)
                + "A" + r + " " + r + " 0 0 1 " + (x + w) + " " + (y + r)
                + "V" + (y + h - r)
                + "A" + r + " " + r + " 0 0 1 " + (x + w - r) + " " + (y + h)
                + "H" + (x + r)
                + "A" + r + " " + r + " 0 0 1 " + x + " " + (y + h - r)
                + "V" + (y + r)
                + "A" + r + " " + r + " 0 0 1 " + (x + r) + " " + y + "Z"
    }

    // A folder tree at @p x: a parent folder and two child folders on a guide line.
    function folderTree(x) {
        return roundedRect(x, 5, 15, 10, 2)
                + "M" + (x + 5) + " 15V36M" + (x + 5) + " 24h7M" + (x + 5) + " 36h7"
                + roundedRect(x + 12, 19.5, 13, 9, 2)
                + roundedRect(x + 12, 31.5, 13, 9, 2)
    }

    // Photo cells inside the collection box of the Import diagram.
    function collectionCells() {
        let path = ""
        for (let i = 0; i < 3; ++i) {
            for (let j = 0; j < 2; ++j)
                path += roundedRect(140 + i * 14, 11 + j * 13, 10, 8, 1.5)
        }
        return path
    }

    Rectangle {
        anchors.fill: parent
        radius: appTheme.controlRadius
        color: card.selected ? appTheme.buttonSelectedFillColor
                             : card.hovered ? appTheme.hoverColor : appTheme.bgBaseColor
        border.width: card.selected ? 2 : 1
        border.color: card.selected ? card.selectedEdgeColor
                                    : card.activeFocus ? appTheme.accentColor
                                                       : appTheme.controlBorderColor
    }

    ColumnLayout {
        id: content
        anchors.fill: parent
        anchors.margins: card.padding
        spacing: appTheme.spaceSm

        RowLayout {
            Layout.fillWidth: true
            spacing: appTheme.spaceSm

            IconImage {
                Layout.preferredWidth: 16
                Layout.preferredHeight: 16
                Layout.alignment: Qt.AlignVCenter
                sourceSize: Qt.size(16, 16)
                source: card.mountAction ? "qrc:/panel_icons/folder-symlink.svg"
                                         : "qrc:/panel_icons/tray-download.svg"
                color: card.selected ? appTheme.textColor : appTheme.iconColor
            }

            Label {
                Layout.fillWidth: true
                Layout.alignment: Qt.AlignVCenter
                text: card.title
                elide: Text.ElideRight
                color: card.enabled ? appTheme.textColor : appTheme.textMutedColor
                font.family: appTheme.uiFontFamily
                font.pixelSize: appTheme.fontSizeTitle
                font.weight: appTheme.fontWeightStrong
            }

            // Radio mark: a hairline ring, a thick light ring when selected.
            Rectangle {
                Layout.preferredWidth: 14
                Layout.preferredHeight: 14
                Layout.alignment: Qt.AlignVCenter
                radius: 7
                color: "transparent"
                border.width: card.selected ? 4 : 1.5
                border.color: card.selected ? card.selectedEdgeColor : appTheme.textMutedColor
            }
        }

        // The diagram: a drawing in 200 x 44 coordinates that fits the card width, and the labels
        // below it in the UI font.
        Item {
            id: diagram
            Layout.fillWidth: true
            Layout.preferredHeight: card.drawingHeight + labelRow.height + appTheme.spaceXs
            Layout.topMargin: appTheme.spaceXs

            readonly property real drawingScale: Math.max(0.1, Math.min(width / 200,
                                                                         card.drawingHeight / 44))
            readonly property real drawingX: (width - 200 * drawingScale) / 2

            function labelCenterX(diagramX) {
                return drawingX + diagramX * drawingScale
            }

            Item {
                id: drawing
                x: diagram.drawingX
                width: 200
                height: 44
                scale: diagram.drawingScale
                transformOrigin: Item.TopLeft

                Shape {
                    anchors.fill: parent
                    preferredRendererType: Shape.CurveRenderer

                    ShapePath {
                        fillColor: "transparent"
                        strokeColor: card.inkColor
                        strokeWidth: 1.4
                        capStyle: ShapePath.RoundCap
                        joinStyle: ShapePath.RoundJoin
                        PathSvg {
                            path: card.mountAction
                                  ? card.folderTree(18) + card.folderTree(150)
                                    + "M70 28h56M120 23l6 5-6 5"
                                  : card.folderTree(18) + "M70 23h50M114 18l6 5-6 5"
                                    + card.roundedRect(134, 5, 50, 36, 4)
                        }
                    }

                    ShapePath {
                        fillColor: card.mountAction
                                   ? "transparent"
                                   : Qt.rgba(card.inkColor.r, card.inkColor.g, card.inkColor.b,
                                             0.55)
                        strokeColor: "transparent"
                        PathSvg {
                            path: card.mountAction ? "" : card.collectionCells()
                        }
                    }
                }

                // Sync mark on the Mount arrow.
                Shape {
                    visible: card.mountAction
                    x: 91
                    y: 4
                    width: 24
                    height: 24
                    scale: 0.58
                    transformOrigin: Item.TopLeft
                    preferredRendererType: Shape.CurveRenderer

                    ShapePath {
                        fillColor: "transparent"
                        strokeColor: card.inkColor
                        strokeWidth: 2.4
                        capStyle: ShapePath.RoundCap
                        joinStyle: ShapePath.RoundJoin
                        PathSvg {
                            path: "M20 11A8.1 8.1 0 0 0 4.5 9M4 5v4h4M4 13a8.1 8.1 0 0 0 15.5 2m.5 4v-4h-4"
                        }
                    }
                }
            }

            Item {
                id: labelRow
                y: card.drawingHeight + appTheme.spaceXs
                width: parent.width
                height: diskLabel.implicitHeight

                Label {
                    id: diskLabel
                    x: Math.max(0, diagram.labelCenterX(31) - width / 2)
                    width: Math.min(implicitWidth, diagram.width / 2)
                    text: qsTr("Disk")
                    elide: Text.ElideRight
                    color: card.inkColor
                    font.family: appTheme.uiFontFamily
                    font.pixelSize: appTheme.fontSizeCaption
                }

                Label {
                    readonly property real centerX: card.mountAction ? diagram.labelCenterX(162)
                                                                     : diagram.labelCenterX(159)
                    width: Math.min(implicitWidth, diagram.width / 2)
                    x: Math.min(diagram.width - width, centerX - width / 2)
                    text: card.mountAction ? qsTr("Library")
                                           : card.collectionName.length > 0 ? card.collectionName
                                                                            : qsTr("Collection")
                    elide: Text.ElideRight
                    color: card.inkColor
                    font.family: appTheme.uiFontFamily
                    font.pixelSize: appTheme.fontSizeCaption
                }
            }
        }

        ImportFolderChoiceMark {
            Layout.fillWidth: true
            checked: !card.mountAction
            text: qsTr("Arrange freely")
        }

        ImportFolderChoiceMark {
            Layout.fillWidth: true
            checked: card.mountAction
            text: qsTr("Sync folder structure")
        }
    }

    MouseArea {
        id: pointer
        anchors.fill: parent
        hoverEnabled: true
        cursorShape: Qt.PointingHandCursor
        onClicked: {
            card.forceActiveFocus(Qt.MouseFocusReason)
            card.activated()
        }
    }

    // One property line of a card: a check when the choice has the property, a cross when it
    // lacks it.
    component ImportFolderChoiceMark: RowLayout {
        id: mark

        property bool checked: false
        property string text: ""

        spacing: appTheme.spaceSm
        opacity: checked ? 1.0 : 0.75
        Accessible.role: Accessible.StaticText
        Accessible.name: text

        Shape {
            Layout.preferredWidth: 13
            Layout.preferredHeight: 13
            Layout.alignment: Qt.AlignVCenter
            preferredRendererType: Shape.CurveRenderer

            ShapePath {
                fillColor: "transparent"
                strokeColor: mark.checked && card.enabled ? appTheme.editorListSelectedFillColor
                                                          : appTheme.textMutedColor
                // The path is in 24 x 24 icon coordinates; the scale does not widen the stroke.
                strokeWidth: 2.4 * 13 / 24
                capStyle: ShapePath.RoundCap
                joinStyle: ShapePath.RoundJoin
                scale: Qt.size(13 / 24, 13 / 24)
                PathSvg {
                    path: mark.checked ? "M5 12l5 5L20 7" : "M18 6L6 18M6 6l12 12"
                }
            }
        }

        Label {
            Layout.fillWidth: true
            Layout.alignment: Qt.AlignVCenter
            text: mark.text
            elide: Text.ElideRight
            color: mark.checked && card.enabled ? appTheme.textColor : appTheme.textMutedColor
            font.family: appTheme.uiFontFamily
            font.pixelSize: appTheme.fontSizeCaption
        }
    }
}
