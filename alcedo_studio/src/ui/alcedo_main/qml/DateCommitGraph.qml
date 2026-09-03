import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// GitHub-style capture heatmap. Week columns (7 weekday rows) wrap to the
// inspector width. The default window is one year ending on the latest dated
// photo in `model` (dateStats {label, count}); calendar years are optional.
// Reads the already-loaded stats list — no extra queries.
Item {
    id: root
    objectName: "dateCommitGraph"

    property var model: []
    property string selectedLabel: ""
    property var folderKey: 0
    property color accentColor: appTheme.toneSteel
    signal dayClicked(string label)

    property string selectedYearKey: "rolling"
    property var countsByDate: ({})
    property string latestDate: ""
    property var availableYears: []
    property var histogramFolderKey

    readonly property color textColor: appTheme.textColor
    readonly property color mutedTextColor: appTheme.textMutedColor
    readonly property int cellMinSize: appTheme.dateGraphCellMinSize
    readonly property int cellGap: appTheme.dateGraphCellGap
    readonly property int cellRadius: appTheme.dateGraphCellRadius
    readonly property int monthLabelHeight: appTheme.lineHeightCaption
    readonly property int wrapRowGap: appTheme.spaceSm
    readonly property var levelColors: [
        appTheme.dateGraphLevel0Color,
        appTheme.dateGraphLevel1Color,
        appTheme.dateGraphLevel2Color,
        appTheme.dateGraphLevel3Color,
        appTheme.dateGraphLevel4Color
    ]

    readonly property int gridInnerWidth: Math.max(cellMinSize, width)
    readonly property int weeksPerRow: {
        const pitch = cellMinSize + cellGap
        return Math.max(1, Math.floor((gridInnerWidth + cellGap) / pitch))
    }
    readonly property int cellSize: {
        const gaps = Math.max(0, weeksPerRow - 1) * cellGap
        return Math.max(cellMinSize, Math.floor((gridInnerWidth - gaps) / weeksPerRow))
    }
    readonly property int weekPitch: cellSize + cellGap
    readonly property int weekColumnHeight: 7 * cellSize + 6 * cellGap
    readonly property int wrapRowHeight: monthLabelHeight + weekColumnHeight + wrapRowGap

    readonly property var yearOptions: {
        const opts = [{ value: "rolling", label: qsTr("Last year") }]
        const years = availableYears
        for (let i = 0; i < years.length; ++i)
            opts.push({ value: years[i], label: String(years[i]) })
        return opts
    }

    readonly property var dateWindow: {
        const end = parseIso(latestDate)
        if (!end)
            return { startLabel: "", endLabel: "", dayCount: 0 }
        let start = null
        if (selectedYearKey !== "rolling") {
            const year = Number(selectedYearKey)
            if (year >= 1000) {
                start = new Date(year, 0, 1)
                end.setFullYear(year, 11, 31)
            }
        }
        if (!start)
            start = rollingStart(end)
        let n = 0
        const cursor = new Date(start.getFullYear(), start.getMonth(), start.getDate())
        const last = new Date(end.getFullYear(), end.getMonth(), end.getDate())
        while (cursor <= last) {
            ++n
            cursor.setDate(cursor.getDate() + 1)
        }
        return {
            startLabel: formatIso(start),
            endLabel: formatIso(end),
            dayCount: n
        }
    }

    readonly property int dayCount: dateWindow.dayCount
    readonly property string startDate: dateWindow.startLabel
    readonly property string endDate: dateWindow.endLabel
    readonly property int histogramSize: Object.keys(countsByDate).length

    readonly property var weeks: buildWeeks()
    readonly property var wrapRows: {
        const all = weeks
        const per = weeksPerRow
        const rows = []
        for (let i = 0; i < all.length; i += per)
            rows.push(all.slice(i, i + per))
        return rows
    }

    property int hoverWeekIndex: -1
    property int hoverDayIndex: -1
    property string hoveredLabel: ""
    property int hoveredCount: 0
    property real hoverMouseX: 0
    property real hoverMouseY: 0
    property var lastCellInfo: ({ found: false, level: -1, count: 0, x: 0, y: 0 })
    property string pendingFilterLabel: ""

    implicitHeight: col.implicitHeight
    implicitWidth: 200
    width: parent ? parent.width : implicitWidth
    Accessible.role: Accessible.Grouping
    Accessible.name: qsTr("Capture activity")

    QtObject {
        id: yearComboModel
        readonly property string label: ""
        readonly property bool enabled: true
        readonly property var entries: root.yearOptions
        readonly property int currentIndex: {
            const opts = root.yearOptions
            for (let i = 0; i < opts.length; ++i) {
                if (String(opts[i].value) === root.selectedYearKey)
                    return i
            }
            return 0
        }
        function selectIndex(index) {
            const opts = root.yearOptions
            if (index >= 0 && index < opts.length)
                root.selectYear(String(opts[index].value))
        }
    }

    onModelChanged: ingestHistogram()
    onSelectedLabelChanged: {
        if (selectedLabel.length === 0)
            ingestHistogram()
    }
    onFolderKeyChanged: ingestHistogram()
    Component.onCompleted: ingestHistogram()

    function isIsoDate(label) {
        return typeof label === "string" && label.length === 10
                && label.charCodeAt(4) === 45 && label.charCodeAt(7) === 45
    }

    function parseIso(label) {
        if (!isIsoDate(label))
            return null
        const year = Number(label.slice(0, 4))
        const month = Number(label.slice(5, 7))
        const day = Number(label.slice(8, 10))
        if (!year || month < 1 || month > 12 || day < 1)
            return null
        return new Date(year, month - 1, day)
    }

    function pad2(value) {
        return value < 10 ? "0" + value : String(value)
    }

    function formatIso(date) {
        return date.getFullYear() + "-" + pad2(date.getMonth() + 1) + "-" + pad2(date.getDate())
    }

    function rollingStart(end) {
        const next = new Date(end.getFullYear(), end.getMonth(), end.getDate() + 1)
        next.setFullYear(next.getFullYear() - 1)
        return next
    }

    function levelFor(count, maxCount) {
        if (count <= 0 || maxCount <= 0)
            return 0
        if (maxCount === 1)
            return 4
        const ratio = count / maxCount
        if (ratio > 0.75)
            return 4
        if (ratio > 0.50)
            return 3
        if (ratio > 0.25)
            return 2
        return 1
    }

    function readIncomingHistogram() {
        const next = {}
        const yearSet = {}
        let latest = ""
        const rows = model || []
        for (let i = 0; i < rows.length; ++i) {
            const label = String(rows[i].label)
            if (!isIsoDate(label))
                continue
            next[label] = Number(rows[i].count)
            if (label > latest)
                latest = label
            yearSet[label.slice(0, 4)] = true
        }
        const years = Object.keys(yearSet)
        years.sort()
        years.reverse()
        return { counts: next, latest: latest, years: years }
    }

    function incomingIsDateFilterEcho(incoming) {
        if (latestDate.length === 0)
            return false
        const keys = Object.keys(incoming)
        const cachedKeys = Object.keys(countsByDate)
        if (keys.length === 0 || keys.length >= cachedKeys.length)
            return false
        for (let i = 0; i < keys.length; ++i) {
            const label = keys[i]
            if (!Object.prototype.hasOwnProperty.call(countsByDate, label))
                return false
            if (Number(incoming[label]) !== Number(countsByDate[label]))
                return false
        }
        return true
    }

    function applyHistogram(parsed) {
        countsByDate = parsed.counts
        latestDate = parsed.latest
        availableYears = parsed.years
        histogramFolderKey = folderKey
        if (selectedYearKey !== "rolling" && parsed.years.indexOf(selectedYearKey) < 0)
            selectedYearKey = "rolling"
    }

    function ingestHistogram() {
        const parsed = readIncomingHistogram()
        const folderChanged = String(folderKey) !== String(histogramFolderKey)
        if (folderChanged || latestDate.length === 0) {
            pendingFilterLabel = ""
            applyHistogram(parsed)
            return
        }

        if (pendingFilterLabel.length > 0) {
            pendingFilterLabel = ""
            return
        }

        if (selectedLabel.length > 0 || incomingIsDateFilterEcho(parsed.counts))
            return

        applyHistogram(parsed)
    }

    function requestDayFilter(label) {
        if (label.length === 0)
            return
        pendingFilterLabel = label
        dayClicked(label)
    }

    function selectYear(key) {
        if (key === "rolling") {
            selectedYearKey = "rolling"
            return
        }
        if (availableYears.indexOf(key) >= 0)
            selectedYearKey = key
    }

    function cellInfo(label) {
        const all = weeks
        const per = weeksPerRow
        for (let w = 0; w < all.length; ++w) {
            const days = all[w].days
            for (let d = 0; d < days.length; ++d) {
                const cell = days[d]
                if (!cell || !cell.inWindow || cell.label !== label)
                    continue
                const row = Math.floor(w / per)
                const col = w % per
                return {
                    found: true,
                    level: cell.level,
                    count: cell.count,
                    x: col * weekPitch + cellSize * 0.5,
                    y: row * wrapRowHeight + monthLabelHeight + d * weekPitch + cellSize * 0.5
                }
            }
        }
        return { found: false, level: -1, count: 0, x: 0, y: 0 }
    }

    function inspectCell(label) {
        lastCellInfo = cellInfo(label)
    }

    function emptyPadCell() {
        return { label: "", count: 0, level: 0, inWindow: false, month: -1, day: 0 }
    }

    function buildWeeks() {
        const win = dateWindow
        const start = parseIso(win.startLabel)
        if (!start)
            return []

        const counts = countsByDate
        const days = []
        let maxCount = 0
        const cursor = new Date(start.getFullYear(), start.getMonth(), start.getDate())
        for (let i = 0; i < win.dayCount; ++i) {
            const label = formatIso(cursor)
            const count = Number(counts[label] || 0)
            if (count > maxCount)
                maxCount = count
            days.push({
                label: label,
                count: count,
                level: 0,
                inWindow: true,
                month: cursor.getMonth(),
                day: cursor.getDate()
            })
            cursor.setDate(cursor.getDate() + 1)
        }
        for (let i = 0; i < days.length; ++i)
            days[i].level = levelFor(days[i].count, maxCount)

        const loc = Qt.locale()
        const firstDow = loc.firstDayOfWeek === 7 ? 0 : loc.firstDayOfWeek
        const lead = (start.getDay() - firstDow + 7) % 7
        const cells = []
        for (let i = 0; i < lead; ++i)
            cells.push(emptyPadCell())
        for (let i = 0; i < days.length; ++i)
            cells.push(days[i])
        while (cells.length % 7 !== 0)
            cells.push(emptyPadCell())

        const out = []
        for (let i = 0; i < cells.length; i += 7) {
            const slice = cells.slice(i, i + 7)
            let month = -1
            let showMonth = false
            for (let d = 0; d < 7; ++d) {
                if (slice[d].inWindow) {
                    month = slice[d].month
                    break
                }
            }
            if (month >= 0) {
                if (i === 0) {
                    showMonth = true
                } else {
                    const prev = cells[i - 1]
                    showMonth = !prev.inWindow || prev.month !== month
                }
            }
            out.push({
                days: slice,
                showMonth: showMonth,
                monthLabel: month >= 0
                            ? loc.standaloneMonthName(month, Locale.ShortFormat)
                            : ""
            })
        }
        return out
    }

    function monthSpan(rowWeeks, index) {
        if (!rowWeeks || index < 0 || index >= rowWeeks.length)
            return 0
        const week = rowWeeks[index]
        if (!week || (index > 0 && !week.showMonth))
            return 0
        let span = 1
        for (let i = index + 1; i < rowWeeks.length; ++i) {
            if (rowWeeks[i].showMonth)
                break
            ++span
        }
        return span
    }

    function cellAt(px, py) {
        if (px < 0 || wrapRows.length === 0)
            return null
        const row = Math.floor(py / wrapRowHeight)
        if (row < 0 || row >= wrapRows.length)
            return null
        const localY = py - row * wrapRowHeight - monthLabelHeight
        if (localY < 0)
            return null
        const day = Math.floor(localY / weekPitch)
        if (day < 0 || day > 6)
            return null
        const col = Math.floor(px / weekPitch)
        const rowWeeks = wrapRows[row]
        if (col < 0 || col >= rowWeeks.length)
            return null
        const cell = rowWeeks[col].days[day]
        if (!cell || !cell.inWindow)
            return null
        return { cell: cell, weekIndex: row * weeksPerRow + col, dayIndex: day }
    }

    function hoverText() {
        if (hoveredLabel.length === 0)
            return ""
        const date = parseIso(hoveredLabel)
        const dateText = date ? Qt.formatDate(date, Locale.LongFormat) : hoveredLabel
        if (hoveredCount <= 0)
            return qsTr("No photos on %1").arg(dateText)
        if (hoveredCount === 1)
            return qsTr("%1 photo on %2").arg(hoveredCount).arg(dateText)
        return qsTr("%1 photos on %2").arg(hoveredCount).arg(dateText)
    }

    ColumnLayout {
        id: col
        anchors.left: parent.left
        anchors.right: parent.right
        spacing: appTheme.spaceSm

        AdjustmentCombo {
            id: yearCombo
            objectName: "dateCommitGraphYearRow"
            visible: latestDate.length > 0
            Layout.fillWidth: true
            showResetButton: false
            controlObjectName: "dateCommitGraphYearCombo"
            model: yearComboModel
        }

        Label {
            visible: latestDate.length > 0
            Layout.fillWidth: true
            text: root.startDate + " – " + root.endDate
            color: root.mutedTextColor
            font.family: appTheme.dataFontFamily
            font.pixelSize: appTheme.fontSizeCaption
            font.weight: appTheme.fontWeightRegular
        }

        Item {
            id: graphGrid
            objectName: "dateCommitGraphGrid"
            visible: root.weeks.length > 0
            Layout.fillWidth: true
            implicitHeight: Math.max(0, root.wrapRows.length * root.wrapRowHeight
                                     - (root.wrapRows.length > 0 ? root.wrapRowGap : 0))

            Repeater {
                model: root.wrapRows.length
                delegate: Item {
                    id: wrapRow
                    required property int index
                    readonly property var rowWeeks: root.wrapRows[index]
                    width: graphGrid.width
                    height: root.wrapRowHeight - root.wrapRowGap
                    y: index * root.wrapRowHeight

                    Repeater {
                        model: wrapRow.rowWeeks.length
                        delegate: Label {
                            required property int index
                            readonly property var week: wrapRow.rowWeeks[index]
                            readonly property bool showLabel: index === 0
                                    || (week && week.showMonth)
                            readonly property int span: showLabel
                                    ? root.monthSpan(wrapRow.rowWeeks, index) : 0
                            x: index * root.weekPitch
                            width: Math.max(root.cellSize, span * root.weekPitch - root.cellGap)
                            height: root.monthLabelHeight
                            visible: showLabel && week && week.monthLabel.length > 0
                            text: week ? week.monthLabel : ""
                            color: root.mutedTextColor
                            font.family: appTheme.uiFontFamily
                            font.pixelSize: appTheme.fontSizeCaption
                            font.weight: appTheme.fontWeightRegular
                            elide: Text.ElideRight
                        }
                    }

                    Repeater {
                        model: wrapRow.rowWeeks.length
                        delegate: Item {
                            id: weekCol
                            required property int index
                            readonly property var week: wrapRow.rowWeeks[index]
                            readonly property int weekIndex: wrapRow.index * root.weeksPerRow + index
                            x: index * root.weekPitch
                            y: root.monthLabelHeight
                            width: root.cellSize
                            height: root.weekColumnHeight

                            Repeater {
                                model: 7
                                delegate: Rectangle {
                                    required property int index
                                    readonly property var cell: weekCol.week
                                                                ? weekCol.week.days[index] : null
                                    readonly property bool inWindow: cell ? cell.inWindow : false
                                    readonly property bool isSelected: inWindow
                                            && cell.label === root.selectedLabel
                                            && root.selectedLabel.length > 0
                                    y: index * root.weekPitch
                                    width: root.cellSize
                                    height: root.cellSize
                                    radius: root.cellRadius
                                    objectName: inWindow && cell
                                                ? "dateCommitGraphCell_" + cell.label : ""
                                    color: cell ? (root.levelColors[cell.level] || root.levelColors[0])
                                                : "transparent"
                                    border.width: isSelected ? 1 : 0
                                    border.color: root.accentColor
                                }
                            }
                        }
                    }
                }
            }

            Rectangle {
                visible: root.hoverWeekIndex >= 0
                x: (root.hoverWeekIndex % root.weeksPerRow) * root.weekPitch
                y: Math.floor(root.hoverWeekIndex / root.weeksPerRow) * root.wrapRowHeight
                   + root.monthLabelHeight + root.hoverDayIndex * root.weekPitch
                width: root.cellSize
                height: root.cellSize
                radius: root.cellRadius
                color: "transparent"
                border.width: 1
                border.color: root.textColor
            }

            MouseArea {
                anchors.fill: parent
                hoverEnabled: true
                acceptedButtons: Qt.LeftButton | Qt.RightButton
                cursorShape: Qt.PointingHandCursor
                onPositionChanged: function(mouse) {
                    root.hoverMouseX = mouse.x
                    root.hoverMouseY = mouse.y
                    const hit = root.cellAt(mouse.x, mouse.y)
                    if (!hit) {
                        root.hoverWeekIndex = -1
                        root.hoverDayIndex = -1
                        root.hoveredLabel = ""
                        root.hoveredCount = 0
                        return
                    }
                    root.hoverWeekIndex = hit.weekIndex
                    root.hoverDayIndex = hit.dayIndex
                    root.hoveredLabel = hit.cell.label
                    root.hoveredCount = hit.cell.count
                }
                onExited: {
                    root.hoverWeekIndex = -1
                    root.hoverDayIndex = -1
                    root.hoveredLabel = ""
                    root.hoveredCount = 0
                }
                onClicked: function(mouse) {
                    if (mouse.button === Qt.RightButton) {
                        if (root.selectedLabel.length > 0)
                            root.requestDayFilter(root.selectedLabel)
                        return
                    }
                    const hit = root.cellAt(mouse.x, mouse.y)
                    if (hit && hit.cell.count > 0)
                        root.requestDayFilter(hit.cell.label)
                }
            }

            ToolTip {
                id: cellTip
                parent: Overlay.overlay
                visible: root.hoveredLabel.length > 0 && Overlay.overlay !== null
                text: root.hoverText()
                delay: 160
                timeout: -1
                x: {
                    const layer = Overlay.overlay
                    if (!layer)
                        return 0
                    const pad = appTheme.spaceSm
                    const mapped = graphGrid.mapToItem(layer, root.hoverMouseX + pad, 0)
                    return Math.min(mapped.x, Math.max(0, layer.width - implicitWidth))
                }
                y: {
                    const layer = Overlay.overlay
                    if (!layer)
                        return 0
                    const pad = appTheme.spaceSm
                    const below = graphGrid.mapToItem(layer, 0, root.hoverMouseY + pad).y
                    if (below + implicitHeight <= layer.height)
                        return below
                    return Math.max(0, graphGrid.mapToItem(layer, 0, root.hoverMouseY).y
                                    - implicitHeight - pad)
                }
            }
        }

        RowLayout {
            id: legend
            objectName: "dateCommitGraphLegend"
            visible: root.weeks.length > 0
            Layout.fillWidth: true
            spacing: appTheme.spaceXs

            Label {
                text: qsTr("Less")
                color: root.mutedTextColor
                font.family: appTheme.uiFontFamily
                font.pixelSize: appTheme.fontSizeCaption
            }

            Repeater {
                model: 5
                delegate: Rectangle {
                    required property int index
                    Layout.preferredWidth: root.cellMinSize
                    Layout.preferredHeight: root.cellMinSize
                    radius: root.cellRadius
                    color: root.levelColors[index]
                }
            }

            Label {
                text: qsTr("More")
                color: root.mutedTextColor
                font.family: appTheme.uiFontFamily
                font.pixelSize: appTheme.fontSizeCaption
            }

            Item { Layout.fillWidth: true }
        }

        Label {
            visible: latestDate.length === 0
            Layout.fillWidth: true
            Layout.topMargin: appTheme.spaceXs
            horizontalAlignment: Text.AlignHCenter
            text: qsTr("No capture dates")
            color: root.mutedTextColor
            font.family: appTheme.uiFontFamily
            font.pixelSize: appTheme.fontSizeCaption
            font.italic: true
        }
    }
}
