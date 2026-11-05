/*
 * IDENT - IRP_MJ_PNP and IRP_MJ_DEVICE_CONTROL front ends for Windows 2000/XP
 *
 * A PCI IDE function in compatibility mode raises ISA IRQ 14 and 15, which
 * the PCI bus driver does not report as its resources. ScsiPort refuses to
 * start a PCI adapter whose resource list has no interrupt, so the start
 * IRP of such a function gets IRQ 14 added on its way to ScsiPort: the raw
 * list the bus-relative number, the translated list what the HAL gives for
 * ISA IRQ 14. ScsiPort connects one interrupt for a PnP adapter; on x86
 * and x64 the driver connects IRQ 15 itself once the adapter has started
 * and runs ScsiPort's interrupt routine for it under ScsiPort's interrupt
 * lock, as ScsiPort does for the second interrupt of a non-PnP adapter
 * (see IdeConnectSecondary).
 * A native-mode function brings its own BARs and interrupt and is passed
 * on unchanged.
 *
 * The resource requirements of a native-mode function are kept off the
 * legacy ports while a compatibility-mode function decodes them: a BIOS
 * may leave BAR 0-3 at 1F0h/3F4h/170h/374h, nothing on record claims
 * those ports for the other function, and both controllers would answer.
 * Built into the 2000/XP driver only.
 */

#include <ntddk.h>
#include <srb.h>
#include <ntddscsi.h>

/* Not in every DDK's header for every platform; exported by every HAL */
ULONG NTAPI HalGetInterruptVector(INTERFACE_TYPE InterfaceType, ULONG BusNumber, ULONG BusInterruptLevel,
                                  ULONG BusInterruptVector, PKIRQL Irql, PKAFFINITY Affinity);

/* idemain.c: the secondary channel's interrupt on or off (synchronized) */
BOOLEAN IdeSecondaryInterrupt(IN PVOID HwInit, IN BOOLEAN On);

#define IDE_PNP_POOL_TAG        ((ULONG)'I' | ((ULONG)'D' << 8) | ((ULONG)'N' << 16) | ((ULONG)'T' << 24))

static PDRIVER_DISPATCH g_IdeOrigPnp;

typedef struct _IDE_PNP_LISTS {
    PDEVICE_OBJECT     Device;
    PCM_RESOURCE_LIST  Raw;
    PCM_RESOURCE_LIST  Translated;
} IDE_PNP_LISTS;

#define IDE_PNP_LIST_SLOTS      8
static IDE_PNP_LISTS g_IdePnpLists[IDE_PNP_LIST_SLOTS];

/* The secondary channel's IRQ 15 of a started compatibility-mode adapter */
typedef BOOLEAN (NTAPI *IDE_ISR)(PKINTERRUPT Interrupt, PVOID Context);
typedef struct _IDE_SECOND_IRQ {
    PDEVICE_OBJECT     Device;
    PVOID              HwInit;
    PKINTERRUPT        Ours;
    PKINTERRUPT        ScsiInterrupt;
    IDE_ISR            ScsiIsr;
    PVOID              ScsiContext;
    BOOLEAN            Deferred;        /* IRQ 15 above ScsiPort's level: through a DPC */
    BOOLEAN            Active;
    LONG               DpcBusy;
    KDPC               Dpc;
} IDE_SECOND_IRQ;

static IDE_SECOND_IRQ g_IdeSecondIrq[IDE_PNP_LIST_SLOTS];
static PDEVICE_OBJECT g_IdeStartingDevice;
static PVOID g_IdeStartingHwInit;

static VOID IdeConnectSecondary(IN PDEVICE_OBJECT Device, IN PVOID HwInit);
static VOID IdeDisconnectSecondary(IN PDEVICE_OBJECT Device);

/* The primary channel's IRQ only: ScsiPort connects one interrupt for a
   PnP adapter; IRQ 15 is connected after the start (IdeConnectSecondary) */
#define IDE_LEGACY_IRQ_COUNT    1
static const ULONG g_IdeLegacyIrq[IDE_LEGACY_IRQ_COUNT] = { 14 };

/* No CRT: byte loops for the few bytes moved here */
static VOID
IdePnpCopy(OUT PVOID Dst, IN PVOID Src, IN ULONG Bytes)
{
    PUCHAR d = (PUCHAR)Dst, s = (PUCHAR)Src;
    while (Bytes--) *d++ = *s++;
}

static VOID
IdePnpZero(OUT PVOID Dst, IN ULONG Bytes)
{
    PUCHAR d = (PUCHAR)Dst;
    while (Bytes--) *d++ = 0;
}

/* The descriptors of the first list. Reached through a byte offset: the
   headers declare PartialDescriptors[1] inside List[1], and an optimizing
   compiler may take that bound literally and look at descriptor 0 only. */
static PCM_PARTIAL_RESOURCE_DESCRIPTOR
IdeDescriptors(IN PCM_RESOURCE_LIST List)
{
    return (PCM_PARTIAL_RESOURCE_DESCRIPTOR)((PUCHAR)List +
               FIELD_OFFSET(CM_RESOURCE_LIST, List[0].PartialResourceList.PartialDescriptors));
}

