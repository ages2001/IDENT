#include "ident.h"

/* idemain.c - DriverEntry, controller and channel discovery (PCI native
   and compatibility mode, legacy ISA), initialization, ScsiPort entry
   points, interrupt watchdog, power */

BOOLEAN IdeHwInitialize(IN PVOID DeviceExtension);
BOOLEAN IdeStartIo(IN PVOID DeviceExtension, IN PSCSI_REQUEST_BLOCK Srb);
BOOLEAN IdeInterrupt(IN PVOID DeviceExtension);
BOOLEAN IdeResetBus(IN PVOID HwDeviceExtension, IN ULONG PathId);
ULONG   IdeFindAdapter(IN PVOID DeviceExtension, IN PVOID Context, IN PVOID BusInformation,
                       IN PCHAR ArgumentString, IN OUT PPORT_CONFIGURATION_INFORMATION ConfigInfo,
                       OUT PBOOLEAN Again);
VOID    IdeTimer(IN PVOID DeviceExtension);
#ifdef IDE_W9X
BOOLEAN IdeAdapterState(IN PVOID DeviceExtension, IN PVOID Context, IN BOOLEAN SaveState);
#endif
#if !defined(IDE_NT4)
SCSI_ADAPTER_CONTROL_STATUS IdeAdapterControl(IN PVOID DeviceExtension,
                                              IN SCSI_ADAPTER_CONTROL_TYPE ControlType,
                                              IN PVOID Parameters);
#endif

/* Crash dump / hibernation copy of the driver (DriverEntry without a
   driver object): polled, no registry access. */
static BOOLEAN g_IdeDumpMode;

/* ------------------------------------------------------------------ */
/* Windows 9x: VMM services                                            */
/* ------------------------------------------------------------------ */

#ifdef IDE_W9X

#define VMM_DEVICE_ID                   0x0001
#define Debug_Printf_Service_Ordinal    0x012D

/* _MapPhysToLinear(PhysAddr, nBytes, flags): a memory-mapped BAR. The 9x
   ScsiPortGetDeviceBase does not map one reliably (KB Q169584). VMM
   service 006Ch, called through int 20h; 0xFFFFFFFF on failure. */
#ifdef __GNUC__
static PVOID
IdeMapPhysToLinear(ULONG PhysAddr, ULONG Bytes)
{
    PVOID linear;

    __asm__ __volatile__(
        "pushl $0\n\t"
        "pushl %2\n\t"
        "pushl %1\n\t"
        "int $0x20\n\t"
        ".word 0x006C\n\t"
        ".word 0x0001\n\t"
        "addl $12, %%esp"
        : "=a" (linear)
        : "r" (PhysAddr), "r" (Bytes)
        : "ecx", "edx", "memory", "cc");
    return linear;
}
#else
static PVOID
IdeMapPhysToLinear(ULONG PhysAddr, ULONG Bytes)
{
    PVOID linear;

    __asm push 0
    __asm push Bytes
    __asm push PhysAddr
    __asm int  0x20
    __asm _emit 0x6C
    __asm _emit 0x00
    __asm _emit ((VMM_DEVICE_ID >> 0) & 0xFF)
    __asm _emit ((VMM_DEVICE_ID >> 8) & 0xFF)
    __asm add  esp, 3*4
    __asm mov  linear, eax
    return linear;
}
#endif

#ifdef IDE_DBG_ENABLED
/* _Debug_Printf_Service(format, argptr): WDEB386, SoftICE or DebugView */
VOID __cdecl
IdeDbgPrint(PCHAR Format, ...)
{
    __asm lea  eax, (Format + 4)
    __asm push eax
    __asm push Format
    __asm int  0x20
    __asm _emit ((Debug_Printf_Service_Ordinal >> 0) & 0xFF)
    __asm _emit ((Debug_Printf_Service_Ordinal >> 8) & 0xFF)
    __asm _emit ((VMM_DEVICE_ID >> 0) & 0xFF)
    __asm _emit ((VMM_DEVICE_ID >> 8) & 0xFF)
    __asm add  esp, 2*4
}
#endif

/* Required: SCSIPORT.PDR reports "Init Failure" if HwAdapterState is NULL.
   On "restore" the BIOS may have reset the drives (APM resume): the next
   request puts the transfer modes and settings back (see IdeCheckState). */
BOOLEAN
IdeAdapterState(IN PVOID DeviceExtension, IN PVOID Context, IN BOOLEAN SaveState)
{
    (VOID)Context;
    IDE_DBG((IDE_PFX "AdapterState: %s\n", SaveState ? "save" : "restore"));
    if (!SaveState) ((PHW_DEVICE_EXTENSION)DeviceExtension)->CheckState = TRUE;
    return TRUE;
}

#endif /* IDE_W9X */

/* ------------------------------------------------------------------ */
/* Claim table: channels by command port, functions by bus/slot        */
/* ------------------------------------------------------------------ */

#define IDE_MAX_CLAIMED                 (IDE_MAX_CONTROLLERS * 2 + IDE_LEGACY_CHANNELS)

static USHORT g_IdeClaimedPorts[IDE_MAX_CLAIMED];
static ULONG  g_IdeClaimedCount;

static BOOLEAN
IdeIsPortClaimed(IN USHORT CmdPort)
{
    ULONG i;
    for (i = 0; i < g_IdeClaimedCount; i++) {
        if (g_IdeClaimedPorts[i] == CmdPort) return TRUE;
    }
    return FALSE;
}

static VOID
IdeClaimPort(IN USHORT CmdPort)
{
    if (IdeIsPortClaimed(CmdPort)) return;
    if (g_IdeClaimedCount < IDE_MAX_CLAIMED) g_IdeClaimedPorts[g_IdeClaimedCount++] = CmdPort;
}

/* ------------------------------------------------------------------ */
/* PCI configuration access                                            */
/* ------------------------------------------------------------------ */

#define IDE_PCI_HEADER_LENGTH           64
#define IDE_PCI_MULTIFUNCTION           0x80

#ifdef IDE_NT31_COMPAT
/* Configuration mechanism #1 through the I/O ports, for a HAL without PCI
   support (NT 3.1). */
#define IDE_PCI_CONFIG_ADDRESS          0xCF8
#define IDE_PCI_CONFIG_DATA             0xCFC
#define IDE_PCI_CONFIG_ENABLE           0x80000000UL

static BOOLEAN g_IdePciProbed;
static BOOLEAN g_IdePciViaPorts;

static VOID
IdePciSelect(IN ULONG Bus, IN ULONG Slot, IN ULONG Offset)
{
    PCI_SLOT_NUMBER slot;

    slot.u.AsULONG = Slot;
    ScsiPortWritePortUlong((PULONG)IDE_PCI_CONFIG_ADDRESS,
                           IDE_PCI_CONFIG_ENABLE | ((Bus & 0xFF) << 16) |
                           ((ULONG)slot.u.bits.DeviceNumber << 11) |
                           ((ULONG)slot.u.bits.FunctionNumber << 8) |
                           (Offset & 0xFC));
}

static ULONG
IdePciReadPortDword(IN ULONG Bus, IN ULONG Slot, IN ULONG Offset)
{
    IdePciSelect(Bus, Slot, Offset);
    return ScsiPortReadPortUlong((PULONG)IDE_PCI_CONFIG_DATA);
}

static BOOLEAN
IdePciReadPortHeader(IN ULONG Bus, IN ULONG Slot, OUT PPCI_COMMON_CONFIG PciConfig)
{
    PULONG dw = (PULONG)PciConfig;
    ULONG i;

    dw[0] = IdePciReadPortDword(Bus, Slot, 0);
    if ((dw[0] & 0xFFFF) == 0xFFFF || (dw[0] & 0xFFFF) == 0) return FALSE;
    for (i = 1; i < IDE_PCI_HEADER_LENGTH / sizeof(ULONG); i++) {
        dw[i] = IdePciReadPortDword(Bus, Slot, i * sizeof(ULONG));
    }
    return TRUE;
}

static VOID
IdePciProbeAccess(IN PHW_DEVICE_EXTENSION HwInit)
{
    ULONG id, save;

    if (g_IdePciProbed) return;
    g_IdePciProbed = TRUE;

    if (ScsiPortGetBusData(HwInit, PCIConfiguration, 0, 0, &id, sizeof(ULONG)) != 0) return;

    save = ScsiPortReadPortUlong((PULONG)IDE_PCI_CONFIG_ADDRESS);
    ScsiPortWritePortUlong((PULONG)IDE_PCI_CONFIG_ADDRESS, IDE_PCI_CONFIG_ENABLE);
    if (ScsiPortReadPortUlong((PULONG)IDE_PCI_CONFIG_ADDRESS) == IDE_PCI_CONFIG_ENABLE) {
        g_IdePciViaPorts = TRUE;
    }
    ScsiPortWritePortUlong((PULONG)IDE_PCI_CONFIG_ADDRESS, save);
    IDE_DBG((IDE_PFX "PCI: no HAL config access, mechanism #1 %s\n",
             g_IdePciViaPorts ? "used" : "not found"));
}
#endif /* IDE_NT31_COMPAT */

/* Configuration space of the extension's own function (timing registers,
   command register) */
ULONG
IdePciReadConfig(IN PHW_DEVICE_EXTENSION HwInit, IN ULONG Offset, IN ULONG Width)
{
    UCHAR buffer[256];
    ULONG value = 0;

    if (!HwInit->HasPci || Offset + Width > sizeof(buffer)) return 0;
#ifdef IDE_NT31_COMPAT
    if (g_IdePciViaPorts) {
        ULONG dw = IdePciReadPortDword(HwInit->PciBus, HwInit->PciSlot, Offset & ~3UL);
        dw >>= 8 * (Offset & 3);
        return (Width == 1) ? (dw & 0xFF) : (Width == 2) ? (dw & 0xFFFF) : dw;
    }
#endif
    if (ScsiPortGetBusData(HwInit, PCIConfiguration, HwInit->PciBus, HwInit->PciSlot,
                           buffer, Offset + Width) < Offset + Width) {
        return 0;
    }
    CopyMemoryBytes(&value, &buffer[Offset], Width);
    return value;
}

VOID
IdePciWriteConfig(IN PHW_DEVICE_EXTENSION HwInit, IN ULONG Offset, IN ULONG Width, IN ULONG Value)
{
    if (!HwInit->HasPci) return;
#ifdef IDE_NT31_COMPAT
    /* No ScsiPortSetBusDataByOffset to import on NT 3.1: configuration
       mechanism #1 through the ports, as the HAL does on x86 */
    IdePciSelect(HwInit->PciBus, HwInit->PciSlot, Offset & ~3UL);
    if (Width == 1) {
        IDE_OUTB(IDE_PCI_CONFIG_DATA + (Offset & 3), Value);
    } else if (Width == 2) {
        IDE_OUTW(IDE_PCI_CONFIG_DATA + (Offset & 2), Value);
    } else {
        IDE_OUTD(IDE_PCI_CONFIG_DATA, Value);
    }
#else
    ScsiPortSetBusDataByOffset(HwInit, PCIConfiguration, HwInit->PciBus, HwInit->PciSlot,
                               &Value, Offset, Width);
#endif
}

static BOOLEAN
IdeReadPciHeader(
    IN PHW_DEVICE_EXTENSION HwInit,
    IN ULONG Bus,
    IN ULONG Slot,
    OUT PPCI_COMMON_CONFIG PciConfig,
    OUT PBOOLEAN BusMissing
)
{
    ULONG bytesRead;

#ifdef IDE_NT31_COMPAT
    if (g_IdePciViaPorts) {
        if (BusMissing != NULL) *BusMissing = FALSE;
        return IdePciReadPortHeader(Bus, Slot, PciConfig);
    }
#endif
    bytesRead = ScsiPortGetBusData(HwInit, PCIConfiguration, Bus, Slot, PciConfig, IDE_PCI_HEADER_LENGTH);
    if (BusMissing != NULL) *BusMissing = (BOOLEAN)(bytesRead == 0);
    if (bytesRead < IDE_PCI_HEADER_LENGTH) return FALSE;
    if (PciConfig->VendorID == 0xFFFF || PciConfig->VendorID == 0x0000) return FALSE;
    return TRUE;
}

/* ------------------------------------------------------------------ */
/* Controller identification                                           */
/* ------------------------------------------------------------------ */

