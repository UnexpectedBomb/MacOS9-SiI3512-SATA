/*
 * sil3512_scsi.c — SCSI Manager 4.3 SIM registration + SCSI-to-ATA translation
 * layer (SATL), Milestone 3. This is the piece that makes drives on the card
 * appear to OS 9 as SCSI targets, so Drive Setup / the File Manager mount them.
 *
 * FirmTek's SeriTek cards present their SATA drives as a SCSI bus; we do the
 * same. We register a bus with SCSIRegisterBus and supply a SIM action routine;
 * incoming SCSIExecIO parameter blocks carry a CDB that we translate to ATA
 * taskfile/DMA operations via sil3512_ata.c.
 *
 * Structure and the register set are grounded in the OS 9 <SCSI.h> API; runtime
 * (callback timing, sync vs async completion, interrupt-driven I/O) is confirmed
 * at hardware bring-up (M5). ExecIO here completes synchronously (polled DMA).
 */
#include <MacTypes.h>
#include <SCSI.h>
#include <DriverServices.h>
#include "sil3512.h"

/* SCSI status byte */
#define SCSI_STATUS_GOOD    0x00
#define SCSI_STATUS_CHECK   0x02

#define kSCSICDBIsPointer   (1UL << 24)   /* scsiFlags bit kbSCSICDBIsPointer */

static UInt16               gBusID;
static SCSIMakeCallbackUPP  gMakeCallback;
static UInt16               gIdBuf[256];   /* scratch IDENTIFY buffer */

/* Write lockout. Was default OFF (read-only safety during read-path validation).
 * v27 turns it ON: the driver's read/write path is fully HW-validated and we now
 * deliberately WRITE — DIZero reformats the SSD (user data is expendable) to a
 * clean OS-9-native HFS volume that OS 9 can mount (the OS-X-made wrapped/journaled
 * HFS+ crashes OS 9's own mounter; see project memory). */
int gSilWriteEnabled = 1;

UInt16 sil_scsi_busid(void) { return gBusID; }

/* --- tiny local mem helpers (avoid extra header deps) --- */
static void mzero(void *p, UInt32 n) { UInt8 *d = (UInt8 *)p; while (n--) *d++ = 0; }
static void mcopy(void *dst, const void *src, UInt32 n)
{ UInt8 *d = (UInt8 *)dst; const UInt8 *s = (const UInt8 *)src; while (n--) *d++ = *s++; }
static void put_be32(UInt8 *p, UInt32 v)
{ p[0] = (UInt8)(v >> 24); p[1] = (UInt8)(v >> 16); p[2] = (UInt8)(v >> 8); p[3] = (UInt8)v; }

static void complete(void *pb) { InvokeSCSIMakeCallbackUPP(pb, gMakeCallback); }

/* Copy up to scsiDataLength bytes of a data-in reply into the client buffer. */
static void reply_in(SCSI_IO *io, const void *src, UInt32 srcLen)
{
    UInt32 n = (srcLen < io->scsiDataLength) ? srcLen : io->scsiDataLength;
    if (io->scsiDataPtr && n) mcopy(io->scsiDataPtr, src, n);
    io->scsiDataResidual = (long)(io->scsiDataLength - n);
}

/* --- SATL: one SCSIExecIO --------------------------------------------------- */

