#include "ident.h"

/* idesatl.c - SCSI-to-ATA translation, ATAPI pass-through, miniport IOCTLs */

#define IDE_DEV(hw, ch, unit)   (&(hw)->Channels[ch].Devices[unit])

/* ------------------------------------------------------------------ */
/* 64-bit block number helpers (two ULONGs, no __int64 needed)         */
/* ------------------------------------------------------------------ */

static IDE_LBA
IdeLbaFromBigEndian(IN PUCHAR Bytes, IN ULONG Count)
{
    IDE_LBA lba;
    ULONG i;

    lba.Low = 0;
    lba.High = 0;
    for (i = 0; i < Count; i++) {
        lba.High = (lba.High << 8) | (lba.Low >> 24);
        lba.Low = (lba.Low << 8) | Bytes[i];
    }
    return lba;
}

static ULONG
IdeBigEndian32(IN PUCHAR Bytes)
{
    return ((ULONG)Bytes[0] << 24) | ((ULONG)Bytes[1] << 16) |
           ((ULONG)Bytes[2] << 8)  |  (ULONG)Bytes[3];
}

static VOID
IdePutBigEndian32(OUT PUCHAR Bytes, IN ULONG Value)
{
    Bytes[0] = (UCHAR)(Value >> 24);
    Bytes[1] = (UCHAR)(Value >> 16);
    Bytes[2] = (UCHAR)(Value >> 8);
    Bytes[3] = (UCHAR)Value;
}

static VOID
IdeLbaAdd(IN OUT PIDE_LBA Lba, IN ULONG Count)
{
    ULONG low = Lba->Low + Count;
    if (low < Lba->Low) Lba->High++;
    Lba->Low = low;
}

static IDE_LBA
IdeLastLba(IN PIDE_DEVICE Dev)
{
    IDE_LBA last = Dev->TotalSectors;

    if (last.Low == 0 && last.High == 0) return last;
    if (last.Low == 0) last.High--;
    last.Low--;
    return last;
}

static BOOLEAN
IdeLbaRangeValid(IN PIDE_DEVICE Dev, IN IDE_LBA Lba, IN ULONG Count)
{
    ULONG endLow, endHigh;

    endLow = Lba.Low + Count;
    endHigh = Lba.High;
    if (endLow < Lba.Low) {
        if (endHigh == 0xFFFFFFFF) return FALSE;
        endHigh++;
    }
    if (endHigh != Dev->TotalSectors.High) return (BOOLEAN)(endHigh < Dev->TotalSectors.High);
    return (BOOLEAN)(endLow <= Dev->TotalSectors.Low);
}

static BOOLEAN
IdeLbaFits28(IN IDE_LBA Lba, IN ULONG Count)
{
    if (Lba.High != 0 || Lba.Low >= ATA_LBA28_LIMIT) return FALSE;
    if (Count > ATA_LBA28_MAX_SECTORS) return FALSE;
    return (BOOLEAN)((ATA_LBA28_LIMIT - Lba.Low) > Count);
}

/* ------------------------------------------------------------------ */
/* IDENTIFY DEVICE                                                     */
/* ------------------------------------------------------------------ */

VOID
IdeParseIdentify(IN PHW_DEVICE_EXTENSION HwInit, IN ULONG Channel, IN ULONG Unit)
{
    PIDE_CHANNEL ch = &HwInit->Channels[Channel];
    PIDE_DEVICE dev = &ch->Devices[Unit];
    PUSHORT id = (PUSHORT)ch->BounceBuffer;
    IDE_LBA lba48;
    BOOLEAN w83valid;
    ULONG cyl, heads, spt;

    dev->LbaSupported = FALSE;
    dev->Lba48 = FALSE;
    dev->Fua = FALSE;
    dev->FlushExt = FALSE;
    dev->TotalSectors.Low = 0;
    dev->TotalSectors.High = 0;
    dev->SectorSize = IDE_SECTOR_SIZE;
    dev->PhysExponent = 0;
    dev->Cylinders = 0;
    dev->Heads = 0;
    dev->Spt = 0;
    dev->Cdb16 = FALSE;
    dev->DmaDir = FALSE;
    dev->Trim = FALSE;
    dev->TrimZeroes = FALSE;
    dev->Smart = FALSE;
    dev->RotationRate = 0;
    dev->FormFactor = 0;

    if (dev->IdentifyValid) {
        ULONG i;
        dev->Id[IDE_ID_CONFIG] = id[IDW_GENERAL_CONFIG];
        dev->Id[IDE_ID_CYLINDERS] = id[IDW_CYLINDERS];
        dev->Id[IDE_ID_HEADS] = id[IDW_HEADS];
        dev->Id[IDE_ID_SPT] = id[IDW_SECTORS_PER_TRACK];
        dev->Id[IDE_ID_CMDSET_1] = id[IDW_CMDSET_SUPPORTED_1];
        dev->Id[IDE_ID_CMDSET_EXT] = id[IDW_CMDSET_SUPPORTED_EXT];
        dev->Id[IDE_ID_ENABLED_1] = id[IDW_CMDSET_ENABLED_1];
        for (i = 0; i < 10; i++) dev->Id[IDE_ID_SERIAL + i] = id[IDW_SERIAL + i];
        for (i = 0; i < 4; i++) dev->Id[IDE_ID_FIRMWARE + i] = id[IDW_FIRMWARE + i];
        for (i = 0; i < 20; i++) dev->Id[IDE_ID_MODEL + i] = id[IDW_MODEL + i];
    }

    if (!dev->IdentifyValid) return;

    if (dev->IsAtapi) {
        dev->Cdb16 = (BOOLEAN)((id[IDW_GENERAL_CONFIG] & 0x0003) == IDW0_ATAPI_CDB16);
        dev->DmaDir = (BOOLEAN)((id[IDW_ATAPI_DMADIR] & IDW62_DMADIR_REQUIRED) != 0);
        IDE_DBG((IDE_PFX "Channel %lu.%lu: ATAPI, %lu-byte packets%s\n", Channel, Unit,
                 (ULONG)(dev->Cdb16 ? 16 : 12), dev->DmaDir ? ", DMADIR" : ""));
        return;
    }

    /* Word 106: logical sector size (words 117-118) and logical sectors per
       physical sector; all LBAs and counts are in logical sectors. */
    if ((id[IDW_SECTOR_SIZE_INFO] & IDW106_VALID_MASK) == IDW106_VALID) {
        if (id[IDW_SECTOR_SIZE_INFO] & IDW106_LONG_LOGICAL) {
            ULONG bytes = ((ULONG)id[IDW_LOGICAL_SECTOR_WORDS] |
                           ((ULONG)id[IDW_LOGICAL_SECTOR_WORDS + 1] << 16)) * 2;
            if (bytes >= IDE_SECTOR_SIZE && bytes <= IDE_MAX_SECTOR_SIZE &&
                (bytes & (bytes - 1)) == 0) {
                dev->SectorSize = bytes;
            } else {
                IDE_DBG((IDE_PFX "Channel %lu.%lu: logical sector of %lu bytes not supported\n",
                         Channel, Unit, bytes));
                dev->IdentifyValid = FALSE;
                return;
            }
        }
        if (id[IDW_SECTOR_SIZE_INFO] & IDW106_MULTI_LOGICAL) {
            dev->PhysExponent = (UCHAR)(id[IDW_SECTOR_SIZE_INFO] & IDW106_PHYS_EXP_MASK);
        }
    }

    dev->LbaSupported = (BOOLEAN)((id[IDW_CAPABILITIES] & IDW49_LBA_SUPPORTED) != 0);
    w83valid = (BOOLEAN)((id[IDW_CMDSET_SUPPORTED_2] & IDW83_VALID_MASK) == IDW83_VALID);

    lba48.Low = (ULONG)id[IDW_LBA48_SECTORS] | ((ULONG)id[IDW_LBA48_SECTORS + 1] << 16);
    lba48.High = (ULONG)id[IDW_LBA48_SECTORS + 2] | ((ULONG)id[IDW_LBA48_SECTORS + 3] << 16);

    if (dev->LbaSupported && w83valid && (id[IDW_CMDSET_SUPPORTED_2] & IDW83_LBA48) &&
        (lba48.Low != 0 || lba48.High != 0)) {
        dev->Lba48 = TRUE;
        dev->TotalSectors = lba48;
    } else if (dev->LbaSupported) {
        dev->TotalSectors.Low = (ULONG)id[IDW_LBA28_SECTORS] | ((ULONG)id[IDW_LBA28_SECTORS + 1] << 16);
    }

    /* CHS geometry: the current translation when valid, else the default */
    cyl = id[IDW_CYLINDERS];
    heads = id[IDW_HEADS];
    spt = id[IDW_SECTORS_PER_TRACK];
    if ((id[IDW_FIELD_VALIDITY] & IDW53_WORDS_54_58_VALID) &&
        id[IDW_CUR_CYLINDERS] != 0 && id[IDW_CUR_HEADS] != 0 && id[IDW_CUR_SPT] != 0) {
        cyl = id[IDW_CUR_CYLINDERS];
        heads = id[IDW_CUR_HEADS];
        spt = id[IDW_CUR_SPT];
    }
    if (heads <= 16 && spt <= 255 && cyl != 0 && heads != 0 && spt != 0) {
        dev->Cylinders = (USHORT)cyl;
        dev->Heads = (UCHAR)heads;
        dev->Spt = (UCHAR)spt;
    }
    if (!dev->LbaSupported) {
        /* No LBA: the capacity is the CHS product */
        dev->TotalSectors.Low = (ULONG)dev->Cylinders * dev->Heads * dev->Spt;
    }

    dev->FlushExt = (BOOLEAN)(dev->Lba48 && w83valid && (id[IDW_CMDSET_SUPPORTED_2] & IDW83_FLUSH_EXT));
    dev->Fua = (BOOLEAN)(dev->Lba48 &&
                         (id[IDW_CMDSET_SUPPORTED_EXT] & IDW83_VALID_MASK) == IDW83_VALID &&
                         (id[IDW_CMDSET_SUPPORTED_EXT] & IDW84_FUA));

    /* TRIM (a 48-bit DMA command) and what the medium is */
    dev->Trim = (BOOLEAN)(dev->Lba48 && (id[IDW_DSM_SUPPORT] & IDW169_TRIM) && id[IDW_DSM_SUPPORT] != 0xFFFF);
    dev->TrimZeroes = (BOOLEAN)(dev->Trim && (id[IDW_ADDITIONAL_SUPPORTED] & IDW69_DETERMINISTIC_TRIM) &&
                                (id[IDW_ADDITIONAL_SUPPORTED] & IDW69_ZERO_AFTER_TRIM));
    if (id[IDW_ROTATION_RATE] == 1 || (id[IDW_ROTATION_RATE] >= 0x0401 && id[IDW_ROTATION_RATE] <= 0xFFFE)) {
        dev->RotationRate = id[IDW_ROTATION_RATE];
    }
    dev->FormFactor = (UCHAR)(id[IDW_FORM_FACTOR] & 0x0F);
    dev->Smart = (BOOLEAN)((id[IDW_CMDSET_SUPPORTED_1] & IDW82_SMART) && (id[IDW_CMDSET_ENABLED_1] & IDW82_SMART) &&
                           id[IDW_CMDSET_SUPPORTED_1] != 0xFFFF);

    IDE_DBG((IDE_PFX "Channel %lu.%lu: LBA=%s LBA48=%s sectors=%08lX:%08lX x %lu bytes, CHS %lu/%lu/%lu, FUA=%s write cache=%s\n",
             Channel, Unit,
             dev->LbaSupported ? "yes" : "no",
             dev->Lba48 ? "yes" : "no",
             dev->TotalSectors.High, dev->TotalSectors.Low, dev->SectorSize,
             (ULONG)dev->Cylinders, (ULONG)dev->Heads, (ULONG)dev->Spt,
             dev->Fua ? "yes" : "no",
             (id[IDW_CMDSET_ENABLED_1] & IDW82_WRITE_CACHE) ? "on" : "off"));
    IDE_DBG((IDE_PFX "Channel %lu.%lu: TRIM=%s%s rotation %04lX SMART=%s\n", Channel, Unit,
             dev->Trim ? "yes" : "no", dev->TrimZeroes ? " (zeroes)" : "", (ULONG)dev->RotationRate,
             dev->Smart ? "yes" : "no"));
}

/* ------------------------------------------------------------------ */
/* Sense data                                                          */
/* ------------------------------------------------------------------ */

