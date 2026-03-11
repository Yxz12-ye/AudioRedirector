#include "vm_mic_common.h"
#include "vm_mic_wav.h"
#include "vm_mic_loopback.h"

static void PrintUsage()
{
    printf("vm_mic_sender [--stats [interval_ms]] [--wav path_to_pcm16.wav]\n");
    printf("  (no --wav): capture default render (WASAPI loopback) and inject\n");
    printf("  --wav:      play local PCM 16-bit WAV file into virtual mic\n");
}

int wmain(int argc, wchar_t** argv)
{
    bool           stats          = false;
    int            statsIntervalMs = 1000;
    SourceMode     mode           = SourceMode::Loopback;
    const wchar_t* wavPath        = nullptr;

    for (int i = 1; i < argc; ++i)
    {
        if (_wcsicmp(argv[i], L"--stats") == 0)
        {
            stats = true;
            if (i + 1 < argc && iswdigit(argv[i + 1][0]))
            {
                statsIntervalMs = _wtoi(argv[++i]);
            }
        }
        else if (_wcsicmp(argv[i], L"--wav") == 0)
        {
            if (i + 1 >= argc)
            {
                PrintUsage();
                return 1;
            }
            if (mode != SourceMode::Loopback)
            {
                PrintUsage();
                return 1;
            }
            mode    = SourceMode::WavFile;
            wavPath = argv[++i];
        }
        else if (_wcsicmp(argv[i], L"--help") == 0 ||
                 _wcsicmp(argv[i], L"-h") == 0)
        {
            PrintUsage();
            return 0;
        }
        else
        {
            PrintUsage();
            return 1;
        }
    }

    if (statsIntervalMs <= 0)
    {
        statsIntervalMs = 1000;
    }

    if (!SetConsoleCtrlHandler(ConsoleHandler, TRUE))
    {
        printf("Warning: failed to set console handler.\n");
    }

    HANDLE device = CreateFileW(VM_MIC_WIN32_NAME,
                                GENERIC_READ | GENERIC_WRITE,
                                FILE_SHARE_READ | FILE_SHARE_WRITE,
                                nullptr,
                                OPEN_EXISTING,
                                FILE_ATTRIBUTE_NORMAL,
                                nullptr);
    if (device == INVALID_HANDLE_VALUE)
    {
        printf("Failed to open %ls (err=%lu)\n", VM_MIC_WIN32_NAME, GetLastError());
        return 1;
    }

    VM_MIC_FORMAT vmFormat = {};
    DWORD         bytesReturned = 0;
    if (!DeviceIoControl(device, IOCTL_VM_MIC_GET_FORMAT, nullptr, 0,
                         &vmFormat, sizeof(vmFormat), &bytesReturned, nullptr))
    {
        printf("IOCTL_VM_MIC_GET_FORMAT failed (err=%lu)\n", GetLastError());
        CloseHandle(device);
        return 1;
    }

    if (vmFormat.sample_rate != VM_MIC_SAMPLE_RATE ||
        vmFormat.channels    != VM_MIC_CHANNELS ||
        vmFormat.bits_per_sample != VM_MIC_BITS_PER_SAMPLE)
    {
        printf("Driver format mismatch: %lu Hz, %u ch, %u bits\n",
               vmFormat.sample_rate, vmFormat.channels, vmFormat.bits_per_sample);
        CloseHandle(device);
        return 1;
    }

    if (!DeviceIoControl(device, IOCTL_VM_MIC_START, nullptr, 0, nullptr, 0, &bytesReturned, nullptr))
    {
        printf("IOCTL_VM_MIC_START failed (err=%lu)\n", GetLastError());
        CloseHandle(device);
        return 1;
    }

    int rc = 0;
    if (mode == SourceMode::WavFile)
    {
        rc = RunWavMode(device, wavPath, stats, statsIntervalMs);
    }
    else
    {
        rc = RunLoopbackMode(device, stats, statsIntervalMs);
    }

    // Best-effort stop; driver will ignore if already stopped.
    DeviceIoControl(device, IOCTL_VM_MIC_STOP, nullptr, 0, nullptr, 0, &bytesReturned, nullptr);

    CloseHandle(device);
    return rc;
}
