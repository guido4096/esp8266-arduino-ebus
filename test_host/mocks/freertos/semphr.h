// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "FreeRTOS.h"

// Host task creation is disabled, so completion semaphores are unavailable too.
inline SemaphoreHandle_t xSemaphoreCreateBinary() { return nullptr; }
