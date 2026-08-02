/*
 * sil3512_os.c — Mac OS 9 glue for the SiI3512 NDRV (Milestone 2).
 *
 * The only file that pulls in the OS 9 driver headers. Responsibilities:
 *   - extract the card's Name Registry node from the DriverInitInfo
 *   - enable the PCI device (Memory Space + Bus Master in the Command register)
 *   - map the BAR5 MMIO register block by correlating "assigned-addresses"
 *     (find the entry for config register 0x24 = BAR5, memory space) with the
 *     OF-mapped logical address in "AAPL,address"
 *   - run controller bring-up (sil_hc_init)
 *
 * PCI-enable + register-mapping idioms follow ../usb2-ehci/src/ehci_os.c and
 * ../usb2-ehci/M2-GLUE-SPEC.md (confirmed against Apple's OHCI UIMInitialize).
 */
#include <MacTypes.h>
#include <Devices.h>
#include <NameRegistry.h>
#include <PCI.h>
#include <DriverServices.h>
#include <MacMemory.h>
#include <OSUtils.h>          /* Delay() */
#include <Resources.h>        /* Get1Resource (M6.2a boot self-install) */
#include <Files.h>            /* FSWrite boot log (M6.2a) */

#include "sil3512.h"

sil_softc gSil;

/* One wired PRD (scatter/gather) table, reused across the polled, single-
 * threaded DMA ops. Each SFF-8038i entry is 8 bytes: LE base address, then
 * LE (byteCount | EOT<<31). The SATL chunks large transfers to fit this table,
 * so its size only bounds per-command size, never total request size. 256
 * entries => worst-case (one 4 KB page each) 1 MB / 2048 sectors per command. */
#define SIL_PRD_ENTRIES  256
#define SIL_PRD_EOT      0x80000000UL
static struct { UInt8 *base; UInt32 basePhys; } gPRD;

/* Conservative sectors-per-command the PRD table guarantees, assuming the
 * worst case of one 4 KB page per entry (GetPhysical coalesces contiguous
 * pages, so real capacity is usually far higher). */
UInt32 sil_dma_max_sectors(void)
{
    return (SIL_PRD_ENTRIES * 4096UL) / 512UL;      /* = SIL_PRD_ENTRIES * 8 */
}

/* PCI config space */
#define kPCICommandReg    0x04
#define kPCICacheLineReg  0x0C
#define kPCIBAR5Reg       0x24    /* config offset of BAR5 (our MMIO window) */
#define kPCICmdMemSpace   0x0002
#define kPCICmdBusMaster  0x0004
/* v54: PCI capability list + Power-Management capability (to see/undo a D3 power-down) */
#define kPCIStatusReg      0x06
#define kPCIStatusCapList  0x0010
#define kPCICapPtrReg      0x34
#define kPCICapIDPowerMgmt 0x01
#define kPCIPMCSROffset    0x04
#define kPCIPMCSRStateMask 0x0003

/* OF "assigned-addresses" entry = 5 x UInt32; phys.hi low byte = BAR config
 * offset, bits 24-25 = address space (2=mem32, 3=mem64). */
#define kAddrEntryWords   5
#define AA_REG(physhi)    ((physhi) & 0xFF)
#define AA_SPACE(physhi)  (((physhi) >> 24) & 0x3)

/* --- small helpers --------------------------------------------------------- */

void sil_delay_ms(UInt32 ms)
{
    /* Classic Delay() — 60 Hz ticks, task-level, bulletproof. (DelayForHardware
     * with a UpTime()+ms deadline HUNG on hardware — v6 isolated it — so we use
     * Delay instead.) Granularity is ~16.7 ms, which is fine for reset/link-up
     * polling. TODO(resident driver): an interrupt-safe spin (Delay must not be
     * called at interrupt time) once we move off task level. */
    unsigned long fin;
    unsigned long ticks = (ms * 60UL + 999UL) / 1000UL;   /* ms -> ticks, round up */
    if (ticks < 1) ticks = 1;
    Delay(ticks, &fin);
}

/* Raw PCI cache-line-size register (in 32-bit dwords) — sata_sil.c derives the
 * per-port FIFO cls value from this. */
