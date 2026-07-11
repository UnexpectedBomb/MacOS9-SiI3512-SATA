# SiI3512 SATA — Mac OS 9 driver for the LaCie 130823 eSATA PCI card

A native PowerPC driver (`ndrv`) — plus, for boot support, an Open Firmware FCode ROM —
that lets a Power Mac G4 MDD **boot with**, **read drives from**, and eventually **boot
from** a LaCie 130823 (Silicon Image **SiI3512**) eSATA PCI card under Mac OS 9.2.2.

Full theory of operation, feasibility verdict, and sources: **[FEASIBILITY.md](FEASIBILITY.md)**.
Register map: **[docs/SIL3512-REGISTERS.md](docs/SIL3512-REGISTERS.md)**.

**Short version:** feasible. The SiI3512 is register-compatible with the SiI3112 (Linux
`sata_sil.c` drives both from one map; 3512 differs only by one DMA-errata quirk), and the
3112 is a proven OS 9 boot device (FirmTek SeriTek). The reason no 3512 boots OS 9 today is
SeriTek's **device-ID software lockout** (it binds only to `0x3112`), not the silicon. We
author our own driver keyed to `0x3512`, routing around the lockout.

## Status

- **M0 — feasibility + register reference — DONE.** See the two docs above.
- **M1 — NDRV container shape — DONE.** Builds `SiI3512SATA.ndrv`: a CFM `ndrv` exporting
  `TheDriverDescription` (matched to Name Registry node `pci1095,3512`) and `DoDriverIO`
  (Initialize/Open/Close/Read/Write/Control/Status dispatch). All ops are stubs. Validated
  against the EHCI RE tools: 2 exports, 0 imports, `cfrg` fragment `SiI3512SATA`, PowerPC.
- **M2 — controller bring-up — DONE (compiles/links, real OS imports).**
  `src/sil3512_regs.h` (full BAR5 map + byte-reversed MMIO helpers, per-port offset table),
  `src/sil3512_hw.c` (OS-independent: per-port FIFO config, `RERR_ON_DMA_ACT` errata fix,
  SATA link-up = COMRESET via SControl → poll SStatus `DET==3` → clear SError → unmask IRQ),
  `src/sil3512_os.c` (`sil_os_initialize`: extract node from DriverInitInfo → PCI enable
  Mem+BusMaster → map **BAR5** by matching config-register `0x24` in `assigned-addresses` →
  `sil_hc_init`). PEF verified: imports `ExpMgrConfig*`/`RegistryPropertyGet`/`DelayForHardware`/
  `UpTime` across 3 import libs; still 2 clean exports. **Runtime untested until M5** (same
  posture as EHCI M2).
- **M3 — command engine + SCSI-to-ATA (SATL) — DONE (compiles/links, real OS imports).**
  `src/sil3512_ata.c` (OS-independent ATA engine: IDENTIFY via PIO with model/LBA48/capacity
  parse; READ/WRITE via BMDMA against a PRD table; polled completion). `src/sil3512_os.c`
  gains the wired PRD pool + `sil_dma_prepare`/`sil_dma_complete` (LockMemory→GetPhysical→
  build SFF-8038i PRD entries, EOT on the last). `src/sil3512_scsi.c` registers a **SCSI
  Manager 4.3 SIM** (`SCSIRegisterBus` + `SIMAction` UPP) and translates the core CDBs —
  TEST UNIT READY, INQUIRY, READ CAPACITY(10), READ(10)/WRITE(10), REQUEST SENSE — to ATA
  ops so present ports appear as SCSI targets and Drive Setup mounts them. PEF verified:
  imports `SCSIRegisterBus`/`NewRoutineDescriptor`/`LockMemory`/`GetPhysical` (4 libs, 17
  syms); still 2 exports. **Runtime untested until M5.**
  - **Large-volume support (by intent).** Uses LBA48 EXT commands whenever a drive reports
    LBA48, carrying the full 32-bit LBA — so the usable range is **up to 2 TB** (Mac OS 9's
    own 32-bit block / HFS+ ceiling), not the 137 GB LBA28 wall. The SATL chunks READ/WRITE(10)
    to fit the PRD table (`sil_dma_max_sectors`), so total request size is unbounded — only
    per-command size is capped (256 sectors LBA28 / PRD-limited LBA48). Drives physically
    > 2 TB are detected and clamped, and READ CAPACITY(10) reports `0xFFFFFFFF` per spec.
  - Known boundaries: polled (not interrupt-driven) completion; single shared PRD table
    (fine for the synchronous path); no > 2 TB addressing (OS-imposed, not fixable card-side);
    ATAPI not handled.
