#pragma once

#include "vm_mic_common.h"

int RunLoopbackMode(
    HANDLE device,
    bool   stats,
    int    statsIntervalMs);