UInt8 sil_pci_cacheline(void)
{
    UInt32 v = 0;
    (void)ExpMgrConfigReadByte((RegEntryIDPtr)&gSil.node,
                               (LogicalAddress)kPCICacheLineReg, (UInt8 *)&v);
    return (UInt8)v;
}

/* Read a Name Registry property into a pool buffer (freed by caller). */
static OSStatus get_prop(RegEntryIDPtr node, const char *name,
                         void **outBuf, ByteCount *outSize)
{
    OSStatus err;
    ByteCount size = 0;
    void *buf;

    err = RegistryPropertyGetSize(node, name, &size);
    if (err != noErr) return err;
    buf = PoolAllocateResident(size, false);
    if (buf == NULL) return memFullErr;
    err = RegistryPropertyGet(node, name, buf, &size);
    if (err != noErr) { PoolDeallocate(buf); return err; }
    *outBuf = buf; *outSize = size;
    return noErr;
}

/* Map BAR5: find the assigned-addresses entry for config register 0x24 in
 * memory space, then take its OF-mapped logical address from AAPL,address. */
static OSStatus map_bar5(sil_softc *sc, RegEntryIDPtr node)
{
    OSStatus err, result = paramErr;
    UInt32 *aa = NULL, *la = NULL;
    ByteCount aaSize = 0, laSize = 0;
    UInt32 i, nEntries;

    err = get_prop(node, "assigned-addresses", (void **)&aa, &aaSize);
    if (err != noErr) return err;
    err = get_prop(node, "AAPL,address", (void **)&la, &laSize);
    if (err != noErr) { PoolDeallocate(aa); return err; }

    nEntries = aaSize / (kAddrEntryWords * sizeof(UInt32));
    for (i = 0; i < nEntries && (i * sizeof(UInt32)) < laSize; i++) {
        UInt32 physHi = aa[i * kAddrEntryWords];   /* registry values host-endian */
        if (AA_REG(physHi) == kPCIBAR5Reg && AA_SPACE(physHi) >= 2) {
            sc->bar5 = (volatile void *)la[i];
            result = noErr;
            break;
        }
    }
    PoolDeallocate(aa);
    PoolDeallocate(la);
    return result;
}

/* Allocate the wired PRD table (one contiguous, page-aligned block with a
 * stable physical address). Same wiring idiom as ../usb2-ehci alloc_framelist. */
static OSStatus prd_pool_init(void)
{
    Ptr raw;
    LogicalAddress buf;
    LogicalToPhysicalTable tbl;
    unsigned long count = 1;
    OSStatus err;
    UInt32 bytes = SIL_PRD_ENTRIES * 8;

    raw = NewPtrSysClear(bytes + 0x1000);
    if (raw == NULL) return memFullErr;
    buf = (LogicalAddress)(((UInt32)raw + 0xFFF) & ~0xFFFUL);   /* page align */

    err = LockMemory(buf, bytes);
    if (err != noErr) return err;
    tbl.logical.address = buf;
    tbl.logical.count = bytes;
    err = GetPhysical(&tbl, &count);
    if (err != noErr) return err;

    gPRD.base = (UInt8 *)buf;
    gPRD.basePhys = (UInt32)tbl.physical[0].address;
    return noErr;
}

/*
 * Wire a client data buffer and build a PRD table describing its physical
 * pages. GetPhysical resolves up to 8 segments per call, so we loop until the
 * whole buffer is covered or the table fills. Returns 1 on success and hands
 * back the PRD table's physical address; the caller must call sil_dma_complete
 * (with the same buffer) to unwire it after the transfer.
 */