static VOID
IdeBuildSense(OUT PSENSE_DATA Sense, IN UCHAR Key, IN UCHAR Asc, IN UCHAR Ascq)
{
    ZeroMemoryBytes(Sense, sizeof(SENSE_DATA));
    Sense->ErrorCode = 0x70;
    Sense->Valid = (Key != SCSI_SENSE_NO_SENSE) ? 1 : 0;
    Sense->SenseKey = Key;
    Sense->AdditionalSenseLength = sizeof(SENSE_DATA) - 8;
    Sense->AdditionalSenseCode = Asc;
    Sense->AdditionalSenseCodeQualifier = Ascq;
}

static VOID
IdeCopyToSrb(IN PSCSI_REQUEST_BLOCK Srb, IN PVOID Data, IN ULONG Length)
{
    ULONG n = Length;

    if (Srb->DataBuffer == NULL) return;
    if (n > Srb->DataTransferLength) n = Srb->DataTransferLength;
    ZeroMemoryBytes(Srb->DataBuffer, Srb->DataTransferLength);
    CopyMemoryBytes(Srb->DataBuffer, Data, n);
}

static VOID
IdeGood(IN PSCSI_REQUEST_BLOCK Srb)
{
    Srb->SrbStatus = SRB_STATUS_SUCCESS;
    Srb->ScsiStatus = SCSISTAT_GOOD;
}

static VOID
IdeFailWithRawSense(IN PIDE_DEVICE Dev, IN PSCSI_REQUEST_BLOCK Srb, IN PVOID Sense, IN ULONG Length)
{
    Srb->SrbStatus = SRB_STATUS_ERROR;
    Srb->ScsiStatus = SCSISTAT_CHECK_CONDITION;

#if IDE_AUTOSENSE
    if (Srb->SenseInfoBuffer != NULL && Srb->SenseInfoBufferLength != 0 &&
        !(Srb->SrbFlags & SRB_FLAGS_DISABLE_AUTOSENSE)) {
        ULONG n = Length;
        if (n > Srb->SenseInfoBufferLength) n = Srb->SenseInfoBufferLength;
        ZeroMemoryBytes(Srb->SenseInfoBuffer, Srb->SenseInfoBufferLength);
        CopyMemoryBytes(Srb->SenseInfoBuffer, Sense, n);
        Srb->SrbStatus |= SRB_STATUS_AUTOSENSE_VALID;
        return;
    }
#endif

    if (Length > IDE_SENSE_MAX) Length = IDE_SENSE_MAX;
    CopyMemoryBytes(Dev->Sense, Sense, Length);
    Dev->SenseLength = (UCHAR)Length;
    Dev->SensePending = TRUE;
}

static VOID
IdeFailWithSense(IN PIDE_DEVICE Dev, IN PSCSI_REQUEST_BLOCK Srb, IN UCHAR Key, IN UCHAR Asc, IN UCHAR Ascq)
{
    SENSE_DATA sense;

    if (Key == SCSI_SENSE_NO_SENSE) {
        Srb->SrbStatus = SRB_STATUS_ERROR;
        Srb->ScsiStatus = SCSISTAT_CHECK_CONDITION;
        return;
    }
    IdeBuildSense(&sense, Key, Asc, Ascq);
    IdeFailWithRawSense(Dev, Srb, &sense, sizeof(sense));
}

static VOID
IdeInvalidField(IN PIDE_DEVICE Dev, IN PSCSI_REQUEST_BLOCK Srb)
{
    IdeFailWithSense(Dev, Srb, SCSI_SENSE_ILLEGAL_REQUEST, IDE_ASC_INVALID_CDB_FIELD, 0);
}

/* ATA Status/Error -> sense (subset of the SAT / libata mapping) */
static VOID
IdeAtaErrorToSense(IN UCHAR Status, IN UCHAR Error, OUT PUCHAR Key, OUT PUCHAR Asc, OUT PUCHAR Ascq)
{
    *Key = SCSI_SENSE_NO_SENSE;
    *Asc = 0;
    *Ascq = 0;

    if (!(Status & IDE_STS_ERR)) {
        if (Status & IDE_STS_DF) { *Key = SCSI_SENSE_HARDWARE_ERROR; *Asc = 0x44; }
        return;                                 /* bus master error / timeout */
    }
    if (Error & (IDE_ERROR_MEDIA_CHANGE | IDE_ERROR_MEDIA_CHANGE_REQ)) {
        *Key = SCSI_SENSE_UNIT_ATTENTION;  *Asc = IDE_ASC_MEDIUM_CHANGED;
    } else if (Error & IDE_ERROR_ICRC) {
        *Key = SCSI_SENSE_ABORTED_COMMAND; *Asc = IDE_ASC_IU_CRC_ERROR; *Ascq = 0x03;
    } else if (Error & IDE_ERROR_UNCORRECTABLE) {
        *Key = SCSI_SENSE_MEDIUM_ERROR;    *Asc = IDE_ASC_UNRECOVERED_READ; *Ascq = 0x04;
    } else if (Error & IDE_ERROR_ID_NOT_FOUND) {
        *Key = SCSI_SENSE_ILLEGAL_REQUEST; *Asc = IDE_ASC_LBA_OUT_OF_RANGE;
    } else if (Error & IDE_ERROR_ABORT) {
        *Key = SCSI_SENSE_ABORTED_COMMAND;
    } else {
        *Key = SCSI_SENSE_HARDWARE_ERROR;
    }
}

VOID
IdeSatlCompleteAtaError(IN PHW_DEVICE_EXTENSION HwInit, IN ULONG Channel, IN PSCSI_REQUEST_BLOCK Srb)
{
    PIDE_DEVICE dev = IDE_DEV(HwInit, Channel, IDE_UNIT_OF_TARGET(Srb->TargetId));
    UCHAR key, asc, ascq;

    IdeAtaErrorToSense(dev->LastStatus, dev->LastError, &key, &asc, &ascq);
    IdeFailWithSense(dev, Srb, key, asc, ascq);
}

static VOID
IdeCompleteResult(IN PHW_DEVICE_EXTENSION HwInit, IN ULONG Channel, IN PSCSI_REQUEST_BLOCK Srb, IN ULONG Result)
{
    switch (Result) {
    case IDE_CMD_OK:
        IdeGood(Srb);
        break;
    case IDE_CMD_BAD_BUFFER:
        Srb->SrbStatus = SRB_STATUS_INVALID_REQUEST;
        break;
    default:
        IdeSatlCompleteAtaError(HwInit, Channel, Srb);
        break;
    }
}

/* ------------------------------------------------------------------ */
/* Commands answered by the driver itself                              */
/* ------------------------------------------------------------------ */

static VOID
IdeExtractAtaString(IN PUSHORT IdentifyWords, IN ULONG StartWord, IN ULONG NumWords,
                    OUT PUCHAR OutBuffer, IN ULONG OutBufferMax)
{
    ULONG i;
    ULONG outIndex = 0;
    USHORT w;

    for (i = 0; i < NumWords && (outIndex + 1) < OutBufferMax; i++) {
        w = IdentifyWords[StartWord + i];
        OutBuffer[outIndex++] = (UCHAR)(w >> 8);
        OutBuffer[outIndex++] = (UCHAR)w;
    }
    while (outIndex > 0 && (OutBuffer[outIndex - 1] == ' ' || OutBuffer[outIndex - 1] == '\0')) {
        outIndex--;
    }
    while (outIndex < OutBufferMax) {
        OutBuffer[outIndex++] = ' ';
    }
}

/* Peripheral device type of an ATAPI device: IDENTIFY PACKET DEVICE word 0
   bits 12:8 (05h CD/DVD, 00h direct access such as ZIP, LS-120 or
   SuperDisk, 07h optical memory, 01h tape); 1Fh (unknown) and a device
   without IDENTIFY data count as CD/DVD */
static UCHAR
IdeAtapiDeviceType(IN PIDE_DEVICE Dev)
{
    UCHAR type;

    if (!Dev->IdentifyValid) return READ_ONLY_DIRECT_ACCESS_DEVICE;
    type = (UCHAR)((Dev->Id[IDE_ID_CONFIG] >> 8) & 0x1F);
    return (type == 0x1F) ? (UCHAR)READ_ONLY_DIRECT_ACCESS_DEVICE : type;
}

static VOID
IdeHandleInquiryVpd(IN PHW_DEVICE_EXTENSION HwInit, IN PIDE_DEVICE Dev, IN PSCSI_REQUEST_BLOCK Srb)
{
    UCHAR page[64];
    ULONG len, allocLen = ((ULONG)Srb->Cdb[3] << 8) | Srb->Cdb[4];
    BOOLEAN unmap = (BOOLEAN)(Dev->Trim && Dev->UseDma && HwInit->TrimAllowed);

    ZeroMemoryBytes(page, sizeof(page));
    page[0] = Dev->IsAtapi ? IdeAtapiDeviceType(Dev) : DIRECT_ACCESS_DEVICE;
    page[1] = Srb->Cdb[2];

    if (Dev->IsAtapi && Srb->Cdb[2] != IDE_VPD_SUPPORTED_PAGES && Srb->Cdb[2] != IDE_VPD_SERIAL_NUMBER) {
        IdeInvalidField(Dev, Srb);
        return;
    }
    switch (Srb->Cdb[2]) {
    case IDE_VPD_SUPPORTED_PAGES:
        page[4] = IDE_VPD_SUPPORTED_PAGES;
        page[5] = IDE_VPD_SERIAL_NUMBER;
        len = 6;
        if (!Dev->IsAtapi) {
            page[len++] = IDE_VPD_BLOCK_LIMITS;
            page[len++] = IDE_VPD_BLOCK_CHARACTERISTICS;
            page[len++] = IDE_VPD_PROVISIONING;
        }
        page[3] = (UCHAR)(len - 4);
        break;

    case IDE_VPD_BLOCK_LIMITS:
        page[3] = 0x3C;
        IdePutBigEndian32(&page[8], IDE_MAX_TRANSFER / Dev->SectorSize);
        IdePutBigEndian32(&page[12], IDE_MAX_TRANSFER / Dev->SectorSize);
        if (unmap) {
            IdePutBigEndian32(&page[20], IDE_UNMAP_MAX_BLOCKS);
            IdePutBigEndian32(&page[24], IDE_UNMAP_MAX_DESCRIPTORS);
            IdePutBigEndian32(&page[28], 1UL << Dev->PhysExponent);
        }
        len = 4 + 0x3C;
        break;

    case IDE_VPD_BLOCK_CHARACTERISTICS:
        page[3] = 0x3C;
        page[4] = (UCHAR)(Dev->RotationRate >> 8);
        page[5] = (UCHAR)Dev->RotationRate;
        page[7] = Dev->FormFactor;
        len = 4 + 0x3C;
        break;

    case IDE_VPD_PROVISIONING:
        page[3] = 4;
        page[5] = (UCHAR)((unmap ? 0x80 : 0) | ((unmap && Dev->TrimZeroes) ? 0x04 : 0));
        len = 4 + 4;
        break;

    case IDE_VPD_SERIAL_NUMBER:
        page[3] = 20;
        if (Dev->IdentifyValid) {
            IdeExtractAtaString(Dev->Id, IDE_ID_SERIAL, 10, &page[4], 20);
        } else {
            ULONG i;
            for (i = 0; i < 20; i++) page[4 + i] = ' ';
        }
        len = 4 + 20;
        break;

    default:
        IdeInvalidField(Dev, Srb);
        return;
    }

    if (len > allocLen) len = allocLen;
    IdeCopyToSrb(Srb, page, len);
    IdeGood(Srb);
}

