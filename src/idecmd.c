#include "ident.h"

/* idecmd.c - task file access, PIO and bus master DMA transfers, polled and
   interrupt-completed commands, transfer mode selection, Intel timing */

/* ------------------------------------------------------------------ */
/* Register helpers                                                    */
/* ------------------------------------------------------------------ */

UCHAR
IdeAltStatus(IN PIDE_CHANNEL Ch)
{
    return IDE_INB(Ch->Ctl);
}

/* Poll the alternate status register until (status & Mask) == Value */
BOOLEAN
IdeWaitStatus(IN PIDE_CHANNEL Ch, IN UCHAR Mask, IN UCHAR Value, IN ULONG TimeoutUsec)
{
    ULONG elapsed = 0;
    UCHAR status;

    for (;;) {
        status = IdeAltStatus(Ch);
        if ((status & Mask) == Value) return TRUE;
        if (elapsed >= TimeoutUsec) return FALSE;
        IDE_POLL_STEP(elapsed, 1);
    }
}

/* Device register with DEV; the device needs 400 ns before its registers
   are valid */
VOID
IdeSelectUnit(IN PIDE_CHANNEL Ch, IN ULONG Unit)
{
    IDE_OUTB(Ch->Cmd + IDE_REG_DEVICE, IDE_DEV_DEFAULT | (Unit ? IDE_DEV_SLAVE : 0));
    Ch->SelectedUnit = (UCHAR)Unit;
    IdeAltStatus(Ch);
    IdeAltStatus(Ch);
    ScsiPortStallExecution(1);
}

VOID
IdeSetControl(IN PIDE_CHANNEL Ch, IN UCHAR Bits)
{
    IDE_OUTB(Ch->Ctl, IDE_CTL_BASE | Bits);
}

/* Registers after a command: Status, Error, count, LBA, Device; the
   previous (HOB) bytes for 48-bit commands */
static VOID
IdeReadResult(IN PIDE_CHANNEL Ch, IN BOOLEAN Is48Bit, IN UCHAR Status)
{
    PIDE_ATA_RESULT r = &Ch->Result;
    PUCHAR cmd = Ch->Cmd;

    r->Status = Status;
    r->Error = IDE_INB(cmd + IDE_REG_ERROR);
    r->CountLow = IDE_INB(cmd + IDE_REG_COUNT);
    r->Lba0 = IDE_INB(cmd + IDE_REG_LBA0);
    r->Lba1 = IDE_INB(cmd + IDE_REG_LBA1);
    r->Lba2 = IDE_INB(cmd + IDE_REG_LBA2);
    r->Device = IDE_INB(cmd + IDE_REG_DEVICE);
    r->CountHigh = 0;
    r->Lba3 = 0;
    r->Lba4 = 0;
    r->Lba5 = 0;
    if (Is48Bit) {
        IdeSetControl(Ch, IDE_CTL_HOB | IDE_CTL_NIEN);
        r->CountHigh = IDE_INB(cmd + IDE_REG_COUNT);
        r->Lba3 = IDE_INB(cmd + IDE_REG_LBA0);
        r->Lba4 = IDE_INB(cmd + IDE_REG_LBA1);
        r->Lba5 = IDE_INB(cmd + IDE_REG_LBA2);
        IdeSetControl(Ch, IDE_CTL_NIEN);
    }
}

/* ------------------------------------------------------------------ */
/* Channel reset                                                       */
/* ------------------------------------------------------------------ */

/* Software reset of both devices on the channel (SRST). Returns FALSE
   when a device stays busy. Interrupts are left disabled (nIEN). */
BOOLEAN
IdeResetChannel(IN PHW_DEVICE_EXTENSION HwInit, IN ULONG Channel)
{
    PIDE_CHANNEL ch = &HwInit->Channels[Channel];
    ULONG ms;
    UCHAR status;

    ch->SelectedUnit = 0xFF;
    if (ch->Bm != NULL) {
        IDE_OUTB(ch->Bm + BM_REG_COMMAND, 0);
        IDE_OUTB(ch->Bm + BM_REG_STATUS, IDE_INB(ch->Bm + BM_REG_STATUS) | BM_STS_INTERRUPT | BM_STS_ERROR);
    }

    IdeSetControl(ch, IDE_CTL_SRST | IDE_CTL_NIEN);
    ScsiPortStallExecution(20);
    IdeSetControl(ch, IDE_CTL_NIEN);
    ScsiPortStallExecution(2000);

    /* Both devices: BSY may take long after a reset (spin-up) */
    for (ms = 0; ms < IDE_RESET_TIMEOUT_MS; ms++) {
        status = IdeAltStatus(ch);
        if (status == 0xFF || !(status & IDE_STS_BSY)) break;
        ScsiPortStallExecution(1000);
    }
    ch->NeedsSetup = TRUE;
    if (ms >= IDE_RESET_TIMEOUT_MS) {
        IDE_DBG((IDE_PFX "Channel %lu: still busy %lu ms after reset (status %02lX)\n",
                 Channel, ms, (ULONG)status));
        return FALSE;
    }
    IDE_DBG((IDE_PFX "Channel %lu: reset done in %lu ms\n", Channel, ms));
    return TRUE;
}

/* Stop the bus master and keep the devices quiet (no interrupts) */
VOID
IdeStopChannel(IN PHW_DEVICE_EXTENSION HwInit, IN ULONG Channel)
{
    PIDE_CHANNEL ch = &HwInit->Channels[Channel];

    if (!ch->Present) return;
    if (ch->Bm != NULL) {
        IDE_OUTB(ch->Bm + BM_REG_COMMAND, 0);
        IDE_OUTB(ch->Bm + BM_REG_STATUS, IDE_INB(ch->Bm + BM_REG_STATUS) | BM_STS_INTERRUPT | BM_STS_ERROR);
    }
    IdeSetControl(ch, IDE_CTL_NIEN);
}

/* ------------------------------------------------------------------ */
/* PRD table                                                           */
/* ------------------------------------------------------------------ */

static BOOLEAN
IdeBuildPrd(
    IN PHW_DEVICE_EXTENSION HwInit,
    IN PSCSI_REQUEST_BLOCK Srb,
    IN PIDE_PRD_ENTRY Prd,
    IN PVOID Buffer,
    IN ULONG Length
)
{
    PUCHAR virt = (PUCHAR)Buffer;
    ULONG left = Length;
    ULONG index = 0;
    ULONG segLen;
    ULONG toPage;
    SCSI_PHYSICAL_ADDRESS phys;

    /* SFF-8038i asks for even addresses and counts; some controllers need
       dword alignment. Anything else goes with PIO (the caller retries). */
    if ((Length & 3) || (Length == 0)) return FALSE;

    while (left > 0 && index < IDE_PRD_ENTRIES) {
        phys = ScsiPortGetPhysicalAddress(HwInit, Srb, virt, &segLen);
        if ((phys.LowPart == 0 && phys.HighPart == 0) || phys.HighPart != 0 || (phys.LowPart & 3)) {
            return FALSE;
        }
        if (segLen > left) segLen = left;
        toPage = 4096 - (phys.LowPart & 0xFFF);
        if (segLen > toPage) segLen = toPage;
        segLen &= ~3UL;
        if (segLen == 0) return FALSE;

        Prd[index].Address = phys.LowPart;
        Prd[index].CountEot = segLen & 0xFFFF;
        virt += segLen;
        left -= segLen;
        index++;
    }
    if (left > 0) return FALSE;
    Prd[index - 1].CountEot |= BM_PRD_EOT;
    return TRUE;
}

/* ------------------------------------------------------------------ */
/* Task file                                                           */
/* ------------------------------------------------------------------ */

static ULONG
IdeTimeoutUsec(IN PSCSI_REQUEST_BLOCK Srb, IN PATA_REQUEST Req)
{
    ULONG sec;

    if (Req->TimeoutSec != 0) {
        sec = Req->TimeoutSec;
    } else if (Srb == NULL) {
        sec = IDE_POLL_TIMEOUT_INTERNAL_SEC;
    } else {
        sec = Srb->TimeOutValue ? Srb->TimeOutValue : IDE_POLL_TIMEOUT_DEFAULT_SEC;
        if (sec > 1) sec--;
        if (sec > IDE_POLL_TIMEOUT_MAX_SEC) sec = IDE_POLL_TIMEOUT_MAX_SEC;
    }
    return sec * 1000000UL;
}

/* Select the device and wait until it accepts a command. The device the
   last command went to is still selected: only its status is checked. */
static BOOLEAN
IdePrepareDevice(IN PIDE_CHANNEL Ch, IN ULONG Unit)
{
    if (Ch->SelectedUnit == Unit) {
        if ((IdeAltStatus(Ch) & (IDE_STS_BSY | IDE_STS_DRQ)) == 0) return TRUE;
        Ch->SelectedUnit = 0xFF;
    }
    IdeSelectUnit(Ch, Unit);
    if (!IdeWaitStatus(Ch, IDE_STS_BSY | IDE_STS_DRQ, 0, IDE_BUSY_WAIT_MS * 1000UL)) {
        Ch->SelectedUnit = 0xFF;
        return FALSE;
    }
    IdeSelectUnit(Ch, Unit);
    return TRUE;
}

/* Bus master registers for a transfer: PRD pointer, direction, status
   bits cleared; not started yet */
static BOOLEAN
IdePrepareDma(IN PHW_DEVICE_EXTENSION HwInit, IN PIDE_CHANNEL Ch, IN PSCSI_REQUEST_BLOCK Srb, IN PATA_REQUEST Req)
{
    if (!IdeBuildPrd(HwInit, Srb, Ch->Prd, Req->DataBuffer, Req->DataBufferLen)) {
        return FALSE;
    }
    IDE_OUTB(Ch->Bm + BM_REG_COMMAND, Req->IsWrite ? 0 : BM_CMD_READ);
    IDE_OUTD(Ch->Bm + BM_REG_PRD, Ch->PrdPhysical);
    IDE_OUTB(Ch->Bm + BM_REG_STATUS, IDE_INB(Ch->Bm + BM_REG_STATUS) | BM_STS_INTERRUPT | BM_STS_ERROR);
    return TRUE;
}

/* Write the task file and the command register */
static VOID
IdeWriteTaskFile(IN PIDE_CHANNEL Ch, IN PATA_REQUEST Req, IN BOOLEAN UseDma)
{
    PUCHAR cmd = Ch->Cmd;
    UCHAR dev = (UCHAR)(Req->Unit ? IDE_DEV_SLAVE : 0);

    if (Req->IsAtapi) {
        ULONG byteCount = (Req->DataBufferLen > 0xFFFE) ? 0xFFFE : Req->DataBufferLen;
        UCHAR features = 0;
        PIDE_DEVICE d = &Ch->Devices[Req->Unit];

        if (UseDma) {
            features = IDE_PACKET_DMA;
            if (d->DmaDir && !Req->IsWrite) features |= IDE_PACKET_DMADIR;
        }
        IDE_OUTB(cmd + IDE_REG_FEATURES, features);
        IDE_OUTB(cmd + IDE_REG_COUNT, 0);
        IDE_OUTB(cmd + IDE_REG_LBA0, 0);
        IDE_OUTB(cmd + IDE_REG_LBA1, byteCount & 0xFF);
        IDE_OUTB(cmd + IDE_REG_LBA2, (byteCount >> 8) & 0xFF);
        IDE_OUTB(cmd + IDE_REG_DEVICE, IDE_DEV_DEFAULT | dev);
        IDE_OUTB(cmd + IDE_REG_COMMAND, IDE_COMMAND_PACKET);
        return;
    }

    if (Req->Is48Bit) {
        IDE_OUTB(cmd + IDE_REG_FEATURES, Req->Features >> 8);
        IDE_OUTB(cmd + IDE_REG_COUNT, Req->SectorCount >> 8);
        IDE_OUTB(cmd + IDE_REG_LBA0, Req->Lba.Low >> 24);
        IDE_OUTB(cmd + IDE_REG_LBA1, Req->Lba.High);
        IDE_OUTB(cmd + IDE_REG_LBA2, Req->Lba.High >> 8);
    }
    IDE_OUTB(cmd + IDE_REG_FEATURES, Req->Features);
    IDE_OUTB(cmd + IDE_REG_COUNT, Req->SectorCount);      /* 256 / 65536 as 0 */
    IDE_OUTB(cmd + IDE_REG_LBA0, Req->Lba.Low);
    IDE_OUTB(cmd + IDE_REG_LBA1, Req->Lba.Low >> 8);
    IDE_OUTB(cmd + IDE_REG_LBA2, Req->Lba.Low >> 16);

    if (Req->RawDevice) {
        dev |= (UCHAR)(Req->Device & ~IDE_DEV_SLAVE);
    } else if (Req->Chs) {
        /* Lba.Low: [7:0] sector, [15:8] head, [31:16] cylinder */
        dev |= (UCHAR)(IDE_DEV_DEFAULT | ((Req->Lba.Low >> 8) & 0x0F));
    } else if (Req->Is48Bit) {
        dev |= IDE_DEV_LBA;
    } else {
        dev |= (UCHAR)(IDE_DEV_DEFAULT | IDE_DEV_LBA | ((Req->Lba.Low >> 24) & 0x0F));
    }
    IDE_OUTB(cmd + IDE_REG_DEVICE, dev);
    IDE_OUTB(cmd + IDE_REG_COMMAND, Req->Command);
}