static ULONG
IdeDescriptorCount(IN PCM_RESOURCE_LIST List)
{
    if (List == NULL || List->Count == 0) return 0;
    return List->List[0].PartialResourceList.Count;
}

/* Number of descriptors of one type in the first list */
static ULONG
IdeResourceListCount(IN PCM_RESOURCE_LIST List, IN UCHAR Type)
{
    PCM_PARTIAL_RESOURCE_DESCRIPTOR d;
    ULONG i, n = IdeDescriptorCount(List), found = 0;

    if (n == 0) return 0;
    d = IdeDescriptors(List);
    for (i = 0; i < n; i++) {
        if (d[i].Type == Type) found++;
    }
    return found;
}

/* Port ranges of 8 bytes: the command blocks of native-mode channels
   (BAR 0 / BAR 2); the bus master block is 16 bytes */
static ULONG
IdeCommandBlockCount(IN PCM_RESOURCE_LIST List)
{
    PCM_PARTIAL_RESOURCE_DESCRIPTOR d;
    ULONG i, n = IdeDescriptorCount(List), found = 0;

    if (n == 0) return 0;
    d = IdeDescriptors(List);
    for (i = 0; i < n; i++) {
        if (d[i].Type == CmResourceTypePort && d[i].u.Port.Length == 8) found++;
    }
    return found;
}

/* Size of a one-list CM_RESOURCE_LIST; 0 when it has more than one list */
static ULONG
IdeResourceListSize(IN PCM_RESOURCE_LIST List, OUT PULONG Count)
{
    PCM_PARTIAL_RESOURCE_DESCRIPTOR d;
    ULONG i, size;

    *Count = 0;
    if (List == NULL || List->Count != 1) return 0;
    *Count = IdeDescriptorCount(List);
    size = FIELD_OFFSET(CM_RESOURCE_LIST, List[0].PartialResourceList.PartialDescriptors);
    d = IdeDescriptors(List);
    for (i = 0; i < *Count; i++) {
        size += sizeof(CM_PARTIAL_RESOURCE_DESCRIPTOR);
        if (d[i].Type == CmResourceTypeDeviceSpecific) {
            size += d[i].u.DeviceSpecificData.DataSize;
            /* the data follows the descriptor; nothing after it is read */
            break;
        }
    }
    return size;
}

/* A copy of the list with the legacy IRQ in place of any interrupt */
static PCM_RESOURCE_LIST
IdeAppendLegacyIrqs(IN PCM_RESOURCE_LIST List, IN BOOLEAN Translated)
{
    PCM_RESOURCE_LIST copy;
    PCM_PARTIAL_RESOURCE_DESCRIPTOR src, d;
    ULONG size, count, i, n;

    size = IdeResourceListSize(List, &count);
    if (size == 0) return NULL;
    /* the device-specific data, if any, must stay last: appended entries
       go in front of it, so the copy is laid out again */
    copy = (PCM_RESOURCE_LIST)ExAllocatePoolWithTag(NonPagedPool,
                size + IDE_LEGACY_IRQ_COUNT * sizeof(CM_PARTIAL_RESOURCE_DESCRIPTOR), IDE_PNP_POOL_TAG);
    if (copy == NULL) return NULL;
    IdePnpCopy(copy, List, FIELD_OFFSET(CM_RESOURCE_LIST, List[0].PartialResourceList.PartialDescriptors));
    src = IdeDescriptors(List);
    d = IdeDescriptors(copy);
    for (i = 0, n = 0; i < count; i++) {
        if (src[i].Type == CmResourceTypeDeviceSpecific) break;
        if (src[i].Type == CmResourceTypeInterrupt) continue;    /* a PCI line the channels do not raise */
        d[n++] = src[i];
    }
    count = n;
    for (i = 0; i < IDE_LEGACY_IRQ_COUNT; i++, count++) {
        IdePnpZero(&d[count], sizeof(d[count]));
        d[count].Type = CmResourceTypeInterrupt;
        d[count].ShareDisposition = CmResourceShareDeviceExclusive;
        d[count].Flags = CM_RESOURCE_INTERRUPT_LATCHED;
        if (Translated) {
            KIRQL irql = 0;
            KAFFINITY affinity = 0;
            d[count].u.Interrupt.Vector = HalGetInterruptVector(Isa, 0, g_IdeLegacyIrq[i], g_IdeLegacyIrq[i],
                                                                &irql, &affinity);
            d[count].u.Interrupt.Level = irql;
            d[count].u.Interrupt.Affinity = affinity;
        } else {
            d[count].u.Interrupt.Level = g_IdeLegacyIrq[i];
            d[count].u.Interrupt.Vector = g_IdeLegacyIrq[i];
            d[count].u.Interrupt.Affinity = (KAFFINITY)-1;
        }
    }
    copy->List[0].PartialResourceList.Count = count;
    return copy;
}