static UCHAR IdeChipKind(IN USHORT VendorId, IN USHORT DeviceId);

/* An IDE function: class 01:01, or a SATA/RAID-class controller that
   still presents IDE task files (its BARs say so: native mode set) */
static BOOLEAN
IdePciConfigIsIde(IN PPCI_COMMON_CONFIG PciConfig)
{
    if (PciConfig->BaseClass != PCI_CLASS_MASS_STORAGE) return FALSE;
    if ((PciConfig->HeaderType & 0x7F) != 0) return FALSE;
    if (PciConfig->SubClass == PCI_SUBCLASS_IDE) return TRUE;

    /* Silicon Image 3112/3512/3114 and the ATI IXP SATA functions built on
       them (SATA, class 0180 or 0104) */
    return (BOOLEAN)IDE_CHIP_IS_SII(IdeChipKind(PciConfig->VendorID, PciConfig->DeviceID));
}

/* Intel PATA controllers whose timing registers the driver programs */
static UCHAR
IdeIntelKind(IN USHORT VendorId, IN USHORT DeviceId)
{
    if (VendorId != 0x8086) return IDE_INTEL_NONE;
    switch (DeviceId) {
    case 0x7010:                                        /* PIIX3 */
    case 0x1230:                                        /* PIIX */
        return IDE_INTEL_PIIX3;
    case 0x7111:                                        /* PIIX4 */
    case 0x7199:                                        /* 82443MX */
    case 0x7601:                                        /* 82372FB */
    case 0x2421:                                        /* ICH0 */
        return IDE_INTEL_PIIX4;
    case 0x2411:                                        /* ICH */
        return IDE_INTEL_ICH66;
    case 0x244A: case 0x244B:                           /* ICH2 */
    case 0x248A: case 0x248B:                           /* ICH3 */
    case 0x24CA: case 0x24CB:                           /* ICH4 */
    case 0x24DB:                                        /* ICH5 */
    case 0x25A2:                                        /* 6300ESB */
    case 0x266F:                                        /* ICH6 */
    case 0x269E:                                        /* 631xESB/632xESB */
    case 0x27DF:                                        /* ICH7 */
    case 0x2850:                                        /* ICH8M */
        return IDE_INTEL_ICH100;
    default:
        return IDE_INTEL_NONE;
    }
}

/* Controller families the driver handles beyond SFF-8038i */
static UCHAR
IdeChipKind(IN USHORT VendorId, IN USHORT DeviceId)
{
    if (VendorId == 0x1095) {
        switch (DeviceId) {
        case 0x0643: return IDE_CHIP_CMD643;
        case 0x0646: return IDE_CHIP_CMD646;
        case 0x0648: return IDE_CHIP_CMD648;
        case 0x0649: return IDE_CHIP_CMD649;
        case 0x3112:
        case 0x0240: return IDE_CHIP_SII3112;
        case 0x3512: return IDE_CHIP_SII3512;
        case 0x3114: return IDE_CHIP_SII3114;
        case 0x0640: return IDE_CHIP_CMD640;
        default:     break;
        }
    } else if (VendorId == 0x1042 && (DeviceId == 0x1000 || DeviceId == 0x1001)) {
        return IDE_CHIP_RZ1000;
    } else if (VendorId == 0x1166) {
        if (DeviceId == 0x0211) return IDE_CHIP_OSB4;
        if (DeviceId == 0x0212 || DeviceId == 0x0213 || DeviceId == 0x0217) return IDE_CHIP_CSB;
    } else if (VendorId == 0x1002) {
        /* ATI IXP 300/400 SATA: Silicon Image 3112 cores */
        if (DeviceId == 0x436E || DeviceId == 0x4379 || DeviceId == 0x437A) return IDE_CHIP_SII3112;
    } else if (VendorId == 0x10B9 && DeviceId == 0x5229) {
        return IDE_CHIP_ALI;
    }
    return IDE_CHIP_GENERIC;
}

/* A CMD64x interrupt latch: cleared when set. TRUE when it was set (the
   interrupt came from this channel). 643/646: CFR/ARTTIM23 in config
   space; 648/649: MRDMODE at bus master base + 1. */
BOOLEAN
IdeChipClearInterrupt(IN PHW_DEVICE_EXTENSION HwInit, IN ULONG Channel)
{
    ULONG port = HwInit->FirstChannel + Channel;
    ULONG reg, mask, value;

    switch (HwInit->Chip) {
    case IDE_CHIP_CMD643:
    case IDE_CHIP_CMD646:
        reg = port ? CMD_ARTTIM23 : CMD_CFR;
        mask = port ? CMD_ARTTIM23_INTR_CH1 : CMD_CFR_INTR_CH0;
        value = IdePciReadConfig(HwInit, reg, 1);
        if (!(value & mask)) return FALSE;
        /* the latch is write-1-to-clear, the other bits go back unchanged */
        IdePciWriteConfig(HwInit, reg, 1, value);
        return TRUE;
    case IDE_CHIP_CMD648:
    case IDE_CHIP_CMD649:
        if (HwInit->CmdMrdMode == NULL) return FALSE;
        mask = port ? CMD_MRDMODE_INTR_CH1 : CMD_MRDMODE_INTR_CH0;
        value = IDE_INB(HwInit->CmdMrdMode);
        if (!(value & mask)) return FALSE;
        /* only this channel's latch: the other one may be pending */
        IDE_OUTB(HwInit->CmdMrdMode, (value & ~(CMD_MRDMODE_INTR_CH0 | CMD_MRDMODE_INTR_CH1)) | mask);
        return TRUE;
    default:
        return FALSE;
    }
}

/* Controller set-up beyond SFF-8038i; again after a power transition */
VOID
IdeChipInit(IN PHW_DEVICE_EXTENSION HwInit)
{
    static const USHORT siiSien[SII_MAX_PORTS] = { 0x148, 0x1C8, 0x348, 0x3C8 };
    static const USHORT siiFifo[SII_MAX_PORTS] = { 0x40, 0x44, 0x240, 0x244 };
    static const USHORT siiSfis[SII_MAX_PORTS] = { 0x14C, 0x1CC, 0x34C, 0x3CC };
    ULONG c, value, ports, cls;

    if (IDE_CHIP_IS_CMD(HwInit->Chip) && HwInit->PciWritable) {
        /* Interrupts not blocked, memory read line on (as Linux
           pata_cmd64x sets them up); stale latches cleared */
        value = IdePciReadConfig(HwInit, CMD_MRDMODE, 1);
        IdePciWriteConfig(HwInit, CMD_MRDMODE, 1, (value & ~CMD_MRDMODE_BLOCK) | CMD_MRDMODE_READ_LINE);
        for (c = 0; c < HwInit->ChannelCount; c++) IdeChipClearInterrupt(HwInit, c);
    }

    if (IDE_CHIP_IS_SII(HwInit->Chip) && HwInit->SiiMmio != NULL) {
        ports = (HwInit->SiiMmioLength >= SII_MMIO_LENGTH_3114) ? 4 : 2;
        cls = IdePciReadConfig(HwInit, 0x0C, 1);            /* cache line size, dwords */
        for (c = 0; c < ports; c++) {
            /* SATA (PHY, hot-plug) interrupts off: nothing here would
               acknowledge them, and the PCI line would stay asserted */
            ScsiPortWriteRegisterUlong((PULONG)(HwInit->SiiMmio + siiSien[c]), 0);
            /* FIFO PCI bus arbitration thresholds from the cache line size */
            if (cls != 0) {
                value = (cls >> 3) + 1;
                ScsiPortWriteRegisterUshort((PUSHORT)(HwInit->SiiMmio + siiFifo[c]), (USHORT)((value << 8) | value));
            }
            /* 3512/3114: "R_ERR on DMA activate FIS" errata */
            if (HwInit->Chip != IDE_CHIP_SII3112) {
                value = ScsiPortReadRegisterUlong((PULONG)(HwInit->SiiMmio + siiSfis[c]));
                if ((value & 0x3) == 0x1) {
                    ScsiPortWriteRegisterUlong((PULONG)(HwInit->SiiMmio + siiSfis[c]), value & ~0x3UL);
                }
            }
        }
    }
}

/* Intel 450NX servers: PIIX4 DMA can hang with the original 82454NX PXB,
   and with revisions below 5 when PXB bus lock is on (Linux ata_piix) */
static BOOLEAN
IdeIntel450nxErrata(IN PHW_DEVICE_EXTENSION HwInit)
{
    ULONG saveBus = HwInit->PciBus, saveSlot = HwInit->PciSlot;
    ULONG bus, dev, id, rev, cfg;
    PCI_SLOT_NUMBER slot;
    BOOLEAN errata = FALSE;

    for (bus = 0; bus < 4 && !errata; bus++) {
        for (dev = 0; dev < 32 && !errata; dev++) {
            slot.u.AsULONG = 0;
            slot.u.bits.DeviceNumber = dev;
            HwInit->PciBus = bus;
            HwInit->PciSlot = slot.u.AsULONG;
            id = IdePciReadConfig(HwInit, 0x00, 4);
            if (id != 0x84CB8086) continue;
            rev = IdePciReadConfig(HwInit, 0x08, 1);
            cfg = IdePciReadConfig(HwInit, 0x41, 2);
            if (rev == 0x00 || ((cfg & (1UL << 14)) && rev < 5)) errata = TRUE;
        }
    }
    HwInit->PciBus = saveBus;
    HwInit->PciSlot = saveSlot;
    return errata;
}

/* Family of the PCI function and what it needs: called once the channels
   are attached */
static BOOLEAN
IdeChipSetup(IN PHW_DEVICE_EXTENSION HwInit, IN PPCI_COMMON_CONFIG PciConfig,
             IN INTERFACE_TYPE BusType, IN ULONG BusNumber)
{
    SCSI_PHYSICAL_ADDRESS pa;
    ULONG bar, length, value;

    HwInit->Chip = IdeChipKind(PciConfig->VendorID, PciConfig->DeviceID);
    HwInit->Revision = PciConfig->RevisionID;
    pa.HighPart = 0;

    switch (HwInit->Chip) {
    case IDE_CHIP_CMD648:
    case IDE_CHIP_CMD649:
        bar = PciConfig->u.type0.BaseAddresses[IDE_BAR_BUS_MASTER];
        if ((bar & 1) && (bar & 0xFFFC) != 0) {
            pa.LowPart = (bar & 0xFFFC) + 1;
            HwInit->CmdMrdMode = (PUCHAR)ScsiPortGetDeviceBase(HwInit, BusType, BusNumber, pa, 1, TRUE);
        }
        break;
    case IDE_CHIP_CMD640:
        /* Linux pata_cmd640: read-ahead off on all four drives (its data
           FIFO corrupts data), PIO 0 command timing, 512-byte bursts */
        IdePciWriteConfig(HwInit, 0x5B, 1, 0x00);
        IdePciWriteConfig(HwInit, 0x52, 1, 0x00);
        IdePciWriteConfig(HwInit, 0x59, 1, 0x40);
        value = IdePciReadConfig(HwInit, 0x51, 1);
        IdePciWriteConfig(HwInit, 0x51, 1, value | 0xC0);
        value = IdePciReadConfig(HwInit, 0x57, 1);
        IdePciWriteConfig(HwInit, 0x57, 1, value | 0x0C);
        IDE_DBG((IDE_PFX "FindAdapter: CMD640, read-ahead off, channels serialized\n"));
        break;
    case IDE_CHIP_RZ1000:
        /* Linux pata_rz1000: the read-ahead FIFO corrupts data; it must be
           off, or the controller is not driven at all */
        value = IdePciReadConfig(HwInit, 0x40, 2);
        IdePciWriteConfig(HwInit, 0x40, 2, value & 0xDFFF);
        value = IdePciReadConfig(HwInit, 0x40, 2);
        if (value & 0x2000) {
            IDE_DBG((IDE_PFX "FindAdapter: RZ1000 read-ahead still on, controller left alone\n"));
            return FALSE;
        }
        IDE_DBG((IDE_PFX "FindAdapter: RZ1000, read-ahead off, channels serialized\n"));
        break;
    case IDE_CHIP_ALI:
        /* Linux pata_ali: DMA from revision 20h, 48-bit DMA from C5h,
           ATAPI DMA not reliable */
        if (HwInit->Revision < 0x20) HwInit->DmaAllowed = FALSE;
        if (HwInit->Revision < 0xC5) HwInit->NoLba48Dma = TRUE;
        HwInit->NoAtapiDma = TRUE;
        break;
    case IDE_CHIP_SII3112:
    case IDE_CHIP_SII3512:
    case IDE_CHIP_SII3114:
        bar = PciConfig->u.type0.BaseAddresses[SII_MMIO_BAR];
        length = (HwInit->Chip == IDE_CHIP_SII3114) ? SII_MMIO_LENGTH_3114 : SII_MMIO_LENGTH;
        if (!(bar & 1) && (bar & 0xFFFFFFF0) != 0) {
            pa.LowPart = bar & 0xFFFFFFF0;
#ifdef IDE_W9X
            HwInit->SiiMmio = (PUCHAR)IdeMapPhysToLinear(pa.LowPart, length);
            if (HwInit->SiiMmio == (PUCHAR)0xFFFFFFFF) HwInit->SiiMmio = NULL;
#else
            HwInit->SiiMmio = (PUCHAR)ScsiPortGetDeviceBase(HwInit, BusType, BusNumber, pa, length, FALSE);
            if (HwInit->SiiMmio == NULL && BusType != PCIBus) {
                HwInit->SiiMmio = (PUCHAR)ScsiPortGetDeviceBase(HwInit, PCIBus, HwInit->PciBus, pa, length, FALSE);
            }
#endif
            if (HwInit->SiiMmio != NULL) HwInit->SiiMmioLength = length;
        }
        IDE_DBG((IDE_PFX "FindAdapter: Silicon Image registers at %08lX %s\n", pa.LowPart,
                 HwInit->SiiMmio != NULL ? "mapped" : "not mapped, timing as the BIOS set it"));
        break;
    default:
        break;
    }
    if (HwInit->Intel == IDE_INTEL_PIIX4 && HwInit->DmaAllowed && IdeIntel450nxErrata(HwInit)) {
        IDE_DBG((IDE_PFX "FindAdapter: 450NX errata, DMA off\n"));
        HwInit->DmaAllowed = FALSE;
    }
    IdeChipInit(HwInit);
    return TRUE;
}

