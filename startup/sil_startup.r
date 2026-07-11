/* sil_startup.r — resources for the faceless SiI3512 Startup-Items auto-mount app.
 * onlyBackground => "have no user interface" (faceless, per Processes.r); the app
 * auto-runs from the System Folder's "Startup Items" with no window and no app-menu
 * presence. Mirrors the probe's SIZE, swapping backgroundAndForeground -> onlyBackground. */
#include "Processes.r"
#include "Types.r"

resource 'SIZE' (-1) {
    reserved, acceptSuspendResumeEvents, reserved, canBackground,
    multiFinderAware, onlyBackground, dontGetFrontClicks,
    ignoreChildDiedEvents, is32BitCompatible, isHighLevelEventAware,
    onlyLocalHLEvents, notStationeryAware, dontUseTextEditServices,
    notDisplayManagerAware, reserved, reserved,
    800 * 1024, 600 * 1024
};

resource 'vers' (1, "SiI3512Startup") {
    0x01, 0x00, development, 0x00, verUS,
    "1.0", "1.0 SiI3512 faceless Startup-Items auto-mount (settle 20s)"
};
