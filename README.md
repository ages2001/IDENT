# IDENT — PATA/SATA IDE Driver for Windows 95/98/Me and Windows NT 3.1 to XP x64

**IDENT** is a SCSI miniport driver for IDE controllers: parallel ATA, and SATA controllers running in IDE mode. It replaces the in-box `atdisk`, `atapi` and `ESDI_506` drivers of Windows 95, 98 and Me, Windows NT 3.1, 3.50, 3.51 and 4.0, and Windows 2000, XP and Server 2003 (x86 and x64) with one driver that gives every one of these systems UDMA, 48-bit LBA, native-mode controllers on any interrupt, large sectors, SMART and TRIM.

| | |
|---|---|
| **Version** | 1.1.0.0 |
| **Operating systems** | Windows 95 / 98 / Me · Windows NT 3.1 / 3.50 / 3.51 / 4.0 · Windows 2000 / XP / Server 2003 (x86) · Windows XP x64 / Server 2003 x64 |
| **Hardware** | PCI IDE controllers (class code `01-01`) in compatibility or native mode; SATA controllers in IDE mode (Intel, VIA, AMD, nVidia, SiS, ALi and others; Silicon Image 3112/3512/3114 and ATI IXP SATA); legacy ISA/VLB channels; up to 32 controllers |
| **Devices** | PATA and SATA disks (48-bit LBA, 512-byte to 4 KB logical sectors, SSDs with TRIM), ATAPI CD/DVD drives, removable ATAPI disks (ZIP, LS-120/SuperDisk) |
| **Binaries** | `ident9x.mpd` (Windows 9x/Me) · `ident.sys` (NT 3.1–4.0) · `ident.sys` (2000/XP/2003, x86 and x64) |

## Highlights

