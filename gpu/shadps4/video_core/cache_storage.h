// SPDX-FileCopyrightText: Copyright 2025 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "common/path_util.h"
#include "common/singleton.h"
#include "common/types.h"

#include <atomic>
#include <functional>
#include <thread>
#include <vector>

namespace Storage {

enum class BlobType : u32 {
    ShaderMeta,
    ShaderBinary,
    PipelineKey,
    ShaderProfile,
    PipelineBinary,
    ShaderInput,
};

enum class BlobAction : u8 {
    Keep,
    Delete,
};

class DataBase {
public:
    static DataBase& Instance() {
        return *Common::Singleton<DataBase>::Instance();
    }

    void Open();
    void Close();
    /// bbport: blocks until every queued blob write has reached the disk. Close() uses it before
    /// stopping the IO worker, which otherwise could exit with writes still queued (the process
    /// leaves through _Exit, so nothing else flushes them either).
    void Flush();
    [[nodiscard]] bool IsOpened() const {
        return opened.load(std::memory_order_acquire);
    }
    [[nodiscard]] const std::filesystem::path& Directory() const {
        return cache_path;
    }
    void FinishPreload();
    /// bbport: removes every cached blob and stale import staging directories.
    void Clear();
    /// bbport: checks <exe_dir>/cache_epoch.bin (with fallback in cache_path if unwritable).
    /// If the stored epoch is missing or older than required_epoch, performs a one-time full
    /// Clear() before writing the new epoch stamp. Returns true if an existing cache was cleared.
    bool EnsureEpoch(u32 required_epoch);

    bool Save(BlobType type, const std::string& name, std::vector<u8>&& data);
    bool Save(BlobType type, const std::string& name, std::vector<u32>&& data);

    void Load(BlobType type, const std::string& name, std::vector<u8>& data);
    void Load(BlobType type, const std::string& name, std::vector<u32>& data);

    /// bbport: func returns BlobAction::Delete only for a blob that is provably stale/corrupt;
    /// BlobAction::Keep preserves the blob even if it could not be preloaded on this launch.
    /// Returns the number of deleted blobs.
    u32 ForEachBlob(BlobType type, const std::function<BlobAction(std::vector<u8>&& data)>& func);

private:
    std::jthread io_worker{};
    std::filesystem::path cache_path{};
    std::atomic<bool> opened{false};
};

/// bbport: returns true when <exe_dir>/cache_epoch.bin (or fallback cache_dir/cache_epoch.bin)
/// already records at least required_epoch.
bool IsCacheEpochCurrent(const std::filesystem::path& cache_dir, u32 required_epoch);

} // namespace Storage
