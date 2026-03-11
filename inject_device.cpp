#include <sysvad.h>
#include "inject_device.h"

#define VM_MIC_POOLTAG 'cMvS'

typedef struct _VM_MIC_CONTEXT
{
    KSPIN_LOCK  Lock;
    BOOLEAN     Initialized;
    BOOLEAN     Running;
    BYTE*       Buffer;
    ULONG       BufferBytes;
    ULONG       ReadPos;
    ULONG       WritePos;
    ULONG       UsedBytes;
    VM_MIC_STATS Stats;
} VM_MIC_CONTEXT;

static VM_MIC_CONTEXT g_VmMic = {};

_IRQL_requires_max_(DISPATCH_LEVEL)
static VOID
VmMicAdvance(_Inout_ ULONG* Pos, _In_ ULONG Amount, _In_ ULONG BufferBytes)
{
    ULONG newPos = *Pos + Amount;
    if (newPos >= BufferBytes)
    {
        newPos -= BufferBytes;
        if (newPos >= BufferBytes)
        {
            newPos %= BufferBytes;
        }
    }
    *Pos = newPos;
}

_IRQL_requires_max_(DISPATCH_LEVEL)
static VOID
VmMicCopyIn(_Out_writes_bytes_(Bytes) BYTE* Buffer, _In_ ULONG BufferBytes, _Inout_ ULONG* WritePos,
            _In_reads_bytes_(Bytes) const BYTE* Src, _In_ ULONG Bytes)
{
    ULONG first = min(Bytes, BufferBytes - *WritePos);
    RtlCopyMemory(Buffer + *WritePos, Src, first);
    if (Bytes > first)
    {
        RtlCopyMemory(Buffer, Src + first, Bytes - first);
    }
    VmMicAdvance(WritePos, Bytes, BufferBytes);
}

_IRQL_requires_max_(DISPATCH_LEVEL)
static VOID
VmMicCopyOut(_In_reads_bytes_(Bytes) const BYTE* Buffer, _In_ ULONG BufferBytes, _Inout_ ULONG* ReadPos,
             _Out_writes_bytes_(Bytes) BYTE* Dest, _In_ ULONG Bytes)
{
    ULONG first = min(Bytes, BufferBytes - *ReadPos);
    RtlCopyMemory(Dest, Buffer + *ReadPos, first);
    if (Bytes > first)
    {
        RtlCopyMemory(Dest + first, Buffer, Bytes - first);
    }
    VmMicAdvance(ReadPos, Bytes, BufferBytes);
}