static VOID
IdeFreePnpLists(IN PDEVICE_OBJECT Device)
{
    ULONG i;

    for (i = 0; i < IDE_PNP_LIST_SLOTS; i++) {
        if (g_IdePnpLists[i].Device == Device) {
            if (g_IdePnpLists[i].Raw != NULL) ExFreePool(g_IdePnpLists[i].Raw);
            if (g_IdePnpLists[i].Translated != NULL) ExFreePool(g_IdePnpLists[i].Translated);
            IdePnpZero(&g_IdePnpLists[i], sizeof(g_IdePnpLists[i]));
        }
    }
}

/* ------------------------------------------------------------------ */
/* Native-mode BARs off the legacy ports                               */
/* ------------------------------------------------------------------ */

#define IDE_PNP_IO_FLOOR        0x1000

/* A function with a channel in compatibility mode anywhere: it decodes
   the legacy ports, which no resource list has on record */
static BOOLEAN
IdeCompatFunctionPresent(VOID)
{
    PCI_SLOT_NUMBER slot;
    UCHAR cfg[16];
    ULONG bus, device, function;
    BOOLEAN multiFunction;

    for (bus = 0; bus < 256; bus++) {
        for (device = 0; device < 32; device++) {
            multiFunction = FALSE;
            for (function = 0; function < 8; function++) {
                if (function > 0 && !multiFunction) break;
                slot.u.AsULONG = 0;
                slot.u.bits.DeviceNumber = device;
                slot.u.bits.FunctionNumber = function;
                if (HalGetBusDataByOffset(PCIConfiguration, bus, slot.u.AsULONG, cfg, 0, sizeof(cfg)) < sizeof(cfg) ||
                    (cfg[0] == 0xFF && cfg[1] == 0xFF) || (cfg[0] == 0 && cfg[1] == 0)) {
                    if (function == 0) break;
                    continue;
                }
                if (function == 0) multiFunction = (BOOLEAN)((cfg[0x0E] & 0x80) != 0);
                if (cfg[0x0B] == 0x01 && cfg[0x0A] == 0x01 && (cfg[0x0E] & 0x7F) == 0 &&
                    (cfg[0x09] & 0x05) != 0x05) {
                    return TRUE;
                }
            }
        }
    }
    return FALSE;
}

/* The compatibility-mode function about to start owns the legacy ports.
   A function with all channels in native mode whose BARs a BIOS left on
   those ports would answer as well until the PCI bus driver starts it
   with the new BARs (see below), so its I/O decoding is turned off until
   then; the bus driver turns it on again at its start. */
static VOID
IdeQuietNativeOnLegacyPorts(VOID)
{
    PCI_SLOT_NUMBER slot;
    UCHAR cfg[0x20];
    ULONG bus, device, function, i, bar;
    USHORT command;
    BOOLEAN multiFunction, legacy;

    for (bus = 0; bus < 256; bus++) {
        for (device = 0; device < 32; device++) {
            multiFunction = FALSE;
            for (function = 0; function < 8; function++) {
                if (function > 0 && !multiFunction) break;
                slot.u.AsULONG = 0;
                slot.u.bits.DeviceNumber = device;
                slot.u.bits.FunctionNumber = function;
                if (HalGetBusDataByOffset(PCIConfiguration, bus, slot.u.AsULONG, cfg, 0, sizeof(cfg)) < sizeof(cfg) ||
                    (cfg[0] == 0xFF && cfg[1] == 0xFF) || (cfg[0] == 0 && cfg[1] == 0)) {
                    if (function == 0) break;
                    continue;
                }
                if (function == 0) multiFunction = (BOOLEAN)((cfg[0x0E] & 0x80) != 0);
                if (cfg[0x0B] != 0x01 || (cfg[0x0E] & 0x7F) != 0) continue;
                if (cfg[0x0A] == 0x01 && (cfg[0x09] & 0x05) != 0x05) continue;
                if (cfg[0x0A] != 0x01 && cfg[0x0A] != 0x04 && cfg[0x0A] != 0x80) continue;
                command = (USHORT)(cfg[0x04] | (cfg[0x05] << 8));
                if (!(command & 1)) continue;
                legacy = FALSE;
                for (i = 0; i < 4; i++) {
                    bar = (ULONG)cfg[0x10 + 4 * i] | ((ULONG)cfg[0x11 + 4 * i] << 8);
                    if (!(bar & 1)) continue;
                    bar &= 0xFFFC;
                    if (bar == 0x1F0 || bar == 0x170 || bar == 0x3F4 || bar == 0x374 ||
                        bar == 0x3F6 || bar == 0x376) {
                        legacy = TRUE;
                    }
                }
                if (!legacy) continue;
                command &= ~1;
                HalSetBusDataByOffset(PCIConfiguration, bus, slot.u.AsULONG, &command, 0x04, sizeof(command));
                DbgPrint("[IDENT] PnP start: bus %lu slot %08lX (native mode on the legacy ports) quiet until its start\n",
                         bus, slot.u.AsULONG);
            }
        }
    }
}

/* The command and control blocks (8 and 4 bytes) of a native-mode
   function may not be put below 1000h */
