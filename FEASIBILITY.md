# Mac OS 9 driver for the LaCie 130823 eSATA PCI card, feasibility & theory

**Card:** LaCie 130823 "2-Port eSATA PCI", chipset **Silicon Image SiI3512**
("SATALink", SteelVine family), PCI vendor `0x1095`, device `0x3512`, 32-bit PCI.
**Target machine:** Power Mac G4 MDD (New World, Open Firmware 3, boots Mac OS 9.2.2).

## The three goals, and what each requires

The user wants, in order of difficulty:

1. **Boot OS 9 with the card installed** (today the card hangs boot entirely).
2. **Read drives attached to the card** from within OS 9.
3. **Boot OS 9 from a drive on the card.**

These map onto the classic anatomy of a bootable Mac PCI storage card. A card that
works in an Open Firmware Mac carries **three** software layers in its expansion-ROM
EEPROM (this is exactly what FirmTek's SeriTek cards ship):

| Layer | Runs where | Buys us |
|-------|-----------|---------|
| **Open Firmware FCode** | OF, at `probe-all` / device-tree build | Goal 1 (clean claim → no hang) + Goal 3 (`load` from disk) |
| **Classic NDRV** (PowerPC CFM driver) | Mac OS 9, loaded from the card's declaration ROM via the Name Registry | Goal 2 (drives visible/mountable) + hands off from OF for Goal 3 |
| **OS X kext** | macOS | irrelevant here |

Goal 2 needs the NDRV. Goals 1 and 3 need the FCode. So the NDRV is on the critical
path for everything and is where we start; the FCode is a separate deliverable that
also has a hardware prerequisite (see "What only hardware can tell us").

## Feasibility verdict: YES, with one honest caveat

### Why it's feasible, register compatibility is confirmed

The SiI3512 is **not** a new programming model. Its datasheet is subtitled *"Based on
the proven architecture of the industry-leading SiI 3112."* The decisive proof is
Linux: a **single** driver, `drivers/ata/sata_sil.c`, drives `0x3112`, `0x3512`, and
`0x3114` off an **identical** register map. The only 3512-specific difference is one
errata quirk flag (`SIL_FLAG_RERR_ON_DMA_ACT`, assert R_ERR on DMA-activate FIS).
Taskfile, BMDMA/scatter-gather, and SATA SControl/SStatus offsets are all shared.
See `docs/SIL3512-REGISTERS.md`.

The **SiI3112 is a proven OS 9 boot device**: FirmTek SeriTek/1S2 (and Sonnet Tempo
SATA) cards use it, and dosdude1's community-flashed 3112 cards boot OS 9 today. So the
hardware programming model we must target has a known-good OS 9 reference implementation.

### The caveat, why nobody has done the 3512 before, and why that's not silicon

Community attempts to make 3512 cards boot OS 9 have failed, but the evidence points
to a **software lockout, not a hardware wall**:

- SeriTek's OS 9 NDRV and FCode **gate on PCI device ID `0x3112`** and further check
  approved EEPROM part numbers (dosdude1 reverse-engineered and *removed* the EEPROM-ID
  check: it looked for AM29LV040 `01 4F`, MX29LV040 `C2 4F`, PM39LV040 `9D 3E`). A
  binary keyed to `0x3112` simply won't bind to a `0x3512` card, that's a `if (id !=
  0x3112) reject`, not a missing capability.
- The one reported "tried the 3512, won't boot" result was flashing **unmodified
  SeriTek firmware**, i.e. hitting exactly that device-ID gate.

Because we're **authoring** the driver (and, for Goal 3, the FCode) against the *shared*
register interface and keying it to `0x3512` + the `RERR_ON_DMA_ACT` quirk, we route
around the lockout. Linux proves the silicon is fully drivable; we're porting that
knowledge to the OS 9 / Open Firmware driver model rather than reusing a locked binary.

**Residual risk** (the honest part): there could be a *second*, board-level reason the
LaCie card misbehaves at OF time (its stock PC option-ROM, an EEPROM that's too small or
not writable, or a PCB electrical quirk, documented even among "identical-chip" 3112
boards). None of these are driver-logic problems; they're addressed at the FCode/flash
stage and can only be fully characterised on the hardware.

## Why the card hangs OS 9 boot today (hypothesis, to confirm on hardware)

On a New World Mac, Open Firmware builds the device tree at `probe-all`: for each PCI
function it looks for an expansion-ROM image with an Open Firmware code type (PCI Data
Structure `code-type 1`). The LaCie card ships a **PC/x86 option ROM** (`code-type 0`),
which OF should skip, synthesising a generic node from the config header instead. A bare
storage node with no matching driver is normally harmless, so a hard hang implies one of:

- OF chokes evaluating/inventorying the stock option ROM (bad checksum, partial match);
- the Mac OS ROM's PCI/Slot-Manager scan or interrupt setup wedges on the device
  (compare the ATY,Bee VBL-interrupt hang we already root-caused on this platform);
- an electrical/PCI-protocol interaction specific to this board + MDD.

The robust, community-proven fix for all of these is the same: put a **proper Mac FCode**
on the card that cleanly claims device `1095,3512`, initialises its properties, and (if
needed) disables the stock option-ROM decode. That's precisely why flashed cards stop
hanging. **We must observe where it actually hangs before committing the FCode design.**

## Recommended architecture

- **Driver family: SCSI Manager 4.3 HBA/SIM**, not an ATA Interface Module. FirmTek's
  cards "function as SCSI buses in vintage Macs," and the SCSI Manager 4.3 SIM/HBA plug-in
  API is well documented (Inside Macintosh: Devices). Presenting each SATA port as a SCSI
  target means Drive Setup, volume mounting, and Startup Disk all work unchanged. The NDRV
  contains a **SCSI-to-ATA translation layer (SATL)**: SCSI CDBs in → ATA taskfile/DMA
  commands out. (ATA-Interface-Module is the alternative; kept as a fallback.)
- **Build pipeline: reuse the proven `../usb2-ehci` Retro68 flow**, `add_library(SHARED)`
  → `MakePEF` → `Rez` wrap as file type `ndrv`, exporting the two data symbols
  (`TheDriverDescription`, and our HBA entry table) with a `cfrg`. That project already
  demonstrates a real PCI bus-master NDRV in Retro68 with PCILib/DriverServices imports,
  wired DMA (`NewPtrSysClear`+`LockMemory`+`GetPhysical`), and byte-swapped MMIO
  (`lwbrx`/`stwbrx`), a SATA HBA is the same class of driver. **This supersedes the old
  "NDRVs need CodeWarrior" assumption.**
- **FCode:** authored in Forth/FCode, tokenised, placed in the card EEPROM alongside the
  (LZSS-compressed, per SeriTek) NDRV. SeriTek's ROM layout is the template.

## What only the hardware can tell us (gates the FCode/flash work)

1. **Exact boot-hang symptom:** no chime? chime → hang at gray screen (OF)? hang at the
   Happy Mac / progress bar (OS)? Can you drop into OF (Cmd-Opt-O-F) *with the card in*,
   and does `dev /pci` + `.properties` show the node? This localises goal 1.
2. **The card's EEPROM:** part number, size (SeriTek needs ≥128 KB compressed / 512 KB
   full), socketed vs soldered, and low-voltage writable. This determines whether the
   FCode+NDRV can be flashed in place, needs a programmer, or needs an EEPROM swap.

