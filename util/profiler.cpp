#include "profiler.hpp"

#ifdef ENABLE_PROFILING
#include <mutex>

namespace {
thread_local std::unordered_map<std::string, ProfileStats> tls_stats;
std::unordered_map<std::string, ProfileStats> global_stats;
std::mutex global_mutex;
}

Profiler::Scoped::~Scoped() {
    auto end = Clock::now();
    auto duration_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start_).count();
    Profiler::add_sample(name_, static_cast<uint64_t>(duration_ns));
}

void Profiler::add_sample(const char* name, uint64_t duration_ns) {
    auto& entry = tls_stats[std::string(name)];
    entry.count += 1;
    entry.total_ns += duration_ns;
}

void Profiler::publish_thread_stats() {
    if (tls_stats.empty()) return;
    std::lock_guard<std::mutex> lock(global_mutex);
    for (const auto& [name, stats] : tls_stats) {
        auto& aggregate = global_stats[name];
        aggregate.count += stats.count;
        aggregate.total_ns += stats.total_ns;
    }
    tls_stats.clear();
}

std::unordered_map<std::string, ProfileStats> Profiler::snapshot() {
    Profiler::publish_thread_stats();
    std::lock_guard<std::mutex> lock(global_mutex);
    return global_stats;
}

void Profiler::reset() {
    std::lock_guard<std::mutex> lock(global_mutex);
    global_stats.clear();
}

#endif // ENABLE_PROFILING
