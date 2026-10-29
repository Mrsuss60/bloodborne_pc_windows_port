// SPDX-FileCopyrightText: Copyright 2026 bbport contributors
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <miniz.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <csetjmp>
#include <dlfcn.h>
#include <ucontext.h>
#include <csignal>
#include <unistd.h>
#endif

#include "common/hash.h"
#include "common/path_util.h"
#include "common/serdes.h"
#include "video_core/renderer_vulkan/vk_pipeline_cache.h"
#include "video_core/renderer_vulkan/vk_pipeline_serialization.h"
#include "video_core/renderer_vulkan/vk_shader_capture.h"
#include "shader_recompiler/backend/spirv/emit_spirv.h"
#include "shader_recompiler/info.h"
#include "shader_recompiler/profile.h"
#include "shader_recompiler/recompiler.h"
#include "shader_recompiler/specialization.h"

namespace BbShaderRebuild {

namespace fs = std::filesystem;

constexpr size_t HeaderWords = 4;

std::vector<u8> ReadFile(const fs::path& path);
std::string HexName(u64 value);

static inline const void* FindBytes(const void* haystack, size_t haystacklen, const void* needle, size_t needlelen) {
    if (!haystack || !needle || needlelen == 0 || haystacklen < needlelen) return nullptr;
    const u8* h = static_cast<const u8*>(haystack);
    const u8* n = static_cast<const u8*>(needle);
    for (size_t i = 0; i <= haystacklen - needlelen; ++i) {
        if (h[i] == n[0] && std::memcmp(h + i, n, needlelen) == 0) {
            return h + i;
        }
    }
    return nullptr;
}

/// The guest memory one record read, mapped back at its addresses for the translation.
class GuestMemory {
public:
    bool Map(const std::vector<BbShaderCapture::Input::Region>& regions) {
        std::map<u64, u64> pages;
        for (const auto& region : regions) {
            const u64 first = region.address & ~(Granule - 1);
            const u64 end = (region.address + region.bytes.size() + Granule - 1) & ~(Granule - 1);
            pages[first] = std::max(pages[first], end);
        }
        std::vector<std::pair<u64, u64>> spans;
        for (const auto& [first, end] : pages) {
            if (!spans.empty() && first <= spans.back().second) {
                spans.back().second = std::max(spans.back().second, end);
            } else {
                spans.emplace_back(first, end);
            }
        }
        for (const auto& [first, end] : spans) {
#ifdef _WIN32
            void* at = VirtualAlloc(reinterpret_cast<void*>(first), end - first,
                                    MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
            if (at != reinterpret_cast<void*>(first)) {
                if (at) {
                    VirtualFree(at, 0, MEM_RELEASE);
                }
                return false;
            }
#else
            void* at = mmap(reinterpret_cast<void*>(first), end - first, PROT_READ | PROT_WRITE,
                            MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
            if (at != reinterpret_cast<void*>(first)) {
                if (at != MAP_FAILED) {
                    munmap(at, end - first);
                }
                return false;
            }
#endif
            mapped.emplace_back(first, end - first);
        }
        for (const auto& region : regions) {
            std::memcpy(reinterpret_cast<void*>(region.address), region.bytes.data(),
                        region.bytes.size());
        }
        return true;
    }
    ~GuestMemory() {
        for (const auto& [address, size] : mapped) {
#ifdef _WIN32
            VirtualFree(reinterpret_cast<void*>(address), 0, MEM_RELEASE);
#else
            munmap(reinterpret_cast<void*>(address), size);
#endif
        }
    }

private:
#ifdef _WIN32
    static constexpr u64 Granule = 0x10000;
#else
    static constexpr u64 Granule = 0x1000;
#endif
    std::vector<std::pair<u64, u64>> mapped;
};

// ---- Shader recipes (shader_rebuild.h): the cache's compilations without the game's code ----

class GameShaders {
public:
    void Load(const fs::path& game_dir, const std::unordered_set<u64>* wanted) {
        std::error_code ec;
        for (const auto& entry : fs::directory_iterator{game_dir / "dvdroot_ps4" / "shader", ec}) {
            if (entry.path().extension() == ".dcx") {
                source = entry.path().filename().string();
                Scan(Inflate(ReadFile(entry.path())), wanted);
            }
        }
        source = "eboot.bin";
        Scan(ReadFile(game_dir / "eboot.bin"), wanted);
    }
    const std::vector<u32>* Code(u64 hash) const {
        const auto it = programs.find(hash);
        return it != programs.end() ? &it->second : nullptr;
    }
    std::optional<u64> Find(std::span<const u8> bytes) const {
        if (bytes.size() < 8) {
            return std::nullopt;
        }
        u64 head;
        std::memcpy(&head, bytes.data(), sizeof(head));
        const auto [first, last] = by_head.equal_range(head);
        for (auto it = first; it != last; ++it) {
            const auto& code = programs.at(it->second);
            if (bytes.size() <= code.size() * sizeof(u32) &&
                std::memcmp(code.data(), bytes.data(), bytes.size()) == 0) {
                return it->second;
            }
        }
        return std::nullopt;
    }
    [[nodiscard]] size_t Count() const noexcept {
        return programs.size();
    }

private:
    static std::vector<u8> Inflate(const std::vector<u8>& dcx) {
        const auto find = [&](const char* tag) -> size_t {
            const void* at = FindBytes(dcx.data(), std::min<size_t>(dcx.size(), 0x100), tag, 4);
            return at ? size_t(static_cast<const u8*>(at) - dcx.data()) : dcx.size();
        };
        const size_t dcs = find("DCS\0"), dca = find("DCA\0");
        if (dcx.size() < 0x4c || std::memcmp(dcx.data(), "DCX\0", 4) != 0 || dcs + 8 > dcx.size() ||
            dca + 8 > dcx.size()) {
            return {};
        }
        const u32 size = u32(dcx[dcs + 4]) << 24 | u32(dcx[dcs + 5]) << 16 |
                         u32(dcx[dcs + 6]) << 8 | u32(dcx[dcs + 7]);
        std::vector<u8> out(size);
        mz_ulong out_size = size;
        if (mz_uncompress(out.data(), &out_size, dcx.data() + dca + 8, dcx.size() - dca - 8) !=
            MZ_OK) {
            return {};
        }
        out.resize(out_size);
        return out;
    }
    void Scan(const std::vector<u8>& data, const std::unordered_set<u64>* wanted) {
        constexpr size_t BlockSize = 28; // signature, version, flags/length, hash, crc32
        for (size_t at = 0; at + BlockSize <= data.size();) {
            const void* found = FindBytes(data.data() + at, data.size() - at, "OrbShdr", 7);
            if (!found) {
                break;
            }
            const size_t pos = size_t(static_cast<const u8*>(found) - data.data());
            at = pos + 1;
            if (pos + BlockSize > data.size()) {
                break;
            }
            u32 bits;
            u64 hash;
            std::memcpy(&bits, data.data() + pos + 8, sizeof(bits));
            std::memcpy(&hash, data.data() + pos + 16, sizeof(hash));
            const size_t length = bits >> 8;
            if (length < 8 || length % 4 || length > pos || programs.contains(hash) ||
                (wanted && !wanted->contains(hash))) {
                continue;
            }
            const u8* start = nullptr;
            for (size_t offset = length; offset <= std::min(pos, length + 1024); offset += 4) {
                const u8* at_ptr = data.data() + pos - offset;
                u32 words[2];
                std::memcpy(words, at_ptr, sizeof(words));
                if (words[0] == 0xBEEB03FFu && size_t(words[1] + 1) * 8 == offset) {
                    start = at_ptr;
                    break;
                }
            }
            if (!start) {
                continue;
            }
            std::vector<u32> code(length / sizeof(u32));
            std::memcpy(code.data(), start, length);
            u64 head;
            std::memcpy(&head, start, sizeof(head));
            by_head.emplace(head, hash);
            locations.emplace(hash, std::pair{source, pos});
            programs.emplace(hash, std::move(code));
        }
    }

    std::unordered_map<u64, std::vector<u32>> programs;
    std::unordered_multimap<u64, u64> by_head;

public:
    std::string source;
    std::unordered_map<u64, std::pair<std::string, size_t>> locations;
};

struct RecipePerm {
    std::vector<u8> input;
    std::vector<u8> meta;
    struct Ref {
        u64 address;
        u64 hash;
        u32 size;
    };
    std::vector<Ref> refs;
};

struct RecipeProgram {
    u64 hash = 0;
    std::vector<RecipePerm> perms;
};

struct RecipeKey {
    std::string name;
    std::vector<u8> bytes;
};

bool ReadPack(const fs::path& path, std::vector<RecipeProgram>& programs,
              std::vector<RecipeKey>& keys);
size_t WritePack(const std::vector<RecipeProgram>& programs, const std::vector<RecipeKey>& keys,
                 const fs::path& path);

} // namespace BbShaderRebuild
