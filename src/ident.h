/*
 * IDENT - PATA/SATA IDE SCSI miniport driver (legacy and PCI native mode,
 * bus master DMA with PIO fallback)
 *
 *   IDE_W9X  Windows 95/98/Me (implies IDE_NT4)
 *   IDE_NT4  Windows NT 3.1/3.50/3.51/4.0
 *   (neither) Windows 2000/XP/2003, x86 and x64
 */

#ifndef _IDENT_H_
#define _IDENT_H_

#include <miniport.h>
#include <scsi.h>

/* ------------------------------------------------------------------ */
/* Target configuration                                                */
/* ------------------------------------------------------------------ */

#if defined(IDE_W9X) && !defined(IDE_NT4)
#define IDE_NT4
#endif

#ifdef IDE_NT4
/* NT4/9x: no Dma64BitAddresses field; class drivers send REQUEST SENSE */
#define IDE_AUTOSENSE           0
#else
#define IDE_AUTOSENSE           1
#endif

/* The NT build also runs on NT 3.1, whose ScsiPort and HAL differ:
   HW_INITIALIZATION_DATA must be exactly 40h bytes, the ConfigInfo is
   58h bytes (no SlotNumber / second interrupt), the HAL has no PCI
   configuration access and ScsiPortSetBusDataByOffset does not exist. */
#if defined(IDE_NT4) && !defined(IDE_W9X)
#define IDE_NT31_COMPAT
#endif

#ifndef FIELD_OFFSET
#define FIELD_OFFSET(type, field)       ((LONG)&(((type *)0)->field))
#endif

#ifdef IDE_NT31_COMPAT
#define IDE_HWINIT_SIZE_NT31            ((ULONG)FIELD_OFFSET(HW_INITIALIZATION_DATA, VendorId))
#define IDE_HWINIT_SIZE_NT35            ((ULONG)FIELD_OFFSET(HW_INITIALIZATION_DATA, DeviceId) + sizeof(PVOID))
#endif
/* ConfigInfo field present in the structure ScsiPort handed us? (NT 3.1
   and the 9x port driver pass older, shorter layouts) */
#define IDE_CONFIG_HAS(ci, field)       ((ci)->Length >= (ULONG)FIELD_OFFSET(PORT_CONFIGURATION_INFORMATION, field) + sizeof((ci)->field))

/* NT family: always; 9x: DEBUG builds only */
#if !defined(IDE_W9X) || defined(DEBUG)
#define IDE_DBG_ENABLED
#endif

#ifdef IDE_W9X
#define IDE_DRIVER_NAME         "ident9x"
#else
#define IDE_DRIVER_NAME         "IDENT"
#endif

#define IDE_DRIVER_VERSION      0x01010000  /* 1.1.0.0 */

/* ------------------------------------------------------------------ */
/* Debug output                                                        */
/* ------------------------------------------------------------------ */

/* IDE_DBG((IDE_PFX "Channel %lu\n", ch)); -- double parentheses, since
   VC++ 2.0 / MSVC 4.0 have no variadic macros. Use %lu %lX %s %c only. */
#define IDE_PFX                 "[" IDE_DRIVER_NAME "] "

#ifdef IDE_DBG_ENABLED
#ifdef IDE_W9X
VOID __cdecl IdeDbgPrint(PCHAR Format, ...);
#define IDE_DBG(args)           IdeDbgPrint args
#else
ULONG __cdecl DbgPrint(PCH Format, ...);
#define IDE_DBG(args)           DbgPrint args
#endif
#else
#define IDE_DBG(args)
#endif

/* ------------------------------------------------------------------ */
/* Port I/O                                                            */
/* ------------------------------------------------------------------ */

/* x64 MinGW: srb.h maps the port and register functions to HAL macros;
   the x64 ScsiPort exports them, so take those. */
#if defined(_AMD64_) && defined(__GNUC__)
#undef ScsiPortReadPortUchar
#undef ScsiPortReadPortUshort
#undef ScsiPortReadPortUlong
#undef ScsiPortWritePortUchar
#undef ScsiPortWritePortUshort
#undef ScsiPortWritePortUlong
#undef ScsiPortReadPortBufferUshort
#undef ScsiPortWritePortBufferUshort
UCHAR  ScsiPortReadPortUchar(PUCHAR Port);
USHORT ScsiPortReadPortUshort(PUSHORT Port);
ULONG  ScsiPortReadPortUlong(PULONG Port);
VOID   ScsiPortWritePortUchar(PUCHAR Port, UCHAR Value);
VOID   ScsiPortWritePortUshort(PUSHORT Port, USHORT Value);
VOID   ScsiPortWritePortUlong(PULONG Port, ULONG Value);
VOID   ScsiPortReadPortBufferUshort(PUSHORT Port, PUSHORT Buffer, ULONG Count);
VOID   ScsiPortWritePortBufferUshort(PUSHORT Port, PUSHORT Buffer, ULONG Count);
#undef ScsiPortReadRegisterUlong
#undef ScsiPortWriteRegisterUlong
#undef ScsiPortWriteRegisterUshort
ULONG  ScsiPortReadRegisterUlong(PULONG Register);
VOID   ScsiPortWriteRegisterUlong(PULONG Register, ULONG Value);
VOID   ScsiPortWriteRegisterUshort(PUSHORT Register, USHORT Value);
#endif

/* Port arguments are either a mapped base (PUCHAR) plus offset or a raw
   legacy port number; both go through an integer of pointer size. */
#ifdef _WIN64
typedef unsigned __int64 IDE_PORTADDR;
#else
typedef ULONG IDE_PORTADDR;
#endif
#define IDE_PORT(p)             ((PUCHAR)(IDE_PORTADDR)(p))

#define IDE_INB(p)              ScsiPortReadPortUchar(IDE_PORT(p))
#define IDE_INW(p)              ScsiPortReadPortUshort((PUSHORT)IDE_PORT(p))
#define IDE_IND(p)              ScsiPortReadPortUlong((PULONG)IDE_PORT(p))
#define IDE_OUTB(p, v)          ScsiPortWritePortUchar(IDE_PORT(p), (UCHAR)(v))
#define IDE_OUTW(p, v)          ScsiPortWritePortUshort((PUSHORT)IDE_PORT(p), (USHORT)(v))
#define IDE_OUTD(p, v)          ScsiPortWritePortUlong((PULONG)IDE_PORT(p), (ULONG)(v))
#define IDE_INSW(p, buf, n)     ScsiPortReadPortBufferUshort((PUSHORT)IDE_PORT(p), (PUSHORT)(buf), (n))
#define IDE_OUTSW(p, buf, n)    ScsiPortWritePortBufferUshort((PUSHORT)IDE_PORT(p), (PUSHORT)(buf), (n))

