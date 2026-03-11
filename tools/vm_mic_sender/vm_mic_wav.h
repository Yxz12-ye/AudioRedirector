#pragma once

#include "vm_mic_common.h"

int RunWavMode(
    HANDLE         device,
    const wchar_t* wavPath,
    bool           stats,
    int            statsIntervalMs);