/* ATAPI: the packet after the device raised DRQ for it */
static BOOLEAN
IdeSendPacket(IN PIDE_CHANNEL Ch, IN PATA_REQUEST Req, IN ULONG TimeoutUsec)
{
    PIDE_DEVICE d = &Ch->Devices[Req->Unit];
    USHORT packet[8];
    ULONG words = d->Cdb16 ? 8 : 6;
    ULONG i;
    UCHAR status;

    if (!IdeWaitStatus(Ch, IDE_STS_BSY, 0, TimeoutUsec)) return FALSE;
    status = IdeAltStatus(Ch);
    if ((status & IDE_STS_ERR) || !(status & IDE_STS_DRQ)) return FALSE;

    ZeroMemoryBytes(packet, sizeof(packet));
    for (i = 0; i < Req->CdbLength && i < words * 2; i++) {
        ((PUCHAR)packet)[i] = Req->Cdb[i];
    }
    IDE_OUTSW(Ch->Cmd + IDE_REG_DATA, packet, words);
    return TRUE;
}

static VOID IdeSimplexWait(IN PHW_DEVICE_EXTENSION HwInit, IN ULONG Channel);

/* ------------------------------------------------------------------ */
/* PIO data phase                                                      */
/* ------------------------------------------------------------------ */

/* One DRQ block in or out. ATAPI devices say how much they move per
   phase; ATA moves one sector (or the IDENTIFY/log 512 bytes). */
static VOID
IdePioBlock(IN PIDE_CHANNEL Ch, IN BOOLEAN IsAtapi, IN BOOLEAN IsWrite, IN OUT PUCHAR *Buffer, IN OUT PULONG Left,
            IN ULONG BlockSize)
{
    PUCHAR data = Ch->Cmd + IDE_REG_DATA;
    ULONG n, words, i;
    USHORT w;

    if (IsAtapi) {
        BlockSize = (ULONG)IDE_INB(Ch->Cmd + IDE_REG_LBA1) | ((ULONG)IDE_INB(Ch->Cmd + IDE_REG_LBA2) << 8);
        if (BlockSize == 0) BlockSize = 2;
    }
    n = (BlockSize > *Left) ? *Left : BlockSize;
    words = n / 2;

    if (IsWrite) {
        if (words != 0) IDE_OUTSW(data, *Buffer, words);
        if (n & 1) {
            w = (*Buffer)[n - 1];
            IDE_OUTW(data, w);
        }
        /* The device wants more than the request holds: pad */
        for (i = (n + 1) & ~1UL; i < BlockSize; i += 2) IDE_OUTW(data, 0);
    } else {
        if (words != 0) IDE_INSW(data, *Buffer, words);
        if (n & 1) {
            w = IDE_INW(data);
            (*Buffer)[n - 1] = (UCHAR)w;
        }
        /* More than the request holds: drain it */
        for (i = (n + 1) & ~1UL; i < BlockSize; i += 2) IDE_INW(data);
    }
    *Buffer += n;
    *Left -= n;
}

/* ------------------------------------------------------------------ */
/* Synchronous commands (Fast Polling)                                 */
/* ------------------------------------------------------------------ */

/* Issue one command and poll it to completion; a DMA command in flight
   on the channel is completed first. */
ULONG
IdeRunCommand(
    IN PHW_DEVICE_EXTENSION HwInit,
    IN ULONG Channel,
    IN PSCSI_REQUEST_BLOCK Srb,
    IN PATA_REQUEST Req
)
{
    PIDE_CHANNEL ch = &HwInit->Channels[Channel];
    PIDE_DEVICE dev = &ch->Devices[Req->Unit];
    ULONG timeoutUsec, elapsed;
    ULONG result = IDE_CMD_OK;
    BOOLEAN useDma, timedOut = FALSE;
    UCHAR status = 0, bmsts = 0;
    PUCHAR buffer;
    ULONG left, blockSize;

    if (ch->Outstanding != NULL) IdeDrainChannel(HwInit, Channel);

    timeoutUsec = IdeTimeoutUsec(Srb, Req);
    useDma = (BOOLEAN)(Req->UseDma && ch->Bm != NULL && Req->DataBufferLen != 0 && !Req->UseBounce);
    if (useDma) IdeSimplexWait(HwInit, Channel);
    IdeChannelAlive(HwInit, Channel);
    if (useDma && !IdeBusMasterAlive(HwInit, Channel)) return IDE_CMD_HBA_ERROR;

    if (!IdePrepareDevice(ch, Req->Unit)) {
        IDE_DBG((IDE_PFX "Channel %lu.%lu: device busy before command %02lX (status %02lX)\n",
                 Channel, (ULONG)Req->Unit, (ULONG)Req->Command, (ULONG)IdeAltStatus(ch)));
        IdeResetChannel(HwInit, Channel);
        dev->LastStatus = IdeAltStatus(ch);
        dev->LastError = 0;
        return IDE_CMD_HBA_ERROR;
    }
    IdeSetControl(ch, IDE_CTL_NIEN);

    if (useDma && !IdePrepareDma(HwInit, ch, Srb, Req)) {
        IDE_DBG((IDE_PFX "Channel %lu: cannot build PRDs for %lu bytes\n", Channel, Req->DataBufferLen));
        return IDE_CMD_BAD_BUFFER;
    }

    IdeWriteTaskFile(ch, Req, useDma);

    if (Req->IsAtapi && !IdeSendPacket(ch, Req, timeoutUsec)) {
        status = IdeAltStatus(ch);
        result = (status & IDE_STS_ERR) ? IDE_CMD_DEVICE_ERROR : IDE_CMD_HBA_ERROR;
        timedOut = (BOOLEAN)(result == IDE_CMD_HBA_ERROR);
        goto done;
    }

    elapsed = 0;
    if (useDma) {
        IDE_OUTB(ch->Bm + BM_REG_COMMAND, (Req->IsWrite ? 0 : BM_CMD_READ) | BM_CMD_START);
        for (;;) {
            bmsts = IDE_INB(ch->Bm + BM_REG_STATUS);
            status = IdeAltStatus(ch);
            if (bmsts & BM_STS_ERROR) {
                result = IDE_CMD_HBA_ERROR;
                break;
            }
            if (!(status & (IDE_STS_BSY | IDE_STS_DRQ)) &&
                ((bmsts & BM_STS_INTERRUPT) || !(bmsts & BM_STS_ACTIVE))) {
                break;
            }
            if ((status & IDE_STS_ERR) && !(status & IDE_STS_BSY)) {
                result = IDE_CMD_DEVICE_ERROR;
                break;
            }
            if (elapsed >= timeoutUsec) {
                result = IDE_CMD_HBA_ERROR;
                timedOut = TRUE;
                break;
            }
            IDE_POLL_STEP(elapsed, 1);
        }
        IDE_OUTB(ch->Bm + BM_REG_COMMAND, 0);
        IDE_OUTB(ch->Bm + BM_REG_STATUS, bmsts | BM_STS_INTERRUPT | BM_STS_ERROR);
        status = IDE_INB(ch->Cmd + IDE_REG_STATUS);
        if (result == IDE_CMD_OK && (status & IDE_STS_ERR)) result = IDE_CMD_DEVICE_ERROR;
    } else {
        buffer = Req->UseBounce ? ch->BounceBuffer : (PUCHAR)Req->DataBuffer;
        left = Req->DataBufferLen;
        /* 512 bytes at a time, then the status again: a DRQ block of a
           larger logical sector just continues (DRQ stays set), and an
           emulated device that hands out 512-byte blocks for such a sector
           is served too */
        blockSize = IDE_SECTOR_SIZE;

        for (;;) {
            /* BSY first, then the device tells what it wants */
            for (;;) {
                status = IdeAltStatus(ch);
                if (!(status & IDE_STS_BSY)) break;
                if (elapsed >= timeoutUsec) break;
                IDE_POLL_STEP(elapsed, 1);
            }
            if (status & IDE_STS_BSY) {
                result = IDE_CMD_HBA_ERROR;
                timedOut = TRUE;
                break;
            }
            if (status & IDE_STS_ERR) {
                result = IDE_CMD_DEVICE_ERROR;
                break;
            }
            if (!(status & IDE_STS_DRQ)) {
                /* Command over. An ATA transfer that stopped short is an error */
                if (left != 0 && !Req->IsAtapi) result = IDE_CMD_HBA_ERROR;
                break;
            }
            if (left == 0 && !Req->IsAtapi) {
                /* Device offers data we did not ask for */
                result = IDE_CMD_HBA_ERROR;
                break;
            }
            IdePioBlock(ch, Req->IsAtapi, Req->IsWrite, &buffer, &left, blockSize);
            /* Give the device its 400 ns before the status is read again */
            IdeAltStatus(ch);
        }
        status = IDE_INB(ch->Cmd + IDE_REG_STATUS);
        if (result == IDE_CMD_OK && (status & IDE_STS_ERR)) result = IDE_CMD_DEVICE_ERROR;
    }

done:
    IdeReadResult(ch, Req->Is48Bit, status);
    dev->LastStatus = status;
    dev->LastError = ch->Result.Error;
    /* A drive that ignores nIEN still sets the CMD64x latch */
    if (IDE_CHIP_IS_CMD(HwInit->Chip)) IdeChipClearInterrupt(HwInit, Channel);

    if (useDma) {
        if (result == IDE_CMD_OK) {
            dev->DmaErrors = 0;
        } else if (result == IDE_CMD_HBA_ERROR || (dev->LastError & IDE_ERROR_ICRC)) {
            if (++dev->DmaErrors >= IDE_DMA_ERROR_LIMIT) IdeDisableDma(HwInit, Channel, Req->Unit);
        }
    }

    if (result != IDE_CMD_OK) {
        IDE_DBG((IDE_PFX "Channel %lu.%lu: cmd %02lX %s, status %02lX error %02lX bm %02lX\n",
                 Channel, (ULONG)Req->Unit,
                 (ULONG)(Req->IsAtapi ? Req->Cdb[0] : Req->Command),
                 timedOut ? "timed out" : "failed",
                 (ULONG)status, (ULONG)ch->Result.Error, (ULONG)bmsts));
        if (timedOut || (status & (IDE_STS_BSY | IDE_STS_DRQ))) {
            IdeResetChannel(HwInit, Channel);
        }
    }
    return result;
}

/* Non-data ATA command to one device */
ULONG
IdeRunSimple(
    IN PHW_DEVICE_EXTENSION HwInit,
    IN ULONG Channel,
    IN ULONG Unit,
    IN UCHAR Command,
    IN USHORT Features,
    IN ULONG Count,
    IN ULONG TimeoutSec
)
{
    ATA_REQUEST req;

    ZeroMemoryBytes(&req, sizeof(req));
    req.Unit = (UCHAR)Unit;
    req.Command = Command;
    req.Features = Features;
    req.SectorCount = Count;
    req.TimeoutSec = TimeoutSec;
    return IdeRunCommand(HwInit, Channel, NULL, &req);
}

/* IDENTIFY (PACKET) DEVICE; the data stays in the channel's bounce buffer
   for IdeParseIdentify */
