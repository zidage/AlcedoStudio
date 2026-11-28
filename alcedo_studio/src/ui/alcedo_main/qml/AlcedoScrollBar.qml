import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Basic as Basic

// Shared vertical scroll bar: the bar of the Inspector pages (the Basic style bar of their
// ScrollView). A 6 px round thumb that shows while the view moves, or while the bar is hovered
// or dragged, and fades out after. The Basic style is named here, so a file that imports the
// Material controls still gets this bar (#1028). Give the bar its own column, or a z above an
// input layer that covers the view, so it takes its own drags.
Basic.ScrollBar {
    policy: ScrollBar.AsNeeded
}
