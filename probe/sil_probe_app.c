/*
 * sil_probe_app.c -- SiI3512 driver-binding DIFF DIAGNOSTIC (v70).
 *
 * The v69 hardware run crashed OPENING the ROM-claimed driver: a PowerPC
 * unmapped-memory exception at PC=R0=0x9141003C, LR in DriverLoaderLib
 * (OpenInstalledDriver). Static RE of the extracted DriverLoaderLib confirmed the
 * open path branches through a cross-TOC glue whose target is a WILD pointer, and
 * that it reads dCtlWindow (DCE+0x1e) = 0xFFFFFFFF along the way. The SAME garbage
 * address hit v64 (expert auto-open, flags 0x07), so the ROM-claimed driver is
 * un-openable by ANY path. 0x9141003C is NOT in our PEF, so the OS never reached
 * our code -- the family-expert claim produced a binding the open dispatch cannot
 * use.
 *
 * v70 does NOT open anything and touches NO MMIO. It DUMPS the claimed driver's
 * binding (GetDriverForDevice / GetDriverInformation / GetDCtlEntry) and then
 * InstallDriverFromMemory's our SAME PEF as a KNOWN-OPENABLE reference (v68 proved
 * that instance's kOpen dispatch works) and dumps ITS binding -- so we can DIFF
 * exactly which field the ROM claim left malformed (connID / fragMain / dCtlDriver
 * / dCtlFlags / dCtlWindow / openCount). Read-only on the claimed driver; safe.
 * Runs on the v65 OR v69 claimed system, NO ROM swap. REBOOT before each run
 * (the reference install is resident; a 2nd run double-installs and crashes).
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
#include <MacMemory.h>
#include <Errors.h>
#include <Gestalt.h>
#include <Files.h>
#include <Disks.h>
#include <Devices.h>
#include <CodeFragments.h>
#include <Folders.h>
#include <NameRegistry.h>
#include "sil3512.h"

static void PCat(Str255 d, const char *s){ short l=d[0]; while(*s&&l<255)d[++l]=(unsigned char)*s++; d[0]=(unsigned char)l; }
static void PCatP(Str255 d, const unsigned char *ps){ short l=d[0],i,n=ps[0]; for(i=1;i<=n&&l<255;i++)d[++l]=ps[i]; d[0]=(unsigned char)l; }
static void PCatDec(Str255 d, long v){ char t[12]; short n=0; unsigned long u; if(v<0){if(d[0]<255)d[++d[0]]='-';u=(unsigned long)(-v);}else u=(unsigned long)v;
  if(!u){PCat(d,"0");return;} while(u){t[n++]=(char)('0'+(u%10));u/=10;} while(n>0&&d[0]<255)d[++d[0]]=t[--n]; }
static void PCatHex(Str255 d, unsigned long v, int digits){ static const char h[]="0123456789ABCDEF"; int i; for(i=digits-1;i>=0;i--) if(d[0]<255) d[++d[0]]=(unsigned char)h[(v>>(i*4))&0xF]; }

static short gLogRef=0,gLogVol=0; static Str255 gLines[80]; static short gN=0;
static void LogOpen(void){ long dirID; FSSpec sp;
  if(FindFolder(kOnSystemDisk,kSystemFolderType,kDontCreateFolder,&gLogVol,&dirID)!=noErr)return;
  if(FSMakeFSSpec(gLogVol,dirID,"\pSiI3512 Probe Log",&sp)==noErr)FSpDelete(&sp);
  if(FSpCreate(&sp,'ttxt','TEXT',smSystemScript)!=noErr)return;
  if(FSpOpenDF(&sp,fsRdWrPerm,&gLogRef)!=noErr)gLogRef=0; }
static void Out(Str255 s){ if(gLogRef){long len=s[0];char cr='\r';FSWrite(gLogRef,&len,&s[1]);len=1;FSWrite(gLogRef,&len,&cr);FlushVol(NULL,gLogVol);}
  if(gN<80){BlockMoveData(s,gLines[gN],(long)s[0]+1);gN++;} }
static void Say(const char *s){ Str255 L; L[0]=0; PCat(L,s); Out(L); }

static void DumpNodeProps(RegEntryID *e)
{
    RegPropertyIter cookie; Boolean done=false; RegPropertyNameBuf nm;
    if (RegistryPropertyIterateCreate(e,&cookie)!=noErr){ Say("  (property iterate create failed)"); return; }
    while (RegistryPropertyIterate(&cookie,nm,&done)==noErr && !done) {
        RegPropertyValueSize sz=0, got; UInt8 v[128]; Str255 L; int i,n;
        RegistryPropertyGetSize(e,nm,&sz);
        got=(RegPropertyValueSize)sizeof(v); if(sz<got) got=sz;
        if(RegistryPropertyGet(e,nm,v,&got)!=noErr) got=0;
        L[0]=0; PCat(L,"  "); PCat(L,nm); PCat(L," ["); PCatDec(L,(long)sz); PCat(L,"]='");
        n=(int)got; if(n>72)n=72;
        for(i=0;i<n;i++){ char c=(char)v[i]; if(L[0]<252) L[++L[0]]=(unsigned char)((c>=32&&c<127)?c:'.'); }
        PCat(L,"'"); Out(L);
        L[0]=0; PCat(L,"      hex:"); n=(int)got; if(n>28)n=28;
        for(i=0;i<n;i++){ PCat(L," "); PCatHex(L,(unsigned long)v[i],2); }
        Out(L);
    }
    RegistryPropertyIterateDispose(&cookie);
}

/* MemTop low-mem global (0x0108) bounds our guarded reads so a garbage pointer
 * (0xFFFFFFFF, 0, unaligned) is never dereferenced into a bus error. */
