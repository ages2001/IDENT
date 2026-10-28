/*
 * IDENT - IRP_MJ_PNP start-device hook for Windows 2000/XP
 *
 * A PCI IDE function in compatibility mode raises ISA IRQ 14 and 15, which
 * the PCI bus driver does not report as its resources. ScsiPort refuses to
 * start a PCI adapter whose resource list has no interrupt, so the start
 * IRP of such a function gets IRQ 14 added on its way to ScsiPort: the raw
 * list the bus-relative number, the translated list what the HAL gives for
 * ISA IRQ 14. ScsiPort connects one interrupt for a PnP adapter, so the
 * secondary channel runs with Fast Polling. A native-mode function brings
 * its own BARs and interrupt and is passed on unchanged.
 *
 * The resource requirements of a native-mode function are kept off the
 * legacy ports while a compatibility-mode function decodes them: a BIOS
 * may leave BAR 0-3 at 1F0h/3F4h/170h/374h, nothing on record claims
 * those ports for the other function, and both controllers would answer.
 * Built into the 2000/XP driver only.
 */

#include <ntddk.h>

/* Not in every DDK's header for every platform; exported by every HAL */
ULONG NTAPI HalGetInterruptVector(INTERFACE_TYPE InterfaceType, ULONG BusNumber, ULONG BusInterruptLevel,
                                  ULONG BusInterruptVector, PKIRQL Irql, PKAFFINITY Affinity);

#define IDE_PNP_POOL_TAG        ((ULONG)'I' | ((ULONG)'D' << 8) | ((ULONG)'N' << 16) | ((ULONG)'T' << 24))

static PDRIVER_DISPATCH g_IdeOrigPnp;

typedef struct _IDE_PNP_LISTS {
    PDEVICE_OBJECT     Device;
    PCM_RESOURCE_LIST  Raw;
    PCM_RESOURCE_LIST  Translated;
} IDE_PNP_LISTS;

#define IDE_PNP_LIST_SLOTS      8
static IDE_PNP_LISTS g_IdePnpLists[IDE_PNP_LIST_SLOTS];

/* The primary channel's IRQ only: ScsiPort connects one interrupt for a
   PnP adapter, the secondary channel runs with Fast Polling */
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
            } else {
                if (newRaw != NULL) ExFreePool(newRaw);
                if (newTranslated != NULL) ExFreePool(newTranslated);
            }
        }
    }

    status = g_IdeOrigPnp(Device, Irp);

    if (stack->MinorFunction == IRP_MN_REMOVE_DEVICE) IdeFreePnpLists(Device);
    return status;
}

/* After ScsiPortInitialize: ScsiPort's IRP_MJ_PNP entry gets a front end */
VOID
IdeHookPnp(IN PVOID DriverObject)
{
    PDRIVER_OBJECT driver = (PDRIVER_OBJECT)DriverObject;

    if (driver == NULL || driver->MajorFunction[IRP_MJ_PNP] == NULL ||
        driver->MajorFunction[IRP_MJ_PNP] == IdePnpDispatch) {
        return;
    }
    g_IdeOrigPnp = driver->MajorFunction[IRP_MJ_PNP];
    driver->MajorFunction[IRP_MJ_PNP] = IdePnpDispatch;
    DbgPrint("[IDENT] PnP dispatch hooked\n");
}