int sil_dma_prepare(void *buf, UInt32 len, int isWrite,
                    UInt32 *prdPhysOut, void **cookieOut)
{
    UInt8 *cur = (UInt8 *)buf;
    UInt32 remaining = len;
    UInt32 *prd = (UInt32 *)gPRD.base;
    int nEnt = 0;

    (void)isWrite;                                  /* LockMemory wires both ways */
    if (gPRD.base == NULL || len == 0) return 0;
    if (LockMemory(buf, len) != noErr) return 0;

    while (remaining > 0 && nEnt < SIL_PRD_ENTRIES) {
        LogicalToPhysicalTable tbl;
        unsigned long count = 8;
        unsigned long i, resolved = 0;

        tbl.logical.address = cur;
        tbl.logical.count = remaining;
        if (GetPhysical(&tbl, &count) != noErr || count == 0) {
            UnlockMemory(buf, len);
            return 0;
        }
        for (i = 0; i < count && nEnt < SIL_PRD_ENTRIES; i++) {
            UInt32 seg = (UInt32)tbl.physical[i].count;
            prd[nEnt * 2 + 0] = sil_cpu_to_le32((UInt32)tbl.physical[i].address);
            /* byteCount in low 16 bits (0 => 64 KB); EOT set later on the last */
            prd[nEnt * 2 + 1] = sil_cpu_to_le32(seg & 0xFFFF);
            resolved += seg;
            nEnt++;
        }
        cur += resolved;
        remaining -= resolved;
    }
    if (remaining > 0) {                            /* table too small for buffer */
        UnlockMemory(buf, len);
        return 0;
    }
    /* set EOT on the final entry (second dword, bit 31 — value is LE) */
    prd[(nEnt - 1) * 2 + 1] |= sil_cpu_to_le32(SIL_PRD_EOT);

    *prdPhysOut = gPRD.basePhys;
    *cookieOut = buf;
    return 1;
}

void sil_dma_complete(void *cookie, UInt32 len)
{
    if (cookie) UnlockMemory(cookie, len);
}

/* --- driver command handlers (called from DoDriverIO) ---------------------- */

/*
 * Bring the controller to the point of being register-accessible: stash the
 * node, enable PCI (Mem+BusMaster), map BAR5, allocate the PRD pool. Does NOT
 * run sil_hc_init (reset/link-up) — the caller does that next, so a diagnostic
 * loader can read registers between mapping and resetting. nodeArg is a
 * RegEntryIDPtr (kept void* so the header stays OS-free).
 */
OSStatus sil_bringup(void *nodeArg)
{
    RegEntryIDPtr node;
    OSStatus err;
    UInt16 cmd = 0;

    if (nodeArg == NULL) return paramErr;
    *(RegEntryID *)&gSil.node = *(RegEntryID *)nodeArg;
    node = (RegEntryIDPtr)&gSil.node;

    /* enable Memory Space + Bus Master, preserving other Command bits */
    err = ExpMgrConfigReadWord(node, (LogicalAddress)kPCICommandReg, &cmd);
    if (err != noErr) return err;
    cmd |= (kPCICmdMemSpace | kPCICmdBusMaster);
    err = ExpMgrConfigWriteWord(node, (LogicalAddress)kPCICommandReg, cmd);
    if (err != noErr) return err;

    err = map_bar5(&gSil, node);
    if (err != noErr) return err;

    /* v53: stash the OF-programmed BAR5 config value (config 0x24) so sil_pci_reassert
     * can rewrite it if the card is reset post-mount. A PCI reset zeroes the BARs (not
     * just the Command register), which is why v52's command-only re-assert wasn't
     * enough. Config-space reads survive a memory-decode teardown. */
    gSil.bar5cfg = 0;
    (void)ExpMgrConfigReadLong(node, (LogicalAddress)kPCIBAR5Reg, &gSil.bar5cfg);

    /* v54: locate the PCI Power-Management capability (ID 0x01). v52/v53 proved the
     * teardown is NOT a config clear (re-writing command + BAR was ineffective); the
     * remaining config-readable-but-decode-dead suspect is a D3 power-down, which lives
     * in the PMCSR (pmCap+4). sil_pci_reassert reads/undoes it from here. */
    gSil.pmCap = 0;
    {
        UInt16 pcistat = 0;
        if (ExpMgrConfigReadWord(node, (LogicalAddress)kPCIStatusReg, &pcistat) == noErr
            && (pcistat & kPCIStatusCapList)) {
            UInt8 off = 0; int guard = 0;
            (void)ExpMgrConfigReadByte(node, (LogicalAddress)kPCICapPtrReg, &off);
            off &= 0xFC;
            while (off >= 0x40 && guard++ < 48) {
                UInt8 id = 0, next = 0;
                (void)ExpMgrConfigReadByte(node, (LogicalAddress)(UInt32)off, &id);
                (void)ExpMgrConfigReadByte(node, (LogicalAddress)(UInt32)(off + 1), &next);
                if (id == kPCICapIDPowerMgmt) { gSil.pmCap = off; break; }
                off = (UInt8)(next & 0xFC);
            }
        }
    }

    err = prd_pool_init();
    if (err != noErr) return err;

    gSil.inited = 1;
    return noErr;
}