_Use_decl_annotations_
NTSTATUS VmMicInitialize(ULONG BufferBytes)
{
    if (BufferBytes == 0)
    {
        return STATUS_INVALID_PARAMETER;
    }

    BufferBytes -= (BufferBytes % VM_MIC_BLOCK_ALIGN);
    if (BufferBytes == 0)
    {
        return STATUS_INVALID_PARAMETER;
    }

    if (g_VmMic.Initialized)
    {
        return STATUS_SUCCESS;
    }

    RtlZeroMemory(&g_VmMic, sizeof(g_VmMic));
    KeInitializeSpinLock(&g_VmMic.Lock);

    g_VmMic.Buffer = (BYTE*)ExAllocatePool2(POOL_FLAG_NON_PAGED, BufferBytes, VM_MIC_POOLTAG);
    if (g_VmMic.Buffer == NULL)
    {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    g_VmMic.BufferBytes = BufferBytes;
    g_VmMic.Initialized = TRUE;

    return STATUS_SUCCESS;
}

_Use_decl_annotations_
VOID VmMicShutdown()
{
    if (!g_VmMic.Initialized)
    {
        return;
    }

    if (g_VmMic.Buffer)
    {
        ExFreePoolWithTag(g_VmMic.Buffer, VM_MIC_POOLTAG);
        g_VmMic.Buffer = NULL;
    }

    RtlZeroMemory(&g_VmMic, sizeof(g_VmMic));
}

_Use_decl_annotations_
VOID VmMicStart()
{
    KIRQL oldIrql;
    KeAcquireSpinLock(&g_VmMic.Lock, &oldIrql);

    g_VmMic.Running = TRUE;
    g_VmMic.ReadPos = 0;
    g_VmMic.WritePos = 0;
    g_VmMic.UsedBytes = 0;
    RtlZeroMemory(&g_VmMic.Stats, sizeof(g_VmMic.Stats));

    KeReleaseSpinLock(&g_VmMic.Lock, oldIrql);
}

_Use_decl_annotations_
VOID VmMicStop()
{
    KIRQL oldIrql;
    KeAcquireSpinLock(&g_VmMic.Lock, &oldIrql);

    g_VmMic.Running = FALSE;
    g_VmMic.ReadPos = 0;
    g_VmMic.WritePos = 0;
    g_VmMic.UsedBytes = 0;
    g_VmMic.Stats.buffer_level = 0;

    KeReleaseSpinLock(&g_VmMic.Lock, oldIrql);
}

_Use_decl_annotations_
BOOLEAN VmMicIsRunning()
{
    BOOLEAN running;
    KIRQL oldIrql;
    KeAcquireSpinLock(&g_VmMic.Lock, &oldIrql);
    running = g_VmMic.Running ? TRUE : FALSE;
    KeReleaseSpinLock(&g_VmMic.Lock, oldIrql);
    return running;
}

_Use_decl_annotations_
ULONG VmMicRead(BYTE* Dest, ULONG BytesToRead)
{
    if (!g_VmMic.Initialized || Dest == NULL || BytesToRead == 0)
    {
        return 0;
    }

    KIRQL oldIrql;
    KeAcquireSpinLock(&g_VmMic.Lock, &oldIrql);

    if (!g_VmMic.Running)
    {
        KeReleaseSpinLock(&g_VmMic.Lock, oldIrql);
        return 0;
    }

    ULONG available = g_VmMic.UsedBytes;
    ULONG toRead = min(BytesToRead, available);

    if (toRead > 0)
    {
        VmMicCopyOut(g_VmMic.Buffer, g_VmMic.BufferBytes, &g_VmMic.ReadPos, Dest, toRead);
        g_VmMic.UsedBytes -= toRead;
        g_VmMic.Stats.bytes_out += toRead;
    }

    if (toRead < BytesToRead)
    {
        g_VmMic.Stats.underruns += 1;
    }

    g_VmMic.Stats.buffer_level = g_VmMic.UsedBytes;

    KeReleaseSpinLock(&g_VmMic.Lock, oldIrql);
    return toRead;
}

_Use_decl_annotations_
NTSTATUS VmMicWrite(const BYTE* Src, ULONG BytesToWrite)
{
    if (!g_VmMic.Initialized || Src == NULL || BytesToWrite == 0)
    {
        return STATUS_INVALID_PARAMETER;
    }

    KIRQL oldIrql;
    KeAcquireSpinLock(&g_VmMic.Lock, &oldIrql);

    if (!g_VmMic.Running)
    {
        KeReleaseSpinLock(&g_VmMic.Lock, oldIrql);
        return STATUS_DEVICE_NOT_READY;
    }

    ULONG drop = 0;
    if (BytesToWrite > g_VmMic.BufferBytes)
    {
        drop = BytesToWrite - g_VmMic.BufferBytes;
        Src += drop;
        BytesToWrite = g_VmMic.BufferBytes;
    }

    ULONG freeSpace = g_VmMic.BufferBytes - g_VmMic.UsedBytes;
    if (BytesToWrite > freeSpace)
    {
        drop += (BytesToWrite - freeSpace);
        VmMicAdvance(&g_VmMic.ReadPos, BytesToWrite - freeSpace, g_VmMic.BufferBytes);
        g_VmMic.UsedBytes = g_VmMic.BufferBytes - BytesToWrite;
    }

    if (drop > 0)
    {
        g_VmMic.Stats.overruns += 1;
    }

    VmMicCopyIn(g_VmMic.Buffer, g_VmMic.BufferBytes, &g_VmMic.WritePos, Src, BytesToWrite);
    g_VmMic.UsedBytes += BytesToWrite;
    g_VmMic.Stats.bytes_in += BytesToWrite;
    g_VmMic.Stats.buffer_level = g_VmMic.UsedBytes;

    KeReleaseSpinLock(&g_VmMic.Lock, oldIrql);
    return STATUS_SUCCESS;
}

_Use_decl_annotations_
VOID VmMicGetFormat(PVM_MIC_FORMAT Format)
{
    if (Format == NULL)
    {
        return;
    }

    Format->sample_rate = VM_MIC_SAMPLE_RATE;
    Format->channels = VM_MIC_CHANNELS;
    Format->bits_per_sample = VM_MIC_BITS_PER_SAMPLE;
    Format->block_align = VM_MIC_BLOCK_ALIGN;
}

_Use_decl_annotations_
VOID VmMicGetStats(PVM_MIC_STATS Stats)
{
    if (Stats == NULL)
    {
        return;
    }

    KIRQL oldIrql;
    KeAcquireSpinLock(&g_VmMic.Lock, &oldIrql);
    *Stats = g_VmMic.Stats;
    Stats->buffer_level = g_VmMic.UsedBytes;
    KeReleaseSpinLock(&g_VmMic.Lock, oldIrql);
}
