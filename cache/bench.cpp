// #include <iostream>
// #include <vector>
// #include <chrono>
// #include <random>
// #include <algorithm>
// #include "range_cache.hpp"

// int main() {
//     const size_t numEntries = 100000;
//     const size_t numQueries = 1000000;

//     RangeCache<int, int> cache(numEntries);

//     // Insert random ranges
//     std::mt19937 rng(42);
//     std::uniform_int_distribution<int> dist(0, 1000000);

//     for (size_t i = 0; i < numEntries; ++i) {
//         int start = dist(rng);
//         int end = start + dist(rng) % 100;
//         cache.add(start, end, i);
//     }

//     std::vector<int> keys;
//     keys.reserve(numQueries);
//     for (size_t i = 0; i < numQueries; ++i) {
//         keys.push_back(dist(rng));
//     }

//     std::vector<uint64_t> latencies;
//     latencies.reserve(numQueries);

//     auto t0 = std::chrono::high_resolution_clock::now();
//     for (auto key : keys) {
//         auto ts1 = std::chrono::high_resolution_clock::now();
//         auto* e = cache.search(key);
//         auto ts2 = std::chrono::high_resolution_clock::now();
//         latencies.push_back(std::chrono::duration_cast<std::chrono::nanoseconds>(ts2 - ts1).count());
//         (void)e;
//     }
//     auto t1 = std::chrono::high_resolution_clock::now();

//     // Throughput
//     double seconds = std::chrono::duration<double>(t1 - t0).count();
//     double throughput = numQueries / seconds;
//     std::cout << "Throughput: " << throughput << " ops/sec\n";

//     // Latency histogram
//     std::sort(latencies.begin(), latencies.end());
//     auto p50 = latencies[numQueries * 50 / 100];
//     auto p95 = latencies[numQueries * 95 / 100];
//     auto p99 = latencies[numQueries * 99 / 100];

//     std::cout << "Latency (ns): p50=" << p50
//               << " p95=" << p95
//               << " p99=" << p99 << "\n";

//     return 0;
// }
#include <iostream>
#include <thread>
#include <vector>
#include <random>
#include <chrono>
#include <atomic>
#include <algorithm>

#include "../cache/range_cache.hpp"
#include "../util/profiler.hpp"

using namespace std::chrono;

using Key = uint64_t;
using Item = uint64_t;

int main(int argc, char** argv) {
    int threads = 4;
    int duration_sec = 5;
    if (argc > 1) threads = std::atoi(argv[1]);
    if (argc > 2) duration_sec = std::atoi(argv[2]);

    RangeCache<Key, Item> cache(1024 * 1024 * 64); // 64MB

    std::atomic<bool> stop{false};
    std::atomic<uint64_t> ops{0};

    std::vector<uint64_t> latencies;
    latencies.resize(threads * 100000);

    auto worker = [&](int tid) {
        std::mt19937_64 rng(tid + 12345);
        std::uniform_int_distribution<uint64_t> keyDist(1, 1000000);
        size_t idx = tid * 100000;
    while (!stop.load()) {
            uint64_t k = keyDist(rng);
            Key key = k;
            // 60% searches, 30% adds, 10% invalidates
            int op = k % 10;
            auto start = steady_clock::now();
            if (op < 6) {
                auto ep = cache.search(key);
                (void)ep;
            } else /*if (op < 9)*/ {
                Item it = k;
                cache.add(key, key, it);
            }
            // } else {
            //     // invalidate a random id if exists; use a small sample to
            //     // avoid a full snapshot traversal (matches RangeCache change).
            //     auto snap = cache.entriesSample(16, 1024);
            //     if (!snap.empty()) {
            //         auto n = snap[k % snap.size()];
            //         cache.invalidate(n);
            //     }
            // }
            auto end = steady_clock::now();
            latencies[idx++] = duration_cast<nanoseconds>(end - start).count();
            ops.fetch_add(1, std::memory_order_relaxed);
            if (idx >= (size_t)(tid * 100000 + 100000)) idx = tid * 100000;
        }
        // Publish this thread's TLS profiler stats before thread exit.
        Profiler::publish_thread_stats();
    };

    std::vector<std::thread> ths;
    for (int i = 0; i < threads; ++i) ths.emplace_back(worker, i);

    std::this_thread::sleep_for(std::chrono::seconds(duration_sec));
    stop.store(true);
    for (auto &t : ths) t.join();

    std::vector<uint64_t> all;
    for (auto v : latencies) if (v) all.push_back(v);
    if (all.empty()) {
        std::cout << "No ops recorded\n";
        return 0;
    }
    std::sort(all.begin(), all.end());
    auto p50 = all[all.size() * 50 / 100];
    auto p95 = all[all.size() * 95 / 100];
    auto p99 = all[all.size() * 99 / 100];
    std::cout << "threads="<<threads<<" ops="<<ops.load()
              <<" p50(ns)="<<p50<<" p95(ns)="<<p95<<" p99(ns)="<<p99<<"\n";

    // Publish per-thread stats and take a global snapshot from Profiler
    Profiler::publish_thread_stats();
    auto snap = Profiler::snapshot();
    std::cout << "Profiler snapshot:\n";
    for (auto &kv : snap) {
        const auto &name = kv.first;
        const auto &s = kv.second;
        std::cout << "  " << name << " count=" << s.count
                  << " total_ns=" << s.total_ns
                  << " avg_ns=" << (uint64_t)s.average_ns() << "\n";
    }
    return 0;
}
