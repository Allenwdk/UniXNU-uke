// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <Uefi.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>
#include <Protocol/GraphicsOutput.h>

EFI_STATUS
EFIAPI
UefiMain (
  IN EFI_HANDLE ImageHandle,
  IN EFI_SYSTEM_TABLE *SystemTable
  )
{
  UINT64 Mmfr0;
  UINT64 Midr;
  UINT64 CurrentEl;
  UINT64 Frequency;
  UINTN Size;
  UINTN Key;
  UINTN Stride;
  UINT32 Version;
  UINTN Offset;
  UINT64 FreePages;
  UINT64 ReservedPages;
  EFI_MEMORY_DESCRIPTOR *Map;
  EFI_MEMORY_DESCRIPTOR *Descriptor;
  EFI_GRAPHICS_OUTPUT_PROTOCOL *Gop;
  EFI_STATUS Status;

  (VOID) ImageHandle;
  (VOID) SystemTable;
  gBS->SetWatchdogTimer (0, 0, 0, NULL);
  __asm__ volatile ("mrs %0, id_aa64mmfr0_el1" : "=r" (Mmfr0));
  __asm__ volatile ("mrs %0, midr_el1" : "=r" (Midr));
  __asm__ volatile ("mrs %0, CurrentEL" : "=r" (CurrentEl));
  __asm__ volatile ("mrs %0, cntfrq_el0" : "=r" (Frequency));
  Print (L"UKE-PREFLIGHT: boot-core MIDR=0x%lx EL=%lu\r\n", Midr, CurrentEl >> 2);
  Print (L"UKE-PREFLIGHT: MMFR0=0x%lx TGran4=%lu TGran16=%lu TGran64=%lu\r\n",
    Mmfr0, (Mmfr0 >> 28) & 15, (Mmfr0 >> 20) & 15, (Mmfr0 >> 24) & 15);
  Print (L"UKE-PREFLIGHT: timer frequency=%lu\r\n", Frequency);

  Size = 0;
  Map = NULL;
  Status = gBS->GetMemoryMap (&Size, Map, &Key, &Stride, &Version);
  if (Status != EFI_BUFFER_TOO_SMALL) {
    return EFI_DEVICE_ERROR;
  }
  Size += 8 * Stride;
  Map = AllocatePool (Size);
  if (Map == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }
  Status = gBS->GetMemoryMap (&Size, Map, &Key, &Stride, &Version);
  if (EFI_ERROR (Status)) {
    FreePool (Map);
    return Status;
  }
  FreePages = 0;
  ReservedPages = 0;
  for (Offset = 0; Offset < Size; Offset += Stride) {
    Descriptor = (EFI_MEMORY_DESCRIPTOR *) ((UINT8 *) Map + Offset);
    if (Descriptor->Type == EfiConventionalMemory) {
      FreePages += Descriptor->NumberOfPages;
    }
    if (Descriptor->Type == EfiReservedMemoryType) {
      ReservedPages += Descriptor->NumberOfPages;
    }
  }
  Print (L"UKE-PREFLIGHT: UEFI descriptors=%lu conventional-pages=%lu reserved-pages=%lu\r\n",
    (UINT64) (Size / Stride), FreePages, ReservedPages);
  FreePool (Map);
  Status = gBS->LocateProtocol (&gEfiGraphicsOutputProtocolGuid, NULL, (VOID **) &Gop);
  if (!EFI_ERROR (Status) && Gop->Mode != NULL && Gop->Mode->Info != NULL) {
    Print (L"UKE-PREFLIGHT: GOP %ux%u stride=%u format=%u base=0x%lx size=0x%lx\r\n",
      Gop->Mode->Info->HorizontalResolution, Gop->Mode->Info->VerticalResolution,
      Gop->Mode->Info->PixelsPerScanLine, Gop->Mode->Info->PixelFormat,
      Gop->Mode->FrameBufferBase, (UINT64) Gop->Mode->FrameBufferSize);
  } else {
    Print (L"UKE-PREFLIGHT: GOP unavailable\r\n");
  }
  Print (L"UKE-PREFLIGHT: complete; boot core only; no XNU handoff\r\n");
  return EFI_SUCCESS;
}
