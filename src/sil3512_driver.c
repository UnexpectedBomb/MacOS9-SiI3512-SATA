/*
 * sil3512_driver.c — Mac OS 9 native PCI driver ('ndrv') for the
 * Silicon Image SiI3512 SATA controller (LaCie 130823 eSATA PCI card).
 *
 * MILESTONE 1 (this file): a shape-correct native device driver container.
 * It exports the two symbols the OS 9 native-driver model requires of a
 * standalone PCI device driver:
 *
 *   TheDriverDescription — CFM driver descriptor. Its nameInfoStr is matched
 *                          by the Name Registry against the card's PCI node
 *                          "pci1095,3512" (vendor 0x1095 Silicon Image,
 *                          device 0x3512). This is what routes THIS card to
 *                          THIS driver and sidesteps SeriTek's 0x3112-only
 *                          device-ID lockout (see ../FEASIBILITY.md).
 *   DoDriverIO           — the unified native-driver entry point. The I/O
 *                          system dispatches Initialize/Open/Close/Read/Write/
 *                          Control/Status/Finalize here.
 *
 * Architecture (see ../FEASIBILITY.md): on Open we register a SCSI Manager 4.3
 * HBA/SIM so each SATA port appears as a SCSI target; a SCSI-to-ATA translation
 * layer (SATL, milestone 3) turns SCSI CDBs into SiI3512 taskfile/BMDMA ops.
 * All operations are stubs in M1 — this milestone proves container shape and
 * Name-Registry matching only. Controller bring-up = M2, command engine = M3.
 *
 * Build pipeline cloned from the proven ../usb2-ehci NDRV flow
 * (SHARED lib -> MakePEF -> Rez wrap as file type 'ndrv').
 */

typedef signed char    SInt8;
typedef unsigned char  UInt8;
typedef short          SInt16;
typedef unsigned short UInt16;
typedef long           SInt32;
typedef unsigned long  UInt32;
typedef long           OSStatus;
typedef unsigned long  OSType;

#define FOURCC(a,b,c,d) (((UInt32)(a)<<24)|((UInt32)(b)<<16)|((UInt32)(c)<<8)|(UInt32)(d))
#define noErr             0L
#define paramErr        (-50L)
#define kSil3512Unimpl  (-6650L)   /* placeholder until the op is implemented */

/* IOCommandKind bits (Devices.h). The Device Manager tags each command as
 * synchronous, asynchronous, or immediate. Immediate commands complete via the
 * DoDriverIO return value; sync/async commands complete only when the driver
 * calls IOCommandIsComplete(cmdID, result) — see the epilogue in DoDriverIO. */
#define kSynchronousIOCommandKind   0x00000001UL
#define kAsynchronousIOCommandKind  0x00000002UL
#define kImmediateIOCommandKind     0x00000004UL

/* --- byte-accurate DriverDescription (matches <DriverFamilyMatching.h>) --- */
typedef struct { UInt8 len; char s[31]; } Str31;
typedef struct { UInt8 majorRev, minorAndBugRev, stage, nonRelRev; } NumVersion;
typedef struct { OSType category; OSType type; NumVersion version; } SilServiceInfo;

typedef struct {
    OSType     sig;             /* 'mtej' kTheDescriptionSignature            */
    UInt32     descVersion;     /* 0 kInitialDriverDescriptor                 */
    Str31      nameInfoStr;     /* "pci1095,3512"  <- match THIS card         */
    NumVersion typeVersion;     /* driver version                             */
    UInt32     driverRuntime;   /* runtime flags                              */
    Str31      driverName;      /* "SiI3512SATA"                              */
    UInt32     reserved[8];
    UInt32     nServices;       /* >=1 REQUIRED ("at least one service")       */
    SilServiceInfo service0;    /* the one service (ndrv / block-storage)      */
} SilDriverDescription;

/*
 * A native driver's description MUST declare at least one service (per
 * DriverFamilyMatching.h: "The List of Services (at least one)") — omitting it
 * makes VerifyFragmentAsDriver reject the fragment. We declare a generic native
 * driver ('ndrv') of block-storage type ('blok'). driverRuntime keeps
 * kDriverIsLoadedUponDiscovery|kDriverIsOpenedUponLoad (0x03).
 */
SilDriverDescription TheDriverDescription = {
    FOURCC('m','t','e','j'),
    0,
    { 12, "pci1095,3512" },
    { 1, 0, 0x80 /*final*/, 0 },
    0x00000003UL,
    { 11, "SiI3512SATA" },
    { 0,0,0,0,0,0,0,0 },
    1,
    { FOURCC('n','d','r','v'), FOURCC('b','l','o','k'), { 1, 0, 0x80, 0 } }
};