static VOID
IdeHandleInquiry(IN PHW_DEVICE_EXTENSION HwInit, IN PIDE_DEVICE Dev, IN PSCSI_REQUEST_BLOCK Srb)
{
    PUSHORT id = Dev->Id;
    INQUIRYDATA inq;
    PUCHAR v, p;
    ULONG i;

    if (Srb->Cdb[1] & 0x01) {
        IdeHandleInquiryVpd(HwInit, Dev, Srb);
        return;
    }
    if (Srb->Cdb[2] != 0) {
        IdeInvalidField(Dev, Srb);
        return;
    }

    ZeroMemoryBytes(&inq, sizeof(inq));
    inq.DeviceType = Dev->IsAtapi ? IdeAtapiDeviceType(Dev) : DIRECT_ACCESS_DEVICE;
    /* ATAPI: word 0 bit 7, and every CD/DVD drive as before */
    inq.RemovableMedia = (Dev->IsAtapi && inq.DeviceType == READ_ONLY_DIRECT_ACCESS_DEVICE) ? 1 :
                         ((id[IDE_ID_CONFIG] & IDW0_REMOVABLE) ? 1 : 0);
    inq.Versions = Dev->IsAtapi ? 0 : IDE_INQUIRY_VERSION_SPC3;
    inq.ResponseDataFormat = 2;
    inq.AdditionalLength = 31;
    inq.CommandQueue = 0;

    if (Dev->IdentifyValid) {
        IdeExtractAtaString(id, IDE_ID_MODEL, 4, inq.VendorId, 8);
        IdeExtractAtaString(id, IDE_ID_MODEL + 4, 8, inq.ProductId, 16);
        IdeExtractAtaString(id, IDE_ID_FIRMWARE, 2, inq.ProductRevisionLevel, 4);
    } else {
        v = (PUCHAR)(Dev->IsAtapi ? "ATAPI   " : "ATA     ");
        p = (PUCHAR)(Dev->IsAtapi ? "IDE CD-ROM      " : "IDE HARDDISK    ");
        for (i = 0; i < 8; i++) inq.VendorId[i] = v[i];
        for (i = 0; i < 16; i++) inq.ProductId[i] = p[i];
        inq.ProductRevisionLevel[0] = '1';
        inq.ProductRevisionLevel[1] = '.';
        inq.ProductRevisionLevel[2] = '0';
        inq.ProductRevisionLevel[3] = '0';
    }

    IdeCopyToSrb(Srb, &inq, sizeof(inq));
    IdeGood(Srb);
}

static VOID
IdeHandleReportLuns(IN PSCSI_REQUEST_BLOCK Srb)
{
    UCHAR data[16];
    ULONG allocLen = IdeBigEndian32(&Srb->Cdb[6]);
    ULONG len = 16;

    ZeroMemoryBytes(data, sizeof(data));
    IdePutBigEndian32(&data[0], 8);
    if (len > allocLen) len = allocLen;
    IdeCopyToSrb(Srb, data, len);
    IdeGood(Srb);
}

/* READ CAPACITY(10) for 2 TB and more (last LBA FFFFFFFFh or beyond):
   FFFFFFFFh sends a class driver that knows READ CAPACITY(16) there.
   One that does not (NT 3.x/4.0, 9x, 2000, XP x86) adds one to it in 32
   bits and sees a disk of 0 bytes; it gets FFFFFFFEh, the most it can
   address with READ(10), and sees the first 2 TB. */
static VOID
IdeHandleReadCapacity10(IN PHW_DEVICE_EXTENSION HwInit, IN PIDE_DEVICE Dev, IN PSCSI_REQUEST_BLOCK Srb)
{
    UCHAR data[8];
    IDE_LBA last = IdeLastLba(Dev);

    if (last.High != 0 || last.Low == 0xFFFFFFFF) {
        last.Low = HwInit->Capacity16 ? 0xFFFFFFFF : 0xFFFFFFFE;
    }
    IdePutBigEndian32(&data[0], last.Low);
    IdePutBigEndian32(&data[4], Dev->SectorSize);
    IdeCopyToSrb(Srb, data, sizeof(data));
    IdeGood(Srb);
}

static VOID
IdeHandleReadCapacity16(IN PHW_DEVICE_EXTENSION HwInit, IN PIDE_DEVICE Dev, IN PSCSI_REQUEST_BLOCK Srb)
{
    UCHAR data[IDE_READ_CAPACITY16_DATA_LEN];
    IDE_LBA last = IdeLastLba(Dev);
    ULONG allocLen = IdeBigEndian32(&Srb->Cdb[10]);
    ULONG n;

    ZeroMemoryBytes(data, sizeof(data));
    IdePutBigEndian32(&data[0], last.High);
    IdePutBigEndian32(&data[4], last.Low);
    IdePutBigEndian32(&data[8], Dev->SectorSize);
    data[13] = (UCHAR)(Dev->PhysExponent & 0x0F);
    if (Dev->Trim && Dev->UseDma && HwInit->TrimAllowed) {
        data[14] = (UCHAR)(0x80 | (Dev->TrimZeroes ? 0x40 : 0));        /* LBPME, LBPRZ */
    }

    n = sizeof(data);
    if (n > allocLen) n = allocLen;
    IdeCopyToSrb(Srb, data, n);
    IdeGood(Srb);
}

static VOID
IdeHandleRequestSenseAta(IN PIDE_DEVICE Dev, IN PSCSI_REQUEST_BLOCK Srb)
{
    SENSE_DATA sense;

    if (Dev->SensePending) {
        IdeCopyToSrb(Srb, Dev->Sense, Dev->SenseLength);
        Dev->SensePending = FALSE;
    } else {
        IdeBuildSense(&sense, SCSI_SENSE_NO_SENSE, 0, 0);
        IdeCopyToSrb(Srb, &sense, sizeof(sense));
    }
    IdeGood(Srb);
}

static VOID
IdeHandleModeSenseAta(IN PIDE_DEVICE Dev, IN PSCSI_REQUEST_BLOCK Srb)
{
    PUSHORT id = Dev->Id;
    BOOLEAN ten = (BOOLEAN)(Srb->Cdb[0] == SCSIOP_MODE_SENSE10);
    BOOLEAN dbd = (BOOLEAN)((Srb->Cdb[1] & 0x08) != 0);
    BOOLEAN llba = (BOOLEAN)(ten && (Srb->Cdb[1] & 0x10) != 0);
    UCHAR pageCode = (UCHAR)(Srb->Cdb[2] & 0x3F);
    UCHAR pc = (UCHAR)(Srb->Cdb[2] >> 6);
    ULONG allocLen;
    UCHAR buf[96];
    ULONG hdrLen, len, descLen;
    ULONG blocks, cyls, heads, spt;
    PUCHAR p;
    BOOLEAN any = FALSE;

    allocLen = ten ? (((ULONG)Srb->Cdb[7] << 8) | Srb->Cdb[8]) : Srb->Cdb[4];

    ZeroMemoryBytes(buf, sizeof(buf));
    hdrLen = ten ? 8 : 4;
    len = hdrLen;

    descLen = 0;
    if (!dbd && llba) {
        p = &buf[len];
        IdePutBigEndian32(&p[0], Dev->TotalSectors.High);
        IdePutBigEndian32(&p[4], Dev->TotalSectors.Low);
        IdePutBigEndian32(&p[12], Dev->SectorSize);
        descLen = 16;
        len += 16;
    } else if (!dbd) {
        blocks = (Dev->TotalSectors.High != 0 || Dev->TotalSectors.Low > 0xFFFFFF)
                     ? 0xFFFFFF : Dev->TotalSectors.Low;
        p = &buf[len];
        p[1] = (UCHAR)(blocks >> 16);
        p[2] = (UCHAR)(blocks >> 8);
        p[3] = (UCHAR)blocks;
        p[5] = (UCHAR)(Dev->SectorSize >> 16);
        p[6] = (UCHAR)(Dev->SectorSize >> 8);
        p[7] = (UCHAR)Dev->SectorSize;
        descLen = 8;
        len += 8;
    }

    if (pageCode == IDE_MODE_PAGE_GEOMETRY || pageCode == IDE_MODE_PAGE_ALL) {
        cyls = Dev->Cylinders;
        heads = Dev->Heads;
        spt = Dev->Spt;
        if (cyls == 0 || heads == 0 || spt == 0) {
            ULONG total32 = (Dev->TotalSectors.High != 0) ? 0xFFFFFFFF : Dev->TotalSectors.Low;
            heads = 255;
            spt = 63;
            cyls = total32 / (heads * spt);
            if (cyls == 0) cyls = 1;
        }
        p = &buf[len];
        p[0] = IDE_MODE_PAGE_GEOMETRY;
        p[1] = 0x16;
        if (pc != 1) {
            p[2] = (UCHAR)(cyls >> 16);
            p[3] = (UCHAR)(cyls >> 8);
            p[4] = (UCHAR)cyls;
            p[5] = (UCHAR)heads;
            p[20] = (UCHAR)(spt >> 8);
            p[21] = (UCHAR)spt;
        }
        len += 24;
        any = TRUE;
    }

    if (pageCode == IDE_MODE_PAGE_CACHING || pageCode == IDE_MODE_PAGE_ALL) {
        p = &buf[len];
        p[0] = IDE_MODE_PAGE_CACHING;
        p[1] = 0x12;
        if (pc == 1) {
            if (id[IDE_ID_CMDSET_1] & IDW82_WRITE_CACHE) p[2] |= 0x04;
            if (id[IDE_ID_CMDSET_1] & IDW82_LOOKAHEAD) p[12] |= 0x20;
        } else {
            if (id[IDE_ID_ENABLED_1] & IDW82_WRITE_CACHE) p[2] |= 0x04;
            if (!(id[IDE_ID_ENABLED_1] & IDW82_LOOKAHEAD)) p[12] |= 0x20;
        }
        len += 20;
        any = TRUE;
    }

    if (pageCode == IDE_MODE_PAGE_CONTROL || pageCode == IDE_MODE_PAGE_ALL) {
        p = &buf[len];
        p[0] = IDE_MODE_PAGE_CONTROL;
        p[1] = 0x0A;
        len += 12;
        any = TRUE;
    }

    if (!any && pageCode != 0) {
        IdeInvalidField(Dev, Srb);
        return;
    }

    /* DPOFUA: FUA writes are honoured (WRITE DMA FUA EXT, or a flush) */
    if (ten) {
        buf[0] = (UCHAR)((len - 2) >> 8);
        buf[1] = (UCHAR)(len - 2);
        buf[3] = 0x10;
        buf[4] = (UCHAR)((descLen == 16) ? 0x01 : 0x00);
        buf[7] = (UCHAR)descLen;
    } else {
        buf[0] = (UCHAR)(len - 1);
        buf[2] = 0x10;
        buf[3] = (UCHAR)descLen;
    }

    if (len > allocLen) len = allocLen;
    IdeCopyToSrb(Srb, buf, len);
    IdeGood(Srb);
}

static VOID
IdeHandleModeSelectAta(IN PHW_DEVICE_EXTENSION HwInit, IN ULONG Channel, IN ULONG Unit, IN PSCSI_REQUEST_BLOCK Srb)
{
    PIDE_DEVICE dev = IDE_DEV(HwInit, Channel, Unit);
    BOOLEAN ten = (BOOLEAN)(Srb->Cdb[0] == IDE_SCSIOP_MODE_SELECT10);
    PUCHAR data = (PUCHAR)Srb->DataBuffer;
    ULONG listLen, pos, hdrLen, pageLen;
    BOOLEAN wce, dra;

    listLen = ten ? (((ULONG)Srb->Cdb[7] << 8) | Srb->Cdb[8]) : Srb->Cdb[4];
    if (listLen > Srb->DataTransferLength) listLen = Srb->DataTransferLength;
    if (listLen == 0 || data == NULL) {
        IdeGood(Srb);
        return;
    }

    hdrLen = ten ? 8 : 4;
    if (listLen < hdrLen) {
        IdeFailWithSense(dev, Srb, SCSI_SENSE_ILLEGAL_REQUEST, IDE_ASC_PARAM_LIST_LENGTH, 0);
        return;
    }
    pos = hdrLen + (ten ? (((ULONG)data[6] << 8) | data[7]) : data[3]);

    while (pos + 2 <= listLen) {
        pageLen = (ULONG)data[pos + 1] + 2;
        if (pos + pageLen > listLen) break;
        if ((data[pos] & 0x3F) == IDE_MODE_PAGE_CACHING && pageLen >= 13) {
            wce = (BOOLEAN)((data[pos + 2] & 0x04) != 0);
            dra = (BOOLEAN)((data[pos + 12] & 0x20) != 0);

            if (dev->Id[IDE_ID_CMDSET_1] & IDW82_WRITE_CACHE) {
                dev->WantWriteCache = (UCHAR)(wce ? 1 : 2);
            } else if (wce) {
                IdeFailWithSense(dev, Srb, SCSI_SENSE_ILLEGAL_REQUEST, IDE_ASC_INVALID_PARAM_FIELD, 0);
                return;
            }
            if (dev->Id[IDE_ID_CMDSET_1] & IDW82_LOOKAHEAD) {
                dev->WantLookAhead = (UCHAR)(dra ? 2 : 1);
            }
            IdeApplyDeviceSettings(HwInit, Channel, Unit);
            /* IDENTIFY again: word 85 shows what is in effect now */
            if (IdeIdentifyDevice(HwInit, Channel, Unit)) IdeParseIdentify(HwInit, Channel, Unit);
        }
        pos += pageLen;
    }
    IdeGood(Srb);
}