/* ------------------------------------------------------------------ */
/* Channel descriptions                                                */
/* ------------------------------------------------------------------ */

typedef struct _IDE_CHANNEL_DESC {
    USHORT  CmdPort;
    USHORT  CtlPort;
    USHORT  BmPort;         /* 0 without bus master */
    UCHAR   Irq;            /* 0 unknown */
    BOOLEAN Native;
    UCHAR   Index;          /* 0 primary, 1 secondary of its controller */
} IDE_CHANNEL_DESC, *PIDE_CHANNEL_DESC;

static const struct { USHORT Cmd; USHORT Ctl; UCHAR Irq; } g_IdeLegacy[IDE_LEGACY_CHANNELS] = {
    { IDE_LEGACY_PRIMARY_CMD,    IDE_LEGACY_PRIMARY_CTL,    IDE_LEGACY_PRIMARY_IRQ },
    { IDE_LEGACY_SECONDARY_CMD,  IDE_LEGACY_SECONDARY_CTL,  IDE_LEGACY_SECONDARY_IRQ },
    { IDE_LEGACY_TERTIARY_CMD,   IDE_LEGACY_TERTIARY_CTL,   IDE_LEGACY_TERTIARY_IRQ },
    { IDE_LEGACY_QUATERNARY_CMD, IDE_LEGACY_QUATERNARY_CTL, IDE_LEGACY_QUATERNARY_IRQ },
};

/* The two channels of a PCI IDE function, from its class code and BARs */
static ULONG
IdeDescribePciChannels(IN PPCI_COMMON_CONFIG PciConfig, OUT PIDE_CHANNEL_DESC Desc)
{
    ULONG bar4 = PciConfig->u.type0.BaseAddresses[IDE_BAR_BUS_MASTER];
    BOOLEAN sataClass = (BOOLEAN)(PciConfig->SubClass != PCI_SUBCLASS_IDE);
    /* The programming interface tells of a bus master only in class 0101;
       the SATA/RAID-class functions taken here have theirs at BAR 4 */
    USHORT bm = (USHORT)(((bar4 & 1) && (bar4 & 0xFFFC) != 0 &&
                          ((PciConfig->ProgIf & IDE_PROGIF_BUS_MASTER) || sataClass)) ? (bar4 & 0xFFFC) : 0);
    ULONG n = 0, c;

    for (c = 0; c < 2; c++) {
        UCHAR nativeBit = (UCHAR)(c ? IDE_PROGIF_SECONDARY_NATIVE : IDE_PROGIF_PRIMARY_NATIVE);
        ULONG barCmd = PciConfig->u.type0.BaseAddresses[c ? IDE_BAR_SECONDARY_CMD : IDE_BAR_PRIMARY_CMD];
        ULONG barCtl = PciConfig->u.type0.BaseAddresses[c ? IDE_BAR_SECONDARY_CTL : IDE_BAR_PRIMARY_CTL];
        PIDE_CHANNEL_DESC d = &Desc[n];

        ZeroMemoryBytes(d, sizeof(*d));
        d->Index = (UCHAR)c;
        if ((PciConfig->ProgIf & nativeBit) || sataClass) {
            if (!(barCmd & 1) || !(barCtl & 1) || (barCmd & 0xFFF8) == 0 || (barCtl & 0xFFFC) == 0) continue;
            d->CmdPort = (USHORT)(barCmd & 0xFFF8);
            d->CtlPort = (USHORT)((barCtl & 0xFFFC) + 2);
            d->Irq = (UCHAR)((PciConfig->u.type0.InterruptLine == 0xFF) ? 0 : PciConfig->u.type0.InterruptLine);
            d->Native = TRUE;
        } else {
            d->CmdPort = g_IdeLegacy[c].Cmd;
            d->CtlPort = g_IdeLegacy[c].Ctl;
            d->Irq = g_IdeLegacy[c].Irq;
            d->Native = FALSE;
        }
        d->BmPort = (USHORT)(bm ? bm + 8 * c : 0);
        n++;
    }
    return n;
}

/* Walk the PCI buses for the next IDE function with an unclaimed channel */
static BOOLEAN
IdeScanPci(
    IN PHW_DEVICE_EXTENSION HwInit,
    OUT PULONG FoundBus,
    OUT PULONG FoundSlot,
    OUT PPCI_COMMON_CONFIG PciConfig
)
{
    PCI_SLOT_NUMBER slot;
    ULONG bus, device, function, n, i;
    BOOLEAN busMissing, multiFunction;
    IDE_CHANNEL_DESC desc[2];

    for (bus = 0; bus < 256; bus++) {
        for (device = 0; device < 32; device++) {
            multiFunction = FALSE;
            for (function = 0; function < 8; function++) {
                if (function > 0 && !multiFunction) break;
                slot.u.AsULONG = 0;
                slot.u.bits.DeviceNumber = device;
                slot.u.bits.FunctionNumber = function;

                if (!IdeReadPciHeader(HwInit, bus, slot.u.AsULONG, PciConfig, &busMissing)) {
                    if (busMissing && device == 0 && function == 0) return FALSE;
                    if (function == 0) break;
                    continue;
                }
                if (function == 0) {
                    multiFunction = (BOOLEAN)((PciConfig->HeaderType & IDE_PCI_MULTIFUNCTION) != 0);
                }
                if (!IdePciConfigIsIde(PciConfig)) continue;

                n = IdeDescribePciChannels(PciConfig, desc);
                for (i = 0; i < n; i++) {
                    if (!IdeIsPortClaimed(desc[i].CmdPort)) {
                        *FoundBus = bus;
                        *FoundSlot = slot.u.AsULONG;
                        return TRUE;
                    }
                }
            }
        }
    }
    return FALSE;
}

#if !defined(IDE_NT4)
/* The PCI IDE function whose compatibility-mode channel sits at CmdPort */
static BOOLEAN
IdeFindCompatFunction(
    IN PHW_DEVICE_EXTENSION HwInit,
    IN USHORT CmdPort,
    OUT PULONG FoundBus,
    OUT PULONG FoundSlot,
    OUT PPCI_COMMON_CONFIG PciConfig
)
{
    PCI_SLOT_NUMBER slot;
    IDE_CHANNEL_DESC desc[2];
    ULONG bus, device, function, n, i;
    BOOLEAN busMissing, multiFunction;

    for (bus = 0; bus < 256; bus++) {
        for (device = 0; device < 32; device++) {
            multiFunction = FALSE;
            for (function = 0; function < 8; function++) {
                if (function > 0 && !multiFunction) break;
                slot.u.AsULONG = 0;
                slot.u.bits.DeviceNumber = device;
                slot.u.bits.FunctionNumber = function;
                if (!IdeReadPciHeader(HwInit, bus, slot.u.AsULONG, PciConfig, &busMissing)) {
                    if (busMissing && device == 0 && function == 0) return FALSE;
                    if (function == 0) break;
                    continue;
                }
                if (function == 0) multiFunction = (BOOLEAN)((PciConfig->HeaderType & IDE_PCI_MULTIFUNCTION) != 0);
                if (!IdePciConfigIsIde(PciConfig)) continue;
                n = IdeDescribePciChannels(PciConfig, desc);
                for (i = 0; i < n; i++) {
                    if (!desc[i].Native && desc[i].CmdPort == CmdPort) {
                        *FoundBus = bus;
                        *FoundSlot = slot.u.AsULONG;
                        return TRUE;
                    }
                }
            }
        }
    }
    return FALSE;
}
#endif

/* Anything answering on a legacy channel? A floating bus reads 0xFF on
   both ports; a device drives the status and the count register. */
static BOOLEAN
IdeLegacyChannelPresent(IN USHORT CmdPort, IN USHORT CtlPort)
{
    UCHAR s1, s2, c;

    s1 = IDE_INB(CtlPort);
    s2 = IDE_INB(CmdPort + IDE_REG_STATUS);
    if (s1 == 0xFF && s2 == 0xFF) return FALSE;
    if (s1 & IDE_STS_BSY) return TRUE;              /* busy device, decided later */
    IDE_OUTB(CmdPort + IDE_REG_DEVICE, IDE_DEV_DEFAULT);
    ScsiPortStallExecution(2);
    IDE_OUTB(CmdPort + IDE_REG_COUNT, 0x55);
    IDE_OUTB(CmdPort + IDE_REG_LBA0, 0xAA);
    IDE_OUTB(CmdPort + IDE_REG_COUNT, 0xAA);
    IDE_OUTB(CmdPort + IDE_REG_LBA0, 0x55);
    IDE_OUTB(CmdPort + IDE_REG_COUNT, 0x55);
    IDE_OUTB(CmdPort + IDE_REG_LBA0, 0xAA);
    c = IDE_INB(CmdPort + IDE_REG_COUNT);
    if (c != 0x55) return FALSE;
    c = IDE_INB(CmdPort + IDE_REG_LBA0);
    return (BOOLEAN)(c == 0xAA);
}

/* ------------------------------------------------------------------ */
/* DMA arena: PRD table and bounce buffer per channel                  */
/* ------------------------------------------------------------------ */

#define IDE_CHANNEL_DMA_BYTES           (IDE_PRD_TABLE_SIZE + IDE_BOUNCE_SIZE)

