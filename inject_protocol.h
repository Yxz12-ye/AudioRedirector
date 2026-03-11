#pragma once

// Requires Windows headers (windows.h or ntddk.h) to be included before this file.

// Device names
#define VM_MIC_DEVICE_NAME   L"\\Device\\SysvadVmMic"
#define VM_MIC_DOSDEVICE_NAME L"\\DosDevices\\SysvadVmMic"
#define VM_MIC_WIN32_NAME    L"\\\\.\\SysvadVmMic"

// IOCTL helpers for user-mode builds
#ifndef CTL_CODE
#define CTL_CODE(DeviceType, Function, Method, Access) ( \
    ((DeviceType) << 16) | ((Access) << 14) | ((Function) << 2) | (Method) )
#endif

#ifndef FILE_DEVICE_UNKNOWN
#define FILE_DEVICE_UNKNOWN 0x00000022
#endif

#ifndef METHOD_BUFFERED
#define METHOD_BUFFERED 0
#endif

#ifndef FILE_ANY_ACCESS
#define FILE_ANY_ACCESS 0
#endif

// Fixed PoC format: 48kHz, mono, 16-bit PCM
#define VM_MIC_SAMPLE_RATE     48000
#define VM_MIC_CHANNELS        1
#define VM_MIC_BITS_PER_SAMPLE 16
#define VM_MIC_BLOCK_ALIGN     2

// IOCTLs
#define IOCTL_VM_MIC_GET_FORMAT CTL_CODE(FILE_DEVICE_UNKNOWN, 0x800, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_VM_MIC_START      CTL_CODE(FILE_DEVICE_UNKNOWN, 0x801, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_VM_MIC_STOP       CTL_CODE(FILE_DEVICE_UNKNOWN, 0x802, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_VM_MIC_WRITE      CTL_CODE(FILE_DEVICE_UNKNOWN, 0x803, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_VM_MIC_GET_STATS  CTL_CODE(FILE_DEVICE_UNKNOWN, 0x804, METHOD_BUFFERED, FILE_ANY_ACCESS)

typedef struct _VM_MIC_FORMAT
{
    ULONG  sample_rate;
    USHORT channels;
    USHORT bits_per_sample;
    ULONG  block_align;
} VM_MIC_FORMAT, *PVM_MIC_FORMAT;

typedef struct _VM_MIC_WRITE_PACKET
{
    ULONG payload_bytes;   // size of following PCM payload in bytes
    ULONG frame_count;     // number of PCM frames in payload
    ULONG flags;           // reserved for future use
    UCHAR payload[1];      // variable length payload
} VM_MIC_WRITE_PACKET, *PVM_MIC_WRITE_PACKET;

typedef struct _VM_MIC_STATS
{
    ULONGLONG bytes_in;
    ULONGLONG bytes_out;
    ULONGLONG overruns;
    ULONGLONG underruns;
    ULONGLONG buffer_level; // bytes currently buffered
} VM_MIC_STATS, *PVM_MIC_STATS;