- **M5 — hardware bring-up — IN PROGRESS.** Probe/loader app built (`probe/`). See below.
- **M4 — Open Firmware FCode — DEFERRED (opt-in, last).** Hardware recon (2026-07-02)
  showed the old "card prevents boot" was a **dual-boot (OS X) artifact**, not an OS 9
  problem: with the Leopard disk gone the MDD boots OS 9 to the desktop with the card
  installed (ASP sees it in SLOT-3 as node `SunrichSATA3512`, `device_type ata`, vendor 1095).
  So FCode is needed **only for goal 3 (boot *from* the card)**. The flash is a **soldered**
  29LV040A → in-place flash only (riskier); do this only if boot-from-card is wanted.

## Probe / hardware validation (`probe/`)

`probe/sil_probe_app.c` is a double-clickable PowerPC app that validates the driver on real
hardware (Retro68 can't build an auto-loading PPC INIT, so an app is the validation path —
and the safe way to make first contact). It reuses the real driver sources and runs in
staged order, printing before each step so a hang/fault localizes itself:

1. find the card in the Name Registry **by vendor/device ID 1095/3512** (not by name, since
   the Sunrich FCode names it `SunrichSATA3512`);
2. PCI enable + BAR5 map;
3. first MMIO read (SYSCFG) — proves the mapping before any writes;
4. controller bring-up (reset + SATA link-up), prints per-port SStatus / presence;
5. IDENTIFY (model, LBA48, capacity);
6. real DMA read of LBA 0 + partition-map signature check.

v1 stops before registering a SCSI bus — it proves bring-up + IDENTIFY + the DMA read path.
v2 adds SCSI-SIM registration so drives mount in the Finder.

```
cmake -S probe -B probe/build \
  -DCMAKE_TOOLCHAIN_FILE=~/Retro68-build/toolchain/powerpc-apple-macos/cmake/retroppc.toolchain.cmake
cmake --build probe/build
# -> probe/build/SiI3512Probe.img  (APM-wrapped, mounts on OS 9)  + .bin (MacBinary)
```
Attach a formatted SATA drive to card port 0 before running.

## Build

```
cmake -S . -B build \
  -DCMAKE_TOOLCHAIN_FILE=~/Retro68-build/toolchain/powerpc-apple-macos/cmake/retroppc.toolchain.cmake
cmake --build build
# -> build/SiI3512SATA.ndrv   (file type 'ndrv'; PEF in data fork, cfrg in resource fork)
```

## Validate (closed loop against the EHCI RE tools)

```
T=../usb2-feasibility/tools
python3 $T/pefdump.py  build/SiI3512SATA.pef                          # 2 exports, 0 imports
python3 $T/cfrgdump.py <(cat build/SiI3512SATA.ndrv/..namedfork/rsrc) # fragment "SiI3512SATA"
```

## Two hardware facts we need before M4/M5 (only you can get these)

1. **Where does it hang** with the card installed? No chime / hang at gray screen (OF) /
   hang at Happy Mac (OS)? Can you enter Open Firmware (Cmd-Opt-O-F) with the card in, and
   does `dev /pci` + `.properties` list the node?
2. **The card's EEPROM:** part number, size, socketed vs soldered, low-voltage writable —
   this decides how (and whether in place) we flash the FCode+NDRV.

## Layout
- `src/sil3512_driver.c` — `TheDriverDescription` (`pci1095,3512`) + `DoDriverIO` dispatch
- `src/sil3512_driver.exp` — exported symbols
- `sil3512.r` — `cfrg` (fragment `SiI3512SATA`)
- `CMakeLists.txt` — SHARED → MakePEF → Rez wrap as `ndrv`
- `docs/SIL3512-REGISTERS.md` — register reference (from `sata_sil.c` + datasheet)
