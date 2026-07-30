import QtQuick
import QtQuick.Layouts

// Shared compact icon navigation used by editor tool groups. The model entries
// provide key, icon, label, and itemObjectName fields; selection stays owned by the
// caller so it can survive Loader teardown.
Rectangle {
    id: root

    property var items: []
    property string currentKey: ""
    property bool controlsEnabled: true
    property color trackColor: appTheme.bgBaseColor
    property color trackBorderColor: appTheme.cardBorderColor
    property color idleIconColor: appTheme.textMutedColor
    property color selectedFillColor: appTheme.editorListSelectedFillColor
    property color selectedInkColor: appTheme.editorListSelectedInkColor
    property string thumbObjectName: ""

    signal activated(string key)

    readonly property int navHit: appTheme.iconButtonHitSizeCompact
    readonly property int navSpacing: 2
    readonly property int navTrackInset: appTheme.spaceXs
    readonly property int navIndex: {
        for (let index = 0; index < root.items.length; ++index) {
            if (String(root.items[index].key) === root.currentKey)
                return index
        }
        return 0
    }
    readonly property int thumbSize: Math.min(
        navHit - navTrackInset,
        Math.max(appTheme.iconOpticalSizeCompact + appTheme.spaceSm,
                 navHit - appTheme.spaceSm - navTrackInset))

    implicitWidth: navRow.implicitWidth + 2 * navTrackInset
    implicitHeight: navHit + appTheme.spaceXs
    radius: appTheme.controlRadiusSmall
    color: trackColor
    border.width: 1
    border.color: trackBorderColor

    Item {
        id: navHost
        anchors.centerIn: parent
        width: navRow.width
        height: root.navHit
        opacity: root.controlsEnabled ? 1.0 : 0.55

        Rectangle {
            id: navThumb
            objectName: root.thumbObjectName
            z: 0
            width: root.thumbSize
            height: root.thumbSize
            radius: Math.max(4, appTheme.controlRadiusSmall - 2)
            color: root.selectedFillColor
            y: (parent.height - height) / 2
            x: root.navIndex * (root.navHit + root.navSpacing)
               + (root.navHit - width) / 2

            Behavior on x {
                enabled: !appTheme.reduceMotion
                NumberAnimation {
                    duration: Math.max(appTheme.motionFoldOpenMs, 240)
                    easing.type: Easing.OutBack
                    easing.overshoot: 1.18
                }
            }

            transformOrigin: Item.Center

            Connections {
                target: root
                function onNavIndexChanged() {
                    if (appTheme.reduceMotion) {
                        navThumb.scale = 1.0
                        return
                    }
                    thumbLandAnim.restart()
                }
            }

            SequentialAnimation {
                id: thumbLandAnim
                NumberAnimation {
                    target: navThumb
                    property: "scale"
                    to: 0.90
                    duration: 70
                    easing.type: Easing.OutQuad
                }
                NumberAnimation {
                    target: navThumb
                    property: "scale"
                    to: 1.0
                    duration: 200
                    easing.type: Easing.OutBack
                    easing.overshoot: 1.4
                }
            }
        }

        Row {
            id: navRow
            z: 1
            spacing: root.navSpacing

            component NavButton: IconActionButton {
                required property int itemIndex
                readonly property var entry: itemIndex < root.items.length
                                                     ? root.items[itemIndex] : ({})

                objectName: String(entry.itemObjectName || "")
                visible: itemIndex < root.items.length
                compact: true
                enabled: root.controlsEnabled
                selected: root.currentKey === String(entry.key || "")
                showHoverFill: false
                showFocusRing: false
                iconSrc: entry.icon || ""
                actionName: entry.label || ""
                iconColorDefault: selected ? root.selectedInkColor : root.idleIconColor
                iconColorMuted: root.idleIconColor
                iconColorSelected: root.selectedInkColor
                fillIdle: "transparent"
                fillSelected: "transparent"
                onClicked: root.activated(String(entry.key || ""))
            }

            // Explicit instances preserve QObject parentage for accessibility
            // discovery and UI automation; a Repeater's delegates are visual
            // children but are not found by QObject::findChild.
            NavButton { itemIndex: 0 }
            NavButton { itemIndex: 1 }
            NavButton { itemIndex: 2 }
            NavButton { itemIndex: 3 }
            NavButton { itemIndex: 4 }
            NavButton { itemIndex: 5 }
        }
    }
}
