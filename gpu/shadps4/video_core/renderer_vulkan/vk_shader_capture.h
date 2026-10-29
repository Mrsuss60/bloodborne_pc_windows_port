// SPDX-FileCopyrightText: Copyright 2026 bbport contributors
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <array>
#include <span>
#include <vector>

#include "common/types.h"
#include "shader_recompiler/backend/bindings.h"
#include "shader_recompiler/info.h"
#include "shader_recompiler/runtime_info.h"

namespace Shader {
struct Info;
struct RuntimeInfo;
struct Profile;
namespace Backend {
struct Bindings;
}
} // namespace Shader

namespace BbShaderCapture {

/// Whether compilations are recorded (BB_SHADER_CAPTURE set).
bool Enabled();

/// Starts recording the guest memory the compiler reads on this thread.
void Begin();

/// Writes the record of a compilation (`in_info`: the info as it was before compiling).
void Write(const Shader::Info& in_info, const Shader::RuntimeInfo& runtime_info,
           const Shader::Profile& profile, const Shader::Backend::Bindings& in_bindings,
           std::span<const u32> code, size_t perm_idx, std::span<const u32> spv,
           const Shader::Info& out_info, const Shader::Backend::Bindings& out_bindings);

/// The pipeline cache's own record of a compilation's inputs (BBSI): what translating the
/// permutation again needs when a later build changes the compiler's output (shader_rebuild.h).
/// Its layout changes only with InputVersion or the stored structures' sizes.
constexpr u32 InputVersion = 1;
/// The sizes of the structures a record stores as bytes (ReadInput rejects another).
u32 InputLayoutId();

struct Input {
    struct Region {
        u64 address;
        std::vector<u8> bytes;
    };
    u64 params_address = 0, positions_address = 0;
    Shader::HwStage hw_stage{};
    Shader::SwStage sw_stage{};
    u64 pgm_hash = 0, perm_idx = 0, pgm_base = 0;
    std::array<u32, Shader::ShaderParams::NumShaderUserData> user_data{};
    std::vector<u32> code, vs_copy, flat;
    Shader::RuntimeInfo runtime_info{};
    Shader::Backend::Bindings bindings{};
    std::vector<Region> regions;
};

/// Starts recording the guest memory the compiler reads on this thread (for TakeInput).
void BeginInput();
/// The input record of the compilation since BeginInput (`flat`: the info's flat user data).
std::vector<u8> TakeInput(const Shader::Info& in_info, const Shader::RuntimeInfo& runtime_info,
                          const Shader::Backend::Bindings& in_bindings, std::span<const u32> code,
                          size_t perm_idx, std::span<const u32> flat);
/// Parses an input record; false for another version or layout, or damaged bytes.
/// `without_code`: a record whose program code was left out (shader recipes) is valid too.
bool ReadInput(std::span<const u8> bytes, Input& input, bool without_code = false);
/// The record of `input` (ReadInput's layout).
std::vector<u8> WriteInput(const Input& input);

} // namespace BbShaderCapture
