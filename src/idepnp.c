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
 * its own BARs and interrupt and is passed on unchanged. Built into the
 * 2000/XP driver only.
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

/* A copy of the list with the legacy IRQ appended */
static PCM_RESOURCE_LIST
IdeAppendLegacyIrqs(IN PCM_RESOURCE_LIST List, IN BOOLEAN Translated)
{
    PCM_RESOURCE_LIST copy;
    PCM_PARTIAL_RESOURCE_DESCRIPTOR src, d;
    ULONG size, count, i;

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
    for (i = 0; i < count; i++) {
        if (src[i].Type == CmResourceTypeDeviceSpecific) break;
        d[i] = src[i];
    }
    count = i;
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

static NTSTATUS NTAPI
IdePnpDispatch(IN PDEVICE_OBJECT Device, IN PIRP Irp)
{
    PIO_STACK_LOCATION stack = IoGetCurrentIrpStackLocation(Irp);
    NTSTATUS status;

    if (stack->MinorFunction == IRP_MN_START_DEVICE) {
        PCM_RESOURCE_LIST raw = stack->Parameters.StartDevice.AllocatedResources;
        PCM_RESOURCE_LIST translated = stack->Parameters.StartDevice.AllocatedResourcesTranslated;

        DbgPrint("[IDENT] PnP start: %lu descriptor(s), %lu port range(s), %lu interrupt(s)\n",
                 IdeDescriptorCount(raw), IdeResourceListCount(raw, CmResourceTypePort),
                 IdeResourceListCount(raw, CmResourceTypeInterrupt));

        /* A compatibility-mode function: no interrupt and at most the bus
           master range (BAR 4). A native-mode function has BAR 0-3 and its
           own interrupt, and is passed on as the PCI bus driver set it up. */
        if (raw != NULL && translated != NULL &&
            IdeResourceListCount(raw, CmResourceTypeInterrupt) == 0 &&
            IdeResourceListCount(raw, CmResourceTypePort) <= 1) {
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
                DbgPrint("[IDENT] PnP start: IRQ 14 added (vector %lX, IRQL %lu)\n",
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