static BOOLEAN
IdeAllocateDma(IN PHW_DEVICE_EXTENSION HwInit, IN PPORT_CONFIGURATION_INFORMATION ConfigInfo)
{
    ULONG length, phys, misalign, c;
    PUCHAR virt;

    if (HwInit->DmaArea != NULL) return TRUE;

    HwInit->DmaLength = IDE_MAX_CHANNELS * IDE_CHANNEL_DMA_BYTES + IDE_PRD_TABLE_SIZE;
    HwInit->DmaArea = (PUCHAR)ScsiPortGetUncachedExtension(HwInit, ConfigInfo, HwInit->DmaLength);
    if (HwInit->DmaArea == NULL) {
        IDE_DBG((IDE_PFX "AllocateDma: ScsiPortGetUncachedExtension(%lu) failed\n", HwInit->DmaLength));
        return FALSE;
    }
    length = HwInit->DmaLength;
    HwInit->DmaAreaPhysical = ScsiPortGetPhysicalAddress(HwInit, NULL, HwInit->DmaArea, &length);
    if (HwInit->DmaAreaPhysical.LowPart == 0 && HwInit->DmaAreaPhysical.HighPart == 0) {
        IDE_DBG((IDE_PFX "AllocateDma: no physical address\n"));
        return FALSE;
    }
    if (HwInit->DmaAreaPhysical.HighPart != 0) {
        IDE_DBG((IDE_PFX "AllocateDma: area above 4 GB\n"));
        return FALSE;
    }
    ZeroMemoryBytes(HwInit->DmaArea, HwInit->DmaLength);

    virt = HwInit->DmaArea;
    phys = HwInit->DmaAreaPhysical.LowPart;
    misalign = phys % IDE_PRD_TABLE_SIZE;
    if (misalign != 0) {
        virt += IDE_PRD_TABLE_SIZE - misalign;
        phys += IDE_PRD_TABLE_SIZE - misalign;
    }
    for (c = 0; c < IDE_MAX_CHANNELS; c++) {
        PIDE_CHANNEL ch = &HwInit->Channels[c];
        ch->Prd = (PIDE_PRD_ENTRY)virt;
        ch->PrdPhysical = phys;
        virt += IDE_PRD_TABLE_SIZE; phys += IDE_PRD_TABLE_SIZE;
        ch->BounceBuffer = virt;
        ch->BouncePhysical = phys;
        virt += IDE_BOUNCE_SIZE; phys += IDE_BOUNCE_SIZE;
    }
    return TRUE;
}

/* ------------------------------------------------------------------ */
/* Channel bring-up                                                    */
/* ------------------------------------------------------------------ */

/* IDENTIFY, parse, transfer mode and settings for one unit */
static VOID
IdeSetupUnit(IN PHW_DEVICE_EXTENSION HwInit, IN ULONG Channel, IN ULONG Unit)
{
    PIDE_DEVICE dev = &HwInit->Channels[Channel].Devices[Unit];

    if (!dev->Present) return;
    if (IdeIdentifyDevice(HwInit, Channel, Unit)) {
        IdeParseIdentify(HwInit, Channel, Unit);
        IdeSelectTransferMode(HwInit, Channel, Unit);
    } else {
        IdeParseIdentify(HwInit, Channel, Unit);
    }
    IdeApplyDeviceSettings(HwInit, Channel, Unit);
}

/* After a reset: modes and settings of every device on the channel */
VOID
IdeSetupChannel(IN PHW_DEVICE_EXTENSION HwInit, IN ULONG Channel)
{
    PIDE_CHANNEL ch = &HwInit->Channels[Channel];
    ULONG unit;

    ch->NeedsSetup = FALSE;
    for (unit = 0; unit < IDE_DEVICES_PER_CHANNEL; unit++) {
        if (ch->Devices[unit].Present) IdeSetupUnit(HwInit, Channel, unit);
    }
    IdeSetControl(ch, IDE_CTL_NIEN);
}

/* Reset the channel and find its devices */
static VOID
IdeInitializeChannel(IN PHW_DEVICE_EXTENSION HwInit, IN ULONG Channel)
{
    PIDE_CHANNEL ch = &HwInit->Channels[Channel];
    ULONG unit;
    UCHAR status, lba1, lba2, c, n;

    ch->Outstanding = NULL;
    ch->Pending = NULL;
    ch->DeviceCount = 0;

    IdeResetChannel(HwInit, Channel);

    for (unit = 0; unit < IDE_DEVICES_PER_CHANNEL; unit++) {
        PIDE_DEVICE dev = &ch->Devices[unit];
        UCHAR wantWc = dev->WantWriteCache, wantLa = dev->WantLookAhead, dmaErrors = dev->DmaErrors;

        ZeroMemoryBytes(dev, sizeof(IDE_DEVICE));
        dev->Unit = (UCHAR)unit;
        dev->WantWriteCache = wantWc;
        dev->WantLookAhead = wantLa;
        dev->DmaErrors = dmaErrors;

        IdeSelectUnit(ch, unit);
        status = IdeAltStatus(ch);
        lba1 = IDE_INB(ch->Cmd + IDE_REG_LBA1);
        lba2 = IDE_INB(ch->Cmd + IDE_REG_LBA2);
        dev->IsAtapi = (BOOLEAN)(lba1 == IDE_SIG_ATAPI_LBA1 && lba2 == IDE_SIG_ATAPI_LBA2);
        /* Floating bus; or nothing at all (a packet device leaves DRDY
           clear after a reset, so only its signature tells it apart) */
        if (status == 0xFF || (status == 0x00 && !dev->IsAtapi)) continue;

        /* A device: it answers in its registers (a master shadows an
           absent slave only until the status is read) */
        IDE_OUTB(ch->Cmd + IDE_REG_COUNT, 0x55);
        IDE_OUTB(ch->Cmd + IDE_REG_LBA0, 0xAA);
        c = IDE_INB(ch->Cmd + IDE_REG_COUNT);
        n = IDE_INB(ch->Cmd + IDE_REG_LBA0);
        if (c != 0x55 || n != 0xAA) {
            if (!IdeWaitStatus(ch, IDE_STS_BSY, 0, 1000)) continue;
        }
        dev->Present = TRUE;

        if (!IdeIdentifyDevice(HwInit, Channel, unit)) {
            /* No or a stale signature: the other IDENTIFY once */
            dev->IsAtapi = (BOOLEAN)!dev->IsAtapi;
            if (!IdeIdentifyDevice(HwInit, Channel, unit)) {
                dev->Present = FALSE;
                IDE_DBG((IDE_PFX "Channel %lu.%lu: no answer (status %02lX)\n", Channel, unit, (ULONG)status));
                continue;
            }
        }
        IdeParseIdentify(HwInit, Channel, unit);
        IdeSelectTransferMode(HwInit, Channel, unit);
        IdeApplyDeviceSettings(HwInit, Channel, unit);
        ch->DeviceCount++;
    }
    ch->NeedsSetup = FALSE;
    IdeSetControl(ch, IDE_CTL_NIEN);
    IDE_DBG((IDE_PFX "Channel %lu: %lu device(s), %s\n", Channel, (ULONG)ch->DeviceCount,
             ch->Async ? "interrupt" : "Fast Polling"));
}

static VOID
IdeInitializeAll(IN PHW_DEVICE_EXTENSION HwInit)
{
    ULONG c;

    for (c = 0; c < HwInit->ChannelCount; c++) {
        if (HwInit->Channels[c].Present) IdeInitializeChannel(HwInit, c);
    }
    HwInit->Stopped = FALSE;
    HwInit->CheckState = FALSE;
}

/* After a power transition or a BIOS that used the controller: set
   everything up again. MODE SELECT settings are kept per device. */
BOOLEAN
IdeReinitialize(IN PHW_DEVICE_EXTENSION HwInit)
{
    ULONG c;

    IDE_DBG((IDE_PFX "Reinitialize\n"));
    for (c = 0; c < HwInit->ChannelCount; c++) {
        if (HwInit->Channels[c].Outstanding != NULL || HwInit->Channels[c].Pending != NULL) {
            IdeFailOutstanding(HwInit, c, SRB_STATUS_BUS_RESET);
        }
    }
    IdeChipInit(HwInit);
    IdeInitializeAll(HwInit);
    return TRUE;
}

/* The devices may have been reset behind the driver's back (APM resume,
   a BIOS call): put the transfer modes and settings back. */
VOID
IdeCheckState(IN PHW_DEVICE_EXTENSION HwInit)
{
    ULONG c, unit;

    if (HwInit->Stopped) {
        IdeReinitialize(HwInit);
        return;
    }
    if (!HwInit->CheckState) return;
    HwInit->CheckState = FALSE;
    for (c = 0; c < HwInit->ChannelCount; c++) {
        PIDE_CHANNEL ch = &HwInit->Channels[c];
        if (!ch->Present) continue;
        IdeDrainChannel(HwInit, c);
        for (unit = 0; unit < IDE_DEVICES_PER_CHANNEL; unit++) {
            if (ch->Devices[unit].Present) IdeSetupUnit(HwInit, c, unit);
        }
        IdeSetControl(ch, IDE_CTL_NIEN);
    }
}

/* Flush every disk, put it in standby, stop the channels. The next
   request or POWER_UP brings everything back. */
BOOLEAN
IdePowerDown(IN PHW_DEVICE_EXTENSION HwInit)
{
    ULONG c, unit;

    IDE_DBG((IDE_PFX "PowerDown\n"));
    for (c = 0; c < HwInit->ChannelCount; c++) {
        PIDE_CHANNEL ch = &HwInit->Channels[c];
        if (!ch->Present) continue;
        IdeDrainChannel(HwInit, c);
        for (unit = 0; unit < IDE_DEVICES_PER_CHANNEL; unit++) {
            PIDE_DEVICE dev = &ch->Devices[unit];
            if (!dev->Present || dev->IsAtapi) continue;
            IdeFlushDevice(HwInit, c, unit);
            IdeRunSimple(HwInit, c, unit, IDE_COMMAND_STANDBY_IMMEDIATE, 0, 0, IDE_POLL_TIMEOUT_POWER_SEC);
        }
        IdeStopChannel(HwInit, c);
    }
    HwInit->Stopped = TRUE;
    return TRUE;
}

/* ------------------------------------------------------------------ */
/* Interrupt watchdog                                                  */
/* ------------------------------------------------------------------ */

VOID
IdeArmTimer(IN PHW_DEVICE_EXTENSION HwInit)
{
    if (HwInit->TimerArmed || !HwInit->AsyncMode) return;
    HwInit->TimerArmed = TRUE;
    ScsiPortNotification(RequestTimerCall, HwInit, IdeTimer, IDE_TIMER_USEC);
}

/* Back to Fast Polling for good */
static VOID
IdeDisableAsync(IN PHW_DEVICE_EXTENSION HwInit)
{
    ULONG c;

    IDE_DBG((IDE_PFX "Interrupts not delivered, back to Fast Polling\n"));
    for (c = 0; c < HwInit->ChannelCount; c++) {
        if (HwInit->Channels[c].Present) IdeDrainChannel(HwInit, c);
        HwInit->Channels[c].Async = FALSE;
    }
    HwInit->AsyncMode = FALSE;
}

VOID
IdeTimer(IN PVOID DeviceExtension)
{
    PHW_DEVICE_EXTENSION hwInit = (PHW_DEVICE_EXTENSION)DeviceExtension;
    ULONG c, missed = 0;
    BOOLEAN pending = FALSE;

    hwInit->TimerArmed = FALSE;
    if (!hwInit->AsyncMode || hwInit->Stopped) return;

    for (c = 0; c < hwInit->ChannelCount; c++) {
        if (hwInit->Channels[c].Outstanding == NULL) continue;
        missed += IdeServiceChannel(hwInit, c, FALSE);
        if (hwInit->Channels[c].Outstanding != NULL) pending = TRUE;
    }
    if (missed != 0 && ++hwInit->MissedInterrupts >= IDE_MISSED_IRQ_LIMIT) {
        IdeDisableAsync(hwInit);
        return;
    }
    if (pending) IdeArmTimer(hwInit);
}

/* ------------------------------------------------------------------ */
/* DriverEntry                                                         */
/* ------------------------------------------------------------------ */

#ifdef IDE_NT31_COMPAT
static BOOLEAN g_IdeFindAdapterCalled;
#endif

#if !defined(IDE_W9X)
/* Registry access for the NT family. Declared here: miniport.h has no
   registry API. */
typedef struct _IDE_UNICODE_STRING {
    USHORT Length;
    USHORT MaximumLength;
    USHORT *Buffer;
} IDE_UNICODE_STRING, *PIDE_UNICODE_STRING;

typedef struct _IDE_OBJECT_ATTRIBUTES {
    ULONG Length;
    PVOID RootDirectory;
    PIDE_UNICODE_STRING ObjectName;
    ULONG Attributes;
    PVOID SecurityDescriptor;
    PVOID SecurityQualityOfService;
} IDE_OBJECT_ATTRIBUTES;

