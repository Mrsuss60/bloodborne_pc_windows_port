// SPDX-License-Identifier: GPL-2.0-or-later
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <set>
#include <thread>
#include <unordered_map>
#include <vector>
#include "gpu/shadps4/video_core/renderer_vulkan/scene_proxy_release.h"

namespace {

int failures = 0;

void Expect(bool ok, const char* what) {
    if (!ok) {
        std::printf("FAIL: %s\n", what);
        ++failures;
    }
}

constexpr std::uint64_t Key(std::uint64_t uid, std::uint32_t level) {
    return uid << 4 | level;
}

struct Proxy {
    int* alive;
    explicit Proxy(int* counter) : alive{counter} {
        ++*alive;
    }
    ~Proxy() {
        --*alive;
    }
};

void QueueKeepsEveryUid() {
    Vulkan::ProxyReleases releases;
    Expect(releases.Take().empty(), "an empty queue gives nothing");
    std::vector<std::thread> threads;
    for (std::uint64_t t = 0; t < 4; ++t) {
        threads.emplace_back([&releases, t] {
            for (std::uint64_t i = 0; i < 1000; ++i) {
                releases.Add(t * 1000 + i + 1);
            }
        });
    }
    for (auto& thread : threads) {
        thread.join();
    }
    const auto taken = releases.Take();
    const std::set<std::uint64_t> unique(taken.begin(), taken.end());
    Expect(taken.size() == 4000 && unique.size() == 4000, "every uid added from four threads is taken once");
    Expect(releases.Take().empty(), "a second take is empty");
}

void TakesEveryLevelOfOneImage() {
    int alive = 0;
    std::unordered_map<std::uint64_t, std::unique_ptr<Proxy>> entries;
    for (std::uint64_t uid : {7u, 8u}) {
        for (std::uint32_t level : {0u, 2u, 15u}) {
            entries[Key(uid, level)] = std::make_unique<Proxy>(&alive);
        }
    }
    std::vector<std::unique_ptr<Proxy>> doomed;
    const auto taken = Vulkan::TakeProxies(entries, 7, 16, Key, doomed);
    Expect(taken == 3 && doomed.size() == 3, "all three levels of the freed image are taken");
    Expect(entries.size() == 3 && entries.contains(Key(8, 0)) && entries.contains(Key(8, 15)),
           "the other image keeps its proxies");
    Expect(alive == 6, "taken proxies live until the deferred release runs");
    doomed.clear();
    Expect(alive == 3, "the deferred release destroys them");
    Expect(Vulkan::TakeProxies(entries, 7, 16, Key, doomed) == 0, "a freed image is taken once");
    Expect(Vulkan::TakeProxies(entries, 99, 16, Key, doomed) == 0, "an image with no proxy takes nothing");
}

void ChurnStaysBounded() {
    int alive = 0;
    std::unordered_map<std::uint64_t, std::unique_ptr<Proxy>> entries;
    Vulkan::ProxyReleases releases;
    std::uint64_t next_uid = 1;
    std::vector<std::uint64_t> live;
    for (int round = 0; round < 5000; ++round) {
        for (const auto uid : releases.Take()) {
            std::vector<std::unique_ptr<Proxy>> doomed;
            Vulkan::TakeProxies(entries, uid, 16, Key, doomed);
        }
        const std::uint64_t uid = next_uid++;
        entries[Key(uid, 0)] = std::make_unique<Proxy>(&alive);
        live.push_back(uid);
        if (live.size() > 12) {
            releases.Add(live.front());
            live.erase(live.begin());
        }
    }
    Expect(entries.size() <= 14 && alive <= 14, "re-created targets keep a bounded number of proxies");
}

}

int main() {
    QueueKeepsEveryUid();
    TakesEveryLevelOfOneImage();
    ChurnStaysBounded();
    if (failures) {
        std::printf("scene-proxy-release-test: %d failures\n", failures);
        return EXIT_FAILURE;
    }
    std::puts("scene-proxy-release-test: PASS");
    return EXIT_SUCCESS;
}
