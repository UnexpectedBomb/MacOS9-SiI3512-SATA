/*
 * sil_probe_app.c — resident driver install + HFS MOUNT.  v56 ('lpwr'=false opt-out: decline power switching so ACA can't D3 us)
 *
 * OS 9's built-in HFS mounter hangs on any driver-presented HFS volume — a deep,
 * confirmed OS-9 ROM issue (chased to v38: even MacsBug can't capture the hung
 * stack at interrupt time; corroborated independently by the sibling USB-2.0
 * project). The WIN, proven in that project (r22), is to sidestep HFS entirely:
 * present a FAT volume so PC Exchange (Foreign File Access) mounts it instead.
 *
 * This app installs the driver (its kInitialize brings up the card, scans the MBR,
 * and AddDrive's each FAT partition — see sil3512_disk.c v52 + the "SiI3512 Driver
 * Log"), finds the drive(s), and PBMountVol's each. With Extensions ON, HFS declines
 * the non-'BD' volume and PC Exchange claims + mounts it. The SSD must be
 * FAT32/MBR-formatted on a PC or Mac OS X first.
 *
 * The driver installs as a resident private CFM copy, so it and the mounted volume
 * PERSIST after this app quits. MUST REBOOT between runs (a 2nd install double-
 * installs and crashes). gSilWriteEnabled=1 => the FAT volume mounts read/write.
 */
#include <MacTypes.h>
#include <Quickdraw.h>
#include <Fonts.h>
#include <Windows.h>
#include <Events.h>
#include <TextUtils.h>
#include <Dialogs.h>
#include <TextEdit.h>
#include <Resources.h>
#include <Errors.h>
#include <Gestalt.h>
#include <Files.h>
#include <Disks.h>
#include <Devices.h>
#include <Folders.h>
#include <NameRegistry.h>
#include "sil3512.h"

static void PCat(Str255 d, const char *s){ short l=d[0]; while(*s&&l<255)d[++l]=(unsigned char)*s++; d[0]=(unsigned char)l; }
static void PCatP(Str255 d, const unsigned char *ps){ short l=d[0],i,n=ps[0]; for(i=1;i<=n&&l<255;i++)d[++l]=ps[i]; d[0]=(unsigned char)l; }
static void PCatDec(Str255 d, long v){ char t[12]; short n=0; unsigned long u; if(v<0){if(d[0]<255)d[++d[0]]='-';u=(unsigned long)(-v);}else u=(unsigned long)v;
  if(!u){PCat(d,"0");return;} while(u){t[n++]=(char)('0'+(u%10));u/=10;} while(n>0&&d[0]<255)d[++d[0]]=t[--n]; }
static void PCatHex(Str255 d, unsigned long v, int digits){ static const char h[]="0123456789ABCDEF"; int i; for(i=digits-1;i>=0;i--) if(d[0]<255) d[++d[0]]=(unsigned char)h[(v>>(i*4))&0xF]; }

static short gLogRef=0,gLogVol=0; static Str255 gLines[64]; static short gN=0;
static void LogOpen(void){ long dirID; FSSpec sp;
  if(FindFolder(kOnSystemDisk,kSystemFolderType,kDontCreateFolder,&gLogVol,&dirID)!=noErr)return;
  if(FSMakeFSSpec(gLogVol,dirID,"\pSiI3512 Probe Log",&sp)==noErr)FSpDelete(&sp);
  if(FSpCreate(&sp,'ttxt','TEXT',smSystemScript)!=noErr)return;
  if(FSpOpenDF(&sp,fsRdWrPerm,&gLogRef)!=noErr)gLogRef=0; }
static void Out(Str255 s){ if(gLogRef){long len=s[0];char cr='\r';FSWrite(gLogRef,&len,&s[1]);len=1;FSWrite(gLogRef,&len,&cr);FlushVol(NULL,gLogVol);}
  if(gN<64){BlockMoveData(s,gLines[gN],(long)s[0]+1);gN++;} }
static void Say(const char *s){ Str255 L; L[0]=0; PCat(L,s); Out(L); }

/* Mounted-volume queue (VCB @ 0x0356): detect PC Exchange claiming the volume
 * (same low-mem globals the sibling USB-2.0 trigger uses to confirm its FAT mount). */
#define kVCBQ ((QHdrPtr)0x0356L)
static int VcbCount(void){ QHdrPtr q=kVCBQ; int n=0; QElemPtr e; if(!q)return 0; for(e=q->qHead;e&&n<200;e=e->qLink)n++; return n; }

