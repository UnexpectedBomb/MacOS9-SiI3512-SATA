/*
 * sil3512_disk.c — block-storage / drive-queue layer for the resident driver
 * (M6.1b). Runs INSIDE the installed native driver (not the app).
 *
 *   sil_disk_scan_and_add(refNum) — called from kInitialize after bring-up:
 *       read the Apple Partition Map, and for each Apple_HFS partition AddDrive
 *       a drive-queue entry bound to this driver (drive# -> port,partStart).
 *   sil_disk_read/sil_disk_write(pb) — kRead/kWrite: partition-offset block I/O
 *       via the validated ATA DMA engine.
 *
 * Writes a flushed "SiI3512 Driver Log" (System Folder) so the partition scan +
 * AddDrive are visible even though this code runs during InstallDriverFromMemory.
 */
#include <MacTypes.h>
#include <Devices.h>
#include <Disks.h>
#include <Files.h>
#include <Folders.h>
#include <MacMemory.h>
#include <NameRegistry.h>
#include <DriverServices.h>
#include <DriverGestalt.h>  /* kDriverGestaltCode (csCode 43) */
#include <Notification.h>  /* M6.2b: deferred auto-mount NM */
#include <Gestalt.h>        /* NewGestaltValue — publish the probe ring (v44) */
#include <Errors.h>         /* controlErr */
#include <TextUtils.h>      /* NumToString */
#include <Script.h>         /* smSystemScript */
#include "sil3512.h"

/* v57 (2026-07-09): the mount vehicle is now the faceless Startup-Items app
 * (probe/../startup/sil_startup_app.c), which owns PBMountVol post-settle. The
 * driver's own boot-time NM auto-mount (M6.2b/c) is compiled OUT here: it fired too
 * early (the desktop-load window that knocks out the card's PCI decode — see m62c),
 * AND its sil_hc_init COMRESET would reset the SATA link AFTER the app already
 * mounted. Flip to 1 to restore the self-mounting INIT vehicle for the v2.0 "true
 * extension". */
#ifndef SIL_DRIVER_SELF_MOUNTS
#define SIL_DRIVER_SELF_MOUNTS 0
#endif

/* v39 FAT PIVOT: the DIZero proof-cap (SIL_PROOF_MAX_BLOCKS) is gone — we no longer
 * format through the driver. The SSD is FAT32/MBR-formatted externally and we present
 * the FULL FAT partition (dQDrvSz low word + dQDrvSz2 high word carry the whole
 * 32-bit block count). */

/* ---- driver-side flushed log ---- */
static short gDLog = 0, gDVol = 0;
static int   gLogQuiet = 0;   /* v41: set at the END of kInitialize. Once set, DWrite is a
   * no-op, so kRead/kWrite/kControl/kStatus do ZERO File Manager calls during the mount
   * (no FSWrite/FlushVol re-entry) — matching the proven USB2 FAT-mount driver. */
static void DLogOpen(void)
{
    long d; FSSpec sp;
    if (FindFolder(kOnSystemDisk, kSystemFolderType, kDontCreateFolder, &gDVol, &d) != noErr) return;
    if (FSMakeFSSpec(gDVol, d, "\pSiI3512 Driver Log", &sp) == noErr) FSpDelete(&sp);
    if (FSpCreate(&sp, 'ttxt', 'TEXT', smSystemScript) != noErr) return;
    if (FSpOpenDF(&sp, fsRdWrPerm, &gDLog) != noErr) gDLog = 0;
}
static void DWrite(const unsigned char *pstr)
{
    long n; char cr = '\r';
    if (!gDLog || gLogQuiet) return;
    n = pstr[0]; FSWrite(gDLog, &n, (Ptr)&pstr[1]); n = 1; FSWrite(gDLog, &n, &cr);
    FlushVol(NULL, gDVol);
}
static void PApp(Str255 d, const char *s) { short l=d[0]; while(*s&&l<255)d[++l]=(unsigned char)*s++; d[0]=(unsigned char)l; }
static void PAppN(Str255 d, long v) { Str255 n; short i; NumToString(v,n); for(i=1;i<=n[0]&&d[0]<255;i++)d[++d[0]]=n[i]; }

/* ---- v58 DIAGNOSTIC: probe the BAR's CPU page mapping via GetPageInformation --------------
 * The exhaustive ROM RE concluded the mount-read bus-error is our unclaimed card's BAR logical
 * page being unmapped by the OS (the Uni-N config path stays fine, hence "config alive / decode
 * dead"). GetPageInformation QUERIES the page tables and never faults, so this is crash-safe.
 * We log it for the kInitialize scan reads (mapping present = baseline) AND the File-Manager
 * mount reads; if the mount read shows the page GONE while the scan reads showed it PRESENT,
 * the teardown is confirmed. GetPageInformation is exported by DriverServicesLib but absent
 * from Retro68's headers, so it is hand-declared per Universal Interfaces 3.4.1 (all args are
 * 4-byte => built-in types match the CFM ABI; kCurrentAddressSpaceID = -1, version = 1). */
/* v62: this whole page/SCR-link DIAGNOSTIC (v58-v61) is gated OFF for the PRODUCTION driver
 * that the ROM-claim candidate ships. Production disk_rw does the REAL mount read (MMIO);
 * only the diagnostic build (-DSIL_DIAG_PAGEPROBE=1) skips the mount MMIO and probes instead. */
