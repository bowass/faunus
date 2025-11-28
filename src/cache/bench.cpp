// Standalone benchmark for RangeCache performance characteristics.
// Build via `make cache_bench` or compile manually with the required include paths and skiplist dependency.

#include <iostream>
#include <vector>
#include <thread>
#include <atomic>
#include <random>
#include <chrono>
#include <cstring>
#include <cassert>
#include <getopt.h>

#include "range_cache.hpp"

#include "rdma/local_allocator.hpp"
#include "util/array_var.hpp"

using namespace std::chrono;

struct Config {
    unsigned threads = 8;
    unsigned writers = 8;
    double read_ratio = 0.5;      // fraction of operations that are reads
    unsigned duration_s = 10;
    size_t max_entries = 1<<14;   // RangeCache maxEntries
    unsigned sampling_ms = 1000;  // sample interval for throughput prints
};

static Config cfg;

static void print_usage(const char* prog) {
    std::cout << "Usage: " << prog << " [options]\n"
              << "  --threads N        total threads (<=64)\n"
              << "  --writers W        writer threads (others are readers)\n"
              << "  --duration S       seconds to run (default 10)\n"
              << "  --max_entries N    RangeCache maxEntries\n"
              << "  --read_ratio F     fraction of ops that are reads (0..1)\n"
              << "  --help\n";
}

struct Stats {
    std::atomic<uint64_t> ops{0};
    std::atomic<uint64_t> hits{0};
    std::atomic<uint64_t> misses{0};
    std::atomic<uint64_t> errors{0};
};

using Key = uint64_t;
using Value = ArrayVar<8>;