LONG __stdcall ZwOpenKey(PVOID *KeyHandle, ULONG DesiredAccess, PVOID ObjectAttributes);
LONG __stdcall ZwCreateKey(PVOID *KeyHandle, ULONG DesiredAccess, PVOID ObjectAttributes,
                           ULONG TitleIndex, PVOID Class, ULONG CreateOptions, PULONG Disposition);
LONG __stdcall ZwSetValueKey(PVOID KeyHandle, PVOID ValueName, ULONG TitleIndex, ULONG Type,
                             PVOID Data, ULONG DataSize);
LONG __stdcall ZwQueryValueKey(PVOID KeyHandle, PVOID ValueName, ULONG KeyValueInformationClass,
                               PVOID KeyValueInformation, ULONG Length, PULONG ResultLength);
LONG __stdcall ZwClose(PVOID Handle);

#define IDE_OBJ_CASE_INSENSITIVE        0x00000040
#define IDE_OBJ_KERNEL_HANDLE           0x00000200
#define IDE_KEY_READ_WRITE              0x0002001F
#define IDE_REG_CREATED_NEW_KEY         1
#define IDE_REG_DWORD                   4

static VOID
IdeInitString(OUT PIDE_UNICODE_STRING String, IN USHORT *Text, IN ULONG Bytes)
{
    String->Buffer = Text;
    String->Length = (USHORT)(Bytes - sizeof(USHORT));
    String->MaximumLength = (USHORT)Bytes;
}

static VOID
IdeInitAttributes(OUT IDE_OBJECT_ATTRIBUTES *Oa, IN PIDE_UNICODE_STRING Name, IN PVOID Root)
{
    Oa->Length = sizeof(IDE_OBJECT_ATTRIBUTES);
    Oa->RootDirectory = Root;
    Oa->ObjectName = Name;
#ifdef IDE_NT4
    /* OBJ_KERNEL_HANDLE is 2000 and later; NT 4.0 rejects it */
    Oa->Attributes = IDE_OBJ_CASE_INSENSITIVE;
#else
    Oa->Attributes = IDE_OBJ_CASE_INSENSITIVE | IDE_OBJ_KERNEL_HANDLE;
#endif
    Oa->SecurityDescriptor = NULL;
    Oa->SecurityQualityOfService = NULL;
}
#endif

#if !defined(IDE_NT4)
/* Text-mode setup boots with a bare service key: the [Config] values of
   txtsetup.oem reach only the installed system. Without
   Parameters\PnpInterface the x64 ScsiPort fails IRP_MN_START_DEVICE
   before HwFindAdapter (STOP 0x7B), so the driver adds the entry the INF
   would have made. */
static USHORT g_IdeParametersName[] = L"Parameters";
static USHORT g_IdePnpInterfaceName[] = L"PnpInterface";
static USHORT g_IdePciBusValueName[] = L"5";

static VOID
IdeEnsurePnpInterface(IN PVOID RegistryPath)
{
    IDE_OBJECT_ATTRIBUTES oa;
    IDE_UNICODE_STRING name;
    PVOID service, params, pnp;
    ULONG disposition = 0;
    ULONG one = 1;
    LONG status;

    if (RegistryPath == NULL) return;

    IdeInitAttributes(&oa, (PIDE_UNICODE_STRING)RegistryPath, NULL);
    if (ZwOpenKey(&service, IDE_KEY_READ_WRITE, &oa) < 0) return;

    IdeInitString(&name, g_IdeParametersName, sizeof(g_IdeParametersName));
    IdeInitAttributes(&oa, &name, service);
    status = ZwCreateKey(&params, IDE_KEY_READ_WRITE, &oa, 0, NULL, 0, &disposition);
    if (status >= 0) {
        IdeInitString(&name, g_IdePnpInterfaceName, sizeof(g_IdePnpInterfaceName));
        IdeInitAttributes(&oa, &name, params);
        status = ZwCreateKey(&pnp, IDE_KEY_READ_WRITE, &oa, 0, NULL, 0, &disposition);
        if (status >= 0) {
            if (disposition == IDE_REG_CREATED_NEW_KEY) {
                IdeInitString(&name, g_IdePciBusValueName, sizeof(g_IdePciBusValueName));
                status = ZwSetValueKey(pnp, &name, 0, IDE_REG_DWORD, &one, sizeof(one));
                IDE_DBG((IDE_PFX "DriverEntry: Parameters\\PnpInterface\\5 added (%08lX)\n", (ULONG)status));
            }
            ZwClose(pnp);
        }
        ZwClose(params);
    }
    ZwClose(service);
}
#endif

#if defined(IDE_NT4) && !defined(IDE_W9X)
/* atdisk (Primary disk group) and atapi (an earlier tag) load before IDENT
   and take the channels. oemsetup.inf turns them off; a text-mode setup
   cannot, as txtsetup.oem reaches only IDENT's own key. So once IDENT has
   found a controller, the driver does what oemsetup.inf does: the old
   Start is kept as InboxStart_<name> under the IDENT key (oemsetup.inf
   puts it back when IDENT is removed) and Start set to 4. A driver with a
   saved value is left alone, so one turned on again by hand stays on.
   When a driver is turned off, the disk class driver is made a boot
   driver, as IDENT's disks need it at boot. */
#define IDE_KEY_VALUE_PARTIAL_INFORMATION   2

static USHORT g_IdeStartName[] = L"Start";
static USHORT g_IdeAtdiskName[] = L"atdisk";
static USHORT g_IdeAtapiName[] = L"atapi";
static USHORT g_IdeAtdiskSaved[] = L"InboxStart_atdisk";
static USHORT g_IdeAtapiSaved[] = L"InboxStart_atapi";
static USHORT g_IdeScsidiskName[] = L"scsidisk";
static USHORT g_IdeDiskName[] = L"disk";

static BOOLEAN g_IdeAdapterFound;

static PVOID
IdeRegOpen(IN PVOID Root, IN USHORT *Name, IN ULONG Bytes)
{
    IDE_OBJECT_ATTRIBUTES oa;
    IDE_UNICODE_STRING name;
    PVOID key;

    IdeInitString(&name, Name, Bytes);
    IdeInitAttributes(&oa, &name, Root);
    if (ZwOpenKey(&key, IDE_KEY_READ_WRITE, &oa) < 0) return NULL;
    return key;
}

static BOOLEAN
IdeRegReadDword(IN PVOID Key, IN USHORT *Name, IN ULONG Bytes, OUT PULONG Value)
{
    IDE_UNICODE_STRING name;
    ULONG info[4], length = 0;

    IdeInitString(&name, Name, Bytes);
    /* TitleIndex, Type, DataLength, Data */
    if (ZwQueryValueKey(Key, &name, IDE_KEY_VALUE_PARTIAL_INFORMATION, info, sizeof(info), &length) < 0 ||
        info[1] != IDE_REG_DWORD || info[2] != sizeof(ULONG)) {
        return FALSE;
    }
    *Value = info[3];
    return TRUE;
}

static VOID
IdeRegWriteDword(IN PVOID Key, IN USHORT *Name, IN ULONG Bytes, IN ULONG Value)
{
    IDE_UNICODE_STRING name;

    IdeInitString(&name, Name, Bytes);
    ZwSetValueKey(Key, &name, 0, IDE_REG_DWORD, &Value, sizeof(Value));
}

static VOID
IdeDisableInboxDrivers(IN PVOID RegistryPath)
{
    static USHORT *const inbox[2] = { g_IdeAtdiskName, g_IdeAtapiName };
    static const ULONG inboxBytes[2] = { sizeof(g_IdeAtdiskName), sizeof(g_IdeAtapiName) };
    static USHORT *const saved[2] = { g_IdeAtdiskSaved, g_IdeAtapiSaved };
    static const ULONG savedBytes[2] = { sizeof(g_IdeAtdiskSaved), sizeof(g_IdeAtapiSaved) };
    static USHORT *const cls[2] = { g_IdeScsidiskName, g_IdeDiskName };
    static const ULONG clsBytes[2] = { sizeof(g_IdeScsidiskName), sizeof(g_IdeDiskName) };
    static const PCHAR inboxText[2] = { "atdisk", "atapi" };
    static const PCHAR clsText[2] = { "scsidisk", "disk" };
    PIDE_UNICODE_STRING path = (PIDE_UNICODE_STRING)RegistryPath;
    IDE_OBJECT_ATTRIBUTES oa;
    IDE_UNICODE_STRING parent;
    PVOID ident, services, key;
    ULONG i, n, start, old;
    LONG status;
    BOOLEAN changed = FALSE;

    if (path == NULL || path->Buffer == NULL) return;
    /* The Services key: the registry path without its last component */
    n = path->Length / sizeof(USHORT);
    while (n > 0 && path->Buffer[n - 1] != '\\') n--;
    if (n < 2) return;
    parent.Buffer = path->Buffer;
    parent.Length = (USHORT)((n - 1) * sizeof(USHORT));
    parent.MaximumLength = parent.Length;

    IdeInitAttributes(&oa, path, NULL);
    status = ZwOpenKey(&ident, IDE_KEY_READ_WRITE, &oa);
    if (status < 0) {
        IDE_DBG((IDE_PFX "DriverEntry: service key not opened (%08lX)\n", (ULONG)status));
        return;
    }
    IdeInitAttributes(&oa, &parent, NULL);
    status = ZwOpenKey(&services, IDE_KEY_READ_WRITE, &oa);
    if (status < 0) {
        IDE_DBG((IDE_PFX "DriverEntry: Services key not opened (%08lX)\n", (ULONG)status));
        ZwClose(ident);
        return;
    }

    for (i = 0; i < 2; i++) {
        key = IdeRegOpen(services, inbox[i], inboxBytes[i]);
        if (key == NULL) continue;
        if (!IdeRegReadDword(ident, saved[i], savedBytes[i], &old) &&
            IdeRegReadDword(key, g_IdeStartName, sizeof(g_IdeStartName), &start)) {
            IdeRegWriteDword(ident, saved[i], savedBytes[i], start);
            if (start != 4) {
                IdeRegWriteDword(key, g_IdeStartName, sizeof(g_IdeStartName), 4);
                changed = TRUE;
            }
            IDE_DBG((IDE_PFX "DriverEntry: in-box driver %s, Start %lu -> 4\n", inboxText[i], start));
        }
        ZwClose(key);
    }

    /* scsidisk (NT 3.x) or else disk (NT 4.0): a disk key on NT 3.x has no
       driver behind it */
    for (i = 0; changed && i < 2; i++) {
        key = IdeRegOpen(services, cls[i], clsBytes[i]);
        if (key == NULL) continue;
        if (IdeRegReadDword(key, g_IdeStartName, sizeof(g_IdeStartName), &start) && start != 0) {
            IdeRegWriteDword(key, g_IdeStartName, sizeof(g_IdeStartName), 0);
            IDE_DBG((IDE_PFX "DriverEntry: class driver %s, Start %lu -> 0\n", clsText[i], start));
        }
        ZwClose(key);
        break;
    }
    ZwClose(services);
    ZwClose(ident);
}
#endif