#ifndef SIL_DIAG_PAGEPROBE
#define SIL_DIAG_PAGEPROBE 0
#endif
#if SIL_DIAG_PAGEPROBE
static void PAppHex(Str255 d, unsigned long v){ static const char h[]="0123456789ABCDEF"; int i;
    for(i=28;i>=0;i-=4) if(d[0]<255) d[++d[0]]=(unsigned char)h[(v>>i)&0xF]; }
static short gPLog=0, gPVol=0;
static void PLog(Str255 s){ long n, one=1, d; char cr='\r'; FSSpec sp;
    if(!gPLog){ if(FindFolder(kOnSystemDisk,kSystemFolderType,kDontCreateFolder,&gPVol,&d)!=noErr)return;
        if(FSMakeFSSpec(gPVol,d,"\pSiI3512 PageInfo Log",&sp)==noErr)FSpDelete(&sp);
        if(FSpCreate(&sp,'ttxt','TEXT',smSystemScript)!=noErr)return;
        if(FSpOpenDF(&sp,fsRdWrPerm,&gPLog)!=noErr){gPLog=0;return;} }
    if(gPLog){ n=s[0]; FSWrite(gPLog,&n,(Ptr)&s[1]); FSWrite(gPLog,&one,(Ptr)&cr); FlushVol(NULL,gPVol);} }
/* v59/v60: page-probe + SATA-link stash. SAME principle as the v44 Control/Status ring below —
 * pure memory + config-cycle + SAFE-SCR reads only, so it is safe to record from INSIDE the
 * PBMountVol read. The v58 freeze was sil_diag_page calling PLog->FSWrite at mount time,
 * re-entering the File Manager during PBMountVol = the v47 re-entrant-FM hang (DWrite honors
 * gLogQuiet; PLog did not). The verdict is stashed here and the app dumps it via Gestalt('SiPg')
 * afterward, at task level. Field order MUST match the app's PgRec.
 * v60: v59 proved the BAR page is MAPPED at mount (the fault is NOT a page teardown), so we now
 * also capture the SAFE SCR link registers (bar5+0x100.., proven to respond at mount) + a
 * pure-config snapshot, to reveal WHETHER the SATA link is up at the mount read. We NEVER touch
 * the taskfile (bar5+0x80..0x9F) — that is the region that bus-errors. */
typedef struct { UInt32 magic, seq, phase, bar, st, cnt, fl, present, lba,
                 sstatus, serror, scontrol, cfgCmd, cfgBar, pmcsr; } SilPageRec;
static SilPageRec gPageRec;
/* Query bar5's page mapping + the SAFE SCR link state. Returns 1 if the OS reports the page
 * present. Always stashes to gPageRec (memory, FM-safe); logs to the flushed 'SiI3512 PageInfo
 * Log' ONLY for scan reads (task level) — NEVER at mount (would FSWrite-re-enter the File
 * Manager). GetPageInformation + PageInformation come from DriverServices.h. */
static int sil_diag_page(int mountPhase, int port, UInt32 lba){
    UInt8 pibuf[64]; PageInformation *pi = (PageInformation *)pibuf;
    OSStatus st; int present; long j; Str255 L; UInt32 cnt, flags0, ss;
    for(j=0;j<(long)sizeof(pibuf);j++) pibuf[j]=0;
    st = GetPageInformation(kCurrentAddressSpaceID, (ConstLogicalAddress)gSil.bar5, 0x88UL,
                            kPageInformationVersion, pi);
    cnt = (UInt32)pi->count; flags0 = (UInt32)pi->information[0];
    present = (st==0 && cnt>=1);
    /* SAFE captures only: SCR link regs (0x100 region) + pure-config snapshot. NO taskfile. */
    ss = sil_scr_read(&gSil, port, SIL_SCR_STATUS);
    sil_pci_snapshot();
    gPageRec.seq++;      gPageRec.phase   = (UInt32)mountPhase;
    gPageRec.bar = (UInt32)gSil.bar5;  gPageRec.st = (UInt32)st;
    gPageRec.cnt = cnt;  gPageRec.fl = flags0;
    gPageRec.present = (UInt32)present; gPageRec.lba = lba;
    gPageRec.sstatus  = ss;
    gPageRec.serror   = sil_scr_read(&gSil, port, SIL_SCR_ERROR);
    gPageRec.scontrol = sil_scr_read(&gSil, port, SIL_SCR_CONTROL);
    gPageRec.cfgCmd   = gSil.snapCmd;
    gPageRec.cfgBar   = gSil.snapBar;
    gPageRec.pmcsr    = gSil.snapPmcsr;
    if (!mountPhase) {   /* scan read: task level => FSWrite is safe. mount read: stash only. */
        L[0]=0; PApp(L,"scan  bar="); PAppHex(L,(unsigned long)gSil.bar5);
        PApp(L," st=");  PAppHex(L,(unsigned long)st);
        PApp(L," cnt="); PAppHex(L,(unsigned long)cnt);
        PApp(L," ss=");  PAppHex(L,(unsigned long)ss);
        PApp(L," lba="); PAppN(L,(long)lba);
        PApp(L, present ? " PRESENT" : " *UNMAPPED*");
        PLog(L);
    }
    return present;
}
#endif /* SIL_DIAG_PAGEPROBE */
static void DS(const char *s) { Str255 L; L[0]=0; PApp(L,s); DWrite(L); }
static void DN(const char *s, long v) { Str255 L; L[0]=0; PApp(L,s); PAppN(L,v); DWrite(L); }
/* hex + ascii dump of n bytes (n<=64) after a label — for on-disk signatures */
static void DHex(const char *label, const UInt8 *b, int n)
{
    static const char hx[] = "0123456789ABCDEF";
    Str255 L; int i; L[0]=0; PApp(L,label);
    for (i=0;i<n && L[0]<248;i++){ L[++L[0]]=' '; L[++L[0]]=hx[(b[i]>>4)&0xF]; L[++L[0]]=hx[b[i]&0xF]; }
    if(L[0]<253){ L[++L[0]]=' '; L[++L[0]]='|'; }
    for (i=0;i<n && L[0]<254;i++){ char c=(char)b[i]; L[++L[0]]=(unsigned char)((c>=32&&c<127)?c:'.'); }
    if(L[0]<255) L[++L[0]]='|';
    DWrite(L);
}