/* ------------------------------------------------------------------ */
/* Limits                                                              */
/* ------------------------------------------------------------------ */

#define IDE_MAX_CHANNELS                2           /* per adapter instance */
#define IDE_DEVICES_PER_CHANNEL         2
#define IDE_MAX_TARGETS                 (IDE_MAX_CHANNELS * IDE_DEVICES_PER_CHANNEL)
#define IDE_MAX_CONTROLLERS             32
#define IDE_MAX_TRANSFER                0x20000     /* 128 KB */
#define IDE_SECTOR_SIZE                 512
#define IDE_MAX_SECTOR_SIZE             4096

/* An unaligned 128 KB buffer spans 33 pages; one PRD per page */
#define IDE_MAX_PHYS_BREAKS             32
#define IDE_PRD_ENTRIES                 64
#define IDE_PRD_TABLE_SIZE              (IDE_PRD_ENTRIES * 8)  /* 512, never crosses 64 KB */

/* Per-channel driver-owned buffer: IDENTIFY data, ATAPI REQUEST SENSE /
   MODE SENSE, the odd last word of an ATAPI PIO transfer */
#define IDE_BOUNCE_SIZE                 512

#define IDE_SENSE_MAX                   32

/* ------------------------------------------------------------------ */
/* PCI                                                                 */
/* ------------------------------------------------------------------ */

#define PCI_CLASS_MASS_STORAGE          0x01
#define PCI_SUBCLASS_IDE                0x01
#define PCI_SUBCLASS_RAID               0x04
#define PCI_SUBCLASS_SATA               0x06
#define PCI_SUBCLASS_OTHER_STORAGE      0x80

/* IDE programming interface */
#define IDE_PROGIF_PRIMARY_NATIVE       0x01
#define IDE_PROGIF_SECONDARY_NATIVE     0x04
#define IDE_PROGIF_BUS_MASTER           0x80

#define IDE_BAR_PRIMARY_CMD             0
#define IDE_BAR_PRIMARY_CTL             1
#define IDE_BAR_SECONDARY_CMD           2
#define IDE_BAR_SECONDARY_CTL           3
#define IDE_BAR_BUS_MASTER              4

/* Legacy channels: command block, control register, interrupt */
#define IDE_LEGACY_PRIMARY_CMD          0x1F0
#define IDE_LEGACY_PRIMARY_CTL          0x3F6
#define IDE_LEGACY_PRIMARY_IRQ          14
#define IDE_LEGACY_SECONDARY_CMD        0x170
#define IDE_LEGACY_SECONDARY_CTL        0x376
#define IDE_LEGACY_SECONDARY_IRQ        15
#define IDE_LEGACY_TERTIARY_CMD         0x1E8
#define IDE_LEGACY_TERTIARY_CTL         0x3EE
#define IDE_LEGACY_TERTIARY_IRQ         11
#define IDE_LEGACY_QUATERNARY_CMD       0x168
#define IDE_LEGACY_QUATERNARY_CTL       0x36E
#define IDE_LEGACY_QUATERNARY_IRQ       10
#define IDE_LEGACY_CHANNELS             4

/* ------------------------------------------------------------------ */
/* Task file and bus master registers                                  */
/* ------------------------------------------------------------------ */

#define IDE_REG_DATA                    0
#define IDE_REG_ERROR                   1           /* read */
#define IDE_REG_FEATURES                1           /* write */
#define IDE_REG_COUNT                   2
#define IDE_REG_LBA0                    3           /* sector number */
#define IDE_REG_LBA1                    4           /* cylinder low */
#define IDE_REG_LBA2                    5           /* cylinder high */
#define IDE_REG_DEVICE                  6
#define IDE_REG_STATUS                  7           /* read */
#define IDE_REG_COMMAND                 7           /* write */

/* Device Control (write) / Alternate Status (read) */
#define IDE_CTL_NIEN                    0x02
#define IDE_CTL_SRST                    0x04
#define IDE_CTL_BASE                    0x08        /* ATA-2 reserved-1 bit */
#define IDE_CTL_HOB                     0x80

#define IDE_STS_ERR                     0x01
#define IDE_STS_DRQ                     0x08
#define IDE_STS_DF                      0x20
#define IDE_STS_DRDY                    0x40
#define IDE_STS_BSY                     0x80

#define IDE_DEV_DEFAULT                 0xA0        /* bits 7 and 5 (obsolete, set) */
#define IDE_DEV_SLAVE                   0x10
#define IDE_DEV_LBA                     0x40

/* Bus master IDE (SFF-8038i), 8 bytes per channel */
#define BM_REG_COMMAND                  0
#define BM_REG_STATUS                   2
#define BM_REG_PRD                      4

#define BM_CMD_START                    0x01
#define BM_CMD_READ                     0x08        /* to memory */

#define BM_STS_ACTIVE                   0x01
#define BM_STS_ERROR                    0x02
#define BM_STS_INTERRUPT                0x04
#define BM_STS_DRIVE0_DMA               0x20
#define BM_STS_DRIVE1_DMA               0x40
#define BM_STS_SIMPLEX                  0x80

#define BM_PRD_EOT                      0x80000000UL

/* Signature after reset (count = 1, LBA0 = 1) */
#define IDE_SIG_ATA_LBA1                0x00
#define IDE_SIG_ATA_LBA2                0x00
#define IDE_SIG_ATAPI_LBA1              0x14
#define IDE_SIG_ATAPI_LBA2              0xEB

/* Polling (Fast Polling: every command is completed inside HwStartIo) */
#define IDE_POLL_INTERVAL_USEC          10

/* Per-command limit, from the SRB's TimeOutValue and ending one second
   before ScsiPort's own request timer would expire (see AHCINT). */
#define IDE_POLL_TIMEOUT_DEFAULT_SEC    10
#define IDE_POLL_TIMEOUT_INTERNAL_SEC   3
#define IDE_POLL_TIMEOUT_MAX_SEC        60
#define IDE_POLL_TIMEOUT_POWER_SEC      30
#define IDE_RESET_TIMEOUT_MS            10000       /* BSY after SRST */
#define IDE_BUSY_WAIT_MS                1000        /* before a command */

/* Interrupt watchdog: the timer completes what the interrupt missed;
   after this many misses in a row the adapter falls back to Fast Polling. */
