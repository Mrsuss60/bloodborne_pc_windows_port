// SPDX-FileCopyrightText: Copyright 2026 bbport contributors
// SPDX-License-Identifier: GPL-2.0-or-later
#include "buffers/memory_model.h"
#include "shim/bb_env.h"

#include <cstdio>
#include <cstdlib>

namespace VideoCore {

void GuestMemoryUnusable(const char* why) {
    std::printf("STOP: the GPU driver cannot use the game's memory in place: %s. The port needs "
                "dma-buf export of system memory or VK_EXT_external_memory_host (a current Mesa, "
                "NVIDIA or Windows driver); please report the GPU and driver with this log.\n",
                why);
    std::fflush(stdout);
    std::exit(23);
}

bool WriteVerify() {
    static const bool on = bb_env_on("BB_WRITE_VERIFY");
    return on;
}

bool ConstantsInPlace() {
    // Upstream binds the guest's constant data in place (host-memory import). The fork copies
    // small read-only buffers into its stream buffer, so the switch is reported but not used to
    // change the binding path.
    static const bool on = bb_env_on("BB_CONSTANTS_IN_PLACE");
    return on;
}

} // namespace VideoCore