/* ------------------------------------------------------------------ */
/* ATA disk commands                                                   */
/* ------------------------------------------------------------------ */

BOOLEAN
IdeFlushDevice(IN PHW_DEVICE_EXTENSION HwInit, IN ULONG Channel, IN ULONG Unit)
{
    PIDE_DEVICE dev = IDE_DEV(HwInit, Channel, Unit);

    if (!dev->Present || dev->IsAtapi || !dev->IdentifyValid) return TRUE;
    if (!(dev->Id[IDE_ID_CMDSET_1] & IDW82_WRITE_CACHE)) return TRUE;

    return (BOOLEAN)(IdeRunSimple(HwInit, Channel, Unit,
                                  (UCHAR)(dev->FlushExt ? IDE_COMMAND_FLUSH_CACHE_EXT : IDE_COMMAND_FLUSH_CACHE),
                                  0, 0, IDE_POLL_TIMEOUT_POWER_SEC) == IDE_CMD_OK);
}

/* System shutdown: write cache flushed. With STANDBY=1 also STANDBY
   IMMEDIATE, so the drive parks its heads (or an SSD saves its tables)
   before the power goes; a request after it spins the drive up again. */
VOID
IdeShutdownDevice(IN PHW_DEVICE_EXTENSION HwInit, IN ULONG Channel, IN ULONG Unit)
{
    PIDE_DEVICE dev = IDE_DEV(HwInit, Channel, Unit);

    if (!dev->Present || dev->IsAtapi || !dev->IdentifyValid) return;
    IdeFlushDevice(HwInit, Channel, Unit);
    if (!HwInit->StandbyAtShutdown) return;
    if (IdeRunSimple(HwInit, Channel, Unit, IDE_COMMAND_STANDBY_IMMEDIATE, 0, 0, IDE_POLL_TIMEOUT_POWER_SEC) == IDE_CMD_OK) {
        IDE_DBG((IDE_PFX "Channel %lu.%lu: standby for shutdown\n", Channel, Unit));
    }
}

/* DATA SET MANAGEMENT TRIM of Count sectors from Lba: one 512-byte block
   of up to 64 ranges per command, through the channel's bounce buffer */
static ULONG
IdeTrim(IN PHW_DEVICE_EXTENSION HwInit, IN ULONG Channel, IN ULONG Unit, IN IDE_LBA Lba, IN ULONG Count)
{
    PIDE_CHANNEL ch = &HwInit->Channels[Channel];
    PULONG range;
    ATA_REQUEST req;
    ULONG n, piece, result;

    while (Count > 0) {
        range = (PULONG)ch->BounceBuffer;
        ZeroMemoryBytes(range, IDE_BOUNCE_SIZE);
        for (n = 0; n < IDE_DSM_RANGES_PER_BLOCK && Count > 0; n++) {
            piece = (Count > IDE_DSM_MAX_RANGE) ? IDE_DSM_MAX_RANGE : Count;
            range[n * 2] = Lba.Low;
            range[n * 2 + 1] = (Lba.High & 0xFFFF) | (piece << 16);
            IdeLbaAdd(&Lba, piece);
            Count -= piece;
        }
        ZeroMemoryBytes(&req, sizeof(req));
        req.Unit = (UCHAR)Unit;
        req.Command = IDE_COMMAND_DATA_SET_MANAGEMENT;
        req.Features = IDE_DSM_TRIM;
        req.Is48Bit = TRUE;
        req.SectorCount = 1;
        req.DataBuffer = ch->BounceBuffer;
        req.DataBufferLen = IDE_BOUNCE_SIZE;
        req.IsWrite = TRUE;
        req.UseDma = TRUE;
        req.TimeoutSec = IDE_POLL_TIMEOUT_POWER_SEC;
        result = IdeRunCommand(HwInit, Channel, NULL, &req);
        if (result != IDE_CMD_OK) return result;
    }
    return IDE_CMD_OK;
}

/* C/H/S for a device without LBA, packed as IdeWriteTaskFile wants it */
static BOOLEAN
IdeChsFromLba(IN PIDE_DEVICE Dev, IN IDE_LBA Lba, OUT PIDE_LBA Packed)
{
    ULONG track, sector, head, cyl;

    if (Dev->Spt == 0 || Dev->Heads == 0 || Lba.High != 0) return FALSE;
    track = Lba.Low / Dev->Spt;
    sector = Lba.Low % Dev->Spt + 1;
    head = track % Dev->Heads;
    cyl = track / Dev->Heads;
    if (cyl > 0xFFFF) return FALSE;
    Packed->High = 0;
    Packed->Low = sector | (head << 8) | (cyl << 16);
    return TRUE;
}

/* Fill an ATA_REQUEST for a READ/WRITE; FALSE when the request cannot be
   expressed (then *FlushAfter says nothing) */
static BOOLEAN
IdeBuildReadWrite(
    IN PIDE_DEVICE Dev,
    IN ULONG Unit,
    IN PVOID Buffer,
    IN IDE_LBA Lba,
    IN ULONG Count,
    IN BOOLEAN IsWrite,
    IN BOOLEAN AllowDma,
    IN OUT PBOOLEAN Fua,
    OUT PATA_REQUEST Req
)
{
    BOOLEAN dma = (BOOLEAN)(Dev->UseDma && AllowDma);

    ZeroMemoryBytes(Req, sizeof(*Req));
    Req->Unit = (UCHAR)Unit;
    Req->DataBuffer = Buffer;
    Req->DataBufferLen = Count * Dev->SectorSize;
    Req->IsWrite = IsWrite;
    Req->SectorCount = Count;

    if (!Dev->LbaSupported) {
        if (Count > ATA_LBA28_MAX_SECTORS || !IdeChsFromLba(Dev, Lba, &Req->Lba)) return FALSE;
        Req->Chs = TRUE;
        Req->Command = dma ? (IsWrite ? IDE_COMMAND_WRITE_DMA : IDE_COMMAND_READ_DMA)
                           : (IsWrite ? IDE_COMMAND_WRITE_SECTORS : IDE_COMMAND_READ_SECTORS);
        Req->UseDma = dma;
        return TRUE;
    }

    Req->Lba = Lba;
    if (*Fua && dma && Dev->Fua && !Dev->NoLba48Dma && Count <= ATA_LBA48_MAX_SECTORS) {
        Req->Is48Bit = TRUE;
        Req->Command = IDE_COMMAND_WRITE_DMA_FUA_EXT;
        *Fua = FALSE;
    } else if (IdeLbaFits28(Lba, Count)) {
        Req->Command = dma ? (IsWrite ? IDE_COMMAND_WRITE_DMA : IDE_COMMAND_READ_DMA)
                           : (IsWrite ? IDE_COMMAND_WRITE_SECTORS : IDE_COMMAND_READ_SECTORS);
    } else if (Dev->Lba48 && Count <= ATA_LBA48_MAX_SECTORS) {
        /* A controller without 48-bit DMA: the EXT commands with PIO */
        if (Dev->NoLba48Dma) dma = FALSE;
        Req->Is48Bit = TRUE;
        Req->Command = dma ? (IsWrite ? IDE_COMMAND_WRITE_DMA_EXT : IDE_COMMAND_READ_DMA_EXT)
                           : (IsWrite ? IDE_COMMAND_WRITE_SECTORS_EXT : IDE_COMMAND_READ_SECTORS_EXT);
    } else {
        return FALSE;
    }
    Req->UseDma = dma;
    return TRUE;
}

/* One read or write, polled. DMA that cannot be set up for the buffer, or
   that the device dropped meanwhile after errors, goes again with PIO. */
static ULONG
IdeRunReadWrite(
    IN PHW_DEVICE_EXTENSION HwInit,
    IN ULONG Channel,
    IN PSCSI_REQUEST_BLOCK Srb,
    IN PIDE_DEVICE Dev,
    IN ULONG Unit,
    IN PVOID Buffer,
    IN IDE_LBA Lba,
    IN ULONG Count,
    IN BOOLEAN IsWrite,
    IN OUT PBOOLEAN Fua
)
{
    ATA_REQUEST req;
    ULONG result;
    BOOLEAN fua = *Fua;

    if (!IdeBuildReadWrite(Dev, Unit, Buffer, Lba, Count, IsWrite, TRUE, Fua, &req)) return IDE_CMD_BAD_BUFFER;
    result = IdeRunCommand(HwInit, Channel, Srb, &req);
    if (req.UseDma && (result == IDE_CMD_BAD_BUFFER || (result != IDE_CMD_OK && !Dev->UseDma))) {
        *Fua = fua;
        if (IdeBuildReadWrite(Dev, Unit, Buffer, Lba, Count, IsWrite, FALSE, Fua, &req)) {
            result = IdeRunCommand(HwInit, Channel, Srb, &req);
        }
    }
    return result;
}

/* TRIM mode: the write's data is the pattern, repeated from the first byte */
static BOOLEAN
IdeIsTrimPattern(IN PHW_DEVICE_EXTENSION HwInit, IN PUCHAR Data, IN ULONG Length)
{
    ULONG i, j;

    if (Data == NULL || Length < IDE_TRIM_PATTERN_SIZE) return FALSE;
    for (i = 0; i < Length; i += IDE_TRIM_PATTERN_SIZE) {
        for (j = 0; j < IDE_TRIM_PATTERN_SIZE && i + j < Length; j += 4) {
            if (*(ULONG UNALIGNED *)(Data + i + j) != *(ULONG UNALIGNED *)(&HwInit->TrimPattern[j])) return FALSE;
        }
    }
    return TRUE;
}

/* UNMAP: the block descriptors become DATA SET MANAGEMENT TRIM ranges */
static VOID
IdeAtaUnmap(IN PHW_DEVICE_EXTENSION HwInit, IN ULONG Channel, IN PSCSI_REQUEST_BLOCK Srb)
{
    ULONG unit = IDE_UNIT_OF_TARGET(Srb->TargetId);
    PIDE_DEVICE dev = IDE_DEV(HwInit, Channel, unit);
    PUCHAR param = (PUCHAR)Srb->DataBuffer;
    ULONG listLen = ((ULONG)Srb->Cdb[7] << 8) | Srb->Cdb[8], descLen, n, i, count, result;
    IDE_LBA lba;

    if (!dev->Trim || !dev->UseDma || !HwInit->TrimAllowed) {
        IdeFailWithSense(dev, Srb, SCSI_SENSE_ILLEGAL_REQUEST, IDE_ASC_INVALID_OPCODE, 0);
        return;
    }
    if (listLen > Srb->DataTransferLength) listLen = Srb->DataTransferLength;
    if (listLen == 0 || param == NULL) {
        IdeGood(Srb);
        return;
    }
    if (listLen < 8) {
        IdeFailWithSense(dev, Srb, SCSI_SENSE_ILLEGAL_REQUEST, IDE_ASC_PARAM_LIST_LENGTH, 0);
        return;
    }
    descLen = ((ULONG)param[2] << 8) | param[3];
    if (descLen > listLen - 8) descLen = listLen - 8;
    n = descLen / 16;
    if (n > IDE_UNMAP_MAX_DESCRIPTORS) {
        IdeFailWithSense(dev, Srb, SCSI_SENSE_ILLEGAL_REQUEST, IDE_ASC_INVALID_PARAM_FIELD, 0);
        return;
    }
    for (i = 0; i < n; i++) {
        lba = IdeLbaFromBigEndian(param + 8 + i * 16, 8);
        count = IdeBigEndian32(param + 8 + i * 16 + 8);
        if (count != 0 && (count > IDE_UNMAP_MAX_BLOCKS || !IdeLbaRangeValid(dev, lba, count))) {
            IdeFailWithSense(dev, Srb, SCSI_SENSE_ILLEGAL_REQUEST, IDE_ASC_LBA_OUT_OF_RANGE, 0);
            return;
        }
    }
    for (i = 0; i < n; i++) {
        lba = IdeLbaFromBigEndian(param + 8 + i * 16, 8);
        count = IdeBigEndian32(param + 8 + i * 16 + 8);
        if (count == 0) continue;
        result = IdeTrim(HwInit, Channel, unit, lba, count);
        if (result != IDE_CMD_OK) {
            IdeCompleteResult(HwInit, Channel, Srb, result);
            return;
        }
    }
    IdeGood(Srb);
}

/* SMART: threshold state and temperature (attribute C2h, else BEh).
   FALSE when the drive has no SMART or it failed. */