static VOID
IdeFilterRequirements(IN PIO_RESOURCE_REQUIREMENTS_LIST Req)
{
    PIO_RESOURCE_DESCRIPTOR d;
    PUCHAR p;
    UCHAR cfg[16];
    ULONG a, i, changed = 0;

    if (Req == NULL || Req->InterfaceType != PCIBus) return;
    if (HalGetBusDataByOffset(PCIConfiguration, Req->BusNumber, Req->SlotNumber, cfg, 0, sizeof(cfg)) < sizeof(cfg)) return;
    if (cfg[0x0B] != 0x01) return;
    if (cfg[0x0A] == 0x01 && (cfg[0x09] & 0x05) == 0) return;      /* no native channel */
    if (!IdeCompatFunctionPresent()) return;

    p = (PUCHAR)Req + FIELD_OFFSET(IO_RESOURCE_REQUIREMENTS_LIST, List);
    for (a = 0; a < Req->AlternativeLists; a++) {
        PIO_RESOURCE_LIST list = (PIO_RESOURCE_LIST)p;

        d = (PIO_RESOURCE_DESCRIPTOR)(p + FIELD_OFFSET(IO_RESOURCE_LIST, Descriptors));
        for (i = 0; i < list->Count; i++) {
            ULONG length = d[i].u.Port.Length;

            if (d[i].Type != CmResourceTypePort || length == 0 || length > 8) continue;
            if (d[i].u.Port.MinimumAddress.QuadPart >= IDE_PNP_IO_FLOOR) continue;
            if (d[i].u.Port.MaximumAddress.QuadPart < IDE_PNP_IO_FLOOR + length - 1) {
                /* a fixed (preferred) setting at the old place */
                d[i].u.Port.MaximumAddress.QuadPart = 0xFFFF;
                d[i].u.Port.Alignment = length;
            }
            d[i].u.Port.MinimumAddress.QuadPart = IDE_PNP_IO_FLOOR;
            changed++;
        }
        p = (PUCHAR)&d[list->Count];
    }
    if (changed != 0) {
        DbgPrint("[IDENT] PnP requirements: bus %lu slot %08lX, %lu port range(s) kept off the legacy ports\n",
                 Req->BusNumber, Req->SlotNumber, changed);
    }
}

static NTSTATUS NTAPI
IdePnpDispatch(IN PDEVICE_OBJECT Device, IN PIRP Irp)
{
    PIO_STACK_LOCATION stack = IoGetCurrentIrpStackLocation(Irp);
    UCHAR minor = stack->MinorFunction;
    BOOLEAN compatStart = FALSE;
    NTSTATUS status;

    if (stack->MinorFunction == IRP_MN_FILTER_RESOURCE_REQUIREMENTS) {
        PIO_RESOURCE_REQUIREMENTS_LIST req = (PIO_RESOURCE_REQUIREMENTS_LIST)Irp->IoStatus.Information;
        if (req == NULL) req = stack->Parameters.FilterResourceRequirements.IoResourceRequirementList;
        IdeFilterRequirements(req);
    }

    if (stack->MinorFunction == IRP_MN_START_DEVICE) {
        PCM_RESOURCE_LIST raw = stack->Parameters.StartDevice.AllocatedResources;
        PCM_RESOURCE_LIST translated = stack->Parameters.StartDevice.AllocatedResourcesTranslated;

        DbgPrint("[IDENT] PnP start: %lu descriptor(s), %lu port range(s), %lu interrupt(s)\n",
                 IdeDescriptorCount(raw), IdeResourceListCount(raw, CmResourceTypePort),
                 IdeResourceListCount(raw, CmResourceTypeInterrupt));

        /* A compatibility-mode function: no command block of its own (the
           bus driver does not report BAR 0-3 of a legacy channel), maybe
           a bus master or other BAR, and a PCI interrupt if the BIOS gave
           it one, which the channels do not raise. A native-mode function
           has BAR 0-3 and is passed on as the PCI bus driver set it up. */
        if (raw != NULL && translated != NULL && IdeCommandBlockCount(raw) == 0) {
            IdeQuietNativeOnLegacyPorts();
            PCM_RESOURCE_LIST newRaw = IdeAppendLegacyIrqs(raw, FALSE);
            PCM_RESOURCE_LIST newTranslated = IdeAppendLegacyIrqs(translated, TRUE);
            ULONG i;

            if (newRaw != NULL && newTranslated != NULL) {
                PCM_PARTIAL_RESOURCE_DESCRIPTOR added;

                IdeFreePnpLists(Device);
                for (i = 0; i < IDE_PNP_LIST_SLOTS; i++) {
                    if (g_IdePnpLists[i].Device == NULL) {
                        g_IdePnpLists[i].Device = Device;
                        g_IdePnpLists[i].Raw = newRaw;
                        g_IdePnpLists[i].Translated = newTranslated;
                        break;
                    }
                }
                stack->Parameters.StartDevice.AllocatedResources = newRaw;
                stack->Parameters.StartDevice.AllocatedResourcesTranslated = newTranslated;
                added = &IdeDescriptors(newTranslated)[IdeDescriptorCount(newTranslated) - 1];
                DbgPrint("[IDENT] PnP start: compatibility mode, IRQ 14 connected (vector %lX, IRQL %lu)\n",
                         added->u.Interrupt.Vector, added->u.Interrupt.Level);
                compatStart = TRUE;
            } else {
                if (newRaw != NULL) ExFreePool(newRaw);
                if (newTranslated != NULL) ExFreePool(newTranslated);
            }
        }
    }

    if (minor == IRP_MN_STOP_DEVICE || minor == IRP_MN_REMOVE_DEVICE || minor == IRP_MN_SURPRISE_REMOVAL) {
        IdeDisconnectSecondary(Device);
    }
    if (compatStart) {
        IdeDisconnectSecondary(Device);
        g_IdeStartingHwInit = NULL;
        g_IdeStartingDevice = Device;
    }

    status = g_IdeOrigPnp(Device, Irp);

    if (compatStart) {
        /* ScsiPort starts the adapter within the IRP (HwFindAdapter,
           its interrupt, HwInitialize) */
        if (NT_SUCCESS(status) && status != STATUS_PENDING && g_IdeStartingHwInit != NULL) {
            IdeConnectSecondary(Device, g_IdeStartingHwInit);
        }
        g_IdeStartingDevice = NULL;
        g_IdeStartingHwInit = NULL;
    }
    if (minor == IRP_MN_REMOVE_DEVICE) IdeFreePnpLists(Device);
    return status;
}