#if SIL_DRIVER_SELF_MOUNTS
/* ---- M6.2b: deferred auto-mount via the Notification Manager (no app) --------------- *
 * kInitialize AddDrive's our partition, but M6.2a proved the OS won't mount it that early:
 * the INIT parade runs before the File Manager is fully up at top level. So we arm a
 * one-shot NM here; it fires post-boot in the Finder's context (top level, File Manager
 * ready) and PBMountVol's our drive(s). No app. [[reference_os9_init_resident_driver]]
 * (Harmless in the probe/app path too: the app mounts immediately, then this NM fires and
 * PBMountVol on the already-mounted volume just returns an error.) */
static NMRec  gMountNM;
static short  gMountNMArmed = 0;
static short  gMountRefNum  = 0;

/* Separate flushed log ("SiI3512 Mount Log"), independent of the quiet driver log, written
 * only BEFORE and AFTER PBMountVol (never re-entrantly during a mount read). */
static short  gMLog = 0, gMVol = 0;
static void MWrite(const char *s)
{
    long n = 0, one = 1, d; char cr = '\r'; FSSpec sp;
    if (!gMLog) {
        if (FindFolder(kOnSystemDisk, kSystemFolderType, kDontCreateFolder, &gMVol, &d) != noErr) return;
        if (FSMakeFSSpec(gMVol, d, "\pSiI3512 Mount Log", &sp) == noErr) FSpDelete(&sp);
        if (FSpCreate(&sp, 'ttxt', 'TEXT', smSystemScript) != noErr) return;
        if (FSpOpenDF(&sp, fsRdWrPerm, &gMLog) != noErr) { gMLog = 0; return; }
    }
    if (gMLog) { while (s[n]) n++; (void)FSWrite(gMLog, &n, (Ptr)s);
        (void)FSWrite(gMLog, &one, (Ptr)&cr); (void)FlushVol(NULL, gMVol); }
}
static void MWriteX(const char *label, long v)
{
    char b[96]; int i = 0, j; static const char hx[] = "0123456789ABCDEF";
    while (label[i] && i < 72) { b[i] = label[i]; i++; }
    b[i++] = ' '; b[i++] = '0'; b[i++] = 'x';
    for (j = 28; j >= 0; j -= 4) b[i++] = hx[((unsigned long)v >> j) & 0xF];
    b[i] = 0; MWrite(b);
}

static pascal void MountNMResp(NMRecPtr nmReqPtr)
{
    QHdrPtr q; DrvQElPtr el; int mounted = 0;
    UInt32 findings;
    (void)nmReqPtr;
    MWrite("=== SiI3512 mount NM fired (post-boot, top level): mounting our drive(s) ===");

    /* v57/M6.2c: the INIT vehicle brings the card up at INIT time, but its memory decode is
     * DEAD by the time this post-boot NM reads it. ACA is now removed, so this is NOT the old
     * Audio-CD-Access D3 — it is the OS's own post-INIT-parade PCI/power/ATA finalization
     * disrupting our too-early INIT install. Two moves, in order:
     *   (1) DIAGNOSE to a flushed file — snapshot the card's PCI config with pure config
     *       cycles (which reach it even with memory decode dead), so the teardown state is
     *       captured on disk even if the mount read below bus-errors (crash-proof, unlike the
     *       MacsBug-only snap@ block). This is logged BEFORE any MMIO.
     *   (2) RECOVER — one full re-assert (wake D0 + restore decode/BAR + RE-MAP the logical
     *       BAR + re-init) before PBMountVol touches MMIO. The re-map is the new step the
     *       per-read sil_pci_reassert never did. */
    sil_pci_snapshot();                      /* pure config-space, no MMIO */
    MWriteX("  post-boot snapCmd  (cfg 0x04; want bits 1+2 set)", (long)gSil.snapCmd);
    MWriteX("  post-boot snapBar  (cfg 0x24; want == bar5cfg)",   (long)gSil.snapBar);
    MWriteX("  post-boot snapPmcsr (low 2 bits: 0=D0 .. 3=D3)",   (long)gSil.snapPmcsr);
    MWriteX("  bar5 logical (as mapped at INIT)",                 (long)(UInt32)gSil.bar5);
    MWriteX("  bar5cfg (INIT config 0x24)",                       (long)gSil.bar5cfg);
    MWriteX("  pmCap offset",                                     (long)gSil.pmCap);
    MWrite("  --> full recovery (wake/decode/BAR/re-map/re-init); MMIO begins now");
    findings = sil_remount_recover();
    MWriteX("  findings (1=D3 2=decodeOff 4=BARmoved 8=remapChanged)", (long)findings);
    MWriteX("  bar5 logical (after re-map)",                      (long)(UInt32)gSil.bar5);
    MWrite("  recovery survived MMIO; proceeding to PBMountVol");

    q = GetDrvQHdr();
    for (el = q ? (DrvQElPtr)q->qHead : 0; el; el = (DrvQElPtr)el->qLink) {
        if (el->dQRefNum == gMountRefNum) {
            ParamBlockRec pb; UInt8 *p = (UInt8 *)&pb; long j; OSErr me;
            for (j = 0; j < (long)sizeof(pb); j++) p[j] = 0;
            pb.volumeParam.ioVRefNum = el->dQDrive;
            me = PBMountVol((ParmBlkPtr)&pb);
            MWriteX("  drive#", (long)el->dQDrive);
            MWriteX("    PBMountVol result (0=ok, -55=already mounted)", (long)me);
            if (me == noErr) mounted++;
        }
    }
    MWriteX("=== mount NM done; volumes newly mounted=", (long)mounted);
    if (gMountNMArmed) { (void)NMRemove(&gMountNM); gMountNMArmed = 0; }
}

