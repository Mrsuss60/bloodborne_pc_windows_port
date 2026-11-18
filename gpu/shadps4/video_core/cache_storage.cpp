// SPDX-FileCopyrightText: Copyright 2025-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "common/elf_info.h"
#include "common/io_file.h"
#include "common/polyfill_thread.h"
#include "common/thread.h"
#include "core/emulator_settings.h"

#include "video_core/cache_storage.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_pipeline_cache.h"

#include <algorithm>
#include <condition_variable>
#include <cstdio>
#include <atomic>
#include <functional>
#include <future>
#include <mutex>
#include <queue>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace {

std::mutex submit_mutex{};
std::atomic<u32> num_requests{};
std::condition_variable_any request_cv{};
std::queue<std::packaged_task<void()>> req_queue{};
std::mutex m_request{};

} // namespace

namespace Storage {

void ProcessIO(const std::stop_token& stoken) {
    Common::SetCurrentThreadName("shadPS4:PipelineCacheIO");

    for (;;) {
        {
            std::unique_lock lk{submit_mutex};
            Common::CondvarWait(request_cv, lk, stoken, [&] { return num_requests.load() != 0; });
        }

        // bbport: a stop request must not drop queued writes (Close() flushes through this queue).
        if (stoken.stop_requested() && num_requests == 0) {
            break;
        }

        while (num_requests) {
            std::packaged_task<void()> request{};
            {
                std::scoped_lock lock{m_request};
                if (req_queue.empty()) {
                    continue;
                }
                request = std::move(req_queue.front());
                req_queue.pop();
            }

            if (request.valid()) {
                request();
            }

            --num_requests;
        }
    }
}

constexpr std::string GetBlobFileExtension(BlobType type) {
    switch (type) {
    case BlobType::ShaderMeta: {
        return "meta";
    }
    case BlobType::ShaderBinary: {
        return "spv";
    }
    case BlobType::PipelineKey: {
        return "key";
    }
    case BlobType::ShaderProfile: {
        return "bin";
    }
    case BlobType::PipelineBinary: {
        return "vkp";
    }
    case BlobType::ShaderInput: {
        return "in";
    }
    default:
        UNREACHABLE();
    }
}

void DataBase::Open() {
    if (IsOpened()) {
        return;
    }

    const auto& game_info = Common::ElfInfo::Instance();

    using namespace Common::FS;
    cache_path = GetUserPath(PathType::CacheDir) / game_info.GameSerial();
    if (!std::filesystem::exists(cache_path)) {
        std::filesystem::create_directories(cache_path);
    }

    io_worker = std::jthread{ProcessIO};
    opened.store(true, std::memory_order_release);
}

void DataBase::Close() {
    if (!IsOpened()) {
        return;
    }

    // bbport: process everything that was queued (the driver pipeline cache in particular)
    // before the worker is stopped; stopping first would drop those writes.
    Flush();

    io_worker.request_stop();
    io_worker.join();

    opened.store(false, std::memory_order_release);
    LOG_INFO(Render, "Cache dumped");
}

void DataBase::Flush() {
    if (!IsOpened()) {
        return;
    }

    // The queue is FIFO, so this marker completing means every earlier write is on disk.
    std::promise<void> marker;
    auto done = marker.get_future();
    {
        std::scoped_lock lock{m_request};
        req_queue.emplace([&marker] { marker.set_value(); });
    }
    {
        std::scoped_lock lk{submit_mutex};
        ++num_requests;
    }
    request_cv.notify_one();
    done.wait();
}

template <typename T>
bool WriteVector(const BlobType type, std::filesystem::path&& path_, std::vector<T>&& v) {
    {
        auto request = std::packaged_task<void()>{
            [type, path = std::move(path_), v = std::move(v)]() mutable {
                path.replace_extension(GetBlobFileExtension(type));
                using namespace Common::FS;
                auto temp_path = path;
                temp_path += ".tmp";
                {
                    const auto file = IOFile{temp_path, FileAccessMode::Create};
                    file.Write(v);
                }
#ifdef _WIN32
                if (!MoveFileExW(temp_path.c_str(), path.c_str(),
                                 MOVEFILE_REPLACE_EXISTING | MOVEFILE_COPY_ALLOWED)) {
                    std::error_code ec;
                    std::filesystem::remove(path, ec);
                    std::filesystem::rename(temp_path, path, ec);
                }
#else
                std::error_code ec;
                std::filesystem::rename(temp_path, path, ec);
#endif
            }};
        std::scoped_lock lock{m_request};
        req_queue.emplace(std::move(request));
    }

    std::scoped_lock lk{submit_mutex};
    ++num_requests;
    request_cv.notify_one();
    return true;
}

template <typename T>
void LoadVector(BlobType type, std::filesystem::path& path, std::vector<T>& v) {
    using namespace Common::FS;
    path.replace_extension(GetBlobFileExtension(type));
    std::error_code ec;
    if (!std::filesystem::exists(path, ec)) {
        v.clear();
        return;
    }
    const auto file = IOFile{path, FileAccessMode::Read};
    if (!file.IsOpen()) {
        v.clear();
        return;
    }
    v.resize(file.GetSize() / sizeof(T));
    file.Read(v);
}

bool DataBase::Save(BlobType type, const std::string& name, std::vector<u8>&& data) {
    if (!IsOpened()) {
        return false;
    }

    return WriteVector(type, cache_path / name, std::move(data));
}

bool DataBase::Save(BlobType type, const std::string& name, std::vector<u32>&& data) {
    if (!IsOpened()) {
        return false;
    }

    return WriteVector(type, cache_path / name, std::move(data));
}

void DataBase::Load(BlobType type, const std::string& name, std::vector<u8>& data) {
    if (!IsOpened()) {
        return;
    }

    auto path = cache_path / name;
    LoadVector(type, path, data);
}

void DataBase::Load(BlobType type, const std::string& name, std::vector<u32>& data) {
    if (!IsOpened()) {
        return;
    }

    auto path = cache_path / name;
    LoadVector(type, path, data);
}

u32 DataBase::ForEachBlob(BlobType type,
                          const std::function<BlobAction(std::vector<u8>&& data)>& func) {
    const auto& ext = GetBlobFileExtension(type);
    std::vector<std::filesystem::path> rejected;
    for (const auto& file_name : std::filesystem::directory_iterator{cache_path}) {
        if (file_name.path().extension().string().ends_with(ext)) {
            using namespace Common::FS;
            std::vector<u8> data;
            {
                const auto& file = IOFile{file_name, FileAccessMode::Read};
                if (!file.IsOpen()) {
                    continue;
                }
                data.resize(file.GetSize());
                file.Read(data);
            }
            if (func(std::move(data)) == BlobAction::Delete) {
                rejected.push_back(file_name.path());
            }
        }
    }
    u32 removed = 0;
    std::error_code ec;
    for (const auto& path : rejected) {
        removed += std::filesystem::remove(path, ec) ? 1 : 0;
    }
    return removed;
}

void DataBase::Clear() {
    if (!IsOpened()) {
        return;
    }
    Flush();
    std::error_code ec;
    u64 removed = 0;
    for (const auto& entry : std::filesystem::directory_iterator(cache_path, ec)) {
        if (entry.is_regular_file(ec)) {
            if (entry.path().filename() == "cache_epoch.bin") {
                continue;
            }
            if (std::filesystem::remove(entry.path(), ec)) {
                ++removed;
            }
        } else if (entry.is_directory(ec)) {
            const auto name = entry.path().filename();
            if (name == "import" || name == "import.tmp") {
                const auto sub_removed = std::filesystem::remove_all(entry.path(), ec);
                if (!ec && sub_removed != static_cast<std::uintmax_t>(-1)) {
                    removed += sub_removed;
                }
            }
        }
    }
    LOG_WARNING(Render, "Pipeline cache cleared ({} entries): it is rebuilt for this build",
                removed);
}

namespace {

constexpr u32 CacheEpochMagic = 0x42424345u; // "BBCE"

std::filesystem::path GetExeEpochPath() {
    std::error_code ec;
#ifdef _WIN32
    wchar_t exe_buf[MAX_PATH]{};
    const DWORD len = GetModuleFileNameW(nullptr, exe_buf, MAX_PATH);
    if (len > 0 && len < MAX_PATH) {
        return std::filesystem::path(exe_buf).parent_path() / "cache_epoch.bin";
    }
#else
    const auto exe = std::filesystem::read_symlink("/proc/self/exe", ec);
    if (!ec && !exe.empty()) {
        return exe.parent_path() / "cache_epoch.bin";
    }
#endif
    return {};
}

struct CacheEpochHeader {
    u32 magic{};
    u32 epoch{};
};

bool ReadEpochFile(const std::filesystem::path& path, u32& out_epoch) {
    if (path.empty()) {
        return false;
    }
    std::error_code ec;
    if (!std::filesystem::exists(path, ec)) {
        return false;
    }
    const Common::FS::IOFile file{path, Common::FS::FileAccessMode::Read};
    if (!file.IsOpen() || file.GetSize() != sizeof(CacheEpochHeader)) {
        return false;
    }
    CacheEpochHeader hdr{};
    if (!file.ReadObject(hdr) || hdr.magic != CacheEpochMagic) {
        return false;
    }
    out_epoch = hdr.epoch;
    return true;
}

bool WriteEpochFile(const std::filesystem::path& path, u32 epoch) {
    if (path.empty()) {
        return false;
    }
    std::error_code ec;
    const auto parent = path.parent_path();
    if (!parent.empty()) {
        std::filesystem::create_directories(parent, ec);
    }
    auto temp_path = path;
    temp_path += ".tmp";
    const CacheEpochHeader hdr{CacheEpochMagic, epoch};
    {
        const Common::FS::IOFile file{temp_path, Common::FS::FileAccessMode::Create};
        if (!file.IsOpen() || !file.WriteObject(hdr)) {
            std::filesystem::remove(temp_path, ec);
            return false;
        }
    }
#ifdef _WIN32
    if (!MoveFileExW(temp_path.c_str(), path.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        std::filesystem::remove(path, ec);
        std::filesystem::rename(temp_path, path, ec);
        if (ec) {
            std::filesystem::remove(temp_path, ec);
            return false;
        }
    }
    return true;
#else
    std::filesystem::rename(temp_path, path, ec);
    if (ec) {
        std::filesystem::remove(temp_path, ec);
        return false;
    }
    return true;
#endif
}

bool CacheDirHasEntries(const std::filesystem::path& dir) {
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
        if (entry.path().filename() == "cache_epoch.bin") {
            continue;
        }
        return true;
    }
    return false;
}

} // namespace

bool IsCacheEpochCurrent(const std::filesystem::path& cache_dir, u32 required_epoch) {
    u32 stored = 0;
    if (ReadEpochFile(GetExeEpochPath(), stored) && stored >= required_epoch) {
        return true;
    }
    if (!cache_dir.empty() && ReadEpochFile(cache_dir / "cache_epoch.bin", stored) &&
        stored >= required_epoch) {
        return true;
    }
    return false;
}

bool DataBase::EnsureEpoch(u32 required_epoch) {
    if (!IsOpened()) {
        return false;
    }
    const auto exe_epoch_path = GetExeEpochPath();
    const auto fallback_epoch_path = cache_path / "cache_epoch.bin";

    u32 stored = 0;
    if (ReadEpochFile(exe_epoch_path, stored) && stored >= required_epoch) {
        std::printf("Shader cache: epoch %u current\n", stored);
        return false;
    }
    u32 fallback_stored = 0;
    if (ReadEpochFile(fallback_epoch_path, fallback_stored) && fallback_stored >= required_epoch) {
        // Mirror to out/ if writable so user cache clears preserve the stamp.
        WriteEpochFile(exe_epoch_path, fallback_stored);
        std::printf("Shader cache: epoch %u current\n", fallback_stored);
        return false;
    }

    const u32 prev_epoch = std::max(stored, fallback_stored);
    const bool had_entries = CacheDirHasEntries(cache_path);
    if (had_entries) {
        LOG_WARNING(Render,
                    "Pipeline cache epoch updated ({} -> {}): clearing stale cache for this build",
                    prev_epoch, required_epoch);
        std::printf("Shader cache: epoch updated (%u -> %u), clearing stale cache\n", prev_epoch,
                    required_epoch);
        Clear();
    } else {
        std::printf("Shader cache: initialized at epoch %u\n", required_epoch);
    }

    if (!WriteEpochFile(exe_epoch_path, required_epoch)) {
        if (!WriteEpochFile(fallback_epoch_path, required_epoch)) {
            LOG_WARNING(Render, "Failed to persist cache epoch {} stamp", required_epoch);
        }
    }
    return had_entries;
}

void DataBase::FinishPreload() {}

} // namespace Storage