/* ------------------------------------------------------------------ */
/* IRQ 15 for the secondary channel                                    */
/* ------------------------------------------------------------------ */

/* HwFindAdapter of a compatibility-mode function started through the
   hooked start IRP: the adapter whose IRQ 15 is connected afterwards */
VOID
IdePnpNoteAdapter(IN PVOID HwInit)
{
    if (g_IdeStartingDevice != NULL) g_IdeStartingHwInit = HwInit;
}

#if defined(_WIN64) && (defined(_AMD64_) || defined(_M_AMD64) || defined(__x86_64__))
/* KINTERRUPT of Server 2003 SP1 / XP x64 (KeInitializeInterrupt of 3790.1830) */
#define IDE_SECOND_IRQ_HOOK     1
#define IDE_KI_TYPE             0x00        /* SHORT, 22 = InterruptObject */
#define IDE_KI_SIZE             0x02
#define IDE_KI_SERVICE_ROUTINE  0x18
#define IDE_KI_SERVICE_CONTEXT  0x20
#define IDE_KI_ACTUAL_LOCK      0x38
#define IDE_KI_VECTOR           0x48
#define IDE_KI_IRQL             0x4C
#define IDE_KI_SYNC_IRQL        0x4D
#define IDE_KI_CONNECTED        0x4F
#define IDE_KI_MIN_SIZE         0x58
#define IDE_KI_TYPE_INTERRUPT   22
#elif defined(_X86_) && !defined(_WIN64)
/* KINTERRUPT of NT 4.0 to Server 2003 x86 */
#define IDE_SECOND_IRQ_HOOK     1
#define IDE_KI_TYPE             0x00        /* SHORT, 22 = InterruptObject */
#define IDE_KI_SIZE             0x02
#define IDE_KI_SERVICE_ROUTINE  0x0C
#define IDE_KI_SERVICE_CONTEXT  0x10
#define IDE_KI_ACTUAL_LOCK      0x1C
#define IDE_KI_VECTOR           0x24
#define IDE_KI_IRQL             0x28
#define IDE_KI_SYNC_IRQL        0x29
#define IDE_KI_CONNECTED        0x2B
#define IDE_KI_MIN_SIZE         0x30
#define IDE_KI_TYPE_INTERRUPT   22
#endif

#ifdef IDE_SECOND_IRQ_HOOK

/* ScsiPort's interrupt routine, called under its interrupt lock */
static BOOLEAN NTAPI
IdeSecondScsiIsr(IN PVOID Context)
{
    IDE_SECOND_IRQ *e = (IDE_SECOND_IRQ *)Context;
    return e->ScsiIsr(e->ScsiInterrupt, e->ScsiContext);
}

static BOOLEAN NTAPI
IdeSecondIsr(IN PKINTERRUPT Interrupt, IN PVOID Context)
{
    IDE_SECOND_IRQ *e = (IDE_SECOND_IRQ *)Context;
    (VOID)Interrupt;
    if (!e->Deferred) {
        /* Connected with ScsiPort's lock at its level: as for its own line */
        return IdeSecondScsiIsr(e);
    }
    /* IRQ 15 above ScsiPort's level: the line is ours alone (edge, not
       shared), ScsiPort's routine follows at its level from a DPC; the
       channel's INTRQ stays until its status register is read there */
    if (e->Active) KeInsertQueueDpc(&e->Dpc, NULL, NULL);
    return TRUE;
}

