import QtQuick
import QtQuick.Controls
import QtTest

// LensCatalogPicker standalone: two-tier searchable menu against an inline
// fixture catalog (mix of interchangeable lenses and a fixed-lens camera).
// Verifies fold behavior, normalized fuzzy matching ("70-200" → "…70200mm…"),
// multi-token queries, brand filtering, pick signals, detection mapping, and
// revealDetected() filter clearing.
Item {
    id: outer
    width: 1
    height: 1

    ApplicationWindow {
        id: appWin
        width: 420
        height: 720
        visible: true
        title: "LensCatalogPicker"

        // Same shape as EditorLensCatalogModel: brands = [{value,label}],
        // modelsForBrand(brand) = [{value,label}].
        property var fixtureCatalog: ({
                brands: [
                    { value: "Canon", label: "Canon" },
                    { value: "Nikon", label: "Nikon" },
                    { value: "Apple", label: "Apple" }
                ],
                modelsForBrand: function (brand) {
                    const table = {
                        "Canon": [
                            { value: "Canon EF 70-200mm f/2.8L IS II USM",
                              label: "Canon EF 70-200mm f/2.8L IS II USM" },
                            { value: "Canon EF 24-70mm f/2.8L II USM",
                              label: "Canon EF 24-70mm f/2.8L II USM" },
                            { value: "Canon DIGITAL IXUS 400 & compatibles (Standard)",
                              label: "Canon DIGITAL IXUS 400 & compatibles (Standard)" }
                        ],
                        "Nikon": [
                            { value: "Nikkor Z 70-200mm f/2.8 VR S",
                              label: "Nikkor Z 70-200mm f/2.8 VR S" }
                        ],
                        "Apple": [
                            { value: "iPhone XS back camera 4.25mm f/1.8 & compatibles",
                              label: "iPhone XS back camera 4.25mm f/1.8 & compatibles" }
                        ]
                    }
                    return table[brand] || []
                }
            })

        property var pendingProps: ({})

        Loader {
            id: pickerLoader
            anchors.top: parent.top
            anchors.topMargin: 8
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.margins: 8
            asynchronous: false
            source: lensPickerUrl
            onLoaded: {
                var props = appWin.pendingProps
                item.catalog = props.catalog !== undefined
                               ? props.catalog : appWin.fixtureCatalog
                for (var key in props) {
                    if (key !== "catalog")
                        item[key] = props[key]
                }
                pickSpy.target = item
            }
        }

        SignalSpy {
            id: pickSpy
            signalName: "lensPicked"
        }

        TestCase {
            id: tc
            name: "LensCatalogPicker"
            when: appWin.visible

            function reload(props) {
                pickSpy.target = null
                appWin.pendingProps = props || {}
                pickerLoader.active = false
                pickerLoader.active = true
                const p = pickerLoader.item
                verify(p, "picker failed to load")
                return p
            }

            function values(list) {
                var out = []
                for (var i = 0; i < list.length; ++i)
                    out.push(list[i].value)
                return out
            }

            function test_expandCollapse() {
                const p = reload()
                verify(!p.expanded)
                const menu = findChild(p, "lensPickerMenu")
                verify(menu)
                verify(!menu.visible)
                p.toggleExpanded()
                verify(p.expanded)
                tryVerify(function () { return menu.visible }, 1000)
                p.toggleExpanded()
                tryVerify(function () { return !menu.visible }, 1000)
            }

            function test_fieldClickToggles() {
                const p = reload()
                const field = findChild(p, "lensPickerField")
                verify(field)
                mouseClick(field)
                verify(p.expanded)
            }

            function test_fuzzyRangeQueryMatchesMmEntries() {
                const p = reload()
                p.expanded = true
                p.lensQuery = "70-200"
                const found = values(p.filteredEntries)
                compare(found.length, 2, "expected both 70-200mm entries; got " + found)
                verify(found.indexOf("Canon EF 70-200mm f/2.8L IS II USM") >= 0)
                verify(found.indexOf("Nikkor Z 70-200mm f/2.8 VR S") >= 0)
            }

            function test_multiTokenQueryRequiresAllTokens() {
                const p = reload()
                p.expanded = true
                p.lensQuery = "canon 70-200"
                const found = values(p.filteredEntries)
                compare(found.length, 1, "canon+70-200 should keep only the Canon row; got " + found)
                compare(found[0], "Canon EF 70-200mm f/2.8L IS II USM")
                // Token order must not matter.
                p.lensQuery = "70-200 canon"
                compare(values(p.filteredEntries).length, 1)
            }

            function test_fixedLensCameraEntryMatches() {
                const p = reload()
                p.expanded = true
                p.lensQuery = "ixus 400"
                const found = values(p.filteredEntries)
                compare(found.length, 1, "fixed-lens camera row must match; got " + found)
                compare(found[0], "Canon DIGITAL IXUS 400 & compatibles (Standard)")
                // Unpunctuated query still resolves the '&' / parentheses row.
                p.lensQuery = "iphone xs"
                const phone = values(p.filteredEntries)
                compare(phone.length, 1)
                compare(phone[0], "iPhone XS back camera 4.25mm f/1.8 & compatibles")
            }

            function test_brandFilterNarrowsList() {
                const p = reload()
                p.expanded = true
                compare(p.filteredEntries.length, 5)
                p.brandFilter = "Canon"
                const found = p.filteredEntries
                compare(found.length, 3)
                for (var i = 0; i < found.length; ++i)
                    compare(found[i].brand, "Canon")
                // Brand query inside the sub-menu filters brand rows.
                p.brandQuery = "nik"
                compare(p.filteredBrands.length, 1)
                compare(p.filteredBrands[0].value, "Nikon")
                // Clearing the filter shows all entries again.
                p.brandFilter = ""
                p.brandQuery = ""
                compare(p.filteredEntries.length, 5)
            }

            function test_pickEmitsLensPicked() {
                const p = reload()
                p.expanded = true
                p.lensQuery = "nikkor"
                compare(p.filteredEntries.length, 1)
                p.highlightedIndex = 0
                pickSpy.clear()
                verify(p.pickHighlighted())
                compare(pickSpy.count, 1)
                compare(pickSpy.signalArguments[0][0], "Nikon")
                compare(pickSpy.signalArguments[0][1], "Nikkor Z 70-200mm f/2.8 VR S")
            }

            function test_detectedEntryFromExifIdentity() {
                const p = reload({ exifLensMake: "Canon",
                                   exifLensModel: "Canon EF 70-200mm f/2.8L IS II USM" })
                verify(p.detectionAvailable)
                compare(p.detectedEntry.brand, "Canon")
                compare(p.detectedEntry.value, "Canon EF 70-200mm f/2.8L IS II USM")
            }

            function test_detectionFallsBackToContainment() {
                // Real EXIF LensModel strings often omit the maker prefix.
                const p = reload({ exifLensMake: "Nikon",
                                   exifLensModel: "Z 70-200mm f/2.8 VR S" })
                verify(p.detectionAvailable)
                compare(p.detectedEntry.brand, "Nikon")
                compare(p.detectedEntry.value, "Nikkor Z 70-200mm f/2.8 VR S")
            }

            function test_detectionFailureMeansNull() {
                const p = reload({ exifLensMake: "Acme",
                                   exifLensModel: "Acme Mystery Lens 9000" })
                verify(!p.detectionAvailable)
                compare(p.detectedEntry, null)
                // Missing EXIF model entirely: still no detection.
                const p2 = reload({ exifLensMake: "Canon", exifLensModel: "" })
                verify(!p2.detectionAvailable)
            }

            function test_revealDetectedClearsFiltersAndHighlights() {
                const p = reload({ exifLensMake: "Canon",
                                   exifLensModel: "Canon EF 70-200mm f/2.8L IS II USM" })
                p.brandFilter = "Nikon"
                p.brandQuery = "nik"
                p.lensQuery = "nikkor"
                p.brandMenuExpanded = true
                p.revealDetected()
                compare(p.brandFilter, "")
                compare(p.brandQuery, "")
                compare(p.lensQuery, "")
                verify(!p.brandMenuExpanded)
                verify(p.expanded)
                const idx = p.indexOfEntry("Canon", "Canon EF 70-200mm f/2.8L IS II USM")
                verify(idx >= 0)
                compare(p.highlightedIndex, idx)
            }

            function test_emptyQueryState() {
                const p = reload()
                p.expanded = true
                p.lensQuery = "zzz-no-such-lens"
                compare(p.entryCount, 0)
                const hint = findChild(p, "lensEmptyHint")
                verify(hint)
                tryVerify(function () { return hint.visible }, 1000)
            }
        }
    }
}
