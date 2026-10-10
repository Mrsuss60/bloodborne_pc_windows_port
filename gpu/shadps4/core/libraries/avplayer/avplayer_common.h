// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <mutex>
#include <optional>
#include <string_view>
#include <utility>
#include <queue>

#include "core/libraries/avplayer/avplayer.h"

#define AVPLAYER_IS_ERROR(x) ((x) < 0)

namespace Libraries::AvPlayer {

// bbport: whether memory came from the host heap rather than the guest's allocator. Guest
// memory lies at [0x1000000000, 1 TiB) on Linux; on macOS it starts above Apple Silicon's GPU
// carveout (0x7800000000) and the host heap lies above 1 TiB.
inline bool IsHostMemory(const void* memory) {
    const auto address = reinterpret_cast<uintptr_t>(memory);
#ifdef __APPLE__
    return address < 0x7800000000ULL || address >= (1ULL << 40);
#else
    return address < 0x1000000000ULL;
#endif
}

enum class AvState {
    Unknown,
    Initial,
    AddingSource,
    Ready,
    Play,
    Stop,
    EndOfFile,
    Pause,
    C0x08,
    Jump,
    TrickMode,
    C0x0B,
    Buffering,
    Starting,
    Error,
};

enum class AvEventType {
    ChangeFlowState = 21,
    WarningId = 22,
    RevertState = 30,
    AddSource = 40,
    Error = 255,
};

union AvPlayerEventData {
    u32 num_frames; // 20
    AvState state;  // AvEventType::ChangeFlowState
    s32 error;      // AvEventType::WarningId
    u32 attempt;    // AvEventType::AddSource
};

struct AvPlayerEvent {
    AvEventType event;
    AvPlayerEventData payload;
};

template <class T>
class AvPlayerQueue {
public:
    size_t Size() {
        return m_queue.size();
    }

    void Push(T&& value) {
        std::lock_guard guard(m_mutex);
        m_queue.emplace(std::forward<T>(value));
    }

    T& Front() {
        return m_queue.front();
    }

    std::optional<T> Pop() {
        if (Size() == 0) {
            return std::nullopt;
        }
        std::lock_guard guard(m_mutex);
        auto result = std::move(m_queue.front());
        m_queue.pop();
        return result;
    }

    void Clear() {
        std::lock_guard guard(m_mutex);
        m_queue = {};
    }

private:
    std::mutex m_mutex{};
    std::queue<T> m_queue{};
};

AvPlayerSourceType GetSourceType(std::string_view path);

} // namespace Libraries::AvPlayer
