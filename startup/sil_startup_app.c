/*
 * sil_startup_app.c — faceless Startup-Items auto-mount app for the SiI3512 driver.
 *
 * GOAL-2 SHIPPING VEHICLE (2026-07-09). The boot-INIT vehicle (m62a/b/c) FAILED:
 * installing the driver from an INIT and mounting from a Notification-Manager task
 * crashes, because the card's PCI decode is knocked out during the early desktop-load
 * window (config stays pristine — D0, decode enabled, BAR correct — yet MMIO
 * bus-errors; findings=0; see the eSATA project memory / m62c). The v56 PROBE runs the
 * IDENTICAL install + bring-up + mount and SUCCEEDS — because it runs LATER (system
 * settled) from a real application PROCESS (not nested in the Finder's SystemTask).
 *
 * This app IS that probe, automated + faceless. Dropped in the System Folder's
 * "Startup Items", it auto-runs at login, WAITS a few seconds for the OS to finish its
 * PCI/power/ATA startup pass, then:
 *   [1] InstallDriverFromMemory (embedded 'sPEF' 128) -> the driver's kInitialize
 *       brings up the card, scans the Apple Partition Map, and AddDrive's each
 *       Apple_HFS partition (see sil3512_disk.c; driver self-mount NM is compiled OUT),
 *   [2] finds our drive(s) in the drive queue,
 *   [3] PBMountVol's each -> OS 9's built-in HFS mounter -> volume on the desktop.
 * No window, no user interaction. The resident driver + mounted volume persist; the
 * app then idles (kept alive as a real process; faceless via SIZE onlyBackground).
 *
 * Requires Audio CD Access removed/disabled (ship caveat). Reboot removes the driver.
 */
#include <MacTypes.h>
#include <Quickdraw.h>
#include <Fonts.h>
#include <Windows.h>
#include <Events.h>
#include <TextUtils.h>
#include <Dialogs.h>
#include <Resources.h>
#include <Errors.h>
#include <Gestalt.h>
#include <Files.h>
#include <Disks.h>
#include <Devices.h>
#include <Folders.h>
#include <NameRegistry.h>
#include "sil3512.h"

/* Seconds to let the system settle after launch before touching the card. The early
 * desktop-load window disrupts the card's PCI decode (m62c); the probe works only
 * because it runs later. Tunable — bump if the first mount read still bus-errors. */
#define kSettleSeconds  20

static void PCat(Str255 d, const char *s){ short l=d[0]; while(*s&&l<255)d[++l]=(unsigned char)*s++; d[0]=(unsigned char)l; }
static void PCatP(Str255 d, const unsigned char *ps){ short l=d[0],i,n=ps[0]; for(i=1;i<=n&&l<255;i++)d[++l]=ps[i]; d[0]=(unsigned char)l; }
static void PCatDec(Str255 d, long v){ char t[12]; short n=0; unsigned long u; if(v<0){if(d[0]<255)d[++d[0]]='-';u=(unsigned long)(-v);}else u=(unsigned long)v;
  if(!u){PCat(d,"0");return;} while(u){t[n++]=(char)('0'+(u%10));u/=10;} while(n>0&&d[0]<255)d[++d[0]]=t[--n]; }

/* Flushed log ("SiI3512 Startup Log") — no window on a faceless app, so this file is
 * the only status output. Flushed after every line so a crash leaves a breadcrumb. */
static short gLogRef=0,gLogVol=0;
static void LogOpen(void){ long dirID; FSSpec sp;
  if(FindFolder(kOnSystemDisk,kSystemFolderType,kDontCreateFolder,&gLogVol,&dirID)!=noErr)return;
  if(FSMakeFSSpec(gLogVol,dirID,"\pSiI3512 Startup Log",&sp)==noErr)FSpDelete(&sp);
  if(FSpCreate(&sp,'ttxt','TEXT',smSystemScript)!=noErr)return;
  if(FSpOpenDF(&sp,fsRdWrPerm,&gLogRef)!=noErr)gLogRef=0; }
static void Out(Str255 s){ if(gLogRef){long len=s[0];char cr='\r';FSWrite(gLogRef,&len,&s[1]);len=1;FSWrite(gLogRef,&len,&cr);FlushVol(NULL,gLogVol);} }
static void Say(const char *s){ Str255 L; L[0]=0; PCat(L,s); Out(L); }

/* Mounted-volume queue (VCB @ 0x0356): confirm a new volume appeared. */
#define kVCBQ ((QHdrPtr)0x0356L)
static int VcbCount(void){ QHdrPtr q=kVCBQ; int n=0; QElemPtr e; if(!q)return 0; for(e=q->qHead;e&&n<200;e=e->qLink)n++; return n; }

