// SPDX-FileCopyrightText: Copyright 2026 bbport contributors
// SPDX-License-Identifier: GPL-2.0-or-later
// bbport layer: the PS4 GPU's view of memory on a PC GPU, without sparse resources.
//
// Ported from upstream bloodborne_pc 0.7 (gpu/layer/bblayer_gpu_memory.cpp). The Windows fork
// reaches the guest's memory through dense device-local mirror buffers instead of Vulkan sparse
// resources; this module keeps the registry of those mirrors and which 16 KiB blocks they hold.
#include "bblayer_gpu_memory.h"
#include "shim/bb_env.h"

#include <algorithm>
#include <iterator>
#include <atomic>
#include <cstdio>
#include <cstdlib>

namespace BbLayer {

GpuMemory& GpuMemory::Get() {
    static GpuMemory instance;
    return instance;
}

void GpuMemory::SetTranslate(GuestTranslate translate_) {
    translate = translate_;
}

void GpuMemory::SetBlockShift(std::uint32_t shift) {
    block_shift = shift;
}

void GpuMemory::SetPopulate(GuestPopulate populate_) {
    populate = populate_;
}

void GpuMemory::SetHoleEnd(GuestHoleEnd hole_end_) {
    hole_end = hole_end_;
}

std::uint64_t GpuMemory::UnbackedEnd(std::uint64_t va) const {
    std::uint64_t phys = 0, map_end = 0;
    if (!hole_end || !translate || !translate(va, &phys, &map_end) || map_end <= va) {
        return 0;
    }
    const std::uint64_t end = hole_end(phys);
    return end > phys ? std::min(map_end, va + (end - phys)) : 0;
}

void GpuMemory::AddGuestSource(std::uint64_t phys, std::uint64_t size, VkBuffer buffer,
                               VkDeviceAddress device_address, void* owner) {
    std::scoped_lock lk{write_mutex};
    if (size > longest_source.load(std::memory_order_relaxed)) {
        longest_source.store(size, std::memory_order_release);
    }
    const auto* old = guest_sources.load(std::memory_order_acquire);
    auto* list = old ? new std::vector<MemorySource*>(*old) : new std::vector<MemorySource*>();
    auto* source = new MemorySource{MemorySource::Kind::Guest, phys, size, buffer, device_address,
                                    owner};
    // BB_BDA_LOG=1 (diagnostics): GPU addresses of the sources, to place a GPUVM fault.
    static const bool bda_log = bb_env("BB_BDA_LOG") != nullptr;
    if (bda_log) {
        std::printf("BDA: guest source phys %#llx+%#llx at GPU %#llx-%#llx\n",
                    (unsigned long long)phys, (unsigned long long)size,
                    (unsigned long long)device_address,
                    (unsigned long long)(device_address + size));
    }
    list->insert(std::ranges::upper_bound(*list, phys, {}, &MemorySource::base), source);
    guest_sources.store(list, std::memory_order_release);
}

void GpuMemory::RemoveGuestSource(void* owner) {
    std::scoped_lock lk{write_mutex};
    const auto* old = guest_sources.load(std::memory_order_acquire);
    if (!old) {
        return;
    }
    auto* list = new std::vector<MemorySource*>();
    for (MemorySource* source : *old) {
        if (source->owner != owner) {
            list->push_back(source);
        }
    }
    guest_sources.store(list, std::memory_order_release);
}

std::uint64_t GpuMemory::PhysOfDevice(std::uint64_t device_address) const {
    const auto* list = guest_sources.load(std::memory_order_acquire);
    if (list) {
        for (const MemorySource* source : *list) {
            if (device_address - source->device_address < source->size) {
                return source->base + (device_address - source->device_address);
            }
        }
    }
    return ~std::uint64_t{0};
}

const MemorySource* GpuMemory::FindSource(std::uint64_t phys, std::uint64_t want) const {
    const auto* list = guest_sources.load(std::memory_order_acquire);
    if (!list || list->empty()) {
        return nullptr;
    }
    // Sources starting at or below phys, back as far as the longest one reaches: one holding
    // [phys, phys + want) whole, else the one reaching furthest.
    const std::uint64_t reach = longest_source.load(std::memory_order_acquire);
    const MemorySource* best = nullptr;
    for (auto it = std::ranges::upper_bound(*list, phys, {}, &MemorySource::base);
         it != list->begin();) {
        const MemorySource* source = *--it;
        if (source->base + reach <= phys) {
            break;
        }
        if (phys - source->base < source->size &&
            (!best || source->base + source->size > best->base + best->size)) {
            best = source;
            if (best->base + best->size >= phys + want) {
                break;
            }
        }
    }
    return best;
}

const MemorySource* GpuMemory::GuestSource(std::uint64_t phys, std::uint64_t want) const {
    // Whole only below the paged size: larger ranges are cut or paged at source ends anyway.
    const std::uint64_t need = want < WholeLimit ? std::max<std::uint64_t>(want, 1) : 1;
    const MemorySource* source = FindSource(phys, need);
    if (populate && (!source || source->base + source->size < phys + need) &&
        populate(phys, need)) {
        source = FindSource(phys, need);
    }
    return source;
}

namespace {
/// BB_ZERO_SCAN: in-place resolutions that would cross a 16 / 64 MiB granule of physical memory.
std::atomic<std::uint64_t> resolved_calls{0}, straddle16{0}, straddle64{0};
std::atomic<std::uint64_t> resolved_bytes{0}, straddle16_bytes{0}, straddle64_bytes{0};
void NoteStraddle(std::uint64_t phys, std::uint64_t size) {
    static const bool on = bb_env("BB_ZERO_SCAN") != nullptr;
    if (!on || size == 0) {
        return;
    }
    resolved_calls.fetch_add(1, std::memory_order_relaxed);
    resolved_bytes.fetch_add(size, std::memory_order_relaxed);
    if ((phys >> 24) != ((phys + size - 1) >> 24)) {
        straddle16.fetch_add(1, std::memory_order_relaxed);
        straddle16_bytes.fetch_add(size, std::memory_order_relaxed);
    }
    if ((phys >> 26) != ((phys + size - 1) >> 26)) {
        straddle64.fetch_add(1, std::memory_order_relaxed);
        straddle64_bytes.fetch_add(size, std::memory_order_relaxed);
    }
}
} // namespace

extern "C" void bbgpu_straddle_report() {
    std::printf("Zero scan: in-place resolutions %llu (%llu MiB); crossing 16 MiB: %llu (%llu MiB), "
                "64 MiB: %llu (%llu MiB)\n",
                (unsigned long long)resolved_calls.load(), (unsigned long long)(resolved_bytes.load() >> 20),
                (unsigned long long)straddle16.load(), (unsigned long long)(straddle16_bytes.load() >> 20),
                (unsigned long long)straddle64.load(), (unsigned long long)(straddle64_bytes.load() >> 20));
}

std::optional<Span> GpuMemory::ResolveInPlace(std::uint64_t va, std::uint64_t size) const {
    const auto prefix = ResolvePrefix(va, size);
    if (!prefix || prefix->size != size) {
        return std::nullopt;
    }
    NoteStraddle(prefix->source->base + prefix->offset, size);
    return prefix;
}


namespace {
/// BB_ZERO_SCAN (diagnostics): 16 MiB granules of guest addresses the GPU resolved, from the
/// first direct memory address (the runtime's zero scan asks, bbgpu_gpu_touched).
constexpr std::uint64_t TouchBase = 0x1000000000ull, TouchShift = 24, TouchCount = 4096;
std::atomic<std::uint64_t> touched[TouchCount / 64];
void NoteTouch(std::uint64_t va, std::uint64_t size) {
    static const bool on = bb_env("BB_ZERO_SCAN") != nullptr;
    if (!on || size == 0 || va < TouchBase) {
        return;
    }
    for (std::uint64_t g = (va - TouchBase) >> TouchShift;
         g <= (va + size - 1 - TouchBase) >> TouchShift && g < TouchCount; ++g) {
        touched[g / 64].fetch_or(1ull << (g % 64), std::memory_order_relaxed);
    }
}
} // namespace

void NoteGpuTouch(std::uint64_t va, std::uint64_t size) {
    NoteTouch(va, size);
}

extern "C" int bbgpu_gpu_touched(std::uint64_t va) {
    if (va < TouchBase) {
        return 0;
    }
    const std::uint64_t g = (va - TouchBase) >> TouchShift;
    return g < TouchCount && (touched[g / 64].load(std::memory_order_relaxed) >> (g % 64)) & 1;
}

std::optional<Span> GpuMemory::ResolvePrefix(std::uint64_t va, std::uint64_t size) const {

    if (!translate || size == 0) {
        return std::nullopt;
    }
    std::uint64_t phys = 0, map_end = 0;
    if (!translate(va, &phys, &map_end) || map_end <= va) {
        return std::nullopt;
    }
    // Lazy guest memory: a range a buffer binds whole (below the paged size) gets a source that
    // holds all of its physically contiguous run; larger ones (paged, cut) its start.
    std::uint64_t want = 1;
    if (populate && size < WholeLimit) {
        std::uint64_t run = map_end - va;
        while (run < size) {
            std::uint64_t next_phys = 0, next_end = 0;
            if (!translate(va + run, &next_phys, &next_end) || next_phys != phys + run ||
                next_end <= va + run) {
                break;
            }
            run = next_end - va;
        }
        want = std::min(size, run);
    }
    const MemorySource* source = GuestSource(phys, want);
    if (!source) {
        return std::nullopt;
    }
    const std::uint64_t limit = std::min(size, source->base + source->size - phys);
    // The mapping may be split (protection changes): the next pieces must continue it in
    // physical memory.
    std::uint64_t covered = map_end - va;
    while (covered < limit) {
        const std::uint64_t at = va + covered;
        std::uint64_t next_phys = 0, next_end = 0;
        if (!translate(at, &next_phys, &next_end) || next_phys != phys + covered ||
            next_end <= at) {
            break;
        }
        covered = next_end - va;
    }
    NoteTouch(va, std::min(covered, limit));
    return Span{source, phys - source->base, va, std::min(covered, limit)};
}

Resolution GpuMemory::Resolve(std::uint64_t va, std::uint64_t size) const {
    if (size == 0) {
        return {};
    }
    const std::uint64_t first = va >> block_shift;
    const std::uint64_t end = ((va + size - 1) >> block_shift) + 1;
    {
        std::shared_lock lk{mirror_mutex};
        if (AnyValidLocked(first, end)) {
            if (AllValidLocked(first, end)) {
                auto it = mirrors.upper_bound(va);
                if (it != mirrors.begin()) {
                    const MemorySource& mirror = (--it)->second;
                    if (va >= mirror.base && va + size <= mirror.base + mirror.size) {
                        return {Resolution::Kind::Mirror,
                                Span{&mirror, va - mirror.base, va, size}};
                    }
                }
            }
            return {Resolution::Kind::Mixed, {}};
        }
    }
    if (const auto span = ResolveInPlace(va, size)) {
        return {Resolution::Kind::InPlace, *span};
    }
    return {ResolvePrefix(va, size) ? Resolution::Kind::Mixed : Resolution::Kind::None, {}};
}

void GpuMemory::AddMirror(std::uint64_t start, std::uint64_t size, VkBuffer buffer,
                          VkDeviceAddress device_address, void* owner) {
    static const bool bda_log = bb_env("BB_BDA_LOG") != nullptr;
    if (bda_log) {
        std::printf("BDA: mirror %#llx+%#llx at GPU %#llx\n", (unsigned long long)start,
                    (unsigned long long)size, (unsigned long long)device_address);
    }
    std::unique_lock lk{mirror_mutex};
    // Mirrors never overlap (a VRAM copy of a block is in one of them): checked as they come.
    // BB_LAYER_CHECK=1: an overlap stops the game (before the GPU reads through the wrong one).
    static std::atomic<int> overlaps{0};
    auto next = mirrors.lower_bound(start);
    const bool over_next = next != mirrors.end() && next->first < start + size;
    const bool over_prev = next != mirrors.begin() &&
                           std::prev(next)->first + std::prev(next)->second.size > start;
    if ((over_next || over_prev) && overlaps.fetch_add(1) < 20) {
        const auto& other = over_next ? next->second : std::prev(next)->second;
        std::fprintf(stderr, "Layer check: mirror %#llx+%#llx overlaps mirror %#llx+%#llx\n",
                     (unsigned long long)start, (unsigned long long)size,
                     (unsigned long long)other.base, (unsigned long long)other.size);
        static const bool stop = bb_env_on("BB_LAYER_CHECK");
        if (stop) {
            std::fprintf(stderr, "STOP: overlapping mirrors (BB_LAYER_CHECK=1)\n");
            std::fflush(stderr);
            std::abort();
        }
    }
    mirrors[start] = MemorySource{MemorySource::Kind::Mirror, start, size, buffer, device_address,
                                  owner};
}

void GpuMemory::RemoveMirror(std::uint64_t start) {
    std::unique_lock lk{mirror_mutex};
    mirrors.erase(start);
}

std::optional<MemorySource> GpuMemory::MirrorAt(std::uint64_t va) const {
    std::shared_lock lk{mirror_mutex};
    auto it = mirrors.upper_bound(va);
    if (it == mirrors.begin()) {
        return std::nullopt;
    }
    const MemorySource& mirror = (--it)->second;
    if (va - mirror.base >= mirror.size) {
        return std::nullopt;
    }
    return mirror;
}

std::vector<MemorySource> GpuMemory::MirrorsIn(std::uint64_t start, std::uint64_t end) const {
    std::vector<MemorySource> out;
    std::shared_lock lk{mirror_mutex};
    auto it = mirrors.upper_bound(start);
    if (it != mirrors.begin()) {
        --it;
    }
    for (; it != mirrors.end() && it->first < end; ++it) {
        if (it->second.base + it->second.size > start) {
            out.push_back(it->second);
        }
    }
    return out;
}

void GpuMemory::MarkValid(std::uint64_t first, std::uint64_t end) {
    if (first >= end) {
        return;
    }
    std::unique_lock lk{mirror_mutex};
    valid_version.fetch_add(1, std::memory_order_release);
    // Merge with every run touching [first, end).
    auto it = valid.upper_bound(first);
    if (it != valid.begin() && std::prev(it)->second >= first) {
        --it;
    }
    std::uint64_t a = first, b = end;
    while (it != valid.end() && it->first <= b) {
        a = std::min(a, it->first);
        b = std::max(b, it->second);
        valid_blocks -= it->second - it->first;
        it = valid.erase(it);
    }
    valid[a] = b;
    valid_blocks += b - a;
}

void GpuMemory::MarkInvalid(std::uint64_t first, std::uint64_t end) {
    if (first >= end) {
        return;
    }
    std::unique_lock lk{mirror_mutex};
    valid_version.fetch_add(1, std::memory_order_release);
    auto it = valid.upper_bound(first);
    if (it != valid.begin()) {
        --it;
    }
    while (it != valid.end() && it->first < end) {
        const std::uint64_t a = it->first, b = it->second;
        if (b <= first) {
            ++it;
            continue;
        }
        it = valid.erase(it);
        valid_blocks -= b - a;
        if (a < first) {
            valid[a] = first;
            valid_blocks += first - a;
        }
        if (b > end) {
            it = valid.emplace(end, b).first;
            valid_blocks += b - end;
            ++it;
        }
    }
}

bool GpuMemory::AnyValidLocked(std::uint64_t first, std::uint64_t end) const {
    auto it = valid.upper_bound(first);
    if (it != valid.begin() && std::prev(it)->second > first) {
        return true;
    }
    return it != valid.end() && it->first < end;
}

bool GpuMemory::AllValidLocked(std::uint64_t first, std::uint64_t end) const {
    auto it = valid.upper_bound(first);
    if (it == valid.begin()) {
        return false;
    }
    --it;
    return it->first <= first && it->second >= end;
}

bool GpuMemory::AnyValid(std::uint64_t first, std::uint64_t end) const {
    std::shared_lock lk{mirror_mutex};
    return AnyValidLocked(first, end);
}

bool GpuMemory::AllValid(std::uint64_t first, std::uint64_t end) const {
    std::shared_lock lk{mirror_mutex};
    return AllValidLocked(first, end);
}

std::uint64_t GpuMemory::ValidBytes() const {
    std::shared_lock lk{mirror_mutex};
    return valid_blocks << block_shift;
}

} // namespace BbLayer
