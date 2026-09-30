import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// LUT browser filter card (LUT library plan L6A, section 6.5). Album-inspector
// layout: uppercase section titles and one count bar per choice. Every choice
// writes one LutLibraryModel predicate; choosing the selected bar again returns
// that dimension to All. The model owns the choices, their counts, and which
// dimensions apply (Brand and Print only outside General, Print only when a
// candidate declares one). This card keeps no filter state of its own.
Rectangle {
    id: root
    objectName: "editorLutFilterCard"

    property var browser: null

    readonly property color colText: appTheme.textColor
    readonly property color colMuted: appTheme.textMutedColor
    // Count bars use the light list tone at low alpha; the chosen row is an outline.
    readonly property color colBar: appTheme.editorListSelectedFillColor
    readonly property color colSelectionOutline: appTheme.graphSelectionOutlineColor
    readonly property int selectionOutlineWidth: appTheme.graphSelectionOutlineWidth
    readonly property int rowHeight: appTheme.lineHeightBody + appTheme.spaceSm
    // Rows shown before a section folds its remaining choices behind "Show all".
    readonly property int previewRowCount: 8

    readonly property bool anyFilterActive: !!browser
                                            && (String(browser.category) !== "all"
                                                || String(browser.source) !== ""
                                                || String(browser.brand) !== ""
                                                || String(browser.print) !== "all"
                                                || browser.favoritesOnly === true)

    radius: appTheme.panelRadius
    color: appTheme.cardSurfaceColor
    border.width: 1
    border.color: appTheme.cardBorderColor

    function withAlpha(colorValue, alphaValue) {
        return Qt.rgba(colorValue.r, colorValue.g, colorValue.b, alphaValue)
    }

    // Choices without the All entry; All is the state with no bar selected.
    function specificChoices(choices, allValue) {
        const result = []
        if (!choices)
            return result
        for (let i = 0; i < choices.length; ++i) {
            if (String(choices[i].value) !== allValue)
                result.push(choices[i])
        }
        return result
    }

    // Selects @p value, or returns the dimension to All when it is already selected.
    function toggleChoice(dimension, value, allValue) {
        if (!browser)
            return
        const current = String(browser[dimension])
        const next = current === String(value) ? allValue : String(value)
        if (dimension === "category")
            browser.category = next
        else if (dimension === "source")
            browser.source = next
        else if (dimension === "brand")
            browser.brand = next
        else if (dimension === "print")
            browser.print = next
    }

    // One filter dimension: an uppercase title and a count bar per choice.
    component FacetSection: ColumnLayout {
        id: section

        property string title: ""
        property string dimension: ""
        property string allValue: ""
        property var choices: []
        property bool showAll: false

        readonly property int maxCount: {
            let m = 1
            for (let i = 0; i < choices.length; ++i)
                m = Math.max(m, Number(choices[i].count))
            return m
        }
        readonly property int visibleCount: showAll ? choices.length
                                                    : Math.min(choices.length, root.previewRowCount)

        spacing: appTheme.spaceXs / 2

        Label {
            Layout.fillWidth: true
            Layout.bottomMargin: appTheme.spaceXs
            text: section.title
            color: root.colMuted
            font.family: appTheme.uiFontFamily
            font.pixelSize: appTheme.fontSizeCaption
            font.weight: appTheme.fontWeightStrong
            font.capitalization: Font.AllUppercase
            wrapMode: Text.Wrap
        }

        Repeater {
            model: section.visibleCount

            delegate: Item {
                id: facetRow
                required property int index
                readonly property var choice: section.choices[index]
                readonly property bool selected: choice ? choice.selected === true : false
                readonly property int count: choice ? Number(choice.count) : 0

                objectName: "editorLutFacet_" + section.dimension + "_"
                            + (choice ? String(choice.value) : "")
                Layout.fillWidth: true
                implicitHeight: Math.max(root.rowHeight, facetLabels.implicitHeight + appTheme.spaceXs)
                activeFocusOnTab: true
                Accessible.role: Accessible.CheckBox
                Accessible.checked: selected
                Accessible.name: choice ? String(choice.label) : ""

                function activate() {
                    if (choice)
                        root.toggleChoice(section.dimension, String(choice.value), section.allValue)
                }

                Keys.onSpacePressed: activate()
                Keys.onReturnPressed: activate()

                // Count bar behind every row; the chosen row adds an outline. Hover and
                // keyboard focus use the quiet hover well.
                Rectangle {
                    anchors.fill: parent
                    radius: appTheme.controlRadiusSmall
                    color: facetHover.hovered || facetRow.activeFocus
                           ? appTheme.buttonHoveredFillColor : "transparent"
                }

                Rectangle {
                    anchors.left: parent.left
                    anchors.top: parent.top
                    anchors.bottom: parent.bottom
                    width: parent.width * facetRow.count / section.maxCount
                    radius: appTheme.controlRadiusSmall
                    color: root.withAlpha(root.colBar, 0.12)
                }

                Rectangle {
                    objectName: "editorLutFacetOutline"
                    anchors.fill: parent
                    radius: appTheme.controlRadiusSmall
                    color: "transparent"
                    visible: facetRow.selected
                    border.width: root.selectionOutlineWidth
                    border.color: root.colSelectionOutline
                }

                RowLayout {
                    id: facetLabels
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.verticalCenter: parent.verticalCenter
                    anchors.leftMargin: appTheme.spaceSm
                    anchors.rightMargin: appTheme.spaceSm
                    spacing: appTheme.spaceSm

                    Label {
                        Layout.fillWidth: true
                        text: facetRow.choice ? String(facetRow.choice.label) : ""
                        color: facetRow.count > 0 || facetRow.selected ? root.colText : root.colMuted
                        font.family: appTheme.uiFontFamily
                        font.pixelSize: appTheme.fontSizeCaption
                        font.weight: facetRow.selected ? appTheme.fontWeightStrong
                                                       : appTheme.fontWeightRegular
                        wrapMode: Text.Wrap
                    }

                    Label {
                        text: String(facetRow.count)
                        color: root.colMuted
                        font.family: appTheme.dataFontFamily
                        font.pixelSize: appTheme.fontSizeCaption
                        font.weight: appTheme.fontWeightRegular
                    }
                }

                HoverHandler {
                    id: facetHover
                    cursorShape: Qt.PointingHandCursor
                }
                TapHandler {
                    onTapped: facetRow.activate()
                }
            }
        }

        Label {
            visible: section.choices.length > root.previewRowCount
            Layout.leftMargin: appTheme.spaceSm
            text: section.showAll ? qsTr("Show less")
                                  : qsTr("Show all %1").arg(section.choices.length)
            color: root.colMuted
            font.family: appTheme.uiFontFamily
            font.pixelSize: appTheme.fontSizeCaption
            font.weight: appTheme.fontWeightStrong

            HoverHandler { cursorShape: Qt.PointingHandCursor }
            TapHandler { onTapped: section.showAll = !section.showAll }
        }
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: appTheme.spaceMd
        spacing: appTheme.spaceMd

        RowLayout {
            Layout.fillWidth: true
            spacing: appTheme.spaceSm

            Label {
                objectName: "editorLutFilterTitle"
                Layout.fillWidth: true
                text: qsTr("Filters")
                color: root.colText
                font.family: appTheme.uiFontFamily
                font.pixelSize: appTheme.fontSizeSection
                font.weight: appTheme.fontWeightHeading
                wrapMode: Text.Wrap
            }

            Label {
                id: clearFiltersAction
                objectName: "editorLutClearFilters"
                visible: root.anyFilterActive
                text: qsTr("Clear")
                color: clearHover.hovered ? root.colText : root.colMuted
                font.family: appTheme.uiFontFamily
                font.pixelSize: appTheme.fontSizeCaption
                font.weight: appTheme.fontWeightStrong
                activeFocusOnTab: visible
                Accessible.role: Accessible.Button
                Accessible.name: qsTr("Clear filters")
                Keys.onReturnPressed: root.browser.clearFilters()
                Keys.onSpacePressed: root.browser.clearFilters()

                HoverHandler {
                    id: clearHover
                    cursorShape: Qt.PointingHandCursor
                }
                TapHandler { onTapped: root.browser.clearFilters() }
            }
        }

        ScrollView {
            id: facetScroll
            Layout.fillWidth: true
            Layout.fillHeight: true
            contentWidth: availableWidth
            clip: true

            ColumnLayout {
                width: facetScroll.availableWidth
                spacing: appTheme.spaceLg

                FacetSection {
                    objectName: "editorLutCategorySection"
                    Layout.fillWidth: true
                    title: qsTr("Category")
                    dimension: "category"
                    allValue: "all"
                    choices: root.specificChoices(root.browser ? root.browser.categoryChoices : [],
                                                  "all")
                }

                FacetSection {
                    objectName: "editorLutSourceSection"
                    Layout.fillWidth: true
                    visible: choices.length > 0
                    title: qsTr("Source")
                    dimension: "source"
                    allValue: ""
                    choices: root.specificChoices(root.browser ? root.browser.sourceChoices : [], "")
                }

                FacetSection {
                    objectName: "editorLutBrandSection"
                    Layout.fillWidth: true
                    visible: !!root.browser && root.browser.filmFiltersAvailable
                             && choices.length > 0
                    title: qsTr("Brand")
                    dimension: "brand"
                    allValue: ""
                    choices: root.specificChoices(root.browser ? root.browser.brandChoices : [], "")
                }

                FacetSection {
                    objectName: "editorLutPrintSection"
                    Layout.fillWidth: true
                    visible: !!root.browser && root.browser.filmFiltersAvailable
                             && root.browser.printFilterAvailable
                    title: qsTr("Print")
                    dimension: "print"
                    allValue: "all"
                    choices: root.specificChoices(root.browser ? root.browser.printChoices : [], "all")
                }

                ThemeCheckBox {
                    objectName: "editorLutFavoritesOnly"
                    Layout.fillWidth: true
                    text: qsTr("Favorites only")
                    checked: !!root.browser && root.browser.favoritesOnly === true
                    enabled: !!root.browser
                    onToggled: function(checked) {
                        if (root.browser)
                            root.browser.favoritesOnly = checked
                    }
                }
            }
        }
    }
}
