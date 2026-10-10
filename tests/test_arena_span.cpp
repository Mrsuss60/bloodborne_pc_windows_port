// SPDX-License-Identifier: GPL-2.0-or-later
// Dense buffer arenas (no sparse residency, MoltenVK): which pages a request's arena covers and
// which existing arenas it merges (arena_span.h). No GPU needed.
#include <cassert>
#include <cstdio>
#include <vector>
#include "video_core/buffer_cache/arena_span.h"

namespace {
struct FakeArena {
    u64 cpu_addr;
    u64 size_bytes;
};
constexpr u32 Bits = 28; // 256 MiB pages
constexpr u64 Page = u64{1} << Bits;

struct Space {
    std::vector<const FakeArena*> pages = std::vector<const FakeArena*>(64, nullptr);
    void Place(const FakeArena& arena) {
        for (u64 p = arena.cpu_addr >> Bits; p <= (arena.cpu_addr + arena.size_bytes - 1) >> Bits; ++p) {
            pages[p] = &arena;
        }
    }
    auto Span(u64 first, u64 last) const {
        return VideoCore::ComputeArenaSpan<FakeArena>(pages, first, last, Bits);
    }
};
} // namespace

int main() {
    // No arena yet: exactly the requested pages, nothing merged.
    {
        Space space;
        const auto s = space.Span(5, 5);
        assert(s.first_page == 5 && s.last_page == 5 && s.merged.empty());
        const auto two = space.Span(5, 6);
        assert(two.first_page == 5 && two.last_page == 6 && two.merged.empty());
    }
    // A request straddling two arenas merges both and covers them whole.
    {
        Space space;
        const FakeArena a{4 * Page, Page}, b{5 * Page, 2 * Page};
        space.Place(a);
        space.Place(b);
        const auto s = space.Span(4, 5);
        assert(s.first_page == 4 && s.last_page == 6);
        assert(s.merged.size() == 2 && s.merged[0] == &a && s.merged[1] == &b);
    }
    // Touching the edge of a large arena grows the span to that arena's full extent.
    {
        Space space;
        const FakeArena big{10 * Page, 4 * Page}; // pages 10..13
        space.Place(big);
        const auto s = space.Span(13, 14);
        assert(s.first_page == 10 && s.last_page == 14);
        assert(s.merged.size() == 1 && s.merged[0] == &big);
    }
    // A gap between arenas: both merge, and the empty page joins the span.
    {
        Space space;
        const FakeArena a{20 * Page, Page}, b{22 * Page, Page};
        space.Place(a);
        space.Place(b);
        const auto s = space.Span(20, 22);
        assert(s.first_page == 20 && s.last_page == 22 && s.merged.size() == 2);
    }
    // The same arena over several pages counts once.
    {
        Space space;
        const FakeArena a{30 * Page, 3 * Page}, b{33 * Page, Page};
        space.Place(a);
        space.Place(b);
        const auto s = space.Span(30, 33);
        assert(s.first_page == 30 && s.last_page == 33 && s.merged.size() == 2);
    }
    std::puts("Arena span: new pages, straddles, large arena edges, gaps, repeats PASS");
    return 0;
}