#define kMemTop (*(volatile unsigned long*)0x0108L)
static int PlausibleRead(unsigned long p, long n){
  unsigned long top = kMemTop; if(top<0x00100000UL||top>0x80000000UL) top=0x80000000UL;
  if(!p || (p&3)) return 0;
  if(p>=0x00001000UL && (p+(unsigned long)n) <= top) return 1;   /* RAM */
  if(p>=0xFF000000UL) return 1;                                   /* ROM */
  return 0;
}
static void DumpWordsG(const char *tag, unsigned long p, int nwords){
  Str255 L; int i,j;
  if(!PlausibleRead(p,(long)nwords*4)){ L[0]=0; PCat(L,tag); PCat(L," (skip implausible 0x"); PCatHex(L,p,8); PCat(L,")"); Out(L); return; }
  for(i=0;i<nwords;i+=4){ L[0]=0; PCat(L,tag); PCat(L,"+"); PCatDec(L,(long)(i*4)); PCat(L,":");
    for(j=i;j<i+4&&j<nwords;j++){ unsigned long w=((volatile unsigned long*)p)[j]; PCat(L," "); PCatHex(L,w,8); } Out(L); }
}

/* Dump one driver's full binding by refNum: GetDriverInformation (flags/openCount/
 * connID/fragMain/name) + GetDCtlEntry (dCtlDriver/dCtlFlags/dCtlWindow + raw DCE).
 * This is the exact info OpenInstalledDriver consumes internally. */