ULONG
DriverEntry(IN PVOID DriverObject, IN PVOID Argument2)
{
    HW_INITIALIZATION_DATA initData;
    ULONG status, status2;

    g_IdeDumpMode = (BOOLEAN)(DriverObject == NULL);

    IDE_DBG((IDE_PFX "DriverEntry%s\n", g_IdeDumpMode ? " (dump)" : ""));

    ZeroMemoryBytes(&initData, sizeof(HW_INITIALIZATION_DATA));

#ifdef IDE_NT31_COMPAT
    initData.HwInitializationDataSize = IDE_HWINIT_SIZE_NT35;
#else
    initData.HwInitializationDataSize = sizeof(HW_INITIALIZATION_DATA);
#endif
    initData.HwInitialize = IdeHwInitialize;
    initData.HwStartIo = IdeStartIo;
    initData.HwInterrupt = IdeInterrupt;
    initData.HwResetBus = IdeResetBus;
    initData.HwFindAdapter = IdeFindAdapter;
#ifdef IDE_W9X
    initData.HwAdapterState = IdeAdapterState;
#endif
#if !defined(IDE_NT4)
    initData.HwAdapterControl = IdeAdapterControl;
#endif
    initData.DeviceExtensionSize = sizeof(HW_DEVICE_EXTENSION);
    initData.AdapterInterfaceType = PCIBus;
    /* Two channels: command block, control register each, and the bus
       master block */
    initData.NumberOfAccessRanges = 8;
    initData.MapBuffers = TRUE;
    initData.NeedPhysicalAddresses = TRUE;
    initData.AutoRequestSense = FALSE;
    initData.TaggedQueuing = FALSE;
    initData.MultipleRequestPerLu = FALSE;

#if !defined(IDE_NT4)
    if (!g_IdeDumpMode) IdeEnsurePnpInterface(Argument2);
#endif

    status = ScsiPortInitialize(DriverObject, Argument2, &initData, NULL);

#ifdef IDE_NT31_COMPAT
    /* NT 3.1 refuses any other structure size before HwFindAdapter is ever
       called, and has no PCI bus to scan: retry with its 40h-byte layout on
       the ISA bus. PCI functions are then found through the ports, legacy
       channels by probing. */
    if (status != 0 && !g_IdeFindAdapterCalled) {
        IDE_DBG((IDE_PFX "DriverEntry: %08lX, retrying with the NT 3.1 layout\n", status));
        initData.HwInitializationDataSize = IDE_HWINIT_SIZE_NT31;
        initData.AdapterInterfaceType = Isa;
        status = ScsiPortInitialize(DriverObject, Argument2, &initData, NULL);
        IDE_DBG((IDE_PFX "DriverEntry: ScsiPortInitialize returned %08lX\n", status));
        if (g_IdeAdapterFound) IdeDisableInboxDrivers(Argument2);
        return status;
    }
#endif

#if defined(IDE_NT4)
    /* The ISA pass: legacy channels not behind a PCI function (ISA/VLB
       controllers), each channel an adapter of its own with its IRQ */
    if (!g_IdeDumpMode) {
        initData.AdapterInterfaceType = Isa;
        status2 = ScsiPortInitialize(DriverObject, Argument2, &initData, NULL);
        IDE_DBG((IDE_PFX "DriverEntry: ScsiPortInitialize returned %08lX (PCI), %08lX (ISA)\n", status, status2));
        if (status != 0 && status2 == 0) status = status2;
#ifndef IDE_W9X
        if (g_IdeAdapterFound) IdeDisableInboxDrivers(Argument2);
#endif
    } else {
        IDE_DBG((IDE_PFX "DriverEntry: ScsiPortInitialize returned %08lX\n", status));
    }
#else
    /* 2000/XP: everything comes through PnP, ISA controllers as *PNP0600.
       An adapter detected by ScsiPort itself (NT 4.0 style) would make
       this a legacy driver and take the system's sleep states away. The
       start IRP of a compatibility-mode PCI function gets its legacy IRQ
       added before ScsiPort sees it (idepnp.c). */
    (VOID)status2;
    if (!g_IdeDumpMode && status == 0) IdeHookPnp(DriverObject);
    IDE_DBG((IDE_PFX "DriverEntry: ScsiPortInitialize returned %08lX\n", status));
#endif
    return status;
}

/* ------------------------------------------------------------------ */
/* HwFindAdapter                                                       */
/* ------------------------------------------------------------------ */

static BOOLEAN
IdeArgPresent(IN PCHAR Args, IN PCHAR Key)
{
    ULONG i, j;
    CHAR a, b;

    if (Args == NULL) return FALSE;
    for (i = 0; Args[i] != 0; i++) {
        for (j = 0; Key[j] != 0; j++) {
            a = Args[i + j];
            b = Key[j];
            if (a >= 'a' && a <= 'z') a = (CHAR)(a - 'a' + 'A');
            if (a != b) break;
        }
        if (Key[j] == 0) return TRUE;
    }
    return FALSE;
}

/* Map one channel's ports and record it in the extension */
static BOOLEAN
IdeAttachChannel(
    IN PHW_DEVICE_EXTENSION HwInit,
    IN PPORT_CONFIGURATION_INFORMATION ConfigInfo,
    IN INTERFACE_TYPE BusType,
    IN ULONG BusNumber,
    IN ULONG Index,
    IN PIDE_CHANNEL_DESC Desc
)
{
    PIDE_CHANNEL ch = &HwInit->Channels[Index];
    SCSI_PHYSICAL_ADDRESS pa;

    ZeroMemoryBytes(ch, FIELD_OFFSET(IDE_CHANNEL, Prd));
    ch->CmdPort = Desc->CmdPort;
    ch->BmPort = Desc->BmPort;
    ch->Irq = Desc->Irq;
    ch->Native = Desc->Native;

    pa.HighPart = 0;
    pa.LowPart = Desc->CmdPort;
    ch->Cmd = (PUCHAR)ScsiPortGetDeviceBase(HwInit, BusType, BusNumber, pa, 8, TRUE);
    pa.LowPart = Desc->CtlPort;
    ch->Ctl = (PUCHAR)ScsiPortGetDeviceBase(HwInit, BusType, BusNumber, pa, 1, TRUE);
    if (ch->Cmd == NULL || ch->Ctl == NULL) {
        IDE_DBG((IDE_PFX "FindAdapter: mapping ports %lX/%lX failed\n",
                 (ULONG)Desc->CmdPort, (ULONG)Desc->CtlPort));
        return FALSE;
    }
    if (Desc->BmPort != 0) {
        pa.LowPart = Desc->BmPort;
        ch->Bm = (PUCHAR)ScsiPortGetDeviceBase(HwInit, BusType, BusNumber, pa, 8, TRUE);
        if (ch->Bm == NULL) {
            IDE_DBG((IDE_PFX "FindAdapter: mapping bus master block %lX failed, PIO only\n", (ULONG)Desc->BmPort));
        }
    }
    ch->Present = TRUE;
    IdeClaimPort(Desc->CmdPort);
    (VOID)ConfigInfo;
    return TRUE;
}

static VOID
IdeSetAccessRange(IN PPORT_CONFIGURATION_INFORMATION ConfigInfo, IN ULONG Index, IN ULONG Port, IN ULONG Length)
{
    PACCESS_RANGE ranges = *ConfigInfo->AccessRanges;

    if (ranges == NULL || Index >= ConfigInfo->NumberOfAccessRanges) return;
    ranges[Index].RangeStart.HighPart = 0;
    ranges[Index].RangeStart.LowPart = Port;
    ranges[Index].RangeLength = Length;
    ranges[Index].RangeInMemory = FALSE;
}