static BOOLEAN
IdeSmartHealth(IN PHW_DEVICE_EXTENSION HwInit, IN ULONG Channel, IN ULONG Unit, OUT PBOOLEAN Exceeded,
               OUT PUCHAR Temperature)
{
    PIDE_CHANNEL ch = &HwInit->Channels[Channel];
    PIDE_DEVICE dev = &ch->Devices[Unit];
    PUCHAR data = ch->BounceBuffer;
    ATA_REQUEST req;
    ULONG i;

    *Exceeded = FALSE;
    *Temperature = 0xFF;
    if (!dev->Smart) return FALSE;

    ZeroMemoryBytes(&req, sizeof(req));
    req.Unit = (UCHAR)Unit;
    req.Command = IDE_COMMAND_SMART;
    req.Features = IDE_SMART_RETURN_STATUS_FEATURE;
    req.Lba.Low = IDE_SMART_LBA_SIGNATURE;
    if (IdeRunCommand(HwInit, Channel, NULL, &req) != IDE_CMD_OK) return FALSE;
    *Exceeded = (BOOLEAN)(ch->Result.Lba1 == 0xF4 && ch->Result.Lba2 == 0x2C);

    ZeroMemoryBytes(&req, sizeof(req));
    ZeroMemoryBytes(data, IDE_BOUNCE_SIZE);
    req.Unit = (UCHAR)Unit;
    req.Command = IDE_COMMAND_SMART;
    req.Features = IDE_SMART_READ_DATA_FEATURE;
    req.SectorCount = 1;
    req.Lba.Low = IDE_SMART_LBA_SIGNATURE;
    req.UseBounce = TRUE;
    req.DataBufferLen = IDE_BOUNCE_SIZE;
    if (IdeRunCommand(HwInit, Channel, NULL, &req) == IDE_CMD_OK) {
        for (i = 0; i < 30; i++) {
            PUCHAR a = data + 2 + i * 12;
            if (a[0] == 0xC2 || (a[0] == 0xBE && *Temperature == 0xFF)) *Temperature = a[5];
        }
    }
    return TRUE;
}

/* LOG SENSE: supported pages, temperature, informational exceptions */
static VOID
IdeAtaLogSense(IN PHW_DEVICE_EXTENSION HwInit, IN ULONG Channel, IN PSCSI_REQUEST_BLOCK Srb)
{
    ULONG unit = IDE_UNIT_OF_TARGET(Srb->TargetId);
    PIDE_DEVICE dev = IDE_DEV(HwInit, Channel, unit);
    UCHAR pageCode = (UCHAR)(Srb->Cdb[2] & 0x3F), buf[20], temperature;
    ULONG allocLen = ((ULONG)Srb->Cdb[7] << 8) | Srb->Cdb[8], len;
    BOOLEAN exceeded;

    ZeroMemoryBytes(buf, sizeof(buf));
    buf[0] = pageCode;
    switch (pageCode) {
    case IDE_LOG_PAGE_SUPPORTED:
        buf[4] = IDE_LOG_PAGE_SUPPORTED;
        len = 5;
        if (dev->Smart) buf[len++] = IDE_LOG_PAGE_TEMPERATURE;
        buf[len++] = IDE_LOG_PAGE_INFO_EXCEPTIONS;
        break;
    case IDE_LOG_PAGE_TEMPERATURE:
        if (!IdeSmartHealth(HwInit, Channel, unit, &exceeded, &temperature)) {
            IdeInvalidField(dev, Srb);
            return;
        }
        /* 0000h temperature, 0001h reference temperature (not known) */
        buf[6] = 0x03; buf[7] = 2; buf[9] = temperature;
        buf[11] = 0x01; buf[12] = 0x03; buf[13] = 2; buf[15] = 0xFF;
        len = 16;
        break;
    case IDE_LOG_PAGE_INFO_EXCEPTIONS:
        IdeSmartHealth(HwInit, Channel, unit, &exceeded, &temperature);
        buf[6] = 0x03;
        buf[7] = 3;
        if (exceeded) buf[8] = IDE_ASC_FAILURE_PREDICTION;
        buf[10] = temperature;
        len = 11;
        break;
    default:
        IdeInvalidField(dev, Srb);
        return;
    }
    buf[2] = (UCHAR)((len - 4) >> 8);
    buf[3] = (UCHAR)(len - 4);
    if (len > allocLen) len = allocLen;
    IdeCopyToSrb(Srb, buf, len);
    IdeGood(Srb);
}

static VOID
IdeAtaReadWrite(
    IN PHW_DEVICE_EXTENSION HwInit,
    IN ULONG Channel,
    IN PSCSI_REQUEST_BLOCK Srb,
    IN IDE_LBA Lba,
    IN ULONG Count,
    IN BOOLEAN IsWrite,
    IN BOOLEAN Fua
)
{
    PIDE_CHANNEL ch = &HwInit->Channels[Channel];
    ULONG unit = IDE_UNIT_OF_TARGET(Srb->TargetId);
    PIDE_DEVICE dev = &ch->Devices[unit];
    ATA_REQUEST req;
    ULONG result;
    UCHAR status;
    BOOLEAN flushAfter;

    if (Count == 0) {
        IdeGood(Srb);
        return;
    }
    if (!IdeLbaRangeValid(dev, Lba, Count)) {
        IDE_DBG((IDE_PFX "Channel %lu.%lu: LBA %08lX:%08lX +%lu out of range\n", Channel, unit, Lba.High, Lba.Low, Count));
        IdeFailWithSense(dev, Srb, SCSI_SENSE_ILLEGAL_REQUEST, IDE_ASC_LBA_OUT_OF_RANGE, 0);
        return;
    }
    if (Count > (Srb->DataTransferLength / dev->SectorSize)) {
        Srb->SrbStatus = SRB_STATUS_INVALID_REQUEST;
        return;
    }
    if (!IsWrite) Fua = FALSE;

    /* TRIM mode: a write of the pattern becomes a TRIM of its sectors */
    if (IsWrite && HwInit->TrimMode && dev->Trim && dev->UseDma && HwInit->TrimAllowed &&
        IdeIsTrimPattern(HwInit, (PUCHAR)Srb->DataBuffer, Count * dev->SectorSize)) {
        IdeCompleteResult(HwInit, Channel, Srb, IdeTrim(HwInit, Channel, unit, Lba, Count));
        return;
    }

    /* flushAfter: FUA wanted and not done by the command itself */
    flushAfter = Fua;
    if (!IdeBuildReadWrite(dev, unit, Srb->DataBuffer, Lba, Count, IsWrite, TRUE, &flushAfter, &req)) {
        IdeInvalidField(dev, Srb);
        return;
    }

    if (dev->MaxSectors != 0 && Count > dev->MaxSectors) {
        /* A drive with a per-command limit (Silicon Image 3112 errata):
           the request in pieces, one after the other */
        PUCHAR data = (PUCHAR)Srb->DataBuffer;
        ULONG chunk;
        BOOLEAN noFua;

        result = IDE_CMD_OK;
        while (Count > 0 && result == IDE_CMD_OK) {
            chunk = (Count > dev->MaxSectors) ? dev->MaxSectors : Count;
            noFua = FALSE;
            result = IdeRunReadWrite(HwInit, Channel, Srb, dev, unit, data, Lba, chunk, IsWrite, &noFua);
            IdeLbaAdd(&Lba, chunk);
            Count -= chunk;
            data += chunk * dev->SectorSize;
        }
    } else {
        /* Interrupt-completed DMA: a plain read or write (FUA needs the
           flush afterwards, so it stays polled) */
        if (req.UseDma && ch->Async && !flushAfter && !HwInit->DumpMode) {
            status = IdeIssueDma(HwInit, Channel, Srb, &req);
            if (status == SRB_STATUS_PENDING) return;
            if (status != SRB_STATUS_INVALID_REQUEST) {
                if (status == SRB_STATUS_ERROR) {
                    IdeSatlCompleteAtaError(HwInit, Channel, Srb);
                } else {
                    Srb->SrbStatus = status;
                }
                return;
            }
            /* No PRD table for this buffer: polled, with PIO */
        }
        result = IdeRunReadWrite(HwInit, Channel, Srb, dev, unit, Srb->DataBuffer, Lba, Count, IsWrite, &Fua);
    }
    if (result == IDE_CMD_OK && Fua && !IdeFlushDevice(HwInit, Channel, unit)) {
        result = IDE_CMD_DEVICE_ERROR;
    }
    IdeCompleteResult(HwInit, Channel, Srb, result);
}

static VOID
IdeAtaVerify(IN PHW_DEVICE_EXTENSION HwInit, IN ULONG Channel, IN PSCSI_REQUEST_BLOCK Srb,
             IN IDE_LBA Lba, IN ULONG Count)
{
    ULONG unit = IDE_UNIT_OF_TARGET(Srb->TargetId);
    PIDE_DEVICE dev = IDE_DEV(HwInit, Channel, unit);
    ATA_REQUEST req;
    ULONG chunk, result = IDE_CMD_OK;

    if (Srb->Cdb[1] & 0x02) {
        IdeInvalidField(dev, Srb);
        return;
    }
    if (Count == 0) {
        IdeGood(Srb);
        return;
    }
    if (!IdeLbaRangeValid(dev, Lba, Count)) {
        IdeFailWithSense(dev, Srb, SCSI_SENSE_ILLEGAL_REQUEST, IDE_ASC_LBA_OUT_OF_RANGE, 0);
        return;
    }

    while (Count > 0 && result == IDE_CMD_OK) {
        ZeroMemoryBytes(&req, sizeof(req));
        req.Unit = (UCHAR)unit;
        if (!dev->LbaSupported) {
            chunk = (Count > ATA_LBA28_MAX_SECTORS) ? ATA_LBA28_MAX_SECTORS : Count;
            if (!IdeChsFromLba(dev, Lba, &req.Lba)) {
                result = IDE_CMD_BAD_BUFFER;
                break;
            }
            req.Chs = TRUE;
            req.Command = IDE_COMMAND_READ_VERIFY;
        } else if (dev->Lba48) {
            chunk = (Count > ATA_LBA48_MAX_SECTORS) ? ATA_LBA48_MAX_SECTORS : Count;
            req.Lba = Lba;
            req.Is48Bit = TRUE;
            req.Command = IDE_COMMAND_READ_VERIFY_EXT;
        } else {
            chunk = (Count > ATA_LBA28_MAX_SECTORS) ? ATA_LBA28_MAX_SECTORS : Count;
            req.Lba = Lba;
            req.Command = IDE_COMMAND_READ_VERIFY;
        }
        req.SectorCount = chunk;
        result = IdeRunCommand(HwInit, Channel, Srb, &req);
        IdeLbaAdd(&Lba, chunk);
        Count -= chunk;
    }
    IdeCompleteResult(HwInit, Channel, Srb, result);
}

static VOID
IdeAtaStartStop(IN PHW_DEVICE_EXTENSION HwInit, IN ULONG Channel, IN PSCSI_REQUEST_BLOCK Srb)
{
    ULONG unit = IDE_UNIT_OF_TARGET(Srb->TargetId);
    UCHAR pc = (UCHAR)(Srb->Cdb[4] >> 4);
    BOOLEAN start = (BOOLEAN)((Srb->Cdb[4] & 0x01) != 0);
    UCHAR command = 0;
    ULONG result = IDE_CMD_OK;

    switch (pc) {
    case 0x0:
        if (start) {
            command = IDE_COMMAND_IDLE_IMMEDIATE;
        } else {
            IdeFlushDevice(HwInit, Channel, unit);
            command = IDE_COMMAND_STANDBY_IMMEDIATE;
        }
        break;
    case 0x1:
    case 0x2:
    case 0xA:
        command = IDE_COMMAND_IDLE_IMMEDIATE;
        break;
    case 0x3:
    case 0xB:
        IdeFlushDevice(HwInit, Channel, unit);
        command = IDE_COMMAND_STANDBY_IMMEDIATE;
        break;
    default:
        break;
    }

    if (command != 0) {
        result = IdeRunSimple(HwInit, Channel, unit, command, 0, 0, IDE_POLL_TIMEOUT_POWER_SEC);
    }
    IdeCompleteResult(HwInit, Channel, Srb, result);
}

/* ------------------------------------------------------------------ */
/* SAT ATA PASS-THROUGH (12) / (16)                                    */
/* ------------------------------------------------------------------ */