static void arm_mount_nm(short refNum)
{
    UInt8 *p = (UInt8 *)&gMountNM; long j;
    if (gMountNMArmed) return;
    gMountRefNum = refNum;
    for (j = 0; j < (long)sizeof(gMountNM); j++) p[j] = 0;
    gMountNM.qType  = 8;                        /* nmType */
    gMountNM.nmResp = NewNMUPP(MountNMResp);    /* mark/sound/str/icon/refCon left 0/NULL */
    if (gMountNM.nmResp && NMInstall(&gMountNM) == noErr) {
        gMountNMArmed = 1; DN("M6.2b: mount NM armed (fires post-boot) refNum=", (long)refNum);
    } else DS("M6.2b: NMInstall/NewNMUPP FAILED");
}
#endif /* SIL_DRIVER_SELF_MOUNTS */

/* ---- v44: Control/Status probe ring. Pure memory writes only, so it is SAFE to
 * record from inside a File Manager mount/file-op (unlike DWrite's FSWrite). Published
 * via Gestalt('Si3L'); the probe app dumps it after the mount to reveal the EXACT
 * csCodes the foreign-FS plugins issue — this is how the sibling USB2 project found the
 * CD-ROM Controls (104/125) behind the "audio CD" misID. ---- */
#define SIL_CS_CAP 64                       /* power of two */
typedef struct { short kind, csCode, ioVRefNum, pad; long p0, p1; } SilCsRec;   /* 16 B */
typedef struct { UInt32 magic, count, cap; SilCsRec recs[SIL_CS_CAP]; } SilCsLog;
static SilCsLog gCsLog;
static void cslog(short kind, ParmBlkPtr pb)   /* kind: 1 = Status, 2 = Control */
{
    SilCsRec *r = &gCsLog.recs[gCsLog.count & (SIL_CS_CAP - 1)];
    r->kind = kind; r->csCode = pb->cntrlParam.csCode; r->ioVRefNum = pb->cntrlParam.ioVRefNum; r->pad = 0;
    r->p0 = ((long)(unsigned short)pb->cntrlParam.csParam[0] << 16) | (unsigned short)pb->cntrlParam.csParam[1];
    r->p1 = ((long)(unsigned short)pb->cntrlParam.csParam[2] << 16) | (unsigned short)pb->cntrlParam.csParam[3];
    gCsLog.count++;
}

/* v67: kRead-path stage trace. Memory-only (pushes into the Si3L ring the app already dumps),
 * so it is hang-safe at mount time where DWrite/FSWrite is suppressed by gLogQuiet. gIoStage
 * records how far disk_rw got; iolog() records one entry per disk_io call as csCode = 700+stage
 * (709 = success), p0 = lba — so a failing File-Manager mount read shows WHERE disk_rw returned 0
 * (703 = sil_dma_prepare failed, 704 = sil_ata_rw_dma failed). */
static short gIoStage = 0;
static void iolog(UInt32 lba, short code)
{
    SilCsRec *r = &gCsLog.recs[gCsLog.count & (SIL_CS_CAP - 1)];
    r->kind = 2; r->csCode = code; r->ioVRefNum = gIoStage; r->pad = 0;
    r->p0 = (long)lba; r->p1 = 0;
    gCsLog.count++;
}

/* ---- drive table (drive# -> port, partition start) ---- */
typedef struct { UInt8 inUse, port; short driveNum; UInt32 partStart, partCount; } sil_drive;
static sil_drive gDrives[SIL3512_N_PORTS * 2];
static int gNDrives = 0;
static UInt32 gWrLogCtr = 0;   /* v28: throttle bulk-write logging (DIZero) */

static sil_drive *find_drive(short driveNum)
{
    int i; for (i = 0; i < gNDrives; i++)
        if (gDrives[i].inUse && gDrives[i].driveNum == driveNum) return &gDrives[i];
    return 0;
}

/* pick a drive number not already in the drive queue (scan for max, +1). */
static short pick_drive_num(void)
{
    QHdrPtr q = GetDrvQHdr();
    DrvQElPtr el = q ? (DrvQElPtr)q->qHead : 0;
    short mx = 4;                       /* keep clear of floppy numbers 1..4 */
    while (el) { if (el->dQDrive > mx) mx = el->dQDrive; el = (DrvQElPtr)el->qLink; }
    return (short)(mx + 1);
}