static void DumpDriverInfo(const char *tag, DriverRefNum rn)
{
  UnitNumber unit=0; DriverFlags fl=0; DriverOpenCount cnt=0;
  static UInt8 locBuf[256]; static UInt8 descBuf[512];
  RegEntryID dev; Str255 nm; CFragConnectionID cid=0; DriverEntryPointPtr fmain=0;
  Str255 L; OSErr e;
  nm[0]=0;
  L[0]=0; PCat(L,"  == "); PCat(L,tag); PCat(L,"  refNum="); PCatDec(L,(long)rn); PCat(L," =="); Out(L);
  e = GetDriverInformation(rn,&unit,&fl,&cnt,nm,&dev,(CFragSystem7Locator*)locBuf,&cid,&fmain,(DriverDescription*)descBuf);
  L[0]=0; PCat(L,"    GetDriverInformation err="); PCatDec(L,(long)e); Out(L);
  if(e==noErr){
    L[0]=0; PCat(L,"    unit="); PCatDec(L,(long)unit);
    PCat(L," flags=0x"); PCatHex(L,(unsigned long)(unsigned short)fl,4);
    PCat(L," openCount="); PCatDec(L,(long)cnt);
    PCat(L," name='"); PCatP(L,nm); PCat(L,"'"); Out(L);
    L[0]=0; PCat(L,"    connID=0x"); PCatHex(L,(unsigned long)cid,8);
    PCat(L,"  fragMain=0x"); PCatHex(L,(unsigned long)fmain,8); Out(L);
    DumpWordsG("    fragMain",(unsigned long)fmain,4);
  }
  { DCtlHandle h=GetDCtlEntry(rn); DCtlPtr dce=(h? *h : 0);
    L[0]=0; PCat(L,"    dctlHandle=0x"); PCatHex(L,(unsigned long)h,8);
    PCat(L," dce=0x"); PCatHex(L,(unsigned long)dce,8); Out(L);
    if(dce && PlausibleRead((unsigned long)dce,0x38)){
      L[0]=0; PCat(L,"    dCtlDriver=0x"); PCatHex(L,(unsigned long)dce->dCtlDriver,8);
      PCat(L," dCtlFlags=0x"); PCatHex(L,(unsigned long)(unsigned short)dce->dCtlFlags,4);
      PCat(L," dCtlWindow(0x1e)=0x"); PCatHex(L,(unsigned long)dce->dCtlWindow,8); Out(L);
      DumpWordsG("    DCE",(unsigned long)dce,14);
      DumpWordsG("    dCtlDriver",(unsigned long)dce->dCtlDriver,8);
    }
  }
}

