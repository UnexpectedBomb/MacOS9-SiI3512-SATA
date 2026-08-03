#!/usr/bin/env python3
# sil_rom_inject.py — inject the SiI3512 NDRV into an OS 9 "Mac OS ROM" as a
# CLAIMED device, so OS 9 owns + initialises the card at boot and keeps its
# legacy taskfile aperture alive through the File-Manager mount (the fix for the
# v60-confirmed chip-gating: at mount everything is healthy EXCEPT the taskfile
# aperture, which the OS gates on an *unclaimed* card's driver-open handshake).
#
# Modelled on tbxi-patches/macmini.py, which injects the ATY,RockHopper2 (an ATI
# Radeon PCI card) NDRV the same way. The lever is the Parcelfile entry:
#     prop flags=0x0000c a=<node> b=ata
#         ndrv flags=0x00006 name=driver,AAPL,MacOS,PowerPC src=SiI3512SATA.pef
# The `driver,AAPL,MacOS,PowerPC` property is the "claim" marker our
# InstallDriverFromMemory path never got (research: unclaimed => not under expert
# control => power/decode not managed => taskfile gated at mount).
#
# Usage (identical convention to the other tbxi-patches scripts):
#     source ~/Developer/rom-tools/venv/bin/activate   # tbxi must be importable
#     python3 sil_rom_inject.py <MacOSROM|dumpdir> -o <out.rom | outdir/>
#   e.g.  python3 sil_rom_inject.py "Mac OS ROM" -o "Mac OS ROM.SiI3512"
#
# NOTE this is a DRAFT candidate: two things must be locked before a hardware run
# (see the driver-side TODOs + the MATCH question at the bottom of this file).

import sys, os, shutil, fnmatch
from os import path

# Elliot Nunn's tbxi-patches directory (contains patch_common). Override with $TBXI_PATCHES.
TBXI_PATCHES = os.environ.get('TBXI_PATCHES', os.path.expanduser('~/rom-tools/tbxi-patches'))
if not path.isdir(TBXI_PATCHES):
    raise SystemExit(
        "tbxi-patches not found at %r.\n"
        "Set $TBXI_PATCHES to Elliot Nunn's Mac OS ROM toolchain 'tbxi-patches' directory."
        % TBXI_PATCHES)
sys.path.insert(0, TBXI_PATCHES)
import patch_common

# ---- the hardware-proven driver core (the exact NDRV the ROM route reuses) ----
# Defaults to the locally built driver; override with $SIL_PEF.
_HERE    = path.dirname(path.abspath(__file__))
OUR_PEF  = os.environ.get('SIL_PEF', path.join(_HERE, '..', 'build', 'SiI3512SATA.pef'))
PEF_NAME = 'SiI3512SATA.pef'          # referenced uncompressed, like the stock controller ndrvs

# ---- device-node MATCH string — LOCKED by the v61 node-probe (2026-07-23) -------
# The v61 Name Registry dump CONFIRMED the card's node has name == compatible ==
# "SunrichSATA3512" (the Sunrich FCode's name; there is NO "pci1095,3512" string on
# the node), device_type == "ata". So we match the node name, exactly like the stock
# ROM's ATA-controller parcels (a=cmd646-ata etc.). The driver's DriverDescription
# nameInfoStr is set to the same "SunrichSATA3512" so the Family Expert binding agrees.
MATCHES     = ['SunrichSATA3512']
DEVICE_TYPE = 'ata'
PROP_FLAGS  = '0x0000c'   # mirrors the stock storage-controller prop entries
NDRV_FLAGS  = '0x00006'   # mirrors cmd646-ata / kauai-ata / keylargo-ata ndrv flags


def parcel_lines():
    out = []
    dedup = ' deduplicate=1' if len(MATCHES) > 1 else ''
    for m in MATCHES:
        out.append('prop flags=%s a=%s b=%s\n' % (PROP_FLAGS, m, DEVICE_TYPE))
        out.append('\tndrv flags=%s name=driver,AAPL,MacOS,PowerPC src=%s%s\n\n'
                   % (NDRV_FLAGS, PEF_NAME, dedup))
    return out


src, cleanup = patch_common.get_src(
    desc='Inject the SiI3512 NDRV into the OS 9 Mac OS ROM as a claimed storage device.')

if not path.exists(OUR_PEF):
    raise SystemExit('driver PEF not found: %s  (build it first)' % OUR_PEF)

injected = False
for (parent, folders, files) in os.walk(src):
    folders.sort(); files.sort()
    if 'Parcelfile' not in files:
        continue
    if any(fnmatch.fnmatch(fn, 'SiI3512SATA*.pef') for fn in os.listdir(parent)):
        print('SiI3512 PEF already present in %s — skipping (idempotent)' % parent)
        injected = True
        continue
    shutil.copy(OUR_PEF, path.join(parent, PEF_NAME))
    with open(path.join(parent, 'Parcelfile'), 'a') as f:
        f.write('\n')
        f.writelines(parcel_lines())
    print('Injected SiI3512 parcel(s): a=%s  b=%s' % (' / a='.join(MATCHES), DEVICE_TYPE))
    print('  copied %s into %s' % (PEF_NAME, parent))
    injected = True

if not injected:
    raise SystemExit('no Parcelfile found in the dump — is this a NewWorld ROM?')

cleanup()   # rebuilds the ROM if -o was a file (no-op if -o was a dump dir)
print('done.')

# ============================================================================
# BEFORE A HARDWARE RUN, lock these (all developed against the reference ROM;
# the real MDD "Mac OS ROM" is still needed for the boot-critical build):
#
#  TODO-1 (driver, runtimeFlags): our DriverDescription.driverRuntime = 0x03
#    (kDriverIsLoadedUponDiscovery|kDriverIsOpenedUponLoad). The 3 working ROM
#    ATA controllers set kDriverIsUnderExpertControl (0x04): cmd646=0x05,
#    heathrow/keylargo=0x04. Add 0x04 (=> 0x07, or mirror cmd646's 0x05) in
#    sil3512_driver.c so the Family Expert manages us — this is the exact
#    "under expert control" status the research tied to proper power/decode
#    ownership (and to Audio-CD-Access coexistence).
#
#  TODO-2 (service category): ours = 'ndrv'/'blok' (generic native driver);
#    the working ROM ATA controllers = 'ata '/null (ATA-family plugins driven by
#    the ATA Manager via a different API). FIRST TRY our existing 'ndrv'/'blok'
#    DoDriverIO driver (cheapest — just add TODO-1's flag + this parcel) and test
#    whether being OS-loaded+opened+expert-controlled at boot alone keeps the
#    aperture alive. If NOT, escalate to a real storage-family presentation:
#    an 'ata ' family plugin (rewrite to the ATA Manager plugin API) or the
#    SCSI-bus SIM (we have the v9-v12 SIM code; research says reliable SiI cards
#    claim as SCSI).
#
#  MATCH: RESOLVED 2026-07-23 by the v61 node-probe — node name == compatible ==
#    "SunrichSATA3512", device_type "ata". MATCHES trimmed to it; driver nameInfoStr
#    set to agree. (vendor-id 0x1095 / device-id 0x3512 exist only as separate integer
#    props — there is no "pci<vendor>,<device>" string, so that match would not bind.)
# ============================================================================