static VOID NTAPI
IdeSecondDpc(IN PKDPC Dpc, IN PVOID Context, IN PVOID A1, IN PVOID A2)
{
    IDE_SECOND_IRQ *e = (IDE_SECOND_IRQ *)Context;
    (VOID)Dpc; (VOID)A1; (VOID)A2;
    InterlockedIncrement(&e->DpcBusy);
    if (e->Active) KeSynchronizeExecution(e->ScsiInterrupt, IdeSecondScsiIsr, e);
    InterlockedDecrement(&e->DpcBusy);
}

static BOOLEAN NTAPI
IdeSecondOn(IN PVOID Context)
{
    return IdeSecondaryInterrupt(((IDE_SECOND_IRQ *)Context)->HwInit, TRUE);
}

static BOOLEAN NTAPI
IdeSecondOff(IN PVOID Context)
{
    return IdeSecondaryInterrupt(((IDE_SECOND_IRQ *)Context)->HwInit, FALSE);
}

/* ScsiPort's interrupt object of the adapter: a pointer in its device
   extension (the miniport's extension is preceded by a pointer to it)
   to an interrupt object connected to IRQ 14 for this device object.
   Each candidate is checked to be readable memory before it is read. */
static PUCHAR
IdeFindScsiInterrupt(IN PDEVICE_OBJECT Device, IN PVOID HwInit, IN ULONG Vector, IN KIRQL Irql)
{
    PUCHAR fdo, ki;
    ULONG off;

    if (!MmIsAddressValid((PUCHAR)HwInit - sizeof(PVOID))) return NULL;
    fdo = *(PUCHAR *)((PUCHAR)HwInit - sizeof(PVOID));
    if (fdo == NULL) return NULL;
    for (off = 0; off < 0x600; off += sizeof(PVOID)) {
        if (!MmIsAddressValid(fdo + off) || !MmIsAddressValid(fdo + off + sizeof(PVOID) - 1)) return NULL;
        ki = *(PUCHAR *)(fdo + off);
        if (ki == NULL || ((ULONG_PTR)ki & (sizeof(PVOID) - 1)) != 0) continue;
        if (!MmIsAddressValid(ki) || !MmIsAddressValid(ki + IDE_KI_MIN_SIZE - 1)) continue;
        if (*(PSHORT)(ki + IDE_KI_TYPE) != IDE_KI_TYPE_INTERRUPT) continue;
        if (*(PSHORT)(ki + IDE_KI_SIZE) < IDE_KI_MIN_SIZE) continue;
        if (*(PULONG)(ki + IDE_KI_VECTOR) != Vector || *(PUCHAR)(ki + IDE_KI_IRQL) != Irql) continue;
        if (*(PUCHAR)(ki + IDE_KI_CONNECTED) != 1) continue;
        if (*(PDEVICE_OBJECT *)(ki + IDE_KI_SERVICE_CONTEXT) != Device) continue;
        return ki;
    }
    return NULL;
}

/* The adapter has started: IRQ 15 connected. Where its level is no
   higher than ScsiPort's synchronization level, with ScsiPort's lock, and
   ScsiPort's interrupt routine runs from it directly. Above that level
   (APIC HALs) the lock could be taken on a processor that holds it
   already (ScsiPort's routines hold it at their level), so the interrupt
   only queues a DPC, which runs ScsiPort's routine synchronized with its
   interrupt. Not done: the secondary channel stays with Fast Polling, as
   before. */
static VOID
IdeConnectSecondary(IN PDEVICE_OBJECT Device, IN PVOID HwInit)
{
    IDE_SECOND_IRQ *e = NULL;
    PUCHAR ki;
    KIRQL irql14 = 0, irql15 = 0, sync;
    KAFFINITY aff14 = 0, aff15 = 0;
    ULONG vector14, vector15, i;
    NTSTATUS status;

    for (i = 0; i < IDE_PNP_LIST_SLOTS; i++) {
        if (g_IdeSecondIrq[i].Device == NULL) { e = &g_IdeSecondIrq[i]; break; }
    }
    if (e == NULL) return;
    vector14 = HalGetInterruptVector(Isa, 0, 14, 14, &irql14, &aff14);
    vector15 = HalGetInterruptVector(Isa, 0, 15, 15, &irql15, &aff15);
    if (vector15 == 0 || aff15 == 0) {
        DbgPrint("[IDENT] PnP start: no vector for IRQ 15, secondary channel with Fast Polling\n");
        return;
    }
    ki = IdeFindScsiInterrupt(Device, HwInit, vector14, irql14);
    if (ki == NULL) {
        DbgPrint("[IDENT] PnP start: ScsiPort's interrupt not found, secondary channel with Fast Polling\n");
        return;
    }
    sync = *(PUCHAR)(ki + IDE_KI_SYNC_IRQL);
    IdePnpZero(e, sizeof(*e));
    e->ScsiInterrupt = (PKINTERRUPT)ki;
    e->ScsiIsr = *(IDE_ISR *)(ki + IDE_KI_SERVICE_ROUTINE);
    e->ScsiContext = *(PVOID *)(ki + IDE_KI_SERVICE_CONTEXT);
    e->HwInit = HwInit;
    e->Deferred = (BOOLEAN)(irql15 > sync);
    KeInitializeDpc(&e->Dpc, IdeSecondDpc, e);
    e->Active = TRUE;
    if (e->Deferred) {
        status = IoConnectInterrupt(&e->Ours, IdeSecondIsr, e, NULL, vector15, irql15, irql15,
                                    Latched, FALSE, aff15, FALSE);
    } else {
        status = IoConnectInterrupt(&e->Ours, IdeSecondIsr, e, *(PKSPIN_LOCK *)(ki + IDE_KI_ACTUAL_LOCK),
                                    vector15, irql15, sync, Latched, FALSE, aff15, FALSE);
    }
    if (!NT_SUCCESS(status)) {
        DbgPrint("[IDENT] PnP start: IRQ 15 not connected (%08lX), secondary channel with Fast Polling\n", status);
        IdePnpZero(e, sizeof(*e));
        return;
    }
    e->Device = Device;
    if (!KeSynchronizeExecution(e->ScsiInterrupt, IdeSecondOn, e)) {
        e->Active = FALSE;
        IoDisconnectInterrupt(e->Ours);
        KeRemoveQueueDpc(&e->Dpc);
        while (e->DpcBusy != 0) KeStallExecutionProcessor(10);
        IdePnpZero(e, sizeof(*e));
        DbgPrint("[IDENT] PnP start: secondary channel with Fast Polling (interrupts off)\n");
        return;
    }
    DbgPrint("[IDENT] PnP start: IRQ 15 connected (vector %lX, IRQL %lu%s), secondary channel interrupt-driven\n",
             vector15, (ULONG)irql15, e->Deferred ? ", through a DPC" : "");
}

