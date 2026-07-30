import QtQuick
import QtQuick.Controls

Menu {
    id: root
    objectName: "imageContextMenu"
    property var actions: []
    property int currentRating: 0
    property bool ratingEnabled: false
    signal actionRequested(string actionId)
    signal ratingRequested(int rating)

    readonly property int dynamicActionOffset: 2

    function openAt(sceneX, sceneY) {
        x = Math.max(0, sceneX)
        y = Math.max(0, sceneY)
        open()
    }

    function starText(value) {
        let text = ""
        const rating = Math.max(0, Math.min(5, Number(value)))
        for (let i = 1; i <= 5; ++i) {
            text += i <= rating ? "\u2605" : "\u2606"
        }
        return text
    }

    Menu {
        id: ratingMenu
        title: qsTr("Rating")
        enabled: root.ratingEnabled

        MenuItem {
            text: qsTr("Unrated")
            checkable: true
            checked: root.currentRating === 0
            onTriggered: root.ratingRequested(0)
        }

        MenuSeparator {}

        Instantiator {
            model: [1, 2, 3, 4, 5]
            delegate: MenuItem {
                readonly property int ratingValue: Number(modelData)
                text: root.starText(ratingValue)
                checkable: true
                checked: root.currentRating === ratingValue
                onTriggered: root.ratingRequested(ratingValue)
            }
            onObjectAdded: function(index, object) {
                ratingMenu.insertItem(index + 2, object)
            }
            onObjectRemoved: function(index, object) {
                ratingMenu.removeItem(object)
            }
        }
    }

    MenuSeparator {}

    Instantiator {
        model: root.actions
        delegate: MenuItem {
            readonly property var actionData: modelData
            objectName: actionData && actionData.id
                        ? "imageContextAction_" + String(actionData.id)
                        : ""
            text: actionData && actionData.label ? actionData.label : ""
            enabled: !(actionData && actionData.enabled === false)
            onTriggered: root.actionRequested(actionData && actionData.id ? actionData.id : "")
        }
        onObjectAdded: function(index, object) {
            root.insertItem(index + root.dynamicActionOffset, object)
        }
        onObjectRemoved: function(index, object) {
            root.removeItem(object)
        }
    }
}
