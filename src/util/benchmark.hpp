#pragma once

#include <chrono>
#include <random>
#include <array>
#include <limits>
#include <vector>
#include <algorithm>
#include <tuple>
#include "rdma/compute_server.hpp"
#include "config/config.hpp"

namespace util {

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
 * 
 * Operation semantics:
 * - Insert: insert-or-update (upsert) - inserts new key or updates existing
 * - Read: read value for key
 * - Delete: delete key-value pair (currently not fully implemented)
 */
class OperationPicker {
public:
    explicit OperationPicker(const std::array<double, 3>& operation_mix) 
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
        return OperationKind::Delete;
    }

private:
    const std::array<double, 3>& operation_mix_;
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
        stats_.record_operation(kind, latency_us, success);
    }

    void reset() {
        stats_.reset();
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
    
    // Percentile statistics (calculated from histograms)
    double p50_latency_us = 0.0;
    double p95_latency_us = 0.0;
    double p99_latency_us = 0.0;
    size_t sample_count = 0;
};

/**
 * Percentile calculator utility.
 */
class PercentileCalculator {
public:
    /**
     * Calculate percentiles from a vector of latency samples.
     * @param samples Vector of latency values (will be sorted in-place)
     * @param percentiles Vector of percentile values (0.0 to 1.0)
     * @return Vector of percentile results in same order as input percentiles
     */
    static std::vector<double> calculate(std::vector<double>& samples, const std::vector<double>& percentiles) {
        std::vector<double> results;
        if (samples.empty()) {
            results.resize(percentiles.size(), 0.0);
            return results;
        }
        
        std::sort(samples.begin(), samples.end());
        
        for (double p : percentiles) {
            double index = p * (samples.size() - 1);
            size_t lower = static_cast<size_t>(index);
            size_t upper = std::min(lower + 1, samples.size() - 1);
            double fraction = index - lower;
            
            double result = samples[lower] + fraction * (samples[upper] - samples[lower]);
            results.push_back(result);
        }
        
        return results;
    }
    
    /**
     * Calculate P50, P95, P99 percentiles.
     * @param samples Vector of latency values (will be sorted in-place)
     * @return Tuple of (P50, P95, P99)
     */
    static std::tuple<double, double, double> calculate_standard(std::vector<double>& samples) {
        auto percentiles = calculate(samples, {0.50, 0.95, 0.99});
        return std::make_tuple(percentiles[0], percentiles[1], percentiles[2]);
    }
};

} // namespace util