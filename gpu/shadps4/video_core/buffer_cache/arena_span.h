// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: the arena pages a dense buffer arena must cover (BufferCache::GetDenseArena). A
// request covers pages [first_page, last_page]; every existing arena it touches is merged, and
// the span grows to their whole extent. Arenas cover contiguous page spans and never overlap.

#pragma once

#include <algorithm>
#include <span>
#include <boost/container/small_vector.hpp>

#include "common/types.h"

namespace VideoCore {

template <typename Arena>
struct ArenaSpan {
    u64 first_page;
    u64 last_page;
    boost::container::small_vector<const Arena*, 4> merged; ///< in address order
};

/// address_space[page] is the arena covering the page, or null. Arena: cpu_addr, size_bytes.
template <typename Arena>
ArenaSpan<Arena> ComputeArenaSpan(std::span<const Arena* const> address_space, u64 first_page,
                                  u64 last_page, u32 page_bits) {
    ArenaSpan<Arena> span{first_page, last_page, {}};
    for (u64 page = first_page; page <= last_page; ++page) {
        const Arena* arena = address_space[page];
        if (!arena || (!span.merged.empty() && span.merged.back() == arena)) {
            continue;
        }
        span.merged.push_back(arena);
        span.first_page = std::min<u64>(span.first_page, arena->cpu_addr >> page_bits);
        span.last_page = std::max<u64>(
            span.last_page, (arena->cpu_addr + arena->size_bytes - 1) >> page_bits);
    }
    return span;
}

} // namespace VideoCore
