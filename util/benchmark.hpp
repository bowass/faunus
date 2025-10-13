#pragma once

#include <chrono>
#include <random>
#include <array>
#include <limits>
#include "../rdma/compute_server.hpp"
#include "../config/faunus_config.hpp"

namespace faunus_util {

/**
 * Summary statistics for a worker thread's performance.
 */
struct WorkerSummary {
    size_t attempted = 0;
    size_t succeeded = 0;
    double elapsed_sec = 0.0;
    double attempted_throughput = 0.0;
    double succeeded_throughput = 0.0;
};

/**
 * Weighted operation picker based on configuration mix.
 */
class OperationPicker {
public:
    explicit OperationPicker(const std::array<double, 4>& operation_mix) 
        : operation_mix_(operation_mix) {}

    template <typename RNG>
    OperationKind weighted_pick(RNG& gen) const {
        std::uniform_real_distribution<double> dis(0.0, 1.0);
        double r = dis(gen);
        double cumulative = 0.0;
        
        cumulative += operation_mix_[0];
        if (r < cumulative) return OperationKind::Insert;
        cumulative += operation_mix_[1];
        if (r < cumulative) return OperationKind::Read;
        cumulative += operation_mix_[2];
        if (r < cumulative) return OperationKind::Update;
        return OperationKind::Delete;
    }

private:
    const std::array<double, 4>& operation_mix_;
};

/**
 * Helper for recording operation results with timing.
 */
class OperationRecorder {
public:
    explicit OperationRecorder(ThreadStats& stats) : stats_(stats) {}

    template <typename TimePoint>
    void record_result(OperationKind kind, const TimePoint& start, const TimePoint& end, bool success) {
        double latency_us = std::chrono::duration_cast<std::chrono::duration<double, std::micro>>(end - start).count();
        stats_.record(kind, latency_us, success);
    }

private:
    ThreadStats& stats_;
};

/**
 * Operation name string helper for logging/debugging.
 */
inline const char* operation_name(OperationKind kind) {
    switch (kind) {
        case OperationKind::Insert: return "insert";
        case OperationKind::Read: return "read";
        case OperationKind::Update: return "update";
        case OperationKind::Delete: return "delete";
        default: return "unknown";
    }
}

/**
 * Operation name by index helper for legacy compatibility.
 */
inline const char* operation_name(size_t idx) {
    return operation_name(static_cast<OperationKind>(idx));
}

/**
 * Aggregated operation statistics for summary reporting.
 */
struct AggregatedOpStats {
    size_t successes = 0;
    size_t failures = 0;
    double total_latency_us = 0.0;
    double min_latency_us = std::numeric_limits<double>::max();
    double max_latency_us = 0.0;
};

} // namespace faunus_util