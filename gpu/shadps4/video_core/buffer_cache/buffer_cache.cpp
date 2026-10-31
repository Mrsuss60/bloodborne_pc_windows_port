// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <chrono>
#include <array>
#include <map>
#include <unordered_map>
#include <algorithm>
#include <bit>
#include <cstdlib>
#include <magic_enum/magic_enum.hpp>
#include "bbport_copy.h"
#include "bbport_toggles.h"
#include "buffers/memory_model.h"
#include "common/alignment.h"
#include "core/memory.h"
#include "video_core/amdgpu/liverpool.h"
#include "video_core/buffer_cache/buffer.h"
#include "video_core/buffer_cache/buffer_cache.h"
#include "video_core/buffer_cache/memory_tracker.h"
#include "video_core/renderer_vulkan/vk_graphics_pipeline.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_runtime.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/texture_cache/texture_cache.h"

#include <vk_mem_alloc.h>

namespace VideoCore {

static constexpr size_t GDS_BUFFER_SIZE = 64_KB;
static constexpr size_t NULL_INDEX_BUFFER_SIZE = 16;
static constexpr size_t STREAM_BUFFER_SIZE = 128_MB;

BufferCache::BufferCache(const Vulkan::Instance& instance_, Vulkan::Scheduler& scheduler_,
                         Vulkan::Runtime& runtime_, AmdGpu::Liverpool* liverpool_,
                         TextureCache& texture_cache_, PageManager& tracker)
    : instance{instance_}, scheduler{scheduler_}, runtime{runtime_},
      staging_pool{runtime_.GetStagingPool()}, liverpool{liverpool_},
      memory{Core::Memory::Instance()}, texture_cache{texture_cache_},
      memory_tracker{std::make_unique<MemoryTracker>(tracker)},
      stream_buffer{instance, scheduler, MemoryType::Stream, STREAM_BUFFER_SIZE},
      gds_buffer{instance, 0, GDS_BUFFER_SIZE, MemoryType::Stream, "GDS Buffer"} {
    // bbport 0.7 memory model: the GPU reaches guest memory through dense device-local mirrors
    // (no sparse resources, no vkQueueBindSparse). Blocks are a fixed 16 KiB; the BDA page table
    // maps every block that has ever been populated to its mirror's device address.
    BbLayer::GpuMemory::Get().SetBlockShift(BLOCK_SHIFT);

    const u64 blocks_per_arena_page = ARENA_PAGE_SIZE / BLOCK_SIZE;
    const u64 total_blocks = blocks_per_arena_page * NUM_ARENA_PAGES;
    const u64 bda_pagetable_size = total_blocks * sizeof(vk::DeviceAddress);
    fault_manager = std::make_unique<FaultManager>(instance, scheduler, *this, BLOCK_SHIFT,
                                                   total_blocks);
    bda_pagetable_buffer = std::make_unique<Buffer>(
        instance, 0, bda_pagetable_size, MemoryType::DeviceLocal, "BDA Page Table Buffer");
    runtime.FillBuffer(bda_pagetable_buffer.get(), 0u, bda_pagetable_size, 0u);
    if (!instance.IsMaintenance6Supported()) {
        null_index_buffer = std::make_unique<Buffer>(instance, 0, NULL_INDEX_BUFFER_SIZE,
                                                     MemoryType::DeviceLocal, "Null Index Buffer");
        runtime.FillBuffer(null_index_buffer.get(), 0u, NULL_INDEX_BUFFER_SIZE, 0u);
    }
    LOG_INFO(Render, "Layer memory: device-local mirrors, {} KiB blocks, {} MiB BDA page table, "
                     "vkQueueBindSparse: 0 calls",
             BLOCK_SIZE >> 10, bda_pagetable_size >> 20);
    // Direct memory access needs the page table and buffer device addresses; without them the
    // in-shader path cannot be used and the setting is rejected here instead of faulting later.
    if (EmulatorSettings.IsDirectMemoryAccessEnabled() &&
        !instance.IsBufferDeviceAddressSupported()) {
        GuestMemoryUnusable("VK_KHR_buffer_device_address is not available");
    }
}

BufferCache::~BufferCache() = default;

void BufferCache::InvalidateMemory(VAddr device_addr, u64 size, bool assume_locks) {
    memory_tracker->InvalidateRegion(device_addr, size, [this, device_addr, size, assume_locks] {
        ReadMemory(device_addr, size, true, assume_locks);
    });
}

// bbport: the game fills its per-frame buffers (constants, skinning output) sequentially and
// every 4 KiB page cost a protection fault (~110k/s in Hunter's Nightmare, a fifth of each
// render worker's time in the kernel). A fault unprotects the aligned window around it instead;
// pages marked CPU-modified without being written only cost an upload when bound.
// Item: context = BufferCache, source = guest address, destination = host pointer, size,
// extra = the Buffer whose mapping holds the destination (flushed after the copy).
void BufferCache::RunGuestCopy(const BbCopy::Item& item) {
    auto* cache = static_cast<BufferCache*>(item.context);
    auto* dst = reinterpret_cast<u8*>(item.destination);
    cache->memory->CopySparseMemory(item.source, dst, item.size);
    auto* buffer = reinterpret_cast<Buffer*>(item.extra);
    buffer->Flush(dst - buffer->mapped_data.data(), item.size);
}

// Small guest copies run on the recording thread (it spins for work: no wakeup, and it is
// idle most of the time); PoolSmallCopies (toggle 524288) batches them for the copy threads.
// Each source is noted once the copy is queued (a WaitHostCopies started after the note covers it).
void BufferCache::SmallGuestCopy(const BbCopy::Item& item) {
    if (!scheduler.IsRecordingDeferred()) {
        item.run(item);
    } else if (!BbToggle::Disabled(BbToggle::PoolSmallCopies)) {
        scheduler.RecordHostCopy([item] { item.run(item); });
    } else {
        BbCopy::QueueCopy(item);
    }
    scheduler.NoteHostCopySource(item.source, item.size);
}

void BufferCache::ExtendWriteFault(VAddr device_addr) {
    static const u64 window = [] {
        const char* env = std::getenv("BB_FAULT_WINDOW");
        const u64 kib = env ? std::strtoull(env, nullptr, 10) : 256;
        return std::bit_ceil(std::clamp<u64>(kib, 4, 1024)) * 1024;
    }();
    if (window <= TRACKER_BYTES_PER_PAGE || BbToggle::Disabled(BbToggle::FaultWindow)) {
        return;
    }
    memory_tracker->ExtendWriteFault(Common::AlignDown(device_addr, window), window);
}

void BufferCache::ReadMemory(VAddr device_addr, u64 size, bool is_write, bool assume_locks) {
    const auto flush_request = [this, device_addr, size, is_write] {
        // GPU-modified ranges come as many small scattered islands,
        // so the download is widened to a window around the request
        constexpr u64 WindowSize = 512_KB;
        std::shared_ptr<Buffer> buffer;
        VAddr mirror_base = 0;
        VAddr mirror_end = 0;
        {
            std::scoped_lock lk{mirrors_mutex};
            Mirror* mirror = FindMirrorLocked(device_addr >> BLOCK_SHIFT);
            if (mirror) {
                buffer = mirror->buffer;
                mirror_base = mirror->buffer->cpu_addr;
                mirror_end =
                    mirror_base + ((mirror->committed_end - mirror->first_block) << BLOCK_SHIFT);
            }
        }
        if (buffer) {
            const VAddr window_start =
                std::max<VAddr>(Common::AlignDown(device_addr, WindowSize), mirror_base);
            const VAddr window_end = std::min<VAddr>(
                std::max<VAddr>(window_start + WindowSize, device_addr + size), mirror_end);
            if (window_start < window_end) {
                DownloadMemory(buffer, mirror_base, mirror_end, window_start,
                               window_end - window_start);
            }
        }
        if (is_write) {
            memory_tracker->MarkRegionAsCpuModified(device_addr, size);
        }
    };
    if (assume_locks) {
        flush_request();
    } else {
        liverpool->SendCommand<true>(std::move(flush_request));
    }
}

void BufferCache::DownloadMemory(const std::shared_ptr<Buffer>& buffer, VAddr mirror_base,
                                  VAddr mirror_end, VAddr device_addr, u64 size) {
    boost::container::small_vector<vk::BufferCopy, 1> copies;
    u64 total_size_bytes = 0;
    memory_tracker->ForEachDownloadRange<false>(device_addr, size, [&](u64 address, u64 size) {
        const auto add_download = [&](VAddr start, VAddr end) {
            start = std::max(start, mirror_base);
            end = std::min(end, mirror_end);
            if (start >= end) {
                return;
            }
            const u64 new_size = end - start;
            copies.push_back(vk::BufferCopy{
                .srcOffset = start - mirror_base,
                .dstOffset = total_size_bytes,
                .size = new_size,
            });
            // Align up to avoid cache conflicts
            constexpr u64 align = 64ULL;
            constexpr u64 mask = ~(align - 1ULL);
            total_size_bytes += (new_size + align - 1) & mask;
        };
        gpu_modified_ranges.ForEachInRange(address, size, add_download);
        gpu_modified_ranges.Subtract(address, size);
    });
    if (total_size_bytes == 0) {
        return;
    }
    const auto download = staging_pool.Request(total_size_bytes, VideoCore::MemoryType::HostCached);
    for (auto& copy : copies) {
        copy.dstOffset += download.offset;
    }
    runtime.CopyBuffer(buffer.get(), download.buffer, copies);
    scheduler.Finish();

    download.buffer->Invalidate(download.offset, download.size);
    for (const auto& copy : copies) {
        auto* dst_addr = std::bit_cast<u8*>(mirror_base + copy.srcOffset);
        memory->TryWriteBacking(dst_addr, download.mapped + (copy.dstOffset - download.offset),
                                copy.size);
    }
    memory_tracker->UnmarkRegionAsGpuModified(device_addr, size);
}

namespace {
// bbport: BB_BUFFER_STATS=1 — how buffer bindings reach the GPU, by guest region (256 MiB),
// printed every 5 s: small read-only copies into the stream buffer, mirror bindings, and the
// bytes those re-upload after CPU writes. Input for engine-level short paths.
struct BufferStats {
    struct Region {
        u64 stream_count{}, stream_bytes{}, mirror_count{}, mirror_bytes{}, upload_bytes{};
    };
    std::map<u64, Region> regions;
    /// Hot window: frame of the last binding per 64 KiB block, and the distribution of the
    /// frames between uses (1, 2, 3, 4, 5-8, 9-16, 17+): the ring's reuse distance.
    std::unordered_map<u64, u64> last_use;
    std::array<u64, 7> reuse{};
    std::chrono::steady_clock::time_point window = std::chrono::steady_clock::now();
    u64 calls = 0;
};
/// 256 MiB regions; 1 MiB inside the hot 0x104xxxxxxx window (the engine's frame data).
BufferStats& Stats();
void NoteHotUse(VAddr address, u32 size) {
    if ((address >> 28) != 0x104) {
        return;
    }
    auto& st = Stats();
    const u64 frame = BbStats::gpu_frames.load(std::memory_order_relaxed);
    for (u64 block = address >> 16; block <= (address + size - 1) >> 16; ++block) {
        auto [it, inserted] = st.last_use.try_emplace(block, frame);
        if (!inserted && it->second != frame) {
            const u64 d = frame - it->second;
            ++st.reuse[d <= 4 ? d - 1 : d <= 8 ? 4 : d <= 16 ? 5 : 6];
            it->second = frame;
        }
    }
}
u64 RegionKey(VAddr address) {
    return (address >> 28) == 0x104 ? (address >> 20) | (1ull << 40) : address >> 28;
}
bool BufferStatsEnabled() {
    static const bool enabled = [] {
        const char* value = std::getenv("BB_BUFFER_STATS");
        return value && value[0] == '1';
    }();
    return enabled;
}
BufferStats& Stats() {
    static BufferStats stats;
    return stats;
}
void PrintBufferStats() {
    auto& st = Stats();
    if ((++st.calls & 4095) != 0) {
        return;
    }
    const auto now = std::chrono::steady_clock::now();
    const double seconds = std::chrono::duration<double>(now - st.window).count();
    if (seconds < 5.0) {
        return;
    }
    std::printf("Buffer stats (%.1f s, per second): region, stream copies/bytes, mirror bindings, "
                "re-uploaded bytes\n", seconds);
    std::printf("  hot window reuse distance in frames (1,2,3,4,5-8,9-16,17+): %llu %llu %llu %llu "
                "%llu %llu %llu\n",
                (unsigned long long)st.reuse[0], (unsigned long long)st.reuse[1],
                (unsigned long long)st.reuse[2], (unsigned long long)st.reuse[3],
                (unsigned long long)st.reuse[4], (unsigned long long)st.reuse[5],
                (unsigned long long)st.reuse[6]);
    st.reuse = {};
    for (const auto& [region, r] : st.regions) {
        const u64 base = (region >> 40) ? (region & ((1ull << 40) - 1)) << 20 : region << 28;
        if ((region >> 40) && r.stream_bytes + r.upload_bytes < 1e6 * seconds) {
            continue; // quiet MiB of the hot window
        }
        std::printf("  %#012llx: %8.0f copies %8.2f MB, %8.0f mirror %8.2f MB bound, %8.2f MB "
                    "uploads\n",
                    static_cast<unsigned long long>(base), r.stream_count / seconds,
                    r.stream_bytes / seconds / 1e6, r.mirror_count / seconds,
                    r.mirror_bytes / seconds / 1e6,
                    r.upload_bytes / seconds / 1e6);
    }
    st.regions.clear();
    st.window = now;
}
} // namespace

BufferCache::Mirror* BufferCache::FindMirrorLocked(u64 block) {
    auto it = mirrors.upper_bound(block);
    if (it == mirrors.begin()) {
        return nullptr;
    }
    --it;
    Mirror& mirror = *it->second;
    return block < mirror.committed_end ? &mirror : nullptr;
}

void BufferCache::RetireMirrorLocked(std::unique_ptr<Mirror> mirror) {
    if (!mirror) {
        return;
    }
    // The commands recorded so far (including the copy out of this mirror) complete before the
    // deferred operation runs, because it waits for the tick they are submitted with.
    auto buffer = std::move(mirror->buffer);
    scheduler.DeferOperation([buffer = std::move(buffer)] {});
}

void BufferCache::MapPageTableLocked(const Mirror& mirror, u64 first_block, u64 last_block) {
    if (first_block > last_block) {
        return;
    }
    const u64 count = last_block - first_block + 1;
    const u64 bytes = count * sizeof(vk::DeviceAddress);
    const auto staging = staging_pool.Request(bytes, MemoryType::HostUncached);
    auto* entries = reinterpret_cast<vk::DeviceAddress*>(staging.mapped);
    const vk::DeviceAddress base = mirror.buffer->BufferDeviceAddress();
    const u64 base_offset = (first_block - mirror.first_block) << BLOCK_SHIFT;
    for (u64 i = 0; i < count; ++i) {
        entries[i] = base + base_offset + (i << BLOCK_SHIFT);
    }
    staging.Flush();
    const vk::BufferCopy copy{
        .srcOffset = staging.offset,
        .dstOffset = first_block * sizeof(vk::DeviceAddress),
        .size = bytes,
    };
    runtime.CopyBuffer(staging.buffer, bda_pagetable_buffer.get(), std::span{&copy, 1});
}

void BufferCache::UploadMissingBlocksLocked(Mirror& mirror, u64 first_block, u64 last_block) {
    boost::container::small_vector<vk::BufferCopy, 8> copies;
    size_t total_size_bytes = 0;
    const auto add_blocks = [&](u64 begin, u64 end) {
        if (begin >= end) {
            return;
        }
        copies.push_back(vk::BufferCopy{
            .srcOffset = 0,
            .dstOffset = begin << BLOCK_SHIFT,
            .size = (end - begin) << BLOCK_SHIFT,
        });
        total_size_bytes += (end - begin) << BLOCK_SHIFT;
    };
    u64 at = first_block;
    BbLayer::GpuMemory::Get().ForEachValid(first_block, last_block + 1, [&](u64 begin, u64 end) {
        if (begin > at) {
            add_blocks(at, begin);
        }
        at = std::max(at, end);
    });
    if (at < last_block + 1) {
        add_blocks(at, last_block + 1);
    }
    if (copies.empty()) {
        return;
    }
    // The blocks are current once the copies below execute, which is before any command that
    // binds them (all recorded in this command buffer, in order).
    BbLayer::GpuMemory::Get().MarkValid(first_block, last_block + 1);
    if (const auto* staging = UploadCopies(mirror.buffer.get(), copies, total_size_bytes)) {
        runtime.CopyBuffer(staging, mirror.buffer.get(), copies);
    }
}

BufferCache::Mirror& BufferCache::EnsureMirrorLocked(u64 first_block, u64 last_block) {
    // Already covered by one committed mirror.
    if (Mirror* mirror = FindMirrorLocked(first_block); mirror && last_block < mirror->committed_end) {
        return *mirror;
    }

    // Mirrors whose committed range intersects the request (committed ranges never overlap).
    std::vector<u64> absorbed;
    for (auto it = mirrors.upper_bound(last_block); it != mirrors.begin();) {
        --it;
        const Mirror& mirror = *it->second;
        if (mirror.committed_end <= first_block) {
            break;
        }
        if (mirror.first_block <= last_block) {
            absorbed.push_back(it->first);
        }
    }

    // One mirror reaches back over the request and has reserved room: grow it in place.
    if (absorbed.size() == 1) {
        Mirror& mirror = *mirrors[absorbed.front()];
        if (mirror.first_block <= first_block && last_block < mirror.capacity_end) {
            if (last_block + 1 > mirror.committed_end) {
                const u64 old_end = mirror.committed_end;
                mirror.committed_end = last_block + 1;
                BbLayer::GpuMemory::Get().RemoveMirror(mirror.first_block << BLOCK_SHIFT);
                BbLayer::GpuMemory::Get().AddMirror(
                    mirror.first_block << BLOCK_SHIFT,
                    (mirror.committed_end - mirror.first_block) << BLOCK_SHIFT,
                    mirror.buffer->Handle(), mirror.buffer->BufferDeviceAddress(), &mirror);
                MapPageTableLocked(mirror, old_end, last_block);
            }
            return mirror;
        }
    }

    // Merge every mirror that the new committed range will touch into one new mirror.
    u64 new_first = first_block;
    u64 new_last = last_block;
    bool changed = true;
    while (changed) {
        changed = false;
        for (const auto& [key, mirror] : mirrors) {
            if (mirror->committed_end <= new_first || mirror->first_block > new_last) {
                continue;
            }
            if (mirror->first_block < new_first) {
                new_first = mirror->first_block;
                changed = true;
            }
            if (mirror->committed_end - 1 > new_last) {
                new_last = mirror->committed_end - 1;
                changed = true;
            }
        }
    }

    std::vector<u64> merge_keys;
    for (const auto& [key, mirror] : mirrors) {
        if (mirror->committed_end > new_first && mirror->first_block <= new_last) {
            merge_keys.push_back(key);
        }
    }

    // Capacity: a modest surplus so sequential growth stays in place, capped at MAX_MIRROR_SIZE
    // (a single larger binding still gets exactly what it needs).
    constexpr u64 MAX_BLOCKS = u64{1} << (ADDRESS_SPACE_BITS - BLOCK_SHIFT);
    const u64 need_blocks = new_last + 1 - new_first;
    const u64 max_mirror_blocks = std::max<u64>(need_blocks, MAX_MIRROR_SIZE / BLOCK_SIZE);
    const u64 extra =
        std::min<u64>(std::max<u64>(need_blocks / 2, 32), max_mirror_blocks - need_blocks);
    u64 capacity_blocks = std::min<u64>(need_blocks + extra, MAX_BLOCKS - new_first);

    auto mirror = std::make_unique<Mirror>();
    mirror->first_block = new_first;
    mirror->committed_end = new_last + 1;
    mirror->capacity_end = new_first + capacity_blocks;
    mirror->buffer = std::make_shared<Buffer>(instance, new_first << BLOCK_SHIFT,
                                              capacity_blocks << BLOCK_SHIFT,
                                              MemoryType::DeviceLocal, "Mirror");
    mirror_bytes += capacity_blocks << BLOCK_SHIFT;

    // Move the current contents of the replaced mirrors into the new one.
    for (const u64 key : merge_keys) {
        Mirror& old = *mirrors[key];
        const vk::BufferCopy copy{
            .srcOffset = 0,
            .dstOffset = (old.first_block - new_first) << BLOCK_SHIFT,
            .size = (old.committed_end - old.first_block) << BLOCK_SHIFT,
        };
        runtime.CopyBuffer(old.buffer.get(), mirror->buffer.get(), std::span{&copy, 1});
    }

    for (const u64 key : merge_keys) {
        const auto it = mirrors.find(key);
        auto old = std::move(it->second);
        mirror_bytes -= (old->capacity_end - old->first_block) << BLOCK_SHIFT;
        BbLayer::GpuMemory::Get().RemoveMirror(key << BLOCK_SHIFT);
        mirrors.erase(it);
        RetireMirrorLocked(std::move(old));
    }

    Mirror& placed = *mirrors.emplace(new_first, std::move(mirror)).first->second;
    BbLayer::GpuMemory::Get().AddMirror(
        new_first << BLOCK_SHIFT, (new_last + 1 - new_first) << BLOCK_SHIFT,
        placed.buffer->Handle(), placed.buffer->BufferDeviceAddress(), &placed);
    MapPageTableLocked(placed, new_first, new_last);
    return placed;
}

std::pair<const Buffer*, u64> BufferCache::ObtainBuffer(VAddr device_addr, u32 size,
                                                        bool is_written, bool is_texel_buffer) {
    const bool stats = BufferStatsEnabled();
    if (stats) {
        PrintBufferStats();
        NoteHotUse(device_addr, size);
    }
    // For read-only buffers use device local stream buffer to reduce renderpass breaks.
    if (!is_written && size <= STREAM_THRESHOLD && !IsRegionGpuModified(device_addr, size)) {
        if (stats) {
            auto& region = Stats().regions[RegionKey(device_addr)];
            ++region.stream_count;
            region.stream_bytes += size;
        }
        // bbport: the guest data is copied on a copy thread, started now; submission and
        // guest-visible fences wait for it (Scheduler::WaitHostCopies).
        if (!stream_buffer.mapped_data.empty() &&
            !BbToggle::Disabled(BbToggle::DeferredStreamCopies)) {
            if (const auto offset = stream_buffer.Reserve(size, instance.UniformMinAlignment())) {
                SmallGuestCopy({
                    .run = &RunGuestCopy,
                    .context = this,
                    .source = device_addr,
                    .destination = reinterpret_cast<u64>(stream_buffer.mapped_data.data() + *offset),
                    .size = size,
                    .extra = reinterpret_cast<u64>(static_cast<Buffer*>(&stream_buffer)),
                });
                return {&stream_buffer, *offset};
            }
        }
        const auto [data, offset] = stream_buffer.Map(size, instance.UniformMinAlignment());
        memory->CopySparseMemory(device_addr, data, size);
        stream_buffer.Commit();
        return {&stream_buffer, offset};
    }
    const u64 first_block = device_addr >> BLOCK_SHIFT;
    const u64 last_block = (device_addr + size - 1) >> BLOCK_SHIFT;
    const u64 uploaded_before = BbStats::buffer_upload_bytes.load(std::memory_order_relaxed);
    std::scoped_lock lk{mirrors_mutex};
    Mirror& mirror = EnsureMirrorLocked(first_block, last_block);
    UploadMissingBlocksLocked(mirror, first_block, last_block);
    SynchronizeMemoryLocked(mirror.buffer.get(), device_addr, size, is_written, is_texel_buffer);
    if (stats) {
        auto& region = Stats().regions[RegionKey(device_addr)];
        ++region.mirror_count;
        region.mirror_bytes += size;
        region.upload_bytes +=
            BbStats::buffer_upload_bytes.load(std::memory_order_relaxed) - uploaded_before;
    }
    if (is_written) {
        gpu_modified_ranges.Add(device_addr, size);
    }
    return {mirror.buffer.get(), device_addr - mirror.buffer->cpu_addr};
}

std::pair<const Buffer*, u64> BufferCache::ObtainBufferForImage(VAddr device_addr, u32 size) {
    if (IsRegionGpuModified(device_addr, size)) {
        return ObtainBuffer(device_addr, size, false);
    }
    const auto staging = staging_pool.Request(size, VideoCore::MemoryType::HostUncached,
                                              instance.StorageMinAlignment());
    if (!BbToggle::Disabled(BbToggle::DeferredUploads)) {
        // bbport: texture data is copied on the copy threads (streaming: 100+ MB per frame);
        // the upload reads the staging only after submission, which waits for the copies.
        constexpr u64 Chunk = 1_MB;
        for (u64 offset = 0; offset < staging.size; offset += Chunk) {
            const BbCopy::Item item{
                .run = &RunGuestCopy,
                .context = this,
                .source = device_addr + offset,
                .destination = reinterpret_cast<u64>(staging.mapped + offset),
                .size = std::min(Chunk, staging.size - offset),
                .extra = reinterpret_cast<u64>(staging.buffer),
            };
            if (staging.size < Chunk) {
                SmallGuestCopy(item);
            } else {
                BbCopy::Async([item] { item.run(item); });
                scheduler.NoteHostCopySource(item.source, item.size);
            }
        }
        return {staging.buffer, staging.offset};
    }
    memory->CopySparseMemory(device_addr, staging.mapped, staging.size);
    staging.Flush();
    return {staging.buffer, staging.offset};
}

bool BufferCache::IsRegionCpuModified(VAddr addr, size_t size) {
    return memory_tracker->IsRegionCpuModified(addr, size);
}

bool BufferCache::IsRegionGpuModified(VAddr addr, size_t size) {
    return memory_tracker->IsRegionGpuModified(addr, size);
}

void BufferCache::ProcessFaultBuffer() {
    fault_manager->ProcessFaultBuffer();
}

void BufferCache::SynchronizeDmaBuffers() {
    std::scoped_lock lk{mirrors_mutex};
    for (const auto& [key, mirror] : mirrors) {
        if (mirror->committed_end <= mirror->first_block) {
            continue;
        }
        constexpr u64 ChunkSize = 16_MB;
        VAddr addr = mirror->first_block << BLOCK_SHIFT;
        u64 remaining = (mirror->committed_end - mirror->first_block) << BLOCK_SHIFT;
        while (remaining > 0) {
            const u64 size = std::min(remaining, ChunkSize);
            SynchronizeMemoryLocked(mirror->buffer.get(), addr, static_cast<u32>(size), false,
                                    false);
            addr += size;
            remaining -= size;
        }
    }
}

bool BufferCache::SynchronizeMemoryLocked(const Buffer* buffer, VAddr device_addr, u32 size,
                                          bool is_written, bool is_texel_buffer) {
    boost::container::small_vector<vk::BufferCopy, 4> copies;
    size_t total_size_bytes{};
    const Buffer* src_buffer{};
    memory_tracker->ForEachUploadRange(
        device_addr, size, is_written,
        [&](u64 addr, u64 size) {
            copies.emplace_back(total_size_bytes, addr, size);
            total_size_bytes += size;
        },
        [&] { src_buffer = UploadCopies(buffer, copies, total_size_bytes); });

    if (src_buffer) {
        runtime.CopyBuffer(src_buffer, buffer, copies);
    }
    if (is_texel_buffer && !is_written) {
        return SynchronizeMemoryFromImage(buffer, device_addr, size);
    }
    return false;
}

const Buffer* BufferCache::UploadCopies(const Buffer* buffer, std::span<vk::BufferCopy> copies,
                                        size_t total_size_bytes) {
    if (copies.empty()) {
        return nullptr;
    }
    BbStats::buffer_upload_bytes.fetch_add(total_size_bytes, std::memory_order_relaxed);
    const auto staging = staging_pool.Request(total_size_bytes, MemoryType::HostUncached);
    // bbport: the guest memory is copied into staging on the copy threads, started now in
    // groups of about 1 MiB; submission and guest-visible fences wait for them
    // (Scheduler::WaitHostCopies).
    if (!BbToggle::Disabled(BbToggle::DeferredUploads) && total_size_bytes < 1_MB) {
        // Small uploads join the calling thread's batch.
        for (auto& copy : copies) {
            SmallGuestCopy({
                .run = &RunGuestCopy,
                .context = this,
                .source = copy.dstOffset,
                .destination = reinterpret_cast<u64>(staging.mapped + copy.srcOffset),
                .size = copy.size,
                .extra = reinterpret_cast<u64>(staging.buffer),
            });
            copy.srcOffset += staging.offset;
            copy.dstOffset -= buffer->cpu_addr;
        }
        return staging.buffer;
    }
    if (!BbToggle::Disabled(BbToggle::DeferredUploads)) {
        struct HostCopy {
            VAddr source;
            u8* destination;
            u64 size;
        };
        using Group = boost::container::small_vector<HostCopy, 8>;
        auto group = std::make_shared<Group>();
        u64 group_bytes = 0;
        const auto launch = [&] {
            BbCopy::Async([group, memory = memory, staging] {
                for (const auto& copy : *group) {
                    memory->CopySparseMemory(copy.source, copy.destination, copy.size);
                }
                staging.Flush();
            });
            for (const auto& copy : *group) {
                scheduler.NoteHostCopySource(copy.source, copy.size);
            }
        };
        for (auto& copy : copies) {
            group->push_back({copy.dstOffset, staging.mapped + copy.srcOffset, copy.size});
            group_bytes += copy.size;
            copy.srcOffset += staging.offset;
            copy.dstOffset -= buffer->cpu_addr;
            if (group_bytes >= 1_MB) {
                launch();
                group = std::make_shared<Group>();
                group_bytes = 0;
            }
        }
        if (!group->empty()) {
            launch();
        }
        return staging.buffer;
    }
    const auto copy = [&](std::size_t i) {
        memory->CopySparseMemory(copies[i].dstOffset, staging.mapped + copies[i].srcOffset,
                                 copies[i].size);
    };
    if (total_size_bytes >= 2_MB && copies.size() > 1) {
        BbCopy::ParallelFor(copies.size(), copy);
    } else {
        for (std::size_t i = 0; i < copies.size(); ++i) {
            copy(i);
        }
    }
    for (auto& copy : copies) {
        copy.srcOffset += staging.offset;
        copy.dstOffset -= buffer->cpu_addr;
    }
    staging.Flush();
    return staging.buffer;
}

bool BufferCache::SynchronizeMemoryFromImage(const Buffer* buffer, VAddr device_addr, u32 size) {
    if (auto type = texture_cache.IsMeta(device_addr)) {
        if (*type == TextureCache::MetaType::HTile) {
            static constexpr u32 ZmaskUncompressed = 0xf;
            runtime.FillBuffer(buffer, buffer->Offset(device_addr), size, ZmaskUncompressed);
            return true;
        } else {
            LOG_WARNING(Render_Vulkan, "Unhandled metadata type {}", magic_enum::enum_name(*type));
        }
    }
    // bbport: most texel buffers alias no image; remember misses until images change.
    const u64 generation = texture_cache.RegistryGeneration();
    auto& miss = image_miss_cache[((device_addr >> 6) ^ size * 0x9E3779B1u) % image_miss_cache.size()];
    if (miss.address == device_addr && miss.size == size && miss.generation == generation &&
        !BbToggle::Disabled(BbToggle::TextureBindingMemo)) {
        return false;
    }
    const ImageId image_id = texture_cache.FindImageFromRange(device_addr, size);
    if (!image_id) {
        miss = {device_addr, size, generation};
        return false;
    }
    // bbport: the lookups above only read what the texture binding helper leaves alone; the
    // copy below changes image state, so the helper finishes first.
    runtime.BeforeImageAccess();
    Image& image = texture_cache.GetImage(image_id);
    ASSERT_MSG(device_addr == image.info.guest_address,
               "Texel buffer aliases image subresources {:x} : {:x}", device_addr,
               image.info.guest_address);
    const u64 buffer_offset = buffer->Offset(device_addr);
    boost::container::small_vector<vk::BufferImageCopy, 8> buffer_copies;
    for (u32 mip = 0; mip < image.info.resources.levels; mip++) {
        const auto& mip_info = image.info.mips_layout[mip];
        const u32 width = std::max(image.info.size.width >> mip, 1u);
        const u32 height = std::max(image.info.size.height >> mip, 1u);
        const u32 depth = std::max(image.info.size.depth >> mip, 1u);
        if (buffer_offset + mip_info.offset + mip_info.size > buffer->size_bytes) {
            break;
        }
        buffer_copies.push_back(vk::BufferImageCopy{
            .bufferOffset = mip_info.offset,
            .bufferRowLength = mip_info.pitch,
            .bufferImageHeight = mip_info.height,
            .imageSubresource{
                .aspectMask = image.aspect_mask & ~vk::ImageAspectFlagBits::eStencil,
                .mipLevel = mip,
                .baseArrayLayer = 0,
                .layerCount = image.info.resources.layers,
            },
            .imageOffset = {0, 0, 0},
            .imageExtent = {width, height, depth},
        });
    }
    if (buffer_copies.empty()) {
        return false;
    }
    auto& tile_manager = texture_cache.GetTileManager();
    tile_manager.TileImage(image, buffer_copies, buffer, buffer_offset);
    return true;
}

} // namespace VideoCore
