// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: FSR 4 (source-v07 INT8/DOT4 model, FSR-Vulkan provider) as an alternative to the
// FSR 3.1 context of TemporalUpscaler. It consumes the same inputs: HDR scene color, depth,
// motion vectors in render pixels (previous - current) and the jitter offset.
//
// The model is per preset (native, quality, balanced, performance, ultraperf) and per output
// tier; its SPIR-V passes and weights come from tools/fetch_fsr4_assets.sh (BB_FSR4_DIR overrides
// the fsr4_shaders directory). A preset change rebuilds the model graph.

#pragma once

#include <array>
#include <memory>
#include "common/types.h"
#include "video_core/renderer_vulkan/vk_common.h"

namespace Vulkan {

class Instance;
class Scheduler;

class Fsr4Upscaler {
public:
    Fsr4Upscaler(const Instance& instance, Scheduler& scheduler);
    ~Fsr4Upscaler();

    /// An image in layout General, written/read by compute shaders.
    struct Image {
        vk::Image image;
        vk::ImageView view;
        u32 width, height;
        vk::Format format = vk::Format::eUndefined; ///< needed by DLSS (vk_dlss.h)
    };
    struct Frame {
        vk::CommandBuffer cmdbuf;
        Image color, depth, motion, output;
        u32 render_width, render_height;
        int preset; ///< BbSettings::Preset
        std::array<float, 2> jitter;
        float frame_ms, near_plane, far_plane, vertical_fov;
        float sharpness;
        bool sharpen, reset, auto_exposure;
        /// bbport: the R8 reactive mask at render size, or none (view null). DLSS takes it as its
        /// bias-current-colour mask; FSR 4 does not read it (RecordFsr4Reactive blends after).
        Image reactive{};
    };

    /// Records FSR 4 into `frame.cmdbuf`; the output stays in General. False when FSR 4
    /// cannot run (Problem() says why; missing assets or device features are permanent).
    bool Record(const Frame& frame);

    /// Starts building the model for `preset` and that output size on a worker thread, so the
    /// first frame that asks for FSR 4 finds it ready (building it compiles its passes, ~20 s the
    /// first time on some drivers). Nothing if one is built or being built already.
    void Prewarm(int preset, u32 output_width, u32 output_height);
    /// The model is being built in the background: Record returns false (not fatal) until it is
    /// done, and the caller keeps FSR 3.1 on screen meanwhile.
    [[nodiscard]] bool Preparing() const noexcept;

    /// Why the last Record failed, or null.
    [[nodiscard]] const char* Problem() const noexcept;
    /// Assets or device features are missing: FSR 4 will not run in this session.
    [[nodiscard]] bool Fatal() const noexcept;
    /// bbport: forgets a failure (FSR 4.1.1 failed: the built-in FSR 4 model is tried next).
    void ClearFailure() noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

} // namespace Vulkan
