// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "bbport_copy.h"

#include <map>
#include <memory>
#include <mutex>

#include "common/types.h"
#include "layer/bblayer_gpu_memory.h"
#include "video_core/buffer_cache/buffer.h"
#include "video_core/buffer_cache/fault_manager.h"
#include "video_core/buffer_cache/range_set.h"

namespace AmdGpu {
struct Liverpool;
}

namespace Core {
class MemoryManager;
}

namespace Vulkan {
class GraphicsPipeline;
struct SubmitInfo;
class Runtime;
class StagingBufferPool;
} // namespace Vulkan

namespace VideoCore {

class TextureCache;
class MemoryTracker;
class PageManager;

class BufferCache {
    static constexpr u64 ADDRESS_SPACE_BITS = 40;
    static constexpr u64 ARENA_PAGE_BITS = 32;
    static constexpr u64 ARENA_PAGE_SIZE = u64{1} << ARENA_PAGE_BITS;
    static constexpr u64 NUM_ARENA_PAGES = u64{1} << (ADDRESS_SPACE_BITS - ARENA_PAGE_BITS);
    /// Fixed 16 KiB blocks for the BDA page table and the mirrors (upstream 0.7's granularity).
    static constexpr u32 BLOCK_SHIFT = 14;
    static constexpr u64 BLOCK_SIZE = u64{1} << BLOCK_SHIFT;
    /// A mirror never grows past this (unless one binding needs more in a single range).
    static constexpr u64 MAX_MIRROR_SIZE = 64_MB;

public:
    /// Read-only bindings up to this size are copied into a stream buffer (bbport: public for
    /// the draw pipeline's constant ring).
    static constexpr u64 STREAM_THRESHOLD = 16_KB;

    explicit BufferCache(const Vulkan::Instance& instance, Vulkan::Scheduler& scheduler,
                         Vulkan::Runtime& runtime, AmdGpu::Liverpool* liverpool,
                         TextureCache& texture_cache, PageManager& tracker);
    ~BufferCache();

    /// Returns a pointer to GDS device local buffer.
    [[nodiscard]] const Buffer* GetGdsBuffer() const noexcept {
        return &gds_buffer;
    }

    [[nodiscard]] const Buffer* GetNullIndexBuffer() const noexcept {
        return null_index_buffer.get();
    }

    /// Retrieves the device local DBA page table buffer.
    [[nodiscard]] Buffer* GetBdaPageTableBuffer() noexcept {
        return bda_pagetable_buffer.get();
    }

    /// Retrieves the fault buffer.
    [[nodiscard]] Buffer* GetFaultBuffer() noexcept {
        return fault_manager->GetFaultBuffer();
    }

    /// Retrieves the stream buffer.
    StreamBuffer& GetStreamBuffer() noexcept {
        return stream_buffer;
    }

    /// Returns minimum granularity of a page table (and mirror) block.
    u32 GetSparsePageShift() const noexcept {
        return BLOCK_SHIFT;
    }

    /// Invalidates any buffer in the logical page range.
    void InvalidateMemory(VAddr device_addr, u64 size, bool assume_locks = false);

    /// bbport: a guest write fault at `device_addr` was handled; unprotect its neighbourhood.
    void ExtendWriteFault(VAddr device_addr);

    /// Flushes any GPU modified buffer in the logical page range back to CPU memory.
    void ReadMemory(VAddr device_addr, u64 size, bool is_write = false, bool assume_locks = false);

    /// Finds a buffer for the specified region.
    [[nodiscard]] std::pair<const Buffer*, u64> ObtainBuffer(VAddr device_addr, u32 size,
                                                             bool is_written,
                                                             bool is_texel_buffer = false);

    /// Attempts to obtain a buffer without modifying the cache contents.
    [[nodiscard]] std::pair<const Buffer*, u64> ObtainBufferForImage(VAddr device_addr, u32 size);

    /// Return true when a region is modified from the CPU
    [[nodiscard]] bool IsRegionCpuModified(VAddr addr, size_t size);