/* v42: DMA staging (bounce) buffer for READS. The mount freeze survives every
 * driver-behavior change (format, 'minf', re-entrant logging), so the remaining
 * difference from the PROVEN USB2 FAT-mount path is the ONE thing only this driver
 * does: bus-master DMA DIRECTLY into the File Manager's mount buffer (LockMemory +
 * GetPhysical + DMA + unwire on memory the FM is actively using mid-mount). Here we
 * DMA into our OWN static buffer and CPU-copy into the FM buffer, so the FM buffer
 * is only ever touched by a plain memory store (like USB2's staged transfer). If
 * this clears the freeze, direct-DMA-into-the-FM-buffer was the cause. */
static UInt8 gBounce[32UL * 512];       /* 16 KB DMA staging buffer (reads AND writes) */

/* chunked partition-offset block transfer via the validated ATA engine.
 * v43: BOTH directions stage through gBounce, so the File Manager's buffer is NEVER
 * wired/GetPhysical'd/DMA'd — it is only ever touched by a CPU BlockMoveData. v42
 * proved this for READS (it's what let the volume mount); writes still DMA'd directly
 * and empty-trash then crashed, so writes are now staged symmetrically. The driver is
 * synchronous/polled (one command at a time), so sharing gBounce across R/W is safe. */
static int disk_rw(int port, UInt32 lba, UInt32 nblocks, UInt8 *buf, int isWrite)
{
    UInt32 prdCap = sil_dma_max_sectors();
    UInt32 cmdCap = gSil.port[port].lba48 ? 65536UL : 256UL;
    UInt32 chunk  = (prdCap < cmdCap) ? prdCap : cmdCap;
    UInt32 bchunk = sizeof(gBounce) / 512UL;
    gIoStage = 1;                /* v67: entered disk_rw */
#if SIL_DIAG_PAGEPROBE
    /* DIAGNOSTIC build ONLY: probe the BAR mapping + SCR link, then at mount time (gLogQuiet==1)
     * skip ALL MMIO so we never bus-error (the mount read gets a graceful ioErr); scan reads
     * still do the real transfer below. PRODUCTION (SIL_DIAG_PAGEPROBE=0) does the REAL mount
     * read — which is the whole point of the ROM-claim candidate. */
    (void)sil_diag_page(gLogQuiet, port, lba);
    if (gLogQuiet) return 0;
#endif
    (void)sil_pci_reassert();   /* v55: wake to D0 + (on the wake transition) re-init the controller before MMIO.
                                 * ROOT CAUSE (v54): Audio CD Access power-downs our card to D3 (pmCap@0x60),
                                 * which kills decode (v51-v53 bus-errored) AND resets the SATA controller.
                                 * v54's D0 wake stopped the crash but reads came back garbage (badMDBErr -60).
                                 * v55 re-establishes the link (sil_hc_init) after the wake so reads work. */
    gIoStage = 2;                /* v67: passed sil_pci_reassert */
    if (chunk > bchunk) chunk = bchunk;
    while (nblocks > 0) {
        UInt32 n = (nblocks < chunk) ? nblocks : chunk;
        UInt32 bytes = n * 512UL, prd; void *ck;
        if (isWrite) BlockMoveData(buf, gBounce, (long)bytes);       /* FM buffer -> staging */
        gIoStage = 3;            /* v67: about to sil_dma_prepare */
        if (!sil_dma_prepare(gBounce, bytes, isWrite, &prd, &ck)) return 0;
        gIoStage = 4;            /* v67: about to sil_ata_rw_dma */
        if (!sil_ata_rw_dma(&gSil, port, lba, (UInt16)n, prd, isWrite)) { sil_dma_complete(ck, bytes); return 0; }
        sil_dma_complete(ck, bytes);
        if (!isWrite) BlockMoveData(gBounce, buf, (long)bytes);      /* staging -> FM buffer */
        lba += n; buf += bytes; nblocks -= n;
    }
    gIoStage = 9;                /* v67: completed OK */
    return 1;
}

/* ---- kInitialize: scan the APM and AddDrive each Apple_HFS partition (v50 HFS RETRY) ----
 * Native HFS path, restored now that the DMA-into-FM-buffer mount freeze is FIXED (the bounce, v42).
 * We pivoted to FAT (v39) to escape that freeze before it was root-caused, then hit an unreliable PC
 * Exchange / audio-CD-race wall (v43-v49). HFS uses OS 9's BUILT-IN mounter (NOT Foreign File Access)
 * => no audio-CD race, no PC-Exchange fragility, native read/write. Scan the Apple Partition Map (blk0
 * 'ER' DDR; 'PM' entries; type "Apple_HFS" covers HFS + HFS+) and AddDrive the HFS partition; PBMountVol
 * then reaches the built-in mounter via the bounced read path. SSD must be Mac-formatted (Mac OS
 * Extended, Apple Partition Map scheme). */
