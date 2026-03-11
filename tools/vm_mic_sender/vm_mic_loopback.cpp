#include "vm_mic_loopback.h"

int RunLoopbackMode(
    HANDLE device,
    bool   stats,
    int    statsIntervalMs)
{
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(hr))
    {
        printf("CoInitializeEx failed (hr=0x%08lx)\n", hr);
        return 1;
    }

    ComPtr<IMMDeviceEnumerator> enumerator;
    hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                          __uuidof(IMMDeviceEnumerator), (void**)&enumerator);
    if (FAILED(hr))
    {
        printf("CoCreateInstance(MMDeviceEnumerator) failed (hr=0x%08lx)\n", hr);
        CoUninitialize();
        return 1;
    }

    ComPtr<IMMDevice> defaultDevice;
    hr = enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &defaultDevice);
    if (FAILED(hr))
    {
        printf("GetDefaultAudioEndpoint failed (hr=0x%08lx)\n", hr);
        CoUninitialize();
        return 1;
    }

    ComPtr<IAudioClient> audioClient;
    hr = defaultDevice->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&audioClient);
    if (FAILED(hr))
    {
        printf("Activate(IAudioClient) failed (hr=0x%08lx)\n", hr);
        CoUninitialize();
        return 1;
    }

    WAVEFORMATEX* mixFormat = nullptr;
    hr = audioClient->GetMixFormat(&mixFormat);
    if (FAILED(hr) || mixFormat == nullptr)
    {
        printf("GetMixFormat failed (hr=0x%08lx)\n", hr);
        CoUninitialize();
        return 1;
    }

    InputFormat inFormat;
    if (!ParseFormat(mixFormat, inFormat))
    {
        printf("Unsupported mix format (tag=0x%04x, bits=%u)\n",
               mixFormat->wFormatTag, mixFormat->wBitsPerSample);
        CoTaskMemFree(mixFormat);
        CoUninitialize();
        return 1;
    }

    REFERENCE_TIME defaultPeriod = 0;
    REFERENCE_TIME minPeriod     = 0;
    hr = audioClient->GetDevicePeriod(&defaultPeriod, &minPeriod);
    if (FAILED(hr))
    {
        printf("GetDevicePeriod failed (hr=0x%08lx)\n", hr);
        CoTaskMemFree(mixFormat);
        CoUninitialize();
        return 1;
    }

    REFERENCE_TIME bufferDuration = defaultPeriod * 4;
    hr = audioClient->Initialize(AUDCLNT_SHAREMODE_SHARED,
                                 AUDCLNT_STREAMFLAGS_LOOPBACK | AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
                                 bufferDuration,
                                 0,
                                 mixFormat,
                                 nullptr);
    if (FAILED(hr))
    {
        printf("IAudioClient::Initialize failed (hr=0x%08lx)\n", hr);
        CoTaskMemFree(mixFormat);
        CoUninitialize();
        return 1;
    }

    HANDLE hEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!hEvent)
    {
        printf("CreateEvent failed (err=%lu)\n", GetLastError());
        CoTaskMemFree(mixFormat);
        CoUninitialize();
        return 1;
    }

    hr = audioClient->SetEventHandle(hEvent);
    if (FAILED(hr))
    {
        printf("SetEventHandle failed (hr=0x%08lx)\n", hr);
        CloseHandle(hEvent);
        CoTaskMemFree(mixFormat);
        CoUninitialize();
        return 1;
    }

    ComPtr<IAudioCaptureClient> captureClient;
    hr = audioClient->GetService(__uuidof(IAudioCaptureClient), (void**)&captureClient);
    if (FAILED(hr))
    {
        printf("GetService(IAudioCaptureClient) failed (hr=0x%08lx)\n", hr);
        CloseHandle(hEvent);
        CoTaskMemFree(mixFormat);
        CoUninitialize();
        return 1;
    }

    hr = audioClient->Start();
    if (FAILED(hr))
    {
        printf("IAudioClient::Start failed (hr=0x%08lx)\n", hr);
        CloseHandle(hEvent);
        CoTaskMemFree(mixFormat);
        CoUninitialize();
        return 1;
    }

    DWORD taskIndex = 0;
    HANDLE mmcss = AvSetMmThreadCharacteristicsW(L"Pro Audio", &taskIndex);

    const UINT32 outSampleRate = VM_MIC_SAMPLE_RATE;
    Resampler    resampler(static_cast<double>(inFormat.sampleRate), static_cast<double>(outSampleRate));
    std::vector<float>   mono;
    std::vector<int16_t> pending;
    size_t               pendingStart  = 0;
    const UINT32         chunkFrames   = 480; // 10ms at 48kHz

    auto   nextStats      = std::chrono::steady_clock::now() + std::chrono::milliseconds(statsIntervalMs);
    const double bytesPerSecond = static_cast<double>(VM_MIC_SAMPLE_RATE * VM_MIC_BLOCK_ALIGN);
    DWORD  bytesReturned  = 0;

    while (g_running)
    {
        DWORD waitRes = WaitForSingleObject(hEvent, 2000);
        if (waitRes != WAIT_OBJECT_0)
        {
            continue;
        }

        while (true)
        {
            UINT32 packetLength = 0;
            hr = captureClient->GetNextPacketSize(&packetLength);
            if (FAILED(hr) || packetLength == 0)
            {
                break;
            }

            BYTE*  data      = nullptr;
            UINT32 numFrames = 0;
            DWORD  flags     = 0;
            hr = captureClient->GetBuffer(&data, &numFrames, &flags, nullptr, nullptr);
            if (FAILED(hr))
            {
                break;
            }

            mono.resize(numFrames);
            if (flags & AUDCLNT_BUFFERFLAGS_SILENT)
            {
                memset(mono.data(), 0, numFrames * sizeof(float));
            }
            else
            {
                if (inFormat.type == InputFormat::Type::Float32)
                {
                    const float* in = reinterpret_cast<const float*>(data);
                    for (UINT32 f = 0; f < numFrames; ++f)
                    {
                        float sum = 0.0f;
                        for (UINT32 ch = 0; ch < inFormat.channels; ++ch)
                        {
                            sum += in[f * inFormat.channels + ch];
                        }
                        mono[f] = sum / static_cast<float>(inFormat.channels);
                    }
                }
                else
                {
                    const int16_t* in = reinterpret_cast<const int16_t*>(data);
                    for (UINT32 f = 0; f < numFrames; ++f)
                    {
                        int32_t sum = 0;
                        for (UINT32 ch = 0; ch < inFormat.channels; ++ch)
                        {
                            sum += in[f * inFormat.channels + ch];
                        }
                        float v = static_cast<float>(sum) / (static_cast<float>(inFormat.channels) * 32768.0f);
                        mono[f] = v;
                    }
                }
            }

            captureClient->ReleaseBuffer(numFrames);

            resampler.push(mono.data(), mono.size());
            resampler.produce(pending);

            while (pending.size() - pendingStart >= chunkFrames)
            {
                size_t payloadBytes = chunkFrames * sizeof(int16_t);
                size_t headerBytes  = sizeof(VM_MIC_WRITE_PACKET) - 1;
                std::vector<uint8_t> ioBuf(headerBytes + payloadBytes);
                auto* pkt = reinterpret_cast<VM_MIC_WRITE_PACKET*>(ioBuf.data());
                pkt->payload_bytes = static_cast<ULONG>(payloadBytes);
                pkt->frame_count   = chunkFrames;
                pkt->flags         = 0;
                memcpy(pkt->payload, pending.data() + pendingStart, payloadBytes);

                if (!DeviceIoControl(device, IOCTL_VM_MIC_WRITE,
                                     ioBuf.data(),
                                     static_cast<DWORD>(ioBuf.size()),
                                     nullptr, 0, &bytesReturned, nullptr))
                {
                    printf("IOCTL_VM_MIC_WRITE failed (err=%lu)\n", GetLastError());
                    g_running = false;
                    break;
                }

                pendingStart += chunkFrames;

                if (pendingStart > 4096 && pendingStart > pending.size() / 2)
                {
                    pending.erase(pending.begin(), pending.begin() + static_cast<ptrdiff_t>(pendingStart));
                    pendingStart = 0;
                }
            }
        }

        if (stats)
        {
            auto now = std::chrono::steady_clock::now();
            if (now >= nextStats)
            {
                VM_MIC_STATS s = {};
                if (DeviceIoControl(device, IOCTL_VM_MIC_GET_STATS, nullptr, 0,
                                    &s, sizeof(s), &bytesReturned, nullptr))
                {
                    double levelMs = (bytesPerSecond > 0.0) ? (static_cast<double>(s.buffer_level) * 1000.0 / bytesPerSecond) : 0.0;
                    printf("stats: in=%llu out=%llu over=%llu under=%llu level=%llu (%.1f ms)\n",
                           s.bytes_in, s.bytes_out, s.overruns, s.underruns, s.buffer_level, levelMs);
                }
                nextStats = now + std::chrono::milliseconds(statsIntervalMs);
            }
        }
    }

    if (mmcss)
    {
        AvRevertMmThreadCharacteristics(mmcss);
    }

    audioClient->Stop();
    CloseHandle(hEvent);
    CoTaskMemFree(mixFormat);
    CoUninitialize();
    return 0;
}