#define SAT_PROT_NON_DATA       3
#define SAT_PROT_PIO_IN         4
#define SAT_PROT_PIO_OUT        5
#define SAT_PROT_DMA            6
#define SAT_PROT_UDMA_IN        10
#define SAT_PROT_UDMA_OUT       11
#define SAT_PROT_RETURN_INFO    15

static VOID
IdePassThroughSense(IN PIDE_DEVICE Dev, IN PSCSI_REQUEST_BLOCK Srb, IN PIDE_ATA_RESULT R,
                    IN BOOLEAN Extend, IN BOOLEAN Error)
{
    UCHAR s[22];

    ZeroMemoryBytes(s, sizeof(s));
    s[0] = 0x72;
    s[1] = Error ? SCSI_SENSE_ABORTED_COMMAND : SCSI_SENSE_RECOVERED_ERROR;
    s[2] = 0x00;
    s[3] = IDE_ASCQ_ATA_INFO_AVAILABLE;
    s[7] = 14;
    s[8] = 0x09;
    s[9] = 0x0C;
    s[10] = (UCHAR)(Extend ? 1 : 0);
    s[11] = R->Error;
    s[12] = R->CountHigh;
    s[13] = R->CountLow;
    s[14] = R->Lba3;
    s[15] = R->Lba0;
    s[16] = R->Lba4;
    s[17] = R->Lba1;
    s[18] = R->Lba5;
    s[19] = R->Lba2;
    s[20] = R->Device;
    s[21] = R->Status;
    IdeFailWithRawSense(Dev, Srb, s, sizeof(s));
}

static VOID
IdeAtaPassThrough(IN PHW_DEVICE_EXTENSION HwInit, IN ULONG Channel, IN PSCSI_REQUEST_BLOCK Srb)
{
    PIDE_CHANNEL ch = &HwInit->Channels[Channel];
    ULONG unit = IDE_UNIT_OF_TARGET(Srb->TargetId);
    PIDE_DEVICE dev = &ch->Devices[unit];
    PUCHAR cdb = Srb->Cdb;
    BOOLEAN sixteen = (BOOLEAN)(cdb[0] == IDE_SCSIOP_ATA_PASSTHROUGH16);
    UCHAR protocol = (UCHAR)((cdb[1] >> 1) & 0x0F);
    BOOLEAN extend = (BOOLEAN)(sixteen && (cdb[1] & 0x01));
    BOOLEAN ckCond = (BOOLEAN)((cdb[2] & 0x20) != 0);
    BOOLEAN toDevice = (BOOLEAN)((cdb[2] & 0x08) == 0);
    ATA_REQUEST req;
    ULONG result;

    if (protocol == SAT_PROT_RETURN_INFO) {
        IdePassThroughSense(dev, Srb, &ch->Result, extend, FALSE);
        return;
    }
    if (protocol != SAT_PROT_NON_DATA && protocol != SAT_PROT_PIO_IN &&
        protocol != SAT_PROT_PIO_OUT && protocol != SAT_PROT_DMA &&
        protocol != SAT_PROT_UDMA_IN && protocol != SAT_PROT_UDMA_OUT) {
        IdeInvalidField(dev, Srb);
        return;
    }

    ZeroMemoryBytes(&req, sizeof(req));
    req.Unit = (UCHAR)unit;
    req.RawDevice = TRUE;
    req.Is48Bit = extend;
    if (sixteen) {
        req.Features = (USHORT)(cdb[4] | (extend ? ((USHORT)cdb[3] << 8) : 0));
        req.SectorCount = cdb[6] | (extend ? ((ULONG)cdb[5] << 8) : 0);
        req.Lba.Low = (ULONG)cdb[8] | ((ULONG)cdb[10] << 8) | ((ULONG)cdb[12] << 16);
        if (extend) {
            req.Lba.Low |= (ULONG)cdb[7] << 24;
            req.Lba.High = (ULONG)cdb[9] | ((ULONG)cdb[11] << 8);
        }
        req.Device = cdb[13];
        req.Command = cdb[14];
    } else {
        req.Features = cdb[3];
        req.SectorCount = cdb[4];
        req.Lba.Low = (ULONG)cdb[5] | ((ULONG)cdb[6] << 8) | ((ULONG)cdb[7] << 16);
        req.Device = cdb[8];
        req.Command = cdb[9];
    }

    if (protocol != SAT_PROT_NON_DATA) {
        if (Srb->DataBuffer == NULL || Srb->DataTransferLength == 0) {
            IdeInvalidField(dev, Srb);
            return;
        }
        req.DataBuffer = Srb->DataBuffer;
        req.DataBufferLen = Srb->DataTransferLength;
        req.IsWrite = (BOOLEAN)(protocol == SAT_PROT_PIO_OUT || protocol == SAT_PROT_UDMA_OUT ||
                                (protocol == SAT_PROT_DMA && toDevice));
        req.UseDma = (BOOLEAN)(dev->UseDma && protocol != SAT_PROT_PIO_IN && protocol != SAT_PROT_PIO_OUT);
    }

    result = IdeRunCommand(HwInit, Channel, Srb, &req);
    if (result == IDE_CMD_BAD_BUFFER) {
        Srb->SrbStatus = SRB_STATUS_INVALID_REQUEST;
    } else if (result != IDE_CMD_OK || ckCond) {
        IdePassThroughSense(dev, Srb, &ch->Result, extend, (BOOLEAN)(result != IDE_CMD_OK));
    } else {
        IdeGood(Srb);
    }
}

static VOID
IdeProcessAtaSrb(IN PHW_DEVICE_EXTENSION HwInit, IN ULONG Channel, IN PSCSI_REQUEST_BLOCK Srb)
{
    ULONG unit = IDE_UNIT_OF_TARGET(Srb->TargetId);
    PIDE_DEVICE dev = IDE_DEV(HwInit, Channel, unit);
    PUCHAR cdb = Srb->Cdb;
    IDE_LBA lba;
    ULONG count;
    BOOLEAN fua = (BOOLEAN)((cdb[1] & 0x08) != 0);

    switch (cdb[0]) {
    case SCSIOP_INQUIRY:
        IdeHandleInquiry(HwInit, dev, Srb);
        return;

    case SCSIOP_REQUEST_SENSE:
        IdeHandleRequestSenseAta(dev, Srb);
        return;

    case IDE_SCSIOP_REPORT_LUNS:
        IdeHandleReportLuns(Srb);
        return;

    case SCSIOP_TEST_UNIT_READY:
    case SCSIOP_MEDIUM_REMOVAL:
        IdeGood(Srb);
        return;

    case SCSIOP_SYNCHRONIZE_CACHE:
    case IDE_SCSIOP_SYNC_CACHE16:
        IdeCompleteResult(HwInit, Channel, Srb,
                          IdeFlushDevice(HwInit, Channel, unit) ? IDE_CMD_OK : IDE_CMD_DEVICE_ERROR);
        return;

    case IDE_SCSIOP_START_STOP_UNIT:
        IdeAtaStartStop(HwInit, Channel, Srb);
        return;

    case SCSIOP_READ_CAPACITY:
        IdeHandleReadCapacity10(HwInit, dev, Srb);
        return;

    case IDE_SCSIOP_SERVICE_ACTION_IN16:
        if ((cdb[1] & 0x1F) == IDE_SA_READ_CAPACITY16) {
            IdeHandleReadCapacity16(HwInit, dev, Srb);
            return;
        }
        break;

    case SCSIOP_MODE_SENSE:
    case SCSIOP_MODE_SENSE10:
        IdeHandleModeSenseAta(dev, Srb);
        return;

    case IDE_SCSIOP_MODE_SELECT6:
    case IDE_SCSIOP_MODE_SELECT10:
        IdeHandleModeSelectAta(HwInit, Channel, unit, Srb);
        return;

    case IDE_SCSIOP_ATA_PASSTHROUGH12:
    case IDE_SCSIOP_ATA_PASSTHROUGH16:
        IdeAtaPassThrough(HwInit, Channel, Srb);
        return;

    case SCSIOP_READ6:
    case SCSIOP_WRITE6:
        lba.High = 0;
        lba.Low = ((ULONG)(cdb[1] & 0x1F) << 16) | ((ULONG)cdb[2] << 8) | cdb[3];
        count = cdb[4] ? cdb[4] : 256;
        IdeAtaReadWrite(HwInit, Channel, Srb, lba, count, (BOOLEAN)(cdb[0] == SCSIOP_WRITE6), FALSE);
        return;

    case SCSIOP_READ:
    case SCSIOP_WRITE:
        lba = IdeLbaFromBigEndian(&cdb[2], 4);
        count = ((ULONG)cdb[7] << 8) | cdb[8];
        IdeAtaReadWrite(HwInit, Channel, Srb, lba, count, (BOOLEAN)(cdb[0] == SCSIOP_WRITE), fua);
        return;

    case IDE_SCSIOP_READ12:
    case IDE_SCSIOP_WRITE12:
        lba = IdeLbaFromBigEndian(&cdb[2], 4);
        count = IdeBigEndian32(&cdb[6]);
        IdeAtaReadWrite(HwInit, Channel, Srb, lba, count, (BOOLEAN)(cdb[0] == IDE_SCSIOP_WRITE12), fua);
        return;

    case IDE_SCSIOP_READ16:
    case IDE_SCSIOP_WRITE16:
        lba = IdeLbaFromBigEndian(&cdb[2], 8);
        count = IdeBigEndian32(&cdb[10]);
        IdeAtaReadWrite(HwInit, Channel, Srb, lba, count, (BOOLEAN)(cdb[0] == IDE_SCSIOP_WRITE16), fua);
        return;

    case SCSIOP_VERIFY:
        IdeAtaVerify(HwInit, Channel, Srb, IdeLbaFromBigEndian(&cdb[2], 4), ((ULONG)cdb[7] << 8) | cdb[8]);
        return;

    case IDE_SCSIOP_VERIFY12:
        IdeAtaVerify(HwInit, Channel, Srb, IdeLbaFromBigEndian(&cdb[2], 4), IdeBigEndian32(&cdb[6]));
        return;

    case IDE_SCSIOP_VERIFY16:
        IdeAtaVerify(HwInit, Channel, Srb, IdeLbaFromBigEndian(&cdb[2], 8), IdeBigEndian32(&cdb[10]));
        return;

    case IDE_SCSIOP_UNMAP:
        IdeAtaUnmap(HwInit, Channel, Srb);
        return;

    case IDE_SCSIOP_LOG_SENSE:
        IdeAtaLogSense(HwInit, Channel, Srb);
        return;
    }

    IdeFailWithSense(dev, Srb, SCSI_SENSE_ILLEGAL_REQUEST, IDE_ASC_INVALID_OPCODE, 0);
}

/* ------------------------------------------------------------------ */
/* ATAPI                                                               */
/* ------------------------------------------------------------------ */

/* ATAPI command with data in the channel's bounce buffer (PIO) */
static ULONG
IdeAtapiBounceCommand(IN PHW_DEVICE_EXTENSION HwInit, IN ULONG Channel, IN ULONG Unit, IN PUCHAR Cdb, IN ULONG DataLength)
{
    ATA_REQUEST req;

    ZeroMemoryBytes(&req, sizeof(req));
    req.Unit = (UCHAR)Unit;
    req.IsAtapi = TRUE;
    req.UseBounce = TRUE;
    req.DataBufferLen = DataLength;
    req.Cdb = Cdb;
    req.CdbLength = 12;

    ZeroMemoryBytes(HwInit->Channels[Channel].BounceBuffer, IDE_BOUNCE_SIZE);
    return IdeRunCommand(HwInit, Channel, NULL, &req);
}

static ULONG
IdeAtapiFetchSense(IN PHW_DEVICE_EXTENSION HwInit, IN ULONG Channel, IN ULONG Unit, IN ULONG AllocLen)
{
    UCHAR cdb[12];

    if (AllocLen == 0 || AllocLen > 252) AllocLen = sizeof(SENSE_DATA);
    AllocLen = (AllocLen + 1) & ~1UL;

    ZeroMemoryBytes(cdb, sizeof(cdb));
    cdb[0] = SCSIOP_REQUEST_SENSE;
    cdb[4] = (UCHAR)AllocLen;

    if (IdeAtapiBounceCommand(HwInit, Channel, Unit, cdb, AllocLen) != IDE_CMD_OK) return 0;
    return AllocLen;
}