OSStatus sil_disk_scan_and_add(short refNum)
{
    int port; static UInt8 blk[512];

    DLogOpen();
    /* v44: pure-memory Control/Status probe ring, published via Gestalt for the app to dump. */
    gCsLog.magic = 0x5369334cUL /* 'Si3L' */; gCsLog.count = 0; gCsLog.cap = SIL_CS_CAP;
    (void)NewGestaltValue('Si3L', (long)&gCsLog);
#if SIL_DIAG_PAGEPROBE
    gPageRec.magic = 0x53695067UL /* 'SiPg' */; gPageRec.seq = 0;   /* mount-verdict stash */
    (void)NewGestaltValue('SiPg', (long)&gPageRec);
    DN("=== SiI3512 driver v61 DIAGNOSTIC: page/SCR-link probe (verdict -> Gestalt 'SiPg'), refNum=", refNum);
#else
    DN("=== SiI3512 driver v69 ROM-CLAIM (bring-up in kOpen; node SunrichSATA3512, expert-control 0x05, REAL mount I/O), refNum=", refNum);
#endif
    DHex("  bar5(logical)", (const UInt8 *)&gSil.bar5, 4);
    DHex("  bar5cfg(0x24) ", (const UInt8 *)&gSil.bar5cfg, 4);
    DHex("  pmCap(cfg off)", (const UInt8 *)&gSil.pmCap, 1);
    { const void *sp = (const void *)&gSil.snapSeq;   /* v54: dm THIS addr in MacsBug at the crash */
      DHex("  snap@ (dm this)", (const UInt8 *)&sp, 4); }
    for (port = 0; port < SIL3512_N_PORTS; port++) {
        UInt32 mapCnt, e;
        if (!gSil.port[port].present) continue;
        DN("port present: ", port);
        if (!disk_rw(port, 0, 1, blk, 0)) { DS("  block0 read FAILED"); continue; }
        if (!(blk[0] == 0x45 && blk[1] == 0x52)) { DS("  no 'ER' DDR - not APM, skip"); DHex("  blk0", blk, 8); continue; }
        if (!disk_rw(port, 1, 1, blk, 0) || !(blk[0]==0x50 && blk[1]==0x4D)) { DS("  no 'PM' map - skip"); continue; }
        mapCnt = ((UInt32)blk[4]<<24)|((UInt32)blk[5]<<16)|((UInt32)blk[6]<<8)|blk[7];
        if (mapCnt > 63) mapCnt = 63;
        for (e = 1; e <= mapCnt; e++) {
            UInt32 pstart, pcount;
            if (!disk_rw(port, e, 1, blk, 0) || blk[0]!=0x50 || blk[1]!=0x4D) break;
            /* partition type string @+48; match "Apple_HFS" (covers HFS + HFS+) */
            if (!(blk[48]=='A'&&blk[49]=='p'&&blk[50]=='p'&&blk[51]=='l'&&blk[52]=='e'&&
                  blk[53]=='_'&&blk[54]=='H'&&blk[55]=='F'&&blk[56]=='S')) continue;
            pstart = ((UInt32)blk[8]<<24)|((UInt32)blk[9]<<16)|((UInt32)blk[10]<<8)|blk[11];
            pcount = ((UInt32)blk[12]<<24)|((UInt32)blk[13]<<16)|((UInt32)blk[14]<<8)|blk[15];
            if (gNDrives < (int)(sizeof(gDrives)/sizeof(gDrives[0]))) {
                DrvQElPtr dq; DrvSts *ds; Ptr raw; short dnum = pick_drive_num();
                /* DrvSts status prefix valid (diskInPlace=8/installed=1) — hard-won v31/v32. */
                raw = NewPtrSysClear(sizeof(DrvSts) + 8);
                if (raw == NULL) { DS("  NewPtr failed"); break; }
                ds = (DrvSts *)raw;
                ds->track       = 0;
                ds->writeProt   = gSilWriteEnabled ? 0 : (char)0x80;
                ds->diskInPlace = 8;
                ds->installed   = 1;
                ds->sides       = 0;
                dq = (DrvQElPtr)&ds->qLink;
                dq->qType = 1;
                dq->dQDrvSz  = (unsigned short)(pcount & 0xFFFF);
                dq->dQDrvSz2 = (unsigned short)(pcount >> 16);
                AddDrive(refNum, dnum, dq);
                gDrives[gNDrives].inUse = 1; gDrives[gNDrives].port = (UInt8)port;
                gDrives[gNDrives].driveNum = dnum; gDrives[gNDrives].partStart = pstart;
                gDrives[gNDrives].partCount = pcount; gNDrives++;
                DN("  AddDrive Apple_HFS drive#=", dnum); DN("    partStart=", (long)pstart); DN("    partCount=", (long)pcount);
            }
        }
    }
    DN("disk init done, drives added=", gNDrives);

    /* v50 diagnostic (crash-free, pre-mount): dump the HFS Master Directory Block (partition block 2)
     * of the first added partition, to confirm it's an HFS/HFS+ volume that reads clean through our
     * bounced path before the built-in mounter runs. 'BD'=HFS, 'H+'=HFS+; drEmbedSigWord@124='H+'
     * indicates an HFS-wrapped HFS+ volume. */
    if (gNDrives > 0) {
        int p = gDrives[0].port; UInt32 ps = gDrives[0].partStart;
        DS("--- HFS MDB (partition block 2) ---");
        if (disk_rw(p, ps + 2, 1, blk, 0)) {
            DHex("MDB@0   ", blk+0,   16);
            DHex("MDB@120 ", blk+120, 16);
        } else DS("  MDB read FAIL");
    }

#if SIL_DRIVER_SELF_MOUNTS
    /* M6.2b: arm the deferred auto-mount NM (fires post-boot at top level, File Manager
     * ready; no app). M6.2a proved AddDrive works at boot but the OS won't mount that early. */
    if (gNDrives > 0) arm_mount_nm(refNum);
#else
    /* v57: the faceless Startup-Items app owns the mount (post-settle, real-process
     * context). The driver only brings up + AddDrive here. */
#endif

    /* v41: keep the mount QUIET — re-entrant FSWrite during PBMountVol hangs the File Manager (v47).
     * All logging above ran at task level (kInitialize), before this. */
    gLogQuiet = 1;

    return noErr;
}