#define IDE_TIMER_USEC                  100000
#define IDE_MISSED_IRQ_LIMIT            5
#define IDE_DMA_ERROR_LIMIT             3           /* then PIO for the device */

/* ------------------------------------------------------------------ */
/* ATA                                                                 */
/* ------------------------------------------------------------------ */

#define IDE_COMMAND_DATA_SET_MANAGEMENT 0x06
#define IDE_DSM_TRIM                    0x0001
#define IDE_DSM_RANGES_PER_BLOCK        64
#define IDE_DSM_MAX_RANGE               0xFFFF
#define IDE_SMART_RETURN_STATUS_FEATURE 0xDA
#define IDE_SMART_READ_DATA_FEATURE     0xD0
#define IDE_SMART_LBA_SIGNATURE         0xC24F00    /* LBA 23:8 = C2h 4Fh */
#define IDE_COMMAND_READ_SECTORS        0x20
#define IDE_COMMAND_READ_SECTORS_EXT    0x24
#define IDE_COMMAND_READ_DMA_EXT        0x25
#define IDE_COMMAND_READ_LOG_EXT        0x2F
#define IDE_COMMAND_WRITE_SECTORS       0x30
#define IDE_COMMAND_WRITE_SECTORS_EXT   0x34
#define IDE_COMMAND_WRITE_DMA_EXT       0x35
#define IDE_COMMAND_WRITE_DMA_FUA_EXT   0x3D
#define IDE_COMMAND_READ_VERIFY         0x40
#define IDE_COMMAND_READ_VERIFY_EXT     0x42
#define IDE_COMMAND_INIT_DEV_PARAMS     0x91
#define IDE_COMMAND_PACKET              0xA0
#define IDE_COMMAND_IDENTIFY_PACKET     0xA1
#define IDE_COMMAND_SMART               0xB0
#define IDE_COMMAND_READ_DMA            0xC8
#define IDE_COMMAND_WRITE_DMA           0xCA
#define IDE_COMMAND_STANDBY_IMMEDIATE   0xE0
#define IDE_COMMAND_IDLE_IMMEDIATE      0xE1
#define IDE_COMMAND_FLUSH_CACHE         0xE7
#define IDE_COMMAND_FLUSH_CACHE_EXT     0xEA
#define IDE_COMMAND_IDENTIFY_DEVICE     0xEC
#define IDE_COMMAND_SET_FEATURES        0xEF

#define IDE_FEATURE_ENABLE_WRITE_CACHE  0x02
#define IDE_FEATURE_SET_TRANSFER_MODE   0x03
#define IDE_FEATURE_DISABLE_LOOKAHEAD   0x55
#define IDE_FEATURE_DISABLE_WRITE_CACHE 0x82
#define IDE_FEATURE_ENABLE_LOOKAHEAD    0xAA

/* SET FEATURES 03h: transfer mode in the count register */
#define IDE_XFER_PIO_DEFAULT            0x00
#define IDE_XFER_PIO_FLOW               0x08        /* + mode 3..4 */
#define IDE_XFER_MWDMA                  0x20        /* + mode 0..2 */
#define IDE_XFER_UDMA                   0x40        /* + mode 0..6 */

/* ATAPI PACKET features */
#define IDE_PACKET_DMA                  0x01
#define IDE_PACKET_DMADIR               0x04        /* device to host, when word 62 asks for it */

/* ATA Error register */
#define IDE_ERROR_ABORT                 0x04
#define IDE_ERROR_MEDIA_CHANGE_REQ      0x08
#define IDE_ERROR_ID_NOT_FOUND          0x10
#define IDE_ERROR_MEDIA_CHANGE          0x20
#define IDE_ERROR_UNCORRECTABLE         0x40
#define IDE_ERROR_ICRC                  0x80        /* UDMA CRC */

#define ATAPI_ERROR_SENSE_KEY(err)      (((err) >> 4) & 0x0F)

/* IDENTIFY DEVICE words */
#define IDW_GENERAL_CONFIG              0
#define IDW_CYLINDERS                   1
#define IDW_HEADS                       3
#define IDW_SECTORS_PER_TRACK           6
#define IDW_SERIAL                      10
#define IDW_FIRMWARE                    23
#define IDW_MODEL                       27
#define IDW_MULTIPLE                    47
#define IDW_CAPABILITIES                49
#define IDW_PIO_OLD                     51
#define IDW_FIELD_VALIDITY              53
#define IDW_CUR_CYLINDERS               54
#define IDW_CUR_HEADS                   55
#define IDW_CUR_SPT                     56
#define IDW_LBA28_SECTORS               60
#define IDW_ATAPI_DMADIR                62
#define IDW_MWDMA                       63
#define IDW_PIO_MODES                   64
#define IDW_SATA_CAPABILITIES           76
#define IDW_CMDSET_SUPPORTED_1          82
#define IDW_CMDSET_SUPPORTED_2          83
#define IDW_CMDSET_SUPPORTED_EXT        84
#define IDW_CMDSET_ENABLED_1            85
#define IDW_CMDSET_ENABLED_2            86
#define IDW_UDMA                        88
#define IDW_HW_RESET_RESULT             93
#define IDW_LBA48_SECTORS               100
#define IDW_SECTOR_SIZE_INFO            106
#define IDW_ADDITIONAL_SUPPORTED        69
#define IDW_DSM_MAX_BLOCKS              105
#define IDW_FORM_FACTOR                 168
#define IDW_DSM_SUPPORT                 169
#define IDW_ROTATION_RATE               217
#define IDW69_DETERMINISTIC_TRIM        0x4000
#define IDW69_ZERO_AFTER_TRIM           0x0020
#define IDW169_TRIM                     0x0001
#define IDW_LOGICAL_SECTOR_WORDS        117

#define IDW0_ATAPI                      0x8000      /* bit 15 set, 14 clear */
#define IDW0_ATAPI_CDB16                0x0001      /* bits 1:0 = 1 */
#define IDW0_REMOVABLE                  0x0080
#define IDW49_DMA_SUPPORTED             0x0100
#define IDW49_LBA_SUPPORTED             0x0200
#define IDW49_IORDY_SUPPORTED           0x0800
#define IDW53_WORDS_54_58_VALID         0x0001
#define IDW53_WORDS_64_70_VALID         0x0002
#define IDW53_WORD_88_VALID             0x0004
#define IDW62_DMADIR_REQUIRED           0x8000
#define IDW82_SMART                     0x0001
#define IDW82_WRITE_CACHE               0x0020
#define IDW82_LOOKAHEAD                 0x0040
#define IDW83_VALID_MASK                0xC000
#define IDW83_VALID                     0x4000
#define IDW83_LBA48                     0x0400
#define IDW83_FLUSH                     0x1000
#define IDW83_FLUSH_EXT                 0x2000
#define IDW84_FUA                       0x0040
#define IDW93_CABLE_80                  0x2000
#define IDW106_VALID_MASK               0xC000
#define IDW106_VALID                    0x4000
#define IDW106_MULTI_LOGICAL            0x2000
#define IDW106_LONG_LOGICAL             0x1000
#define IDW106_PHYS_EXP_MASK            0x000F

