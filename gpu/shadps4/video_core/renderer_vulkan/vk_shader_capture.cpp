// SPDX-FileCopyrightText: Copyright 2026 bbport contributors
// SPDX-License-Identifier: GPL-2.0-or-later
#include "video_core/renderer_vulkan/vk_shader_capture.h"
#include "shim/bb_env.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "common/serdes.h"
#include "shader_recompiler/backend/bindings.h"
#include "shader_recompiler/frontend/decode.h"
#include "shader_recompiler/frontend/opcodes.h"
#include "shader_recompiler/info.h"
#include "shader_recompiler/profile.h"
#include "shader_recompiler/runtime_info.h"

namespace BbShaderCapture {

namespace {

/// Guest memory read while compiling: a size of 0 is a program (its length found by decoding).
struct Read {
    const u8* address;
    size_t size;
};
thread_local std::vector<Read> reads;

void RecordRead(const void* address, size_t size) {
    reads.push_back({static_cast<const u8*>(address), size});
}

const char* Directory() {
    static const char* dir = bb_env("BB_SHADER_CAPTURE");
    return dir && *dir ? dir : nullptr;
}

/// Bytes of a program at `code` up to and including its s_setpc_b64 (fetch shaders return so).
size_t ProgramBytes(const u8* code) {
    const auto* start = reinterpret_cast<const u32*>(code);
    Shader::Gcn::GcnCodeSlice code_slice(start, start + 1024);
    Shader::Gcn::GcnDecodeContext decoder;
    while (!code_slice.atEnd()) {
        const auto inst = decoder.decodeInstruction(code_slice);
        if (inst.opcode == Shader::Gcn::Opcode::S_SETPC_B64) {
            break;
        }
    }
    return size_t(code_slice.position() - start) * sizeof(u32);
}

struct Out {
    std::vector<u8> bytes;
    void Raw(const void* data, size_t size) {
        const auto* p = static_cast<const u8*>(data);
        bytes.insert(bytes.end(), p, p + size);
    }
    template <typename T>
    void Value(const T& value) {
        Raw(&value, sizeof(T));
    }
    void Words(std::span<const u32> words) {
        Value(u32(words.size()));
        Raw(words.data(), words.size_bytes());
    }
};

} // namespace

bool Enabled() {
    return Directory() != nullptr;
}

void Begin() {
    reads.clear();
    Shader::guest_read_hook = RecordRead;
}

namespace {

/// The guest memory read since Begin(), merged into ranges.
std::vector<std::pair<const u8*, size_t>> MergedReads() {
    std::vector<std::pair<const u8*, size_t>> ranges;
    for (const auto& read : reads) {
        ranges.emplace_back(read.address, read.size ? read.size : ProgramBytes(read.address));
    }
    std::ranges::sort(ranges);
    std::vector<std::pair<const u8*, size_t>> merged;
    for (const auto& [address, size] : ranges) {
        if (!merged.empty() && address <= merged.back().first + merged.back().second) {
            auto& last = merged.back();
            last.second = std::max<size_t>(last.second, size_t(address + size - last.first));
        } else {
            merged.emplace_back(address, size);
        }
    }
    return merged;
}

/// Layout of the structures an input record stores as bytes: another layout cannot be read.
u32 InputLayout() {
    return u32(sizeof(Shader::RuntimeInfo) * 31 + sizeof(Shader::Backend::Bindings) * 7 +
               Shader::ShaderParams::NumShaderUserData);
}

struct In {
    std::span<const u8> bytes;
    size_t at = 0;
    bool ok = true;
    void Raw(void* out, size_t size) {
        if (at + size > bytes.size()) {
            ok = false;
            return;
        }
        std::memcpy(out, bytes.data() + at, size);
        at += size;
    }
    template <typename T>
    T Value() {
        T value{};
        Raw(&value, sizeof(T));
        return value;
    }
    std::vector<u32> Words() {
        const u32 count = Value<u32>();
        if (!ok || size_t(count) * 4 > bytes.size() - at) {
            ok = false;
            return {};
        }
        std::vector<u32> words(count);
        Raw(words.data(), words.size() * sizeof(u32));
        return words;
    }
};

} // namespace

u32 InputLayoutId() {
    return InputLayout();
}

void BeginInput() {
    reads.clear();
    Shader::guest_read_hook = RecordRead;
}

std::vector<u8> TakeInput(const Shader::Info& in_info, const Shader::RuntimeInfo& runtime_info,
                          const Shader::Backend::Bindings& in_bindings, std::span<const u32> code,
                          size_t perm_idx, std::span<const u32> flat) {
    Shader::guest_read_hook = nullptr;
    Out out;
    out.Raw("BBSI", 4);
    out.Value(InputVersion);
    out.Value(InputLayout());
    out.Value(Shader::MotionVectors::params_address);
    out.Value(Shader::MotionVectors::positions_address);
    out.Value(u32(in_info.hw_stage));
    out.Value(u32(in_info.sw_stage));
    out.Value(in_info.pgm_hash);
    out.Value(u64(perm_idx));
    out.Value(u64(in_info.pgm_base));
    std::array<u32, Shader::ShaderParams::NumShaderUserData> user_data{};
    std::copy_n(in_info.user_data.begin(),
                std::min(in_info.user_data.size(), user_data.size()), user_data.begin());
    out.Value(user_data);
    out.Words(code);
    auto stored_rt = runtime_info;
    std::span<const u32> vs_copy{};
    if (runtime_info.hw_stage == Shader::HwStage::Geometry) {
        vs_copy = runtime_info.hw.gs.vs_copy;
        stored_rt.hw.gs.vs_copy = {};
    }
    out.Value(stored_rt);
    out.Words(vs_copy);
    out.Value(in_bindings);
    out.Words(flat);
    const auto merged = MergedReads();
    out.Value(u32(merged.size()));
    for (const auto& [address, size] : merged) {
        out.Value(u64(reinterpret_cast<uintptr_t>(address)));
        out.Value(u32(size));
        out.Raw(address, size);
    }
    return std::move(out.bytes);
}

bool ReadInput(std::span<const u8> bytes, Input& input, bool without_code) {
    In in{bytes};
    char magic[4];
    in.Raw(magic, 4);
    if (!in.ok || std::memcmp(magic, "BBSI", 4) != 0 || in.Value<u32>() != InputVersion ||
        in.Value<u32>() != InputLayout()) {
        return false;
    }
    input.params_address = in.Value<u64>();
    input.positions_address = in.Value<u64>();
    input.hw_stage = Shader::HwStage(in.Value<u32>());
    input.sw_stage = Shader::SwStage(in.Value<u32>());
    input.pgm_hash = in.Value<u64>();
    input.perm_idx = in.Value<u64>();
    input.pgm_base = in.Value<u64>();
    input.user_data = in.Value<std::array<u32, Shader::ShaderParams::NumShaderUserData>>();
    input.code = in.Words();
    input.runtime_info = in.Value<Shader::RuntimeInfo>();
    input.vs_copy = in.Words();
    input.bindings = in.Value<Shader::Backend::Bindings>();
    input.flat = in.Words();
    const u32 regions = in.Value<u32>();
    input.regions.clear();
    for (u32 i = 0; i < regions && in.ok; ++i) {
        auto& region = input.regions.emplace_back();
        region.address = in.Value<u64>();
        const u32 size = in.Value<u32>();
        if (!in.ok || size > bytes.size() - in.at) {
            return false;
        }
        region.bytes.resize(size);
        in.Raw(region.bytes.data(), size);
    }
    return in.ok && in.at == bytes.size() && (without_code || !input.code.empty());
}

std::vector<u8> WriteInput(const Input& input) {
    Out out;
    out.Raw("BBSI", 4);
    out.Value(InputVersion);
    out.Value(InputLayout());
    out.Value(input.params_address);
    out.Value(input.positions_address);
    out.Value(u32(input.hw_stage));
    out.Value(u32(input.sw_stage));
    out.Value(input.pgm_hash);
    out.Value(input.perm_idx);
    out.Value(input.pgm_base);
    out.Value(input.user_data);
    out.Words(input.code);
    out.Value(input.runtime_info);
    out.Words(input.vs_copy);
    out.Value(input.bindings);
    out.Words(input.flat);
    out.Value(u32(input.regions.size()));
    for (const auto& region : input.regions) {
        out.Value(region.address);
        out.Value(u32(region.bytes.size()));
        out.Raw(region.bytes.data(), region.bytes.size());
    }
    return std::move(out.bytes);
}

void Write(const Shader::Info& in_info, const Shader::RuntimeInfo& runtime_info,
           const Shader::Profile& profile, const Shader::Backend::Bindings& in_bindings,
           std::span<const u32> code, size_t perm_idx, std::span<const u32> spv,
           const Shader::Info& out_info, const Shader::Backend::Bindings& out_bindings) {
    Shader::guest_read_hook = nullptr;
    Out out;
    out.Raw("BBSC", 4);
    out.Value(u32{2}); // format version
    // Globals the backend reads (object motion vectors).
    out.Value(Shader::MotionVectors::params_address);
    out.Value(Shader::MotionVectors::positions_address);
    out.Value(u32(in_info.hw_stage));
    out.Value(u32(in_info.sw_stage));
    out.Value(in_info.pgm_hash);
    out.Value(u64(perm_idx));
    out.Value(u64(in_info.pgm_base));
    std::array<u32, Shader::ShaderParams::NumShaderUserData> user_data{};
    std::copy_n(in_info.user_data.begin(),
                std::min(in_info.user_data.size(), user_data.size()), user_data.begin());
    out.Value(user_data);
    out.Words(code);
    out.Value(runtime_info);
    out.Words(runtime_info.hw_stage == Shader::HwStage::Geometry ? runtime_info.hw.gs.vs_copy
                                                                 : std::span<const u32>{});
    out.Value(profile);
    out.Value(in_bindings);

    // The memory read, merged into ranges.
    std::vector<std::pair<const u8*, size_t>> ranges;
    for (const auto& read : reads) {
        ranges.emplace_back(read.address, read.size ? read.size : ProgramBytes(read.address));
    }
    std::ranges::sort(ranges);
    std::vector<std::pair<const u8*, size_t>> merged;
    for (const auto& [address, size] : ranges) {
        if (!merged.empty() && address <= merged.back().first + merged.back().second) {
            auto& last = merged.back();
            last.second = std::max<size_t>(last.second, size_t(address + size - last.first));
        } else {
            merged.emplace_back(address, size);
        }
    }
    out.Value(u32(merged.size()));
    for (const auto& [address, size] : merged) {
        out.Value(u64(reinterpret_cast<uintptr_t>(address)));
        out.Value(u32(size));
        out.Raw(address, size);
    }

    out.Words(spv);
    Serialization::Archive archive;
    out_info.Serialize(archive);
    const auto info_bytes = archive.TakeOff();
    out.Value(u64(info_bytes.size()));
    out.Raw(info_bytes.data(), info_bytes.size());
    out.Value(out_bindings);

    static std::atomic<u32> sequence{0};
    char name[512];
    std::snprintf(name, sizeof(name), "%s/%016llx_%u_%zu_%u.bbsc", Directory(),
                  (unsigned long long)in_info.pgm_hash, u32(in_info.hw_stage), perm_idx,
                  sequence.fetch_add(1));
    if (FILE* file = std::fopen(name, "wb")) {
        std::fwrite(out.bytes.data(), 1, out.bytes.size(), file);
        std::fclose(file);
    }
}

} // namespace BbShaderCapture