/* v52: re-assert PCI Memory-Space + Bus-Master enables; return the PRE-value of the Command
 * register. The MacsBug capture (v50, Audio CD Access ON) showed our next ATA status MMIO read
 * (sil_r8 at bar5+0x87) BUS-ERRORS post-mount => the card stopped decoding its BAR. The likely
 * cause is the OS CD subsystem (Apple CD/DVD driver / ATA Mgr) probing our SiI3512 as a CD-ROM
 * candidate and clearing the Command register (or resetting the card). Config-space access works
 * even when memory decode is off, so re-asserting Mem+BusMaster here restores decode (if that's the
 * cause) and the following MMIO read succeeds instead of faulting. Pre-value bit1=MemSpace,
 * bit2=BusMaster lets the caller see whether decode had been cleared. */
UInt16 sil_pci_reassert(void)
{
    RegEntryIDPtr node = (RegEntryIDPtr)&gSil.node;
    UInt16 cmd = 0;
    UInt32 bar = 0;
    UInt16 pmcsr = 0;
    int woke = 0;
    if (!gSil.inited) return 0;

    /* v54: snapshot the AS-FOUND config state BEFORE touching anything (dm &gSil.snapSeq).
     * v52 (command bits) and v53 (BAR base) both re-established config and the read STILL
     * faulted => not a config clear. v54 confirmed the real cause: ACA power-downs the card
     * to D3 (pmCap@0x60) — config stays readable, memory decode dies. */
    (void)ExpMgrConfigReadWord(node, (LogicalAddress)kPCICommandReg, &cmd);
    (void)ExpMgrConfigReadLong(node, (LogicalAddress)kPCIBAR5Reg, &bar);
    if (gSil.pmCap)
        (void)ExpMgrConfigReadWord(node,
                (LogicalAddress)(UInt32)(gSil.pmCap + kPCIPMCSROffset), &pmcsr);
    gSil.snapCmd   = cmd;
    gSil.snapBar   = bar;
    gSil.snapPmcsr = gSil.pmCap ? (UInt32)pmcsr : 0xFFFFFFFFUL;
    gSil.snapSeq++;

    /* v54: wake to D0 — memory decode can't return while the card is in D1/D2/D3. */
    if (gSil.pmCap && (pmcsr & kPCIPMCSRStateMask) != 0) {
        (void)ExpMgrConfigWriteWord(node,
                (LogicalAddress)(UInt32)(gSil.pmCap + kPCIPMCSROffset),
                (UInt16)(pmcsr & ~kPCIPMCSRStateMask));
        woke = 1;
    }

    /* v53: restore the BAR5 base if cleared, then re-enable Mem+BusMaster (config-space
     * writes work even before the D0 recovery delay). */
    if (gSil.bar5cfg && bar != gSil.bar5cfg)
        (void)ExpMgrConfigWriteLong(node, (LogicalAddress)kPCIBAR5Reg, gSil.bar5cfg);
    if ((cmd & (kPCICmdMemSpace | kPCICmdBusMaster)) != (kPCICmdMemSpace | kPCICmdBusMaster))
        (void)ExpMgrConfigWriteWord(node, (LogicalAddress)kPCICommandReg,
                                    (UInt16)(cmd | kPCICmdMemSpace | kPCICmdBusMaster));

    /* v55: a D3->D0 transition RESETS the SiI3512 (SATA link + taskfile/DMA state lost),
     * so waking it isn't enough — v54 proved decode returns (no crash) but reads came back
     * garbage (PBMountVol badMDBErr -60). Re-establish the controller here (FIFO/quirk +
     * COMRESET link-up) so the caller's next MMIO read returns real data. Only on the wake
     * transition, so healthy I/O stays on the fast path. The mount path is task level, so
     * sil_hc_init's Delay-based link-up is safe. */
    if (woke) {
        sil_delay_ms(10);            /* PCI D3hot->D0 recovery before any MMIO */
        (void)sil_hc_init(&gSil);    /* reset + SATA link-up (re-reads DET, clears SError) */
        gSil.reinitCount++;
    }
    return cmd;
}

