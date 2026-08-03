# Findings, the driver, the mount wall, and what the research concluded

This document records what we learned building a from‑scratch Mac OS 9 driver for the Silicon Image **SiI3512** SATA controller, in particular the one wall between "the driver reads the disk perfectly" and "the volume mounts on the desktop."

---

## 1. The driver core is proven on hardware

On a Power Mac G4 MDD (Mac OS 9.2.2) with a SATA SSD on the card, the driver reliably and repeatably (every boot):

- enables the card over PCI (Command = Memory‑Space + Bus‑Master) and maps **BAR5** by correlating config register `0x24` in the Name Registry's `assigned-addresses` with the OF‑mapped logical address in `AAPL,address`;
- brings up the SATA link (COMRESET via SControl → poll SStatus `DET==3` → clear SError) and applies the `RERR_ON_DMA_ACT` errata fix;
- runs `IDENTIFY` (verified against a real drive: model "TEAML5Lite3D120G", LBA48, ~120 GB);
- performs **bus‑master DMA reads and writes**, byte‑verified against known on‑disk data (the first DMA success read the Apple Partition Map DDR, `45 52 …` = 'ER', block size `0x200`, block count matching the drive; a non‑destructive write round‑trip verified + restored);
- installs into the Device Manager Unit Table via `InstallDriverFromMemory`, and in `kInitialize` scans the Apple Partition Map and `AddDrive`s the HFS partition;
- services partition‑offset block I/O through `DoDriverIO` (kRead/kWrite), single‑ and multi‑block.

None of this is theoretical. It is on real hardware, it works on every run, and it works with the *original early* driver build as well as the latest.

Two hard‑won lessons from getting here are reusable for any OS 9 native driver:

- **Native‑driver shape.** `VerifyFragmentAsDriver`/`InstallDriverFromMemory` accept a PEF only if it exports exactly `DoDriverIO` + `TheDriverDescription`, the `DriverDescription` declares ≥1 service, and the fragment **`main` is `DoDriverIO`**. Retro68 emits a shared‑lib PEF with no `main`; `patch-pef-main.py` post‑patches the loader header to point `main` at `DoDriverIO` (matching Apple's shipping NVIDIA `ndrv`).
- **Completion contract.** `DoDriverIO` must call `IOCommandIsComplete(cmdID, err)` for non‑immediate commands, or a synchronous File‑Manager read hangs forever even though the read succeeded; immediate commands (Initialize/Open/Close/Finalize) complete via the return value and must **not** call it.
- **DMA staging.** Never bus‑master DMA directly into a File‑Manager‑supplied buffer, DMA into a driver‑owned static buffer and `BlockMoveData` into the FM buffer. Direct‑to‑FM‑buffer DMA froze the mount / crashed writes.

---

## 2. The wall: the mount read

`AddDrive` puts the drive in the queue; then `PBMountVol` asks OS 9's File Manager to mount it, and the File Manager dispatches `DoDriverIO` kRead to the driver. The **first MMIO read of the taskfile status register (`BAR5 + 0x87`) raises a PowerPC access exception (bus error)**, and the whole BAR5 memory window is then dead, even MacsBug `dm <BAR5>` fails.

The paradox: at the instant of the fault, **PCI config space is perfectly healthy**,

| Config | Value at crash |
|---|---|
| Command register | Memory‑Space + Bus‑Master **set** |
| BAR5 | unchanged, **correct** |
| PMCSR power state | **D0** (not D3) |

So the card's config says "I'm here, powered, decoding memory at BAR5," yet a CPU read of BAR5 bus‑errors. **Config alive, memory decode dead**, and only on the File‑Manager mount read; the byte‑identical scan reads in `kInitialize` succeed microseconds earlier.

MacsBug signature, every time: fault in `sil_r8` (the byte MMIO reader), **R3 = `0x80081000`** (BAR5 base), **R4 = `0x87`**, faulting address **`0x80081087`**.

---

## 3. What we ruled out (and how)

- **Our code / driver version.** Rebuilt the original known‑good driver, pre‑dating every recent change, and it crashes byte‑identically. → not a regression we introduced.
- **A literal PCI D3 power‑down.** PMCSR reads **D0** at the crash. (Earlier, Apple's *Audio CD Access* extension *was* driving the card to D3 out‑of‑band via the PMCSR; returning DriverGestalt `'lpwr'=false` did **not** stop it, and ACA has since been removed entirely, the crash remains.)
- **SATA link power management.** SControl IPM disables Partial+Slumber; the canonical Linux driver does no link PM at all; and link PM is a PHY‑layer state that physically **cannot** disable a PCI BAR (a sleeping link yields `SStatus=0` reads, not a whole‑window bus error).
- **Launch vehicle / timing / context.** A boot INIT + Notification‑Manager mount, a faceless Startup‑Items app, and a foreground double‑clicked app **all fail identically**; waiting 20 s+ after boot doesn't help (the scan reads succeed at that point regardless). → not foreground/background, not settle‑time.
- **Wrong port.** The crash is at `0x87` = **port 0's** status register, and the driver finds the drive on port 0 (log: `port present: 0`), consistent. A drive on port 1 would crash at `0xC7`.
- **Physical.** Reproduces after a **full cold power‑cycle** and after **reseating the card + cables**.

---

## 4. What the research concluded

A multi‑source deep‑dive with each claim adversarially verified, sources: Apple's *Designing PCI Cards and Drivers for Power Macintosh*, the SiI3512 datasheet (SiI‑DS‑0102‑D), Linux `drivers/ata/sata_sil.c`, and the macos9lives / 68kMLA / MacRumors / TinkerDifferent community record.

**Two clean rule‑outs sharpened the target.**
- Not power management, PMCSR = D0 at the crash; the chip's D3 capability (PMCSR at config `0x64`, power‑state bits `[1:0]`) is real but not what's firing.
- Not SATA link PM, the Linux driver has none, and link PM cannot reach a PCI BAR.
→ the decode‑death is a **PCI‑target‑level** phenomenon: the memory window itself being switched off.

**Why `'lpwr'=false` never helped.** Apple's PCI book (Ch. 12): a *family expert* "may issue power commands … at any time," and the `'lpwr'` opt‑out only lets a driver reject a **family expert's** SetPowerMode (csCode 70) call, it **cannot** block a raw, out‑of‑band PMCSR write. A card that nothing has claimed is exactly the unprotected case.

**The structural difference between us and every card that works.** `InstallDriverFromMemory` installs the driver into the Unit Table but explicitly does **not open it**, so our card never gets the `driver-ref` Name Registry property, is never `kDriverIsUnderExpertControl`, and never receives the documented "switch from low‑power to high‑power mode when the device is opened." Every SiI3112 card that mounts reliably under OS 9 is one the OS **claims as a SCSI bus** via a SIM (a special NDRV) in the card's **declaration ROM**, working through SCSI Manager 4.3, *not* an ATA path. **Leading (unproven) hypothesis:** the OS reprograms or closes our card's memory window at mount time *because it is unclaimed*.

**The sobering headline.** Reliable OS 9 operation on Silicon Image SATA chips is delivered by **FCode + a Mac driver in the card's declaration ROM**, and the **SiI3512 specifically has zero documented OS 9 success by anyone, by any means**, only the SiI3112 is OS‑9‑proven, and only via FirmTek's SeriTek ROM (WiebeTech's SiI3512 firmware is OS X only). This is unmapped territory on this exact chip.

**But not categorically impossible.** A real software‑only mass‑storage driver *has* mounted (non‑boot) data disks on the SiI3112/3114 with no card ROM at all, presenting as a SCSI bus. So software‑only data mounting is demonstrated in general, just never on the 3512.

**Theories the verification *killed*** (so we don't chase them): the datasheet's "DSI bit ⇒ a D3→D0 round‑trip leaves the chip uninitialized" idea, and the "`device_type='ata'` ⇒ OS 9's ATA Manager claims/resets our node" idea. The *exact* mechanism of the window‑close therefore remains **unproven**.

---

## 5. The open question

**What closes a PCI card's BAR memory window at File‑Manager‑mount time while config space stays healthy (D0, Memory‑Space enabled, BAR correct)?** The most likely remaining candidate is a **PCI‑to‑PCI bridge or the Uni‑N host bridge reprogramming/closing the card's memory aperture**, the classic "config alive, decode dead" signature, possibly because nothing has formally claimed the card. That is the question put to the community in [MACOS9LIVES-POST.md](MACOS9LIVES-POST.md).

---

## 6. Paths forward

1. **Community.** Post the forum question; the macos9lives crowd have deep SeriTek / SiI311x knowledge and are the people the research kept citing.
2. **Software lever.** Formally *open/claim* the device, call the driver "open" so it gets the `driver-ref` property and the high‑power‑on‑open transition, and ideally present it as a claimed **SCSI bus** like the working cards, so the OS leaves its memory window alone through the mount. Unproven, and the research is skeptical a software open fully replicates what the ROM does; but it is the single best remaining software experiment. (This project already contains a SCSI Manager 4.3 SIM in `src/sil3512_scsi.c`, built early then set aside for the block‑driver model that now hits this wall.)
3. **The ROM endgame.** FCode + this NDRV in the card's **512 KB ROM**, both the documented‑reliable answer *and* what booting *from* the eSATA drive requires. The flash is a **soldered** 29LV040A (in‑place flash only, e.g. via `flashrom satasii` on a PCI‑slot PC, dump and back up first). The driver core in this repo is exactly the NDRV that goes in that ROM, so none of the work is lost.