static VOID
IdeAtapiFailed(IN PHW_DEVICE_EXTENSION HwInit, IN ULONG Channel, IN PSCSI_REQUEST_BLOCK Srb)
{
    ULONG unit = IDE_UNIT_OF_TARGET(Srb->TargetId);

    Srb->SrbStatus = SRB_STATUS_ERROR;
    Srb->ScsiStatus = SCSISTAT_CHECK_CONDITION;

#if IDE_AUTOSENSE
    if (Srb->SenseInfoBuffer != NULL && Srb->SenseInfoBufferLength != 0 &&
        !(Srb->SrbFlags & SRB_FLAGS_DISABLE_AUTOSENSE)) {
        PIDE_DEVICE dev = IDE_DEV(HwInit, Channel, unit);
        UCHAR status = dev->LastStatus, error = dev->LastError;
        ULONG n;

        n = IdeAtapiFetchSense(HwInit, Channel, unit, sizeof(SENSE_DATA));
        if (n != 0) {
            IdeFailWithRawSense(dev, Srb, HwInit->Channels[Channel].BounceBuffer, n);
        } else if (status & IDE_STS_ERR) {
            IdeFailWithSense(dev, Srb, (UCHAR)ATAPI_ERROR_SENSE_KEY(error), 0, 0);
        }
    }
#else
    (VOID)HwInit;
    (VOID)Channel;
    (VOID)unit;
#endif
}

static VOID
IdeAtapiRequestSense(IN PHW_DEVICE_EXTENSION HwInit, IN ULONG Channel, IN PSCSI_REQUEST_BLOCK Srb)
{
    ULONG unit = IDE_UNIT_OF_TARGET(Srb->TargetId);
    PIDE_DEVICE dev = IDE_DEV(HwInit, Channel, unit);
    ULONG allocLen = Srb->Cdb[4];
    ULONG n;
    SENSE_DATA empty;

    if (dev->SensePending) {
        IdeHandleRequestSenseAta(dev, Srb);
        return;
    }
    if (allocLen > Srb->DataTransferLength) allocLen = Srb->DataTransferLength;

    n = IdeAtapiFetchSense(HwInit, Channel, unit, allocLen);
    if (n != 0) {
        IdeCopyToSrb(Srb, HwInit->Channels[Channel].BounceBuffer, n);
    } else {
        IdeBuildSense(&empty, SCSI_SENSE_NO_SENSE, 0, 0);
        IdeCopyToSrb(Srb, &empty, sizeof(empty));
    }
    IdeGood(Srb);
}

/* ATAPI has no MODE SENSE(6): send (10), convert the header like atapi.sys */
static VOID
IdeAtapiModeSense6(IN PHW_DEVICE_EXTENSION HwInit, IN ULONG Channel, IN PSCSI_REQUEST_BLOCK Srb)
{
    ULONG unit = IDE_UNIT_OF_TARGET(Srb->TargetId);
    PUCHAR bounce = HwInit->Channels[Channel].BounceBuffer;
    UCHAR cdb[12];
    UCHAR hdr6[4];
    ULONG xferLen, total, blockDesc, i;

    /* The same request with 4 more bytes for the longer header; DBD, page
       control and page code as given */
    xferLen = (ULONG)Srb->Cdb[4] + 4;
    if (xferLen < 8) xferLen = 8;               /* at least the whole header */
    if (xferLen > IDE_BOUNCE_SIZE) xferLen = IDE_BOUNCE_SIZE;
    xferLen = (xferLen + 1) & ~1UL;             /* PRD byte counts are even */

    ZeroMemoryBytes(cdb, sizeof(cdb));
    cdb[0] = SCSIOP_MODE_SENSE10;
    cdb[1] = (UCHAR)(Srb->Cdb[1] & 0x08);
    cdb[2] = Srb->Cdb[2];
    cdb[7] = (UCHAR)(xferLen >> 8);
    cdb[8] = (UCHAR)xferLen;

    if (IdeAtapiBounceCommand(HwInit, Channel, unit, cdb, xferLen) != IDE_CMD_OK) {
        IdeAtapiFailed(HwInit, Channel, Srb);
        return;
    }

    /* Header (10) to header (6): data length less 3, medium type, device
       specific parameter (write protect of a removable disk), block
       descriptor length; the descriptors and pages follow unchanged */
    total = (((ULONG)bounce[0] << 8) | bounce[1]) + 2;     /* all the drive has */
    if (total < 8) total = 8;
    blockDesc = ((ULONG)bounce[6] << 8) | bounce[7];
    hdr6[0] = (UCHAR)((total - 5 > 255) ? 255 : total - 5);
    if (total > xferLen) total = xferLen;                   /* what came back */
    hdr6[1] = bounce[2];
    hdr6[2] = bounce[3];
    hdr6[3] = (UCHAR)((blockDesc > 255) ? 255 : blockDesc);
    for (i = 8; i < total; i++) bounce[i - 4] = bounce[i];
    for (i = 0; i < 4; i++) bounce[i] = hdr6[i];

    IdeCopyToSrb(Srb, bounce, total - 4);
    IdeGood(Srb);
}

/* Packet commands moving sectors: those use DMA when the drive has it */
static BOOLEAN
IdeAtapiBulkCommand(IN UCHAR Opcode)
{
    return (BOOLEAN)(Opcode == SCSIOP_READ || Opcode == SCSIOP_WRITE ||
                     Opcode == IDE_SCSIOP_READ12 || Opcode == IDE_SCSIOP_WRITE12 ||
                     Opcode == IDE_SCSIOP_READ_CD || Opcode == IDE_SCSIOP_WRITE_VERIFY);
}

static VOID
IdeProcessAtapiSrb(IN PHW_DEVICE_EXTENSION HwInit, IN ULONG Channel, IN PSCSI_REQUEST_BLOCK Srb)
{
    PIDE_CHANNEL ch = &HwInit->Channels[Channel];
    ULONG unit = IDE_UNIT_OF_TARGET(Srb->TargetId);
    PIDE_DEVICE dev = &ch->Devices[unit];
    ATA_REQUEST req;
    ULONG result;
    UCHAR status;

    /* ATAPI has no READ(6)/WRITE(6) (MMC, SFF-8070i): the same transfer as
       READ(10)/WRITE(10), for a class driver of a removable disk that
       sends the short form */
    if (Srb->Cdb[0] == SCSIOP_READ6 || Srb->Cdb[0] == SCSIOP_WRITE6) {
        PUCHAR c = Srb->Cdb;
        ULONG lba = ((ULONG)(c[1] & 0x1F) << 16) | ((ULONG)c[2] << 8) | c[3];
        ULONG count = (c[4] != 0) ? c[4] : 256;
        UCHAR op = (UCHAR)((c[0] == SCSIOP_READ6) ? SCSIOP_READ : SCSIOP_WRITE);

        ZeroMemoryBytes(c, 10);
        c[0] = op;
        c[2] = (UCHAR)(lba >> 24);
        c[3] = (UCHAR)(lba >> 16);
        c[4] = (UCHAR)(lba >> 8);
        c[5] = (UCHAR)lba;
        c[7] = (UCHAR)(count >> 8);
        c[8] = (UCHAR)count;
        Srb->CdbLength = 10;
    }

    switch (Srb->Cdb[0]) {
    case SCSIOP_INQUIRY:
        IdeHandleInquiry(HwInit, dev, Srb);
        return;

    case SCSIOP_REQUEST_SENSE:
        IdeAtapiRequestSense(HwInit, Channel, Srb);
        return;

    case IDE_SCSIOP_REPORT_LUNS:
        IdeHandleReportLuns(Srb);
        return;

    case SCSIOP_SYNCHRONIZE_CACHE:
        IdeGood(Srb);
        return;

    case SCSIOP_MODE_SENSE:
        IdeAtapiModeSense6(HwInit, Channel, Srb);
        return;
    }

    ZeroMemoryBytes(&req, sizeof(req));
    req.Unit = (UCHAR)unit;
    req.IsAtapi = TRUE;
    req.DataBuffer = Srb->DataBuffer;
    req.DataBufferLen = (Srb->DataBuffer != NULL) ? Srb->DataTransferLength : 0;
    req.IsWrite = (BOOLEAN)((Srb->SrbFlags & SRB_FLAGS_DATA_OUT) != 0);
    req.Cdb = Srb->Cdb;
    req.CdbLength = Srb->CdbLength;
    req.UseDma = (BOOLEAN)(dev->UseDma && IdeAtapiBulkCommand(Srb->Cdb[0]) &&
                           req.DataBufferLen != 0 && (req.DataBufferLen & 1) == 0);

    if (req.UseDma && ch->Async && !HwInit->DumpMode) {
        status = IdeIssueDma(HwInit, Channel, Srb, &req);
        if (status == SRB_STATUS_PENDING) return;
        if (status != SRB_STATUS_INVALID_REQUEST) {
            if (status == SRB_STATUS_ERROR) {
                IdeAtapiFailed(HwInit, Channel, Srb);
            } else {
                Srb->SrbStatus = status;
            }
            return;
        }
        req.UseDma = FALSE;                     /* no PRD table for this buffer */
    }

    result = IdeRunCommand(HwInit, Channel, Srb, &req);
    if (result == IDE_CMD_BAD_BUFFER && req.UseDma) {
        req.UseDma = FALSE;
        result = IdeRunCommand(HwInit, Channel, Srb, &req);
    }
    if (result == IDE_CMD_OK) {
        IdeGood(Srb);
    } else if (result == IDE_CMD_BAD_BUFFER) {
        Srb->SrbStatus = SRB_STATUS_INVALID_REQUEST;
    } else {
        IdeAtapiFailed(HwInit, Channel, Srb);
    }
}

/* ------------------------------------------------------------------ */
/* Entry                                                               */
/* ------------------------------------------------------------------ */

VOID
IdeSatlProcessSrb(IN PHW_DEVICE_EXTENSION HwInit, IN PSCSI_REQUEST_BLOCK Srb)
{
    ULONG channel = Srb->TargetId / IDE_DEVICES_PER_CHANNEL;
    ULONG unit = IDE_UNIT_OF_TARGET(Srb->TargetId);
    PIDE_CHANNEL ch;
    PIDE_DEVICE dev;

    if (Srb->PathId != 0 || channel >= HwInit->ChannelCount || !HwInit->Channels[channel].Present) {
        Srb->SrbStatus = SRB_STATUS_SELECTION_TIMEOUT;
        return;
    }
    ch = &HwInit->Channels[channel];
    dev = &ch->Devices[unit];

    if (!dev->Present) {
        Srb->SrbStatus = SRB_STATUS_SELECTION_TIMEOUT;
        return;
    }
    if (Srb->Lun != 0) {
        Srb->SrbStatus = SRB_STATUS_NO_DEVICE;
        return;
    }

    /* After a reset: transfer modes and settings first */
    if (ch->NeedsSetup && ch->Outstanding == NULL) {
        IDE_DBG((IDE_PFX "Channel %lu: setting up again after reset\n", channel));
        IdeSetupChannel(HwInit, channel);
    }

    if (Srb->Cdb[0] != SCSIOP_REQUEST_SENSE) {
        dev->SensePending = FALSE;
    }

    if (dev->IsAtapi) {
        IdeProcessAtapiSrb(HwInit, channel, Srb);
    } else {
        IdeProcessAtaSrb(HwInit, channel, Srb);
    }
}

/* A DMA command the interrupt (or the watchdog) finished */
VOID
IdeSatlCompleteDma(IN PHW_DEVICE_EXTENSION HwInit, IN ULONG Channel, IN PSCSI_REQUEST_BLOCK Srb, IN ULONG Result)
{
    PIDE_DEVICE dev = IDE_DEV(HwInit, Channel, IDE_UNIT_OF_TARGET(Srb->TargetId));

    if (Result == IDE_CMD_OK) {
        IdeGood(Srb);
    } else if (dev->IsAtapi) {
        IdeAtapiFailed(HwInit, Channel, Srb);
    } else {
        IdeSatlCompleteAtaError(HwInit, Channel, Srb);
    }
    ScsiPortNotification(RequestComplete, HwInit, Srb);
}

/* The request that waited for the channel; ScsiPort was told to go on
   when it was queued */
BOOLEAN
IdeSatlStartPending(IN PHW_DEVICE_EXTENSION HwInit, IN ULONG Channel, IN PSCSI_REQUEST_BLOCK Srb)
{
    (VOID)Channel;
    Srb->SrbStatus = SRB_STATUS_PENDING;
    IdeSatlProcessSrb(HwInit, Srb);
    if (Srb->SrbStatus != SRB_STATUS_PENDING) {
        ScsiPortNotification(RequestComplete, HwInit, Srb);
        return FALSE;
    }
    return TRUE;
}

