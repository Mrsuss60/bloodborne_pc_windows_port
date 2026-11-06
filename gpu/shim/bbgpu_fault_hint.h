// bbport: flags that are only valid while a guest access violation is being handled.
// src/probe.c sets them around bbgpu_handle_fault(); the page tracker reads them.
#pragma once

namespace BbFaultHint {

/// The faulting guest instruction is inside a PS4 routine that keeps locals in the System V red
/// zone (the particle, blood-splatter and cloth vertex generators). Windows x64 writes its
/// exception frame below RSP on every dispatch, which overwrites those locals, so the routine
/// must not take another write-tracking fault on the same page. The page is kept writable and
/// CPU-modified for the rest of the session instead.
inline thread_local bool redzone = false;

} // namespace BbFaultHint