/* v57 (M6.2c): pure config-space snapshot — read Command(0x04), BAR5(0x24) and PMCSR into
 * the snap* fields with config cycles ONLY. Config cycles reach the card even when its memory
 * decode is dead (or it is in D3), so this never bus-errors. The caller logs the snap* fields
 * to a flushed file BEFORE any MMIO-touching recovery, so the post-boot teardown state is
 * captured on disk even if a later MMIO read faults (crash-proof; no MacsBug needed). */
void sil_pci_snapshot(void)
{
    RegEntryIDPtr node = (RegEntryIDPtr)&gSil.node;
    UInt16 cmd = 0, pmcsr = 0;
    UInt32 bar = 0;
    if (!gSil.inited) return;
    (void)ExpMgrConfigReadWord(node, (LogicalAddress)kPCICommandReg, &cmd);
    (void)ExpMgrConfigReadLong(node, (LogicalAddress)kPCIBAR5Reg,    &bar);
    if (gSil.pmCap)
        (void)ExpMgrConfigReadWord(node,
                (LogicalAddress)(UInt32)(gSil.pmCap + kPCIPMCSROffset), &pmcsr);
    gSil.snapCmd   = cmd;
    gSil.snapBar   = bar;
    gSil.snapPmcsr = gSil.pmCap ? (UInt32)pmcsr : 0xFFFFFFFFUL;
    gSil.snapSeq++;
}

/* v57 (M6.2c): one-shot FULL recovery run once at the top of the post-boot mount NM, before
 * PBMountVol issues any read. Uses the snap* values captured by sil_pci_snapshot() (call it
 * first). Goes further than the per-read sil_pci_reassert in two ways that matter for the
 * NON-ACA teardown (ACA is removed, so this is the OS's own post-INIT-parade PCI/power/ATA
 * finalization disrupting our too-early INIT install):
 *   - it RE-MAPS the logical BAR from AAPL,address (sil_pci_reassert only ever restored the
 *     CONFIG BAR at 0x24; if the OS rebuilt our CPU-side memory window, the stale logical
 *     pointer is exactly what makes sil_r8 bus-error), and
 *   - it re-inits the controller UNCONDITIONALLY (not only on a D3->D0 wake).
 * Returns a findings bitmask so the caller can log what the teardown actually did:
 *   0x01 = card was power-managed (PMCSR state != D0)
 *   0x02 = Command decode bits (MemSpace|BusMaster) were cleared
 *   0x04 = BAR5 config value (0x24) differed from the INIT value
 *   0x08 = re-mapping the logical BAR changed the address (window was rebuilt) */
UInt32 sil_remount_recover(void)
{
    RegEntryIDPtr node = (RegEntryIDPtr)&gSil.node;
    volatile void *oldBar = gSil.bar5;
    UInt32 findings = 0;
    if (!gSil.inited) return 0;

    if (gSil.pmCap && gSil.snapPmcsr != 0xFFFFFFFFUL
        && (gSil.snapPmcsr & kPCIPMCSRStateMask) != 0) findings |= 0x01;
    if ((gSil.snapCmd & (kPCICmdMemSpace | kPCICmdBusMaster))
        != (kPCICmdMemSpace | kPCICmdBusMaster)) findings |= 0x02;
    if (gSil.bar5cfg && gSil.snapBar != gSil.bar5cfg) findings |= 0x04;

    /* 1. wake to D0 first — a card in D3 responds ONLY to config cycles; re-enabling the
     *    Command MemSpace bit does nothing until it is back in D0. */
    if (findings & 0x01) {
        (void)ExpMgrConfigWriteWord(node,
                (LogicalAddress)(UInt32)(gSil.pmCap + kPCIPMCSROffset),
                (UInt16)(gSil.snapPmcsr & ~kPCIPMCSRStateMask));
        sil_delay_ms(10);                    /* PCI D3hot->D0 recovery before any MMIO */
    }
    /* 2. restore the config BAR + decode enables (config writes work pre-decode). */
    if (findings & 0x04)
        (void)ExpMgrConfigWriteLong(node, (LogicalAddress)kPCIBAR5Reg, gSil.bar5cfg);
    if (findings & 0x02)
        (void)ExpMgrConfigWriteWord(node, (LogicalAddress)kPCICommandReg,
                (UInt16)(gSil.snapCmd | kPCICmdMemSpace | kPCICmdBusMaster));
    /* 3. RE-MAP the logical BAR (the new step): re-fetch AAPL,address in case the OS rebuilt
     *    our memory window post-INIT. No MMIO — registry reads only. */
    (void)map_bar5(&gSil, node);
    if (gSil.bar5 != oldBar) findings |= 0x08;
    /* 4. re-establish the controller (FIFO/quirk + SATA link-up). FIRST MMIO happens here;
     *    the caller has already flushed the snapshot diagnostic, so a fault here still leaves
     *    the evidence on disk. */
    (void)sil_hc_init(&gSil);
    gSil.reinitCount++;
    return findings;
}