BOOLEAN
IdeIdentifyDevice(IN PHW_DEVICE_EXTENSION HwInit, IN ULONG Channel, IN ULONG Unit)
{
    PIDE_CHANNEL ch = &HwInit->Channels[Channel];
    PIDE_DEVICE dev = &ch->Devices[Unit];
    PUSHORT id = (PUSHORT)ch->BounceBuffer;
    ATA_REQUEST req;
    ULONG result;

    ZeroMemoryBytes(ch->BounceBuffer, 512);
    ZeroMemoryBytes(&req, sizeof(req));
    req.Unit = (UCHAR)Unit;
    req.Command = dev->IsAtapi ? IDE_COMMAND_IDENTIFY_PACKET : IDE_COMMAND_IDENTIFY_DEVICE;
    req.UseBounce = TRUE;
    req.DataBufferLen = 512;
    req.TimeoutSec = IDE_POLL_TIMEOUT_INTERNAL_SEC;

    result = IdeRunCommand(HwInit, Channel, NULL, &req);
    if (result == IDE_CMD_OK && (id[IDW_MODEL] != 0 || id[IDW_GENERAL_CONFIG] != 0) &&
        id[IDW_GENERAL_CONFIG] != 0xFFFF) {
        dev->IdentifyValid = TRUE;
    } else {
        dev->IdentifyValid = FALSE;
        IDE_DBG((IDE_PFX "Channel %lu.%lu: IDENTIFY failed (%lu), status %02lX error %02lX\n",
                 Channel, Unit, result, (ULONG)dev->LastStatus, (ULONG)dev->LastError));
    }
    return dev->IdentifyValid;
}

/* Write cache / look-ahead as set through MODE SELECT; the CHS translation
   for devices without LBA */
VOID
IdeApplyDeviceSettings(IN PHW_DEVICE_EXTENSION HwInit, IN ULONG Channel, IN ULONG Unit)
{
    PIDE_DEVICE dev = &HwInit->Channels[Channel].Devices[Unit];

    if (!dev->Present || dev->IsAtapi) return;

    if (!dev->LbaSupported && dev->Heads != 0 && dev->Spt != 0) {
        ATA_REQUEST req;
        ZeroMemoryBytes(&req, sizeof(req));
        req.Unit = (UCHAR)Unit;
        req.Command = IDE_COMMAND_INIT_DEV_PARAMS;
        req.SectorCount = dev->Spt;
        req.RawDevice = TRUE;
        req.Device = (UCHAR)(IDE_DEV_DEFAULT | ((dev->Heads - 1) & 0x0F));
        IdeRunCommand(HwInit, Channel, NULL, &req);
    }
    if (dev->WantWriteCache != 0) {
        IdeRunSimple(HwInit, Channel, Unit, IDE_COMMAND_SET_FEATURES,
                     (USHORT)((dev->WantWriteCache == 1) ? IDE_FEATURE_ENABLE_WRITE_CACHE : IDE_FEATURE_DISABLE_WRITE_CACHE), 0, 0);
    }
    if (dev->WantLookAhead != 0) {
        IdeRunSimple(HwInit, Channel, Unit, IDE_COMMAND_SET_FEATURES,
                     (USHORT)((dev->WantLookAhead == 1) ? IDE_FEATURE_ENABLE_LOOKAHEAD : IDE_FEATURE_DISABLE_LOOKAHEAD), 0, 0);
    }
    /* SPINDOWN=n: the drive's own standby timer (IDLE with a count), so it
       spins down after n idle minutes on any Windows version */
    if (HwInit->SpinDown != 0 && dev->RotationRate != 1) {
        IdeRunSimple(HwInit, Channel, Unit, IDE_COMMAND_IDLE, 0, HwInit->SpinDown, 0);
    }
}

/* ------------------------------------------------------------------ */
/* Intel PIIX / ICH timing registers                                   */
/* ------------------------------------------------------------------ */

#define INTEL_IDETIM_PRIMARY            0x40
#define INTEL_IDETIM_SECONDARY          0x42
#define INTEL_SIDETIM                   0x44
#define INTEL_UDMACTL                   0x48
#define INTEL_UDMATIM                   0x4A
#define INTEL_IDE_CONFIG                0x54

#define INTEL_IDETIM_TIME0              0x0001      /* fast timing (TIME1) */
#define INTEL_IDETIM_IE0                0x0002      /* IORDY sample */
#define INTEL_IDETIM_PPE0               0x0004      /* prefetch/posting */
#define INTEL_IDETIM_DTE0               0x0008      /* fast timing for DMA only */
#define INTEL_IDETIM_SITRE              0x4000      /* slave timing in SIDETIM */
#define INTEL_IDETIM_IDE                0x8000      /* decode enable */

/* ISP / RCT for PIO 0..4 (and the MWDMA modes mapped onto PIO 0/3/4) */
static const UCHAR g_IntelTimings[5][2] = { {0, 0}, {0, 0}, {1, 0}, {2, 1}, {2, 3} };

static VOID
IdeIntelSetPio(IN PHW_DEVICE_EXTENSION HwInit, IN ULONG Port, IN ULONG Unit, IN ULONG Pio, IN BOOLEAN Atapi)
{
    ULONG masterReg = Port ? INTEL_IDETIM_SECONDARY : INTEL_IDETIM_PRIMARY;
    ULONG master = IdePciReadConfig(HwInit, masterReg, 2);
    ULONG slave;
    ULONG control = 0;
    ULONG udma;

    if (Pio > 4) Pio = 4;
    if (Pio >= 2) control |= INTEL_IDETIM_TIME0;
    if (Pio >= 3) control |= INTEL_IDETIM_IE0;
    if (!Atapi) control |= INTEL_IDETIM_PPE0;

    if (Unit != 0) {
        master |= INTEL_IDETIM_SITRE;
        master &= ~0x0070;
        master |= control << 4;
        slave = IdePciReadConfig(HwInit, INTEL_SIDETIM, 1);
        slave &= Port ? 0x0F : 0xF0;
        slave |= (((ULONG)g_IntelTimings[Pio][0] << 2) | g_IntelTimings[Pio][1]) << (Port ? 4 : 0);
        IdePciWriteConfig(HwInit, INTEL_SIDETIM, 1, slave);
    } else {
        master &= ~0x3307;
        master |= control;
        master |= ((ULONG)g_IntelTimings[Pio][0] << 12) | ((ULONG)g_IntelTimings[Pio][1] << 8);
    }
    master |= INTEL_IDETIM_IDE;
    IdePciWriteConfig(HwInit, masterReg, 2, master);

    /* UDMA off until a UDMA mode is chosen */
    if (HwInit->Intel >= IDE_INTEL_PIIX4) {
        udma = IdePciReadConfig(HwInit, INTEL_UDMACTL, 1);
        udma &= ~(1UL << (2 * Port + Unit));
        IdePciWriteConfig(HwInit, INTEL_UDMACTL, 1, udma);
    }
}

static VOID
IdeIntelSetDma(IN PHW_DEVICE_EXTENSION HwInit, IN ULONG Port, IN ULONG Unit, IN UCHAR ModeType, IN ULONG Mode, IN ULONG Pio)
{
    ULONG masterReg = Port ? INTEL_IDETIM_SECONDARY : INTEL_IDETIM_PRIMARY;
    ULONG devid = 2 * Port + Unit;
    ULONG master, slave, udmaEnable = 0, udmaTiming, ideConf;
    ULONG control, neededPio, uSpeed, uClock;
    static const ULONG neededPioForMwdma[3] = { 0, 3, 4 };

    master = IdePciReadConfig(HwInit, masterReg, 2);
    if (HwInit->Intel >= IDE_INTEL_PIIX4) udmaEnable = IdePciReadConfig(HwInit, INTEL_UDMACTL, 1);

    if (ModeType == IDE_MODE_UDMA) {
        /* Clock (33/66/100 MHz) and divider */
        uSpeed = (2 - (Mode & 1) < Mode) ? 2 - (Mode & 1) : Mode;
        if (Mode == 5) uClock = 0x1000;
        else if (Mode > 2) uClock = 1;
        else uClock = 0;

        udmaEnable |= 1UL << devid;
        udmaTiming = IdePciReadConfig(HwInit, INTEL_UDMATIM, 2);
        udmaTiming &= ~(3UL << (4 * devid));
        udmaTiming |= uSpeed << (4 * devid);
        IdePciWriteConfig(HwInit, INTEL_UDMATIM, 2, udmaTiming);

        if (HwInit->Intel >= IDE_INTEL_ICH66) {
            ideConf = IdePciReadConfig(HwInit, INTEL_IDE_CONFIG, 2);
            ideConf &= ~(0x1001UL << devid);
            ideConf |= uClock << devid;
            IdePciWriteConfig(HwInit, INTEL_IDE_CONFIG, 2, ideConf);
        }
    } else {
        /* MWDMA runs on the PIO timings */
        if (Mode > 2) Mode = 2;
        neededPio = neededPioForMwdma[Mode];
        control = INTEL_IDETIM_TIME0 | INTEL_IDETIM_IE0;
        if (Pio < neededPio) control |= INTEL_IDETIM_DTE0;      /* PIO stays slow */

        if (Unit != 0) {
            master &= 0xFF4F;
            master |= control << 4;
            slave = IdePciReadConfig(HwInit, INTEL_SIDETIM, 1);
            slave &= Port ? 0x0F : 0xF0;
            slave |= (((ULONG)g_IntelTimings[neededPio][0] << 2) | g_IntelTimings[neededPio][1]) << (Port ? 4 : 0);
            IdePciWriteConfig(HwInit, INTEL_SIDETIM, 1, slave);
        } else {
            master &= 0xCCF4;
            master |= control;
            master |= ((ULONG)g_IntelTimings[neededPio][0] << 12) | ((ULONG)g_IntelTimings[neededPio][1] << 8);
        }
        master |= INTEL_IDETIM_IDE;
        IdePciWriteConfig(HwInit, masterReg, 2, master);
        udmaEnable &= ~(1UL << devid);
    }
    if (HwInit->Intel >= IDE_INTEL_PIIX4) IdePciWriteConfig(HwInit, INTEL_UDMACTL, 1, udmaEnable);
}

/* 80-conductor cable on this channel, as the ICH sees it */
static BOOLEAN
IdeIntelCable80(IN PHW_DEVICE_EXTENSION HwInit, IN ULONG Port)
{
    ULONG ideConf;

    if (HwInit->Intel < IDE_INTEL_ICH66) return FALSE;
    ideConf = IdePciReadConfig(HwInit, INTEL_IDE_CONFIG, 2);
    return (BOOLEAN)((ideConf & (0x30UL << (2 * Port))) != 0);
}

/* ------------------------------------------------------------------ */
/* Transfer mode                                                       */
/* ------------------------------------------------------------------ */

static ULONG
IdeHighestBit(IN ULONG Mask, IN ULONG Max)
{
    ULONG m, best = 0xFFFFFFFF;
    for (m = 0; m <= Max; m++) {
        if (Mask & (1UL << m)) best = m;
    }
    return best;
}

/* SET FEATURES: transfer mode */
static BOOLEAN
IdeSetXferMode(IN PHW_DEVICE_EXTENSION HwInit, IN ULONG Channel, IN ULONG Unit, IN UCHAR ModeType, IN ULONG Mode)
{
    ULONG count;

    if (ModeType == IDE_MODE_UDMA) count = IDE_XFER_UDMA | Mode;
    else if (ModeType == IDE_MODE_MWDMA) count = IDE_XFER_MWDMA | Mode;
    else if (Mode >= 3) count = IDE_XFER_PIO_FLOW | Mode;
    else count = IDE_XFER_PIO_DEFAULT;

    return (BOOLEAN)(IdeRunSimple(HwInit, Channel, Unit, IDE_COMMAND_SET_FEATURES,
                                  IDE_FEATURE_SET_TRANSFER_MODE, count, 0) == IDE_CMD_OK);
}

/* CMD64x: address setup and active/recovery for one device, at 33 MHz
   from the ATA timing tables (PIO 0-4, then MWDMA 0-2), as Linux
   pata_cmd64x computes them. The secondary channel's two devices share
   one address setup field, the slower one wins. */
