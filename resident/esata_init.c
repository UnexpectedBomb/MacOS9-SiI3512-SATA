/*
 * esata_init.c — M6.2a 68K INIT for the SiI3512 eSATA driver. Loads the resident-driver PEF
 * ('PPC ' 128), finds its PPC InstallMe entry, and calls it SYNCHRONOUSLY via Mixed Mode.
 * InstallMe (PPC) then does the InstallDriverFromMemory that makes the SiI3512 driver resident
 * in the Device Manager Unit Table (DriverLoaderLib is PPC-only, so a 68K INIT can't call it
 * directly — hence the PPC helper). Cloned from usb2-ehci/resident/ehci_initdrv.c (R2b-3 proven).
 * MUST use NewRoutineDescriptorTrap (plain NewRoutineDescriptor is a 68K no-op passthrough that
 * returns the PPC TVector unchanged -> JSR'ing PPC data as 68K -> boot crash).
 * Ship caveat: Audio CD Access must be disabled. See [[reference_os9_init_resident_driver]].
 */
#include <OSUtils.h>
#include <MacMemory.h>
#include <Resources.h>
#include <Gestalt.h>
#include <CodeFragments.h>
#include <MixedMode.h>
#include <Sound.h>
#include <Files.h>
#include "Retro68Runtime.h"

enum { uppInstallMeProcInfo = kCStackBased };   /* void InstallMe(void) */

static short gLog = 0;
static void L(const char *s)
{
    long n = 0, z = 1;
    if (!gLog) { FSSpec sp; (void)FSMakeFSSpec(0, 0, "\pSiI3512 INIT Log", &sp); (void)FSpDelete(&sp);
        if (FSpCreate(&sp, 'ttxt', 'TEXT', 0) == noErr) (void)FSpOpenDF(&sp, fsRdWrPerm, &gLog); }
    if (gLog) { while (s[n]) n++; (void)FSWrite(gLog, &n, (Ptr)s);
        (void)FSWrite(gLog, &z, (Ptr)"\r"); (void)FlushVol(0, 0); }
}
static void Lx(const char *label, unsigned long v)
{
    char b[96]; int i = 0, j; static const char hx[] = "0123456789abcdef";
    while (label[i] && i < 72) { b[i] = label[i]; i++; }
    b[i++] = ' '; b[i++] = '0'; b[i++] = 'x';
    for (j = 28; j >= 0; j -= 4) b[i++] = hx[(v >> j) & 0xF];
    b[i] = 0; L(b);
}

void _start(void)
{
    long cfmAttr = 0;
    Handle h = NULL;
    CFragConnectionID connID = (CFragConnectionID)0;
    Ptr mainAddr; Str255 errName;
    Ptr symAddr; CFragSymbolClass symClass;
    UniversalProcPtr upp;
    OSErr err;
    Boolean haveConn = false;

    RETRO68_RELOCATE();
    Retro68CallConstructors();

    SysBeep(30);                                  /* independent "INIT loaded" signal */
    L("=== SiI3512Init (M6.2a): 68K INIT _start ran ===");

    if (Gestalt(gestaltCFMAttr, &cfmAttr) != noErr) { L("Gestalt(CFM) FAILED"); goto done; }
    if (!(cfmAttr & (1L << gestaltCFMPresent)))      { L("CFM NOT present"); goto done; }

    h = Get1Resource('PPC ', 128);
    if (h == NULL) { L("Get1Resource('PPC ',128) == NULL"); goto done; }
    HLock(h);

    /* transient load just to call InstallMe; the RESIDENT copy is made by InstallDriverFromMemory
     * inside InstallMe, owned by the Device Manager -> this connection can be dropped afterward. */
    err = GetMemFragment(*h, GetHandleSize(h), "\pSiI3512ResidentLoader", kPrivateCFragCopy,
                         &connID, &mainAddr, errName);
    Lx("GetMemFragment err (0=ok)", (unsigned long)(long)err);
    if (err != noErr) goto done;
    haveConn = true;

    err = FindSymbol(connID, "\pInstallMe", &symAddr, &symClass);
    Lx("FindSymbol(InstallMe) err (0=ok)", (unsigned long)(long)err);
    if (err != noErr) goto done;

    upp = NewRoutineDescriptorTrap((ProcPtr)symAddr, uppInstallMeProcInfo, kPowerPCISA);
    Lx("InstallMe descriptor (!= symAddr)", (unsigned long)upp);
    if (upp == NULL) goto done;

    L(">>> calling InstallMe (installs resident SiI3512 driver) <<<");
    (*(void (*)(void))upp)();
    L("<<< InstallMe returned; resident driver should be installed; boot continues >>>");
    DisposeRoutineDescriptorTrap(upp);

done:
    if (haveConn) (void)CloseConnection(&connID);   /* drop the transient loader connection */
    if (h) HUnlock(h);
    Retro68FreeGlobals();
}
