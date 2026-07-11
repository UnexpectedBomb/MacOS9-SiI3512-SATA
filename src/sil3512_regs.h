/*
 * sil3512_regs.h — Silicon Image SiI3512 register map (BAR5 MMIO).
 *
 * Offsets verified against Linux drivers/ata/sata_sil.c (which drives the
 * 3112/3512/3114 from this one map) and the SiI3512 datasheet. See
 * ../docs/SIL3512-REGISTERS.md.
 *
 * ENDIANNESS: the SiI3512 is a PCI device — its MMIO registers and the PRD
 * scatter/gather tables it reads are LITTLE-ENDIAN. We run on big-endian PPC,
 * so 16/32-bit accesses byte-reverse (lhbrx/sthbrx, lwbrx/stwbrx), exactly as
 * ../usb2-ehci/src/ehci_regs.h does. Byte accesses need no swap.
 */
#ifndef SIL3512_REGS_H
#define SIL3512_REGS_H

typedef unsigned char  UInt8;
typedef unsigned short UInt16;
typedef unsigned long  UInt32;

/* --- byte-reversed (little-endian) MMIO access on big-endian PPC --- */
static inline UInt8 sil_r8(volatile void *base, UInt32 off)
{
    return *((volatile UInt8 *)base + off);
}
static inline void sil_w8(volatile void *base, UInt32 off, UInt8 v)
{
    *((volatile UInt8 *)base + off) = v;
    __asm__ __volatile__("eieio");
}
static inline UInt16 sil_r16(volatile void *base, UInt32 off)
{
    volatile UInt16 *p = (volatile UInt16 *)((volatile UInt8 *)base + off);
    UInt16 v;
    __asm__ __volatile__("lhbrx %0,0,%1" : "=r"(v) : "r"(p) : "memory");
    return v;
}
static inline void sil_w16(volatile void *base, UInt32 off, UInt16 v)
{
    volatile UInt16 *p = (volatile UInt16 *)((volatile UInt8 *)base + off);
    __asm__ __volatile__("sthbrx %0,0,%1" : : "r"(v), "r"(p) : "memory");
    __asm__ __volatile__("eieio");
}
static inline UInt32 sil_r32(volatile void *base, UInt32 off)
{
    volatile UInt32 *p = (volatile UInt32 *)((volatile UInt8 *)base + off);
    UInt32 v;
    __asm__ __volatile__("lwbrx %0,0,%1" : "=r"(v) : "r"(p) : "memory");
    return v;
}
static inline void sil_w32(volatile void *base, UInt32 off, UInt32 v)
{
    volatile UInt32 *p = (volatile UInt32 *)((volatile UInt8 *)base + off);
    __asm__ __volatile__("stwbrx %0,0,%1" : : "r"(v), "r"(p) : "memory");
    __asm__ __volatile__("eieio");
}
/* host (big-endian) <-> little-endian DMA field (for PRD tables in M3) */
static inline UInt32 sil_cpu_to_le32(UInt32 v)
{
    return ((v >> 24) & 0x000000FFUL) | ((v >> 8) & 0x0000FF00UL) |
           ((v << 8) & 0x00FF0000UL) | ((v << 24) & 0xFF000000UL);
}

/* ================= this is a 2-port part ================= */
#define SIL3512_N_PORTS   2

/* Per-port register block offsets within BAR5 (ports 0,1). */
typedef struct {
    UInt32 tf;         /* ATA taskfile register block base            */
    UInt32 ctl;        /* device control / alt status                 */
    UInt32 bmdma;      /* bus-master DMA command+status (SFF-8038i)    */
    UInt32 bmdma2;     /* BMDMA2 / PRD table physical pointer          */
    UInt32 fifo_cfg;   /* FIFO configuration                          */
    UInt32 scr;        /* SATA SControl/SStatus/SError block base      */
    UInt32 sien;       /* SATA interrupt enable                       */
    UInt32 xfer_mode;  /* transfer-mode                               */
    UInt32 sfis_cfg;   /* SATA FIS config (RERR-on-DMA-act quirk here) */
} sil_port_regs;

