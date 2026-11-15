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

#include <condition_variable>
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
    const auto file = IOFile{path, FileAccessMode::Read};
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

u32 DataBase::ForEachBlob(BlobType type, const std::function<bool(std::vector<u8>&& data)>& func) {
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
            if (!func(std::move(data))) {
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
    std::error_code ec;
    u64 removed = 0;
    for (const auto& entry : std::filesystem::directory_iterator(cache_path, ec)) {
        if (entry.is_regular_file(ec) && std::filesystem::remove(entry.path(), ec)) {
            ++removed;
        }
    }
    LOG_WARNING(Render, "Pipeline cache cleared ({} files): it is rebuilt for this build", removed);
}

void DataBase::FinishPreload() {}

} // namespace Storage
