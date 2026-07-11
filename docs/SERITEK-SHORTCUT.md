# Sanity check: is a device-ID-patched SeriTek ROM a fast path to boot-from-card?

**Question:** For goal 3 (boot Mac OS 9 *from* a drive on the LaCie card), can we skip
writing our own Open Firmware FCode by taking dosdude1's reverse-engineered SeriTek/1S2 ROM
(SiI3112), patching the device-ID check `3112`→`3512`, and flashing it onto the LaCie card?

**Verdict:** *Executable but not recommended as a drop-in.* The soldered-flash obstacle is
solved (flashrom can read/write it in place), but a patched 3112 image is unproven on the
3512 and carries a real correctness risk (errata). Its true value is as **reference + tooling**,
plus it surfaces one do-it-regardless action: **dump the LaCie flash first.**

## Findings

### 1. The soldered flash is NOT a barrier — flashrom `satasii` handles the SiI3512 in place
flashrom's `satasii` programmer talks to the flash **through the SATA controller**, so the
soldered 29LV040A needs no desoldering:
```
flashrom --programmer satasii -r lacie-backup.bin     # read/backup (do this first!)
flashrom --programmer satasii -w newimage.bin         # write
```
Runs from a **PC with a legacy PCI slot booted to FreeDOS** (USB stick). This is the enabling
logistics fact — and it works regardless of which goal-3 path we take. (Logistics need: a
PCI-slot PC + FreeDOS. The Mac-side SeriTek1S2Flasher exists too but likely gates on card type.)

### 2. Unmodified SeriTek firmware categorically won't run on a 3512
The SeriTek ROM **requires the PCI device ID to read as `0x3112`** to execute — evidenced by
the Adaptec-card trick ("remove the 24C02 EEPROM to restore 3112 IDs so the SeriTek ROM will
run"). The SiI3512's device ID is **hardwired `0x3512`** in silicon — no strap/EEPROM trick can
make it read 3112. So the ROM's ID checks *must* be patched. This is exactly the community's
"flashed unmodified SeriTek onto a 3512 → won't boot" result.

### 3. The device-ID checks are in *multiple* places, inside compressed sections
Confirmed there are checks in **both the OFW FCode and the Classic NDRV** (and the OS X driver).
The OS 9 NDRV is **LZSS-compressed** inside the ROM. Patching means: decompress → locate every
`3112` check in FCode + NDRV → patch → recompress. Tooling exists (`lzss-fcode-new.4th`), but
it's fiddly and no one has produced a working 3512 patch. The nearest cross-chip attempt (a
3112 image on a 3114) reportedly "no workie."

### 4. The correctness trap: errata mismatch (the reason not to ship SeriTek's NDRV)
SeriTek's driver is tuned for the **SiI3112**. The **SiI3512 needs the `RERR_ON_DMA_ACT`
errata workaround** (assert R_ERR on DMA-activate FIS) that the 3112 driver does **not** apply
(verified from Linux `sata_sil.c`). A device-ID-patched-but-otherwise-3112 NDRV could therefore
**boot yet be unreliable** under DMA load on the 3512. **Our own NDRV (M2) already handles this
quirk** — so we keep ours for runtime no matter what.

### 5. External corroboration of our reframing
A forum user with a **SiI3512 card "ticking along just fine in my MDD"** (declining to reflash
it) independently confirms what we found on hardware: these 3512 cards **boot OS 9 fine**; they
just have no driver / no drive access. Good outside validation.

## Recommendation

**Do not flash patched SeriTek firmware wholesale.** Instead, when we reach goal 3:

1. **Dump the LaCie card's existing 512 KB flash with flashrom** (`-r`) — *this is the one
   high-value, low-risk action, and we do it regardless of path.* It gives us:
   - a safety backup (mandatory before any write to a soldered part), and
   - the existing **Sunrich FCode** to study — it already creates the working `ata` node
     (per ASP), so it may already contain boot/`block` bits we can build on.
2. **Write our own FCode**, keyed to `3512` and matched to our NDRV, using the dumped Sunrich
   FCode **and** the SeriTek FCode as references (blueprints, not binaries).
3. **Keep our NDRV** (correct `RERR_ON_DMA_ACT` handling) for the runtime driver.
4. If we do borrow SeriTek's FCode, **strip its NDRV** from the flashed image so our (correct)
   driver binds instead of its (3112-tuned) one.

Net: the shortcut isn't a clear time-saver (we'd need our NDRV anyway, and FCode patching
across compressed sections is unproven for the 3512), but the SeriTek ROM + LZSS tools +
flashrom recipe become our **goal-3 toolkit**, and the SeriTek/Sunrich FCode is a valuable
reference for authoring ours.

## Assets to grab when we start goal 3
- `1s2-patched-compressed-rom.zip` (dosdude1) + full 512K ROM — reference FCode/NDRV layout
- `lzss-fcode-new.4th` — LZSS (de)compression for the ROM's FCode/NDRV
- `flashrom` (satasii programmer) on FreeDOS — dump + write the LaCie flash in place
- Hosted on the 68kMLA / TinkerDifferent SiI3112 flashing threads (forum attachments)

## Sources
- flashrom `satasii` reads/writes SiI3512 in place; FreeDOS recipe —
  [MacRumors SIL3112 flashing guide](https://forums.macrumors.com/threads/guide-to-flashing-pc-sil3112-sata-cards-for-mac.1690231/),
  [flashrom manual](https://flashrom.org/classic_cli_manpage.html)
- SeriTek ROM requires 3112 ID (Adaptec 24C02 trick); checks in FCode + NDRV; LZSS; tools;
  3512 patch untried, 3114 cross-chip failed —
  [TinkerDifferent 2025 flashing guide](https://tinkerdifferent.com/threads/flashing-the-silicon-image-sil3112-to-work-in-macs-2025-edition.1494/)
- 3512 vs 3112 errata (`RERR_ON_DMA_ACT`) — Linux [`sata_sil.c`](https://raw.githubusercontent.com/torvalds/linux/master/drivers/ata/sata_sil.c)
