# SiI3512 on OS 9: from-scratch driver does DMA read/write + reads the partition map fine, but the card's BAR memory window dies the instant the File Manager mounts. Config space stays healthy. Bridge aperture? Device claiming?

**Setup:** Power Mac G4 (MDD), Mac OS 9.2.2. I've written a native PCI driver from scratch (PowerPC CFM `ndrv`, built with Retro68, no CodeWarrior) for a **Silicon Image SiI3512** 2‑port SATA controller (a LaCie 130823 card; PCI `0x1095/0x3512`), installed at runtime via `InstallDriverFromMemory` from a small app. I know the SiI3512 has no OS 9 track record (the SeriTek/SiI3112 is *the* OS‑9 chip); I'm trying to drive it in pure software for a non‑boot data disk, and I've hit one very specific wall I'd love expert eyes on.

## What works, repeatably, every boot

- PCI enable (Command = Memory‑Space + Bus‑Master), BAR5 mapped from `assigned-addresses` / `AAPL,address`
- SATA COMRESET link‑up, `IDENTIFY`
- **Bus‑master DMA reads *and* writes**, byte‑verified against known on‑disk data
- Reading the **Apple Partition Map** and the **HFS+ volume header (MDB)**, all through the same BAR5 MMIO path

The driver's `kInitialize` does a full partition scan (half a dozen DMA reads + taskfile polls) and `AddDrive`s the HFS partition. **This part never fails.**

## The wall, the mount read, and only the mount read

The moment OS 9's File Manager mounts the volume (`PBMountVol` → Device Manager dispatches `DoDriverIO` kRead to my driver), the **first** MMIO read of the ATA taskfile status register (`BAR5 + 0x87`) raises a **PowerPC access exception (bus error)**. The **entire BAR5 memory window is then dead**, even MacsBug `dm <BAR5>` fails to read it.

The maddening part: at the instant of the crash, **PCI config space is perfectly healthy**:

- Command register: Memory‑Space + Bus‑Master still set
- BAR5 value: unchanged and correct
- **PMCSR power state: D0** (not D3)

So: **config space alive, memory decode dead.** And it fires *only* on the File‑Manager mount read, never on the byte‑identical driver‑init scan reads microseconds earlier.

## What I've ruled out (so please don't spend time here)

- **My code / driver version**, an older, known‑good build of the driver crashes identically. Independent of driver version and of how it's launched (boot INIT + Notification Manager, faceless Startup‑Items app, foreground double‑clicked app).
- **A literal PCI D3 power‑down**, the PMCSR reads **D0** at the crash. (An earlier finding: Apple's Audio CD Access extension *was* D3‑ing this card out‑of‑band via the PMCSR; returning DriverGestalt `'lpwr' = false` did **not** stop it, and ACA has since been removed entirely, the mount‑read crash remains.)
- **SATA link power management**, I disable Partial+Slumber via SControl IPM; and link PM is a PHY‑layer state that can't disable a PCI BAR anyway (a slumbering link gives `SStatus=0` reads, not a whole‑window bus error).
- **Physical**, reproduces after a full cold power‑cycle and after reseating the card + cables.

## My questions

1. **What closes or kills a PCI card's BAR memory window at File‑Manager‑mount time while config space stays healthy (D0, Memory‑Space enabled, BAR correct)?** This "config alive / memory decode dead" signature smells like a **PCI‑to‑PCI bridge or the Uni‑N host bridge reprogramming or closing the card's memory aperture**, perhaps because nothing has formally *claimed* the card. Has anyone seen the OS reprogram/close a slot's memory window out from under a running driver, and how do you stop it?

2. **Does a software‑installed storage driver have to formally *claim/open* the device to survive the mount?** `InstallDriverFromMemory` installs the driver into the Unit Table but (per Apple's *Designing PCI Cards and Drivers*) explicitly does **not** open it, so my card never gets the `driver-ref` Name Registry property, is never `kDriverIsUnderExpertControl`, and never gets the "switch to high‑power when opened" transition. Every SiI3112 card that mounts reliably is one the OS claims as a **SCSI bus** via a SIM in the card's ROM. **Can a runtime‑installed (no‑ROM) driver be made to formally open/claim its device, or present as a claimed SCSI bus, without the FCode‑created ROM node?** Would that keep the card "in use" so the OS leaves its memory window alone through the mount?

3. **Has anyone ever driven a SiI3512 (not 3112) under OS 9 by *any* means**, software or a ROM image? I've found zero prior success on this exact chip; I'd love to be wrong, or to hear definitively that it's ROM‑or‑nothing.

I have full MacsBug traces, register dumps, and logs for any of the above. Thanks for reading, this community's SiI311x/SeriTek knowledge is exactly what I'm hoping to tap.