ULONG
IdeFindAdapter(
    IN PVOID DeviceExtension,
    IN PVOID Context,
    IN PVOID BusInformation,
    IN PCHAR ArgumentString,
    IN OUT PPORT_CONFIGURATION_INFORMATION ConfigInfo,
    OUT PBOOLEAN Again
)
{
    PHW_DEVICE_EXTENSION hwInit = (PHW_DEVICE_EXTENSION)DeviceExtension;
    PCI_COMMON_CONFIG pciConfig;
    IDE_CHANNEL_DESC desc[2];
    ULONG descCount = 0, first = 0, count = 0;
    ULONG targetBus = 0, targetSlot = 0;
    ULONG i, c;
    BOOLEAN hasSlot, pci = FALSE, legacyPass;
    BOOLEAN interrupts, level2;
    INTERFACE_TYPE busType;
    ULONG busNumber;

    (VOID)Context;
    (VOID)BusInformation;
    *Again = FALSE;

#ifdef IDE_NT31_COMPAT
    g_IdeFindAdapterCalled = TRUE;
    IdePciProbeAccess(hwInit);
    hasSlot = (BOOLEAN)IDE_CONFIG_HAS(ConfigInfo, SlotNumber);
#else
    hasSlot = TRUE;
#endif
    if (hwInit->ChannelCount != 0) return SP_RETURN_FOUND;

    hwInit->DumpMode = (BOOLEAN)(g_IdeDumpMode || IdeArgPresent(ArgumentString, "DUMP="));
    /* ScsiPort's own enumeration (NT 4.0 everywhere, the ISA pass on
       2000/XP) as opposed to a PnP start or the dump stack's call */
#if defined(IDE_NT4)
    legacyPass = (BOOLEAN)!hwInit->DumpMode;
#else
    legacyPass = (BOOLEAN)(!hwInit->DumpMode && ConfigInfo->AdapterInterfaceType == Isa && hasSlot &&
                           ConfigInfo->SlotNumber == 0 && ConfigInfo->NumberOfAccessRanges != 0 &&
                           (*ConfigInfo->AccessRanges)[0].RangeLength == 0);
#endif
    hwInit->DmaAllowed = (BOOLEAN)!IdeArgPresent(ArgumentString, "DMA=0");
    hwInit->DmaForced = IdeArgPresent(ArgumentString, "DMA=1");
    hwInit->PciWritable = TRUE;

    IDE_DBG((IDE_PFX "FindAdapter: bus type %lu, bus %lu slot %08lX, %lu range(s), IRQ %lu%s\n",
             (ULONG)ConfigInfo->AdapterInterfaceType, ConfigInfo->SystemIoBusNumber,
             hasSlot ? ConfigInfo->SlotNumber : 0xFFFFFFFF, ConfigInfo->NumberOfAccessRanges,
             ConfigInfo->BusInterruptLevel, hwInit->DumpMode ? ", dump" : ""));

    /* Step 1: the PCI function ScsiPort / PnP / SCSIPORT.PDR called us for */
    if (hasSlot && ConfigInfo->AdapterInterfaceType == PCIBus) {
        targetBus = ConfigInfo->SystemIoBusNumber;
        targetSlot = ConfigInfo->SlotNumber;
        if (IdeReadPciHeader(hwInit, targetBus, targetSlot, &pciConfig, NULL) &&
            IdePciConfigIsIde(&pciConfig)) {
            descCount = IdeDescribePciChannels(&pciConfig, desc);
#if !defined(IDE_NT4)
            /* A PnP start: the function is this device node's, also when an
               earlier instance (before a stop, removal or driver update)
               left its channels in the claim table */
            if (!legacyPass && descCount != 0) pci = TRUE;
#endif
            for (i = 0; i < descCount && !pci; i++) {
                if (!IdeIsPortClaimed(desc[i].CmdPort)) pci = TRUE;
            }
#if !defined(IDE_NT4)
            if (!pci && !legacyPass) return SP_RETURN_NOT_FOUND;
#endif
        }
    }

    /* Step 2: the next PCI IDE function with a free channel (NT 4.0 and
       NT 3.1 scan the bus themselves) */
#if defined(IDE_NT4)
    if (!pci && ConfigInfo->AdapterInterfaceType != Isa) {
        if (IdeScanPci(hwInit, &targetBus, &targetSlot, &pciConfig)) {
            descCount = IdeDescribePciChannels(&pciConfig, desc);
            pci = TRUE;
        }
    }
#ifdef IDE_NT31_COMPAT
    /* NT 3.1: one ISA pass does the PCI functions too */
    if (!pci && !hasSlot && IdeScanPci(hwInit, &targetBus, &targetSlot, &pciConfig)) {
        descCount = IdeDescribePciChannels(&pciConfig, desc);
        pci = TRUE;
    }
#endif
#else
#endif

    if (pci) {
        /* Which of the function's channels, and how many, this instance takes */
        for (first = 0; first < descCount && IdeIsPortClaimed(desc[first].CmdPort); first++) ;
#if !defined(IDE_NT4)
        if (!legacyPass) first = 0;             /* PnP start: the whole function */
#endif
#if !defined(IDE_NT4)
        if (legacyPass) {
            while (first < descCount && desc[first].Native) first++;
        }
#endif
        if (first >= descCount) return SP_RETURN_NOT_FOUND;
#ifdef IDE_W9X
        count = descCount - first;              /* the whole function: one call per device node */
#else
        if (legacyPass && !IDE_CHIP_SERIALIZED(IdeChipKind(pciConfig.VendorID, pciConfig.DeviceID))) {
            count = 1;                          /* one channel per instance: its own IRQ */
        } else {
            count = descCount - first;          /* the whole function */
        }
#endif
        hwInit->HasPci = TRUE;
        hwInit->PciBus = targetBus;
        hwInit->PciSlot = targetSlot;
        hwInit->VendorId = pciConfig.VendorID;
        hwInit->DeviceId = pciConfig.DeviceID;
        hwInit->Intel = IdeIntelKind(pciConfig.VendorID, pciConfig.DeviceID);
        hwInit->FirstChannel = desc[first].Index;

        /* I/O space and bus master on */
        if ((pciConfig.Command & 0x0005) != 0x0005 || (pciConfig.Command & (1 << 10))) {
            IdePciWriteConfig(hwInit, 0x04, 2, (pciConfig.Command | 0x0005) & ~(1UL << 10));
        }
        IDE_DBG((IDE_PFX "FindAdapter: %04lX:%04lX at bus %lu slot %08lX, progif %02lX, channel %lu%s\n",
                 (ULONG)pciConfig.VendorID, (ULONG)pciConfig.DeviceID, targetBus, targetSlot,
                 (ULONG)pciConfig.ProgIf, (ULONG)desc[first].Index, (count > 1) ? " and next" : ""));
    } else if (legacyPass && (ConfigInfo->AdapterInterfaceType == Isa || !hasSlot)) {
        /* Step 3: a legacy channel nobody claimed (ISA pass, NT 3.1) */
        for (i = 0; i < IDE_LEGACY_CHANNELS; i++) {
            if (IdeIsPortClaimed(g_IdeLegacy[i].Cmd)) continue;
#if !defined(IDE_NT4)
            /* A PCI function's channel: PnP starts the function (idepnp.c) */
            if (IdeFindCompatFunction(hwInit, g_IdeLegacy[i].Cmd, &targetBus, &targetSlot, &pciConfig)) continue;
#endif
            if (!IdeLegacyChannelPresent(g_IdeLegacy[i].Cmd, g_IdeLegacy[i].Ctl)) {
                IdeClaimPort(g_IdeLegacy[i].Cmd);   /* not probed again */
                continue;
            }
            break;
        }
        if (i >= IDE_LEGACY_CHANNELS) return SP_RETURN_NOT_FOUND;
        ZeroMemoryBytes(&desc[0], sizeof(desc[0]));
        desc[0].CmdPort = g_IdeLegacy[i].Cmd;
        desc[0].CtlPort = g_IdeLegacy[i].Ctl;
        desc[0].Irq = g_IdeLegacy[i].Irq;
        desc[0].Index = (UCHAR)(i & 1);
        descCount = 1;
        first = 0;
        count = 1;
        hwInit->FirstChannel = desc[0].Index;
        IDE_DBG((IDE_PFX "FindAdapter: legacy channel at %lX, IRQ %lu\n", (ULONG)desc[0].CmdPort, (ULONG)desc[0].Irq));
    } else {
#if defined(IDE_NT4)
        return SP_RETURN_NOT_FOUND;
#else
        /* PnP ISA (*PNP0600) or a dump instance: ports from the resources */
        {
            PACCESS_RANGE ranges = *ConfigInfo->AccessRanges;
            ZeroMemoryBytes(&desc[0], sizeof(desc[0]));
            if (ranges != NULL && ConfigInfo->NumberOfAccessRanges >= 2 && ranges[0].RangeLength >= 8) {
                desc[0].CmdPort = (USHORT)ranges[0].RangeStart.LowPart;
                desc[0].CtlPort = (USHORT)(ranges[1].RangeStart.LowPart + ((ranges[1].RangeLength >= 4) ? 2 : 0));
                desc[0].Irq = (UCHAR)ConfigInfo->BusInterruptLevel;
                if (ConfigInfo->NumberOfAccessRanges >= 3 && ranges[2].RangeLength >= 8) {
                    desc[0].BmPort = (USHORT)ranges[2].RangeStart.LowPart;
                }
            } else if (hwInit->DumpMode) {
                /* The dump stack's copy of a legacy adapter's configuration:
                   the ISA pass reports no ranges for a PCI function's
                   channel (see there), its IRQ names the channel */
                for (i = 0; i < IDE_LEGACY_CHANNELS; i++) {
                    if (g_IdeLegacy[i].Irq == ConfigInfo->BusInterruptLevel) break;
                }
                if (i >= IDE_LEGACY_CHANNELS) return SP_RETURN_NOT_FOUND;
                desc[0].CmdPort = g_IdeLegacy[i].Cmd;
                desc[0].CtlPort = g_IdeLegacy[i].Ctl;
                desc[0].Irq = g_IdeLegacy[i].Irq;
                if (IdeFindCompatFunction(hwInit, desc[0].CmdPort, &targetBus, &targetSlot, &pciConfig)) {
                    IDE_CHANNEL_DESC all[2];
                    ULONG n = IdeDescribePciChannels(&pciConfig, all);
                    for (i = 0; i < n; i++) {
                        if (all[i].CmdPort == desc[0].CmdPort) desc[0].BmPort = all[i].BmPort;
                    }
                    hwInit->HasPci = TRUE;
                    hwInit->PciBus = targetBus;
                    hwInit->PciSlot = targetSlot;
                    hwInit->VendorId = pciConfig.VendorID;
                    hwInit->DeviceId = pciConfig.DeviceID;
                    hwInit->Intel = IdeIntelKind(pciConfig.VendorID, pciConfig.DeviceID);
                }
            } else {
                return SP_RETURN_NOT_FOUND;
            }
            desc[0].Index = (UCHAR)((desc[0].CmdPort == IDE_LEGACY_SECONDARY_CMD) ? 1 : 0);
            descCount = 1;
            first = 0;
            count = 1;
            hwInit->FirstChannel = desc[0].Index;
            IDE_DBG((IDE_PFX "FindAdapter: channel from %s at %lX\n", (ranges != NULL && ranges[0].RangeLength >= 8) ? "resources" : "its IRQ", (ULONG)desc[0].CmdPort));
        }
#endif
    }

    /* Bus type and number for mapping and the interrupt */
    busType = ConfigInfo->AdapterInterfaceType;
    busNumber = ConfigInfo->SystemIoBusNumber;
    /* Our own enumeration and the dump stack: the interrupt and bus type
       are ours to set. A PnP start brought them in the resources. */
    if (legacyPass || hasSlot == FALSE || ConfigInfo->AdapterInterfaceType != PCIBus) {
        if (pci && desc[first].Native) {
            /* PCI interrupt (level, shared): the function's own slot */
            busType = PCIBus;
            busNumber = targetBus;
            if (hasSlot) {
                ConfigInfo->SystemIoBusNumber = targetBus;
                ConfigInfo->SlotNumber = targetSlot;
            }
            ConfigInfo->InterruptMode = LevelSensitive;
            ConfigInfo->BusInterruptLevel = desc[first].Irq;
            ConfigInfo->BusInterruptVector = desc[first].Irq;
#ifdef IDE_NT31_COMPAT
            if (!hasSlot) {
                /* NT 3.1 has no PCI bus handler: INTx only on request, on the
                   EISA bus; Fast Polling otherwise */
                busType = Eisa;
                busNumber = 0;
                ConfigInfo->AdapterInterfaceType = Eisa;
                if (!IdeArgPresent(ArgumentString, "IRQ=1")) {
                    ConfigInfo->BusInterruptLevel = 0;
                    ConfigInfo->BusInterruptVector = 0;
                }
            }
#endif
        } else {
            /* Legacy IRQ 14/15 (edge). The EISA bus handler gives a 32-bit DMA
               adapter for the bus master; a plain ISA channel needs none. */
            busType = (desc[first].BmPort != 0) ? Eisa : Isa;
            busNumber = 0;
            ConfigInfo->AdapterInterfaceType = busType;
            ConfigInfo->SystemIoBusNumber = 0;
            ConfigInfo->InterruptMode = Latched;
            ConfigInfo->BusInterruptLevel = desc[first].Irq;
            ConfigInfo->BusInterruptVector = desc[first].Irq;
        }
        if (IdeArgPresent(ArgumentString, "IRQ=0") ||
            (pci && IDE_CHIP_SERIALIZED(IdeChipKind(pciConfig.VendorID, pciConfig.DeviceID)))) {
            ConfigInfo->BusInterruptLevel = 0;
            ConfigInfo->BusInterruptVector = 0;
        }
        /* Report the ranges this instance uses. On 2000/XP a PCI function's
           ports are on record with its device node; reported again here they
           would conflict with it. */
        for (i = 0; i < ConfigInfo->NumberOfAccessRanges; i++) IdeSetAccessRange(ConfigInfo, i, 0, 0);
#if !defined(IDE_NT4)
        if (!pci)
#endif
        {
            for (c = 0; c < count && 3 * c + 2 < ConfigInfo->NumberOfAccessRanges; c++) {
                IdeSetAccessRange(ConfigInfo, 3 * c, desc[first + c].CmdPort, 8);
                IdeSetAccessRange(ConfigInfo, 3 * c + 1, desc[first + c].CtlPort, 1);
                IdeSetAccessRange(ConfigInfo, 3 * c + 2, desc[first + c].BmPort, desc[first + c].BmPort ? 8 : 0);
            }
        }
        *Again = TRUE;
    } else if (pci) {
        busType = PCIBus;
        busNumber = targetBus;
    }

    for (c = 0; c < count; c++) {
        if (!IdeAttachChannel(hwInit, ConfigInfo, busType, busNumber, c, &desc[first + c])) {
            return SP_RETURN_ERROR;
        }
    }
    hwInit->ChannelCount = (UCHAR)count;
    if (hwInit->HasPci && !IdeChipSetup(hwInit, &pciConfig, busType, busNumber)) {
        hwInit->ChannelCount = 0;
        return SP_RETURN_NOT_FOUND;
    }

    /* Legacy channels: atdisk must leave them alone */
    for (c = 0; c < count; c++) {
        if (desc[first + c].CmdPort == IDE_LEGACY_PRIMARY_CMD) ConfigInfo->AtdiskPrimaryClaimed = TRUE;
        if (desc[first + c].CmdPort == IDE_LEGACY_SECONDARY_CMD) ConfigInfo->AtdiskSecondaryClaimed = TRUE;
    }

    if (IDE_CONFIG_HAS(ConfigInfo, MaximumNumberOfTargets)) {
        ConfigInfo->MaximumNumberOfTargets = (UCHAR)(count * IDE_DEVICES_PER_CHANNEL);
    }
    ConfigInfo->NumberOfBuses = 1;
    ConfigInfo->InitiatorBusId[0] = 7;
    ConfigInfo->MaximumTransferLength = IDE_MAX_TRANSFER;
    ConfigInfo->NumberOfPhysicalBreaks = IDE_MAX_PHYS_BREAKS;
    ConfigInfo->AlignmentMask = 0x1;
    ConfigInfo->CachesData = FALSE;

    /* A 32-bit scatter/gather master. Also for a PCI channel without a
       bus master: ScsiPort allocates the uncached extension (PRD tables,
       IDENTIFY buffer) through the adapter's DMA object, and a PCI or
       EISA slave gets none. A plain ISA channel: no DMA adapter (an ISA
       master would get map registers below 16 MB, and their copy-back
       would overwrite what PIO put into the buffer). */
    ConfigInfo->Master = FALSE;
    ConfigInfo->ScatterGather = FALSE;
    ConfigInfo->Dma32BitAddresses = FALSE;
    for (c = 0; c < count; c++) {
        if (hwInit->Channels[c].Bm != NULL || busType != Isa) {
            ConfigInfo->Master = TRUE;
            ConfigInfo->ScatterGather = TRUE;
            ConfigInfo->Dma32BitAddresses = TRUE;
        }
    }

    /* Interrupt-completed DMA where an interrupt is connected: the first
       channel's, the second one's own (BusInterruptLevel2) or the shared
       PCI line of a native-mode controller. Off in the dump stack, with
       IRQ=0, on 9x unless IRQ=1. */
    interrupts = (BOOLEAN)(ConfigInfo->BusInterruptLevel != 0 || ConfigInfo->BusInterruptVector != 0);
#ifdef IDE_W9X
    level2 = FALSE;                             /* SCSIPORT.PDR has no second interrupt */
#else
    level2 = (BOOLEAN)(IDE_CONFIG_HAS(ConfigInfo, BusInterruptLevel2) &&
                       (ConfigInfo->BusInterruptLevel2 != 0 || ConfigInfo->BusInterruptVector2 != 0));
#endif
    if (hwInit->DumpMode || IdeArgPresent(ArgumentString, "IRQ=0")) interrupts = FALSE;
    if (IDE_CHIP_SERIALIZED(hwInit->Chip)) interrupts = FALSE;
#ifdef IDE_W9X
    if (!IdeArgPresent(ArgumentString, "IRQ=1")) interrupts = FALSE;
    /* Their latch is cleared in config space, which is not touched from
       an interrupt on 9x */
    if (hwInit->Chip == IDE_CHIP_CMD643 || hwInit->Chip == IDE_CHIP_CMD646) interrupts = FALSE;
#endif
    hwInit->AsyncMode = interrupts;
    for (c = 0; c < count; c++) {
        PIDE_CHANNEL ch = &hwInit->Channels[c];
        /* The second channel of a PnP-started compatibility-mode function:
           ScsiPort connects only the first interrupt of a PnP adapter */
        ch->Async = (BOOLEAN)(interrupts && ch->Bm != NULL &&
                              (c == 0 || ch->Native || (level2 && legacyPass)));
    }

    IDE_DBG((IDE_PFX "FindAdapter: %lu channel(s) at %lX%s, IRQ %lu/%lu, %s%s\n",
             count, (ULONG)desc[first].CmdPort, hwInit->Channels[0].Bm ? " (bus master)" : "",
             ConfigInfo->BusInterruptLevel,
             level2 ? ConfigInfo->BusInterruptLevel2 : 0,
             interrupts ? "interrupt" : "Fast Polling",
             hwInit->DmaAllowed ? "" : ", DMA off"));

    if (!IdeAllocateDma(hwInit, ConfigInfo)) return SP_RETURN_ERROR;
#if defined(IDE_NT4) && !defined(IDE_W9X)
    g_IdeAdapterFound = TRUE;
#endif
    return SP_RETURN_FOUND;
}

