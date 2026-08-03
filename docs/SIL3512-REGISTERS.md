# SiI3512 register reference (for the OS 9 NDRV)

Programming model shared with the SiI3112/3114, verified from Linux
`drivers/ata/sata_sil.c` (one driver, IDs 3112/3512/3114) and the SiI3512 datasheet
(bitsavers SiI-DS-0102-D). All MMIO lives in the **BAR5** memory window. This card is a
2-port part → ports 0 and 1 only. Registers are **little-endian**; on the big-endian
PPC use byte-reversing access (`lwbrx`/`stwbrx`, as in `../usb2-ehci/src/ehci_regs.h`).

## PCI identity
- Vendor `0x1095` (Silicon Image / CMD), Device `0x3512`.
- BARs: BAR0–4 are the legacy IDE-style taskfile/BMDMA decode; **BAR5** is the unified
  memory-mapped register block we use. Enable via PCI Command = `0x0006`
  (MemorySpace | BusMaster), `ExpMgrConfigWriteWord(node, 0x04, 0x0006)`.
- Class: mass-storage. (Confirm the exact class-code the LaCie ROM presents when we read
  the node's properties on hardware, it affects the OF node name / our match string.)

## BAR5 offsets (bytes)

### Per-port blocks (port 0, port 1)
| Register            | Port 0 | Port 1 | Notes |
|---------------------|--------|--------|-------|
| Taskfile base       | 0x80   | 0xC0   | ATA taskfile (data/features/count/LBA/device/cmd) |
| Control / AltStatus | 0x8A   | 0xCA   | device control + alt status |
| BMDMA cmd/status    | 0x00   | 0x08   | bus-master DMA command+status (SFF-8038i style) |
| BMDMA2 / PRD ptr    | 0x10   | 0x18   | scatter-gather PRD table phys pointer |
| FIFO config         | 0x40   | 0x44   | |
| Xfer mode           | 0xB4   | 0xF4   | |
| SCR base (SControl/SStatus/SError) | 0x100 | 0x180 | SATA superset registers |
| SIEN (SATA IRQ en)  | 0x148  | 0x1C8  | |
| SFIS config         | 0x14C  | 0x1CC  | |

(Ports 2/3 exist on the 3114 at 0x280/0x2C0 etc., not present on this 2-port 3512.)

### Global
| Register     | Offset | Notes |
|--------------|--------|-------|
| SIL_SYSCFG   | 0x48   | per-port IDE interrupt mask bits at 22–25 |

### SCR sub-registers (relative to each port's SCR base)
Standard SATA superset ordering: `SStatus` (link/PHY status, DET field = device present),
`SError`, `SControl` (DET/SPD/IPM). Bring-up sets SControl to trigger COMRESET, waits for
`SStatus.DET == 3` (device present + PHY ready), then clears `SError`.

## The 3512 quirk (vs 3112)
- `SIL_FLAG_RERR_ON_DMA_ACT`: the 3512/3114 need the "assert R_ERR on DMA-activate FIS"
  errata workaround. Program the per-port SFIS/pattern setup accordingly at init.
- The 3112's `SIL_FLAG_MOD15WRITE` (limit certain Seagate writes to 15 sectors) does
  **not** apply to the 3512.

## Command flow (what M2/M3 implement)
1. **Bring-up:** enable PCI (cmd=0x0006), map BAR5, global reset, per-port SATA link-up
   (SControl COMRESET → wait DET==3 → clear SError), apply 3512 quirk, unmask IRQs.
2. **IDENTIFY:** issue ATA `IDENTIFY DEVICE` (0xEC) via the taskfile; read 512-byte id
   block (model, LBA48 support, capacity) to build the SCSI INQUIRY / READ CAPACITY reply.
3. **Read/Write:** build a PRD scatter-gather list of the client buffer's physical pages,
   write PRD phys to BMDMA2, set direction + start in BMDMA cmd, issue ATA
   `READ/WRITE DMA (EXT)`; completion via port IRQ → BMDMA status.
4. **SATL:** translate SCSI CDBs (INQUIRY, TEST UNIT READY, READ CAPACITY(10), READ(10)/
   WRITE(10), REQUEST SENSE) to the above so OS 9's SCSI Manager mounts the volumes.

## Cross-references in this repo
- MMIO byte-swap helpers + DMA idiom: `../usb2-ehci/src/ehci_regs.h`, `../usb2-ehci/src/ehci_os.c`.
- PCI-enable / register-base-mapping template: `../usb2-ehci/M2-GLUE-SPEC.md`.