static void satl_execio(SCSI_IO *io)
{
    int port = io->scsiDevice.targetID;
    UInt8 *cdb = (io->scsiFlags & kSCSICDBIsPointer)
                     ? io->scsiCDB.cdbPtr : io->scsiCDB.cdbBytes;
    sil_port_state *ps;

    io->scsiSCSIstatus = SCSI_STATUS_GOOD;
    io->scsiResult = noErr;
    io->scsiDataResidual = (long)io->scsiDataLength;

    /* map target -> port; LUN must be 0 */
    if (port < 0 || port >= SIL3512_N_PORTS || io->scsiDevice.LUN != 0 ||
        !gSil.port[port].present) {
        io->scsiResult = scsiSelectTimeout;
        complete(io);
        return;
    }
    ps = &gSil.port[port];

    switch (cdb[0]) {
    case 0x00:                                          /* TEST UNIT READY */
        break;

    case 0x12: {                                        /* INQUIRY */
        UInt8 inq[36];
        mzero(inq, sizeof(inq));
        inq[0] = 0x00;                                  /* direct-access device */
        inq[1] = 0x00;                                  /* not removable */
        inq[2] = 0x02;                                  /* SCSI-2 */
        inq[3] = 0x02;                                  /* response data format */
        inq[4] = 31;                                    /* additional length */
        mcopy(&inq[8], "SiI3512 ", 8);                  /* vendor id */
        mcopy(&inq[16], ps->model, 16);                 /* product id (from IDENTIFY) */
        mcopy(&inq[32], "0001", 4);                     /* revision */
        reply_in(io, inq, sizeof(inq));
        break;
    }

    case 0x25: {                                        /* READ CAPACITY(10) */
        UInt8 cap[8];
        /* last LBA; if the drive is > 2 TB, report 0xFFFFFFFF per SCSI spec to
         * signal "capacity exceeds READ CAPACITY(10)" (OS 9 tops out at 2 TB). */
        UInt32 lastLBA = ps->oversize ? 0xFFFFFFFFUL
                                      : (ps->nSectors ? ps->nSectors - 1 : 0);
        put_be32(&cap[0], lastLBA);
        put_be32(&cap[4], 512);                         /* block length */
        reply_in(io, cap, sizeof(cap));
        break;
    }

    case 0x28:                                          /* READ(10) */
    case 0x2A: {                                        /* WRITE(10) */
        int isWrite = (cdb[0] == 0x2A);
        UInt32 lba = ((UInt32)cdb[2] << 24) | ((UInt32)cdb[3] << 16) |
                     ((UInt32)cdb[4] << 8)  | cdb[5];
        UInt32 remaining = (UInt32)(((UInt16)cdb[7] << 8) | cdb[8]);  /* sectors */
        UInt8 *bufp = io->scsiDataPtr;
        /* per-command cap = min(what the PRD table covers, what the ATA command
         * encodes: 256 sectors for LBA28, 65536 for LBA48). Chunking here means
         * the total request size is unbounded — only per-command size is capped. */
        UInt32 prdCap = sil_dma_max_sectors();
        UInt32 cmdCap = ps->lba48 ? 65536UL : 256UL;
        UInt32 chunkCap = (prdCap < cmdCap) ? prdCap : cmdCap;
        int failed = 0;

        if (isWrite && !gSilWriteEnabled) {             /* write lockout (data safety) */
            io->scsiSCSIstatus = SCSI_STATUS_CHECK;
            io->scsiResult = scsiNonZeroStatus;
            break;
        }

        while (remaining > 0) {
            UInt32 n = (remaining < chunkCap) ? remaining : chunkCap;
            UInt32 bytes = n * 512UL;
            UInt32 prdPhys; void *cookie;

            if (!sil_dma_prepare(bufp, bytes, isWrite, &prdPhys, &cookie)) {
                failed = 1; break;
            }
            if (!sil_ata_rw_dma(&gSil, port, lba, (UInt16)n, prdPhys, isWrite)) {
                sil_dma_complete(cookie, bytes);
                failed = 1; break;
            }
            sil_dma_complete(cookie, bytes);
            lba += n; bufp += bytes; remaining -= n;
        }

        if (failed) {
            io->scsiSCSIstatus = SCSI_STATUS_CHECK;
            io->scsiResult = scsiNonZeroStatus;
        } else {
            io->scsiDataResidual = (long)(remaining * 512UL);   /* 0 on success */
        }
        break;
    }

    case 0x03: {                                        /* REQUEST SENSE */
        UInt8 sense[18];
        mzero(sense, sizeof(sense));
        sense[0] = 0x70;                                /* current errors, fixed */
        sense[7] = 10;                                  /* additional sense length */
        reply_in(io, sense, sizeof(sense));
        break;
    }

    default:                                            /* unsupported CDB */
        io->scsiSCSIstatus = SCSI_STATUS_CHECK;
        io->scsiResult = scsiNonZeroStatus;
        break;
    }

    if (io->scsiSCSIstatus != SCSI_STATUS_GOOD && io->scsiResult == noErr)
        io->scsiResult = scsiNonZeroStatus;
    complete(io);
}

/* --- SCSIBusInquiry --------------------------------------------------------- */

static void bus_inquiry(SCSIBusInquiryPB *pb)
{
    pb->scsiEngineCount = 0;
    pb->scsiMaxTransferType = 1;
    pb->scsiDataTypes = 0;
    pb->scsiFeatureFlags = 0;
    pb->scsiVersionNumber = 2;                          /* SCSI-2 */
    pb->scsiHBAInquiry = 0;
    pb->scsiInitiatorID = 7;                            /* conventional HBA id */
    pb->scsiIOFlagsSupported = 0;
    pb->scsiMaxLUN = 0;                                 /* one LUN per target */
    pb->scsiResult = noErr;
}

/* --- SIM entry points ------------------------------------------------------- */

static pascal OSErr sim_init(Ptr simGlobals) { (void)simGlobals; return noErr; }

static pascal void sim_action(void *scsiPB, Ptr simGlobals)
{
    SCSIHdr *hdr = (SCSIHdr *)scsiPB;
    (void)simGlobals;
    switch (hdr->scsiFunctionCode) {
    case SCSIExecIO:
        satl_execio((SCSI_IO *)scsiPB);                 /* completes itself */
        return;
    case SCSIBusInquiry:
        bus_inquiry((SCSIBusInquiryPB *)scsiPB);
        break;
    default:
        hdr->scsiResult = noErr;                        /* accept/no-op others */
        break;
    }
    complete(scsiPB);
}

/* --- registration ----------------------------------------------------------- */

OSStatus sil_scsi_register(void)
{
    SIMInitInfo init;
    OSErr err;
    int p;

    /* IDENTIFY present ports so INQUIRY/READ CAPACITY have real data. */
    for (p = 0; p < SIL3512_N_PORTS; p++) {
        if (gSil.port[p].present && sil_ata_identify(&gSil, p, gIdBuf))
            sil_ata_parse_identify(&gSil.port[p], gIdBuf);
    }

    mzero(&init, sizeof(init));
    init.staticSize     = 4;                            /* XPT allocates SIMstaticPtr */
    init.SIMInit        = NewSIMInitUPP(sim_init);
    init.SIMAction      = NewSIMActionUPP(sim_action);
    init.ioPBSize       = (UInt16)sizeof(SCSI_IO);
    init.oldCallCapable = false;
    init.simRegEntry    = (Ptr)&gSil.node;              /* our PCI node (New World) */

    err = SCSIRegisterBus(&init);
    if (err != noErr) return err;

    gBusID = init.busID;                                /* XPT-assigned bus number */
    gMakeCallback = init.MakeCallback;                  /* XPT completion glue */
    return noErr;
}