    /// Return true when a region is modified from the GPU
    [[nodiscard]] bool IsRegionGpuModified(VAddr addr, size_t size);

    /// Processes the fault buffer.
    void ProcessFaultBuffer();

    /// Synchronizes all buffers needed for DMA.
    void SynchronizeDmaBuffers();

private:
    /// A dense device-local copy of a run of guest blocks. `buffer` is allocated for the whole
    /// capacity; only [first_block, committed_end) is mapped in the page table and valid.
    struct Mirror {
        /// Shared so a download can keep the buffer alive after a merge retires it.
        std::shared_ptr<Buffer> buffer;
        u64 first_block{};
        u64 committed_end{};
        u64 capacity_end{};
    };

    /// The mirror committed over `block`, or null (caller holds mirrors_mutex).
    Mirror* FindMirrorLocked(u64 block);

    /// Makes [first_block, last_block] covered by one committed mirror (allocating, growing or
    /// merging as needed) and maps its blocks in the BDA page table.
    Mirror& EnsureMirrorLocked(u64 first_block, u64 last_block);
    /// Uploads the guest data of every block of [first_block, last_block] that has never been
    /// populated in a mirror.
    void UploadMissingBlocksLocked(Mirror& mirror, u64 first_block, u64 last_block);
    /// Points the BDA page table entries of [first_block, last_block] at `mirror`.
    void MapPageTableLocked(const Mirror& mirror, u64 first_block, u64 last_block);
    /// Schedules the buffers of retired mirrors for destruction once the GPU is past this tick.
    void RetireMirrorLocked(std::unique_ptr<Mirror> mirror);

    /// Downloads GPU-modified data of one mirror. The buffer is kept alive by the shared_ptr and
    /// no lock is held (a download waits for the GPU).
    void DownloadMemory(const std::shared_ptr<Buffer>& buffer, VAddr mirror_base,
                        VAddr mirror_end, VAddr device_addr, u64 size);

    bool SynchronizeMemoryLocked(const Buffer* buffer, VAddr device_addr, u32 size,
                                 bool is_written, bool is_texel_buffer);

    /// bbport: batched small copies on the copy threads (BbCopy::QueueCopy).
    static void RunGuestCopy(const BbCopy::Item& item);
    void SmallGuestCopy(const BbCopy::Item& item);

    const Buffer* UploadCopies(const Buffer* buffer, std::span<vk::BufferCopy> copies,
                               size_t total_size_bytes);

    bool SynchronizeMemoryFromImage(const Buffer* buffer, VAddr device_addr, u32 size);

    const Vulkan::Instance& instance;
    Vulkan::Scheduler& scheduler;
    // bbport: texel buffer ranges known not to alias an image, per image registry generation.
    struct ImageMiss {
        VAddr address = 0;
        u32 size = 0;
        u64 generation = ~0ULL;
    };
    std::array<ImageMiss, 256> image_miss_cache{};
    Vulkan::Runtime& runtime;
    Vulkan::StagingBufferPool& staging_pool;
    AmdGpu::Liverpool* liverpool;
    Core::MemoryManager* memory;
    TextureCache& texture_cache;
    std::unique_ptr<MemoryTracker> memory_tracker;

    StreamBuffer stream_buffer;
    Buffer gds_buffer;
    std::unique_ptr<Buffer> null_index_buffer;
    RangeSet gpu_modified_ranges;

    std::unique_ptr<FaultManager> fault_manager;
    std::unique_ptr<Buffer> bda_pagetable_buffer;

    /// Guards the mirror map and the page table writes that go with it. ObtainBuffer runs on the
    /// draw/recording threads; the GPU command thread only reads mirrors (downloads).
    std::mutex mirrors_mutex;
    std::map<u64, std::unique_ptr<Mirror>> mirrors;
    u64 mirror_bytes{}; ///< Bytes allocated for mirrors (diagnostics).
};

} // namespace VideoCore
