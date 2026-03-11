#include "vm_mic_wav.h"

static bool LoadPcm16Wav(
    const wchar_t* path,
    UINT32&        outChannels,
    UINT32&        outSampleRate,
    std::vector<float>& outMono)
{
    HANDLE hFile = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hFile == INVALID_HANDLE_VALUE)
    {
        wprintf(L"Failed to open WAV file %ls (err=%lu)\n", path, GetLastError());
        return false;
    }

    LARGE_INTEGER sizeLi{};
    if (!GetFileSizeEx(hFile, &sizeLi) || sizeLi.QuadPart < 44)
    {
        CloseHandle(hFile);
        printf("WAV file too small.\n");
        return false;
    }

    const size_t fileSize = static_cast<size_t>(sizeLi.QuadPart);
    std::vector<uint8_t> data(fileSize);
    DWORD bytesRead = 0;
    if (!ReadFile(hFile, data.data(), (DWORD)fileSize, &bytesRead, nullptr) ||
        bytesRead != fileSize)
    {
        CloseHandle(hFile);
        printf("Failed to read WAV file.\n");
        return false;
    }
    CloseHandle(hFile);

    const uint8_t* p   = data.data();
    const uint8_t* end = data.data() + fileSize;

    // RIFF header
    if (end - p < 12 ||
        memcmp(p, "RIFF", 4) != 0 ||
        memcmp(p + 8, "WAVE", 4) != 0)
    {
        printf("Not a RIFF/WAVE file.\n");
        return false;
    }
    p += 12;

    bool         haveFmt  = false;
    bool         haveData = false;
    WAVEFORMATEX wf{};
    const uint8_t* pcmData  = nullptr;
    DWORD          pcmBytes = 0;

    while (p + 8 <= end)
    {
        char id[5] = {};
        memcpy(id, p, 4);
        DWORD chunkSize = 0;
        memcpy(&chunkSize, p + 4, 4);
        p += 8;

        const uint8_t* chunkData = p;
        const uint8_t* next      = p + chunkSize;
        if (next > end) break;

        if (memcmp(id, "fmt ", 4) == 0)
        {
            // 标准 PCM WAV 的 fmt chunk 常见大小是 16 (PCMWAVEFORMAT)，
            // 比 sizeof(WAVEFORMATEX) 小，这里只要求 >=16 即可。
            if (chunkSize < 16)
            {
                printf("fmt chunk too small (size=%u).\n", chunkSize);
                return false;
            }

            ZeroMemory(&wf, sizeof(wf));
            DWORD copySize = chunkSize;
            if (copySize > sizeof(WAVEFORMATEX))
            {
                copySize = sizeof(WAVEFORMATEX);
            }
            memcpy(&wf, chunkData, copySize);

            if (wf.wFormatTag != WAVE_FORMAT_PCM || wf.wBitsPerSample != 16)
            {
                printf("Only PCM 16-bit WAV is supported (tag=0x%04x bits=%u).\n",
                    wf.wFormatTag, wf.wBitsPerSample);
                return false;
            }
            haveFmt = true;
        }
        else if (memcmp(id, "data", 4) == 0)
        {
            pcmData  = chunkData;
            pcmBytes = chunkSize;
            haveData = true;
        }

        p = next;
        // chunks are word-aligned
        if ((chunkSize & 1) && p < end) ++p;
    }

    if (!haveFmt || !haveData || pcmData == nullptr || pcmBytes == 0)
    {
        printf("WAV missing fmt or data chunk.\n");
        return false;
    }

    outChannels   = wf.nChannels;
    outSampleRate = wf.nSamplesPerSec;

    const int16_t* inSamples   = reinterpret_cast<const int16_t*>(pcmData);
    const size_t   frameSize   = wf.nChannels; // 16-bit, so blockAlign == 2 * channels
    const size_t   totalFrames = pcmBytes / (sizeof(int16_t) * frameSize);

    outMono.clear();
    outMono.reserve(totalFrames);

    for (size_t f = 0; f < totalFrames; ++f)
    {
        int32_t sum = 0;
        for (UINT32 ch = 0; ch < wf.nChannels; ++ch)
        {
            sum += inSamples[f * frameSize + ch];
        }
        float v = static_cast<float>(sum) /
            (static_cast<float>(wf.nChannels) * 32768.0f);
        outMono.push_back(v);
    }

    return true;
}

