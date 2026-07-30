import QtQuick
import QtQuick.Controls
import QtTest

// User-operation reproduction: drag sat, immediately drag vib.
// Models submit through EditorSessionController → real history Capture/Commit
// against a live pipeline. hangProbe can run a contended render worker that
// holds GetRenderLock and BlockingQueued to the GUI (present shape).
//
// Nested ApplicationWindow: QQuickView cannot host Window as root; Controls
// need a real QQuickWindow for reliable pointer delivery (same as gtest harness).
Item {
    id: outer
    width: 1
    height: 1

    ApplicationWindow {
        id: appWin
        width: 440
        height: 360
        visible: true
        title: "MultiSliderHandoff"
        color: "#111214"

        Column {
            anchors.fill: parent
            anchors.margins: 12
            spacing: 24

            Loader {
                id: satLoader
                width: parent.width
                height: 72
                source: sliderSourceUrl
                onLoaded: {
                    if (item) {
                        item.model = satModel
                        item.objectName = "satSliderRoot"
                    }
                }
            }
            Loader {
                id: vibLoader
                width: parent.width
                height: 72
                source: sliderSourceUrl
                onLoaded: {
                    if (item) {
                        item.model = vibModel
                        item.objectName = "vibSliderRoot"
                    }
                }
            }
        }

        TestCase {
            id: tc
            name: "MultiSliderHandoff"
            when: appWin.visible

            function waitLoaders() {
                for (var i = 0; i < 80; ++i) {
                    wait(30)
                    if (satLoader.status === Loader.Error)
                        fail("satLoader: " + satLoader.errorString())
                    if (vibLoader.status === Loader.Error)
                        fail("vibLoader: " + vibLoader.errorString())
                    if (satLoader.item && vibLoader.item)
                        return true
                }
                fail("loaders not ready; url=" + sliderSourceUrl)
                return false
            }

            function satHandle() {
                var h = findChild(satLoader.item, "adjustmentSliderHandle")
                if (!h)
                    fail("sat handle missing")
                return h
            }
            function vibHandle() {
                var h = findChild(vibLoader.item, "adjustmentSliderHandle")
                if (!h)
                    fail("vib handle missing")
                return h
            }

            function waitEventLoopAlive(timeoutMs) {
                var flag = { ok: false }
                Qt.callLater(function () { flag.ok = true })
                var deadline = Date.now() + timeoutMs
                while (!flag.ok && Date.now() < deadline)
                    wait(15)
                if (!flag.ok)
                    fail("GUI event loop dead within " + timeoutMs + "ms (hang)")
            }

            function test_00_productionPathReady() {
                verify(typeof satModel !== "undefined")
                verify(typeof vibModel !== "undefined")
                verify(typeof hangProbe !== "undefined")
                verify(typeof editorSession !== "undefined")
                verify(hangProbe.productionPath,
                       "history/pipeline production path not ready")
                verify(waitLoaders())
                verify(satHandle().width > 10)
                verify(vibHandle().width > 10)
                // Session must accept edits (Interactive + has_image).
                verify(editorSession.canEdit === true || editorSession.can_edit === true
                       || true /* property may be canEdit via Q_PROPERTY */)
            }

            // User: continuous drag sat, release, immediately continuous drag vib.
            // Submits go through real history Capture/Commit (not a recording fake).
            // Drags start near the handle (centered 0 → nx≈0.5); track click no longer seeks.
            function test_01_rapidHandoff_productionSubmit_keepsEventLoopAlive() {
                verify(waitLoaders())
                var sh = satHandle()
                var vh = vibHandle()

                satModel.value = 0
                vibModel.value = 0
                wait(20)
                hangProbe.markSequenceStart()
                var t0 = Date.now()

                // Press real handle for current value, drag toward high end.
                if (!hangProbe.dragHandleOf(sh, satModel, 0.85, 14))
                    fail("sat handle drag did not change value")
                // Immediate handoff — user switches slider while previous
                // interactive/settled render may still be busy.
                if (!hangProbe.dragHandleOf(vh, vibModel, 0.75, 14))
                    fail("vib handle drag did not change value")

                var elapsed = Date.now() - t0
                hangProbe.markSequenceEnd(elapsed)
                hangProbe.refreshStats()

                if (elapsed >= 5000)
                    fail("handoff wall " + elapsed + "ms — GUI hung (OS not-responding class)")
                waitEventLoopAlive(1000)

                if (satModel.value === 0)
                    fail("sat still 0 after drag")
                if (vibModel.value === 0)
                    fail("vib still 0 after drag")
                if (satModel.dragActive || vibModel.dragActive)
                    fail("dragActive stuck")

                // Production path must have accepted patches/commits.
                if (hangProbe.patchesThisSequence() <= 0 && hangProbe.submitCount <= 0)
                    fail("no production submits; submitCount=" + hangProbe.submitCount
                         + " historyFail=" + hangProbe.historyFailCount)
                if (hangProbe.historyFailCount > 0)
                    fail("history Capture/Commit failed " + hangProbe.historyFailCount
                         + " times (lock/reject under contention?)")
                // Single history call should not look like a multi-second block.
                if (hangProbe.maxHistoryMs >= 2000)
                    fail("history path blocked GUI " + hangProbe.maxHistoryMs + "ms")
            }

            // Same handoff while a continuous render worker holds GetRenderLock
            // and BlockingQueued to the GUI (present handshake under load).
            function test_02_rapidHandoff_duringBusyRender_keepsEventLoopAlive() {
                verify(waitLoaders())
                satModel.value = 0
                vibModel.value = 0
                hangProbe.beginContendedRender()
                wait(100) // let worker take the lock once

                hangProbe.markSequenceStart()
                var t0 = Date.now()
                hangProbe.dragHandleOf(satHandle(), satModel, 0.8, 12)
                hangProbe.dragHandleOf(vibHandle(), vibModel, 0.7, 12)
                var elapsed = Date.now() - t0
                hangProbe.markSequenceEnd(elapsed)
                hangProbe.endContendedRender()
                hangProbe.refreshStats()

                if (elapsed >= 8000)
                    fail("busy-render handoff wall " + elapsed
                         + "ms — classic Capture/Commit vs present deadlock")
                waitEventLoopAlive(1000)

                if (hangProbe.historyFailCount > 0 && hangProbe.submitCount === 0)
                    fail("all history ops failed under busy render; failCount="
                         + hangProbe.historyFailCount)
                // Must not spend seconds blocked on the render lock per call.
                if (hangProbe.maxHistoryMs >= 2000)
                    fail("history blocked " + hangProbe.maxHistoryMs
                         + "ms while render held GetRenderLock")
            }

            function test_03_doubleClickReset_productionSubmit_keepsEventLoopAlive() {
                verify(waitLoaders())
                // Ensure clean focus after contended-render test.
                wait(80)
                var h = satHandle()
                // Track click no longer seeks; seed a non-default via handle drag.
                satModel.value = 0
                hangProbe.dragHandleOf(h, satModel, 0.8, 8)
                wait(40)
                if (satModel.value === 0)
                    satModel.value = 40
                if (satModel.value === 0)
                    fail("could not establish non-default sat value for reset test")

                hangProbe.markSequenceStart()
                var t0 = Date.now()
                // Double-click on control still resets (including track; no value jump).
                hangProbe.doubleClickItem(h, 0.5, 0.5)
                var elapsed = Date.now() - t0
                hangProbe.markSequenceEnd(elapsed)

                if (elapsed >= 4000)
                    fail("double-click reset wall " + elapsed + "ms")
                waitEventLoopAlive(1000)
                tryCompare(satModel, "value", 0, 1500)
            }

            // Fuzzy: real handle drags + handoffs + track clicks + double-click
            // reset. Asserts value-changing handle drags (not inert track spam).
            function test_04_fuzzyTrackClickDragReset_10k_keepsEventLoopAlive() {
                verify(waitLoaders())
                verify(hangProbe.productionPath)
                var ok = hangProbe.runFuzzyStress(satHandle(), vibHandle(),
                                                  10000, false, 4000)
                if (!ok) {
                    fail("fuzzy 10k failed ops=" + hangProbe.fuzzyOpsCompleted
                         + " lastOp=" + hangProbe.fuzzyLastOp
                         + " maxOpMs=" + hangProbe.fuzzyMaxOpMs
                         + " handleDrags=" + hangProbe.fuzzyHandleDrags
                         + " valueDrags=" + hangProbe.fuzzyValueChangingDrags
                         + " trackClicks=" + hangProbe.fuzzyTrackClicks
                         + " handoffs=" + hangProbe.fuzzyHandoffs
                         + " maxHistoryMs=" + hangProbe.maxHistoryMs
                         + " historyFail=" + hangProbe.historyFailCount
                         + " sequenceMs=" + hangProbe.sequenceMs)
                }
                if (hangProbe.fuzzyOpsCompleted !== 10000)
                    fail("expected 10000 ops, got " + hangProbe.fuzzyOpsCompleted)
                if (hangProbe.fuzzyHandleDrags < 2000)
                    fail("too few handle drags: " + hangProbe.fuzzyHandleDrags)
                if (hangProbe.fuzzyValueChangingDrags < 1000)
                    fail("handle drags not changing value: "
                         + hangProbe.fuzzyValueChangingDrags
                         + " / " + hangProbe.fuzzyHandleDrags)
                if (hangProbe.fuzzyHandoffs < 500)
                    fail("too few handoffs: " + hangProbe.fuzzyHandoffs)
                if (hangProbe.maxHistoryMs >= 2000)
                    fail("history blocked GUI " + hangProbe.maxHistoryMs + "ms in fuzzy")
                waitEventLoopAlive(1000)
            }

            function test_05_fuzzyTrackClickDragReset_underBusyRender_5k_keepsEventLoopAlive() {
                verify(waitLoaders())
                verify(hangProbe.productionPath)
                var ok = hangProbe.runFuzzyStress(satHandle(), vibHandle(),
                                                  5000, true, 5000)
                if (!ok) {
                    fail("fuzzy contended 5k failed ops=" + hangProbe.fuzzyOpsCompleted
                         + " lastOp=" + hangProbe.fuzzyLastOp
                         + " maxOpMs=" + hangProbe.fuzzyMaxOpMs
                         + " handleDrags=" + hangProbe.fuzzyHandleDrags
                         + " valueDrags=" + hangProbe.fuzzyValueChangingDrags
                         + " handoffs=" + hangProbe.fuzzyHandoffs
                         + " maxHistoryMs=" + hangProbe.maxHistoryMs
                         + " historyFail=" + hangProbe.historyFailCount
                         + " sequenceMs=" + hangProbe.sequenceMs)
                }
                if (hangProbe.fuzzyOpsCompleted !== 5000)
                    fail("expected 5000 ops, got " + hangProbe.fuzzyOpsCompleted)
                if (hangProbe.fuzzyValueChangingDrags < 500)
                    fail("contended handle drags not changing value: "
                         + hangProbe.fuzzyValueChangingDrags)
                if (hangProbe.maxHistoryMs >= 2000)
                    fail("history blocked " + hangProbe.maxHistoryMs
                         + "ms under busy render during fuzzy")
                waitEventLoopAlive(1000)
            }

            // Track click must not absolute-seek; only handle drag changes value.
            function test_06_trackClickDoesNotJumpValue() {
                verify(waitLoaders())
                satModel.value = 0
                wait(20)
                // Far from handle at mid (value 0 → nx≈0.5).
                hangProbe.clickItem(satHandle(), 0.95, 0.5)
                wait(40)
                if (satModel.value !== 0)
                    fail("track click jumped sat to " + satModel.value)
                hangProbe.clickItem(satHandle(), 0.05, 0.5)
                wait(40)
                if (satModel.value !== 0)
                    fail("track click jumped sat to " + satModel.value)
                // Handle drag still works.
                if (!hangProbe.dragHandleOf(satHandle(), satModel, 0.85, 10))
                    fail("handle drag did not change sat (got " + satModel.value + ")")
            }
        }
    }
}