OSStatus sil_os_initialize(void *initInfo)
{
    DriverInitInfoPtr info = (DriverInitInfoPtr)initInfo;
    OSStatus err;

    if (info == NULL) return paramErr;
    err = sil_bringup(&info->deviceEntry);       /* enable + map + PRD pool */
    if (err != noErr) return err;
    err = sil_hc_init(&gSil);                     /* reset + SATA link-up    */
    if (err != noErr) return err;
    return sil_disk_scan_and_add(info->refNum);  /* AddDrive HFS partitions */
}

/* v69: ROM-CLAIM model. When claimed from the ROM parcel, kInitialize runs at EARLY boot, where
 * the full bring-up hangs (v63: Delay()/DMA before the timer + DMA services are up). So the ROM
 * driver splits it: kInitialize only STASHES the DriverInitInfo (node + refNum) and returns noErr
 * (boot-safe -- no hardware, no Delay, no DMA -- this is what let v65 boot to a stable desktop);
 * the real bring-up + AddDrive runs in kOpen (sil_os_open_bringup), which fires POST-boot when an
 * app OpenInstalledDriver's the claimed driver -- a settled, timers-up, DMA-up context. */
static RegEntryID gInitNode;
static short      gInitRefNum = 0;
static int        gBroughtUp  = 0;
OSStatus sil_os_init_stash(void *initInfo)
{
    DriverInitInfoPtr info = (DriverInitInfoPtr)initInfo;
    if (info == NULL) return paramErr;
    gInitNode   = info->deviceEntry;
    gInitRefNum = info->refNum;
    gBroughtUp  = 0;
    return noErr;
}
OSStatus sil_os_open_bringup(void)
{
    OSStatus err;
    if (gBroughtUp) return noErr;                 /* bring up + AddDrive exactly once */
    err = sil_bringup(&gInitNode);                /* enable + map + PRD pool */
    if (err != noErr) return err;
    err = sil_hc_init(&gSil);                     /* reset + SATA link-up    */
    if (err != noErr) return err;
    err = sil_disk_scan_and_add(gInitRefNum);     /* AddDrive HFS partitions */
    if (err == noErr) gBroughtUp = 1;
    return err;
}

/* ---- M6.2a: boot-time self-install (no app) ---------------------------------------- *
 * The shippable "no app" path. The 68K INIT (resident/esata_init.c) loads our PEF
 * ('PPC ' 128) as a transient fragment and Mixed-Mode-calls InstallMe; InstallMe copies
 * the SAME PEF into the system heap and InstallDriverFromMemory's it, so the driver goes
 * RESIDENT in the Device Manager Unit Table (independent of the dying INIT). The installed
 * driver's DoDriverIO(kInitialize) then brings up the card + AddDrive's the HFS partition;
 * the OS's startup mount pass mounts the volume. Pattern proven by usb2-ehci R2b-3
 * [[reference_os9_init_resident_driver]]. Requires Audio CD Access disabled (ship caveat). */
