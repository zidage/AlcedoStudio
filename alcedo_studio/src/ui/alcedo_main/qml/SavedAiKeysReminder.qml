import QtQuick

// Launch reminder of issue #710: Advanced Content Analysis is removed, but the OS credential
// store can still hold API keys or a Codex login that the user saved for it. When the main
// window is ready and a stored provider profile has a saved credential, the shell shows one
// snackbar that points to Settings > Saved AI Keys. The reminder shows at most once per launch,
// and it stops after the user deletes every saved credential.
QtObject {
    id: reminder

    property var profileController: null       // appModules.aiProviderProfiles
    property var host: null                    // Main: showSnackbar(text)
    property bool shown: false

    function showIfSavedKeys() {
        if (reminder.shown || !reminder.host || !reminder.profileController
                || !reminder.profileController.hasSavedCredentials) {
            return
        }
        reminder.shown = true
        reminder.host.showSnackbar(qsTr("Alcedo Studio still holds AI keys that you saved for Advanced Content Analysis, which this version removes. Delete them in Settings > Saved AI Keys."))
    }
}
