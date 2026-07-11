/*
 * sil3512_ata.c — ATA command engine (Milestone 3), OS-independent.
 *
 * Drives the per-port taskfile + BMDMA registers (offsets in sil3512_regs.h):
 *   - IDENTIFY DEVICE via PIO (to learn model, LBA48, capacity)
 *   - READ/WRITE via bus-master DMA against a caller-supplied PRD table
 *
 * The SCSI-to-ATA translation layer (sil3512_scsi.c) calls these; the PRD table
 * and buffer wiring are built by the OS layer (sil3512_os.c), which owns
 * LockMemory/GetPhysical. Runtime is untested until hardware bring-up (M5);
 * this milestone delivers the register-level logic and a clean compile.
 *
 * NOTE(hw): the whole engine here is polled. Interrupt-driven completion (using
 * the per-port IRQ unmasked in M2) is the M4/M5 refinement; polling keeps M3
 * self-contained and verifiable step by step on hardware.
 */
#include "sil3512.h"

/* taskfile byte access relative to a port's tf base */
static UInt8 tf_r8(sil_softc *sc, int port, UInt32 reg)
{
    return sil_r8(sc->bar5, kSilPort[port].tf + reg);
}
static void tf_w8(sil_softc *sc, int port, UInt32 reg, UInt8 v)
{
    sil_w8(sc->bar5, kSilPort[port].tf + reg, v);
}

/* Spin until BSY clears (or timeout). Returns 1 on ready, 0 on timeout. */
static int wait_not_busy(sil_softc *sc, int port, UInt32 ms)
{
    UInt32 i;
    for (i = 0; i < ms; i++) {
        if (!(tf_r8(sc, port, SIL_TF_STATUS) & SIL_ATA_BSY)) return 1;
        sil_delay_ms(1);
    }
    return 0;
}

/* Spin until DRQ sets with BSY clear (data ready), or timeout. */
static int wait_drq(sil_softc *sc, int port, UInt32 ms)
{
    UInt32 i;
    for (i = 0; i < ms; i++) {
        UInt8 st = tf_r8(sc, port, SIL_TF_STATUS);
        if (st & SIL_ATA_ERR) return 0;
        if (!(st & SIL_ATA_BSY) && (st & SIL_ATA_DRQ)) return 1;
        sil_delay_ms(1);
    }
    return 0;
}

/* IDENTIFY DEVICE (PIO). Fills id256 with 256 16-bit words. Returns 1 on ok. */
int sil_ata_identify(sil_softc *sc, int port, UInt16 *id256)
{
    int i;

    tf_w8(sc, port, SIL_TF_DEVICE, SIL_DEV_LBA);      /* select drive 0, LBA   */
    if (!wait_not_busy(sc, port, 500)) return 0;
    tf_w8(sc, port, SIL_TF_COMMAND, SIL_CMD_IDENTIFY);
    if (!wait_drq(sc, port, 1000)) return 0;          /* spin-up can be slow   */

    for (i = 0; i < 256; i++)
        id256[i] = sil_r16(sc->bar5, kSilPort[port].tf + SIL_TF_DATA);
    return 1;
}

/* Extract an ATA text field (byte-swapped within each 16-bit word) into dst,
 * trimming trailing spaces and NUL-terminating. wordCount words -> 2*wordCount chars.
 * Exported so the probe app can also pull serial (words 10..19) and firmware
 * revision (words 23..26) for on-hardware read-path verification. */
void sil_ata_string(char *dst, const UInt16 *id, int firstWord, int wordCount)
{
    int i, n = 0;
    for (i = 0; i < wordCount; i++) {
        UInt16 w = id[firstWord + i];
        dst[n++] = (char)(w >> 8);      /* ATA stores the first char in the high byte */
        dst[n++] = (char)(w & 0xFF);
    }
    while (n > 0 && (dst[n - 1] == ' ' || dst[n - 1] == 0)) n--;
    dst[n] = 0;
}

/* Parse the fields the SATL needs out of an IDENTIFY block.
 *
 * Capacity is tracked as a 32-bit sector count, which spans the full range Mac
 * OS 9 can address (2^32 * 512 B = 2 TB — the OS's own block/HFS+ ceiling, not
 * ours). A drive physically larger than 2 TB (nonzero bits 32..47 of the 48-bit
 * count) is clamped and flagged oversize so READ CAPACITY reports the ceiling
 * rather than a wrapped, wrong value. */
void sil_ata_parse_identify(sil_port_state *ps, const UInt16 *id)
{
    sil_ata_string(ps->model, id, 27, 20);            /* words 27..46 = model  */
    ps->lba48 = (id[83] & (1 << 10)) ? 1 : 0;         /* word 83 bit 10        */
    ps->oversize = 0;
    if (ps->lba48) {
        UInt32 lo = ((UInt32)id[101] << 16) | id[100];   /* count bits  0..31  */
        UInt32 hi = ((UInt32)id[103] << 16) | id[102];   /* count bits 32..47  */
        if (hi != 0) {                                /* physically > 2 TB      */
            ps->nSectors = 0xFFFFFFFFUL;
            ps->oversize = 1;
        } else {
            ps->nSectors = lo;
        }
    } else {
        ps->nSectors = ((UInt32)id[61] << 16) | id[60];   /* words 60..61      */
    }
}

