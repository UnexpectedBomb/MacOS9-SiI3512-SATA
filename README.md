# SiI3512 SATA, a from-scratch Mac OS 9 driver for the LaCie 130823 eSATA card

A native PowerPC `ndrv` for the Silicon Image **SiI3512** 2‑port SATA controller (the LaCie 130823 eSATA PCI card) on a Power Mac G4 MDD, Mac OS 9.2.2, written from scratch with Retro68 (no CodeWarrior). The eventual goal is to **boot from** an eSATA drive; the near‑term goal is to **read and mount** drives under OS 9.

> ### Status (July 2026): the driver core works on real hardware, the OS 9 volume *mount* is an open wall.
> Every low‑level operation is hardware‑validated and repeatable: PCI bring‑up, SATA link, `IDENTIFY`, bus‑master DMA read **and** write, reading the partition map and HFS+ header. The one thing that doesn't work is the final handoff to OS 9's File Manager to mount the volume on the desktop, it trips a PCI memory‑decode fault we've traced but not yet defeated.
>
> **The full technical story, the elimination trail, and the research conclusions are in [docs/FINDINGS.md](docs/FINDINGS.md).**

## What works, validated on real hardware, every run

- PCI enable (Command = Memory‑Space + Bus‑Master); **BAR5** mapped from `assigned-addresses` / `AAPL,address`
- SATA COMRESET link‑up; `IDENTIFY` (model, LBA48, capacity, verified against a real drive)
- **Bus‑master DMA reads _and_ writes**, byte‑verified against known on‑disk data (LBA48 EXT commands; usable to OS 9's 2 TB ceiling)
- Reading the **Apple Partition Map** and the **HFS+ volume header (MDB)**
- Installing into the Device Manager Unit Table (`InstallDriverFromMemory`); `kInitialize` scans the APM and `AddDrive`s the HFS partition
- Partition‑offset block I/O through `DoDriverIO` (kRead/kWrite), single‑ and multi‑block

The driver's `kInitialize` does a full partition scan (several DMA reads + taskfile polls) and reads the volume header correctly **every boot.**

## The open wall, the mount read

The instant OS 9's File Manager mounts the volume (`PBMountVol` → Device Manager dispatches `DoDriverIO` kRead), the first MMIO read of the ATA taskfile status register (`BAR5 + 0x87`) raises a **PowerPC access exception (bus error)**, and the *entire* BAR5 memory window goes dead, even MacsBug can't read it. Yet at that instant PCI **config space is perfectly healthy**: Command still shows Memory‑Space + Bus‑Master, BAR5 is correct, PMCSR = **D0** (not D3). So: **config alive, memory decode dead**, and only on the File‑Manager mount read, never on the byte‑identical driver‑init scan reads microseconds earlier.

**Ruled out** (evidence in [docs/FINDINGS.md](docs/FINDINGS.md)): our code (an older known‑good build fails identically), a literal D3 power‑down (PMCSR = D0), SATA link power management, the launch vehicle (INIT, faceless Startup‑Items app, foreground app all fail the same), timing/settle, wrong port, and physical (cold power‑cycle + reseat).

## What the research concluded

A multi‑source deep‑dive with adversarial verification (Apple's *Designing PCI Cards and Drivers*, the SiI3512 datasheet, Linux `sata_sil.c`, and the macos9lives/68kMLA community record) found:

- The decode‑death is a **PCI‑target‑level** phenomenon (the memory window being switched off), **not** power management.
- `InstallDriverFromMemory` installs the driver but does **not open/claim** the device, so the card is never marked "in use." Every SiI3112 card that mounts reliably under OS 9 is one the OS **claims as a SCSI bus** via a SIM in the card's ROM. Leading (unproven) hypothesis: the OS closes our unclaimed card's memory window at mount time.
- **Reliable OS 9 operation on these chips comes from FCode + a driver in the card's declaration ROM**, and the **SiI3512 specifically has zero documented OS 9 success by anyone, by any means**, only the SiI3112 is proven, and only via FirmTek's SeriTek ROM. This is unmapped territory on this exact chip.
- Not categorically impossible: a software‑only driver *has* mounted (non‑boot) data disks on the SiI3112/3114 with no card ROM.

## Where this goes next

1. **Community input**, a question for the macos9lives forum is drafted at [docs/MACOS9LIVES-POST.md](docs/MACOS9LIVES-POST.md).
2. **Software lever**, formally open/claim the device (ideally present it as a claimed SCSI bus, like the working cards) so the OS keeps its memory window alive through the mount.
3. **The real endgame**, FCode + this NDRV in the card's 512 KB ROM, which is both the documented‑reliable answer *and* what booting *from* the eSATA drive requires. The driver core here is exactly the NDRV that would go in that ROM, so none of the work is lost.

## Build

Requires the [Retro68](https://github.com/autc04/Retro68) PowerPC toolchain.

```sh
# 1) The driver ndrv
cmake -S . -B build \
  -DCMAKE_TOOLCHAIN_FILE=~/Retro68-build/toolchain/powerpc-apple-macos/cmake/retroppc.toolchain.cmake
cmake --build build            # -> build/SiI3512SATA.ndrv  (+ .pef; main patched to DoDriverIO)

# 2) The probe app (embeds the driver PEF, drives it step-by-step on hardware)
cd probe && ./embed-pef.sh ../build/SiI3512SATA.pef driver_pef.r sPEF 128
cmake -S . -B build -DCMAKE_TOOLCHAIN_FILE=<toolchain> && cmake --build build   # -> SiI3512Probe.bin

# 3) The faceless Startup-Items auto-mount app. Same pattern under startup./
```

Re‑embed the driver PEF (`embed-pef.sh`) whenever the driver changes. Test disk‑driver code on an **expendable** OS 9 volume, a bad boot‑time build can wedge startup.

## Layout

- `src/`, the driver: `sil3512_driver.c` (DoDriverIO dispatch + `TheDriverDescription`), `sil3512_os.c` (OS glue: PCI/BAR/DMA/registry), `sil3512_hw.c` (bring‑up), `sil3512_ata.c` (ATA engine), `sil3512_scsi.c` (SCSI Manager 4.3 SIM), `sil3512_disk.c` (drive‑queue + block I/O), `sil3512_regs.h` (BAR5 register map)
- `probe/`, hardware‑validation app (double‑clickable; drives the driver in staged order so a fault localizes itself)
- `startup/`, faceless Startup‑Items auto‑mount app
- `resident/`, 68K INIT + PPC driver resident‑install vehicle (the "true extension" path; deferred to v2.0)
- `patch-pef-main.py`, post‑MakePEF: point the PEF `main` at `DoDriverIO` (the native‑driver shape OS 9 requires)
- `docs/`
  - [`FINDINGS.md`](docs/FINDINGS.md), the mount investigation + research conclusions (**start here for the technical story**)
  - [`SIL3512-REGISTERS.md`](docs/SIL3512-REGISTERS.md), BAR5 register reference (from `sata_sil.c` + datasheet)
  - [`SERITEK-SHORTCUT.md`](docs/SERITEK-SHORTCUT.md), why patched SeriTek firmware isn't a drop‑in
  - [`MACOS9LIVES-POST.md`](docs/MACOS9LIVES-POST.md), the community question
  - [`FEASIBILITY.md`](FEASIBILITY.md), the original (July 2) feasibility analysis (historical; the premise held, the mount wall is newer)

## Status of the goals

- **Goal 1, boot the Mac with the card installed**, satisfied for OS 9 (the old "card prevents boot" was a dual‑boot/OS X artifact; OS 9 boots to the desktop with the card in).
- **Goal 2, read/mount drives under OS 9**, driver reads perfectly; the **mount** is the open wall (above).
- **Goal 3, boot *from* the card**, needs FCode + this NDRV in the card's soldered 512 KB ROM (in‑place flash only). This is also the research‑backed path to a *reliable* mount.

*Work in progress. The hard 90%, a working from‑scratch SiI3512 driver on OS 9, is done; the last mile (the OS mount) is genuinely stubborn and is the same wall that blocks every non‑ROM approach on this chip.*
