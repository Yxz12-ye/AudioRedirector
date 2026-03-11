#pragma once

#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <avrt.h>
#include <ksmedia.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <cwctype>
#include <cstddef>
#include <vector>
#include <atomic>
#include <chrono>
#include <cmath>

#include "inject_protocol.h"

// Simple COM pointer helper with RAII semantics.
template <typename T>
struct ComPtr
{
    T* ptr = nullptr;
    ~ComPtr() { if (ptr) { ptr->Release(); ptr = nullptr; } }
    T** operator&() { return &ptr; }
    T* operator->() const { return ptr; }
    T* get() const { return ptr; }
    void reset(T* p = nullptr) { if (ptr) ptr->Release(); ptr = p; }
    ComPtr() = default;
    ComPtr(const ComPtr&) = delete;
    ComPtr& operator=(const ComPtr&) = delete;
};

struct InputFormat
{
    enum class Type { Float32, Int16 };
    Type   type       = Type::Float32;
    UINT32 channels   = 0;
    UINT32 sampleRate = 0;
    UINT32 blockAlign = 0;
};

enum class SourceMode
{
    Loopback,
    WavFile
};

// Global running flag controlled by console control handler.
inline std::atomic<bool> g_running(true);

inline BOOL WINAPI ConsoleHandler(DWORD ctrlType)
{
    switch (ctrlType)
    {
    case CTRL_C_EVENT:
    case CTRL_BREAK_EVENT:
    case CTRL_CLOSE_EVENT:
        g_running = false;
        return TRUE;
    default:
        return FALSE;
    }
}

inline int16_t FloatToInt16(float v)
{
    if (v > 1.0f) v = 1.0f;
    if (v < -1.0f) v = -1.0f;
    float scaled = v * 32767.0f;
    if (scaled >= 0.0f)
    {
        return static_cast<int16_t>(scaled + 0.5f);
    }
    return static_cast<int16_t>(scaled - 0.5f);
}

inline bool ParseFormat(const WAVEFORMATEX* wfx, InputFormat& out)
{
    if (!wfx)
    {
        return false;
    }

    out.channels   = wfx->nChannels;
    out.sampleRate = wfx->nSamplesPerSec;
    out.blockAlign = wfx->nBlockAlign;

    if (wfx->wFormatTag == WAVE_FORMAT_IEEE_FLOAT && wfx->wBitsPerSample == 32)
    {
        out.type = InputFormat::Type::Float32;
        return true;
    }

    if (wfx->wFormatTag == WAVE_FORMAT_PCM && wfx->wBitsPerSample == 16)
    {
        out.type = InputFormat::Type::Int16;
        return true;
    }

    if (wfx->wFormatTag == WAVE_FORMAT_EXTENSIBLE)
    {
        auto wfe = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(wfx);
        if (IsEqualGUID(wfe->SubFormat, KSDATAFORMAT_SUBTYPE_IEEE_FLOAT) && wfx->wBitsPerSample == 32)
        {
            out.type = InputFormat::Type::Float32;
            return true;
        }
        if (IsEqualGUID(wfe->SubFormat, KSDATAFORMAT_SUBTYPE_PCM) && wfx->wBitsPerSample == 16)
        {
            out.type = InputFormat::Type::Int16;
            return true;
        }
    }

    return false;
}

// Very small linear resampler feeding 16-bit output frames.
struct Resampler
{
    double              ratio  = 1.0; // input_rate / output_rate
    double              srcPos = 0.0;
    std::vector<float>  buffer;
    size_t              start  = 0;

    Resampler(double inRate, double outRate)
    {
        ratio  = inRate / outRate;
        srcPos = 0.0;
    }

    void push(const float* samples, size_t count)
    {
        buffer.insert(buffer.end(), samples, samples + count);
    }

    void produce(std::vector<int16_t>& out)
    {
        while (start + static_cast<size_t>(srcPos) + 1 < buffer.size())
        {
            size_t idx  = start + static_cast<size_t>(srcPos);
            double frac = srcPos - floor(srcPos);
            float  s0   = buffer[idx];
            float  s1   = buffer[idx + 1];
            float  v    = s0 + (s1 - s0) * static_cast<float>(frac);
            out.push_back(FloatToInt16(v));
            srcPos += ratio;
        }

        size_t consume = static_cast<size_t>(srcPos);
        start += consume;
        srcPos -= consume;

        if (start > 4096 && start > buffer.size() / 2)
        {
            buffer.erase(buffer.begin(), buffer.begin() + static_cast<ptrdiff_t>(start));
            start = 0;
        }
    }
};