int main(int argc, char** argv) {
    static struct option long_options[] = {
        {"threads", required_argument, nullptr, 't'},
        {"writers", required_argument, nullptr, 'w'},
        {"duration", required_argument, nullptr, 'd'},
        {"max_entries", required_argument, nullptr, 'm'},
        {"read_ratio", required_argument, nullptr, 'r'},
        {"help", no_argument, nullptr, 'h'},
        {nullptr,0,nullptr,0}
    };

    int opt;
    while ((opt = getopt_long(argc, argv, "t:w:d:s:m:r:h", long_options, nullptr)) != -1) {
        switch (opt) {
            case 't': cfg.threads = std::stoul(optarg); break;
            case 'w': cfg.writers = std::stoul(optarg); break;
            case 'd': cfg.duration_s = std::stoul(optarg); break;
            case 'm': cfg.max_entries = std::stoul(optarg); break;
            case 'r': cfg.read_ratio = std::stod(optarg); break;
            case 'h': print_usage(argv[0]); return 0;
            default: print_usage(argv[0]); return 1;
        }
    }

    if (cfg.threads == 0 || cfg.writers > cfg.threads) {
        std::cerr << "Invalid threads/writers configuration\n";
        return 1;
    }
    if (cfg.threads > 64) cfg.threads = 64;

    std::cout << "Configuration: threads=" << cfg.threads
              << " writers=" << cfg.writers
              << " duration=" << cfg.duration_s << "s"
              << " max_entries=" << cfg.max_entries
              << " read_ratio=" << cfg.read_ratio
              << "\n";

    // instantiate RangeCache<int,int> (adjust template params if you used different types)
    RangeCache<Key, Value> cache(cfg.max_entries, 64, 8);

    // Shared control variables
    std::atomic<bool> start{false}, stop{false};
    std::vector<std::thread> threads;
    Stats stats;

    // Pre-generate a bunch of keys/ranges to use (to reduce allocation overhead)
    const int KEY_SPACE = 1<<30;
    const int RANGE_LEN = 256;

    // Each writer thread will produce a sequence id + per-thread counter to generate ranges
    // Reader threads pick uniformly random keys and validate returned entries.
    std::atomic<uint64_t> global_adds{0};

    auto worker = [&](unsigned tid, bool is_writer) {
        std::mt19937_64 rng((uint64_t)tid ^ std::random_device{}());
        std::uniform_int_distribution<Key> key_dist(0, KEY_SPACE-1);
        size_t local_ops = 0;
        size_t local_hits = 0;
        size_t local_misses = 0;
        size_t local_errors = 0;

        // wait for start
        while (!start.load(std::memory_order_acquire)) std::this_thread::yield();

        auto end_time = steady_clock::now() + seconds(cfg.duration_s);
        while (!stop.load(std::memory_order_acquire) && steady_clock::now() < end_time) {
            if (is_writer) {
                // writer behavior: add ranges; occasionally invalidate random keys
                int base = int(global_adds.fetch_add(1) % KEY_SPACE);
                int s = base;
                int e = base + RANGE_LEN;
                cache.add(s, e, base); // item=base for simple payload
                local_ops++;

                // occasionally remove a random entry we know (best-effort)
                if ((rng() & 0xff) == 0) {
                    // pick a random key and try to remove the entry found
                    Key k = key_dist(rng);
                    auto* ent = cache.search(k);
                    if (ent) {
                        // best-effort invalidate (may race)
                        cache.invalidate(ent);
                    }
                }
            } else {
                // reader behavior: perform searches
                Key k = key_dist(rng);
                auto* ent = cache.search(k);
                local_ops++;
                if (ent) {
                    // Validate that returned entry covers key (correctness check)
                    if (!(k >= ent->start && k < ent->end)) {
                        ++local_errors;
                    } else {
                        ++local_hits;
                    }
                } else ++local_misses;
            }

            // occasional short pause to let other threads interleave
            if ((rng() & 0x3f) == 0) std::this_thread::sleep_for(std::chrono::microseconds(1));
        }
        stats.ops.fetch_add(local_ops, std::memory_order_relaxed);
        stats.hits.fetch_add(local_hits, std::memory_order_relaxed);
        stats.misses.fetch_add(local_misses, std::memory_order_relaxed);
        stats.errors.fetch_add(local_errors, std::memory_order_relaxed);
    };

    // spawn threads
    for (unsigned i = 0; i < cfg.threads; ++i) {
        bool is_writer = (i < cfg.writers);
        threads.emplace_back(worker, i, is_writer);
    }

    // reporter thread prints periodic throughput & some basic stats
    std::thread reporter([&](){
        start.store(true, std::memory_order_release);
        auto last_ops = uint64_t(0);
        auto last_time = steady_clock::now();
        for (unsigned t = 0; t < cfg.duration_s; ++t) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
            uint64_t cur_ops = stats.ops.load(std::memory_order_relaxed);
            auto now = steady_clock::now();
            double sec = duration<double>(now - last_time).count();
            uint64_t ops_delta = cur_ops - last_ops;
            double ops_per_s = ops_delta / sec;
            last_ops = cur_ops;
            last_time = now;
            std::cout << "[report] ops/sec=" << ops_per_s
                      << " total_ops=" << cur_ops
                      << " hits=" << stats.hits.load() << " misses=" << stats.misses.load()
                      << " errors=" << stats.errors.load()
                      << '\n';
        }
        stop.store(true, std::memory_order_release);
    });

    // wait for workers
    for (auto &th : threads) if (th.joinable()) th.join();
    if (reporter.joinable()) reporter.join();

    // Final totals
    uint64_t total_ops = stats.ops.load();
    uint64_t total_hits = stats.hits.load();
    uint64_t total_misses = stats.misses.load();
    uint64_t total_errors = stats.errors.load();

    std::cout << "==== Summary ====\n";
    std::cout << "Duration(s): " << cfg.duration_s << "\n";
    std::cout << "Threads: " << cfg.threads << " writers: " << cfg.writers << "\n";
    std::cout << "Total ops: " << total_ops << " ops/s ~ " << (total_ops / double(cfg.duration_s)) << "\n";
    std::cout << "Hits: " << total_hits << " Misses: " << total_misses << " Errors: " << total_errors << "\n";

    if (total_errors > 0) {
        std::cerr << "Correctness errors detected! Check reclamation / traversal epoch pinning.\n";
        return 2;
    }
    std::cout << "No correctness errors detected.\n";
    return 0;
}
