// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "common/assert.h"
#include "video_core/renderer_vulkan/vk_shader_util.h"

namespace Vulkan {

vk::ShaderModule CompileSPV(std::span<const u32> code, vk::Device device) {
    if (code.empty()) {
        LOG_CRITICAL(Render_Vulkan, "CompileSPV: empty SPIR-V module");
        return {};
    }
    const vk::ShaderModuleCreateInfo shader_info = {
        .codeSize = code.size() * sizeof(u32),
        .pCode = code.data(),
    };

    auto [module_result, module] = device.createShaderModule(shader_info);
    /* bbport: a shader the driver rejects used to abort the process (exit code 23). The callers
     * that build pipelines handle a null module (the pipeline then fails to create and is
     * skipped); a host pass with static SPIR-V only gets here on a broken device. */
    if (module_result != vk::Result::eSuccess) {
        LOG_CRITICAL(Render_Vulkan, "Failed to compile SPIR-V shader: {}",
                     vk::to_string(module_result));
        return {};
    }
    return module;
}

} // namespace Vulkan