/* ---- kRead / kWrite: partition-offset block I/O ----
 * ONE consolidated log line per op. Every DWrite is an FSWrite+FlushVol, i.e. a
 * File Manager re-entry from inside the File Manager's own mount read — unsafe in
 * principle (v22-v24 survived it, but it's a latent corruptor). v24 logged 6
 * lines per read (~36 re-entries per mount); folding to 1 line cuts that ~6x.
 * Logged AFTER the transfer so the line carries the result and marks the last
 * op that fully completed (a crash inside disk_rw shows as a missing next line). */
static OSStatus disk_io(void *pbv, int isWrite)
{
    ParmBlkPtr pb = (ParmBlkPtr)pbv;
    sil_drive *d = find_drive(pb->ioParam.ioVRefNum);
    UInt32 lba, nblk; int ok, doLog; Str255 L;
    if (d == 0) d = (gNDrives > 0) ? &gDrives[0] : 0;    /* single-drive fallback */
    if (d == 0) { DS("  NO DRIVE"); return paramErr; }
    lba  = d->partStart + (UInt32)(pb->ioParam.ioPosOffset / 512);
    nblk = (UInt32)(pb->ioParam.ioReqCount / 512);
    if (nblk == 0) { pb->ioParam.ioActCount = 0; return noErr; }
    if (isWrite && !gSilWriteEnabled) { DS("kW write-protected"); return wPrErr; }
    /* Log EVERY read (few, during mount) but only every 256th write: DIZero does a
     * full-volume zero pass and each DWrite is an FSWrite+FlushVol that would both
     * flood the log and cripple throughput. Failures are always logged. Logging
     * BEFORE the transfer means a crash inside disk_rw shows as a before with no
     * after. */
    doLog = (int)(gWrLogCtr++, 1);   /* v47: log EVERY op (read + write) to localize the freeze */
    if (doLog) {
        L[0]=0;
        PApp(L, isWrite ? "kW> vref=" : "kR> vref="); PAppN(L, (long)pb->ioParam.ioVRefNum);
        PApp(L, " pos="); PAppN(L, pb->ioParam.ioPosOffset);
        PApp(L, " cnt="); PAppN(L, pb->ioParam.ioReqCount);
        PApp(L, " lba="); PAppN(L, (long)lba);
        PApp(L, " n=");   PAppN(L, (long)nblk);
        DWrite(L);
    }
    ok = disk_rw(d->port, lba, nblk, (UInt8 *)pb->ioParam.ioBuffer, isWrite);
    iolog(lba, (short)(700 + gIoStage));   /* v67: trace outcome/stage of each FM-routed op (hang-safe ring) */
    pb->ioParam.ioActCount = ok ? (long)(nblk * 512UL) : 0;
    if (!ok)          DS(isWrite ? "kW< FAIL" : "kR< FAIL");   /* always log failures */
    else if (doLog)   DHex(isWrite ? "kW< OK " : "kR< OK ", (UInt8 *)pb->ioParam.ioBuffer, 16);
    return ok ? noErr : ioErr;
}
OSStatus sil_disk_read(void *pb)  { return disk_io(pb, 0); }
OSStatus sil_disk_write(void *pb) { return disk_io(pb, 1); }

/* v31 diagnostic: log EVERY DoDriverIO command code + kind at dispatch entry, so
 * commands we don't otherwise log (kOpen=0/kClose=1/kKillIO=6/kFinalize=8/...)
 * become visible right up to the mount hang. Skip read(2)/write(3) — disk_io
 * logs those (throttled) and DIZero's bulk writes would flood this. If NOTHING
 * from here appears after the last MDB read, OS 9 hung with no driver call at all
 * (=> the hang is purely internal; points at the interrupt-completion theory). */
void sil_disk_logcmd(unsigned long code, unsigned long kind)
{
    Str255 L;
    /* v32: log ALL commands incl. read(2)/write(3) with their kind (sync=1/async=2/
     * immediate=4). Safe now because this run SKIPS DIZero (no bulk-write flood);
     * the read kinds tell us if the mount reads are async (=> completion-timing) if
     * the drive-status fix doesn't land. */
    L[0]=0; PApp(L,"DoIO code="); PAppN(L,(long)code);
    PApp(L," kind="); PAppN(L,(long)kind); DWrite(L);
}

/*
 * Status: we implement NO Status selectors, so decline every one with statusErr.
 * This is the crux of the v24 crash fix. csCode 43 = DriverGestalt: the mount
 * queries selectors ('devt','sync','boot',...) whose driverGestaltResponse the
 * driver is expected to fill; several are POINTERS. v24 returned noErr WITHOUT
 * filling it, so the mount dereferenced whatever garbage sat in the param block
 * -> "application quit unexpectedly". statusErr is the documented "I don't
 * support this selector" answer -> the File Manager uses safe defaults (exactly
 * how pre-DriverGestalt drivers behave). Log the gestalt selector so we can see
 * which ones the mount wanted, in case any needs a real answer later.
 */
