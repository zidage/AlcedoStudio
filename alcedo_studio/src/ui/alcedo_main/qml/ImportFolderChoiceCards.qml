//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

import QtQuick
import QtQuick.Layouts

// The two choices of Import Folder side by side, with radio behavior: the Settings "Default
// Behavior" page (#1077) and the Import Folder dialog (#1078). The owner holds the selection;
// the cards report the choice the user activates.
RowLayout {
    id: cards

    /// "import", "mount", or "" for no selected card (Settings: Ask every time).
    property string selectedAction: ""
    /// Label of the collection box in the Import diagram; empty shows the word "Collection".
    property string collectionName: ""
    property real drawingHeight: 40
    /// False disables the Mount card: a drop of several items or of files has no one folder to
    /// mount (#1078).
    property bool mountEnabled: true
    /// Prefix of the objectName of each card: <prefix>Import and <prefix>Mount.
    property string cardObjectNamePrefix: "importFolderChoice"

    signal actionSelected(string action)

    spacing: appTheme.spaceSm

    Accessible.role: Accessible.Grouping

    ImportFolderChoiceCard {
        objectName: cards.cardObjectNamePrefix + "Import"
        Layout.fillWidth: true
        Layout.fillHeight: true
        Layout.preferredWidth: 1
        action: "import"
        selected: cards.selectedAction === "import"
        collectionName: cards.collectionName
        drawingHeight: cards.drawingHeight
        onActivated: cards.actionSelected("import")
    }

    ImportFolderChoiceCard {
        objectName: cards.cardObjectNamePrefix + "Mount"
        Layout.fillWidth: true
        Layout.fillHeight: true
        Layout.preferredWidth: 1
        action: "mount"
        enabled: cards.mountEnabled
        selected: cards.selectedAction === "mount"
        collectionName: cards.collectionName
        drawingHeight: cards.drawingHeight
        onActivated: cards.actionSelected("mount")
    }
}