#define ATA_LBA28_LIMIT                 0x10000000UL
#define ATA_LBA28_MAX_SECTORS           256
#define ATA_LBA48_MAX_SECTORS           65536

/* Transfer mode as kept per device */
#define IDE_MODE_PIO                    0
#define IDE_MODE_MWDMA                  1
#define IDE_MODE_UDMA                   2

/* ------------------------------------------------------------------ */
/* SCSI opcodes missing from older SCSI.H versions                     */
/* ------------------------------------------------------------------ */

#ifndef SCSIOP_SYNCHRONIZE_CACHE
#define SCSIOP_SYNCHRONIZE_CACHE        0x35
#endif
#ifndef SCSIOP_MODE_SENSE10
#define SCSIOP_MODE_SENSE10             0x5A
#endif

#define IDE_SCSIOP_START_STOP_UNIT      0x1B
#define IDE_SCSIOP_MODE_SELECT6         0x15
#define IDE_SCSIOP_MODE_SELECT10        0x55
#define IDE_SCSIOP_WRITE_VERIFY         0x2E
#define IDE_SCSIOP_ATA_PASSTHROUGH12    0xA1
#define IDE_SCSIOP_REPORT_LUNS          0xA0
#define IDE_SCSIOP_READ12               0xA8
#define IDE_SCSIOP_WRITE12              0xAA
#define IDE_SCSIOP_VERIFY12             0xAF
#define IDE_SCSIOP_READ_CD              0xBE
#define IDE_SCSIOP_ATA_PASSTHROUGH16    0x85
#define IDE_SCSIOP_READ16               0x88
#define IDE_SCSIOP_WRITE16              0x8A
#define IDE_SCSIOP_VERIFY16             0x8F
#define IDE_SCSIOP_SYNC_CACHE16         0x91
#define IDE_SCSIOP_SERVICE_ACTION_IN16  0x9E
#define IDE_SCSIOP_UNMAP                0x42
#define IDE_SCSIOP_LOG_SENSE            0x4D
#define IDE_SA_READ_CAPACITY16          0x10
#define IDE_READ_CAPACITY16_DATA_LEN    32

#define IDE_INQUIRY_VERSION_SPC3        0x05

#define IDE_VPD_SUPPORTED_PAGES         0x00
#define IDE_VPD_SERIAL_NUMBER           0x80
#define IDE_VPD_BLOCK_LIMITS            0xB0
#define IDE_VPD_BLOCK_CHARACTERISTICS   0xB1
#define IDE_VPD_PROVISIONING            0xB2

#define IDE_LOG_PAGE_SUPPORTED          0x00
#define IDE_LOG_PAGE_TEMPERATURE        0x0D
#define IDE_LOG_PAGE_INFO_EXCEPTIONS    0x2F
#define IDE_ASC_FAILURE_PREDICTION      0x5D
#define IDE_UNMAP_MAX_DESCRIPTORS       64
#define IDE_UNMAP_MAX_BLOCKS            (IDE_DSM_RANGES_PER_BLOCK * IDE_DSM_MAX_RANGE)

#define IDE_MODE_PAGE_CACHING           0x08
#define IDE_MODE_PAGE_GEOMETRY          0x04
#define IDE_MODE_PAGE_CONTROL           0x0A
#define IDE_MODE_PAGE_ALL               0x3F

#define IDE_ASC_NONE                    0x00
#define IDE_ASC_UNRECOVERED_READ        0x11
#define IDE_ASC_PARAM_LIST_LENGTH       0x1A
#define IDE_ASC_INVALID_OPCODE          0x20
#define IDE_ASC_LBA_OUT_OF_RANGE        0x21
#define IDE_ASC_INVALID_CDB_FIELD       0x24
#define IDE_ASC_INVALID_PARAM_FIELD     0x26
#define IDE_ASC_MEDIUM_CHANGED          0x28
#define IDE_ASC_POWER_ON_RESET          0x29
#define IDE_ASC_NO_MEDIA                0x3A
#define IDE_ASC_IU_CRC_ERROR            0x47
#define IDE_ASCQ_ATA_INFO_AVAILABLE     0x1D

/* ------------------------------------------------------------------ */
/* 64-bit block numbers without compiler 64-bit types                  */
/* ------------------------------------------------------------------ */

typedef struct _IDE_LBA {
    ULONG Low;
    ULONG High;
} IDE_LBA, *PIDE_LBA;

/* ------------------------------------------------------------------ */
/* Hardware structures                                                 */
/* ------------------------------------------------------------------ */

#pragma pack(push, 1)

typedef struct _IDE_PRD_ENTRY {
    ULONG Address;                      /* bit 0 clear */
    ULONG CountEot;                     /* [15:0] bytes (0 = 64 KB), bit 31 EOT */
} IDE_PRD_ENTRY, *PIDE_PRD_ENTRY;

/* ATA registers as the device returned them */
typedef struct _IDE_ATA_RESULT {
    UCHAR Status;
    UCHAR Error;
    UCHAR Lba0;
    UCHAR Lba1;
    UCHAR Lba2;
    UCHAR Device;
    UCHAR Lba3;
    UCHAR Lba4;
    UCHAR Lba5;
    UCHAR CountLow;
    UCHAR CountHigh;
} IDE_ATA_RESULT, *PIDE_ATA_RESULT;

#pragma pack(pop)

typedef struct _MODE_PARAMETER_HEADER_10 {
    UCHAR ModeDataLengthMsb;
    UCHAR ModeDataLengthLsb;
    UCHAR MediumType;
    UCHAR Reserved[5];
} MODE_PARAMETER_HEADER_10, *PMODE_PARAMETER_HEADER_10;

/* ------------------------------------------------------------------ */
/* Miniport IOCTLs (SRB_FUNCTION_IO_CONTROL)                           */
/* ------------------------------------------------------------------ */

typedef struct _IDE_SRB_IO_CONTROL {
    ULONG HeaderLength;
    UCHAR Signature[8];
    ULONG Timeout;
    ULONG ControlCode;
    ULONG ReturnCode;
    ULONG Length;
} IDE_SRB_IO_CONTROL, *PIDE_SRB_IO_CONTROL;