/* ------------------------------------------------------------------ */
/* Miniport IOCTLs: SMART ("SCSIDISK") and power control ("IDENT")     */
/* ------------------------------------------------------------------ */

static BOOLEAN
IdeSignatureIs(IN PIDE_SRB_IO_CONTROL Ctl, IN PCHAR Signature)
{
    ULONG i;
    for (i = 0; i < 8; i++) {
        if (Ctl->Signature[i] != (UCHAR)Signature[i]) return FALSE;
    }
    return TRUE;
}

/* Disk for a SMART request: bDriveNumber is the target (channel * 2 +
   unit), as atapi.sys numbers them */
static PIDE_DEVICE
IdeSmartDevice(IN PHW_DEVICE_EXTENSION HwInit, IN PSCSI_REQUEST_BLOCK Srb, IN UCHAR Drive, OUT PULONG Target)
{
    ULONG target = Drive;
    PIDE_DEVICE dev;

    if (target >= HwInit->ChannelCount * IDE_DEVICES_PER_CHANNEL ||
        !IDE_DEVICE_OF_TARGET(HwInit, target)->Present) {
        target = Srb->TargetId;
    }
    if (target >= HwInit->ChannelCount * IDE_DEVICES_PER_CHANNEL) return NULL;
    dev = IDE_DEVICE_OF_TARGET(HwInit, target);
    if (!dev->Present) return NULL;
    *Target = target;
    return dev;
}

static ULONG
IdeSmartCommand(IN PHW_DEVICE_EXTENSION HwInit, IN PSCSI_REQUEST_BLOCK Srb, IN PIDE_SRB_IO_CONTROL Ctl, IN ULONG Available)
{
    PIDE_SENDCMD_IN in = (PIDE_SENDCMD_IN)((PUCHAR)Ctl + Ctl->HeaderLength);
    PIDE_SENDCMD_OUT out = (PIDE_SENDCMD_OUT)in;
    IDE_IDEREGS regs;
    ATA_REQUEST req;
    PIDE_DEVICE dev;
    PIDE_CHANNEL ch;
    ULONG target, channel, dataLen = 0, result;
    BOOLEAN dataIn = FALSE, dataOut = FALSE;
    UCHAR drive;

    if (Available < IDE_SENDCMD_IN_HEADER) return IDE_SMART_INVALID_BUFFER;
    regs = in->Regs;
    drive = in->DriveNumber;

    dev = IdeSmartDevice(HwInit, Srb, drive, &target);
    if (dev == NULL) return IDE_SMART_INVALID_DRIVE;
    channel = target / IDE_DEVICES_PER_CHANNEL;
    ch = &HwInit->Channels[channel];

    switch (Ctl->ControlCode) {
    case IDE_SMART_IDENTIFY:
        regs.Command = dev->IsAtapi ? IDE_COMMAND_IDENTIFY_PACKET : IDE_COMMAND_IDENTIFY_DEVICE;
        dataIn = TRUE;
        dataLen = 512;
        break;
    case IDE_SMART_READ_ATTRIBS:
    case IDE_SMART_READ_THRESHOLDS:
        dataIn = TRUE;
        dataLen = 512;
        break;
    case IDE_SMART_READ_LOG:
        dataIn = TRUE;
        dataLen = (ULONG)(regs.SectorCount ? regs.SectorCount : 1) * 512;
        break;
    case IDE_SMART_WRITE_LOG:
        dataOut = TRUE;
        dataLen = (ULONG)(regs.SectorCount ? regs.SectorCount : 1) * 512;
        break;
    default:
        break;
    }
    if (dev->IsAtapi && Ctl->ControlCode != IDE_SMART_IDENTIFY) return IDE_SMART_INVALID_DRIVE;

    if ((dataIn && Available < IDE_SENDCMD_OUT_HEADER + dataLen) ||
        (dataOut && Available < IDE_SENDCMD_IN_HEADER + dataLen)) {
        return IDE_SMART_INVALID_BUFFER;
    }

    ZeroMemoryBytes(&req, sizeof(req));
    req.Unit = (UCHAR)IDE_UNIT_OF_TARGET(target);
    req.Command = regs.Command;
    req.Features = regs.Features;
    req.SectorCount = regs.SectorCount;
    req.Lba.Low = (ULONG)regs.SectorNumber | ((ULONG)regs.CylLow << 8) | ((ULONG)regs.CylHigh << 16);
    req.RawDevice = TRUE;
    req.Device = (UCHAR)(regs.DriveHead & 0xE0);
    if (dataIn || dataOut) {
        req.DataBuffer = dataIn ? out->Buffer : in->Buffer;
        req.DataBufferLen = dataLen;
        req.IsWrite = dataOut;
    }

    result = IdeRunCommand(HwInit, channel, Srb, &req);

    ZeroMemoryBytes(out, IDE_SENDCMD_OUT_HEADER);
    if (result != IDE_CMD_OK) {
        out->DriverError = IDE_SMART_DRV_ERROR;
        out->IdeError = dev->LastError;
        return 0;
    }

    if (Ctl->ControlCode == IDE_SMART_RETURN_STATUS) {
        PIDE_IDEREGS r = (PIDE_IDEREGS)out->Buffer;
        if (Available < IDE_SENDCMD_OUT_HEADER + sizeof(IDE_IDEREGS)) return IDE_SMART_INVALID_BUFFER;
        r->Features = ch->Result.Error;
        r->SectorCount = ch->Result.CountLow;
        r->SectorNumber = ch->Result.Lba0;
        r->CylLow = ch->Result.Lba1;
        r->CylHigh = ch->Result.Lba2;
        r->DriveHead = ch->Result.Device;
        r->Command = ch->Result.Status;
        r->Reserved = 0;
        out->BufferSize = sizeof(IDE_IDEREGS);
    } else if (dataIn) {
        out->BufferSize = dataLen;
    }
    return 0;
}

static VOID
IdeSmartVersion(IN PHW_DEVICE_EXTENSION HwInit, IN PIDE_SRB_IO_CONTROL Ctl, IN ULONG Available)
{
    PIDE_GETVERSION v = (PIDE_GETVERSION)((PUCHAR)Ctl + Ctl->HeaderLength);
    ULONG target;

    if (Available < sizeof(IDE_GETVERSION)) {
        Ctl->ReturnCode = IDE_SMART_INVALID_BUFFER;
        return;
    }
    ZeroMemoryBytes(v, sizeof(IDE_GETVERSION));
    v->Version = 1;
    v->Revision = 1;
    v->Capabilities = IDE_SMART_CAP_ATA_ID | IDE_SMART_CAP_ATAPI_ID | IDE_SMART_CAP_SMART;
    /* Device map as atapi.sys reports it: bit n ATA, bit n+4 ATAPI */
    for (target = 0; target < 4 && target < HwInit->ChannelCount * IDE_DEVICES_PER_CHANNEL; target++) {
        PIDE_DEVICE dev = IDE_DEVICE_OF_TARGET(HwInit, target);
        if (!dev->Present) continue;
        v->IdeDeviceMap |= (UCHAR)(dev->IsAtapi ? (0x10 << target) : (0x01 << target));
    }
    Ctl->ReturnCode = 0;
}

static VOID
IdeFillInfo(IN PHW_DEVICE_EXTENSION HwInit, OUT PIDE_IOCTL_INFO Info)
{
    ULONG c, unit;

    ZeroMemoryBytes(Info, sizeof(IDE_IOCTL_INFO));
    Info->Version = IDE_DRIVER_VERSION;
    Info->Flags = (HwInit->AsyncMode ? IDE_INFO_ASYNC : 0) |
                  (HwInit->Stopped ? IDE_INFO_POWERED_DOWN : 0) | (HwInit->TrimMode ? IDE_INFO_TRIM_MODE : 0);
    Info->ChannelCount = HwInit->ChannelCount;
    Info->PciVendorDevice = HwInit->HasPci ? (((ULONG)HwInit->DeviceId << 16) | HwInit->VendorId) : 0;
    Info->InterruptCount = HwInit->InterruptCount;
    for (c = 0; c < IDE_MAX_TARGETS; c++) Info->Mode[c] = 0xFF;
    for (c = 0; c < HwInit->ChannelCount; c++) {
        PIDE_CHANNEL ch = &HwInit->Channels[c];
        Info->CommandPort[c] = ch->CmdPort;
        Info->BusMasterPort[c] = ch->BmPort;
        Info->Irq[c] = ch->Irq;
        Info->Native[c] = ch->Native;
        if (ch->Bm != NULL) Info->Flags |= IDE_INFO_BUS_MASTER;
        for (unit = 0; unit < IDE_DEVICES_PER_CHANNEL; unit++) {
            PIDE_DEVICE dev = &ch->Devices[unit];
            UCHAR m;
            if (!dev->Present) continue;
            if (dev->UseDma) {
                m = (UCHAR)((dev->ModeType == IDE_MODE_UDMA ? IDE_INFO_MODE_UDMA : IDE_INFO_MODE_MWDMA) | dev->Mode);
            } else {
                m = (UCHAR)(IDE_INFO_MODE_PIO | dev->PioMode);
            }
            if (dev->IsAtapi) m |= IDE_INFO_MODE_ATAPI;
            Info->Mode[IDE_TARGET_OF(c, unit)] = m;
            Info->DmaErrors[IDE_TARGET_OF(c, unit)] = dev->DmaErrors;
        }
    }
}

VOID
IdeProcessIoControl(IN PHW_DEVICE_EXTENSION HwInit, IN PSCSI_REQUEST_BLOCK Srb)
{
    PIDE_SRB_IO_CONTROL ctl = (PIDE_SRB_IO_CONTROL)Srb->DataBuffer;
    ULONG available;

    if (ctl == NULL || Srb->DataTransferLength < sizeof(IDE_SRB_IO_CONTROL) ||
        ctl->HeaderLength < sizeof(IDE_SRB_IO_CONTROL) ||
        ctl->HeaderLength > Srb->DataTransferLength) {
        Srb->SrbStatus = SRB_STATUS_INVALID_REQUEST;
        return;
    }
    available = Srb->DataTransferLength - ctl->HeaderLength;
    if (available > ctl->Length) available = ctl->Length;

    if (IdeSignatureIs(ctl, IDE_SMART_SIGNATURE)) {
        IdeCheckState(HwInit);
        if (ctl->ControlCode == IDE_SMART_VERSION) {
            IdeSmartVersion(HwInit, ctl, available);
        } else if (ctl->ControlCode > IDE_SMART_VERSION && ctl->ControlCode <= IDE_SMART_WRITE_LOG) {
            ctl->ReturnCode = IdeSmartCommand(HwInit, Srb, ctl, available);
        } else {
            Srb->SrbStatus = SRB_STATUS_INVALID_REQUEST;
            return;
        }
        Srb->SrbStatus = SRB_STATUS_SUCCESS;
        return;
    }

    /* The controller is not woken up for these: an ACPI driver may query
       it while it is powered down */
    if (IdeSignatureIs(ctl, IDE_IOCTL_SIGNATURE)) {
        ctl->ReturnCode = 0;
        switch (ctl->ControlCode) {
        case IDE_IOCTL_GET_INFO:
            if (available >= sizeof(IDE_IOCTL_INFO)) {
                IdeFillInfo(HwInit, (PIDE_IOCTL_INFO)((PUCHAR)ctl + ctl->HeaderLength));
            } else {
                ctl->ReturnCode = 1;
            }
            break;
        case IDE_IOCTL_POWER_DOWN:
            IdePowerDown(HwInit);
            break;
        case IDE_IOCTL_POWER_UP:
            if (HwInit->Stopped) IdeReinitialize(HwInit);
            break;
        case IDE_IOCTL_TRIM_MODE_ON:
            if (available < IDE_TRIM_PATTERN_SIZE || !HwInit->TrimAllowed) {
                ctl->ReturnCode = 1;
                break;
            }
            CopyMemoryBytes(HwInit->TrimPattern, (PUCHAR)ctl + ctl->HeaderLength, IDE_TRIM_PATTERN_SIZE);
            HwInit->TrimMode = TRUE;
            break;
        case IDE_IOCTL_TRIM_MODE_OFF:
            HwInit->TrimMode = FALSE;
            break;
        default:
            Srb->SrbStatus = SRB_STATUS_INVALID_REQUEST;
            return;
        }
        Srb->SrbStatus = SRB_STATUS_SUCCESS;
        return;
    }

    Srb->SrbStatus = SRB_STATUS_INVALID_REQUEST;
}
