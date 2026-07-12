// Copyright 2024-present Niobium Microsystems, Inc.
// Licensed under the Apache License, Version 2.0.
//
// Point-in-time resident-set-size probe. Telemetry only: the memory
// budget is enforced on the simulator's own poly ledger (allocator
// slack and library statics make RSS the wrong enforcement variable).

#pragma once

#if defined(__APPLE__)
#include <mach/mach.h>
#elif defined(__linux__)
#include <cstdio>
#include <cstring>
#endif

namespace niobium::fhetch_sim {

// Current resident set size in MiB; 0 where unsupported.
inline long current_rss_mb() {
#if defined(__linux__)
    FILE* f = std::fopen("/proc/self/status", "r");
    if (f == nullptr) return 0;
    long kb = 0;
    char line[256];
    while (std::fgets(line, sizeof(line), f) != nullptr) {
        if (std::strncmp(line, "VmRSS:", 6) == 0) {
            std::sscanf(line + 6, "%ld", &kb);
            break;
        }
    }
    std::fclose(f);
    return kb / 1024;
#elif defined(__APPLE__)
    mach_task_basic_info info;
    mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
    if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO,
                  reinterpret_cast<task_info_t>(&info), &count) != KERN_SUCCESS)
        return 0;
    return static_cast<long>(info.resident_size / (1024 * 1024));
#else
    return 0;
#endif
}

}  // namespace niobium::fhetch_sim
