/*
 * sil3512_hw.c — SiI3512 controller bring-up (Milestone 2), OS-independent.
 *
 * Mirrors the init that Linux sata_sil.c performs (sil_init_controller +
 * libata SATA link-up), translated to our register helpers:
 *   - per-port FIFO config from the PCI cache-line size
 *   - the RERR-on-DMA-activate errata fix (the 3512/3114 quirk)
 *   - SATA link bring-up: COMRESET via SControl, wait for SStatus DET==3
 *   - clear SError, then unmask the port's IDE interrupt in SYSCFG
 *
 * Runtime behavior is untested until hardware bring-up (M5) — same posture as
 * the EHCI M2 core. What this milestone guarantees: correct register
 * programming and a clean compile/link against the real OS imports.
 */
#include "sil3512.h"

UInt32 sil_scr_read(sil_softc *sc, int port, UInt32 scrOff)
{
    return sil_r32(sc->bar5, kSilPort[port].scr + scrOff);
}
void sil_scr_write(sil_softc *sc, int port, UInt32 scrOff, UInt32 val)
{
    sil_w32(sc->bar5, kSilPort[port].scr + scrOff, val);
}

/*
 * Bring up one SATA port's PHY link.
 * Sequence (SATA SControl/SStatus, per AHCI/libata convention):
 *   1. assert COMRESET (SControl DET=1, IPM=disable partial+slumber)
 *   2. hold ~2 ms, then release (DET=0)
 *   3. poll SStatus until DET==3 (device present + PHY comm established)
 * Returns 1 if a device is present and ready, 0 if the port is empty.
 */
int sil_port_linkup(sil_softc *sc, int port)
{
    UInt32 ssts;
    int tries;

    sil_scr_write(sc, port, SIL_SCR_CONTROL, SIL_SCTL_IPM_NONE | SIL_SCTL_DET_INIT);
    sil_delay_ms(2);
    sil_scr_write(sc, port, SIL_SCR_CONTROL, SIL_SCTL_IPM_NONE | SIL_SCTL_DET_NONE);

    /* Allow up to ~100 ms for the PHY to establish (spun-up drives can be slow). */
    for (tries = 0; tries < 100; tries++) {
        ssts = sil_scr_read(sc, port, SIL_SCR_STATUS);
        if ((ssts & SIL_SSTS_DET_MASK) == SIL_SSTS_DET_READY) {
            sil_scr_write(sc, port, SIL_SCR_ERROR, 0xFFFFFFFFUL);  /* clear SError */
            return 1;
        }
        sil_delay_ms(1);
    }
    return 0;
}

/* Apply the 3512 R_ERR-on-DMA-activate FIS errata fix on a port
 * (sata_sil.c: if (sfis_cfg & 3) == 1, clear the low two bits). */
static void sil_apply_rerr_quirk(sil_softc *sc, int port)
{
    UInt32 tmp = sil_r32(sc->bar5, kSilPort[port].sfis_cfg);
    if ((tmp & 0x3) == 0x1)
        sil_w32(sc->bar5, kSilPort[port].sfis_cfg, tmp & ~0x3UL);
}

/* Program per-port FIFO arbitration from the PCI cache-line size register
 * (sata_sil.c: raw = PCI_CACHE_LINE_SIZE reg; cls = (raw>>3)+1; writew((cls<<8)|cls)). */
static void sil_program_fifo(sil_softc *sc, int port)
{
    UInt8 clsRaw = sil_pci_cacheline();      /* raw register, in 32-bit dwords */
    if (clsRaw) {
        UInt32 cls = (UInt32)(clsRaw >> 3) + 1;
        sil_w16(sc->bar5, kSilPort[port].fifo_cfg, (UInt16)((cls << 8) | cls));
    }
}

/* Unmask a port's IDE interrupt in the global SYSCFG register. */
static void sil_unmask_irq(sil_softc *sc, int port)
{
    UInt32 cfg = sil_r32(sc->bar5, SIL_SYSCFG);
    cfg &= ~(SIL_MASK_IDE0_INT << port);
    sil_w32(sc->bar5, SIL_SYSCFG, cfg);
}

OSStatus sil_hc_init(sil_softc *sc)
{
    int port;

    sc->nPorts = SIL3512_N_PORTS;
    for (port = 0; port < SIL3512_N_PORTS; port++) {
        sil_program_fifo(sc, port);
        sil_apply_rerr_quirk(sc, port);
        sc->port[port].present = (UInt8)sil_port_linkup(sc, port);
        if (sc->port[port].present)
            sil_unmask_irq(sc, port);
        /* IDENTIFY + capacity/model come in M3 (command engine). */
    }
    return 0;
}