OSStatus sil_disk_status(void *pbv)
{
    ParmBlkPtr pb = (ParmBlkPtr)pbv;
    short cs = pb->cntrlParam.csCode;
    cslog(1, pb);                                     /* v44: record every Status csCode */
    if (cs == kDriveStatus) {                         /* 8 — the Disk Init Manager
        * (DIZero/DIFormat) and the mounter query drive state. Fill a DrvSts:
        * disk present + nonejectable + installed, write-enabled per the lockout.
        * Clear the whole csParam FIRST so the DrvSts fields we don't set
        * (qLink/dQDrive/dQFSID/...) are zero, not leftover garbage a caller
        * might dereference. */
        DrvSts *ds = (DrvSts *)&pb->cntrlParam.csParam[0];
        int k; for (k = 0; k < 11; k++) pb->cntrlParam.csParam[k] = 0;
        ds->track       = 0;
        ds->writeProt   = gSilWriteEnabled ? 0 : (char)0x80;  /* bit7=1 => locked */
        ds->diskInPlace = 8;                          /* nonejectable disk in place */
        ds->installed   = 1;                          /* drive installed            */
        ds->sides       = 0;
        DN("kStatus DriveStatus -> ok, wp=", gSilWriteEnabled ? 0 : 1);
        return noErr;
    }
    if (cs == kDriverGestaltCode) {                  /* 43 */
        DriverGestaltParam *dg = (DriverGestaltParam *)pbv;
        OSType sel = dg->driverGestaltSelector;
        /* v56: answer 'lpwr' (kdgSupportsSwitching) = FALSE => declare NO power-switching
         * support, so the OS/family expert leaves us in D0 and never issues the PCI D3 that
         * Audio CD Access triggers (the v51-v55 sil_r8/bar5+0x87 bus-error root cause).
         * Documented opt-out: "Designing PCI Cards and Drivers for Power Macintosh" p.240
         * ("drivers that do not support these calls should return false to 'lpwr' ... and
         * controlErr to SetPowerMode") + p.470 ("a card's driver may elect to ignore power
         * switching commands ... by returning the DriverGestalt selector 'lpwr'"). The Boolean
         * response is a SCALAR written into the param block (no pointer to deref) => safe,
         * unlike the v24 pointer-type-selector crash that forced decline-all. */
        if (sel == 'lpwr') { dg->driverGestaltResponse = 0; return noErr; }
        /* v49: REMOVED the v46 'devt'='disk' answer. It never stopped the audio-CD claim (Audio CD
         * Access claims via the CD Controls, not 'devt'), and it is the one mount-path behavior added
         * since v42's clean PC Exchange MS-DOS mount — the prime suspect for the v48-Audio-CD-removed
         * crash. Revert to declining EVERY DriverGestalt (exactly v42's Status behavior) so PC Exchange
         * gets the same defaults it successfully mounted with. */
        /* v40: decline EVERY DriverGestalt selector (INCLUDING 'minf'/kdgMediaInfo)
         * with statusErr, matching the sibling USB2 driver whose FAT/PC-Exchange mount
         * SUCCEEDED. v39 ANSWERED 'minf' (numberBlocks/blockSize/mediaType) — the one
         * behavioral difference from that proven config — and PBMountVol froze in OS 9's
         * FSM at the SAME point the HFS mount did (recognizer probes complete, then no
         * further driver call). Declining 'minf' lets the File Manager use safe defaults
         * and take PC Exchange's foreign-FS path instead. statusErr is the documented
         * "selector unsupported" answer (pre-DriverGestalt drivers behave identically). */
        { char s[6]; Str255 L;
          s[0]=(char)(sel>>24); s[1]=(char)(sel>>16); s[2]=(char)(sel>>8); s[3]=(char)sel; s[4]=0;
          L[0]=0; PApp(L,"kStatus DriverGestalt '"); PApp(L,s); PApp(L,"' -> statusErr"); DWrite(L); }
        return statusErr;
    }
    DN("kStatus csCode= (decline) ", (long)cs);
    return statusErr;
}

/* Control: acknowledge (noErr). Control calls set state and carry no response
 * buffer for us to corrupt; v24's csCode 70 was ack'd and the mount proceeded. */
OSStatus sil_disk_control(void *pbv)
{
    ParmBlkPtr pb = (ParmBlkPtr)pbv;
    short cs = pb->cntrlParam.csCode;
    cslog(2, pb);                                     /* record every Control csCode (pure memory) */
    /* v51: decline the CD-ROM Controls 104/125 with controlErr so Audio CD Access backs off. On the
     * HFS path this is SAFE (unlike the FAT v44 freeze): the v50a ring proved OS 9's built-in HFS
     * mounter uses cs=70/8/23/43/22/21/20 and NEVER 104/125 — those are ONLY Audio CD Access's CD
     * probes, and v50b showed they arrive AFTER PBMountVol returns (mount done), when Audio CD Access
     * probes the already-mounted drive as a CD and crashes the system. controlErr => "not a CD" => it
     * backs off => no post-mount crash, with Audio CD Access left ENABLED (no user workaround needed). */
    if (cs == 104 || cs == 125) return controlErr;
    /* v56: decline SetPowerMode (csCode 70) for any LOW-power mode (pmStandby/pmIdle/pmSleep =>
     * csParam[0] != 0) so nothing can put us into D3; still ACK pmActive (csParam[0]==0) exactly
     * as before (the built-in mounter's cs=70 during mount is pmActive, per the v50a ring, so the
     * working mount path is untouched). Pairs with the 'lpwr'=false gate in sil_disk_status. */
    if (cs == 70 && pb->cntrlParam.csParam[0] != 0) return controlErr;
    return noErr;
}
