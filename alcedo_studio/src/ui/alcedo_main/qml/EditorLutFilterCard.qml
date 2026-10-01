import QtQuick
import QtQuick.Controls
import QtQuick.Controls.impl
import QtQuick.Layouts

// LUT browser filter sidebar (LUT library plan L6A, section 6.5). Album-inspector
// layout: uppercase section titles and one count bar per choice. Every choice
// writes one LutLibraryModel predicate; choosing the selected bar again returns
// that dimension to All. Favorites is the first choice, above the sections.
// The model owns the choices, their counts, and which dimensions apply (Brand
// and Print only outside General, Print only when the library declares a print
// film or paper; each print is one choice).
// This sidebar keeps no filter state of its own.
//
// Docked, it sits on the browser's card surface with no chrome; `floating`
// (the page is too narrow to dock it) adds the card border above the results.
Rectangle {
    id: root
    objectName: "editorLutFilterCard"

    property var browser: null
    property bool floating: false

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
                                                || String(browser.print) !== ""
                                                || browser.favoritesOnly === true)

    radius: appTheme.controlRadiusSmall
    color: appTheme.cardSurfaceColor
    border.width: floating ? 1 : 0
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
        const current = dimension === "favorites"
                        ? (browser.favoritesOnly ? "favorites" : allValue)
                        : String(browser[dimension])
        const next = current === String(value) ? allValue : String(value)
        if (dimension === "category")
            browser.category = next
        else if (dimension === "source")
            browser.source = next
        else if (dimension === "brand")
            browser.brand = next
        else if (dimension === "print")
            browser.print = next
        else if (dimension === "favorites")
            browser.favoritesOnly = next === "favorites"
    }

    // One filter dimension: an uppercase title and a count bar per choice.
    component FacetSection: ColumnLayout {
        id: section

        property string title: ""
        // Optional glyph before every row label.
        property url iconSrc: ""
        property color iconColor: appTheme.iconColor
        property string dimension: ""
        property string allValue: ""
        property var choices: []
        property bool showAll: false
        // Count the bars are scaled to; 0 scales them to the largest choice.
        property int barTotal: 0

        readonly property int maxCount: {
            if (barTotal > 0)
                return barTotal
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
            visible: text.length > 0
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

                    ColorImage {
                        visible: String(section.iconSrc).length > 0
                        Layout.preferredWidth: appTheme.iconOpticalSizeCompact
                        Layout.preferredHeight: appTheme.iconOpticalSizeCompact
                        source: section.iconSrc
                        sourceSize.width: appTheme.iconSourceSizeCompact
                        sourceSize.height: appTheme.iconSourceSizeCompact
                        color: section.iconColor
                    }

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
        anchors.margins: root.floating ? appTheme.spaceSm : 0
        spacing: appTheme.spaceSm

        RowLayout {
            Layout.fillWidth: true
            spacing: appTheme.spaceSm

            Label {
                objectName: "editorLutFilterTitle"
                Layout.fillWidth: true
                text: qsTr("Filters")
                color: root.colText
                font.family: appTheme.uiFontFamily
                font.pixelSize: appTheme.fontSizeTitle
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

                // Favorites: one row with the star; choosing it again shows every LUT.
                FacetSection {
                    objectName: "editorLutFavoritesSection"
                    Layout.fillWidth: true
                    dimension: "favorites"
                    allValue: "all"
                    iconSrc: "qrc:/panel_icons/star.svg"
                    iconColor: root.browser && root.browser.favoritesOnly
                               ? appTheme.editorListFavoriteActiveColor : appTheme.iconColor
                    // The lone row's bar shows the starred share of every candidate.
                    barTotal: root.browser && root.browser.favoriteChoices.length > 0
                              ? Number(root.browser.favoriteChoices[0].count) : 0
                    choices: root.specificChoices(root.browser ? root.browser.favoriteChoices : [],
                                                  "all")
                }

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
                    allValue: ""
                    choices: root.specificChoices(root.browser ? root.browser.printChoices : [], "")
                }
            }
        }
    }
}
