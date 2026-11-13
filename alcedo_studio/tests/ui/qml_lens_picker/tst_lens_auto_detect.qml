import QtQuick
import QtQuick.Controls
import QtTest

// Auto-recognition behavior for the RAW Decode lens section against the
// production panel + real lens catalog. lensSession (C++ context object)
// records every submitPatch and exposes a scripted exifLensMake/exifLensModel
// so detection success/failure is deterministic.
Item {
    id: outer
    width: 1
    height: 1

    ApplicationWindow {
        id: appWin
        width: 440
        height: 780
        visible: true
        title: "LensAutoDetect"

        // Canonical detected pair in the production catalog.
        readonly property string exifMake: "Canon"
        readonly property string exifModel: "Canon EF 70-200mm f/2.8L IS II USM"
        readonly property string manualMake: "Nikon"
        readonly property string manualModel: "Nikkor Z 70-200mm f/2.8 VR S"

        Loader {
            id: panelLoader
            anchors.fill: parent
            asynchronous: false
            source: lensPanelUrl
            onLoaded: {
                item.editorSession = lensSession
                item.width = appWin.width
                item.height = appWin.height
            }
        }

        TestCase {
            id: tc
            name: "LensAutoDetect"
            when: appWin.visible

            // Fresh panel per scenario: EXIF + snapshot are staged before the
            // loader reactivates so Component.onCompleted sees final state.
            function reload(exifMake, exifModel, snapshot) {
                lensSession.setExifLens(exifMake, exifModel)
                lensSession.setSnapshot(snapshot || ({}))
                lensSession.clearCalls()
                panelLoader.active = false
                panelLoader.active = true
                const panel = panelLoader.item
                verify(panel, "panel failed to load")
                return panel
            }

            function autoCheck(panel) {
                const box = findChild(panel, "rawLensAutoDetectCheck")
                verify(box, "auto-detect checkbox missing")
                return box
            }

            function picker(panel) {
                const p = findChild(panel, "rawLensCatalogPicker")
                verify(p, "lens catalog picker missing")
                return p
            }

            function lensCalls() {
                var out = []
                for (var i = 0; i < lensSession.calls.length; ++i) {
                    const call = lensSession.calls[i]
                    if (call.fieldKey === "lens_calib")
                        out.push(call)
                }
                return out
            }

            function lastLensParams() {
                const list = lensCalls()
                verify(list.length > 0, "no lens_calib submit recorded")
                return list[list.length - 1].lens
            }

            function test_autoCheckedWhenDetectionSucceeds() {
                const panel = reload(appWin.exifMake, appWin.exifModel, {})
                const box = autoCheck(panel)
                const p = picker(panel)
                verify(p.detectionAvailable)
                compare(p.detectedEntry.brand, appWin.exifMake)
                compare(p.detectedEntry.value, appWin.exifModel)
                verify(box.enabled)
                verify(box.checked)
                compare(lensCalls().length, 0)
            }

            function test_enableSubmitsDetectedPair() {
                const panel = reload(appWin.exifMake, appWin.exifModel, {})
                verify(autoCheck(panel).checked)
                const enabled = findChild(panel, "rawLensEnabledModel")
                verify(enabled)
                enabled.toggle()
                const params = lastLensParams()
                verify(params.enabled === true)
                compare(params.lens_maker, appWin.exifMake)
                compare(params.lens_model, appWin.exifModel)
            }

            function test_detectionFailureDisablesAndUnchecks() {
                const panel = reload("", "", {})
                const box = autoCheck(panel)
                verify(!picker(panel).detectionAvailable)
                verify(!box.enabled)
                verify(!box.checked)
                compare(box.text, "Manual selection required")
            }

            function test_manualPickUnchecksAuto() {
                const panel = reload(appWin.exifMake, appWin.exifModel, {})
                const box = autoCheck(panel)
                const p = picker(panel)
                verify(box.checked)
                p.lensPicked(appWin.manualMake, appWin.manualModel)
                verify(!box.checked)
                const params = lastLensParams()
                compare(params.lens_maker, appWin.manualMake)
                compare(params.lens_model, appWin.manualModel)
            }

            function test_recheckRestoresDetectedAndClearsFilters() {
                const panel = reload(appWin.exifMake, appWin.exifModel, {})
                const box = autoCheck(panel)
                const p = picker(panel)
                p.lensPicked(appWin.manualMake, appWin.manualModel)
                verify(!box.checked)
                p.brandFilter = appWin.manualMake
                p.lensQuery = "nikkor"
                p.brandMenuExpanded = true
                box.toggle()
                verify(box.checked)
                compare(p.brandFilter, "")
                compare(p.lensQuery, "")
                verify(!p.brandMenuExpanded)
                verify(p.expanded)
                compare(p.highlightedIndex,
                        p.indexOfEntry(appWin.exifMake, appWin.exifModel))
                const params = lastLensParams()
                compare(params.lens_maker, appWin.exifMake)
                compare(params.lens_model, appWin.exifModel)
            }

            function test_manualSnapshotPairLoadsUnchecked() {
                const snapshot = {
                    "lens_calib": {
                        "enabled": true,
                        "lens_maker": appWin.manualMake,
                        "lens_model": appWin.manualModel
                    }
                }
                const panel = reload(appWin.exifMake, appWin.exifModel, snapshot)
                const box = autoCheck(panel)
                verify(picker(panel).detectionAvailable)
                verify(!box.checked)
                compare(lensCalls().length, 0)
                const brand = findChild(panel, "rawLensBrandModel")
                const model = findChild(panel, "rawLensModelModel")
                compare(brand.currentValue, appWin.manualMake)
                compare(model.currentValue, appWin.manualModel)
            }

            function test_detectedSnapshotPairLoadsChecked() {
                const snapshot = {
                    "lens_calib": {
                        "enabled": true,
                        "lens_maker": appWin.exifMake,
                        "lens_model": appWin.exifModel
                    }
                }
                const panel = reload(appWin.exifMake, appWin.exifModel, snapshot)
                verify(autoCheck(panel).checked)
                compare(lensCalls().length, 0)
            }

            function test_detectionLossUnchecksBox() {
                const panel = reload(appWin.exifMake, appWin.exifModel, {})
                const box = autoCheck(panel)
                verify(box.checked)
                lensSession.setExifLens("", "")
                verify(!picker(panel).detectionAvailable)
                tryVerify(function () { return !box.checked && !box.enabled }, 1000)
            }
        }
    }
}