/* ------------------------------------------------------------------------- *
 * Native-driver entry point.
 *
 * The real prototype is
 *   OSStatus DoDriverIO(AddressSpaceID spaceID, IOCommandID cmdID,
 *                       IOCommandContents contents, IOCommandCode code,
 *                       IOCommandKind kind);
 * We use opaque typedefs in M1 to keep the scaffold header-light; M2 pulls in
 * <DriverServices.h>/<NameRegistry.h>/<PCI.h> for the real types (and the PCI
 * enable + BAR5 mapping, cloned from ../usb2-ehci/M2-GLUE-SPEC.md).
 * ------------------------------------------------------------------------- */
typedef void      *AddressSpaceID;
typedef void      *IOCommandID;
typedef void      *IOCommandContents;   /* holds the initInfo / IOPB / etc.   */
typedef UInt32     IOCommandCode;
typedef UInt32     IOCommandKind;

/* IOCommandCode values — MUST match <Devices.h> exactly (getting these wrong
 * misroutes every command; e.g. Initialize=7 was landing on our Read handler). */
enum {
    kOpenCommand       = 0,
    kCloseCommand      = 1,
    kReadCommand       = 2,
    kWriteCommand      = 3,
    kControlCommand    = 4,
    kStatusCommand     = 5,
    kKillIOCommand     = 6,
    kInitializeCommand = 7,   /* init driver + device: PCI enable, map BAR5, reset */
    kFinalizeCommand   = 8,
    kReplaceCommand    = 9,
    kSupersededCommand = 10
};

/*
 * The command handlers live in sil3512_os.c (the OS-header file). For
 * kInitializeCommand the I/O system passes a DriverInitInfo (which carries the
 * card's Name Registry node) via contents.initialInfo; we forward the pointer.
 * Read/Write/Control/Status become SCSI-Manager-driven IOPBs once the HBA is
 * registered (M3); until then they decline.
 */
extern OSStatus sil_os_initialize(void *initInfo);
extern OSStatus sil_os_open(void);
extern OSStatus sil_os_close(void);
extern OSStatus sil_os_finalize(void);
extern OSStatus sil_disk_read(void *pb);
extern OSStatus sil_disk_write(void *pb);
extern OSStatus sil_disk_status(void *pb);
extern OSStatus sil_disk_control(void *pb);
extern void     sil_disk_logcmd(unsigned long code, unsigned long kind);  /* v31 diag */

/* DriverServicesLib. Real prototype: OSErr IOCommandIsComplete(IOCommandID,
 * OSErr). OSErr is 16-bit and every result we return fits (noErr..-6650);
 * IOCommandID is a pointer, so our opaque void* typedef is ABI-compatible. */
extern short IOCommandIsComplete(IOCommandID theID, short theResult);

/*
 * Native-driver dispatch + completion contract.
 *
 * Every command runs to a result `err`, then:
 *   - IMMEDIATE commands complete by RETURNING err from DoDriverIO.
 *   - SYNCHRONOUS/ASYNCHRONOUS commands complete ONLY when the driver calls
 *     IOCommandIsComplete(cmdID, err); the return value is ignored for them.
 * The Device Manager always delivers kInitialize/kOpen/kClose/kFinalize/
 * kReplace/kSuperseded as IMMEDIATE (so the epilogue leaves the validated
 * install path unchanged), but File Manager block reads (PBMountVol) arrive as
 * synchronous kRead commands. v22/v23 executed the read correctly ("io OK") yet
 * PBMountVol hung because we returned without ever calling IOCommandIsComplete,
 * so its ioResult never cleared. This epilogue is the fix.
 */
OSStatus DoDriverIO(AddressSpaceID spaceID, IOCommandID cmdID,
                    IOCommandContents contents, IOCommandCode code,
                    IOCommandKind kind)
{
    OSStatus err;
    (void)spaceID;
    sil_disk_logcmd((unsigned long)code, (unsigned long)kind);   /* v31: trace every command */
    switch (code) {
        case kInitializeCommand: err = sil_os_initialize((void *)contents); break;
        /* Disk-driver model: Open just succeeds (drives are AddDrive'd in
         * Initialize, M6.1b); we no longer register a SCSI bus here. Accept the
         * Control/Status queries the Device Manager issues during open/install
         * so the install completes cleanly (was returning kSil3512Unimpl=-6650). */
        case kOpenCommand:       err = noErr; break;
        case kCloseCommand:      err = noErr; break;
        case kFinalizeCommand:   err = sil_os_finalize(); break;
        case kControlCommand:    err = sil_disk_control((void *)contents); break;
        case kStatusCommand:     err = sil_disk_status((void *)contents); break;
        case kReadCommand:       err = sil_disk_read((void *)contents); break;
        case kWriteCommand:      err = sil_disk_write((void *)contents); break;
        case kSupersededCommand:
        case kReplaceCommand:
        case kKillIOCommand:     err = noErr; break;
        default:                 err = paramErr; break;
    }

    if (kind & kImmediateIOCommandKind)
        return err;                                  /* immediate: return completes it */
    return (OSStatus)IOCommandIsComplete(cmdID, (short)err);  /* sync/async: signal FM */
}