static const UCHAR g_IdeCmdDrwtim[8] = { 0xAA, 0x76, 0x44, 0x33, 0x32, 0x88, 0x32, 0x32 };
static const UCHAR g_IdeCmdSetupClocks[8] = { 3, 2, 1, 1, 1, 2, 2, 1 };
static const UCHAR g_IdeCmdSetupBits[6] = { 0x40, 0x40, 0x40, 0x80, 0x00, 0xC0 };
static const UCHAR g_IdeCmdUdmaBits[6] = { 0x30, 0x20, 0x10, 0x20, 0x10, 0x00 };

static ULONG
IdeCmdTimingIndex(IN PIDE_DEVICE Dev)
{
    if (Dev->UseDma && Dev->ModeType == IDE_MODE_MWDMA) return 5 + Dev->Mode;
    return (Dev->PioMode > 4) ? 4 : Dev->PioMode;
}

static VOID
IdeCmdSetHost(IN PHW_DEVICE_EXTENSION HwInit, IN ULONG Channel, IN ULONG Unit)
{
    PIDE_CHANNEL ch = &HwInit->Channels[Channel];
    PIDE_DEVICE dev = &ch->Devices[Unit];
    ULONG port = HwInit->FirstChannel + Channel;
    ULONG index = IdeCmdTimingIndex(dev);
    ULONG setup = g_IdeCmdSetupClocks[index];
    ULONG artReg, drwReg, udmaReg, art, udma;

    if (port) {
        PIDE_DEVICE pair = &ch->Devices[Unit ^ 1];
        artReg = CMD_ARTTIM23;
        drwReg = Unit ? CMD_DRWTIM3 : CMD_DRWTIM2;
        udmaReg = CMD_UDIDETCR1;
        if (pair->Present && g_IdeCmdSetupClocks[IdeCmdTimingIndex(pair)] > setup) {
            setup = g_IdeCmdSetupClocks[IdeCmdTimingIndex(pair)];
        }
    } else {
        artReg = Unit ? CMD_ARTTIM1 : CMD_ARTTIM0;
        drwReg = Unit ? CMD_DRWTIM1 : CMD_DRWTIM0;
        udmaReg = CMD_UDIDETCR0;
    }

    /* ARTTIM23 bit 4 is the secondary interrupt latch: written as 0, so
       a pending interrupt is not lost */
    art = IdePciReadConfig(HwInit, artReg, 1);
    art = (art & ((artReg == CMD_ARTTIM23) ? 0x2F : 0x3F)) | g_IdeCmdSetupBits[setup];
    IdePciWriteConfig(HwInit, artReg, 1, art);
    IdePciWriteConfig(HwInit, drwReg, 1, g_IdeCmdDrwtim[index]);

    if (HwInit->Chip == IDE_CHIP_CMD643) return;            /* no UDMA engine */
    udma = IdePciReadConfig(HwInit, udmaReg, 1);
    udma &= ~((0x30UL << (2 * Unit)) | (0x05UL << Unit));
    if (dev->UseDma && dev->ModeType == IDE_MODE_UDMA && dev->Mode <= 5) {
        udma |= ((ULONG)g_IdeCmdUdmaBits[dev->Mode] << (2 * Unit)) | (1UL << Unit);
        if (dev->Mode > 2) udma |= 4UL << Unit;              /* 15 ns base clock */
    }
    IdePciWriteConfig(HwInit, udmaReg, 1, udma);
}

/* ATA timing in ns per mode (PIO 0-4, MWDMA 0-2, UDMA 0-6): address
   setup, 8-bit active/recovery/cycle, active, recovery, cycle, UDMA cycle */
static const USHORT g_IdeAtaTiming[15][8] = {
    { 70, 290, 240, 600, 165, 150, 600,   0 },
    { 50, 290,  93, 383, 125, 100, 383,   0 },
    { 30, 290,  40, 330, 100,  90, 240,   0 },
    { 30,  80,  70, 180,  80,  70, 180,   0 },
    { 25,  70,  25, 120,  70,  25, 120,   0 },
    { 60,   0,   0,   0, 215, 215, 480,   0 },
    { 45,   0,   0,   0,  80,  50, 150,   0 },
    { 25,   0,   0,   0,  70,  25, 120,   0 },
    {  0,   0,   0,   0,   0,   0,   0, 120 },
    {  0,   0,   0,   0,   0,   0,   0,  80 },
    {  0,   0,   0,   0,   0,   0,   0,  60 },
    {  0,   0,   0,   0,   0,   0,   0,  45 },
    {  0,   0,   0,   0,   0,   0,   0,  30 },
    {  0,   0,   0,   0,   0,   0,   0,  20 },
    {  0,   0,   0,   0,   0,   0,   0,  15 },
};
#define IDE_T_SETUP     0
#define IDE_T_ACT8B     1
#define IDE_T_REC8B     2
#define IDE_T_CYC8B     3
#define IDE_T_ACTIVE    4
#define IDE_T_RECOVER   5
#define IDE_T_CYCLE     6
#define IDE_T_UDMA      7

/* Clocks for one mode: bus clock T and UDMA clock UT in ps, rounded up;
   a DMA mode also covers the PIO mode (PIO commands run with the same
   timing), and active plus recovery are stretched to the cycle time */
static VOID
IdeTimingClocks(IN ULONG Index, IN ULONG PioIndex, IN ULONG T, IN ULONG UT, OUT ULONG Clk[8])
{
    ULONG i, v, p;

    for (i = 0; i < 8; i++) {
        v = g_IdeAtaTiming[Index][i];
        Clk[i] = v ? (v * 1000 - 1) / ((i == IDE_T_UDMA) ? UT : T) + 1 : 0;
        if (Index > 4) {
            p = g_IdeAtaTiming[PioIndex][i];
            p = p ? (p * 1000 - 1) / ((i == IDE_T_UDMA) ? UT : T) + 1 : 0;
            if (p > Clk[i]) Clk[i] = p;
        }
    }
    if (Clk[IDE_T_ACT8B] + Clk[IDE_T_REC8B] < Clk[IDE_T_CYC8B]) {
        Clk[IDE_T_ACT8B] += (Clk[IDE_T_CYC8B] - (Clk[IDE_T_ACT8B] + Clk[IDE_T_REC8B])) / 2;
        Clk[IDE_T_REC8B] = Clk[IDE_T_CYC8B] - Clk[IDE_T_ACT8B];
    }
    if (Clk[IDE_T_ACTIVE] + Clk[IDE_T_RECOVER] < Clk[IDE_T_CYCLE]) {
        Clk[IDE_T_ACTIVE] += (Clk[IDE_T_CYCLE] - (Clk[IDE_T_ACTIVE] + Clk[IDE_T_RECOVER])) / 2;
        Clk[IDE_T_RECOVER] = Clk[IDE_T_CYCLE] - Clk[IDE_T_ACTIVE];
    }
}

static ULONG
IdeClamp(IN ULONG V, IN ULONG Lo, IN ULONG Hi)
{
    return (V < Lo) ? Lo : (V > Hi) ? Hi : V;
}

/* Timing index of the mode in effect: PIO 0-4, MWDMA 5-7, UDMA 8-14 */
static ULONG
IdeModeIndex(IN PIDE_DEVICE Dev)
{
    if (Dev->UseDma && Dev->ModeType == IDE_MODE_MWDMA) return 5 + (Dev->Mode > 2 ? 2 : Dev->Mode);
    if (Dev->UseDma && Dev->ModeType == IDE_MODE_UDMA) return 8 + (Dev->Mode > 6 ? 6 : Dev->Mode);
    return (Dev->PioMode > 4) ? 4 : Dev->PioMode;
}

/* VIA, AMD and nVidia (one register layout, at 40h or 50h, 33 MHz bus):
   address setup (+0Ch, two bits per drive), 8-bit command timing per
   channel (+0Fh - channel), active/recovery per drive (+08h + 3 - drive),
   UDMA per drive (+10h + 3 - drive). The 8-bit timing is shared by both
   drives of the channel, so the slower one sets it. The values follow
   the ATA timing tables as Linux pata_via/pata_amd apply them. */
static VOID
IdeViaSetHost(IN PHW_DEVICE_EXTENSION HwInit, IN ULONG Channel, IN ULONG Unit)
{
    static const UCHAR amdCyc2Udma[16] = { 6, 6, 5, 4, 0, 1, 1, 2, 2, 3, 3, 3, 3, 3, 3, 7 };
    PIDE_CHANNEL ch = &HwInit->Channels[Channel];
    PIDE_DEVICE dev = &ch->Devices[Unit];
    PIDE_DEVICE pair = &ch->Devices[Unit ^ 1];
    ULONG port = HwInit->FirstChannel + Channel;
    ULONG dn = port * 2 + Unit;
    ULONG base = HwInit->ViaBase;
    ULONG T = 30000, UT = 30000;                /* ps */
    ULONG clk[8], peer[8], index = IdeModeIndex(dev);
    ULONG v, shift, code;
    BOOLEAN amd = (BOOLEAN)((HwInit->ViaFlags & IDE_VIA_AMD) != 0);
    BOOLEAN udma = (BOOLEAN)(dev->UseDma && dev->ModeType == IDE_MODE_UDMA);

    if (base == 0 || port > 1) return;
    if (amd) {
        if (HwInit->ViaUdmaType >= 2) UT = T / 2;
    } else {
        switch (HwInit->ViaUdmaType) {
        case 4: UT = T / 2; break;
        case 5: UT = T / 3; break;
        case 6: UT = T / 4; break;
        default: break;
        }
    }
    IdeTimingClocks(index, (dev->PioMode > 4) ? 4 : dev->PioMode, T, UT, clk);
    if (pair->Present) {
        ULONG pp = (pair->PioMode > 4) ? 4 : pair->PioMode;
        IdeTimingClocks(pp, pp, T, UT, peer);
        if (peer[IDE_T_ACT8B] > clk[IDE_T_ACT8B]) clk[IDE_T_ACT8B] = peer[IDE_T_ACT8B];
        if (peer[IDE_T_REC8B] > clk[IDE_T_REC8B]) clk[IDE_T_REC8B] = peer[IDE_T_REC8B];
    }

    if (!(HwInit->ViaFlags & IDE_VIA_BAD_AST)) {
        shift = (3 - dn) * 2;
        v = IdePciReadConfig(HwInit, base + 0x0C, 1);
        v = (v & ~(3UL << shift)) | ((IdeClamp(clk[IDE_T_SETUP], 1, 4) - 1) << shift);
        IdePciWriteConfig(HwInit, base + 0x0C, 1, v);
    }
    IdePciWriteConfig(HwInit, base + 0x0F - port, 1,
                      ((IdeClamp(clk[IDE_T_ACT8B], 1, 16) - 1) << 4) | (IdeClamp(clk[IDE_T_REC8B], 1, 16) - 1));
    IdePciWriteConfig(HwInit, base + 0x08 + 3 - dn, 1,
                      ((IdeClamp(clk[IDE_T_ACTIVE], 1, 16) - 1) << 4) | (IdeClamp(clk[IDE_T_RECOVER], 1, 16) - 1));

    if (amd) {
        /* UDMA 5/6 at 33 MHz: fixed codes */
        if (udma && dev->Mode == 5) clk[IDE_T_UDMA] = 1;
        if (udma && dev->Mode >= 6) clk[IDE_T_UDMA] = 15;
        code = 0x03;                            /* UDMA off */
        if (udma) {
            switch (HwInit->ViaUdmaType) {
            case 1:  code = 0xC0 | (IdeClamp(clk[IDE_T_UDMA], 2, 5) - 2); break;
            case 2:  code = 0xC0 | amdCyc2Udma[IdeClamp(clk[IDE_T_UDMA], 2, 10)]; break;
            case 3:  code = 0xC0 | amdCyc2Udma[IdeClamp(clk[IDE_T_UDMA], 1, 10)]; break;
            default: code = 0xC0 | amdCyc2Udma[IdeClamp(clk[IDE_T_UDMA], 1, 15)]; break;
            }
        }
        IdePciWriteConfig(HwInit, base + 0x10 + 3 - dn, 1, code);
    } else if (HwInit->ViaUdmaType != 0) {
        v = IdePciReadConfig(HwInit, base + 0x10 + 3 - dn, 1) & ~0x20UL;
        if (udma) {
            switch (HwInit->ViaUdmaType) {
            case 2:  code = 0xE0 | (IdeClamp(clk[IDE_T_UDMA], 2, 5) - 2); break;
            case 4:  code = 0xE8 | (IdeClamp(clk[IDE_T_UDMA], 2, 9) - 2); break;
            default: code = 0xE0 | (IdeClamp(clk[IDE_T_UDMA], 2, 9) - 2); break;
            }
            v = (v & 0x10) | code;              /* bit 4: cable detection, kept */
        }
        IdePciWriteConfig(HwInit, base + 0x10 + 3 - dn, 1, v);
    }
}