/*
 * READ/WRITE via bus-master DMA. prdPhys points at a wired PRD table describing
 * the client buffer; count is in 512-byte sectors. Returns 1 on success.
 *
 * Direction (SFF-8038i RWCON): a disk READ moves device->memory, so the bus
 * master WRITES memory => set SIL_BMDMA_DIR_TOMEM. A disk WRITE clears it.
 */
int sil_ata_rw_dma(sil_softc *sc, int port, UInt32 lba, UInt16 count,
                   UInt32 prdPhys, int isWrite)
{
    UInt32 bm  = kSilPort[port].bmdma;
    UInt32 bm2 = kSilPort[port].bmdma2;
    UInt8  dir = (UInt8)(isWrite ? 0 : SIL_BMDMA_DIR_TOMEM);   /* read => write memory */
    UInt8  st;

    if (!wait_not_busy(sc, port, 500)) return 0;

    /* PRD table pointer at bmdma+4 (SFF), written NATIVE — sil_w32 already
     * byte-reverses to little-endian, so do NOT pre-swap. Then set direction in
     * the command byte (no start yet) and clear stale status (ERR|IRQ are W1C). */
    sil_w32(sc->bar5, bm + SIL_BMDMA_PRD, prdPhys);
    sil_w8(sc->bar5, bm + SIL_BMDMA_CMD, dir);
    st = sil_r8(sc->bar5, bm + SIL_BMDMA_STATUS);
    sil_w8(sc->bar5, bm + SIL_BMDMA_STATUS, st | SIL_BMDMA_ERR | SIL_BMDMA_IRQ);

    /* taskfile: LBA + sector count. LBA48 uses the two-write "HOB" scheme —
     * the first write to each register latches the high half. A 32-bit LBA
     * (all Mac OS 9 can address, up to 2 TB) lives entirely in bits 0..31, so
     * the high LBA bytes (bits 32..47) are legitimately zero here. */
    if (sc->port[port].lba48) {
        tf_w8(sc, port, SIL_TF_NSECT, (UInt8)(count >> 8));   /* count bits 8..15 */
        tf_w8(sc, port, SIL_TF_LBAL,  (UInt8)(lba >> 24));    /* LBA bits 24..31  */
        tf_w8(sc, port, SIL_TF_LBAM,  0);                     /* LBA bits 32..39  */
        tf_w8(sc, port, SIL_TF_LBAH,  0);                     /* LBA bits 40..47  */
    }
    tf_w8(sc, port, SIL_TF_NSECT, (UInt8)count);
    tf_w8(sc, port, SIL_TF_LBAL,  (UInt8)(lba));
    tf_w8(sc, port, SIL_TF_LBAM,  (UInt8)(lba >> 8));
    tf_w8(sc, port, SIL_TF_LBAH,  (UInt8)(lba >> 16));
    tf_w8(sc, port, SIL_TF_DEVICE,
          (UInt8)(SIL_DEV_LBA | (sc->port[port].lba48 ? 0 : ((lba >> 24) & 0x0F))));

    tf_w8(sc, port, SIL_TF_COMMAND,
          sc->port[port].lba48
              ? (isWrite ? SIL_CMD_WRITE_DMA_EXT : SIL_CMD_READ_DMA_EXT)
              : (isWrite ? SIL_CMD_WRITE_DMA     : SIL_CMD_READ_DMA));

    /* Kick the DMA engine. SiI-specific: START must be written to the bmdma2
     * register (not the standard command byte) or large-block transfers fail. */
    sil_w8(sc->bar5, bm2, (UInt8)(dir | SIL_BMDMA_START));

    /* poll for completion (interrupt-driven path is a later refinement) */
    {
        UInt32 i;
        for (i = 0; i < 400; i++) {                    /* ~7s cap (Delay=16.7ms/tick) */
            st = sil_r8(sc->bar5, bm + SIL_BMDMA_STATUS);
            if (!(st & SIL_BMDMA_ACTIVE)) break;
            sil_delay_ms(1);
        }
    }

    /* stop the engine (clear START on both), then check BMDMA + ATA error status */
    sil_w8(sc->bar5, bm2, 0);
    sil_w8(sc->bar5, bm + SIL_BMDMA_CMD, 0);
    st = sil_r8(sc->bar5, bm + SIL_BMDMA_STATUS);
    if (st & SIL_BMDMA_ERR) return 0;
    if (tf_r8(sc, port, SIL_TF_STATUS) & (SIL_ATA_ERR | SIL_ATA_DF)) return 0;
    return 1;
}