static short gILog = 0;
static void ILog(const char *s)
{
    long n = 0, one = 1; char cr = '\r'; FSSpec sp;
    if (!gILog) {
        if (FSMakeFSSpec(0, 0, "\pSiI3512 InstallMe Log", &sp) == noErr) (void)FSpDelete(&sp);
        if (FSpCreate(&sp, 'ttxt', 'TEXT', 0) == noErr) (void)FSpOpenDF(&sp, fsRdWrPerm, &gILog);
    }
    if (gILog) { while (s[n]) n++; (void)FSWrite(gILog, &n, (Ptr)s);
        (void)FSWrite(gILog, &one, (Ptr)&cr); (void)FlushVol(0, 0); }
}
static void ILogX(const char *label, long v)
{
    char b[96]; int i = 0, j; static const char hx[] = "0123456789ABCDEF";
    while (label[i] && i < 72) { b[i] = label[i]; i++; }
    b[i++] = ' '; b[i++] = '0'; b[i++] = 'x';
    for (j = 28; j >= 0; j -= 4) b[i++] = hx[((unsigned long)v >> j) & 0xF];
    b[i] = 0; ILog(b);
}

void InstallMe(void)
{
    Handle h; Ptr img; Size sz; RegEntryID node; DriverRefNum ref = 0; OSErr err, ne;
    ILog("=== SiI3512 InstallMe (PPC, at INIT): installing resident driver ===");
    h = Get1Resource('PPC ', 128);
    if (h == NULL) { ILog("  Get1Resource('PPC ',128) == NULL"); return; }
    sz = GetHandleSize(h);
    ILogX("  PEF size", (long)sz);
    img = NewPtrSys(sz);
    if (img == NULL) { ILog("  NewPtrSys FAILED (system heap)"); return; }
    HLock(h); BlockMoveData(*h, img, sz); HUnlock(h);
    ne = (OSErr)sil_find_node(&node);
    ILogX("  sil_find_node err (0=found)", (long)ne);
    if (ne != noErr) { ILog("  SiI3512 node NOT found; abort"); return; }
    err = InstallDriverFromMemory(img, (long)sz, "\pSiI3512disk", &node, 48, 127, &ref);
    ILogX("  InstallDriverFromMemory err (0=ok)", (long)err);
    ILogX("  driver refNum", (long)ref);
    ILog("=== InstallMe done (bring-up + AddDrive: see 'SiI3512 Driver Log') ===");
}

/*
 * Walk the Name Registry for the SiI3512's device node, matching by PCI
 * vendor/device ID (0x1095 / 0x3512) rather than by name — the LaCie card's
 * existing Sunrich FCode names the node "SunrichSATA3512", not "pci1095,3512",
 * so an ID match is the robust way to find it. outNode is a RegEntryIDPtr.
 */
OSStatus sil_find_node(void *outNode)
{
    RegEntryIter cookie;
    RegEntryID entry;
    Boolean done = false, first = true;
    OSStatus err;

    err = RegistryEntryIterateCreate(&cookie);
    if (err != noErr) return err;

    /* First call establishes the descendant walk; kRegIterContinue advances it.
     * (Passing kRegIterDescendants every call only returns the top-level roots.) */
    while (1) {
        UInt32 vid = 0, did = 0;
        RegPropertyValueSize sz;

        err = RegistryEntryIterate(&cookie,
                                   first ? kRegIterDescendants : kRegIterContinue,
                                   &entry, &done);
        first = false;
        if (err != noErr || done) break;

        sz = sizeof(vid);
        if (RegistryPropertyGet(&entry, "vendor-id", &vid, &sz) != noErr) continue;
        if ((vid & 0xFFFF) != 0x1095) continue;
        sz = sizeof(did);
        if (RegistryPropertyGet(&entry, "device-id", &did, &sz) != noErr) continue;
        if ((did & 0xFFFF) != 0x3512) continue;

        *(RegEntryID *)outNode = entry;              /* match */
        RegistryEntryIterateDispose(&cookie);
        return noErr;
    }
    RegistryEntryIterateDispose(&cookie);
    return -1;                                       /* not found */
}

OSStatus sil_os_open(void)
{
    if (!gSil.inited) return paramErr;
    /* register the SCSI Manager 4.3 HBA/SIM so the present ports appear as SCSI
     * targets; IDENTIFY per port happens inside registration. */
    if (!gSil.opened) {
        OSStatus err = sil_scsi_register();
        if (err != noErr) return err;
        gSil.opened = 1;
    }
    return noErr;
}

OSStatus sil_os_close(void)    { gSil.opened = 0; return noErr; }
OSStatus sil_os_finalize(void) { gSil.inited = 0; return noErr; }
