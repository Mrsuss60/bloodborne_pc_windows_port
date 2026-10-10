// SPDX-License-Identifier: GPL-2.0-or-later
// Uses the production descriptor reader; no game files, Vulkan device or window needed.
#include <array>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include "shader_recompiler/frontend/fetch_shader.h"

int main() {
    constexpr u32 pointer_reg = 2;
    constexpr u32 descriptor_offset = 4;
    std::array<u32, Shader::NUM_USER_DATA_REGS> registers{};
    std::array<u32, 8> table{};
    Shader::Info info{};
    info.user_data = registers;

    AmdGpu::Buffer expected{};
    expected.base_address = 0x123400;
    expected.stride = 16;
    expected.num_records = 12;
    std::memcpy(table.data() + descriptor_offset, &expected, sizeof(expected));

    const auto set_pointer = [&](u64 pointer) {
        std::memcpy(registers.data() + pointer_reg, &pointer, sizeof(pointer));
    };
    const auto read = [&] {
        return info.ReadUdReg<AmdGpu::Buffer>(pointer_reg, descriptor_offset);
    };

    // Regression: null table plus a nonzero descriptor offset must not read low memory.
    assert(read() == AmdGpu::Buffer{});
    assert(!read());
    // Pointer metadata is masked before deciding whether the table is null.
    set_pointer(0xabcd000000000000ULL);
    assert(read() == AmdGpu::Buffer{});

    // The vertex consumer must still see an empty descriptor with load overrides applied.
    Shader::Gcn::VertexAttribute attribute{};
    attribute.sgpr_base = pointer_reg;
    attribute.dword_offset = descriptor_offset;
    attribute.inst_offset = 8;
    attribute.data_format = static_cast<u8>(AmdGpu::DataFormat::Format32);
    assert(!attribute.GetSharp(info));

    // Valid tables, masked pointer metadata and instruction offsets retain their behavior.
    set_pointer(reinterpret_cast<u64>(table.data()) | 0xabcd000000000000ULL);
    assert(read() == expected);
    const auto vertex = attribute.GetSharp(info);
    assert(vertex.num_records == expected.num_records);
    assert(vertex.base_address == expected.base_address + attribute.inst_offset);
    assert(vertex.data_format == attribute.data_format);

    // The direct-register path bypasses the pointer indirection.
    std::memcpy(registers.data() + descriptor_offset, &expected, sizeof(expected));
    assert(info.ReadUdReg<AmdGpu::Buffer>(Shader::IR::NumScalarRegs, descriptor_offset) == expected);

    // Draw snapshots take precedence over current user data, including a null table.
    std::array<u32, Shader::NUM_USER_DATA_REGS> snapshot_registers{};
    Shader::Info::ud_snapshots[0] = {
        &info, snapshot_registers.data(), static_cast<u32>(snapshot_registers.size()),
        nullptr, 0, 0,
    };
    Shader::Info::num_ud_snapshots = 1;
    assert(read() == AmdGpu::Buffer{});
    assert(!attribute.GetSharp(info));
    const auto snapshot_pointer = reinterpret_cast<u64>(table.data());
    std::memcpy(snapshot_registers.data() + pointer_reg, &snapshot_pointer, sizeof(snapshot_pointer));
    assert(read() == expected);
    Shader::Info::num_ud_snapshots = 0;

    // Multisampled storage images (Metal cannot write texture2d_ms): only stores or atomics on a
    // multisampled view count; reads and single-sample stores do not.
    const auto image_resource = [](AmdGpu::ImageType type, bool written, bool array) {
        AmdGpu::Image sharp = AmdGpu::Image::Null(false);
        sharp.type = static_cast<u64>(type);
        Shader::ImageResource resource{};
        std::memcpy(resource.sharp_fetch.immediates.data(), &sharp, sizeof(sharp));
        resource.sharp_fetch.offsets.fill(0); // known locations; load_mask 0: immediates
        resource.sharp_fetch.load_mask = 0;
        resource.is_written = written;
        resource.is_array = array;
        return resource;
    };
    const auto writes_msaa = [&](std::initializer_list<Shader::ImageResource> images) {
        Shader::Info image_info{};
        for (const auto& image : images) {
            image_info.images.push_back(image);
        }
        return image_info.WritesMultisampledImage();
    };
    using AmdGpu::ImageType;
    assert(writes_msaa({image_resource(ImageType::Color2DMsaa, true, false)}));
    assert(writes_msaa({image_resource(ImageType::Color2DMsaaArray, true, true)}));
    assert(writes_msaa({image_resource(ImageType::Color2DMsaaArray, true, false)})); // 2DMsaa view
    assert(!writes_msaa({image_resource(ImageType::Color2DMsaa, false, false)}));
    assert(!writes_msaa({image_resource(ImageType::Color2D, true, false)}));
    assert(!writes_msaa({}));
    assert(writes_msaa({image_resource(ImageType::Color2D, true, false),
                        image_resource(ImageType::Color2DMsaa, false, false),
                        image_resource(ImageType::Color2DMsaa, true, false)}));

    std::puts("Shader user data: null tables, vertex consumers, direct registers and snapshots passed");
    std::puts("Shader info: multisampled storage images detected (stores only) passed");
}