/* ALi M5229 (33 MHz bus): FIFO per drive (54h + channel, a nibble per
   drive), address setup (58h + 4 * channel), 8-bit command timing (59h +
   4 * channel, shared by both drives: the slower sets it), active and
   recovery per drive (5Ah + 4 * channel + drive), UDMA per drive (56h +
   channel, a nibble); the values as Linux pata_ali applies them */
static VOID
IdeAliSetHost(IN PHW_DEVICE_EXTENSION HwInit, IN ULONG Channel, IN ULONG Unit)
{
    static const UCHAR udmaCode[7] = { 0x0C, 0x0B, 0x0A, 0x09, 0x08, 0x0F, 0x0D };
    PIDE_CHANNEL ch = &HwInit->Channels[Channel];
    PIDE_DEVICE dev = &ch->Devices[Unit];
    PIDE_DEVICE pair = &ch->Devices[Unit ^ 1];
    ULONG port = HwInit->FirstChannel + Channel;
    ULONG T = 30000;                            /* ps */
    ULONG clk[8], peer[8], index, shift = 4 * Unit, v, fifo;
    BOOLEAN udma = (BOOLEAN)(dev->UseDma && dev->ModeType == IDE_MODE_UDMA);

    if (port > 1) return;
    /* UDMA: the PIO timing for the PIO commands, the UDMA code on top */
    index = udma ? ((dev->PioMode > 4) ? 4 : dev->PioMode) : IdeModeIndex(dev);
    IdeTimingClocks(index, (dev->PioMode > 4) ? 4 : dev->PioMode, T, T, clk);
    if (pair->Present) {
        ULONG pp = (pair->PioMode > 4) ? 4 : pair->PioMode;
        IdeTimingClocks(pp, pp, T, T, peer);
        if (peer[IDE_T_SETUP] > clk[IDE_T_SETUP]) clk[IDE_T_SETUP] = peer[IDE_T_SETUP];
        if (peer[IDE_T_ACT8B] > clk[IDE_T_ACT8B]) clk[IDE_T_ACT8B] = peer[IDE_T_ACT8B];
        if (peer[IDE_T_REC8B] > clk[IDE_T_REC8B]) clk[IDE_T_REC8B] = peer[IDE_T_REC8B];
    }

    /* FIFO: on for disks (PIO 05h, DMA 08h), off for ATAPI */
    fifo = dev->IsAtapi ? 0x00 : (dev->UseDma ? 0x08 : 0x05);
    v = IdePciReadConfig(HwInit, 0x54 + port, 1);
    IdePciWriteConfig(HwInit, 0x54 + port, 1, (v & ~(0x0FUL << shift)) | (fifo << shift));

    IdePciWriteConfig(HwInit, 0x58 + 4 * port, 1, IdeClamp(clk[IDE_T_SETUP], 1, 8) & 7);
    IdePciWriteConfig(HwInit, 0x59 + 4 * port, 1,
                      ((IdeClamp(clk[IDE_T_ACT8B], 1, 8) & 7) << 4) | (IdeClamp(clk[IDE_T_REC8B], 1, 16) & 15));
    IdePciWriteConfig(HwInit, 0x5A + 4 * port + Unit, 1,
                      ((IdeClamp(clk[IDE_T_ACTIVE], 1, 8) & 7) << 4) | (IdeClamp(clk[IDE_T_RECOVER], 1, 16) & 15));

    v = IdePciReadConfig(HwInit, 0x56 + port, 1) & ~(0x0FUL << shift);
    if (udma) {
        v |= (ULONG)udmaCode[dev->Mode > 6 ? 6 : dev->Mode] << shift;
        if (dev->Mode >= 3) IdePciWriteConfig(HwInit, 0x4B, 1, IdePciReadConfig(HwInit, 0x4B, 1) | 0x01);
    }
    IdePciWriteConfig(HwInit, 0x56 + port, 1, v);
}

/* Silicon Image: transfer mode field of the port (0 PIO 0-2, 1 PIO 3/4,
   2 MWDMA, 3 UDMA), one per device */
static VOID
IdeSiiSetHost(IN PHW_DEVICE_EXTENSION HwInit, IN ULONG Channel)
{
    static const USHORT xferReg[SII_MAX_PORTS] = { 0xB4, 0xF4, 0x2B4, 0x2F4 };
    PIDE_CHANNEL ch = &HwInit->Channels[Channel];
    ULONG port = HwInit->FirstChannel + Channel;
    ULONG value, unit, mode;

    if (HwInit->SiiMmio == NULL || port >= SII_MAX_PORTS) return;
    value = ScsiPortReadRegisterUlong((PULONG)(HwInit->SiiMmio + xferReg[port]));
    value &= ~0x33UL;
    for (unit = 0; unit < IDE_DEVICES_PER_CHANNEL; unit++) {
        PIDE_DEVICE dev = &ch->Devices[unit];
        if (!dev->Present) mode = 0;
        else if (dev->UseDma) mode = (dev->ModeType == IDE_MODE_UDMA) ? 3 : 2;
        else mode = (dev->PioMode >= 3) ? 1 : 0;
        value |= mode << (4 * unit);
    }
    ScsiPortWriteRegisterUlong((PULONG)(HwInit->SiiMmio + xferReg[port]), value);
    (VOID)ScsiPortReadRegisterUlong((PULONG)(HwInit->SiiMmio + xferReg[port]));
}

/* Chipset timing for the mode now in effect on the device, and the
   bus master's "drive DMA capable" bit (what a BIOS sets for the drives
   it put in a DMA mode) */
static VOID
IdeProgramHost(IN PHW_DEVICE_EXTENSION HwInit, IN ULONG Channel, IN ULONG Unit)
{
    PIDE_CHANNEL ch = &HwInit->Channels[Channel];
    PIDE_DEVICE dev = &ch->Devices[Unit];
    ULONG port = HwInit->FirstChannel + Channel;

    if (HwInit->PciWritable) {
        if (HwInit->Intel != IDE_INTEL_NONE) {
            IdeIntelSetPio(HwInit, port, Unit, dev->PioMode, dev->IsAtapi);
            if (dev->UseDma) IdeIntelSetDma(HwInit, port, Unit, dev->ModeType, dev->Mode, dev->PioMode);
        } else if (IDE_CHIP_IS_CMD(HwInit->Chip)) {
            IdeCmdSetHost(HwInit, Channel, Unit);
        } else if (IDE_CHIP_IS_VIA_STYLE(HwInit->Chip)) {
            IdeViaSetHost(HwInit, Channel, Unit);
        } else if (HwInit->Chip == IDE_CHIP_ALI && HwInit->AliTiming) {
            IdeAliSetHost(HwInit, Channel, Unit);
        }
    }
    if (IDE_CHIP_IS_SII(HwInit->Chip)) IdeSiiSetHost(HwInit, Channel);

    if (ch->Bm != NULL) {
        UCHAR bit = (UCHAR)(Unit ? BM_STS_DRIVE1_DMA : BM_STS_DRIVE0_DMA);
        UCHAR sts = IDE_INB(ch->Bm + BM_REG_STATUS);
        /* interrupt and error bits written as 0: left alone */
        sts &= (UCHAR)~(bit | BM_STS_INTERRUPT | BM_STS_ERROR);
        if (dev->UseDma) sts |= bit;
        IDE_OUTB(ch->Bm + BM_REG_STATUS, sts);
    }
}

/* Hosts whose timing the driver programs itself: TRUE, and the highest
   UDMA mode (IDE_NO_UDMA: MWDMA at most). FALSE: the timing stays as the
   BIOS set it. */
static BOOLEAN
IdeHostProgrammable(IN PHW_DEVICE_EXTENSION HwInit, OUT PULONG UdmaMax)
{
    *UdmaMax = IDE_NO_UDMA;
    if (!HwInit->PciWritable) return FALSE;
    switch (HwInit->Intel) {
    case IDE_INTEL_PIIX3:  return TRUE;
    case IDE_INTEL_PIIX4:  *UdmaMax = 2; return TRUE;
    case IDE_INTEL_ICH66:  *UdmaMax = 4; return TRUE;
    case IDE_INTEL_ICH100: *UdmaMax = 5; return TRUE;
    default:               break;
    }
    switch (HwInit->Chip) {
    case IDE_CHIP_CMD643:
        return TRUE;
    case IDE_CHIP_CMD646:
        /* UDMA from the 646U2 (revision 5); the 646U (3) needs board
           fixes for it (Linux cmd64x) */
        if (HwInit->Revision >= 5) *UdmaMax = 2;
        return TRUE;
    case IDE_CHIP_CMD648:
        *UdmaMax = 4;
        return TRUE;
    case IDE_CHIP_CMD649:
        *UdmaMax = 5;
        return TRUE;
    case IDE_CHIP_SII3112:
    case IDE_CHIP_SII3512:
    case IDE_CHIP_SII3114:
        if (HwInit->SiiMmio == NULL) return FALSE;
        *UdmaMax = 6;                                       /* SATA link: no PATA timing */
        return TRUE;
    case IDE_CHIP_VIA:
    case IDE_CHIP_AMD:
        if (HwInit->ViaBase == 0) return FALSE;            /* generation not known */
        if (HwInit->ViaUdmaMax != 0xFF) *UdmaMax = HwInit->ViaUdmaMax;
        return TRUE;
    case IDE_CHIP_ALI:
        if (!HwInit->AliTiming) return FALSE;
        if (HwInit->AliUdmaMax != 0xFF) *UdmaMax = HwInit->AliUdmaMax;
        return TRUE;
    default:
        return FALSE;
    }
}

/* 80-conductor cable as the host detects it */
static BOOLEAN
IdeHostCable80(IN PHW_DEVICE_EXTENSION HwInit, IN ULONG Port)
{
    if (HwInit->Chip == IDE_CHIP_CMD648 || HwInit->Chip == IDE_CHIP_CMD649) {
        return (BOOLEAN)((IdePciReadConfig(HwInit, CMD_BMIDECSR, 1) & (1UL << Port)) != 0);
    }
    if (HwInit->Chip == IDE_CHIP_VIA && HwInit->ViaBase != 0 && Port <= 1) {
        if ((HwInit->ViaFlags & IDE_VIA_SATA_PATA) && Port == 0) return TRUE;
        /* UDMA 100 and later: the drive-side cable bits the BIOS latched */
        if (HwInit->ViaUdmaType >= 5) {
            return (BOOLEAN)((IdePciReadConfig(HwInit, 0x50, 4) & (0x10100000UL >> (16 * Port))) != 0);
        }
        return FALSE;
    }
    if (HwInit->Chip == IDE_CHIP_ALI && HwInit->AliTiming && Port <= 1) {
        /* C2h and later: 4Ah bit set per channel = 40 wires */
        if (HwInit->Revision < 0xC2 || HwInit->AliCable40) return FALSE;
        return (BOOLEAN)((IdePciReadConfig(HwInit, 0x4A, 1) & (1UL << Port)) == 0);
    }
    if (HwInit->Chip == IDE_CHIP_AMD && (HwInit->ViaFlags & IDE_VIA_AMD_CABLE) && Port <= 1) {
        return (BOOLEAN)((IdePciReadConfig(HwInit, 0x42, 1) & (Port ? 0x0C : 0x03)) != 0);
    }
    if (HwInit->Chip == IDE_CHIP_AMD && (HwInit->ViaFlags & IDE_VIA_NVIDIA) && Port <= 1) {
        /* nVidia: the cable detection is unreliable; a UDMA mode above 2
           that the BIOS set means an 80-conductor cable */
        ULONG b = HwInit->NvBiosUdma >> (Port ? 0 : 16), u;
        for (u = 0; u < 2; u++) {
            ULONG v = (b >> (u ? 0 : 8)) & 0xFF;
            if ((v & 0xC0) == 0xC0 && ((v & 7) >= 4)) return TRUE;
        }
        return FALSE;
    }
    return IdeIntelCable80(HwInit, Port);
}