int main(void)
{
    WindowPtr win; Rect bounds; EventRecord evt; short i,y;
    RegEntryID card; Handle pefH; DriverRefNum refNum=0; OSErr err;
    short mine[8]; int nmine=0; int vcbBase=0, vcbNow=0;

    InitGraf(&qd.thePort); InitFonts(); InitWindows(); InitMenus();
    TEInit(); InitDialogs(NULL); InitCursor();
    LogOpen();

    bounds.left=12; bounds.top=40; bounds.right=12+600; bounds.bottom=40+460;
    win=NewWindow(NULL,&bounds,"\pSiI3512 v57 probe - foreground baseline re-test",true,documentProc,(WindowPtr)-1L,false,0);
    if(win){ SetPort((GrafPtr)win); TextFont(kFontIDMonaco); TextSize(9);
      MoveTo(8,20); DrawString("\pSiI3512 v56: install driver, APM scan, PBMountVol -> OS 9 built-in HFS mounter.");
      MoveTo(8,36); DrawString("\pRun with Audio CD Access ENABLED (coexistence test). Watch for the volume on the desktop.");
      MoveTo(8,52); DrawString("\pReboot the MDD before EACH run (the resident driver persists)."); }

    Say("=== SiI3512 v57 : probe baseline re-test (APM scan, built-in mounter) ===");
    Say("");

    if (sil_find_node(&card)!=noErr){ Say("card NOT FOUND"); goto finish; }
    pefH = GetResource('sPEF',128);
    if(!pefH||!*pefH){ Say("PEF resource MISSING"); goto finish; }
    HLockHi(pefH);

    vcbBase = VcbCount();

    Say("[1] InstallDriverFromMemory (kInitialize: bring-up + MBR scan + AddDrive) ...");
    err = InstallDriverFromMemory(*pefH, GetHandleSize(pefH), "\pSiI3512disk", &card, 48, 127, &refNum);
    { Str255 L; L[0]=0; PCat(L,"  result="); PCatDec(L,(long)err); PCat(L,"  refNum="); PCatDec(L,(long)refNum); Out(L); }
    if (!(err==noErr && refNum!=0)) { Say("  install failed - see result above"); goto finish; }
    Say("  installed. (MBR scan / AddDrive: see 'SiI3512 Driver Log')");

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
    if (nmine==0) { Say("  no drives added (driver found no FAT partition? see Driver Log)"); goto finish; }

    Say("[3] PBMountVol each -> OS 9 built-in HFS mounter (no Foreign File Access) ...");
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

    /* PC Exchange may finalize the mount at task level -- yield time (WaitNextEvent
     * schedules other processes) and watch the VCB queue for a new volume ~8s. */
    Say("[4] letting the mount settle (~8s), watching the volume queue ...");
    { long dl=TickCount()+8*60; while(TickCount()<dl) (void)WaitNextEvent(0,&evt,3,NULL); }
    vcbNow = VcbCount();
    { Str255 L; L[0]=0; PCat(L,"  VCB volumes: "); PCatDec(L,(long)vcbBase); PCat(L," -> "); PCatDec(L,(long)vcbNow); Out(L); }
    if (vcbNow > vcbBase) Say(">>> A NEW VOLUME MOUNTED - check the desktop = GOAL 2 <<<");
    else                  Say("  no new volume in the VCB queue (see PBMountVol result above)");

    /* [5] dump the driver's Control/Status probe ring (Gestalt 'Si3L'): shows the exact
     * csCodes the foreign-FS plugins issued. Control cs=104/125 = the CD-ROM probes that
     * cause the audio-CD misID (now declined with controlErr). Task-level, safe. */
    Say("");
    Say("[5] driver probe ring (Gestalt 'Si3L') -- cs=code, p0=DriverGestalt selector / csParam:");
    { long gv;
      if (Gestalt('Si3L', &gv) == noErr && gv != 0) {
        typedef struct { short kind, csCode, ioVRefNum, pad; long p0, p1; } CsRec;
        typedef struct { unsigned long magic, count, cap; CsRec recs[1]; } CsLog;
        CsLog *cl = (CsLog *)gv;
        if (cl->magic == 0x5369334cUL && cl->cap && (cl->cap & (cl->cap - 1)) == 0 && cl->cap <= 4096) {
          unsigned long cap = cl->cap, total = cl->count, k, from;
          { Str255 L; L[0]=0; PCat(L,"  total probes="); PCatDec(L,(long)total); Out(L); }
          from = (total > cap) ? total - cap : 0;
          for (k = from; k < total; k++) {
            CsRec *r = &cl->recs[k & (cap - 1)];
            Str255 L; L[0]=0; PCat(L, r->kind==1 ? "  Status  cs=" : "  Control cs=");
            PCatDec(L,(long)r->csCode); PCat(L,"  p0=0x"); PCatHex(L,(unsigned long)r->p0,8); Out(L);
          }
        } else Say("  (ring magic/cap mismatch)");
      } else Say("  (ring not published)");
    }

finish:
    Say("");
    Say("--- send 'SiI3512 Probe Log' + 'SiI3512 Driver Log' ---");
    if (gLogRef) FSClose(gLogRef);
    /* draw the accumulated log into the status window; wait for a click/key to quit
     * (the driver + mounted volume persist after quit; reboot to remove). */
    if(win){ SetPort((GrafPtr)win); TextFont(kFontIDMonaco); TextSize(9);
      EraseRect(&win->portRect);
      y=12; for(i=0;i<gN&&y<456;i++){MoveTo(8,y);DrawString(gLines[i]);y+=12;}
      { long dl=TickCount()+120*60; while(TickCount()<dl)
          if(WaitNextEvent(mDownMask|keyDownMask,&evt,6,NULL)&&(evt.what==mouseDown||evt.what==keyDown))break; }
      DisposeWindow(win); }
    return 0;
}
