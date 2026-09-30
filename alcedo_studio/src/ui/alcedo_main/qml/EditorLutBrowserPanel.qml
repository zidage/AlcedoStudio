import QtQuick
import QtQuick.Layouts

// LUT browser: the `luts` page of the Editor left rail (LUT library plan L6A,
// sections 1.4 and 6.5). Two cards share the page: filters, and the result
// grid with the target indicator. Both bind the application-wide browser model
// and target controller, so every LUT surface shows one library and one target.
// Opening or closing this page never submits an edit and never changes the
// image, the node selection, or the route.
Item {
    id: root
    objectName: "editorLutBrowserPanel"

    property var theme: null
    property var editorSession: null
    property var host: null
    readonly property var modules: (typeof appModules !== "undefined" && appModules) ? appModules : null
    property var browser: modules && modules.lutBrowser ? modules.lutBrowser : null
    property var target: modules && modules.lutTarget ? modules.lutTarget : null
    property var library: modules && modules.lutLibrary ? modules.lutLibrary : null

    // Rail scroll restore (EditorWorkspaceRail captures these per page).
    readonly property real listContentY: resultCard.listContentY
    function restoreListContentY(y) {
        resultCard.restoreListContentY(y)
    }
    function focusSearch() {
        resultCard.focusSearch()
    }

    RowLayout {
        anchors.fill: parent
        spacing: appTheme.spaceSm

        EditorLutFilterCard {
            id: filterCard
            Layout.preferredWidth: appTheme.editorLutBrowserFilterWidth
            Layout.fillHeight: true
            browser: root.browser
        }

        EditorLutResultCard {
            id: resultCard
            Layout.fillWidth: true
            Layout.fillHeight: true
            browser: root.browser
            target: root.target
            library: root.library
            host: root.host
        }
    }
}