## Milestone plan

- **M0**, this doc + `docs/SIL3512-REGISTERS.md` register reference. ✅
- **M1**, shape-correct NDRV binding to `pci1095,3512`, SCSI HBA skeleton (stubs).
- **M2**, controller bring-up: PCI enable, BAR5 map, global + per-port reset, SATA
  SControl/SStatus link-up, 3512 quirk.
- **M3**, command engine (taskfile PIO + BMDMA scatter-gather) and the SCSI→ATA SATL:
  IDENTIFY, READ/WRITE(10), INQUIRY, READ CAPACITY → drives mount in OS 9.
- **M4**, Open Firmware FCode: clean device claim (goal 1) + `block` device methods
  (`open`/`read-blocks`/`seek`) for boot (goal 3). Gated on hardware recon #2.
- **M5**, hardware bring-up on the MDD + LaCie card (recon, install/flash, verify).

## Sources

- LaCie 130823 = SiI3512: [Silicon Image SiI3512 (theretroweb)](https://theretroweb.com/chips/4440),
  [B&H product page](https://www.bhphotovideo.com/c/product/594414-REG/LaCie_130823_2_Port_eSATA_PCI_Adapter.html)
- Which SiI chips boot OS 9 (3112 yes, 3512 failed, 3124 OSX-only):
  [macos9lives forum](http://macos9lives.com/smforum/index.php?topic=4967.0)
- SeriTek firmware layers (FCode + NDRV + OSX driver), device-ID & EEPROM-ID lockout,
  dosdude1 RE/patch:
  [TinkerDifferent flashing guide](https://tinkerdifferent.com/threads/flashing-the-silicon-image-sil3112-to-work-in-macs-2025-edition.1494/)
- Register compatibility (one driver, IDs 3112/3512/3114; offsets; 3512 quirk):
  Linux [`drivers/ata/sata_sil.c`](https://raw.githubusercontent.com/torvalds/linux/master/drivers/ata/sata_sil.c)
- SiI3512 datasheet ("based on the proven architecture of the SiI 3112"):
  [bitsavers SiI-DS-0102-D](https://www.bitsavers.org/components/siliconImage/datasheet/SiI-DS-0102-D_SiI3512.pdf)