/* "IDENT   ": power control for an ACPI driver on NT 3.1-4.0, and info.
   Send through IOCTL_SCSI_MINIPORT to \\.\ScsiN:. */
#define IDE_IOCTL_SIGNATURE             "IDENT   "
#define IDE_IOCTL_GET_INFO              0x00000001
#define IDE_IOCTL_POWER_DOWN            0x00000010  /* flush, standby, channels stopped */
#define IDE_IOCTL_POWER_UP              0x00000011  /* set the channels up again */
#define IDE_IOCTL_TRIM_MODE_ON          0x00000020  /* 4 KB pattern after the header */
#define IDE_IOCTL_TRIM_MODE_OFF         0x00000021
#define IDE_TRIM_PATTERN_SIZE           4096

#define IDE_INFO_ASYNC                  0x00000001  /* DMA completions by interrupt */
#define IDE_INFO_POWERED_DOWN           0x00000002
#define IDE_INFO_BUS_MASTER             0x00000004  /* bus master registers present */
#define IDE_INFO_TRIM_MODE              0x00000008  /* writes of the TRIM pattern become TRIMs */

/* Per device in IDE_IOCTL_INFO.Mode: type in bits 7:4, mode in 3:0;
   0xFF no device */
#define IDE_INFO_MODE_PIO               0x00
#define IDE_INFO_MODE_MWDMA             0x10
#define IDE_INFO_MODE_UDMA              0x20
#define IDE_INFO_MODE_TYPE_MASK         0xF0
#define IDE_INFO_MODE_ATAPI             0x80        /* also set for ATAPI devices */

typedef struct _IDE_IOCTL_INFO {
    ULONG Version;                      /* IDE_DRIVER_VERSION */
    ULONG Flags;                        /* IDE_INFO_xxx */
    ULONG ChannelCount;
    ULONG PciVendorDevice;              /* 0 for a legacy ISA channel */
    USHORT CommandPort[IDE_MAX_CHANNELS];
    USHORT BusMasterPort[IDE_MAX_CHANNELS];
    UCHAR  Irq[IDE_MAX_CHANNELS];
    UCHAR  Native[IDE_MAX_CHANNELS];
    UCHAR  Mode[IDE_MAX_TARGETS];       /* IDE_INFO_MODE_xxx */
    UCHAR  DmaErrors[IDE_MAX_TARGETS];
    ULONG  InterruptCount;
} IDE_IOCTL_INFO, *PIDE_IOCTL_INFO;

/* "SCSIDISK": SMART, as IOCTL_SCSI_MINIPORT_SMART_xxx (NTDDSCSI.H) */
#define IDE_SMART_SIGNATURE             "SCSIDISK"
#define IDE_SMART_BASE                  0x001B0500
#define IDE_SMART_VERSION               (IDE_SMART_BASE + 0x00)
#define IDE_SMART_IDENTIFY              (IDE_SMART_BASE + 0x01)
#define IDE_SMART_READ_ATTRIBS          (IDE_SMART_BASE + 0x02)
#define IDE_SMART_READ_THRESHOLDS       (IDE_SMART_BASE + 0x03)
#define IDE_SMART_ENABLE                (IDE_SMART_BASE + 0x04)
#define IDE_SMART_DISABLE               (IDE_SMART_BASE + 0x05)
#define IDE_SMART_RETURN_STATUS         (IDE_SMART_BASE + 0x06)
#define IDE_SMART_AUTOSAVE              (IDE_SMART_BASE + 0x07)
#define IDE_SMART_SAVE_ATTRIBS          (IDE_SMART_BASE + 0x08)
#define IDE_SMART_OFFLINE_DIAGS         (IDE_SMART_BASE + 0x09)
#define IDE_SMART_AUTO_OFFLINE          (IDE_SMART_BASE + 0x0A)
#define IDE_SMART_READ_LOG              (IDE_SMART_BASE + 0x0B)
#define IDE_SMART_WRITE_LOG             (IDE_SMART_BASE + 0x0C)

#pragma pack(push, 1)

typedef struct _IDE_IDEREGS {
    UCHAR Features;
    UCHAR SectorCount;
    UCHAR SectorNumber;
    UCHAR CylLow;
    UCHAR CylHigh;
    UCHAR DriveHead;
    UCHAR Command;
    UCHAR Reserved;
} IDE_IDEREGS, *PIDE_IDEREGS;

typedef struct _IDE_SENDCMD_IN {
    ULONG       BufferSize;
    IDE_IDEREGS Regs;
    UCHAR       DriveNumber;
    UCHAR       Reserved[3];
    ULONG       Reserved2[4];
    UCHAR       Buffer[1];
} IDE_SENDCMD_IN, *PIDE_SENDCMD_IN;

typedef struct _IDE_SENDCMD_OUT {
    ULONG BufferSize;
    UCHAR DriverError;
    UCHAR IdeError;
    UCHAR Reserved[2];
    ULONG Reserved2[2];
    UCHAR Buffer[1];
} IDE_SENDCMD_OUT, *PIDE_SENDCMD_OUT;

typedef struct _IDE_GETVERSION {
    UCHAR Version;
    UCHAR Revision;
    UCHAR Reserved;
    UCHAR IdeDeviceMap;
    ULONG Capabilities;
    ULONG Reserved2[4];
} IDE_GETVERSION, *PIDE_GETVERSION;

#pragma pack(pop)

#define IDE_SENDCMD_IN_HEADER           ((ULONG)FIELD_OFFSET(IDE_SENDCMD_IN, Buffer))
#define IDE_SENDCMD_OUT_HEADER          ((ULONG)FIELD_OFFSET(IDE_SENDCMD_OUT, Buffer))
#define IDE_SMART_CAP_ATA_ID            0x00000001
#define IDE_SMART_CAP_ATAPI_ID          0x00000002
#define IDE_SMART_CAP_SMART             0x00000004
#define IDE_SMART_DRV_ERROR             1
#define IDE_SMART_INVALID_DRIVE         5
#define IDE_SMART_INVALID_BUFFER        4

/* ------------------------------------------------------------------ */
/* Driver state                                                        */
/* ------------------------------------------------------------------ */

