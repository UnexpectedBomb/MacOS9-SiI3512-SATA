/* sil_probe.r — resources for the SiI3512 probe app (mirrors the proven nkdump). */
#include "Processes.r"
#include "Types.r"

resource 'SIZE' (-1) {
    reserved, acceptSuspendResumeEvents, reserved, canBackground,
    multiFinderAware, backgroundAndForeground, dontGetFrontClicks,
    ignoreChildDiedEvents, is32BitCompatible, isHighLevelEventAware,
    onlyLocalHLEvents, notStationeryAware, dontUseTextEditServices,
    notDisplayManagerAware, reserved, reserved,
    800 * 1024, 600 * 1024
};

resource 'vers' (1, "SiI3512Probe") {
    0x57, 0x00, development, 0x57, verUS,
    "57.0", "57.0 SiI3512 probe baseline re-test (foreground; current NM-gated driver)"
};
