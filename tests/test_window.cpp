// SPDX-License-Identifier: GPL-2.0-or-later
// macOS: Cocoa runs windows on the main thread only. bbgpu_init creates the window, Vulkan
// device, presenter and swapchain there, and bbgpu_run_window_loop runs its events (opens a
// small window for 3 seconds; no game files).
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <thread>
extern "C" {
#include "gpu/bbgpu.h"
}

int main() {
    BbGpuConfig config{};
    config.title = "bbport window test";
    config.serial = "TEST00000";
    config.user_dir = "out/window-test";
    config.width = 640;
    config.height = 360;
    if (bbgpu_init(&config)) {
        std::puts("Window: bbgpu_init failed");
        return 1;
    }
    std::thread([] {
        std::this_thread::sleep_for(std::chrono::seconds(3));
        std::puts("Window: created and run on the main thread for 3 s PASS");
        std::fflush(stdout);
        std::_Exit(0);
    }).detach();
    bbgpu_run_window_loop();
    return 1;
}