/* IDENTIFY words kept per device (indexes into IDE_DEVICE.Id) */
#define IDE_ID_CONFIG                   0           /* word 0 */
#define IDE_ID_CYLINDERS                1           /* word 1 */
#define IDE_ID_HEADS                    2           /* word 3 */
#define IDE_ID_SPT                      3           /* word 6 */
#define IDE_ID_CMDSET_1                 4           /* word 82 */
#define IDE_ID_CMDSET_EXT               5           /* word 84 */
#define IDE_ID_ENABLED_1                6           /* word 85 */
#define IDE_ID_SERIAL                   7           /* words 10..19 */
#define IDE_ID_FIRMWARE                 17          /* words 23..26 */
#define IDE_ID_MODEL                    21          /* words 27..46 */
#define IDE_ID_WORDS                    41

typedef struct _IDE_DEVICE {
    BOOLEAN                 Present;
    BOOLEAN                 IsAtapi;
    BOOLEAN                 IdentifyValid;
    UCHAR                   Unit;           /* 0 master, 1 slave */

    /* Parsed from IDENTIFY DEVICE (ATA only), see IdeParseIdentify */
    BOOLEAN                 LbaSupported;
    BOOLEAN                 Lba48;
    BOOLEAN                 Fua;
    BOOLEAN                 FlushExt;
    UCHAR                   PhysExponent;
    IDE_LBA                 TotalSectors;   /* logical sectors */
    ULONG                   SectorSize;
    USHORT                  Cylinders;      /* CHS translation (no LBA) */
    UCHAR                   Heads;
    UCHAR                   Spt;

    /* ATAPI */
    BOOLEAN                 Cdb16;          /* 16-byte packets */
    BOOLEAN                 DmaDir;         /* DMADIR bit required */

    /* Transfer mode in effect and what the device could do */
    UCHAR                   ModeType;       /* IDE_MODE_xxx */
    UCHAR                   Mode;           /* 0..6 */
    UCHAR                   PioMode;        /* 0..4 */
    BOOLEAN                 UseDma;
    BOOLEAN                 DmaCapable;     /* device and controller */
    UCHAR                   DmaErrors;      /* consecutive; PIO after the limit */
    BOOLEAN                 NoLba48Dma;     /* 48-bit reads/writes with PIO commands */
    UCHAR                   MaxSectors;     /* sectors per command, 0 = no limit */

    /* DATA SET MANAGEMENT TRIM, medium (IDENTIFY words 69, 168, 169, 217) */
    BOOLEAN                 Trim;
    BOOLEAN                 TrimZeroes;     /* deterministic zeroes after TRIM */
    BOOLEAN                 Smart;          /* SMART supported and enabled */
    BOOLEAN                 Standby;        /* STANDBY IMMEDIATE at shutdown done */
    USHORT                  RotationRate;   /* 1 = non-rotating */
    UCHAR                   FormFactor;

    /* MODE SELECT settings, applied again after a reset: 0 as the drive
       has it, 1 on, 2 off */
    UCHAR                   WantWriteCache;
    UCHAR                   WantLookAhead;

    BOOLEAN                 SensePending;
    UCHAR                   SenseLength;
    UCHAR                   Sense[IDE_SENSE_MAX];

    /* Status and Error of the last completed command */
    UCHAR                   LastStatus;
    UCHAR                   LastError;

    USHORT                  Id[IDE_ID_WORDS];
} IDE_DEVICE, *PIDE_DEVICE;

typedef struct _IDE_CHANNEL {
    BOOLEAN                 Present;        /* ports mapped */
    BOOLEAN                 Native;         /* PCI native mode (shared PCI interrupt) */
    BOOLEAN                 NeedsSetup;     /* after a reset: modes and settings */
    BOOLEAN                 Async;          /* DMA completes in the interrupt */
    UCHAR                   DeviceCount;    /* devices found */
    UCHAR                   Irq;            /* reported, for GET_INFO */
    USHORT                  CmdPort;        /* I/O port numbers, for GET_INFO */
    USHORT                  BmPort;
    BOOLEAN                 Off;            /* OFF=: the channel is left alone */
    UCHAR                   UnitsOff;       /* OFF=c.u: bit 0 master, bit 1 slave left alone */

    PUCHAR                  Cmd;            /* mapped command block */
    PUCHAR                  Ctl;            /* mapped control register */
    PUCHAR                  Bm;             /* mapped bus master block, NULL without */

    PIDE_PRD_ENTRY          Prd;
    ULONG                   PrdPhysical;
    PUCHAR                  BounceBuffer;
    ULONG                   BouncePhysical;

    /* One DMA command in flight (interrupt mode); a second request for
       the channel waits in Pending */
    PSCSI_REQUEST_BLOCK     Outstanding;
    PSCSI_REQUEST_BLOCK     Pending;
    UCHAR                   ActiveUnit;
    BOOLEAN                 ActiveWrite;
    BOOLEAN                 ActiveAtapi;

    IDE_ATA_RESULT          Result;
    IDE_DEVICE              Devices[IDE_DEVICES_PER_CHANNEL];
} IDE_CHANNEL, *PIDE_CHANNEL;

/* Intel PIIX/ICH PATA timing programming */
#define IDE_INTEL_NONE                  0
#define IDE_INTEL_PIIX3                 1           /* MWDMA only */
#define IDE_INTEL_PIIX4                 2           /* UDMA 2 */
#define IDE_INTEL_ICH66                 3           /* UDMA 4 */
#define IDE_INTEL_ICH100                4           /* UDMA 5 */

/* Other controller families with needs of their own (IdeChipKind) */
#define IDE_CHIP_GENERIC                0
#define IDE_CHIP_CMD643                 1           /* MWDMA only */
#define IDE_CHIP_CMD646                 2           /* UDMA 2 from revision 5 */
#define IDE_CHIP_CMD648                 3           /* UDMA 4 */
#define IDE_CHIP_CMD649                 4           /* UDMA 5 */
#define IDE_CHIP_ALI                    5           /* M5229 */
#define IDE_CHIP_SII3112                6           /* also 0240 and the ATI IXP SATA functions */
#define IDE_CHIP_SII3512                7
#define IDE_CHIP_SII3114                8
#define IDE_CHIP_CMD640                 9           /* PIO only, read-ahead errata */
#define IDE_CHIP_RZ1000                 10          /* PC Tech RZ1000/1001: PIO only, FIFO errata */
#define IDE_CHIP_OSB4                   11          /* ServerWorks OSB4: no UDMA on disks */
#define IDE_CHIP_CSB                    12          /* ServerWorks CSB5/CSB6 */
/* Both channels in one adapter instance, Fast Polling: the two channels
   share one data FIFO and must never be busy at the same time */
