#include "CodeFragments.r"
/* Native driver code fragment. Fragment name must match the exported driver;
 * data-fork PEF, whole-fork, PowerPC — same recipe as ../usb2-ehci. */
resource 'cfrg' (0) {
    {
        kPowerPCCFragArch, kIsCompleteCFrag, kNoVersionNum, kNoVersionNum,
        kDefaultStackSize, kNoAppSubFolder,
        kImportLibraryCFrag, kDataForkCFragLocator, kZeroOffset, kCFragGoesToEOF,
        "SiI3512SATA"
    }
};