/* ------------------------------------------------------------------ */
/* HwInitialize                                                        */
/* ------------------------------------------------------------------ */

BOOLEAN
IdeHwInitialize(IN PVOID DeviceExtension)
{
    IdeInitializeAll((PHW_DEVICE_EXTENSION)DeviceExtension);
    IDE_DBG((IDE_PFX "HwInitialize: done\n"));
    return TRUE;
}

/* ------------------------------------------------------------------ */
/* HwStartIo                                                           */
/* ------------------------------------------------------------------ */

static VOID
IdeResetTarget(IN PHW_DEVICE_EXTENSION HwInit, IN ULONG Target)
{
    ULONG c = Target / IDE_DEVICES_PER_CHANNEL;

    if (c >= HwInit->ChannelCount || !HwInit->Channels[c].Present) return;
    IdeFailOutstanding(HwInit, c, SRB_STATUS_BUS_RESET);
    IdeResetChannel(HwInit, c);
}

/* A DMA read/write for a channel that is busy with one already: it can
   wait and be started from the completion of the first */
static BOOLEAN
IdeCanQueue(IN PHW_DEVICE_EXTENSION HwInit, IN PSCSI_REQUEST_BLOCK Srb)
{
    PIDE_CHANNEL ch;
    PIDE_DEVICE dev;

    if (Srb->Function != SRB_FUNCTION_EXECUTE_SCSI || HwInit->DumpMode) return FALSE;
    if (Srb->TargetId >= HwInit->ChannelCount * IDE_DEVICES_PER_CHANNEL) return FALSE;
    ch = IDE_CHANNEL_OF_TARGET(HwInit, Srb->TargetId);
    dev = IDE_DEVICE_OF_TARGET(HwInit, Srb->TargetId);
    if (!ch->Async || !dev->Present || !dev->UseDma || ch->NeedsSetup || HwInit->CheckState) return FALSE;

    switch (Srb->Cdb[0]) {
    case SCSIOP_READ6:
    case SCSIOP_WRITE6:
    case SCSIOP_READ:
    case SCSIOP_WRITE:
    case IDE_SCSIOP_READ12:
    case IDE_SCSIOP_WRITE12:
        return TRUE;
    case IDE_SCSIOP_READ16:
    case IDE_SCSIOP_WRITE16:
        return (BOOLEAN)!dev->IsAtapi;
    default:
        return FALSE;
    }
}

BOOLEAN
IdeStartIo(IN PVOID DeviceExtension, IN PSCSI_REQUEST_BLOCK Srb)
{
    PHW_DEVICE_EXTENSION hwInit = (PHW_DEVICE_EXTENSION)DeviceExtension;
    PIDE_CHANNEL ch = NULL;
    ULONG c, unit;

    Srb->SrbStatus = SRB_STATUS_PENDING;

    if (Srb->TargetId < hwInit->ChannelCount * IDE_DEVICES_PER_CHANNEL) {
        ch = IDE_CHANNEL_OF_TARGET(hwInit, Srb->TargetId);
    }

    /* The channel is busy with an interrupt-completed command */
    if (ch != NULL && ch->Outstanding != NULL) {
        if (ch->Pending == NULL && IdeCanQueue(hwInit, Srb)) {
            ch->Pending = Srb;
            ScsiPortNotification(NextRequest, hwInit, NULL);
            return TRUE;
        }
        IdeDrainChannel(hwInit, Srb->TargetId / IDE_DEVICES_PER_CHANNEL);
    }

    switch (Srb->Function) {
    case SRB_FUNCTION_EXECUTE_SCSI:
        IdeCheckState(hwInit);
        IdeSatlProcessSrb(hwInit, Srb);
        break;

    case SRB_FUNCTION_IO_CONTROL:
        IdeProcessIoControl(hwInit, Srb);
        break;

    case SRB_FUNCTION_RESET_BUS:
        IdeResetBus(hwInit, Srb->PathId);
        Srb->SrbStatus = SRB_STATUS_SUCCESS;
        break;

    case SRB_FUNCTION_RESET_DEVICE:
        IdeResetTarget(hwInit, Srb->TargetId);
        Srb->SrbStatus = SRB_STATUS_SUCCESS;
        break;

    case SRB_FUNCTION_ABORT_COMMAND:
        Srb->SrbStatus = SRB_STATUS_ABORT_FAILED;
        break;

    case SRB_FUNCTION_SHUTDOWN:
    case SRB_FUNCTION_FLUSH:
        IdeCheckState(hwInit);
        if (ch != NULL) {
            c = Srb->TargetId / IDE_DEVICES_PER_CHANNEL;
            unit = IDE_UNIT_OF_TARGET(Srb->TargetId);
            if (Srb->Function == SRB_FUNCTION_SHUTDOWN) {
                for (unit = 0; unit < IDE_DEVICES_PER_CHANNEL; unit++) IdeFlushDevice(hwInit, c, unit);
            } else {
                IdeFlushDevice(hwInit, c, unit);
            }
        }
        Srb->SrbStatus = SRB_STATUS_SUCCESS;
        break;

    default:
        Srb->SrbStatus = SRB_STATUS_INVALID_REQUEST;
        break;
    }

    if (Srb->SrbStatus != SRB_STATUS_PENDING) {
        ScsiPortNotification(RequestComplete, hwInit, Srb);
    } else {
        IdeArmTimer(hwInit);
    }
    ScsiPortNotification(NextRequest, hwInit, NULL);
    return TRUE;
}

/* ------------------------------------------------------------------ */
/* HwInterrupt                                                         */
/* ------------------------------------------------------------------ */

BOOLEAN
IdeInterrupt(IN PVOID DeviceExtension)
{
    PHW_DEVICE_EXTENSION hwInit = (PHW_DEVICE_EXTENSION)DeviceExtension;
    BOOLEAN claimed = FALSE;
    ULONG c;
    UCHAR bmsts;

    for (c = 0; c < hwInit->ChannelCount; c++) {
        PIDE_CHANNEL ch = &hwInit->Channels[c];
        if (!ch->Present) continue;

        if (ch->Outstanding != NULL) {
            if (IdeServiceChannel(hwInit, c, TRUE) != 0) {
                hwInit->MissedInterrupts = 0;
                claimed = TRUE;
            }
            continue;
        }
        /* Nothing in flight: a stray interrupt (reset, polled command on
           a device with nIEN clear). Acknowledge it on our own channels. */
        if (ch->Bm != NULL) {
            bmsts = IDE_INB(ch->Bm + BM_REG_STATUS);
            if (bmsts & BM_STS_INTERRUPT) {
                IDE_OUTB(ch->Bm + BM_REG_STATUS, bmsts | BM_STS_INTERRUPT);
                IDE_INB(ch->Cmd + IDE_REG_STATUS);
                claimed = TRUE;
            }
        } else if (!ch->Native && hwInit->AsyncMode) {
            /* Legacy line of our own: reading the status drops INTRQ */
            IDE_INB(ch->Cmd + IDE_REG_STATUS);
            claimed = TRUE;
        }
    }
    /* CMD64x: the controller holds its PCI interrupt while a latch is set */
    if (IDE_CHIP_IS_CMD(hwInit->Chip)) {
        for (c = 0; c < hwInit->ChannelCount; c++) {
            PIDE_CHANNEL ch = &hwInit->Channels[c];
            if (ch->Present && IdeChipClearInterrupt(hwInit, c)) {
                if (ch->Outstanding == NULL) IDE_INB(ch->Cmd + IDE_REG_STATUS);
                claimed = TRUE;
            }
        }
    }
    if (claimed) hwInit->InterruptCount++;
    return claimed;
}

/* ------------------------------------------------------------------ */
/* HwResetBus                                                          */
/* ------------------------------------------------------------------ */

BOOLEAN
IdeResetBus(IN PVOID HwDeviceExtension, IN ULONG PathId)
{
    PHW_DEVICE_EXTENSION hwInit = (PHW_DEVICE_EXTENSION)HwDeviceExtension;
    ULONG c, unit;

    (VOID)PathId;
    IDE_DBG((IDE_PFX "ResetBus\n"));

    for (c = 0; c < hwInit->ChannelCount; c++) {
        PIDE_CHANNEL ch = &hwInit->Channels[c];
        if (!ch->Present) continue;
        if (ch->Outstanding != NULL) {
            /* A DMA command that never finished: counts against DMA */
            PIDE_DEVICE dev = &ch->Devices[ch->ActiveUnit];
            if (++dev->DmaErrors >= IDE_DMA_ERROR_LIMIT) {
                dev->UseDma = FALSE;
                dev->ModeType = IDE_MODE_PIO;
                dev->Mode = 0;
            }
        }
        IdeFailOutstanding(hwInit, c, SRB_STATUS_BUS_RESET);
        IdeResetChannel(hwInit, c);
        for (unit = 0; unit < IDE_DEVICES_PER_CHANNEL; unit++) ch->Devices[unit].SensePending = FALSE;
    }
    return TRUE;
}

/* ------------------------------------------------------------------ */
/* HwAdapterControl (2000/XP/x64): stop and restart for power changes  */
/* ------------------------------------------------------------------ */

#if !defined(IDE_NT4)
SCSI_ADAPTER_CONTROL_STATUS
IdeAdapterControl(
    IN PVOID DeviceExtension,
    IN SCSI_ADAPTER_CONTROL_TYPE ControlType,
    IN PVOID Parameters
)
{
    PHW_DEVICE_EXTENSION hwInit = (PHW_DEVICE_EXTENSION)DeviceExtension;
    PSCSI_SUPPORTED_CONTROL_TYPE_LIST list;
    ULONG i, c;

    switch (ControlType) {
    case ScsiQuerySupportedControlTypes:
        list = (PSCSI_SUPPORTED_CONTROL_TYPE_LIST)Parameters;
        for (i = 0; i < list->MaxControlType; i++) {
            list->SupportedTypeList[i] = (BOOLEAN)(i == ScsiQuerySupportedControlTypes ||
                                                  i == ScsiStopAdapter ||
                                                  i == ScsiRestartAdapter);
        }
        return ScsiAdapterControlSuccess;

    case ScsiStopAdapter:
        IDE_DBG((IDE_PFX "AdapterControl: stop\n"));
        for (c = 0; c < hwInit->ChannelCount; c++) {
            if (!hwInit->Channels[c].Present) continue;
            IdeDrainChannel(hwInit, c);
            IdeStopChannel(hwInit, c);
        }
        hwInit->Stopped = TRUE;
        return ScsiAdapterControlSuccess;

    case ScsiRestartAdapter:
        IDE_DBG((IDE_PFX "AdapterControl: restart\n"));
        IdeReinitialize(hwInit);
        return ScsiAdapterControlSuccess;

    default:
        return ScsiAdapterControlUnsuccessful;
    }
}
#endif
