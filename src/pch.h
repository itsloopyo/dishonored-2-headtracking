// SPDX-License-Identifier: MIT
// Copyright (c) 2026 itsloopyo
#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <atomic>
#include <thread>
#include <string>
#include <vector>
#include <cstdint>
#include <cmath>
#include <chrono>

#include "core/constants.h"