int main(void)
{
    WindowPtr win; Rect bounds; EventRecord evt; short i,y;
    RegEntryID card; OSErr err;
    DriverRefNum claimedRef=0, instRef=0;
    Handle pefH; long pefLen;

    InitGraf(&qd.thePort); InitFonts(); InitWindows(); InitMenus();
    TEInit(); InitDialogs(NULL); InitCursor();
    LogOpen();

    bounds.left=12; bounds.top=40; bounds.right=12+620; bounds.bottom=40+460;
    win=NewWindow(NULL,&bounds,"\pSiI3512 v70 - driver-binding DIFF (no open, no mount)",true,documentProc,(WindowPtr)-1L,false,0);
    if(win){ SetPort((GrafPtr)win); TextFont(kFontIDMonaco); TextSize(9);
      MoveTo(8,20); DrawString("\pSiI3512 v70: DUMP the ROM-claimed driver binding vs a known-openable InstallDriverFromMemory binding.");
      MoveTo(8,36); DrawString("\pThe v69 open crash (0x9141003C) means the claimed driver is un-openable; this finds the malformed field.");
      MoveTo(8,52); DrawString("\pREAD-ONLY, no open, no MMIO, no mount. Reboot before each run (the reference install is resident)."); }

    Say("=== SiI3512 v70 : driver-binding DIFF (CLAIMED vs InstallDriverFromMemory reference) ===");
    Say("  v69 open crash = wild UPP 0x9141003C in DriverLoaderLib; dCtlWindow=0xFFFFFFFF. NO open here.");
    Say("");

    if (sil_find_node(&card)!=noErr){ Say("card NOT FOUND"); goto finish; }
    Say("[*] card node properties (driver-ref / driver-ptr lock the claim):");
    DumpNodeProps(&card);
    Say("");

    /* [1] the node's bound driver = what the ROM family-expert claim created */
    Say("[1] GetDriverForDevice(node) -- the ROM-claimed binding ON the node:");
    { CFragConnectionID cid=0; DriverEntryPointPtr fmain=0; DriverDescriptionPtr dsc=0;
      OSErr e = GetDriverForDevice(&card,&cid,&fmain,&dsc);
      Str255 L; L[0]=0; PCat(L,"    err="); PCatDec(L,(long)e);
      PCat(L,"  connID=0x"); PCatHex(L,(unsigned long)cid,8);
      PCat(L,"  fragMain=0x"); PCatHex(L,(unsigned long)fmain,8);
      PCat(L,"  desc=0x"); PCatHex(L,(unsigned long)dsc,8); Out(L);
      DumpWordsG("    fragMain",(unsigned long)fmain,4); }
    Say("");

    /* [2] CLAIMED driver-ref -> full binding dump (read-only) */
    Say("[2] CLAIMED driver (node 'driver-ref'):");
    { RegPropertyValueSize sz=(RegPropertyValueSize)sizeof(short); short drf=0;
      if (RegistryPropertyGet(&card,"driver-ref",&drf,&sz)==noErr && drf!=0){
        claimedRef=(DriverRefNum)drf;
        DumpDriverInfo("CLAIMED", claimedRef);
      } else Say("    NO driver-ref -- is the v65/v69 ROM installed + card claimed?"); }
    { RegPropertyValueSize sz=(RegPropertyValueSize)sizeof(long); long dp=0;
      if (RegistryPropertyGet(&card,"driver-ptr",&dp,&sz)==noErr){
        Str255 L; L[0]=0; PCat(L,"    node 'driver-ptr'=0x"); PCatHex(L,(unsigned long)dp,8); Out(L);
        DumpWordsG("    driver-ptr",(unsigned long)dp,8); } }
    Say("");

    /* [3] KNOWN-OPENABLE reference: install our SAME PEF the ordinary way. v68 proved
     *     this binding's kOpen dispatch works. Do NOT open it -- only dump the binding. */
    Say("[3] REFERENCE: InstallDriverFromMemory(our embedded PEF), then dump its binding:");
    pefH = GetResource('sPEF',128);
    if(!pefH){ Say("    embedded 'sPEF' 128 NOT FOUND -- cannot build the reference"); }
    else {
      HLock(pefH); pefLen=GetHandleSize(pefH);
      { Str255 L; L[0]=0; PCat(L,"    PEF len="); PCatDec(L,pefLen); Out(L); }
      err = InstallDriverFromMemory(*pefH, pefLen, "\pSiI3512SATA-ref", &card, 48, 127, &instRef);
      { Str255 L; L[0]=0; PCat(L,"    InstallDriverFromMemory err="); PCatDec(L,(long)err);
        PCat(L,"  instRef="); PCatDec(L,(long)instRef); Out(L); }
      if(err==noErr && instRef!=0) DumpDriverInfo("INSTALLED-REF", instRef);
    }
    Say("");
    Say("[=] DIFF CLAIMED vs INSTALLED-REF: connID / fragMain / dCtlDriver / dCtlFlags /");
    Say("    dCtlWindow / openCount. The field that differs is the open blocker.");

finish:
    Say("");
    Say("--- send 'SiI3512 Probe Log' + 'SiI3512 Driver Log' ---");
    if (gLogRef) FSClose(gLogRef);
    /* draw the accumulated log into the status window; wait for a click/key to quit
     * (the reference driver persists after quit; reboot to remove). */
    if(win){ SetPort((GrafPtr)win); TextFont(kFontIDMonaco); TextSize(9);
      EraseRect(&win->portRect);
      y=12; for(i=0;i<gN&&y<456;i++){MoveTo(8,y);DrawString(gLines[i]);y+=12;}
      { long dl=TickCount()+120*60; while(TickCount()<dl)
          if(WaitNextEvent(mDownMask|keyDownMask,&evt,6,NULL)&&(evt.what==mouseDown||evt.what==keyDown))break; }
      DisposeWindow(win); }
    return 0;
}
