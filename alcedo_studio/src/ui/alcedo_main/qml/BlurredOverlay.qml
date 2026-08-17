import QtQuick
import QtQuick.Effects

// Shared blurred-modal shell: snapshots `blurSource`, applies a MultiEffect
// blur, dims with `overlayColor`, and swallows pointer events. Callers drop
// their centered card (or any content) as direct children — the default
// property routes them into `cardSlot`, stacked above the blur/dim layers.
Item {
    id: root
    property Item blurSource: null
    property color overlayColor: Qt.rgba(0, 0, 0, 0.5)
    property bool blurActive: blurSource !== null
    default property alias card: cardSlot.children

    ShaderEffectSource {
        id: snapshot
        width: root.blurActive && root.blurSource ? root.blurSource.width : 0
        height: root.blurActive && root.blurSource ? root.blurSource.height : 0
        sourceItem: root.blurActive ? root.blurSource : null
        sourceRect: root.blurActive && root.blurSource ? Qt.rect(0, 0, root.blurSource.width, root.blurSource.height) : Qt.rect(0, 0, 0, 0)
        textureSize: root.blurActive && root.blurSource ? Qt.size(Math.max(1, root.blurSource.width), Math.max(1, root.blurSource.height)) : Qt.size(1, 1)
        live: root.blurActive
        recursive: false
        hideSource: false
        visible: false
    }

    MultiEffect {
        visible: root.blurActive
        x: root.blurActive && root.blurSource ? root.blurSource.x : 0
        y: root.blurActive && root.blurSource ? root.blurSource.y : 0
        width: root.blurActive && root.blurSource ? root.blurSource.width : 0
        height: root.blurActive && root.blurSource ? root.blurSource.height : 0
        source: snapshot
        blurEnabled: root.blurActive
        blur: 0.6
        blurMax: 64
        saturation: -0.2
        autoPaddingEnabled: false
    }

    Rectangle {
        anchors.fill: parent
        color: root.overlayColor
    }

    MouseArea { anchors.fill: parent; hoverEnabled: true }

    Item {
        id: cardSlot
        anchors.fill: parent
    }
}