/* Stop or removal: our IRQ 15 off before ScsiPort lets go of its
   interrupt (and its lock) */
static VOID
IdeDisconnectSecondary(IN PDEVICE_OBJECT Device)
{
    ULONG i;

    for (i = 0; i < IDE_PNP_LIST_SLOTS; i++) {
        IDE_SECOND_IRQ *e = &g_IdeSecondIrq[i];
        if (e->Device != Device || e->Ours == NULL) continue;
        KeSynchronizeExecution(e->ScsiInterrupt, IdeSecondOff, e);
        e->Active = FALSE;
        IoDisconnectInterrupt(e->Ours);
        KeRemoveQueueDpc(&e->Dpc);
        while (e->DpcBusy != 0) KeStallExecutionProcessor(10);
        IdePnpZero(e, sizeof(*e));
        DbgPrint("[IDENT] PnP: IRQ 15 disconnected\n");
    }
}
#else
static VOID
IdeConnectSecondary(IN PDEVICE_OBJECT Device, IN PVOID HwInit)
{
    (VOID)Device;
    (VOID)HwInit;
}

static VOID
IdeDisconnectSecondary(IN PDEVICE_OBJECT Device)
{
    (VOID)Device;
}
#endif

/* ------------------------------------------------------------------ */
/* IOCTL_SCSI_MINIPORT sent to a disk's PDO                            */
/* ------------------------------------------------------------------ */

/* disk.sys sends its SMART requests (SMART_GET_VERSION, SMART_RCV_DRIVE_DATA,
   SMART_SEND_DRIVE_COMMAND and its own failure prediction) as
   IOCTL_SCSI_MINIPORT to the disk's PDO. ScsiPort of Server 2003 / XP x64
   answers that IOCTL on a PDO with STATUS_INVALID_DEVICE_REQUEST (only the
   adapter's device object takes it). The front end turns it into the
   SRB_FUNCTION_IO_CONTROL request ScsiPort builds for the adapter, sent
   through the PDO, which puts the disk's address into it. Same on every
   2000/XP version: the miniport sees the request as before. */
static PDRIVER_DISPATCH g_IdeOrigDeviceControl;

static NTSTATUS NTAPI
IdePdoIoctlDone(IN PDEVICE_OBJECT Device, IN PIRP Irp, IN PVOID Context)
{
    (VOID)Device;
    (VOID)Irp;
    KeSetEvent((PKEVENT)Context, IO_NO_INCREMENT, FALSE);
    return STATUS_MORE_PROCESSING_REQUIRED;
}

