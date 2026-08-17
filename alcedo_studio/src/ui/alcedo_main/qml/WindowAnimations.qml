import QtQuick

// Window-state requests stay native. Platform frame integration is installed
// before the production window is shown.
Item {
    id: root

    property var host: null

    function minimize() {
        if (root.host) {
            root.host.showMinimized()
        }
    }

    function toggleMaximize() {
        if (!root.host) {
            return
        }

        if (root.host.windowMaximized) {
            root.host.showNormal()
        } else {
            root.host.showMaximized()
        }
    }

    function maximize() {
        if (root.host && !root.host.windowMaximized) {
            root.host.showMaximized()
        }
    }
}