/* Model string (IDENTIFY words 27-46) starting with Prefix */
static BOOLEAN
IdeModelStartsWith(IN PUSHORT Id, IN PCHAR Prefix)
{
    ULONG i;
    CHAR c;

    for (i = 0; i < 40 && Prefix[i] != 0; i++) {
        c = (CHAR)((i & 1) ? (Id[27 + i / 2] & 0xFF) : (Id[27 + i / 2] >> 8));
        if (c != Prefix[i]) return FALSE;
    }
    return TRUE;
}

/* Model string (IDENTIFY words 27-46) equal to Model, trailing blanks aside */
static BOOLEAN
IdeModelIs(IN PUSHORT Id, IN PCHAR Model)
{
    ULONG i;
    CHAR c;

    for (i = 0; i < 40; i++) {
        c = (CHAR)((i & 1) ? (Id[27 + i / 2] & 0xFF) : (Id[27 + i / 2] >> 8));
        if (Model[i] == 0) {
            if (c != ' ' && c != 0) return FALSE;
            continue;
        }
        if (c != Model[i]) return FALSE;
        if (Model[i + 1] == 0) {
            /* rest of the field must be blank */
            for (i++; i < 40; i++) {
                c = (CHAR)((i & 1) ? (Id[27 + i / 2] & 0xFF) : (Id[27 + i / 2] >> 8));
                if (c != ' ' && c != 0) return FALSE;
            }
            return TRUE;
        }
    }
    return TRUE;
}

/* ServerWorks CSB5/CSB6: no UDMA 5 and up with these drives (Linux
   pata_serverworks) */
static const PCHAR g_IdeCsbBadUdma100[] = {
    "ST320011A", "ST340016A", "ST360021A", "ST380021A", NULL
};

/* Silicon Image 3112 data corruption with these drives unless every
   command moves at most 15 sectors (Linux sata_sil, "mod15write") */
static const PCHAR g_IdeSiiMod15Write[] = {
    "ST320012AS", "ST330013AS", "ST340017AS", "ST360015AS", "ST380023AS",
    "ST3120023AS", "ST340014ASL", "ST360014ASL", "ST380011ASL", "ST3120022ASL",
    "ST3160021ASL", "TOSHIBA MK2561GSYN", NULL
};

/* Pick the transfer mode: the fastest the device, the cable and the
   controller allow where the driver programs the controller's timing;
   where it does not, the mode the BIOS set up (its "drive DMA capable"
   flag), and PIO for a parallel drive the BIOS left without DMA */
VOID
IdeSelectTransferMode(IN PHW_DEVICE_EXTENSION HwInit, IN ULONG Channel, IN ULONG Unit)
{
    PIDE_CHANNEL ch = &HwInit->Channels[Channel];
    PIDE_DEVICE dev = &ch->Devices[Unit];
    PUSHORT id = (PUSHORT)ch->BounceBuffer;
    ULONG port = HwInit->FirstChannel + Channel;
    ULONG pio, udma = IDE_NO_UDMA, mwdma = IDE_NO_UDMA, cur, udmaMax, i;
    ULONG selected;
    BOOLEAN cable80, sata, bmCapable, programmable;
    PCHAR why = NULL;
    UCHAR bmsts = 0;

    dev->ModeType = IDE_MODE_PIO;
    dev->Mode = 0;
    dev->UseDma = FALSE;
    dev->DmaCapable = FALSE;
    dev->NoLba48Dma = HwInit->NoLba48Dma;
    dev->MaxSectors = 0;

    if (!dev->IdentifyValid) return;

    /* PIO: word 64 (modes 3/4) when valid, else the old word 51 */
    pio = 0;
    if ((id[IDW_FIELD_VALIDITY] & IDW53_WORDS_64_70_VALID) && (id[IDW_PIO_MODES] & 0x03)) {
        pio = (id[IDW_PIO_MODES] & 0x02) ? 4 : 3;
    } else {
        pio = (id[IDW_PIO_OLD] >> 8) & 0xFF;
        if (pio > 2) pio = 2;
    }
    dev->PioMode = (UCHAR)pio;

    sata = (BOOLEAN)(id[IDW_SATA_CAPABILITIES] != 0 && id[IDW_SATA_CAPABILITIES] != 0xFFFF);
    if (id[IDW_FIELD_VALIDITY] & IDW53_WORD_88_VALID) {
        udma = IdeHighestBit(id[IDW_UDMA] & 0x7F, 6);
    }
    mwdma = IdeHighestBit(id[IDW_MWDMA] & 0x07, 2);

    /* Silicon Image drive errata */
    if (IDE_CHIP_IS_SII(HwInit->Chip) && !dev->IsAtapi) {
        if (HwInit->Chip == IDE_CHIP_SII3112) {
            for (i = 0; g_IdeSiiMod15Write[i] != NULL; i++) {
                if (IdeModelIs(id, g_IdeSiiMod15Write[i])) dev->MaxSectors = 15;
            }
        }
        if (IdeModelIs(id, "Maxtor 4D060H3") && udma != IDE_NO_UDMA && udma > 5) udma = 5;
        if (dev->MaxSectors != 0) {
            IDE_DBG((IDE_PFX "Channel %lu.%lu: 15 sectors per command (Silicon Image 3112 errata)\n", Channel, Unit));
        }
    }
    /* ALi 20h-C1h: no UDMA for WD drives; up to C4h at most 255 sectors
       per command (Linux pata_ali) */
    if (HwInit->Chip == IDE_CHIP_ALI && HwInit->Revision >= 0x20) {
        if (HwInit->Revision < 0xC2 && IdeModelStartsWith(id, "WDC")) udma = IDE_NO_UDMA;
        if (HwInit->Revision < 0xC5) dev->MaxSectors = 255;
    }
    /* ServerWorks errata (Linux pata_serverworks): the OSB4 hangs with
       UDMA to disks, the CSB5/6 corrupts UDMA 5+ with some drives */
    if (!dev->IsAtapi && udma != IDE_NO_UDMA) {
        if (HwInit->Chip == IDE_CHIP_OSB4) {
            udma = IDE_NO_UDMA;
        } else if (HwInit->Chip == IDE_CHIP_CSB && udma > 4) {
            for (i = 0; g_IdeCsbBadUdma100[i] != NULL; i++) {
                if (IdeModelIs(id, g_IdeCsbBadUdma100[i])) udma = 4;
            }
        }
    }

    if (ch->Bm == NULL) why = "no bus master";
    else if (!HwInit->DmaAllowed) why = "DMA off";
    else if (ch->UnitsPio & (1U << Unit)) why = "PIO only (PIO=)";
    else if (dev->DmaErrors >= IDE_DMA_ERROR_LIMIT) why = "after DMA errors";
    else if (!(id[IDW_CAPABILITIES] & IDW49_DMA_SUPPORTED)) why = "device without DMA";
    else if (dev->IsAtapi && HwInit->NoAtapiDma) why = "no ATAPI DMA on this controller";
    else {
        bmsts = IDE_INB(ch->Bm + BM_REG_STATUS);
        if ((bmsts & BM_STS_SIMPLEX) && HwInit->ClearSimplex) {
            /* AMD 756/766: not really simplex (interrupt and error bits
               written as 0: left alone) */
            IDE_OUTB(ch->Bm + BM_REG_STATUS, (UCHAR)(bmsts & 0x60));
            bmsts = IDE_INB(ch->Bm + BM_REG_STATUS);
        }
        /* Simplex bus master: one channel at a time may use DMA. With both
           channels in this adapter they take turns (IdeSimplexWait);
           otherwise the primary has it. */
        if (bmsts & BM_STS_SIMPLEX) {
            if (HwInit->ChannelCount == 2 && HwInit->FirstChannel == 0) {
                HwInit->Simplex = TRUE;
            } else if (port != 0) {
                why = "simplex bus master, DMA on the primary channel";
            }
        }
    }
    if (why != NULL) {
        if (pio >= 3) IdeSetXferMode(HwInit, Channel, Unit, IDE_MODE_PIO, pio);
        IdeProgramHost(HwInit, Channel, Unit);
        IDE_DBG((IDE_PFX "Channel %lu.%lu: PIO %lu (%s)\n", Channel, Unit, pio, why));
        return;
    }

    cable80 = (BOOLEAN)(sata || (id[IDW_HW_RESET_RESULT] & IDW93_CABLE_80) || IdeHostCable80(HwInit, port));

    /* Mode in effect now: set by the BIOS, together with the host timing */
    cur = IDE_NO_UDMA;
    selected = IDE_MODE_PIO;
    if ((id[IDW_FIELD_VALIDITY] & IDW53_WORD_88_VALID) && (id[IDW_UDMA] & 0x7F00)) {
        cur = IdeHighestBit((id[IDW_UDMA] >> 8) & 0x7F, 6);
        selected = IDE_MODE_UDMA;
    } else if (id[IDW_MWDMA] & 0x0700) {
        cur = IdeHighestBit((id[IDW_MWDMA] >> 8) & 0x07, 2);
        selected = IDE_MODE_MWDMA;
    }
    bmCapable = (BOOLEAN)((bmsts & (Unit ? BM_STS_DRIVE1_DMA : BM_STS_DRIVE0_DMA)) != 0);

    programmable = IdeHostProgrammable(HwInit, &udmaMax);
    if (programmable) {
        /* Known controller: the fastest mode within its limits */
        if (!cable80 && udmaMax != IDE_NO_UDMA && udmaMax > 2) udmaMax = 2;
        if (udma != IDE_NO_UDMA && udmaMax != IDE_NO_UDMA) {
            if (udma > udmaMax) udma = udmaMax;
            dev->ModeType = IDE_MODE_UDMA;
            dev->Mode = (UCHAR)udma;
        } else if (mwdma != IDE_NO_UDMA) {
            dev->ModeType = IDE_MODE_MWDMA;
            dev->Mode = (UCHAR)mwdma;
        }
    } else if ((bmCapable || sata) && cur != IDE_NO_UDMA && !HwInit->DmaForced) {
        /* Controller timing left as the BIOS set it: the mode the BIOS
           selected (a SATA link has no PATA timing to match) */
        if (selected == IDE_MODE_UDMA && (udma == IDE_NO_UDMA || cur > udma)) {
            /* That mode is not allowed for this drive here. A SATA link
               can take a lower one; a parallel host would keep timing for
               the BIOS mode, so PIO. */
            if (sata && udma != IDE_NO_UDMA) cur = udma;
            else why = "the mode the BIOS set is not usable with this controller";
        }
        if (why == NULL) {
            dev->ModeType = (UCHAR)selected;
            dev->Mode = (UCHAR)cur;
        }
    } else if (bmCapable || HwInit->DmaForced) {
        /* The BIOS flagged the drive for DMA without a mode on record, or
           DMA=1: the fastest mode the device and cable allow */
        if (udma != IDE_NO_UDMA) {
            if (!cable80 && udma > 2) udma = 2;
            dev->ModeType = IDE_MODE_UDMA;
            dev->Mode = (UCHAR)udma;
        } else if (mwdma != IDE_NO_UDMA) {
            dev->ModeType = IDE_MODE_MWDMA;
            dev->Mode = (UCHAR)mwdma;
        }
    } else {
        /* Unknown parallel ATA controller and the BIOS did not set the
           drive up for DMA: its timing for DMA is unknown */
        why = "controller timing unknown";
    }

    dev->DmaCapable = (BOOLEAN)(udma != IDE_NO_UDMA || mwdma != IDE_NO_UDMA);

    if (dev->ModeType != IDE_MODE_PIO) {
        if (IdeSetXferMode(HwInit, Channel, Unit, dev->ModeType, dev->Mode)) {
            dev->UseDma = TRUE;
        } else {
            IDE_DBG((IDE_PFX "Channel %lu.%lu: SET FEATURES transfer mode rejected\n", Channel, Unit));
            dev->ModeType = IDE_MODE_PIO;
            dev->Mode = 0;
        }
    }
    if (!dev->UseDma && pio >= 3) IdeSetXferMode(HwInit, Channel, Unit, IDE_MODE_PIO, pio);
    IdeProgramHost(HwInit, Channel, Unit);

    IDE_DBG((IDE_PFX "Channel %lu.%lu: %s %lu (PIO %lu, UDMA %lX MWDMA %lX, cable %s, BIOS %s %lu%s%s%s)\n",
             Channel, Unit,
             dev->ModeType == IDE_MODE_UDMA ? "UDMA" : dev->ModeType == IDE_MODE_MWDMA ? "MWDMA" : "PIO",
             (ULONG)(dev->UseDma ? dev->Mode : pio), pio,
             (ULONG)(id[IDW_UDMA] & 0x7F), (ULONG)(id[IDW_MWDMA] & 0x07),
             cable80 ? "80" : "40",
             selected == IDE_MODE_UDMA ? "UDMA" : selected == IDE_MODE_MWDMA ? "MWDMA" : "PIO",
             (cur == IDE_NO_UDMA) ? pio : cur,
             bmCapable ? ", BIOS DMA flag" : "", why != NULL ? ", PIO: " : "", why != NULL ? why : ""));
}