#define IDE_CHIP_SERIALIZED(c)          ((c) == IDE_CHIP_CMD640 || (c) == IDE_CHIP_RZ1000)
#define IDE_CHIP_IS_CMD(c)              ((c) >= IDE_CHIP_CMD643 && (c) <= IDE_CHIP_CMD649)
#define IDE_CHIP_IS_SII(c)              ((c) >= IDE_CHIP_SII3112 && (c) <= IDE_CHIP_SII3114)

/* No UDMA (IdeHostUdmaLimit) */
#define IDE_NO_UDMA                     0xFFFFFFFF

/* CMD64x configuration registers. The interrupt latches stay set until
   software writes 1 to them, and the controller holds its PCI interrupt
   while one is set. */
#define CMD_CFR                         0x50        /* bit 2: primary interrupt latch */
#define CMD_ARTTIM0                     0x53        /* address setup, primary master */
#define CMD_DRWTIM0                     0x54        /* active/recovery, primary master */
#define CMD_ARTTIM1                     0x55
#define CMD_DRWTIM1                     0x56
#define CMD_ARTTIM23                    0x57        /* secondary (shared); bit 4: secondary latch */
#define CMD_DRWTIM2                     0x58
#define CMD_DRWTIM3                     0x5B
#define CMD_MRDMODE                     0x71        /* also at bus master base + 1 */
#define CMD_UDIDETCR0                   0x73        /* UDMA timing, primary */
#define CMD_BMIDECSR                    0x79        /* 648/649: bit 0/1 80-conductor cable */
#define CMD_UDIDETCR1                   0x7B
#define CMD_CFR_INTR_CH0                0x04
#define CMD_ARTTIM23_INTR_CH1           0x10
#define CMD_MRDMODE_INTR_CH0            0x04        /* 648/649 */
#define CMD_MRDMODE_INTR_CH1            0x08
#define CMD_MRDMODE_BLOCK               0x30        /* interrupt blocking, both channels */
#define CMD_MRDMODE_READ_LINE           0x02        /* memory read line */

/* Silicon Image 311x: BAR 5 register space, per port */
#define SII_MMIO_BAR                    5
#define SII_MMIO_LENGTH                 0x200
#define SII_MMIO_LENGTH_3114            0x400
#define SII_MAX_PORTS                   4

typedef struct _HW_DEVICE_EXTENSION {
    ULONG                   PciBus;
    ULONG                   PciSlot;
    USHORT                  VendorId;
    USHORT                  DeviceId;
    BOOLEAN                 HasPci;         /* channels belong to a PCI function */
    BOOLEAN                 PciWritable;    /* config space can be written */
    UCHAR                   Intel;          /* IDE_INTEL_xxx */
    UCHAR                   Chip;           /* IDE_CHIP_xxx */
    UCHAR                   Revision;
    BOOLEAN                 NoLba48Dma;     /* ALi before C5 */
    BOOLEAN                 NoAtapiDma;     /* ALi */
    UCHAR                   ChannelCount;
    UCHAR                   FirstChannel;   /* controller channel of Channels[0] (0/1) */

    BOOLEAN                 AsyncMode;      /* interrupts connected and wanted */
    BOOLEAN                 DmaAllowed;     /* not disabled by DMA=0 */
    BOOLEAN                 DmaForced;      /* DMA=1: use the best mode even without BIOS hints */
    BOOLEAN                 Stopped;        /* ScsiStopAdapter / POWER_DOWN */
    BOOLEAN                 TimerArmed;
    BOOLEAN                 DumpMode;       /* crash dump / hibernation */
    BOOLEAN                 CheckState;     /* verify the devices before the next request */
    ULONG                   MissedInterrupts;
    ULONG                   InterruptCount;

    IDE_CHANNEL             Channels[IDE_MAX_CHANNELS];

    PUCHAR                  CmdMrdMode;     /* CMD648/649: MRDMODE at bus master base + 1 */
    PUCHAR                  SiiMmio;        /* Silicon Image: BAR 5 */
    ULONG                   SiiMmioLength;

    PUCHAR                  DmaArea;
    ULONG                   DmaLength;
    SCSI_PHYSICAL_ADDRESS   DmaAreaPhysical;

    BOOLEAN                 TrimAllowed;    /* TRIM=0 turns UNMAP off */
    BOOLEAN                 Capacity16;     /* class driver follows up with READ CAPACITY(16) */
    BOOLEAN                 StandbyAtShutdown; /* STANDBY=1: spin down on SRB_FUNCTION_SHUTDOWN */
    BOOLEAN                 TrimMode;       /* TRIM_MODE_ON: pattern writes become TRIMs */
    UCHAR                   TrimPattern[IDE_TRIM_PATTERN_SIZE];
} HW_DEVICE_EXTENSION, *PHW_DEVICE_EXTENSION;

/* One ATA/ATAPI command to put on the wire */
typedef struct _ATA_REQUEST {
    PVOID     DataBuffer;       /* ignored with UseBounce */
    ULONG     DataBufferLen;
    BOOLEAN   IsWrite;
    BOOLEAN   IsAtapi;
    BOOLEAN   UseBounce;        /* data in the channel's BounceBuffer */
    BOOLEAN   Is48Bit;          /* ATA only */
    BOOLEAN   RawDevice;        /* Device register as given (pass-through) */
    BOOLEAN   UseDma;           /* bus master transfer */
    BOOLEAN   Chs;              /* Lba holds C/H/S as the device register wants it */
    UCHAR     Unit;
    UCHAR     Command;          /* ATA only */
    UCHAR     Device;           /* with RawDevice */
    USHORT    Features;
    IDE_LBA   Lba;              /* ATA only */
    ULONG     SectorCount;      /* ATA only, 1..65536 (also the raw count) */
    ULONG     TimeoutSec;       /* 0: from the SRB */
    PUCHAR    Cdb;              /* ATAPI only */
    ULONG     CdbLength;
} ATA_REQUEST, *PATA_REQUEST;

/* IdeRunCommand results */
#define IDE_CMD_OK              0
#define IDE_CMD_DEVICE_ERROR    1   /* Status.ERR: device rejected/failed it */
#define IDE_CMD_HBA_ERROR       2   /* bus master error or timeout */
#define IDE_CMD_BAD_BUFFER      3   /* buffer could not be described */

/* ------------------------------------------------------------------ */
/* Helpers                                                             */
/* ------------------------------------------------------------------ */

