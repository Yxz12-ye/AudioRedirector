#pragma once

#include "inject_protocol.h"

#define VM_MIC_DEFAULT_BUFFER_BYTES (256 * 1024)

NTSTATUS VmMicInitialize(_In_ ULONG BufferBytes);
VOID VmMicShutdown();
VOID VmMicStart();
VOID VmMicStop();
BOOLEAN VmMicIsRunning();
ULONG VmMicRead(_Out_writes_bytes_(BytesToRead) BYTE* Dest, _In_ ULONG BytesToRead);
NTSTATUS VmMicWrite(_In_reads_bytes_(BytesToWrite) const BYTE* Src, _In_ ULONG BytesToWrite);
VOID VmMicGetFormat(_Out_ PVM_MIC_FORMAT Format);
VOID VmMicGetStats(_Out_ PVM_MIC_STATS Stats);