/* A PCI function whose I/O decoding was turned off (PnP after a failed
   start of its device node, a BIOS resume) reads FFh in every register;
   its command register is set again. TRUE when the channel answers. */
static BOOLEAN
IdeDecodingOn(IN PHW_DEVICE_EXTENSION HwInit, IN ULONG Channel, IN PUCHAR Port)
{
    ULONG command;

    if (IDE_INB(Port) != 0xFF) return TRUE;
    if (!HwInit->HasPci) return FALSE;
    command = IdePciReadConfig(HwInit, 0x04, 2);
    if (command == 0xFFFF) return FALSE;
    IdePciWriteConfig(HwInit, 0x04, 2, (command | 0x0005) & ~(1UL << 10));
    if (IDE_INB(Port) == 0xFF) return FALSE;
    IDE_DBG((IDE_PFX "Channel %lu: PCI decoding was off, enabled again\n", Channel));
    return TRUE;
}

/* The task file answers? (see IdeDecodingOn) */
BOOLEAN
IdeChannelAlive(IN PHW_DEVICE_EXTENSION HwInit, IN ULONG Channel)
{
    PIDE_CHANNEL ch = &HwInit->Channels[Channel];
    return IdeDecodingOn(HwInit, Channel, ch->Ctl);
}

/* The bus master registers answer? Without them both devices of the
   channel go to PIO. */
BOOLEAN
IdeBusMasterAlive(IN PHW_DEVICE_EXTENSION HwInit, IN ULONG Channel)
{
    PIDE_CHANNEL ch = &HwInit->Channels[Channel];
    ULONG unit;

    if (ch->Bm == NULL) return FALSE;
    if (IdeDecodingOn(HwInit, Channel, ch->Bm + BM_REG_STATUS)) return TRUE;
    IDE_DBG((IDE_PFX "Channel %lu: bus master registers do not answer\n", Channel));
    for (unit = 0; unit < IDE_DEVICES_PER_CHANNEL; unit++) {
        if (ch->Devices[unit].Present) {
            ch->Devices[unit].DmaErrors = IDE_DMA_ERROR_LIMIT;
            IdeDisableDma(HwInit, Channel, unit);
        }
    }
    return FALSE;
}

/* DMA failed repeatedly: PIO from now on for this device */
VOID
IdeDisableDma(IN PHW_DEVICE_EXTENSION HwInit, IN ULONG Channel, IN ULONG Unit)
{
    PIDE_DEVICE dev = &HwInit->Channels[Channel].Devices[Unit];

    if (!dev->UseDma) return;
    IDE_DBG((IDE_PFX "Channel %lu.%lu: DMA off after %lu errors\n", Channel, Unit, (ULONG)dev->DmaErrors));
    dev->UseDma = FALSE;
    dev->ModeType = IDE_MODE_PIO;
    dev->Mode = 0;
    IdeSetXferMode(HwInit, Channel, Unit, IDE_MODE_PIO, dev->PioMode);
    IdeProgramHost(HwInit, Channel, Unit);
}

/* ------------------------------------------------------------------ */
/* Interrupt-completed DMA                                             */
/* ------------------------------------------------------------------ */

/* Simplex bus master: DMA on the other channel finished first */
static VOID
IdeSimplexWait(IN PHW_DEVICE_EXTENSION HwInit, IN ULONG Channel)
{
    PIDE_CHANNEL other;

    if (!HwInit->Simplex || HwInit->ChannelCount != 2) return;
    other = &HwInit->Channels[Channel ^ 1];
    if (other->Outstanding != NULL && !other->ActivePio) IdeDrainChannel(HwInit, Channel ^ 1);
}

/* Start a DMA read/write and return; HwInterrupt (or the watchdog)
   completes it. Returns SRB_STATUS_PENDING, otherwise the status to
   complete the SRB with. */
UCHAR
IdeIssueDma(IN PHW_DEVICE_EXTENSION HwInit, IN ULONG Channel, IN PSCSI_REQUEST_BLOCK Srb, IN PATA_REQUEST Req)
{
    PIDE_CHANNEL ch = &HwInit->Channels[Channel];
    ULONG timeoutUsec;

    if (ch->Outstanding != NULL) return SRB_STATUS_BUSY;
    IdeSimplexWait(HwInit, Channel);
    IdeChannelAlive(HwInit, Channel);
    if (!IdeBusMasterAlive(HwInit, Channel)) return SRB_STATUS_BUSY;  /* retried, with PIO */

    if (!IdePrepareDevice(ch, Req->Unit)) {
        IDE_DBG((IDE_PFX "Channel %lu.%lu: device busy before DMA (status %02lX)\n",
                 Channel, (ULONG)Req->Unit, (ULONG)IdeAltStatus(ch)));
        IdeResetChannel(HwInit, Channel);
        return SRB_STATUS_BUS_RESET;
    }
    if (!IdePrepareDma(HwInit, ch, Srb, Req)) return SRB_STATUS_INVALID_REQUEST;

    IdeSetControl(ch, 0);                       /* interrupts on */
    IdeWriteTaskFile(ch, Req, TRUE);

    if (Req->IsAtapi) {
        timeoutUsec = IdeTimeoutUsec(Srb, Req);
        if (!IdeSendPacket(ch, Req, timeoutUsec)) {
            UCHAR status = IDE_INB(ch->Cmd + IDE_REG_STATUS);
            IdeSetControl(ch, IDE_CTL_NIEN);
            IdeReadResult(ch, FALSE, status);
            ch->Devices[Req->Unit].LastStatus = status;
            ch->Devices[Req->Unit].LastError = ch->Result.Error;
            if (!(status & IDE_STS_ERR)) IdeResetChannel(HwInit, Channel);
            return SRB_STATUS_ERROR;
        }
    }

    ch->Outstanding = Srb;
    ch->ActiveUnit = Req->Unit;
    ch->ActiveWrite = Req->IsWrite;
    ch->ActiveAtapi = Req->IsAtapi;
    ch->ActivePio = FALSE;
    IDE_OUTB(ch->Bm + BM_REG_COMMAND, (Req->IsWrite ? 0 : BM_CMD_READ) | BM_CMD_START);
    return SRB_STATUS_PENDING;
}

/* Start a PIO or non-data command and return; the interrupt moves each
   DRQ block and completes it (an ATA write's first block goes out here,
   it raises no interrupt). Returns SRB_STATUS_PENDING, otherwise the
   status to complete the SRB with (SRB_STATUS_INVALID_REQUEST: run it
   polled instead). */
UCHAR
IdeIssuePio(IN PHW_DEVICE_EXTENSION HwInit, IN ULONG Channel, IN PSCSI_REQUEST_BLOCK Srb, IN PATA_REQUEST Req)
{
    PIDE_CHANNEL ch = &HwInit->Channels[Channel];
    ULONG timeoutUsec = IdeTimeoutUsec(Srb, Req);
    UCHAR status;

    if (Req->UseBounce || Req->UseDma) return SRB_STATUS_INVALID_REQUEST;
    if (ch->Outstanding != NULL) return SRB_STATUS_BUSY;
    IdeChannelAlive(HwInit, Channel);

    if (!IdePrepareDevice(ch, Req->Unit)) {
        IDE_DBG((IDE_PFX "Channel %lu.%lu: device busy before command %02lX (status %02lX)\n",
                 Channel, (ULONG)Req->Unit, (ULONG)Req->Command, (ULONG)IdeAltStatus(ch)));
        IdeResetChannel(HwInit, Channel);
        return SRB_STATUS_BUS_RESET;
    }

    ch->PioBuffer = (PUCHAR)Req->DataBuffer;
    ch->PioLeft = (Req->DataBuffer != NULL) ? Req->DataBufferLen : 0;
    ch->PioTail = FALSE;
    ch->ActiveUnit = Req->Unit;
    ch->ActiveWrite = Req->IsWrite;
    ch->ActiveAtapi = Req->IsAtapi;
    ch->ActiveIs48 = Req->Is48Bit;

    IdeSetControl(ch, 0);                       /* interrupts on */
    IdeWriteTaskFile(ch, Req, FALSE);
    /* BSY is valid 400 ns after the command; until then the status may
       still be the previous one, which would look like the end */
    ScsiPortStallExecution(1);

    if (Req->IsAtapi || (Req->IsWrite && ch->PioLeft != 0)) {
        if (Req->IsAtapi) {
            if (IdeSendPacket(ch, Req, timeoutUsec)) goto issued;
        } else if (IdeWaitStatus(ch, IDE_STS_BSY, 0, timeoutUsec)) {
            status = IdeAltStatus(ch);
            if ((status & (IDE_STS_DRQ | IDE_STS_ERR)) == IDE_STS_DRQ) {
                IdePioBlock(ch, FALSE, TRUE, &ch->PioBuffer, &ch->PioLeft, IDE_SECTOR_SIZE);
                goto issued;
            }
        }
        status = IDE_INB(ch->Cmd + IDE_REG_STATUS);
        IdeSetControl(ch, IDE_CTL_NIEN);
        IdeReadResult(ch, Req->Is48Bit, status);
        ch->Devices[Req->Unit].LastStatus = status;
        ch->Devices[Req->Unit].LastError = ch->Result.Error;
        if (IDE_CHIP_IS_CMD(HwInit->Chip)) IdeChipClearInterrupt(HwInit, Channel);
        if (!(status & IDE_STS_ERR) || (status & (IDE_STS_BSY | IDE_STS_DRQ))) IdeResetChannel(HwInit, Channel);
        return SRB_STATUS_ERROR;
    }

issued:
    ch->ActivePio = TRUE;
    ch->Outstanding = Srb;
    return SRB_STATUS_PENDING;
}

/* The PIO command in flight ended (or failed) */
static VOID
IdeFinishPio(IN PHW_DEVICE_EXTENSION HwInit, IN ULONG Channel, IN UCHAR Status, IN ULONG Result)
{
    PIDE_CHANNEL ch = &HwInit->Channels[Channel];
    PIDE_DEVICE dev = &ch->Devices[ch->ActiveUnit];
    PSCSI_REQUEST_BLOCK srb = ch->Outstanding;

    IdeSetControl(ch, IDE_CTL_NIEN);
    if (IDE_CHIP_IS_CMD(HwInit->Chip)) IdeChipClearInterrupt(HwInit, Channel);
    if (Result != IDE_CMD_OK || (Status & (IDE_STS_ERR | IDE_STS_DF))) {
        IdeReadResult(ch, ch->ActiveIs48, Status);
    } else {
        ch->Result.Status = Status;
        ch->Result.Error = 0;
    }
    dev->LastStatus = Status;
    dev->LastError = ch->Result.Error;
    if (Result != IDE_CMD_OK) {
        IDE_DBG((IDE_PFX "Channel %lu.%lu: PIO command failed, status %02lX error %02lX, %lu bytes left\n",
                 Channel, (ULONG)ch->ActiveUnit, (ULONG)Status, (ULONG)ch->Result.Error, ch->PioLeft));
        if (Status & (IDE_STS_BSY | IDE_STS_DRQ)) IdeResetChannel(HwInit, Channel);
    }
    ch->Outstanding = NULL;
    ch->ActivePio = FALSE;
    ch->PioTail = FALSE;
    IdeSatlCompleteDma(HwInit, Channel, srb, Result);
    IdeStartQueued(HwInit, Channel);
}