#if !defined(IDE_NT4)
#pragma function(memset)
static __inline void * __cdecl memset(void *dst, int val, size_t count) {
    char *p = (char *)dst;
    while (count--) *p++ = (char)val;
    return dst;
}
#define ZeroMemoryBytes(dst, len)       memset((dst), 0, (len))
#else
static __inline void ZeroMemoryBytes(PVOID dst, ULONG len) {
    PUCHAR d = (PUCHAR)dst;
    while (len--) *d++ = 0;
}
#endif

static __inline void CopyMemoryBytes(PVOID dst, PVOID src, ULONG len) {
    PUCHAR d = (PUCHAR)dst;
    PUCHAR s = (PUCHAR)src;
    while (len--) *d++ = *s++;
}

#define IDE_CHANNEL_OF_TARGET(hw, target)   (&(hw)->Channels[(target) / IDE_DEVICES_PER_CHANNEL])
#define IDE_UNIT_OF_TARGET(target)          ((target) % IDE_DEVICES_PER_CHANNEL)
#define IDE_DEVICE_OF_TARGET(hw, target)    (&IDE_CHANNEL_OF_TARGET(hw, target)->Devices[IDE_UNIT_OF_TARGET(target)])
#define IDE_TARGET_OF(ch, unit)             ((ch) * IDE_DEVICES_PER_CHANNEL + (unit))

/* ------------------------------------------------------------------ */
/* Cross-file prototypes                                               */
/* ------------------------------------------------------------------ */

/* idemain.c */
BOOLEAN IdeReinitialize(IN PHW_DEVICE_EXTENSION HwInit);
VOID    IdeChipInit(IN PHW_DEVICE_EXTENSION HwInit);
BOOLEAN IdeChipClearInterrupt(IN PHW_DEVICE_EXTENSION HwInit, IN ULONG Channel);
VOID    IdeCheckState(IN PHW_DEVICE_EXTENSION HwInit);
VOID    IdeArmTimer(IN PHW_DEVICE_EXTENSION HwInit);
BOOLEAN IdePowerDown(IN PHW_DEVICE_EXTENSION HwInit);
VOID    IdeSetupChannel(IN PHW_DEVICE_EXTENSION HwInit, IN ULONG Channel);
ULONG   IdePciReadConfig(IN PHW_DEVICE_EXTENSION HwInit, IN ULONG Offset, IN ULONG Width);
VOID    IdePciWriteConfig(IN PHW_DEVICE_EXTENSION HwInit, IN ULONG Offset, IN ULONG Width, IN ULONG Value);

/* idecmd.c */
BOOLEAN IdeResetChannel(IN PHW_DEVICE_EXTENSION HwInit, IN ULONG Channel);
BOOLEAN IdeWaitStatus(IN PIDE_CHANNEL Ch, IN UCHAR Mask, IN UCHAR Value, IN ULONG TimeoutUsec);
UCHAR   IdeAltStatus(IN PIDE_CHANNEL Ch);
VOID    IdeSetControl(IN PIDE_CHANNEL Ch, IN UCHAR Bits);
VOID    IdeSelectUnit(IN PIDE_CHANNEL Ch, IN ULONG Unit);
ULONG   IdeRunCommand(IN PHW_DEVICE_EXTENSION HwInit, IN ULONG Channel, IN PSCSI_REQUEST_BLOCK Srb, IN PATA_REQUEST Req);
ULONG   IdeRunSimple(IN PHW_DEVICE_EXTENSION HwInit, IN ULONG Channel, IN ULONG Unit,
                     IN UCHAR Command, IN USHORT Features, IN ULONG Count, IN ULONG TimeoutSec);
BOOLEAN IdeIdentifyDevice(IN PHW_DEVICE_EXTENSION HwInit, IN ULONG Channel, IN ULONG Unit);
VOID    IdeApplyDeviceSettings(IN PHW_DEVICE_EXTENSION HwInit, IN ULONG Channel, IN ULONG Unit);
VOID    IdeSelectTransferMode(IN PHW_DEVICE_EXTENSION HwInit, IN ULONG Channel, IN ULONG Unit);
VOID    IdeDisableDma(IN PHW_DEVICE_EXTENSION HwInit, IN ULONG Channel, IN ULONG Unit);
BOOLEAN IdeBusMasterAlive(IN PHW_DEVICE_EXTENSION HwInit, IN ULONG Channel);
BOOLEAN IdeChannelAlive(IN PHW_DEVICE_EXTENSION HwInit, IN ULONG Channel);
#if !defined(IDE_NT4)
VOID    IdeHookPnp(IN PVOID DriverObject);          /* idepnp.c */
#endif
UCHAR   IdeIssueDma(IN PHW_DEVICE_EXTENSION HwInit, IN ULONG Channel, IN PSCSI_REQUEST_BLOCK Srb, IN PATA_REQUEST Req);
ULONG   IdeServiceChannel(IN PHW_DEVICE_EXTENSION HwInit, IN ULONG Channel, IN BOOLEAN FromInterrupt);
VOID    IdeDrainChannel(IN PHW_DEVICE_EXTENSION HwInit, IN ULONG Channel);
VOID    IdeFailOutstanding(IN PHW_DEVICE_EXTENSION HwInit, IN ULONG Channel, IN UCHAR SrbStatus);
VOID    IdeStopChannel(IN PHW_DEVICE_EXTENSION HwInit, IN ULONG Channel);

/* idesatl.c */
VOID    IdeParseIdentify(IN PHW_DEVICE_EXTENSION HwInit, IN ULONG Channel, IN ULONG Unit);
VOID    IdeSatlProcessSrb(IN PHW_DEVICE_EXTENSION HwInit, IN PSCSI_REQUEST_BLOCK Srb);
VOID    IdeProcessIoControl(IN PHW_DEVICE_EXTENSION HwInit, IN PSCSI_REQUEST_BLOCK Srb);
BOOLEAN IdeFlushDevice(IN PHW_DEVICE_EXTENSION HwInit, IN ULONG Channel, IN ULONG Unit);
VOID    IdeShutdownDevice(IN PHW_DEVICE_EXTENSION HwInit, IN ULONG Channel, IN ULONG Unit);
VOID    IdeSatlCompleteAtaError(IN PHW_DEVICE_EXTENSION HwInit, IN ULONG Channel, IN PSCSI_REQUEST_BLOCK Srb);
VOID    IdeSatlCompleteDma(IN PHW_DEVICE_EXTENSION HwInit, IN ULONG Channel, IN PSCSI_REQUEST_BLOCK Srb, IN ULONG Result);
BOOLEAN IdeSatlStartPending(IN PHW_DEVICE_EXTENSION HwInit, IN ULONG Channel, IN PSCSI_REQUEST_BLOCK Srb);

#endif /* _IDENT_H_ */