int RunWavMode(
    HANDLE         device,
    const wchar_t* wavPath,
    bool           stats,
    int            statsIntervalMs)
{
    UINT32 srcChannels   = 0;
    UINT32 srcSampleRate = 0;
    std::vector<float> mono;
    if (!LoadPcm16Wav(wavPath, srcChannels, srcSampleRate, mono))
    {
        return 1;
    }

    if (mono.empty())
    {
        printf("WAV file has no audio data.\n");
        return 1;
    }

    const UINT32 outSampleRate = VM_MIC_SAMPLE_RATE;
    Resampler    resampler(static_cast<double>(srcSampleRate),
                        static_cast<double>(outSampleRate));
    std::vector<int16_t> pending;
    size_t pendingStart       = 0;
    const UINT32 chunkFrames  = 480; // 10 ms at 48 kHz

    auto nextStats = std::chrono::steady_clock::now() +
        std::chrono::milliseconds(statsIntervalMs);
    const double bytesPerSecond =
        static_cast<double>(VM_MIC_SAMPLE_RATE * VM_MIC_BLOCK_ALIGN);

    DWORD bytesReturned = 0;

    // 把整首歌按块推进去
    size_t       pos         = 0;
    const size_t totalFrames = mono.size();
    const size_t pushBlock   = 4096;

    while (pos < totalFrames && g_running)
    {
        size_t block = (totalFrames - pos > pushBlock)
            ? pushBlock
            : (totalFrames - pos);
        resampler.push(mono.data() + pos, block);
        pos += block;

        resampler.produce(pending);

        while (pending.size() - pendingStart >= chunkFrames && g_running)
        {
            size_t payloadBytes = chunkFrames * sizeof(int16_t);
            size_t headerBytes  = sizeof(VM_MIC_WRITE_PACKET) - 1;
            std::vector<uint8_t> ioBuf(headerBytes + payloadBytes);
            auto* pkt = reinterpret_cast<VM_MIC_WRITE_PACKET*>(ioBuf.data());
            pkt->payload_bytes = static_cast<ULONG>(payloadBytes);
            pkt->frame_count   = chunkFrames;
            pkt->flags         = 0;
            memcpy(pkt->payload,
                pending.data() + pendingStart,
                payloadBytes);

            if (!DeviceIoControl(device,
                IOCTL_VM_MIC_WRITE,
                ioBuf.data(),
                static_cast<DWORD>(ioBuf.size()),
                nullptr,
                0,
                &bytesReturned,
                nullptr))
            {
                printf("IOCTL_VM_MIC_WRITE failed (err=%lu)\n",
                    GetLastError());
                g_running = false;
                break;
            }

            // 粗略按时间节流
            Sleep((DWORD)(1000.0 * chunkFrames / 48000.0));

            pendingStart += chunkFrames;
            if (pendingStart > 4096 && pendingStart > pending.size() / 2)
            {
                pending.erase(pending.begin(),
                    pending.begin() +
                    static_cast<ptrdiff_t>(pendingStart));
                pendingStart = 0;
            }

            if (stats)
            {
                auto now = std::chrono::steady_clock::now();
                if (now >= nextStats)
                {
                    VM_MIC_STATS s{};
                    if (DeviceIoControl(device,
                        IOCTL_VM_MIC_GET_STATS,
                        nullptr,
                        0,
                        &s,
                        sizeof(s),
                        &bytesReturned,
                        nullptr))
                    {
                        double levelMs =
                            (bytesPerSecond > 0.0)
                            ? (static_cast<double>(s.buffer_level) *
                                1000.0 / bytesPerSecond)
                            : 0.0;
                        printf("stats: in=%llu out=%llu over=%llu "
                            "under=%llu level=%llu (%.1f ms)\n",
                            s.bytes_in,
                            s.bytes_out,
                            s.overruns,
                            s.underruns,
                            s.buffer_level,
                            levelMs);
                    }
                    nextStats = now +
                        std::chrono::milliseconds(statsIntervalMs);
                }
            }
        }
    }

    // 把 resampler 里可能剩下的尾巴全部冲掉
    if (g_running)
    {
        resampler.produce(pending);
        while (pending.size() - pendingStart >= chunkFrames && g_running)
        {
            size_t payloadBytes = chunkFrames * sizeof(int16_t);
            size_t headerBytes  = sizeof(VM_MIC_WRITE_PACKET) - 1;
            std::vector<uint8_t> ioBuf(headerBytes + payloadBytes);
            auto* pkt = reinterpret_cast<VM_MIC_WRITE_PACKET*>(ioBuf.data());
            pkt->payload_bytes = static_cast<ULONG>(payloadBytes);
            pkt->frame_count   = chunkFrames;
            pkt->flags         = 0;
            memcpy(pkt->payload,
                pending.data() + pendingStart,
                payloadBytes);

            if (!DeviceIoControl(device,
                IOCTL_VM_MIC_WRITE,
                ioBuf.data(),
                static_cast<DWORD>(ioBuf.size()),
                nullptr,
                0,
                &bytesReturned,
                nullptr))
            {
                printf("IOCTL_VM_MIC_WRITE failed (err=%lu)\n",
                    GetLastError());
                break;
            }

            Sleep((DWORD)(1000.0 * chunkFrames / 48000.0));

            pendingStart += chunkFrames;
            if (pendingStart > 4096 && pendingStart > pending.size() / 2)
            {
                pending.erase(pending.begin(),
                    pending.begin() + static_cast<ptrdiff_t>(pendingStart));
                pendingStart = 0;
            }
        }
    }

    return 0;
}