static NTSTATUS
IdePdoMiniportIoctl(IN PDEVICE_OBJECT Device, IN PIRP Irp)
{
    PIO_STACK_LOCATION stack = IoGetCurrentIrpStackLocation(Irp), next;
    PSRB_IO_CONTROL ctl = (PSRB_IO_CONTROL)Irp->AssociatedIrp.SystemBuffer;
    ULONG inLen = stack->Parameters.DeviceIoControl.InputBufferLength;
    ULONG outLen = stack->Parameters.DeviceIoControl.OutputBufferLength;
    PSCSI_REQUEST_BLOCK srb;
    ULONG length;
    KEVENT event;
    PMDL mdl;
    NTSTATUS status;
    PIRP irp;

    if (ctl == NULL || inLen < sizeof(SRB_IO_CONTROL)) {
        status = STATUS_INVALID_PARAMETER;
        goto done;
    }
    if (ctl->HeaderLength != sizeof(SRB_IO_CONTROL)) {
        status = STATUS_REVISION_MISMATCH;
        goto done;
    }
    length = ctl->HeaderLength + ctl->Length;
    if (length < ctl->Length) {
        status = STATUS_INVALID_PARAMETER;
        goto done;
    }
    if (inLen < length && outLen < length) {
        status = STATUS_BUFFER_TOO_SMALL;
        goto done;
    }
    srb = (PSCSI_REQUEST_BLOCK)ExAllocatePoolWithTag(NonPagedPool, sizeof(SCSI_REQUEST_BLOCK), IDE_PNP_POOL_TAG);
    irp = IoAllocateIrp(Device->StackSize, FALSE);
    /* ScsiPort maps the data of a request through the IRP's MDL: one for
       the buffered IOCTL's system buffer (nonpaged pool) */
    mdl = (irp != NULL) ? IoAllocateMdl(ctl, length, FALSE, FALSE, irp) : NULL;
    if (srb == NULL || irp == NULL || mdl == NULL) {
        if (srb != NULL) ExFreePool(srb);
        if (mdl != NULL) IoFreeMdl(mdl);
        if (irp != NULL) { irp->MdlAddress = NULL; IoFreeIrp(irp); }
        status = STATUS_INSUFFICIENT_RESOURCES;
        goto done;
    }
    MmBuildMdlForNonPagedPool(mdl);
    IdePnpZero(srb, sizeof(*srb));
    srb->Length = sizeof(SCSI_REQUEST_BLOCK);
    srb->Function = SRB_FUNCTION_IO_CONTROL;
    srb->SrbFlags = SRB_FLAGS_DATA_IN | SRB_FLAGS_DATA_OUT | SRB_FLAGS_NO_QUEUE_FREEZE;
    srb->DataBuffer = ctl;
    srb->DataTransferLength = length;
    srb->TimeOutValue = ctl->Timeout;
    srb->OriginalRequest = irp;
    KeInitializeEvent(&event, NotificationEvent, FALSE);
    next = IoGetNextIrpStackLocation(irp);
    next->MajorFunction = IRP_MJ_SCSI;
    next->Parameters.Scsi.Srb = srb;
    IoSetCompletionRoutine(irp, IdePdoIoctlDone, &event, TRUE, TRUE, TRUE);
    if (IoCallDriver(Device, irp) == STATUS_PENDING) {
        KeWaitForSingleObject(&event, Executive, KernelMode, FALSE, NULL);
    }
    status = irp->IoStatus.Status;
    irp->MdlAddress = NULL;
    IoFreeMdl(mdl);
    IoFreeIrp(irp);
    ExFreePool(srb);
    Irp->IoStatus.Information = length < outLen ? length : outLen;
    Irp->IoStatus.Status = status;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return status;

done:
    Irp->IoStatus.Information = 0;
    Irp->IoStatus.Status = status;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return status;
}

/* ScsiPort creates the adapter's device object as FILE_DEVICE_CONTROLLER,
   its PDOs with the type of the device (disk, CD-ROM, mass storage) */
static NTSTATUS NTAPI
IdeDeviceControlDispatch(IN PDEVICE_OBJECT Device, IN PIRP Irp)
{
    PIO_STACK_LOCATION stack = IoGetCurrentIrpStackLocation(Irp);

    if (Device->DeviceType != FILE_DEVICE_CONTROLLER &&
        stack->Parameters.DeviceIoControl.IoControlCode == IOCTL_SCSI_MINIPORT &&
        KeGetCurrentIrql() == PASSIVE_LEVEL) {
        return IdePdoMiniportIoctl(Device, Irp);
    }
    return g_IdeOrigDeviceControl(Device, Irp);
}

/* After ScsiPortInitialize: ScsiPort's IRP_MJ_PNP and IRP_MJ_DEVICE_CONTROL
   entries get a front end */
VOID
IdeHookPnp(IN PVOID DriverObject)
{
    PDRIVER_OBJECT driver = (PDRIVER_OBJECT)DriverObject;

    if (driver == NULL) return;
    if (driver->MajorFunction[IRP_MJ_DEVICE_CONTROL] != NULL &&
        driver->MajorFunction[IRP_MJ_DEVICE_CONTROL] != IdeDeviceControlDispatch) {
        g_IdeOrigDeviceControl = driver->MajorFunction[IRP_MJ_DEVICE_CONTROL];
        driver->MajorFunction[IRP_MJ_DEVICE_CONTROL] = IdeDeviceControlDispatch;
    }
    if (driver->MajorFunction[IRP_MJ_PNP] == NULL ||
        driver->MajorFunction[IRP_MJ_PNP] == IdePnpDispatch) {
        return;
    }
    g_IdeOrigPnp = driver->MajorFunction[IRP_MJ_PNP];
    driver->MajorFunction[IRP_MJ_PNP] = IdePnpDispatch;
    DbgPrint("[IDENT] PnP dispatch hooked\n");
}