int main(void)
{
    EventRecord evt;
    RegEntryID card; Handle pefH; DriverRefNum refNum=0; OSErr err;
    short mine[8]; int nmine=0; int vcbBase=0, vcbNow=0; short i; long dl;

    InitGraf(&qd.thePort); InitFonts(); InitWindows(); InitMenus();
    TEInit(); InitDialogs(NULL);
    LogOpen();

    Say("=== SiI3512 faceless Startup-Items auto-mount v1 ===");
    { Str255 L; L[0]=0; PCat(L,"settle "); PCatDec(L,(long)kSettleSeconds);
      PCat(L,"s, then install driver + PBMountVol (app owns the mount; driver NM off)"); Out(L); }

    /* [0] SETTLE: yield to the system (WaitNextEvent schedules other processes, unlike a
     * spin) so it finishes its PCI/power/ATA startup pass before we touch the card. */
    dl = TickCount() + (long)kSettleSeconds*60;
    while (TickCount() < dl) (void)WaitNextEvent(everyEvent, &evt, 6, NULL);
    Say("[0] settle done");

    if (sil_find_node(&card)!=noErr){ Say("card NOT FOUND - idling"); goto idle; }
    pefH = GetResource('sPEF',128);
    if(!pefH||!*pefH){ Say("PEF resource MISSING - idling"); goto idle; }
    HLockHi(pefH);

    vcbBase = VcbCount();

    Say("[1] InstallDriverFromMemory (kInitialize: bring-up + APM scan + AddDrive) ...");
    err = InstallDriverFromMemory(*pefH, GetHandleSize(pefH), "\pSiI3512disk", &card, 48, 127, &refNum);
    { Str255 L; L[0]=0; PCat(L,"  result="); PCatDec(L,(long)err); PCat(L,"  refNum="); PCatDec(L,(long)refNum); Out(L); }
    if (!(err==noErr && refNum!=0)) { Say("  install failed - idling"); goto idle; }
    Say("  installed. (APM scan / AddDrive: see 'SiI3512 Driver Log')");

    Say("[2] find our drive(s) in the drive queue ...");
    { QHdrPtr q = GetDrvQHdr(); DrvQElPtr el = q ? (DrvQElPtr)q->qHead : 0;
      while (el) {
        if (el->dQRefNum == refNum) {
          UInt32 sz = ((UInt32)el->dQDrvSz2<<16)|el->dQDrvSz;
          Str255 L; L[0]=0; PCat(L,"  our drive#="); PCatDec(L,(long)el->dQDrive);
          PCat(L," size(blks)="); PCatDec(L,(long)sz); Out(L);
          if (nmine<8) mine[nmine++]=el->dQDrive;
        }
        el = (DrvQElPtr)el->qLink;
      }
    }
    if (nmine==0) { Say("  no drives added (see Driver Log) - idling"); goto idle; }

    Say("[3] PBMountVol each -> OS 9 built-in HFS mounter ...");
    for (i=0;i<nmine;i++) {
        ParamBlockRec pb; UInt8 *p=(UInt8*)&pb; long j; Str255 vn;
        for(j=0;j<(long)sizeof(pb);j++)p[j]=0;
        pb.volumeParam.ioVRefNum = mine[i];
        err = PBMountVol((ParmBlkPtr)&pb);
        { Str255 L; L[0]=0; PCat(L,"  drive#="); PCatDec(L,(long)mine[i]);
          PCat(L," PBMountVol result="); PCatDec(L,(long)err); Out(L); }
        if (err==noErr || err==volOnLinErr) {
            ParamBlockRec vp; UInt8 *q2=(UInt8*)&vp; for(j=0;j<(long)sizeof(vp);j++)q2[j]=0;
            vn[0]=0; vp.volumeParam.ioNamePtr=vn; vp.volumeParam.ioVRefNum=mine[i]; vp.volumeParam.ioVolIndex=0;
            if (PBGetVInfo(&vp,false)==noErr){ Str255 L; L[0]=0; PCat(L,"    >>> MOUNTED: '"); PCatP(L,vn); PCat(L,"' <<<"); Out(L); }
            else Say("    mounted (volume name read failed)");
        }
    }

    Say("[4] settling (~8s), watching the volume queue ...");
    dl=TickCount()+8*60; while(TickCount()<dl) (void)WaitNextEvent(everyEvent,&evt,6,NULL);
    vcbNow = VcbCount();
    { Str255 L; L[0]=0; PCat(L,"  VCB volumes: "); PCatDec(L,(long)vcbBase); PCat(L," -> "); PCatDec(L,(long)vcbNow); Out(L); }
    if (vcbNow > vcbBase) Say(">>> A NEW VOLUME MOUNTED at boot = GOAL 2 <<<");
    else                  Say("  no new volume in the VCB queue (see PBMountVol result above)");

idle:
    Say("--- idling (faceless); driver + volume persist; reboot to remove ---");
    if (gLogRef) { FSClose(gLogRef); gLogRef=0; }
    /* Stay resident as a real process so the mounted volume persists and this app never
     * blocks the boot. Faceless: no UI; just yield forever. Shutdown terminates it. */
    for(;;) (void)WaitNextEvent(everyEvent,&evt,60,NULL);
    /* not reached */
}