static const sil_port_regs kSilPort[SIL3512_N_PORTS] = {
    { 0x80, 0x8A, 0x00, 0x10, 0x40, 0x100, 0x148, 0xB4, 0x14C },
    { 0xC0, 0xCA, 0x08, 0x18, 0x44, 0x180, 0x1C8, 0xF4, 0x1CC },
};

/* ATA taskfile registers, as byte offsets from a port's tf base. */
#define SIL_TF_DATA        0   /* 16-bit data port                         */
#define SIL_TF_FEATURES    1   /* write: features                          */
#define SIL_TF_ERROR       1   /* read: error                              */
#define SIL_TF_NSECT       2   /* sector count                             */
#define SIL_TF_LBAL        3
#define SIL_TF_LBAM        4
#define SIL_TF_LBAH        5
#define SIL_TF_DEVICE      6   /* device/head (LBA bit, drive select)      */
#define SIL_TF_STATUS      7   /* read: status                             */
#define SIL_TF_COMMAND     7   /* write: command                           */

/* ATA status register bits (read at tf+7 or ctl alt-status). */
#define SIL_ATA_BSY   0x80
#define SIL_ATA_DRDY  0x40
#define SIL_ATA_DF    0x20
#define SIL_ATA_DRQ   0x08
#define SIL_ATA_ERR   0x01

/* SATA SCR sub-registers, byte offsets from a port's scr base
 * (confirmed from sata_sil.c sil_scr_addr). */
#define SIL_SCR_CONTROL   0x00
#define SIL_SCR_STATUS    0x04
#define SIL_SCR_ERROR     0x08

/* SControl fields */
#define SIL_SCTL_DET_INIT   0x00000001UL   /* DET=1: perform COMRESET         */
#define SIL_SCTL_DET_NONE   0x00000000UL   /* DET=0: no action / operate      */
#define SIL_SCTL_IPM_NONE   0x00000300UL   /* IPM=3: disable partial+slumber  */
/* SStatus DET field */
#define SIL_SSTS_DET_MASK   0x0000000FUL
#define SIL_SSTS_DET_READY  0x00000003UL   /* device present + PHY comm ready */

/* Global register */
#define SIL_SYSCFG          0x48
#define SIL_MASK_IDE0_INT   (1UL << 22)    /* per-port IDE IRQ mask; <<port_no */

/* BMDMA registers (per-port, offsets within the bmdma block). Standard SFF-8038i
 * layout: command byte @+0, status byte @+2, PRD-table physical pointer @+4.
 * SiI-specific: the DMA is STARTED by writing the command byte to the separate
 * bmdma2 register (kSilPort[].bmdma2) — required for large-block transfers. */
#define SIL_BMDMA_CMD       0x00   /* command byte offset within the bmdma block */
#define SIL_BMDMA_STATUS    0x02   /* status  byte offset within the bmdma block */
#define SIL_BMDMA_PRD       0x04   /* 32-bit PRD-table physical pointer (SFF)     */
/* command-byte bits (SFF-8038i) */
#define SIL_BMDMA_START     0x01   /* start/stop the DMA engine                  */
#define SIL_BMDMA_DIR_TOMEM 0x08   /* RWCON=1 => transfer device->memory (=disk READ) */
/* status-byte bits */
#define SIL_BMDMA_ACTIVE    0x01   /* DMA in progress                            */
#define SIL_BMDMA_ERR       0x02   /* DMA error                                  */
#define SIL_BMDMA_IRQ       0x04   /* interrupt (transfer complete)              */

/* ATA commands used by the SATL. */
#define SIL_CMD_IDENTIFY    0xEC
#define SIL_CMD_READ_DMA    0xC8
#define SIL_CMD_WRITE_DMA   0xCA
#define SIL_CMD_READ_DMA_EXT  0x25
#define SIL_CMD_WRITE_DMA_EXT 0x35
#define SIL_DEV_LBA         0xE0   /* device reg: LBA mode, drive 0             */

#endif /* SIL3512_REGS_H */
