// SPDX-FileCopyrightText: Copyright 2026 bbport contributors
// SPDX-License-Identifier: GPL-2.0-or-later
// bbport buffers: how the GPU side reaches the game's memory. The GPU uses the game's direct
// memory where it is (its chunks are exported to or imported by Vulkan) and keeps what it reads
// often in VRAM copies; writers announce their writes (no page protection). A driver that cannot
// do this stops the game with the reason (GuestMemoryUnusable).
//
// Ported from upstream bloodborne_pc 0.7 (gpu/buffers/memory_model.h). The Windows fork has no
// dma-buf and no VK_EXT_external_memory_host import yet: the GPU reads guest memory through
// device-local mirrors (see layer/bblayer_gpu_memory.h and buffer_cache), so the in-place
// helpers are capability checks here and the copy path stays in place.
#pragma once

#include "common/types.h"

namespace VideoCore {

/// The driver cannot give the game's memory to the GPU in place: the game stops with `why`.
[[noreturn]] void GuestMemoryUnusable(const char* why);
/// BB_WRITE_VERIFY=1 (diagnostics): watched pages are protected as well, so writes nothing
/// announced show up as write faults.
bool WriteVerify();
/// BB_CONSTANTS_IN_PLACE=1: small read-only buffers (constants) are read where the game wrote
/// them, when the draw executes, instead of being copied when the draw is decoded or recorded.
/// The fork has no host-memory import, so this reports the requested mode and callers fall back
/// to the stream buffer.
bool ConstantsInPlace();

} // namespace VideoCore