/* An interrupt (or the watchdog) for the PIO command in flight: the next
   DRQ block, or the end. Returns 1 when the device had something for us
   that an interrupt reported (or should have). */
static ULONG
IdeServicePio(IN PHW_DEVICE_EXTENSION HwInit, IN ULONG Channel, IN BOOLEAN FromInterrupt)
{
    PIDE_CHANNEL ch = &HwInit->Channels[Channel];
    UCHAR status, reason;
    ULONG t;
    BOOLEAN tail = ch->PioTail;

    status = IdeAltStatus(ch);
    if (status & IDE_STS_BSY) return 0;
    status = IDE_INB(ch->Cmd + IDE_REG_STATUS);          /* drops INTRQ */

    if (status & (IDE_STS_ERR | IDE_STS_DF)) {
        IdeFinishPio(HwInit, Channel, status, (status & IDE_STS_ERR) ? IDE_CMD_DEVICE_ERROR : IDE_CMD_HBA_ERROR);
        return 1;
    }
    if (!(status & IDE_STS_DRQ)) {
        /* Done. An ATA transfer that stopped short is an error. */
        IdeFinishPio(HwInit, Channel, status,
                     (ch->PioLeft != 0 && !ch->ActiveAtapi) ? IDE_CMD_HBA_ERROR : IDE_CMD_OK);
        return (ULONG)(tail ? 0 : 1);
    }

    if (ch->ActiveAtapi) {
        reason = IDE_INB(ch->Cmd + IDE_REG_COUNT);
        if (reason & 0x01) {                    /* wants a packet again: not ours */
            IdeFinishPio(HwInit, Channel, status, IDE_CMD_HBA_ERROR);
            return 1;
        }
        IdePioBlock(ch, TRUE, ch->ActiveWrite, &ch->PioBuffer, &ch->PioLeft, 0);
        IdeAltStatus(ch);
        return 1;
    }

    for (;;) {
        if (ch->PioLeft == 0) {                 /* offers data we did not ask for */
            IdeFinishPio(HwInit, Channel, status, IDE_CMD_HBA_ERROR);
            return 1;
        }
        IdePioBlock(ch, FALSE, ch->ActiveWrite, &ch->PioBuffer, &ch->PioLeft, IDE_SECTOR_SIZE);
        if (ch->PioLeft == 0) break;
        /* The next block is often ready within microseconds: taken at
           once instead of waiting for its interrupt (reading the status
           drops that interrupt again) */
        IdeAltStatus(ch);
        for (t = 0; t < IDE_PIO_SPIN_USEC; t++) {
            status = IdeAltStatus(ch);
            if (!(status & IDE_STS_BSY)) break;
            ScsiPortStallExecution(1);
        }
        if (status & IDE_STS_BSY) return 1;
        status = IDE_INB(ch->Cmd + IDE_REG_STATUS);
        if (status & (IDE_STS_ERR | IDE_STS_DF)) {
            IdeFinishPio(HwInit, Channel, status, (status & IDE_STS_ERR) ? IDE_CMD_DEVICE_ERROR : IDE_CMD_HBA_ERROR);
            return 1;
        }
        if (!(status & IDE_STS_DRQ)) {
            IdeFinishPio(HwInit, Channel, status, IDE_CMD_HBA_ERROR);
            return 1;
        }
    }
    if (!ch->ActiveWrite) {
        /* Last block of a read: no interrupt follows. The device drops BSY
           within microseconds; otherwise the watchdog completes it. */
        for (t = 0; t < 100; t++) {
            status = IdeAltStatus(ch);
            if (!(status & IDE_STS_BSY)) break;
            ScsiPortStallExecution(1);
        }
        if (!(status & IDE_STS_BSY)) {
            status = IDE_INB(ch->Cmd + IDE_REG_STATUS);
            IdeFinishPio(HwInit, Channel, status,
                         (status & IDE_STS_ERR) ? IDE_CMD_DEVICE_ERROR :
                         (status & (IDE_STS_DRQ | IDE_STS_DF)) ? IDE_CMD_HBA_ERROR : IDE_CMD_OK);
        } else {
            ch->PioTail = TRUE;
        }
        return 1;
    }
    IdeAltStatus(ch);
    (VOID)FromInterrupt;
    return 1;
}

/* Complete the DMA command on the channel if the hardware is done with
   it. Returns 1 when something was completed. */
ULONG
IdeServiceChannel(IN PHW_DEVICE_EXTENSION HwInit, IN ULONG Channel, IN BOOLEAN FromInterrupt)
{
    PIDE_CHANNEL ch = &HwInit->Channels[Channel];
    PSCSI_REQUEST_BLOCK srb = ch->Outstanding;
    PIDE_DEVICE dev;
    UCHAR bmsts, status;
    ULONG result = IDE_CMD_OK;

    if (srb == NULL) return 0;
    if (ch->ActivePio) return IdeServicePio(HwInit, Channel, FromInterrupt);
    if (ch->Bm == NULL) return 0;

    bmsts = IDE_INB(ch->Bm + BM_REG_STATUS);
    status = IdeAltStatus(ch);

    if (!(bmsts & (BM_STS_INTERRUPT | BM_STS_ERROR))) {
        /* Not finished unless the device dropped BSY/DRQ and the bus
           master ran out of PRDs (an interrupt edge that was lost) */
        if (FromInterrupt) return 0;
        if ((bmsts & BM_STS_ACTIVE) || (status & (IDE_STS_BSY | IDE_STS_DRQ))) return 0;
    } else if (status & IDE_STS_BSY) {
        return 0;
    }

    IDE_OUTB(ch->Bm + BM_REG_COMMAND, 0);
    IDE_OUTB(ch->Bm + BM_REG_STATUS, bmsts | BM_STS_INTERRUPT | BM_STS_ERROR);
    status = IDE_INB(ch->Cmd + IDE_REG_STATUS);
    IdeSetControl(ch, IDE_CTL_NIEN);
    /* INTRQ dropped with the status read: the CMD64x latch can go now */
    if (IDE_CHIP_IS_CMD(HwInit->Chip)) IdeChipClearInterrupt(HwInit, Channel);

    dev = &ch->Devices[ch->ActiveUnit];
    if ((status & (IDE_STS_ERR | IDE_STS_DRQ | IDE_STS_DF)) || (bmsts & BM_STS_ERROR)) {
        IdeReadResult(ch, FALSE, status);
    } else {
        ch->Result.Status = status;
        ch->Result.Error = 0;
    }
    dev->LastStatus = status;
    dev->LastError = ch->Result.Error;

    if (status & IDE_STS_ERR) {
        result = IDE_CMD_DEVICE_ERROR;
    } else if ((bmsts & BM_STS_ERROR) || (status & (IDE_STS_DRQ | IDE_STS_DF))) {
        result = IDE_CMD_HBA_ERROR;
    }

    if (result == IDE_CMD_OK) {
        dev->DmaErrors = 0;
    } else {
        IDE_DBG((IDE_PFX "Channel %lu.%lu: DMA %s failed, status %02lX error %02lX bm %02lX\n",
                 Channel, (ULONG)ch->ActiveUnit, ch->ActiveWrite ? "write" : "read",
                 (ULONG)status, (ULONG)ch->Result.Error, (ULONG)bmsts));
        if (result == IDE_CMD_HBA_ERROR || (dev->LastError & IDE_ERROR_ICRC)) {
            if (++dev->DmaErrors >= IDE_DMA_ERROR_LIMIT) IdeDisableDma(HwInit, Channel, ch->ActiveUnit);
        }
        if (status & (IDE_STS_BSY | IDE_STS_DRQ)) IdeResetChannel(HwInit, Channel);
    }

    ch->Outstanding = NULL;
    IdeSatlCompleteDma(HwInit, Channel, srb, result);

    /* The requests that waited for the channel */
    IdeStartQueued(HwInit, Channel);
    return 1;
}

/* Start the queued requests in order until one is in flight; ScsiPort
   is asked for more for the units it was held back from */
VOID
IdeStartQueued(IN PHW_DEVICE_EXTENSION HwInit, IN ULONG Channel)
{
    PIDE_CHANNEL ch = &HwInit->Channels[Channel];
    PSCSI_REQUEST_BLOCK next;
    ULONG unit;

    while (ch->Outstanding == NULL && ch->QueueCount != 0) {
        next = ch->Queue[ch->QueueHead];
        ch->Queue[ch->QueueHead] = NULL;
        ch->QueueHead = (UCHAR)((ch->QueueHead + 1) % IDE_QUEUE_DEPTH);
        ch->QueueCount--;
        IdeSatlStartPending(HwInit, Channel, next);
    }
    if (ch->LuBlocked != 0 && ch->QueueCount < IDE_QUEUE_DEPTH) {
        for (unit = 0; unit < IDE_DEVICES_PER_CHANNEL; unit++) {
            if (!(ch->LuBlocked & (1 << unit))) continue;
            ch->LuBlocked &= (UCHAR)~(1 << unit);
            ScsiPortNotification(NextLuRequest, HwInit, 0,
                                 (UCHAR)((Channel * IDE_DEVICES_PER_CHANNEL) + unit), 0);
        }
    }
}

/* Poll until the DMA command on the channel is finished */
VOID
IdeDrainChannel(IN PHW_DEVICE_EXTENSION HwInit, IN ULONG Channel)
{
    PIDE_CHANNEL ch = &HwInit->Channels[Channel];
    ULONG elapsed = 0;

    while (ch->Outstanding != NULL) {
        IdeServiceChannel(HwInit, Channel, FALSE);
        if (ch->Outstanding == NULL) break;
        if (elapsed >= IDE_POLL_TIMEOUT_MAX_SEC * 1000000UL) {
            IDE_DBG((IDE_PFX "Channel %lu: DMA command did not finish\n", Channel));
            IdeFailOutstanding(HwInit, Channel, SRB_STATUS_BUS_RESET);
            IdeResetChannel(HwInit, Channel);
            break;
        }
        IDE_POLL_STEP(elapsed, 1);
    }
}

/* Give the in-flight and the waiting requests back to ScsiPort */
VOID
IdeFailOutstanding(IN PHW_DEVICE_EXTENSION HwInit, IN ULONG Channel, IN UCHAR SrbStatus)
{
    PIDE_CHANNEL ch = &HwInit->Channels[Channel];
    PSCSI_REQUEST_BLOCK srb;

    if (ch->Bm != NULL && ch->Outstanding != NULL) {
        IDE_OUTB(ch->Bm + BM_REG_COMMAND, 0);
        IDE_OUTB(ch->Bm + BM_REG_STATUS, IDE_INB(ch->Bm + BM_REG_STATUS) | BM_STS_INTERRUPT | BM_STS_ERROR);
    }
    srb = ch->Outstanding;
    ch->Outstanding = NULL;
    ch->ActivePio = FALSE;
    ch->PioTail = FALSE;
    if (srb != NULL) {
        srb->SrbStatus = SrbStatus;
        ScsiPortNotification(RequestComplete, HwInit, srb);
    }
    while (ch->QueueCount != 0) {
        srb = ch->Queue[ch->QueueHead];
        ch->Queue[ch->QueueHead] = NULL;
        ch->QueueHead = (UCHAR)((ch->QueueHead + 1) % IDE_QUEUE_DEPTH);
        ch->QueueCount--;
        srb->SrbStatus = SrbStatus;
        ScsiPortNotification(RequestComplete, HwInit, srb);
    }
    if (ch->LuBlocked != 0) {
        ULONG unit;
        for (unit = 0; unit < IDE_DEVICES_PER_CHANNEL; unit++) {
            if (ch->LuBlocked & (1 << unit)) {
                ScsiPortNotification(NextLuRequest, HwInit, 0,
                                     (UCHAR)((Channel * IDE_DEVICES_PER_CHANNEL) + unit), 0);
            }
        }
        ch->LuBlocked = 0;
    }
}
