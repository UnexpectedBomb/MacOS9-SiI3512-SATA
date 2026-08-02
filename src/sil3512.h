/*
 * sil3512.h — shared driver state + prototypes for the SiI3512 NDRV.
 *
 * Split like ../usb2-ehci: sil3512_driver.c is the header-light dispatch
 * container; sil3512_os.c is the only file that pulls in the OS 9 driver
 * headers (PCI/NameRegistry/DriverServices); sil3512_hw.c is OS-independent
 * controller logic driving the registers in sil3512_regs.h.
 */
#ifndef SIL3512_H
#define SIL3512_H

#include "sil3512_regs.h"

typedef long OSStatus;

/* Layout-identical to the OS RegEntryID (struct { UInt32 contents[4]; }); kept
 * opaque here so only sil3512_os.c needs <NameRegistry.h>. Same trick the EHCI
 * driver uses for EHCIRegEntryID. */
typedef struct { UInt32 contents[4]; } SilRegEntryID;

typedef struct {
    UInt8         present;      /* device detected on this port (SStatus DET)   */
    UInt8         atapi;        /* 0 = ATA disk; ATAPI handling is later         */
    UInt8         lba48;        /* supports 48-bit LBA (from IDENTIFY)           */
    UInt8         oversize;     /* true capacity exceeds OS 9's 2 TB addressing  */
    UInt32        nSectors;     /* capacity in 512-byte sectors (clamped to 2 TB)*/
    char          model[41];    /* IDENTIFY model string (for SCSI INQUIRY)      */
} sil_port_state;

typedef struct {
    SilRegEntryID    node;              /* the card's Name Registry node          */
    volatile void   *bar5;              /* mapped MMIO register base              */
    UInt32           bar5cfg;           /* v53: init-time PCI config-0x24 BAR5 value (restore after a card reset) */
    UInt32           snapSeq;           /* v54: teardown diagnostic — `dm` &gSil.snapSeq in MacsBug at the crash */
    UInt32           snapCmd;           /* v54:   PCI config 0x04 (command) as-found at the last sil_pci_reassert */
    UInt32           snapBar;           /* v54:   PCI config 0x24 (BAR5) as-found                                */
    UInt32           snapPmcsr;         /* v54:   PMCSR as-found (0xFFFFFFFF if the card has no PM capability)    */
    UInt32           reinitCount;       /* v55: # of post-wake controller re-inits (dm right after snapPmcsr)    */
    UInt8            pmCap;             /* v54: PCI Power-Mgmt capability config offset (0 = none)                */
    UInt8            inited;
    UInt8            opened;
    UInt8            nPorts;            /* SIL3512_N_PORTS                        */
    sil_port_state   port[SIL3512_N_PORTS];
} sil_softc;

/* single controller instance (one card); generalise to a list if needed later */
extern sil_softc gSil;

/* --- sil3512_os.c (OS 9 glue) --- */
OSStatus sil_os_initialize(void *initInfo);   /* from DoDriverIO kInitializeCommand */
OSStatus sil_os_init_stash(void *initInfo);   /* v69: kInitialize -- stash init info only (boot-safe) */
OSStatus sil_os_open_bringup(void);           /* v69: kOpen -- deferred bring-up + AddDrive (post-boot) */
OSStatus sil_bringup(void *regEntryID);       /* enable+map+PRD (no reset); RegEntryIDPtr */
OSStatus sil_find_node(void *outRegEntryID);  /* find node by vendor/device ID; RegEntryIDPtr */
OSStatus sil_os_open(void);
OSStatus sil_os_close(void);
OSStatus sil_os_finalize(void);
void     sil_delay_ms(UInt32 ms);
UInt16   sil_pci_reassert(void);              /* v52/v53/v54: snapshot + wake(D0) + re-establish PCI decode; returns cmd pre-value */
void     sil_pci_snapshot(void);              /* v57: pure config-space capture -> snap* fields (NO MMIO, never bus-errors) */
UInt32   sil_remount_recover(void);           /* v57: one-shot pre-mount full recovery (wake/decode/BAR/re-map/re-init); returns findings mask */
UInt8    sil_pci_cacheline(void);             /* raw PCI cache-line reg (dwords)  */
/* wire a client buffer + build the PRD table; pair with sil_dma_complete */
int      sil_dma_prepare(void *buf, UInt32 len, int isWrite,
                         UInt32 *prdPhysOut, void **cookieOut);
void     sil_dma_complete(void *cookie, UInt32 len);
UInt32   sil_dma_max_sectors(void);           /* max sectors one PRD table covers */

/* --- sil3512_hw.c (controller bring-up; OS-independent) --- */
OSStatus sil_hc_init(sil_softc *sc);          /* per-port FIFO/quirk/link-up      */
int      sil_port_linkup(sil_softc *sc, int port);   /* COMRESET + wait DET==3    */
UInt32   sil_scr_read(sil_softc *sc, int port, UInt32 scrOff);
void     sil_scr_write(sil_softc *sc, int port, UInt32 scrOff, UInt32 val);

/* --- sil3512_ata.c (ATA command engine; OS-independent) --- */
int      sil_ata_identify(sil_softc *sc, int port, UInt16 *id256);
void     sil_ata_string(char *dst, const UInt16 *id256, int firstWord, int wordCount);
void     sil_ata_parse_identify(sil_port_state *ps, const UInt16 *id256);
int      sil_ata_rw_dma(sil_softc *sc, int port, UInt32 lba, UInt16 count,
                        UInt32 prdPhys, int isWrite);

/* --- sil3512_scsi.c (SCSI Manager 4.3 SIM + SATL; OS glue) --- */
OSStatus sil_scsi_register(void);

/* --- sil3512_disk.c (drive-queue + block I/O; resident-driver path) --- */
OSStatus sil_disk_scan_and_add(short refNum);   /* kInitialize: AddDrive HFS partitions */
OSStatus sil_disk_read(void *pb);               /* kRead:  ParmBlkPtr                    */
OSStatus sil_disk_write(void *pb);              /* kWrite: ParmBlkPtr                    */
OSStatus sil_disk_status(void *pb);             /* kStatus  (logs csCode)                */
OSStatus sil_disk_control(void *pb);            /* kControl (logs csCode)                */
UInt16   sil_scsi_busid(void);         /* XPT-assigned bus number after register  */
extern int gSilWriteEnabled;           /* 0 = WRITE(10) refused (data-safety lock) */

#endif /* SIL3512_H */