- **The fastest transfer mode, whatever the BIOS did**: UDMA 0–6 and MWDMA with the controller timing programmed on Intel PIIX/ICH, CMD 64x and Silicon Image 311x, BIOS settings kept elsewhere, and PIO as the fallback for every case DMA cannot handle
- **Native (enhanced) mode controllers** on any interrupt, compatibility mode on IRQ 14/15, and legacy ISA channels — on every target, NT 3.1 included
- **Every controller in the machine**, side by side: on the NT family a native-mode controller that the BIOS left on the legacy ports is moved to free I/O space, so it no longer hides the drives of the compatibility-mode controller (see [Several controllers in one machine](#several-controllers-in-one-machine))
- **Interrupt-driven DMA** with a watchdog that falls back to Fast Polling where the interrupt routing is wrong
- **Chip errata** handled as Linux does: CMD 64x interrupt latches, SiI3112 "mod15write", RZ1000/CMD640 read-ahead, ALi, ServerWorks and Intel 450NX limits
- **48-bit LBA, 16-byte CDBs and 4 KB sectors** on every target; **TRIM** for SATA SSDs, plus `tools\trimnt.exe` for the free space of a volume
- **SMART** through the `atapi.sys`-compatible "SCSIDISK" IOCTL, ATA PASS-THROUGH, and LOG SENSE health and temperature pages
- **Clean takeover**: the in-box IDE drivers are turned off on installation (and restored on removal on NT), the F6 and OEM setup disks keep the controller on IDENT
- **Power management**: flush at shutdown (standby with `STANDBY=1`), hibernation and crash dumps on 2000/XP, a power-down IOCTL for NT 3.1–4.0 and 9x

- **Channels tab in Device Manager** (2000/XP/x64): check boxes to turn single channels and devices off, without editing the registry
## Quick start

| System | Files | How |
|---|---|---|
| Windows 95 / 98 / Me | `bin\9X` | [Installing on Windows 95 / 98 / Me](#windows-95--98--me) |
| Windows NT 3.1 – 4.0 | `bin\NT\floppy` | [Installing on Windows NT](#windows-nt-31--350--351--nt-40) |
| Windows 2000 / XP / 2003, installed | `bin\2KXP` (Update Driver, Have Disk) | [GUI-mode setup](#gui-mode-setup-windows-2000xpxp-x64server-2003) |
| Windows 2000 / XP / 2003, new installation | `bin\2KXP\floppy_i386` or `floppy_amd64` (F6) | [Text-mode (F6) setup](#text-mode-f6-setup--windows-2000xpxp-x64server-2003) |

Read [In-box IDE drivers](#in-box-ide-drivers) before installing on a machine with more than one IDE controller.

## Contents

- [Overview](#overview) · [Features](#features) · [Architecture](#architecture) · [Feature matrix by target](#feature-matrix-by-target) · [Known Limitations](#known-limitations)
- [Building from source](#building-from-source) · [Installation](#installation) · [Configuration](#configuration) · [Debugging](#debugging) · [Technical Notes](#technical-notes)
- [Version history](#version-history) · [Related drivers](#related-drivers) · [License](#license)

## Overview

IDENT is a SCSI miniport driver built on the classic `ScsiPort` framework (`SCSIPORT.PDR` on Windows 9x/Me, `scsiport.sys` on the NT family). It talks to the ATA task-file registers and the PCI bus-master (SFF-8038i) DMA engine of an IDE channel and translates the SCSI requests of the Windows storage stack into ATA/ATAPI commands, so any PATA or SATA-in-IDE-mode controller can be used as a boot and data device with the same feature set on every target: UDMA/MWDMA with PIO fallback, 48-bit LBA, large logical sectors, SMART, ATA PASS-THROUGH, ATAPI, power management.

### Why?

Because the in-box `atdisk`/`atapi` drivers of these systems are tied to their era: NT 3.1's `atdisk` is PIO-only at ports 1F0h/170h, NT 4.0's `atapi.sys` knows DMA only for a handful of chipsets, and none of them handles a controller in native PCI mode with an arbitrary interrupt, SATA controllers running as IDE, 48-bit addressing on the older systems, or logical sectors other than 512 bytes. IDENT does all of this from one driver, with the same SCSI translation layer, IOCTL interface and Fast Polling fallback as AHCINT and nvme2k.

## Features

- **Controller support**
  - Any PCI IDE controller (class code `01-01`) in **compatibility mode** (ports 1F0h/3F6h and 170h/376h, IRQ 14 and 15) or **native/enhanced mode** (ports and interrupt from the PCI BARs), including the per-channel mix a controller's programming interface allows
  - **SATA controllers in IDE mode** (class code `01-01` with the SATA vendors' device IDs; Silicon Image 3112/3512/3114/0240 and the ATI IXP 300/400 SATA functions built on the 3112, with class code `01-80` or `01-04`)
  - **Legacy ISA/VLB channels** without a PCI function (1F0h/170h/1E8h/168h, IRQ 14/15/11/10) on NT 3.1–4.0 and 9x; on 2000/XP through the `*PNP0600` device nodes the firmware reports
  - Up to two channels per adapter instance, two devices per channel; up to 32 controllers per system (build-time, see [Build-time limits](#build-time-limits)); channels and single devices can be turned off with `OFF=`
  - **Compatibility-mode, native-mode and ISA controllers together** on every target; a native-mode function whose BARs the BIOS left on the legacy ports (1F0h/3F4h/170h/374h, where a compatibility-mode function decodes too) is moved to free I/O space before any channel is used (NT 3.1–4.0) or kept off those ports through its PnP resources (2000/XP/x64); see [Several controllers in one machine](#several-controllers-in-one-machine)
  - **Controller timing programmed by the driver**, so the fastest mode is used whatever the BIOS did: Intel PIIX/PIIX3/PIIX4/ICH0–ICH8 (PIO 0–4, MWDMA 0–2, UDMA 0–5 with 80-conductor cable detection), CMD 643/646/648/649 (address setup, active/recovery and UDMA timing; UDMA 2/4/5 as the chip and revision allow, host-side cable detection on the 648/649) and Silicon Image 3112/3512/3114 (transfer mode register in BAR 5)
  - **Every other controller**: the timing is left as the BIOS set it, and DMA is used for a parallel ATA drive only where the BIOS flagged it as DMA-capable in the bus-master status register (the rule Linux's generic driver uses); a SATA drive keeps the UDMA mode the BIOS selected, since a SATA link has no parallel timing; `DMA=1` overrides this; see [Transfer mode selection](#transfer-mode-selection)
  - **Chip errata handled**: the CMD64x interrupt latches (cleared on every interrupt and after every command, otherwise the PCI interrupt line stays asserted), the SiI3112 "mod15write" data corruption with certain Seagate drives (at most 15 sectors per command), the SiI3512/3114 "R_ERR on DMA activate FIS" errata, SiI SATA interrupts (masked), ALi M5229 limits (no 48-bit DMA before revision C5, no ATAPI DMA, no DMA before revision 20), the RZ1000/CMD640 read-ahead FIFO (turned off, channels serialized), ServerWorks OSB4/CSB UDMA limits, the Intel 450NX PIIX4 DMA errata, simplex bus masters (DMA on one channel only); see [Controller-specific handling](#controller-specific-handling)
  - PCI command register: I/O space and bus mastering are enabled when the firmware left them off, through `ScsiPortSetBusDataByOffset` where it exists and through configuration mechanism #1 (ports `CF8h`/`CFCh`) on NT 3.1–4.0; a function whose decoders were turned off behind the driver's back (PnP after a failed start, a BIOS resume) is re-enabled when its registers read FFh

- **Transfer modes**
  - **Bus-master DMA** (UDMA 0–6, MWDMA 0–2) with a 64-entry PRD table per channel, 128 KB per request, 32 physical breaks
  - **PIO fallback**: a channel without a bus master, a device without DMA, `DMA=0` in the driver parameters, a SET FEATURES the device rejects, an unknown controller the BIOS left without DMA, or three DMA errors on a device switch that device to PIO (modes 0–4, with the host timing set accordingly where the driver programs it); a single request whose buffer is not dword-aligned goes with PIO instead of failing; every command the driver issues itself (IDENTIFY, SET FEATURES, SMART, ATAPI sense) is PIO
  - PIO transfers always move 512-byte blocks: devices with larger logical sectors hand their data out in 512-byte DRQ blocks under PIO

- **SCSI-to-ATA Translation Layer (SATL)** — the same translation layer as AHCINT
  - INQUIRY, REQUEST SENSE, TEST UNIT READY, MEDIUM REMOVAL, READ CAPACITY(10), MODE SENSE(6)/(10), REPORT LUNS
  - READ/WRITE(6), (10), (12) and (16) on all targets; FUA (Force Unit Access) as WRITE DMA FUA EXT
  - SYNCHRONIZE CACHE(10)/(16) as FLUSH CACHE (EXT); VERIFY(10)/(12)/(16) as READ VERIFY SECTORS (EXT)
  - START STOP UNIT as STANDBY IMMEDIATE / IDLE IMMEDIATE (the write cache is flushed first)
  - MODE SENSE caching page (08h, WCE/DRA from IDENTIFY word 85) and control page (0Ah); MODE SELECT on the caching page turns the write cache and read look-ahead on and off (SET FEATURES 02h/82h, AAh/55h), kept across resets and power transitions
  - SAT ATA PASS-THROUGH(12)/(16) (non-data, PIO in/out, DMA; CK_COND and "return response information" with descriptor sense)
  - SMART through `IOCTL_SCSI_MINIPORT` ("SCSIDISK" signature, as `atapi.sys`), so `SMART_RCV_DRIVE_DATA` tools work; see [IOCTL interface](#ioctl-interface)
  - 64-bit SCSI on every target: READ CAPACITY(16) (SERVICE ACTION IN), READ/WRITE/VERIFY(16), SYNCHRONIZE CACHE(16), MODE SENSE(10) long LBA block descriptor (LLBAA); READ CAPACITY(10) returns `0xFFFFFFFF` for disks of 2 TB and more where the class driver then reads READ CAPACITY(16), and `0xFFFFFFFE` (the first 2 TB) where it cannot (see [Disks of 2 TB and more](#disks-of-2-tb-and-more))
  - ATA disks report INQUIRY VERSION `05h` (SPC-3 / SBC-2), advertising 16-byte CDB support; VPD pages `00h`, `80h` (serial number from IDENTIFY), `B0h` (block and unmap limits), `B1h` (rotation rate and form factor, IDENTIFY words 217/168) and `B2h` (logical block provisioning)
  - **UNMAP** as DATA SET MANAGEMENT / TRIM (IDENTIFY word 169) on SATA SSDs behind an IDE-mode controller, when the device runs with bus-master DMA (the command is a DMA command); READ CAPACITY(16) reports LBPME, and LBPRZ for deterministic zeroes after TRIM (word 69); see [TRIM](#trim)
  - **LOG SENSE** pages `00h`, `0Dh` (temperature from the SMART attributes) and `2Fh` (informational exceptions: ASC 5Dh when SMART RETURN STATUS reports a threshold exceeded)
  - **TRIM mode** for Windows versions that send no TRIM: with `tools\trimnt.exe` the free space of a volume is trimmed; see [TRIM](#trim)
  - 28-bit READ/WRITE DMA (or READ/WRITE SECTORS under PIO) whenever the request fits; the EXT commands only on devices whose IDENTIFY DEVICE reports the 48-bit Address feature set (word 83, bit 10)
  - ATAPI (PACKET command) for CD/DVD drives and removable ATAPI disks (ZIP, LS-120/SuperDisk, MO; the peripheral device type comes from IDENTIFY PACKET DEVICE), with DMA when the device and the channel support it; READ(6)/WRITE(6) go to the drive as READ(10)/WRITE(10); TEST UNIT READY / MEDIUM REMOVAL are passed through, REQUEST SENSE returns the drive's own sense data, MODE SENSE(6) is translated to ATAPI MODE SENSE(10) and back — media changes and empty trays are reported correctly to the CD-ROM class driver
  - A unit that did not answer IDENTIFY DEVICE / IDENTIFY PACKET DEVICE is reported as empty (`SRB_STATUS_NO_DEVICE`), never as a zero-length disk; a channel or a controller without any device presents no targets, and the OS boots on
  - Logical sector sizes of 512, 1024, 2048 and 4096 bytes (IDENTIFY word 106 / words 117–118): READ CAPACITY(10)/(16) and MODE SENSE report the drive's own block length, READ CAPACITY(16) also the logical-per-physical exponent (512e drives); see [Logical sector size](#logical-sector-size)

- **Multi-target support from one source tree**
  - Windows 95 / 98 / Me (x86) — `SCSIPORT.PDR` miniport (`ident9x.mpd`)
  - Windows NT 3.1 / 3.50 / 3.51 / NT 4.0 (x86) — `ident.sys` (one binary for all four)
  - Windows 2000 / XP / Server 2003 (x86) and Windows XP x64 Edition / Server 2003 x64 (amd64) — `ident.sys`
  - Every target has the synchronous Fast Polling I/O path; an interrupt-driven DMA path is used wherever ScsiPort connects an interrupt for the channel; the real differences are `#ifdef`s (see [One source tree](#one-source-tree))

- **Interrupt-driven DMA with a watchdog** — disk reads and writes go out as DMA commands and complete from `HwInterrupt`; a 100 ms timer checks the channels while a command is outstanding, and after five completions that no interrupt reported the driver turns the interrupt path off and continues with Fast Polling for good; see [I/O model](#io-model)

- **Power management** — flush on shutdown (standby too with `STANDBY=1`), flush and standby on power transitions, `HwAdapterControl` (stop/restart) on 2000/XP, hibernation and crash dump support, re-initialization of a channel whose controller lost its state, and a power-down/up IOCTL for an ACPI driver on NT 3.1–4.0 and 9x; see [Power management](#power-management)

## Architecture

```
          Windows 9x/Me                         Windows NT/2000/XP

        Application Layer                       Application Layer
               ↓                                       ↓
     IOS (I/O Supervisor) layers                SCSI Class Driver
               ↓                                       ↓
          SCSIPORT.PDR                            ScsiPort.sys
               ↓                                       ↓
          ident9x.mpd                             ident.sys
      (SCSI miniport driver)                 (SCSI miniport driver)
               ↓                                       ↓
   ATA task file + bus master DMA         ATA task file + bus master DMA
       (PCI IDE / ISA channel)                (PCI IDE / ISA channel)
```

### Key Components

| File | Purpose |
|---|---|
| `src\ident.h` | Register layout (task file, control block, bus master), IDENTIFY words, data structures (`HW_DEVICE_EXTENSION`, `IDE_CHANNEL`, `IDE_DEVICE`, `ATA_REQUEST`), IOCTL structures, limits |
| `src\idemain.c` | `DriverEntry`, `HwFindAdapter` (PCI scan, PnP resources, legacy channels), channel bring-up, `HwStartIo`, `HwInterrupt`, the watchdog timer, `HwResetBus`, `HwAdapterControl`, NT 3.1 compatibility |
| `src\idecmd.c` | Command execution: task-file programming, PIO data transfer, PRD building and bus-master DMA, transfer mode selection, Intel timing registers, channel reset, DMA fallback and self-healing |
| `src\idesatl.c` | SCSI-to-ATA translation, IDENTIFY parsing, ATAPI, sense data, MODE SENSE/SELECT, pass-through, SMART and power IOCTLs |
| `src\idepnp.c` | 2000/XP/x64 only: the PnP hook that connects IRQ 14 for a compatibility-mode PCI function and keeps a native-mode function off the legacy ports (see [Windows 2000/XP/x64 specifics](#windows-2000xpx64-specifics)) |
| `src\NT`, `src\2KXP`, `src\9X` | Build files per target |
| `tools\portpage.c`, `tools\mkpp.sh` | The Device Manager page `identpp.dll` for 2000/XP/x64 (see [Device Manager page](#device-manager-page)); the same source builds the page of AHCINT, IDENT, NVMENT and MMCSDNT |
| `bin\` | Prebuilt binaries and install files |

## Feature matrix by target

| | 95/98/Me (x86) | NT 3.1/3.50/3.51/4.0 (x86) | 2000/XP/2003 (x86) and XP x64/2003 x64 (amd64) |
|---|---|---|---|
| Build folder (sources are shared in `src\`) | `src\9X` | `src\NT` | `src\2KXP` |
| Target define | `IDE_W9X` (implies `IDE_NT4`) | `IDE_NT4` | none |
| Driver binary | `ident9x.mpd` (`SCSIPORT.PDR` miniport) | `ident.sys` | `ident.sys` |
| Adapter instance | One per PCI function (both channels) or legacy channel | One per channel (PCI or legacy); the second channel of a function is the next instance | One per PCI function (PnP: both channels) or `*PNP0600` node |
| I/O model | Fast Polling by default; interrupt-driven DMA with `IRQ=1` | Interrupt-driven DMA on every channel with an interrupt (compatibility mode and ISA: IRQ 14/15/11/10; native mode: the function's INTx, on NT 3.1 only with `IRQ=1`); Fast Polling without one and with `IRQ=0` | Interrupt-driven DMA on the channel whose interrupt ScsiPort connected; the second channel of a compatibility-mode function runs with Fast Polling |
| DMA | UDMA/MWDMA with PIO fallback | Same | Same |
| Power management | Flush on shutdown; `HwAdapterState`; power IOCTL | Flush on shutdown; power IOCTL; state check on every request | `HwAdapterControl` stop/restart, hibernation, crash dump; power IOCTL |
| SMART / ATA PASS-THROUGH | Yes | Yes | Yes |
| PCI detection | Bus:slot from `SCSIPORT.PDR`, then a bus scan | Bus:slot from ScsiPort (NT 3.50+), bus scan through the HAL or `CF8h`/`CFCh` (NT 3.1); then the legacy ports | PnP resources; the bus scan only for the crash dump instance |
| Legacy ISA channels (no PCI function) | 1F0h/170h | 1F0h/170h/1E8h/168h, IRQ 14/15/11/10 | Through the `*PNP0600` device nodes (ports and IRQ from ACPI) |
| ATA read/write commands | READ/WRITE DMA or SECTORS (28-bit) when the request ends below LBA 2^28 and moves ≤ 256 sectors; the EXT commands only if IDENTIFY reports 48-bit support | Same | Same |
| READ/WRITE(16), READ CAPACITY(16), INQUIRY VERSION 05h (SPC-3) | Yes | Yes | Yes |
| Addressable capacity | 2^48 sectors (ATA limit) via READ CAPACITY(16); what the OS class driver can use is up to the OS | Same | Same |
| REQUEST SENSE | Yes — ATA errors (LBA out of range, UNC, ABRT) and the driver's own sense; ATAPI asks the drive | Same | Yes |
| Auto-sense on error (`SRB_STATUS_AUTOSENSE_VALID`) | No — class driver issues REQUEST SENSE | No — class driver issues REQUEST SENSE | Yes — ATA errors mapped from the error register, ATAPI sense fetched from the drive |
| ATAPI (PACKET) support | Yes, DMA when available | Yes | Yes |
| ATAPI MODE SENSE(6)→(10) translation | Yes | Yes | Yes |
| 64-bit addressing | No — 32-bit DMA (SFF-8038i PRDs are 32-bit) | No | No — the PRD format is 32-bit; ScsiPort bounces buffers above 4 GB on x64 |
| Hot-plug / late device detection | No | No | No |
| PRD entries per channel | 64 (33 at most in use: 32 physical breaks) | 64 | 64 |
| DMA arena size | 2.5 KB per adapter instance (two PRD tables, two bounce buffers, alignment) | Same | Same |
| Max transfer size per I/O | 128 KB | 128 KB | 128 KB |
| Debug output | VMM `_Debug_Printf_Service` (`DEBUG=1` builds), prefix `[ident9x]` | `DbgPrint`, prefix `[IDENT]` | `DbgPrint`, prefix `[IDENT]` |

## Known Limitations

- No MSI; the interrupt path uses the legacy IRQ 14/15 lines or the function's INTx. Windows 9x uses Fast Polling unless `IRQ=1` is set; NT 3.1 uses Fast Polling for a native-mode PCI function unless `IRQ=1` is set
- On Windows 2000/XP/x64 a controller in compatibility mode is started as one PnP adapter, and ScsiPort connects only one interrupt for a PnP adapter: the primary channel is interrupt-driven, the secondary channel uses Fast Polling (its IRQ 15 stays unconnected). Native-mode controllers (one INTx for both channels) and NT 4.0 (one adapter per channel) do not have this limit
- Everything that is not a disk read or write under DMA (PIO devices, ATAPI without DMA, IDENTIFY, FLUSH, SMART, the crash dump stack) is Fast Polling: a slow command keeps the CPU that issued it busy polling for as long as it takes, up to the SRB timeout (see [Command timeout](#command-timeout))
- One command at a time per channel (ATA has no queuing outside NCQ, which IDE-mode controllers do not expose); ScsiPort is told one request per LU
- 128 KB maximum transfer size per request (32 physical breaks)
- No hot-plug support on any target; devices are detected at `HwInitialize` and again after a power transition or reset
- No link power management or spin-down timers; disks are spun down with STANDBY IMMEDIATE only on power-down, START STOP UNIT and (with `STANDBY=1`) shutdown
- Controller timing is programmed on Intel PIIX/ICH, CMD64x and Silicon Image 311x only. On other chipsets (VIA, AMD, nVidia, SiS, ALi, ServerWorks, HighPoint, Promise, ...) the driver relies on the BIOS for the host side: a parallel drive the BIOS did not flag for DMA runs with PIO (`DMA=1` overrides this, `DMA=0` turns DMA off). An add-in card whose own BIOS is disabled therefore runs its parallel drives with PIO unless `DMA=1` is given
- The chip-specific code (CMD64x latches and timing, Silicon Image BAR 5, the ALi/ServerWorks/RZ1000/CMD640/450NX errata) was verified against QEMU models of these chips (see `TESTS.TXT`), not on the real controllers; the lists of affected drives and revisions follow Linux. Reports from real hardware are welcome
- Windows 9x: the CMD 643/646 run with Fast Polling (their interrupt latches are in configuration space)
- NT 3.x/4.0: after a text-mode installation the in-box IDE drivers are turned off by the driver at its first start, so that boot may still find some channels held by `atapi`/`atdisk`; from the next boot IDENT has them
- Windows 2000/XP: installing IDENT turns `atapi` off for every IDE controller (see [In-box IDE drivers](#in-box-ide-drivers)); a controller left on Windows' driver loses its drives at the next boot
- Silicon Image 3114: channels 3 and 4 have no I/O BARs and are not used; channels 1 and 2 work
- Windows 9x does not move a native-mode function that the BIOS left on the legacy ports (see [Several controllers in one machine](#several-controllers-in-one-machine)); on such a machine (QEMU with SeaBIOS and a second, native IDE controller) the compatibility-mode drives on the shared ports stay hidden, as they are for the BIOS itself
- 2000/XP/x64: hibernation and resume with a native-mode function moved off the legacy ports have not been tested; the BIOS puts the old addresses back at power-on, and `pci.sys` restores the assigned ones when the function powers up
- Moving a native-mode function off the legacy ports on NT 3.1–4.0: the free block is chosen from what the PCI BARs and bridge windows show; I/O that a chipset decodes through its own registers (ACPI, SMBus and GPIO bases) is not visible there, which is why the search stays next to the function's bus-master block and at or above 1000h. Tested on QEMU's PIIX3 and CMD646, not on real hardware
- Simplex bus masters: only the primary channel uses DMA, even when it has no drives
- Memory above 4 GB: tested on XP x64 with 2 GB of the RAM above the 4 GB line (all transfers correct, including PIO into buffers there; XP x64's ScsiPort hands the 32-bit bus master buffers below 4 GB). Windows 2000 with `/PAE` was run on Professional only, which uses no memory above 4 GB; Advanced Server/Datacenter with more than 4 GB has not been tested
- SMART through the "SCSIDISK" IOCTL reaches only the first device of a channel (LUN 0); the second device is reached with ATA PASS-THROUGH
- Disks of 2 TB and more: Windows 9x, NT 3.1–4.0, 2000 and XP x86 address at most the first 2 TB (their class drivers know only READ CAPACITY(10) and 32-bit LBAs); XP x64 and Server 2003 SP1 and later see the whole disk. See [Disks of 2 TB and more](#disks-of-2-tb-and-more)
- Logical sectors larger than 4096 bytes are not taken (the device is left unusable); CHS-only devices (no LBA) are not addressable
- A disk with logical sectors larger than 512 bytes works as a data disk only: BIOS INT 13h and the boot loaders read 512-byte sectors, so the system cannot boot from it (NTLDR hangs or reports `ntoskrnl.exe` missing)
- 9X and NT builds do not fill auto-sense; the OS-era class drivers issue their own REQUEST SENSE
- Windows 95 RTM: see the [SCSIPORT.PDR note](#windows-95--98--me) below

## Building from source

### Requirements

- **Windows 95 / 98 / Me (x86):**
  - A Windows 9x build machine (the build script is written for the Windows 9x `COMMAND.COM` batch interpreter)
  - Windows 95 DDK — expected at `C:\DDK` (provides `MASTER.MK`, `BLOCK\INC`, `INC32`, `BLOCK\LIB\scsiport.lib`)
  - Win32 SDK for Windows NT 4.0 / Windows 95 — expected at `C:\MSTOOLS`
  - Microsoft Macro Assembler (MASM) 6.11 — expected at `C:\MASM611` (`BIN\ML.EXE`, `BIN\NMAKE.EXE`)
  - Microsoft Visual C++ 2.0 — expected at `C:\MSVC20` (`BIN\CL.EXE`, `BIN\LINK.EXE`, `BIN\NMAKE.EXE`)
- **Windows NT 3.1 / 3.50 / 3.51 / NT 4.0 (x86):** Windows NT 4.0 DDK + Visual C++ 4.0 (MSVC 4.0)
- **Windows 2000 / XP (x86):** Windows Server 2003 SP1 DDK ("WDK 6001", build 3790.1830)
- **Windows XP x64 / Server 2003 x64 (amd64):** A DDK/WDK with an amd64 ("WNET") cross-compiler — Windows Server 2003 SP1 DDK ("WDK 6001", build 3790.1830), Windows Server 2003 R2 DDK, or the Windows 7 WDK (WinDDK 7600.16385.1)

### Build Instructions

#### For Windows 95 / 98 / Me (x86)

`build.bat` expects a copy of the repository's `src` folder in `C:\IDENT` (shared sources in `C:\IDENT`, build files in `C:\IDENT\9X`), and `ident9x.lnk` links against `C:\DDK\BLOCK\LIB\scsiport.lib`. If your tools live elsewhere, edit the `SET` lines at the top of `build.bat` (`MASM_ROOT`, `C16_ROOT`, `C32_ROOT`, `SDKROOT`, `DDKROOT`, `IDE_ROOT`) and the library path in `ident9x.lnk`.

```bat
REM Copy the src folder (shared sources + 9X build files) to C:\IDENT
mkdir C:\IDENT
mkdir C:\IDENT\9X
copy <path-to-IDENT>\src\*.* C:\IDENT
copy <path-to-IDENT>\src\9X\*.* C:\IDENT\9X

C:
cd \IDENT\9X

REM Optional: debug build (debug output through VMM, see Debugging)
REM set DEBUG=1

REM Build
build.bat
```

`build.bat` sets `MASTER_MAKE=1` and the tool roots, puts MASM 6.11 and Visual C++ 2.0 on `PATH`, creates `TMP`/`TEMP` (default `C:\WINDOWS\TEMP`) if they are missing, warns if any expected tool is not found, and then runs `nmake` in `C:\IDENT\9X`. The makefile pulls in the DDK's `MASTER.MK` (`BUILD_BITS=32`, `BUILD_TYPE=block`), compiles `..\idemain.c`, `..\idecmd.c` and `..\idesatl.c` with `-DIDE_W9X`, and links them with `ident9x.lnk`.

Output: `C:\IDENT\9X\ident9x.mpd` (plus `ident9x.map`). Run `nmake clean` to remove build output.

#### For Windows NT 3.1 / 3.50 / 3.51 / NT 4.0 (x86)

```bat
cd <path-to-IDENT>\src\NT
build.bat
```

`build.bat` sets `MSVCDIR` (default `C:\MSDEV`) and `DDKDIR` (default `C:\NT4DDK`) — edit them if your tools live elsewhere — then compiles `..\idemain.c`, `..\idecmd.c` and `..\idesatl.c` with `-DIDE_NT4` and links against `scsiport.lib` and `ntoskrnl.lib`.

Output: `src\NT\ident.sys`, for Windows NT 3.1 and later (`-subsystem:native,3.50`; the NT 3.1 loader does not check the subsystem version). The NT build imports only functions NT 3.1's `SCSIPORT.SYS` exports; PCI configuration space is written through ports `CF8h`/`CFCh`.

#### For Windows 2000 / XP (x86)

```bat
REM Set up build environment
Start Menu → Windows Server 2003 DDK → Build Environments → Windows 2000/XP Free Build Environment
REM (runs setenv.bat, configuring PATH/INCLUDE/LIB/DDKBUILDENV for the target)

REM Navigate to driver directory
cd <path-to-IDENT>\src\2KXP

REM Build
build -cZ
```

`sources` compiles the shared `..\idemain.c`, `..\idecmd.c`, `..\idesatl.c` and `..\idepnp.c` (no target define) and links against `scsiport.lib`, `ntoskrnl.lib` and `hal.lib` (`HalGetInterruptVector` for the start-device hook). `-c` forces a clean rebuild and `-Z` prints full build output to the console. Output: `obj\i386\ident.sys` (checked) or `obji386\ident.sys` (free), depending on the environment chosen. Use the **checked** environment for a debug build (asserts enabled, unoptimized), **free** for release.

#### For Windows XP x64 / Server 2003 x64 (amd64)

The x64 driver is built from the same `src\2KXP` folder as the x86 driver; only the build environment differs.

```bat
REM Set up build environment
Start Menu → Windows Server 2003 (WNET) DDK/WDK → Build Environments → x64 Free Build Environment
REM (runs setenv.bat ... WNET AMD64, configuring the amd64 cross-tools and libraries)

REM Navigate to driver directory
cd <path-to-IDENT>\src\2KXP

REM Build
build -cZ
```

Output: `ident.sys` under the amd64 output directory (`objfre_wnet_amd64\amd64\` or equivalent, depending on the exact DDK/WDK version). `ident.rc` supplies version-resource information for this target.

> The 2000/XP and x64 builds should first be verified in the **checked** build environment before being rebuilt and shipped from the **free** build environment.

#### Device Manager page (2000/XP/x64)

`tools\mkpp.sh` builds `identpp.dll` (and the pages of the sibling drivers) from `tools\portpage.c` with MinGW-w64, x86 into `pp\i386` and x64 into `pp\amd64`. The DLL uses no C runtime: it imports only `kernel32`, `user32`, `advapi32`, `setupapi` and `comctl32` functions that Windows 2000 has.

## Installation

### Installation Media

Prebuilt binaries and install files live under `bin\`:

```
bin\
├── 9X\
│   ├── IDENT9X.INF           (Windows 95/98/Me install file)
│   └── IDENT9X.MPD           (SCSIPORT.PDR miniport)
├── NT\
│   ├── oemsetup.inf
│   ├── ident.sys
│   └── floppy\               (NT 3.50/3.51/4.0 driver disk)
│       ├── disk1             (tag file)
│       ├── oemsetup.inf
│       ├── txtsetup.oem
│       └── ident.sys
└── 2KXP\
    ├── ident.inf             (one INF for x86 and x64)
    ├── i386\
    │   └── ident.sys         (x86 binary)
    ├── amd64\
    │   └── ident.sys         (x64 binary)
    ├── floppy_i386\          (F6 disk, 2000/XP/2003 x86)
    │   ├── disk1             (tag file)
    │   ├── txtsetup.oem
    │   ├── ident.inf         (flat: no i386\ subdirectory)
    │   └── ident.sys
    └── floppy_amd64\         (F6 disk, XP x64/2003 x64)
        ├── disk1             (tag file)
        ├── txtsetup.oem
        ├── ident.inf         (flat: no amd64\ subdirectory)
        └── ident.sys
```

The F6 disks carry a flat copy of `ident.inf`: text-mode setup copies the driver and the INF into `%SystemRoot%\OemDir` without subdirectories, and GUI-mode setup installs the controller again from there. With the per-platform subdirectories of the main INF it would ask for `OemDir\amd64\ident.sys`.

### Hardware IDs

The in-box IDE drivers of Windows 2000/XP and 9x already match every IDE controller, so IDENT's install files are written to **outrank** them rather than merely to match. Windows ranks a match on one of a device's *hardware* IDs above a match on one of its *compatible* IDs, and within each list an earlier (more specific) ID above a later one. For a PCI IDE function Windows 2000 reports, for example (PIIX3):

- Hardware IDs: `PCI\VEN_8086&DEV_7010&SUBSYS_11001AF4&REV_00`, `PCI\VEN_8086&DEV_7010&SUBSYS_11001AF4`, `PCI\VEN_8086&DEV_7010&CC_010180`, `PCI\VEN_8086&DEV_7010&CC_0101`
- Compatible IDs: `PCI\VEN_8086&DEV_7010&REV_00`, `PCI\VEN_8086&DEV_7010`, `PCI\VEN_8086&CC_010180`, `PCI\VEN_8086&CC_0101`, `PCI\VEN_8086`, `PCI\CC_010180`, `PCI\CC_0101`

The in-box INFs match on `PCI\VEN_xxxx&DEV_xxxx` (a compatible ID) or on `PCI\CC_0101` (the last compatible ID). `ident.inf` (and `txtsetup.oem`'s `HardwareIds` section) therefore list:

- The chips the in-box INFs name by vendor and device (Intel PIIX–ICH8, VIA, CMD, ALi, SiS, AMD, nVidia, ServerWorks, Cyrix), each as `PCI\VEN_xxxx&DEV_xxxx&CC_0101pp` for the usual programming interfaces: a *hardware* ID, ranking above the in-box `VEN&DEV` match
- `PCI\CC_0101pp` for every programming interface `pp` (00–0F, 80–8F): a compatible ID that ranks above the in-box generic `PCI\CC_0101` match, for controllers no INF names by vendor and device
- `PCI\CC_0101` as a last resort
- `PCI\VEN_1095&DEV_3112/3512/3114/0240` for the Silicon Image SATA controllers (class code `01-80`)

A chip that an in-box INF names by `VEN&DEV` but that is missing from IDENT's chip list keeps the in-box driver; install IDENT on it with **Update Driver → Have Disk** and pick it from the list. On Windows 95/98/Me the INF is of class `hdc` so that it is offered for an `hdc`-class device; a plain `VEN&DEV` list for the same chips is included for Windows 95 RTM, which does not always generate class-code IDs.

Every `bin\2KXP` folder also holds `identpp.dll`, the [Device Manager page](#device-manager-page): the x86 build in `2KXP\`, `i386\` and `floppy_i386\`, the x64 build in `amd64\` and `floppy_amd64\`. `ident.inf` copies it into `System32` and registers it (`EnumPropPages32`); `txtsetup.oem` lists it as a `dll` so that it is on the F6 disk's way into `OemDir` as well. Without it the driver works as before, only the tab is missing.

### Installing

#### Windows 95 / 98 / Me

Windows 9x/Me setup itself runs through the BIOS (INT 13h), so there is no F6-style driver step — install IDENT9x once Windows is up. The in-box ESDI_506.PDR owns the controller after setup, so it is replaced through the device's own **Update Driver**:

1. Copy `bin\9X\IDENT9X.INF` and `IDENT9X.MPD` to a folder on the hard disk (for example `C:\IDENT`). Windows 95 RTM's driver wizard does not see the floppy drive on every machine, so a hard disk folder is the safe choice.
2. Open **Device Manager → Hard disk controllers**, select the IDE controller (for example *Intel 82371SB PCI Bus Master IDE Controller*), **Properties → Driver → Update Driver** (Windows 95: *Change Driver*), **Have Disk**, point it at the folder, and select **IDENT9x PATA/SATA IDE Storage Controller**.
3. Let the files copy, answer **No** when asked to restart, and shut down cleanly before rebooting (a restart before the copy finishes leaves the old driver in place). `IDENT9X.MPD` is copied to `WINDOWS\SYSTEM\IOSUBSYS`.

After the reboot the controller shows under **SCSI controllers**, the disks are driven in 32-bit mode (**Control Panel → System → Performance**: *File System: 32-bit*), and the CD-ROM is reached through the CD-ROM class driver. If the protected-mode driver fails to load, Windows 9x falls back to MS-DOS compatibility mode for the affected disks (check **Performance** and `BOOTLOG.TXT`).

> **Windows 95 RTM users:** if the controller shows a **yellow exclamation mark** in Device Manager, the original Windows 95 `SCSIPORT.PDR` is the problem. Either install the [AMDK6 Update](https://winworldpc.com/download/c3a0c2b0-c398-c2a8-3e02-11c3a6e28094), or extract `SCSIPORT.PDR` from that update and copy it to `WINDOWS\SYSTEM\IOSUBSYS` yourself — rename the existing `SCSIPORT.PDR` there to `SCSIPORT.BKP` first so you keep a backup.

#### GUI-mode setup (Windows 2000/XP/XP x64/Server 2003)

Use `bin\2KXP\ident.inf` with **"Have Disk"** on the IDE controller in Device Manager (**Update Driver**), or let the **Found New Hardware** wizard pick it up for a controller Windows has no driver for. The same INF serves both architectures: it copies `i386\ident.sys` on x86 and `amd64\ident.sys` on x64. The device shows up as **IDENT PATA/SATA IDE Storage Controller** (x86) or **IDENT PATA/SATA IDE Storage Controller (x64)**, and the driver installs as service `IDENT` under `LoadOrderGroup = SCSI Miniport`, boot start. Each PCI IDE function (both channels) becomes one SCSI adapter; the in-box `atapi`/`intelide`/`pciide` stack is detached from that device node and the controller's disks reappear under the SCSI class driver. A reboot is needed when the system disk hangs off the controller being switched.

#### Text-mode (F6) setup — Windows 2000/XP/XP x64/Server 2003

Copy the contents of `bin\2KXP\floppy_i386\` (x86) or `bin\2KXP\floppy_amd64\` (x64) onto a floppy disk (or a virtual floppy image for VM installs), press **F6** at the start of text-mode setup, and select **IDENT PATA/SATA IDE Storage Controller (ages2001)**. Text-mode setup installs the disk through IDENT, and GUI-mode setup keeps it on IDENT because `txtsetup.oem` lists the same hardware IDs as the INF (otherwise setup would hand the controller back to `atapi.sys`).

#### Windows NT 3.1 / 3.50 / 3.51 / NT 4.0

NT uses the older OEM Setup mechanism (`oemsetup.inf`) rather than a standard `.inf`/`.cat` pair. Use **"Have Disk"** in **Windows NT Setup → SCSI Adapters** (NT 3.x) or **Control Panel → SCSI Adapters → Drivers → Add** (NT 4.0), or the equivalent OEM prompt of text-mode setup, point it at `bin\NT\floppy\`, and select **IDENT PATA/SATA IDE Storage Controller (ages2001)**.

Two drivers must not both own the same channel: the ports of a channel are registered with the I/O manager by the driver that loads first (the `Primary disk` group, where `atdisk` lives, loads before `SCSI miniport`), and ScsiPort rejects a later adapter on the same ports as a resource conflict. `oemsetup.inf` therefore disables the in-box IDE drivers when it installs IDENT, and after a text-mode (F6/OEM) installation the driver does it itself at its first start (see [In-box IDE drivers](#in-box-ide-drivers)).

**`boot.ini`:** `multi(0)disk(0)rdisk(0)partition(1)\WINNT` makes NTLDR read through the BIOS (INT 13h); this is the normal choice for an IDE disk, since the BIOS boots from it anyway. `scsi(0)disk(n)rdisk(0)partition(1)\WINNT` with `NTBOOTDD.SYS` (a copy of `ident.sys`) also works; `disk(n)` is the target ID (channel × 2 + unit).

**Disk class driver:** NT 3.x's `scsidisk` (NT 4.0: `disk`) must be a boot driver (`Start = 0`) like the miniport. Otherwise NTLDR does not load it and a system disk on IDENT stops with `0x7B INACCESSIBLE_BOOT_DEVICE`. Text-mode setup sets this when it installs onto a SCSI disk; `oemsetup.inf` sets it too (`Start = 0` for `scsidisk`, or `disk` where there is no `scsidisk`) when the driver is added through *Windows NT Setup* / *SCSI Adapters*. When moving an existing installation onto IDENT by hand, set `HKLM\SYSTEM\CurrentControlSet\Services\scsidisk\Start` (NT 4.0: `...\disk\Start`) to `0` first, and `IDENT`'s own `Start` to `0`.

**CD-ROM drives:** they need the CD-ROM class driver (`scsicdrm` on NT 3.x, `cdrom` on NT 4.0) and CDFS. NT 3.1 supports no IDE CD-ROM drive, so an NT 3.1 installed on IDE (`atdisk`) leaves `scsicdrm` and `cdfs` at `Start = 4`, and a CD-ROM drive on IDENT gets no `\Device\CdRom0` and no drive letter in File Manager. `oemsetup.inf` sets them to `1` (system) when they are at `4`, and so does the driver at boot when it has found a CD-ROM drive (also after a text-mode installation); `cdfs` is left alone when a file system recognizer that loads it is enabled (`cdfs_rec` on NT 3.x, `fs_rec` on NT 4.0). When the driver does it, the drive appears in the same boot. An installation whose `system32\drivers` lacks `SCSICDRM.SYS` and `CDFS.SYS` (or whose `Services` key lacks their entries) needs them from the NT 3.1 CD's `I386` folder first (a compressed file there ends in `_` and is unpacked with `expand`).

### In-box IDE drivers

IDENT takes the place of the IDE drivers Windows brings, so installing it turns those off:

| Windows | Installed by | Disabled (`Start = 4`) | Undone by |
|---|---|---|---|
| NT 3.1 / 3.50 / 3.51 / 4.0 | `oemsetup.inf` (*Windows NT Setup* / *SCSI Adapters*); after a text-mode (F6/OEM) installation with `txtsetup.oem`, the driver itself at its first start | `atdisk`, `atapi` (where present) | Removing IDENT in *SCSI Adapters*: the previous `Start` values, kept as `InboxStart_atdisk` / `InboxStart_atapi` under the IDENT service key, are put back |
| 2000 / XP / 2003 / XP x64 / 2003 x64 | `ident.inf` (Have Disk, Found New Hardware, and GUI-mode setup after an F6 installation: `txtsetup.oem` hands `ident.inf` to GUI-mode setup, which runs it for the controller) | `atapi`, `pciide`, `intelide`, `viaide`, `cmdide`, `aliide`, `amdide`, `toside`; the critical device entry `pci#cc_0101` (any IDE controller that appears later) points to IDENT | Installing Windows' own IDE controller driver again (Update Driver → *Standard Dual Channel PCI IDE Controller*), which sets its services' `Start` again; `pciidex` is a library, not a service, and is left alone |
| 95 / 98 / Me | `IDENT9X.INF` | — | — |

- **2000/XP:** every IDE controller should run with IDENT before the reboot that follows. A second IDE controller still on Windows' driver (installed before, and not switched with Update Driver) loses its drives with `atapi` off. The INF's hardware ID ranking makes IDENT the driver Windows picks for an IDE controller it detects anew.
- **NT 3.x/4.0:** the class driver (`scsidisk`/`disk`) is made a boot driver at the same time (see [Windows NT 3.1 / 3.50 / 3.51 / NT 4.0](#windows-nt-31--350--351--nt-40)), and the CD-ROM class driver and CDFS are turned on when they are off (`scsicdrm`/`cdrom`, `cdfs`: `Start` 4 → 1).
- **NT 3.x/4.0 text-mode setup:** the `[Config]` values of `txtsetup.oem` are written under IDENT's own service key only, so the file cannot reach `atdisk` or `atapi`. The driver takes over: once its `HwFindAdapter` has found a controller (also when the adapter is then refused because an in-box driver holds the ports), it saves each in-box driver's `Start` as `InboxStart_<name>` and sets it to `4`, and makes the disk class driver (`scsidisk`, or `disk` on NT 4.0) a boot driver when it turned one off; this takes effect at the next boot. It happens once per driver: an in-box driver whose `InboxStart_<name>` is already there (saved by `oemsetup.inf` or by an earlier start) is not touched again, so one turned back on by hand stays on. Removing IDENT through *SCSI Adapters* puts the saved values back as after an `oemsetup.inf` installation.
- **Windows 9x** has no single switch for its IDE driver: `ESDI_506.PDR` reads its settings (including `NoIDE`) from each IDE controller's own device node. Installing IDENT9X on a controller replaces `ESDI_506.PDR` as that controller's port driver and removes its two channel device nodes, so `ESDI_506.PDR` no longer touches it. A controller left on Windows' driver keeps `ESDI_506.PDR`; it is a different piece of hardware, so the two never compete for a channel.

## Configuration

Registry values set at install time on the NT family (`ident.inf` service section, `txtsetup.oem` `[Config.scsi.IDENT]`, `oemsetup.inf`):

- **Tag** – boot-load ordering tag `40` (behind the in-box IDE drivers' tags)
- **Group** – `SCSI Miniport`
- **Type / Start / ErrorControl** – `1` (`SERVICE_KERNEL_DRIVER`) / `0` (boot start) / `1` (normal)
- **Event log** – `IoLogMsg.dll` registered as the event message file (`TypesSupported = 7`)
- **2000/XP/x64 only** – `Parameters\PnpInterface\5 = 1` (PnP on the PCI bus). Set by `ident.inf`; when it is missing, the driver creates it itself at load (see [Windows 2000/XP/x64 specifics](#windows-2000xpx64-specifics))

On Windows 9x/Me, `IDENT9X.INF` registers the controller with `DevLoader = *IOS` and `PortDriver = IDENT9X.MPD` (plus `DontLoadIfConflict = Y`); there are no extra settings to configure.

### Driver parameters

On the NT family the driver reads the ScsiPort argument string, the `REG_SZ` value `DriverParameters` under `HKLM\SYSTEM\CurrentControlSet\Services\IDENT\Parameters\Device` (create the keys if they are missing; reboot to apply):

| Value | Effect |
|---|---|
| `DMA=0` | No bus-master DMA: every device runs under PIO (the host timing is still set on Intel controllers) |
| `DMA=1` | Use UDMA/MWDMA on an unknown (non-Intel) controller even when the BIOS left the drive without DMA; the device's fastest mode is set, UDMA above 2 only with an 80-conductor cable |
| `IRQ=0` | No interrupt: every command is Fast Polling. Use this on a machine whose IDE interrupt routing is wrong (the watchdog would get there on its own after five missed interrupts) |
| `STANDBY=1` | STANDBY IMMEDIATE after the flush at system shutdown (see [Power management](#power-management)) |
| `OFF=list` | Channels and devices the driver leaves alone, separated by commas: `c` a whole channel (0 primary, 1 secondary), `c.u` one device (0 master, 1 slave): `OFF=1,0.1` turns off the secondary channel and the primary slave of every controller; a PCI location `bus.device.function:` in front (decimal, as Device Manager shows it) limits an item to one controller: `OFF=0.31.1:1`. A device turned off gets no command (the reset of its channel still reaches it); a channel turned off is not reset and gets no command, and on NT 3.1–4.0 no adapter either, so its ports stay free for another driver |
| `RC16=1` / `RC16=0` | READ CAPACITY(10) reports `FFFFFFFFh` for a disk of 2 TB or more (`RC16=1`, for a class driver or filter that then reads READ CAPACITY(16)), or `FFFFFFFEh` (`RC16=0`); the default depends on the Windows version, see [Disks of 2 TB and more](#disks-of-2-tb-and-more) |
| `TRIM=0` | No UNMAP and no TRIM mode: VPD B2h and READ CAPACITY(16) report no logical block provisioning |
| `NATIVEFIRST=1` | NT 3.1–4.0: channels in native mode get the first adapters, and with them the first disk numbers; the compatibility-mode and ISA channels follow. For NT 3.1 booting from a native-mode controller while a compatibility-mode controller has drives too (see [Windows NT 3.1 specifics](#windows-nt-31-specifics)); on NT 3.50–4.0, which find the boot disk by its signature, it keeps the system partition at `C:` when no drive letters were saved with Disk Administrator (letters then follow the disk order) |
| `IRQ=1` | Windows 9x: connect the device node's interrupt and use interrupt-driven DMA (Fast Polling by default there). Windows NT 3.1: also connect the INTx line of a native-mode PCI function (compatibility-mode and ISA channels use IRQ 14/15 without it); see [Windows NT 3.1 specifics](#windows-nt-31-specifics) and [Windows 9x specifics](#windows-9x-specifics) |

Several values are separated by spaces or semicolons.

On Windows 95/98/Me `SCSIPORT.PDR` passes the string value `AdapterSettings` of the controller's driver key as the argument string. The key is `HKLM\System\CurrentControlSet\Services\Class\hdc\000n` (the one whose `PortDriver` is `IDENT9X.MPD`; Device Manager's controller properties show which); add `"AdapterSettings"="IRQ=1"` there, for example with a `.reg` file, and restart.

### Device Manager page

On Windows 2000, XP and Server 2003 (x86 and x64) the controller's properties in Device Manager have a **Channels** tab (`identpp.dll`):

- The list shows the primary and secondary channel of the controller with their I/O ports, and under each the master and slave device (vendor and product from the inquiry data, or `(no device)`); one turned off at boot is listed as `(turned off)`.
- Clearing a check box turns that channel or device off: the page writes an `OFF=` item with the controller's PCI location (`bus.device.function:item`) into `DriverParameters`; checking it again removes the item. An `OFF=` item without a location (for every controller) that this controller should no longer follow is replaced by items for each of the other IDENT controllers, so they keep it. A channel row turns off the whole channel (`c`), a device row one device (`c.u`).
- The box under the list shows the whole `DriverParameters` string, for all controllers; it can be edited directly (other values, items the list does not show), and the check boxes follow it.
- **OK** or **Apply** stores the string. Device Manager then restarts the controller, which takes effect at once for a controller without the system disk, page file or hibernation file, or asks for a restart of the computer.
- Writing the value needs administrator rights (it is under `HKLM`).

NT 3.1–4.0 have no Device Manager: there `OFF=` goes into the registry by hand, as above. On Windows 9x/Me it goes into `AdapterSettings`.

### Build-time limits

The limits are compile-time constants in `src\ident.h`:

```c
#define IDE_MAX_CHANNELS                2           /* per adapter instance */
#define IDE_DEVICES_PER_CHANNEL         2
#define IDE_MAX_CONTROLLERS             32
#define IDE_MAX_TRANSFER                0x20000     /* 128 KB */
#define IDE_MAX_PHYS_BREAKS             32
#define IDE_PRD_ENTRIES                 64
#define IDE_MISSED_IRQ_LIMIT            5
#define IDE_DMA_ERROR_LIMIT             3           /* then PIO for the device */
```

- **`IDE_MAX_CHANNELS`** and **`IDE_DEVICES_PER_CHANNEL`** follow the hardware (a PCI IDE function has two channels of two devices) and size the device extension and the DMA arena; the target ID of a device is `channel × 2 + unit`, and `InitiatorBusId` is set one past the last target. They are not meant to be changed
- **`IDE_MAX_CONTROLLERS`** sizes the table that records which channels (by command port) a driver instance has claimed, so that two instances never drive the same channel (two entries per controller plus the four legacy channels, 8 bytes each; 8 controllers up to the first 1.1.0.0 build). Keep it at or above the number of IDE controllers in the machine; a `DEBUG=1`/checked build logs when the table is full
- **`IDE_MAX_TRANSFER`** and **`IDE_MAX_PHYS_BREAKS`** are what ScsiPort is told; `IDE_PRD_ENTRIES` (64, a 512-byte table that never crosses a 64 KB boundary) leaves room for a PRD split at a 64 KB boundary of every one of the 33 pages a 128 KB request can touch
- **`IDE_MISSED_IRQ_LIMIT`** and **`IDE_DMA_ERROR_LIMIT`** set when the driver gives up on interrupts (per adapter) and on DMA (per device)

After changing a value, rebuild every target you ship (`src\9X`, `src\NT`, `src\2KXP`); they all compile the same `ident.h`.

## Debugging

The NT, 2000/XP and x64 variants log via `DbgPrint` (through the `IDE_DBG` macro), visible through a kernel debugger (e.g. WinDbg / i386kd, depending on target OS) or `DebugView` once a debugger port is attached. Debug output is prefixed `[IDENT]`. The bring-up lines show what the driver decided for each channel and device:

```
[IDENT] FindAdapter: 8086:7010 at bus 0 slot 00000021, progif 80, channel 0 and next
[IDENT] FindAdapter: 2 channel(s) at 1F0 (bus master), IRQ 14/0, interrupt
[IDENT] Channel 0.0: LBA=yes LBA48=yes sectors=00000000:00C00000 x 512 bytes, CHS 12483/16/63, FUA=no write cache=on
[IDENT] Channel 0.0: MWDMA 2 (PIO 4, UDMA 3F MWDMA 7, cable 80, BIOS UDMA 5)
[IDENT] Channel 1.0: ATAPI, 12-byte packets
[IDENT] Channel 1: 1 device(s), Fast Polling
```

The 9X variant prints through the VMM's `_Debug_Printf_Service` (the same approach as SweetLow's nvme9x), so messages are fully formatted and go wherever Windows 9x debug output goes: WDEB386 on a serial port, SoftICE, or DebugView for Windows 9x. The calls are only compiled into `DEBUG=1` builds (`set DEBUG=1` before `build.bat`); release builds carry no debug code. Messages are prefixed `[ident9x]`.

In code, all targets use one macro with double parentheses (no variadic macros needed for VC++ 2.0 / MSVC 4.0):

```c
IDE_DBG((IDE_PFX "Channel %lu.%lu: status %02lX\n", channel, unit, status));
```

## Technical Notes

### I/O Model

- **Fast Polling (all targets):** a command is issued from `HwStartIo` with the device's nIEN bit set and polled every 10 µs on the alternate status register (and the bus-master status register under DMA) until BSY clears and, for PIO, each DRQ block has been moved (see [Command timeout](#command-timeout)); it is completed before `HwStartIo` returns. Completion never depends on an interrupt being delivered — this also covers text-mode Setup, the crash dump stack, a channel ScsiPort connected no interrupt for, and controllers with broken IRQ routing.
- **Interrupt-driven DMA:** on a channel with an interrupt, disk reads and writes (and ATAPI commands under DMA) are started with nIEN clear and `HwStartIo` returns `SRB_STATUS_PENDING`; `HwInterrupt` reads the bus-master status, stops the engine, reads the task-file status and completes the request. One command is outstanding per channel; while one is, a 100 ms ScsiPort timer (`RequestTimerCall`) also services the channels. If the timer finds completed commands that no interrupt reported five times, the driver turns the interrupt path off for that adapter and continues with Fast Polling for good: a wrong IRQ routing costs latency, not data.
- **PIO:** READ/WRITE SECTORS (EXT) and ATAPI data phases are moved with 16-bit `REP INSW`/`OUTSW` in 512-byte DRQ blocks, polling DRQ between blocks; `READ MULTIPLE` is not used (no gain under emulation or on modern drives, and one more thing to get wrong).

### Transfer mode selection

`IdeSelectTransferMode` runs for every device at bring-up and after a reset:

1. The PIO mode comes from IDENTIFY word 64 (PIO 3/4) or word 51.
2. Without a bus master, with `DMA=0`, after three DMA errors, on a device without DMA capability (word 49 bit 8), on an ATAPI device behind a controller without ATAPI DMA (ALi), or on the secondary channel of a simplex bus master, the device runs under PIO.
3. Otherwise the UDMA (word 88) and MWDMA (word 63) capabilities, the 80-conductor cable (word 93 bit 13, SATA, the Intel or CMD648/649 cable-detect bits), the mode the BIOS left selected and the bus master's "drive DMA capable" bit are read.
   - **Controllers whose timing the driver programs:** the fastest mode within the chip's limit, UDMA above 2 only with an 80-conductor cable. Intel PIIX3: MWDMA only; PIIX4/ICH0: UDMA 2; ICH: UDMA 4; ICH2 and later: UDMA 5 (`IDETIM`, `SIDETIM`, `UDMACTL`, `UDMATIM`, `IDE_CONFIG`). CMD643 and the 646 before revision 5: MWDMA only; 646U2: UDMA 2; 648: UDMA 4; 649: UDMA 5 (`ARTTIMx`, `DRWTIMx`, `UDIDETCRx`). Silicon Image 311x with its BAR 5 mapped: up to UDMA 6, with the port's transfer mode register set.
   - **Any other controller:** the timing is left as the BIOS set it. A drive the BIOS flagged as DMA-capable keeps the mode the BIOS selected (or, flagged without a mode on record, gets the fastest one the cable allows); a SATA drive keeps the selected UDMA mode; a parallel drive without the flag runs with PIO, since the host timing for DMA is unknown. `DMA=1`: the fastest mode the device and cable allow, whatever the BIOS did.
4. SET FEATURES (03h) sets the device's mode; a rejected SET FEATURES means PIO.
5. The bus master's "drive DMA capable" bit is set for a drive in a DMA mode and cleared otherwise, as a BIOS does, so the state survives a re-selection after a reset.

### Controller-specific handling

- **CMD 643/646/648/649:** each channel latches its interrupt in a register (`CFR` bit 2 / `ARTTIM23` bit 4 on the 643/646, `MRDMODE` bits 2/3, also at bus master base + 1, on the 648/649), and the chip holds its PCI interrupt line while a latch is set; nothing in SFF-8038i clears it. The driver clears the channel's latch (write 1) in `HwInterrupt`, after each DMA completion and after each polled command (a drive that ignores nIEN sets it too), and claims an interrupt whose latch it found set; without this the line stays asserted and the system hangs in an interrupt storm. At start-up interrupt blocking is turned off and memory read line on (`MRDMODE`), as Linux `pata_cmd64x` does. On Windows 9x the 643/646, whose latches are in configuration space, stay with Fast Polling, since configuration space is not accessed from an interrupt there.
- **Silicon Image 3112/3512/3114 (and the ATI IXP SATA functions):** BAR 5 is mapped (`ScsiPortGetDeviceBase`; `_MapPhysToLinear` on 9x). The SATA interrupts (PHY, hot-plug) are masked in each port's `SIEN`, since nothing would acknowledge them; the FIFO PCI arbitration thresholds are set from the cache line size; the 3512/3114 get the "R_ERR on DMA activate FIS" errata fix; the transfer mode register is programmed. On the 3112, drives with the "mod15write" errata (Seagate ST320012AS, ST330013AS, ST340017AS, ST360015AS, ST380023AS, ST3120023AS, ST340014ASL, ST360014ASL, ST380011ASL, ST3120022ASL, ST3160021ASL, Toshiba MK2561GSYN) get at most 15 sectors per command: larger requests are split and run one piece after the other. The Maxtor 4D060H3 is limited to UDMA 5. This follows Linux `sata_sil`. Without BAR 5 the controller is treated like any other (timing as the BIOS set it).
- **PC Tech RZ1000/RZ1001 and CMD640:** both have a read-ahead FIFO that corrupts data. The driver turns read-ahead off (RZ1000: configuration register 40h bit 13, read back; a controller on which it stays on is not driven at all; CMD640: read-ahead off for all four drives, as Linux `pata_rz1000`/`pata_cmd640` do). Their two channels share one data path, so both are driven by one adapter instance with one command at a time, with Fast Polling and PIO.
- **ServerWorks OSB4:** no UDMA to disks (the chip hangs); MWDMA is used. **CSB5/CSB6:** no UDMA 5 and up with the Seagate ST320011A, ST340016A, ST360021A and ST380021A (Linux `pata_serverworks`).
- **Intel 450NX servers:** PIIX4 DMA is turned off when an 82454NX PXB of revision 0, or one below revision 5 with bus lock on, is found on buses 0–3 (Linux `ata_piix`).
- **ALi M5229:** no DMA before revision 20h; before revision C5h no 48-bit DMA, so reads and writes above LBA 2^28 use the PIO EXT commands; ATAPI devices run with PIO (Linux `pata_ali` does the same by default).
- **Simplex bus masters** (bus master status bit 7): only one channel may use DMA; the primary channel does, the secondary runs with PIO.
- **Buffer alignment:** a PRD entry gets dword-aligned addresses and counts (SFF-8038i asks for even ones, some controllers for dword). A request whose buffer does not meet that is run with PIO instead of being failed.
- **PCI channels without a bus master** (old controllers such as the CMD640 or PC Tech RZ1000, programming interface without bit 7): run with PIO; ScsiPort is still told of a 32-bit master on the PCI/EISA bus so it can allocate the driver's uncached buffer (NT 4.0 refuses that allocation to a PCI slave and the adapter failed before).

### Error handling and fallback

- **ATA errors** (ERR in the status register) are reported with sense data mapped from the error register: ABRT → ILLEGAL REQUEST, IDNF → LBA OUT OF RANGE, UNC/AMNF → MEDIUM ERROR, and so on. A device that stays BSY or holds DRQ after an error gets a software reset (SRST) of its channel.
- **DMA errors** (the bus-master error bit, a timeout, a transfer that ended with the engine still active) count against the device; after `IDE_DMA_ERROR_LIMIT` (3) it is switched to PIO and the failed request is returned as `SRB_STATUS_BUSY` so ScsiPort sends it again. A bus-master status register that reads FFh (the function's decoders turned off) disables DMA for the channel at once and the request is retried under PIO.
- **Lost decoders:** before each command the channel's control register is checked; when it reads FFh the PCI command register is set again (I/O space, bus master) and the channel reset. This covers a PnP manager that turned the function off after a failed start of one of its device nodes and firmware that leaves the function disabled after a resume.
- **Timeouts** are described under [Command timeout](#command-timeout).

### Power management

- **Shutdown and flush:** `SRB_FUNCTION_FLUSH` issues FLUSH CACHE (EXT) to every disk of the target (only when the write cache is present); SYNCHRONIZE CACHE does the same for one disk. `SRB_FUNCTION_SHUTDOWN` flushes every disk of the target; with `STANDBY=1` it then sends STANDBY IMMEDIATE, so the disk parks its heads (or an SSD saves its tables) before the power goes instead of an emergency power-off. Off by default, as in AHCINT 1.6.0.0: a hang at shutdown on XP x64 was reported with AHCINT 1.5.0.0, which spun the disks down there. The driver declares `CachesData`, without which ScsiPort passes neither request.
- **START STOP UNIT:** stop = flush, then STANDBY IMMEDIATE; start = IDLE IMMEDIATE. The next read or write spins the disk up again.
- **Windows 2000/XP/x64 — `HwAdapterControl`:** `ScsiStopAdapter` (sleep, hibernation, device removal) flushes and puts the disks in standby, disables the device interrupts and stops the bus masters; `ScsiRestartAdapter` sets the channels up again: reset, IDENTIFY, transfer modes, the MODE SELECT settings (write cache, look-ahead).
- **Hibernation and crash dump (2000/XP/x64):** the dump stack loads the miniport a second time without a driver object (or with `DUMP=` in the argument string). That instance uses Fast Polling only and keeps its device extension small; it finds the channel to use from the resources the dump stack hands it, or, for a compatibility-mode function, from the IRQ (14 = primary, 15 = secondary) and the function's PCI header (tested with the system disk in compatibility mode).
- **Lost controller state:** after a power transition on 9x (`HwAdapterState` restore) and on every request on NT 3.1–4.0 a channel whose device needs set-up again (a reset was seen, the decoders were off) is brought up again first; outstanding requests are returned as `SRB_STATUS_BUS_RESET` and retried by ScsiPort.
- **NT 3.1–4.0 and 9x:** these ScsiPorts have no power callbacks. An ACPI or APM driver can use the `IDENT` IOCTL (below): `POWER_DOWN` before the system enters a sleep state, `POWER_UP` (or simply the next request) after resume.

### IOCTL interface

Both are `IOCTL_SCSI_MINIPORT` requests (an `SRB_IO_CONTROL` header followed by the data) sent to `\\.\ScsiN:` (or, on 2000/XP, to the disk):

| Signature | ControlCode | Function |
|---|---|---|
| `"IDENT   "` | `1` GET_INFO | Returns `IDE_IOCTL_INFO`: driver version (`01010000h`), flags (bit 0 interrupt path active, bit 1 powered down, bit 2 bus master present, bit 3 TRIM mode), channel count, PCI vendor/device (0 for a legacy channel), per channel the command and bus-master ports, IRQ and native flag, per device the transfer mode (`00h` PIO, `10h` MWDMA, `20h` UDMA plus the mode number in bits 3:0, bit 7 set for ATAPI, `FFh` no device) and DMA error count, and the interrupt count |
| `"IDENT   "` | `10h` POWER_DOWN | Flush, STANDBY IMMEDIATE on every disk, stop the bus masters, disable the device interrupts. GET_INFO does not wake the controller; any other request does |
| `"IDENT   "` | `11h` POWER_UP | Set the channels up again (as after `ScsiRestartAdapter`) |
| `"IDENT   "` | `20h` TRIM_MODE_ON | A 4 KB pattern follows the header: from now on a write whose data is that pattern repeated is not written but trimmed (see [TRIM](#trim)) |
| `"IDENT   "` | `21h` TRIM_MODE_OFF | Back to normal writes |
| `"SCSIDISK"` | `1B0500h`–`1B050Ch` | SMART, as `IOCTL_SCSI_MINIPORT_SMART_VERSION` ... `_WRITE_LOG` (`NTDDSCSI.H`): IDENTIFY, READ ATTRIBUTES/THRESHOLDS, ENABLE/DISABLE, RETURN STATUS, AUTOSAVE, SAVE ATTRIBUTES, OFFLINE DIAGS, READ/WRITE LOG. `bDriveNumber` is the channel; the device map in SMART_VERSION follows `atapi.sys` (bit n ATA, bit n+4 ATAPI) |

The structures are in `src\ident.h` (`IDE_SRB_IO_CONTROL`, `IDE_IOCTL_INFO`, `IDE_SENDCMD_IN/OUT`).

### TRIM

- **UNMAP:** every block descriptor becomes TRIM ranges of DATA SET MANAGEMENT (06h, 48-bit DMA, up to 65535 blocks per range, 64 ranges per 512-byte block), issued through the channel's bounce buffer. Offered only on devices whose IDENTIFY word 169 bit 0 reports TRIM and that run with bus-master DMA (not with `DMA=0`); VPD B0h gives 64 descriptors and the maximum LBA count, VPD B2h LBPU. LBPRZ (zeroes after TRIM) follows IDENTIFY word 69 bits 14 and 5.
- **TRIM mode:** Windows versions before 7 never send TRIM. `TRIM_MODE_ON` gives the driver a 4 KB pattern; while the mode is on, a write whose data is that pattern repeated over the whole request is trimmed instead of written. `tools\trimnt.exe <drive letter>` switches the mode on with a random pattern on every AHCINT, IDENT, MMCSDNT and NVMENT adapter, fills the free space of the volume with files of the pattern (unbuffered, write-through), switches the mode off and deletes the files. Other writes are written normally all the time.

### ATA command selection

`IdeParseIdentify` reads IDENTIFY DEVICE once per device. 48-bit addressing is used only when word 83 is valid (bits 15:14 = `01`), has bit 10 (48-bit Address feature set) set, and words 100–103 hold a capacity — the same rule as Linux's `ata_id_has_lba48()`. Capacity then comes from words 100–103, otherwise from words 60–61.

Each read/write is checked against that capacity (out of range → CHECK CONDITION, ILLEGAL REQUEST / LBA OUT OF RANGE) and sent as READ/WRITE DMA or READ/WRITE SECTORS (28-bit) when it ends below LBA 2^28 and moves at most 256 sectors; only otherwise, and only on a 48-bit device, as the EXT command. A zero transfer length in READ/WRITE(10)/(16) completes without touching the disk (ATA would read a count of 0 as 256 / 65536 sectors).

LBAs are kept as two `ULONG`s (`IDE_LBA`), so no `__int64` arithmetic — and none of the `_aulldiv`/`_allshl` CRT helpers the 9x build does not link — is needed.

### Logical sector size

ATA disks may use logical sectors larger than 512 bytes ("4Kn" drives use 4096). `IdeParseIdentify` reads word 106: when it is valid (bits 15:14 = `01`) and bit 12 is set, words 117–118 give the logical sector size in words. 512, 1024, 2048 and 4096 bytes are taken; anything else leaves the device unusable instead of guessing. All ATA LBAs and sector counts are in logical sectors, so the SATL only has to report the right block length (READ CAPACITY(10)/(16), MODE SENSE block descriptor) and size each DMA transfer as count × sector size. When bit 13 is set, bits 3:0 (logical sectors per physical sector) go into READ CAPACITY(16) byte 13, so 512e drives report their 4 KB physical sectors. Under PIO the data still arrives in 512-byte DRQ blocks (count × sector size / 512 of them).

What the operating system does with such a disk is up to its class and file system drivers; see `TESTS.TXT` for what was tried (NT 3.1, NT 4.0 and 2000 use 1024- and 4096-byte data disks as FAT/NTFS volumes). Such a disk cannot be the system disk: the BIOS and NTLDR read 512-byte sectors.

### Windows 9x specifics

- **One adapter per PCI function:** `SCSIPORT.PDR` calls `HwFindAdapter` once per device node, so the function's two channels are one adapter (targets 0–1 on the primary, 2–3 on the secondary); the in-box driver's two "channel" device nodes disappear with it.
- **Fast Polling by default:** `SCSIPORT.PDR` connects the single interrupt the device node carries; on a compatibility-mode function that is IRQ 14 only, and the IOS layers above do not expect a miniport to take long per request anyway. `IRQ=1` in `AdapterSettings` (see [Driver parameters](#driver-parameters)) turns the interrupt path on for the channel whose IRQ was connected: on a native-mode controller that is the function's PCI interrupt, shared by both channels.
- **`HwAdapterState`:** `SCSIPORT.PDR` calls this for PCI miniports during Plug and Play state changes; the 9X build provides it, because leaving it `NULL` makes `SCSIPORT.PDR` report "Init Failure" for the miniport. A restore makes the next request check whether the controller lost its state.
- **PCI discovery:** the bus:slot `SCSIPORT.PDR` hands in is tried first. `SCSIPORT.PDR` reads only the device node's own configuration space, so the other PCI functions (the bus scan) are read through configuration mechanism #1 (`CF8h`/`CFCh`).
- **Install:** the INF is of class `hdc` and lists the in-box chips by `VEN&DEV` too, see [Hardware IDs](#hardware-ids) and [Windows 95 / 98 / Me](#windows-95--98--me).

### Windows NT 3.1 specifics

NT 3.1's ScsiPort and HAL predate PCI support in non-PnP miniports. The NT build handles this at run time, so the same `ident.sys` runs on NT 3.1, 3.50, 3.51 and 4.0:

- **`HW_INITIALIZATION_DATA` size:** NT 3.1's `ScsiPortInitialize` accepts only its own 40h-byte structure and fails anything else before calling `HwFindAdapter`. `DriverEntry` first calls it with the NT 3.50–4.0 size on `PCIBus`; if that fails without `HwFindAdapter` ever being called, it retries with the 40h-byte layout on the ISA bus.
- **PCI configuration space:** NT 3.1's `HalGetBusData` only knows CMOS and EISA data, so `ScsiPortGetBusData(PCIConfiguration)` returns nothing. The driver then uses configuration mechanism #1 (ports `CF8h`/`CFCh`) for the bus scan, and on the whole NT build the PCI command register is written through the same ports (`ScsiPortSetBusDataByOffset` does not exist on NT 3.1).
- **`PORT_CONFIGURATION_INFORMATION`:** 58h bytes on NT 3.1, without `SlotNumber` and `MaximumNumberOfTargets` (the access ranges follow right after it). The driver checks `ConfigInfo->Length` before touching those fields.
- **DMA:** the bus master needs 32-bit physical addresses, and NT 3.1's HAL gives an ISA adapter map registers below 16 MB and bounces everything through them. The driver therefore reports `Eisa` as the interface type of a channel with a bus master; the HAL gives a scatter/gather master on EISA no map registers, and the controller sees real physical addresses. A plain ISA channel (no bus master, PIO only) is reported as `Isa`.
- **Interrupt:** a compatibility-mode or ISA channel reports its IRQ (14/15/11/10, edge) on the EISA or ISA bus and is interrupt-driven as on NT 4.0. NT 3.1 has no PCI bus handler, so a native-mode function's INTx line (level-sensitive, from config space) cannot be connected on `PCIBus`; by default such a channel reports no interrupt (level 0) and uses Fast Polling. With `IRQ=1` in `DriverParameters` the INTx line is reported on the EISA bus and the interrupt path is used; if those interrupts do not arrive, the watchdog goes back to Fast Polling. It stays opt-in because a failed interrupt connection on NT 3.1 fails the whole adapter, and with it the boot.
- **Disk numbering:** NT 3.1 numbers the disks in the order their drivers load (and, within IDENT, in the order of its adapters: PCI functions by bus, device and function, primary channel before secondary, then the ISA channels) and resolves `multi(0)disk(0)rdisk(n)` by that order, while NTLDR reads `rdisk(n)` as BIOS drive 80h+n. When the system disk is on a native-mode controller and a compatibility-mode controller earlier on the bus has drives too (an Intel PATA function at 00:1F.1 and the SATA function at 00:1F.2 in native mode, for example), the two orders differ and NT 3.1 does not come up; `NATIVEFIRST=1` in `DriverParameters` puts the native-mode channels first, so the BIOS boot drive (80h) is IDENT's first disk again. Set it before moving the system disk, for example from the running system. IDENT's `Tag` (40) loads it ahead of an untagged SCSI miniport, so with the system disk on another miniport (AHCINT, say) and IDENT added for data disks, IDENT's first disk becomes `rdisk(0)` and the system does not come up (0x7B, or it runs on the wrong partition and stops). Then remove IDENT's `Tag` value or give it one that orders it behind the other miniport, or point `boot.ini` at the right `rdisk(n)`. (Seen on NT 3.1; NT 4.0 identifies the boot disk by its signature.)
- **CD-ROM drives:** NT 3.1 itself has CD-ROM support for SCSI drives only, so an installation on IDE has the CD-ROM class driver and CDFS turned off. IDENT turns them on (see [Windows NT 3.1 / 3.50 / 3.51 / NT 4.0](#windows-nt-31--350--351--nt-40)).
- **Several channels:** each channel asks to be called again (`*Again = TRUE`); the next call takes the next unclaimed channel: the other channel of the same function, the next PCI IDE function, then the legacy ports 1F0h/170h/1E8h/168h that no PCI function decodes.

### Windows NT 4.0 (and 3.50/3.51) specifics

- **One adapter per channel:** every channel is its own ScsiPort adapter with its own interrupt (IRQ 14, IRQ 15, or the function's INTx twice for a native-mode controller, which ScsiPort shares), so both channels of a compatibility-mode function are interrupt-driven.
- **Two passes:** `DriverEntry` calls `ScsiPortInitialize` for `PCIBus` (the PCI functions, bus master channels on the `Eisa` bus handler for 32-bit DMA) and again for `Isa` (the legacy ports no PCI function decodes). Channels found in the PCI pass are recorded by command port so the ISA pass skips them.
- **Interrupt check:** before reporting a channel's interrupt (IRQ 14/15, or a native-mode function's INTx) the driver asks the HAL for a vector (`HalGetInterruptVector`, as ScsiPort will). An MPS HAL (`halmps.dll`) whose MP table has no entry for that interrupt returns none, and ScsiPort would then fail the adapter (`0x7B` with the system disk on it; see AHCINT's GitHub issue #2). The channel then runs with Fast Polling; the debug output says `no vector for IRQ n ... Fast Polling`.
- **`atdisk`/`atapi`:** ScsiPort registers each adapter's access ranges with the I/O manager (`IoReportResourceUsage`); a channel whose ports an in-box driver that loaded earlier already registered is rejected as a resource conflict, and the in-box driver keeps it. Disable the in-box driver (`Start = 4`) when IDENT is to own the channel.

### Disks of 2 TB and more

A disk whose last LBA is `FFFFFFFFh` or beyond does not fit READ CAPACITY(10). The SCSI answer is `FFFFFFFFh`, which tells the class driver to read READ CAPACITY(16). The disk class drivers of Server 2003 SP1 and XP x64 (and later) do; those of NT 3.1–4.0, Windows 9x, 2000 and XP x86 do not: they add one to the value in 32 bits and see a disk of 0 bytes. The driver therefore answers `FFFFFFFEh` (2 TB minus one sector) where the class driver does not know READ CAPACITY(16): those systems see and use the first 2 TB, which is all they can address with READ(10)/WRITE(10) and an MBR anyway. READ CAPACITY(16) and 16-byte CDBs always give the whole disk.

The choice is made once at load time: the NT and 9x builds always clip; the 2000/XP x86 build asks `PsGetVersion` (Server 2003 with a service pack, or later, reads READ CAPACITY(16)); the x64 build never clips. `RC16=1` / `RC16=0` in `DriverParameters` override it (for example `RC16=1` on 9x with a layer such as LBA64HLP.VXD that reads READ CAPACITY(16) itself).

### Windows 2000/XP/x64 specifics

- **PnP adapter:** the PCI bus driver enumerates the IDE function and ScsiPort starts IDENT with the function's resources. A **native-mode** function carries its five port ranges (BAR 0–4) and its PCI interrupt (level-sensitive, shareable, whatever IRQ the firmware routed it to) in the resource list; the start-device hook below leaves such a list alone, and the two channels become one adapter with one shared, interrupt-driven line.
- **Compatibility mode and the missing interrupt:** for a function in compatibility mode `pci.sys` reports only the bus-master port range (BAR 4), any further BAR the function has, and the legacy ports as device-private data; the channels raise ISA IRQ 14 and 15, which are not PCI resources and are not reported. ScsiPort (`scsiport.sys`) refuses to start a PCI adapter whose resource list has no interrupt descriptor (`IRP_MN_START_DEVICE` fails with `IO_ERR_INCORRECT_IRQL`, Device Manager code 10), and connects only the *first* interrupt of a PnP adapter. `DriverEntry` therefore installs a front end on the driver object's `IRP_MJ_PNP` entry after `ScsiPortInitialize` (`src\idepnp.c`): on `IRP_MN_START_DEVICE` with a resource list that has no command block of its own (no 8-byte port range: no channel in native mode), it puts IRQ 14 (raw: level/vector 14; translated: what `HalGetInterruptVector(Isa, 0, 14, 14)` returns) into copies of both resource lists, in place of a PCI interrupt the firmware may have given the function (which its channels do not raise), and passes the IRP on. ScsiPort then connects IRQ 14 for the adapter: the primary channel is interrupt-driven, the secondary channel runs with Fast Polling. The copies are kept until `IRP_MN_REMOVE_DEVICE`. No legacy (`Isa`) detection pass is run on 2000/XP: a non-PnP adapter makes Windows refuse S1–S4, and legacy ISA controllers arrive as `*PNP0600` device nodes anyway.
- **One channel of each mode:** a function with one channel in compatibility mode and one in native mode gets its PCI interrupt; the native channel is interrupt-driven with it and the compatibility-mode channel uses Fast Polling.
- **Native-mode function on the legacy ports:** see [Several controllers in one machine](#several-controllers-in-one-machine).
- **Hibernation:** works with the system disk on IDENT in compatibility mode (tested): the dump instance finds its channel from the IRQ the dump stack hands it (14 = primary, 15 = secondary) and the function's PCI header. For a native-mode function it takes the channel from the port resources it is handed; that path has not been tested with hibernation. S3/S4 need every device on the machine to agree; a VGA adapter without a driver (VgaSave) vetoes them, which has nothing to do with IDENT.
- **Text-mode setup (F6):** the setup kernel boots with a bare `IDENT` service key; the `[Config.scsi.IDENT]` values of `txtsetup.oem` are written only to the installed system. Without `Parameters\PnpInterface\5`, the XP x64 / Server 2003 x64 ScsiPort fails `IRP_MN_START_DEVICE` before `HwFindAdapter` is called, so setup stops with STOP 0x7B (the x86 ScsiPort tolerates the missing value). `DriverEntry` therefore creates `Parameters\PnpInterface\5 = 1` under its service key when the `PnpInterface` key does not exist yet, before `ScsiPortInitialize`. An existing key is left as it is. Both F6 `txtsetup.oem` files also set the value in `[Config.scsi.IDENT]`, so a system installed through F6 gets it from setup as well.

### Several controllers in one machine

Every IDE function and every legacy channel is used, whatever the mix: compatibility-mode functions at 1F0h/170h, native-mode functions at their BARs, ISA channels at 1F0h/170h/1E8h/168h that no PCI function decodes. On NT 3.1–4.0 each channel is an adapter of its own, taken in PCI order (bus, device, function; primary before secondary; with `NATIVEFIRST=1` the native-mode channels first), then the ISA channels; on 9x and 2000/XP each PCI function is one adapter, started in the order Windows enumerates them.

**Native-mode BARs on the legacy ports.** A BIOS may leave the BARs 0–3 of a native-mode function at 1F0h/3F4h/170h/374h (SeaBIOS, the BIOS of QEMU, writes these addresses into every native IDE function other than the PIIX). Next to a compatibility-mode function, which decodes the same ports, both controllers answer: the drives of one hide those of the other, and the bus masters of the two act on the same drive. The typical result is a machine booted from a native-mode controller whose compatibility-mode drives are missing. IDENT resolves this before any channel is used:

- **NT 3.1–4.0:** before the first channel is taken, the driver scans all PCI functions (I/O BARs, bridge windows, IDE functions and the legacy ports of every compatibility-mode channel). A native-mode function whose command or control block overlaps any of these gets a free 32-byte block for its native channels (command block 8 bytes and control block 4 bytes per channel): searched upward from the function's own bus-master block, which lies in the BIOS's pool of PCI I/O space, then downward; at or above 1000h, clear of the ISA 10-bit aliases (`addr & 300h` = 0), outside every other bridge's window on the same bus and inside the window of the bridge in front of the function. I/O decoding is turned off while BAR 0–3 are written, the new addresses are read back, and the old ones are put back if the function does not take them. The debug build logs `Ports: ... moved from 1F0 to ...`.
- **2000/XP/x64:** the BARs belong to `pci.sys` and the PnP manager. On `IRP_MN_FILTER_RESOURCE_REQUIREMENTS` of a function with native channels, while a compatibility-mode function exists, the 8- and 4-byte port ranges (BAR 0–3) are limited to 1000h and above, so the PnP manager assigns other ports and `pci.sys` programs them when it starts the function. A compatibility-mode function may start first, while the native-mode one still decodes the legacy ports: its start-device hook turns off I/O decoding of every function with all channels in native mode and BARs on the legacy ports, and `pci.sys` turns it on again with the new addresses when that function starts.

- **Windows 9x:** the function is not moved. Windows 9x keeps using BIOS INT 13h for every drive that no protected-mode driver claims, and the BIOS reaches its drives at the addresses it set; moved, those INT 13h drives would read the other controller's drive (tried: the native controller's drive letters then showed the volume of the compatibility-mode drive). Windows 95's Configuration Manager also leaves the native function's device node unstarted while its BARs conflict with the compatibility-mode function, so IDENT9X is not called for it. Without the conflict (a BIOS that assigns free I/O space, as real hardware does) both controllers run under IDENT9X.

**Device detection.** After a channel reset both devices' signatures are read first: every register write reaches both devices, so probing the master overwrites the signature of a packet device (CD/DVD) set as slave behind it. A slave still busy after the reset is given up to 3 s.

### Command timeout

A Fast Polling command is polled until the SRB's `TimeOutValue` minus one second (10 s when the SRB carries none, at most 60 s); commands the driver issues on its own (IDENTIFY, SET FEATURES, ATAPI REQUEST SENSE, MODE SENSE translation) get 3 s, flush, standby and the power IOCTL 30 s. An interrupt-driven DMA command is timed by ScsiPort itself (the SRB's `TimeOutValue`); when it expires, ScsiPort resets the bus and the driver stops the bus master, resets the channel and returns the outstanding command.

The one-second margin keeps the miniport's timeout ahead of ScsiPort's: ScsiPort runs its own one-second request timer, and on a multiprocessor system that timer fires on another CPU while `HwStartIo` is still polling. If the poll outlasted the SRB's `TimeOutValue`, ScsiPort would start its own timeout handling (bus reset) for a request the miniport is about to complete.

When a command does time out, the bus master is stopped, the channel gets a software reset, and every device on it is IDENTIFYed and set up again before the next command.

### One source tree

`src\idemain.c`, `src\idecmd.c`, `src\idesatl.c` and `src\ident.h` build every target; `src\idepnp.c` is added on 2000/XP/x64. The build files set the target:

| Define | Target | What it changes |
|---|---|---|
| `IDE_W9X` | Windows 95/98/Me | One adapter per PCI function; Fast Polling unless `IRQ=1`; `HwAdapterState`; debug output via VMM `_Debug_Printf_Service`; implies `IDE_NT4` |
| `IDE_NT4` | NT 3.1/3.50/3.51/4.0 (and 9x) | Two `ScsiPortInitialize` passes (PCI, ISA); one adapter per channel; access ranges and IRQ reported to ScsiPort by the miniport; PCI config writes through `CF8h`/`CFCh`; no auto-sense (the class driver issues REQUEST SENSE) |
| `IDE_NT31_COMPAT` | NT build only (set by `ident.h` for `IDE_NT4` without `IDE_W9X`) | Windows NT 3.1 support in the same binary, see [Windows NT 3.1 specifics](#windows-nt-31-specifics) |
| none | 2000/XP/2003 x86 and x64 | PnP resources; the start-device hook (`idepnp.c`); auto-sense; `HwAdapterControl`; crash dump / hibernation; `Parameters\PnpInterface\5` |

The 2000/XP x86 and XP x64/Server 2003 x64 drivers differ only in the build environment; there are no architecture-specific code paths.

### Memory Allocation

- **DMA arena** – one uncached allocation per adapter instance (`ScsiPortGetUncachedExtension`): per channel a 512-byte PRD table (64 entries, aligned so that it never crosses a 64 KB boundary) and a 512-byte bounce buffer, plus one table's worth of alignment slack — 2.5 KB in all
- **PRD table** – up to 64 entries per command; ScsiPort is told 32 physical breaks and 128 KB per I/O, so an unaligned 128 KB buffer (33 pages) still fits even when every page is split at a 64 KB boundary
- **Bounce buffer** – 512 bytes per channel, used for IDENTIFY, SET FEATURES data and the ATAPI REQUEST SENSE / MODE SENSE commands the driver issues itself
- **Device extension** – one `HW_DEVICE_EXTENSION` per adapter with two `IDE_CHANNEL`s of two `IDE_DEVICE`s; kept small because the crash dump stack allocates it from a small pool

## Version history

### 1.1.0.0 — 2026-10-19

- ATAPI devices other than CD/DVD drives (Iomega ZIP, LS-120/SuperDisk, MO): INQUIRY reported every ATAPI device as a CD-ROM, so a ZIP drive got a CD-ROM drive letter. The device type now comes from IDENTIFY PACKET DEVICE (word 0 bits 12:8, removable bit 7): a ZIP drive is a removable disk. For those, READ(6)/WRITE(6) are sent as READ(10)/WRITE(10), which ATAPI devices understand, and the MODE SENSE(6) translation now keeps the device-specific parameter (write protect of the medium), the block descriptor length and the mode data length, and asks the drive for the whole requested length.
- Device Manager page (**Channels** tab, `identpp.dll`) for 2000/XP/x64: channels and devices turned off with check boxes instead of `OFF=` typed into the registry; installed by `ident.inf` and the F6 disks.
- Every IDE controller in a machine is found, also when booting from a native-mode controller: a native-mode controller that the BIOS left on the legacy ports, where it hid the drives of the compatibility-mode controller, is moved to free I/O space (NT 3.1–4.0) or kept off those ports through its PnP resources (2000/XP/x64).
- An ATAPI drive set as slave behind a hard disk on the same channel is found.
- 2000/XP/x64: a compatibility-mode controller with BARs beyond its bus master, or with a PCI interrupt from the firmware, starts with IRQ 14; a controller with one channel of each mode uses the interrupt each channel raises.
- `NATIVEFIRST=1` (NT 3.1–4.0) gives native-mode channels the first disk numbers, for NT 3.1 booting from a native-mode controller.
- A CD-ROM drive is found and gets a drive letter on NT 3.1 installed on IDE: `oemsetup.inf` and the driver turn on the CD-ROM class driver (`scsicdrm`, `cdrom` on NT 4.0) and CDFS where they are at `Start = 4`, as NT 3.1 leaves them without a SCSI adapter.
- Device detection on real hardware: a device that answers in its registers but left no signature after the reset (status 0) is tried as a packet device instead of being skipped, and a slave that IDENTIFY shows to be the master again (a master that answers for an absent slave, same model and serial number) is ignored.
- NT 3.50–4.0 with an MPS HAL: a channel whose interrupt has no vector in the HAL (no MP table entry) runs with Fast Polling instead of failing its adapter, as found for AHCINT (GitHub issue #2, `0x7B` at boot).
- Up to 32 IDE controllers (8 before).
- `OFF=` turns off whole channels or single devices, optionally on one controller only (by its PCI location); on NT 3.1–4.0 a channel turned off gets no adapter, and its ports stay free.
- Disks of 2 TB and more: READ CAPACITY(10) reported `FFFFFFFFh`, which the class drivers of NT 3.1–4.0, 9x, 2000 and XP x86 turned into a disk of 0 bytes; they now get `FFFFFFFEh` and see the first 2 TB. XP x64 and Server 2003 SP1 and later still get `FFFFFFFFh` and read the full size with READ CAPACITY(16). `RC16=0/1` overrides.
- System shutdown flushes the write cache only; STANDBY IMMEDIATE on `SRB_FUNCTION_SHUTDOWN` (1.0.0.0) is now opt-in with `STANDBY=1`, as in AHCINT 1.6.0.0 after a reported hang at shutdown on XP x64.

### 1.0.0.0 — 2026-10-12

- Initial release: PATA and SATA-in-IDE-mode controllers on Windows 95/98/Me, NT 3.1–4.0, 2000/XP/2003 x86 and XP x64/2003 x64 from one source tree.
- UDMA/MWDMA with PIO fallback, controller timing for Intel PIIX/ICH, CMD 64x and Silicon Image 311x, chip errata handling.
- Compatibility, native and legacy ISA channels; interrupt-driven DMA with a Fast Polling watchdog.
- 48-bit LBA, 16-byte CDBs, logical sectors up to 4 KB, ATAPI with DMA.
- TRIM (SCSI UNMAP as ATA DATA SET MANAGEMENT), TRIM mode and the `trimnt` tool.
- SMART ("SCSIDISK" IOCTL), ATA PASS-THROUGH, VPD B0h/B1h/B2h, LOG SENSE.
- Standby at shutdown, hibernation and crash dump support on 2000/XP, power IOCTL.
- In-box IDE drivers turned off on installation and restored on removal (NT).

## Related drivers

AHCINT, IDENT, MMCSDNT and NVMENT are one family: the same ScsiPort miniport design, SCSI translation layer, IOCTL conventions, Fast Polling fallback and TRIM mode, built from one source tree each for every system from Windows 95 and NT 3.1 to XP x64.

| Driver | Storage |
|---|---|
| **AHCINT** | SATA disks and optical drives on AHCI controllers |
| **IDENT** | PATA disks and optical drives, and SATA controllers in IDE mode |
| **MMCSDNT** | SD/SDHC/SDXC cards and MMC/eMMC on SD host controllers (SDHCI) |
| **NVMENT** | NVMe SSDs (based on [nvme2k](https://github.com/techomancer/nvme2k) by DominBear) |

`tools\trimnt.exe`, shipped with each of them, trims the free space of a volume on any of the four.

## License

Copyright (c) 2026 ages2001. All rights reserved.

**USE AT YOUR OWN RISK.** This driver talks directly to storage controller hardware on operating systems no longer supported or patched by Microsoft. Not recommended for any system holding data you can't afford to lose.

## Acknowledgments

- The ATA/ATAPI-6 and -8, SFF-8038i (PCI IDE Bus Master) and PCI IDE Controller Specification (class code 01-01) documents
- Intel PIIX3/PIIX4/ICH datasheets for the timing registers
- Windows 95 DDK and Windows NT4/2000/XP DDK documentation

## Special Thanks

- infuscomus for porting the sibling drivers to x64
- Dietmar for Fast Polling idea
- DominBear for the [nvme2k](https://github.com/techomancer/nvme2k) project
- SweetLow for [nvme9x](https://github.com/LordOfMice/Tools) (Windows 9x VMM debug output technique)
- [UniATA project](http://alter.org.ua/soft/win/uni_ata/)
- Windows 2000 Dev Community
- Testers
- And everyone who supports it
