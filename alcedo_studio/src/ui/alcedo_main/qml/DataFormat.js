.pragma library

// Text of counts and remaining times in data displays (Settings > Local Content Recognition, the
// AI label dialog, Settings > LUTs). Data text is set in Manrope with tabular figures
// (DESIGN.md, "Data text").

// The count with the digit grouping of the locale: 102,345.
function grouped(value, locale) {
    return Math.max(0, Math.floor(Number(value) || 0)).toLocaleString(locale, "f", 0)
}

// The value with `decimals` decimals, truncated (99,999 reads 99.9K, not 100.0K), without
// trailing zeros.
function _truncated(value, decimals, locale) {
    const factor = Math.pow(10, decimals)
    let text = (Math.floor(value * factor) / factor).toLocaleString(locale, "f", decimals)
    if (decimals > 0) {
        while (text.endsWith("0"))
            text = text.slice(0, -1)
        if (text.endsWith(locale.decimalPoint))
            text = text.slice(0, -locale.decimalPoint.length)
    }
    return text
}

// A large count in at most three significant digits for a display number: below 10,000 the
// grouped count; above it K and M (12.8K, 102K, 1.25M), or 万 and 亿 when the UI language is
// Chinese (1.28万, 10.2万). The exact count stays next to it (CountTile).
function compact(value, languageCode, locale) {
    const count = Math.max(0, Math.floor(Number(value) || 0))
    if (count < 10000)
        return grouped(count, locale)
    const units = String(languageCode).startsWith("zh")
                  ? [[100000000, "亿"], [10000, "万"]]
                  : [[1000000000, "B"], [1000000, "M"], [1000, "K"]]
    for (let index = 0; index < units.length; ++index) {
        const size = units[index][0]
        if (count >= size) {
            const scaled = count / size
            const decimals = scaled >= 100 ? 0 : (scaled >= 10 ? 1 : 2)
            return _truncated(scaled, decimals, locale) + units[index][1]
        }
    }
    return grouped(count, locale)
}

// The remaining time of a running task from the seconds of its estimate (-1: no estimate yet).
function remaining(seconds) {
    if (seconds < 0)
        return qsTr("Estimating the remaining time")
    if (seconds < 60)
        return qsTr("Less than a minute remaining")
    const minutes = Math.round(seconds / 60)
    if (minutes < 60)
        return qsTr("About %1 min remaining").arg(minutes)
    const hours = Math.floor(minutes / 60)
    const rest = minutes % 60
    if (rest === 0)
        return qsTr("About %1 h remaining").arg(hours)
    return qsTr("About %1 h %2 min remaining").arg(hours).arg(rest)
}
