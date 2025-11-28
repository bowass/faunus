#pragma once
#include <array>
#include <vector>
#include <algorithm>
#include <cmath>

namespace util {

/**
 * @brief Efficient histogram-based latency sampler for high-scale simulations.
 * 
 * Uses logarithmic buckets to capture wide latency ranges with bounded memory.
 * Memory usage: ~2KB per sampler vs ~8MB for raw sample storage.
 * 
 * THREAD-LOCAL DESIGN: This class is designed to be used per-thread only.
 * Each thread should have its own instance - no atomic operations needed.
 */
class HistogramLatencySampler {
private:
    // Logarithmic buckets: 0-1μs, 1-2μs, 2-4μs, 4-8μs, ..., up to ~1s
    static constexpr size_t NUM_BUCKETS = 32;
    static constexpr double MIN_LATENCY_US = 0.5;  // Minimum bucket size
    static constexpr double LOG_BASE = 2.0;
    
    std::array<uint64_t, NUM_BUCKETS> buckets_;
    uint64_t total_samples_;
    uint64_t overflow_samples_;  // Samples > max bucket
    
    static size_t get_bucket_index(double latency_us) {
        if (latency_us < MIN_LATENCY_US) return 0;
        
        double log_val = std::log(latency_us / MIN_LATENCY_US) / std::log(LOG_BASE);
        size_t bucket = static_cast<size_t>(std::floor(log_val));
        return std::min(bucket, NUM_BUCKETS - 1);
    }
    
    static double get_bucket_center(size_t bucket_idx) {
        if (bucket_idx == 0) return MIN_LATENCY_US / 2.0;
        return MIN_LATENCY_US * std::pow(LOG_BASE, bucket_idx + 0.5);
    }

public:
    HistogramLatencySampler() : total_samples_(0), overflow_samples_(0) {
        buckets_.fill(0);
    }
    
    // Assignment operator
    HistogramLatencySampler& operator=(const HistogramLatencySampler& other) {
        if (this != &other) {
            total_samples_ = other.total_samples_;
            overflow_samples_ = other.overflow_samples_;
            buckets_ = other.buckets_;
        }
        return *this;
    }
    
    // Copy constructor
    HistogramLatencySampler(const HistogramLatencySampler& other) : 
        buckets_(other.buckets_),
        total_samples_(other.total_samples_), 
        overflow_samples_(other.overflow_samples_) {
    }
    
    void add_sample(double latency_us) {
        size_t bucket_idx = get_bucket_index(latency_us);
        if (bucket_idx == NUM_BUCKETS - 1 && latency_us > get_bucket_upper_bound(bucket_idx)) {
            overflow_samples_++;
        } else {
            buckets_[bucket_idx]++;
        }
        total_samples_++;
    }
    
    /**
     * @brief Calculate percentile using histogram approximation.
     * @param percentile Value between 0.0 and 1.0
     * @return Approximate percentile latency in microseconds
     */
    double calculate_percentile(double percentile) const {
        uint64_t total = total_samples_;
        if (total == 0) return 0.0;
        
        uint64_t target_rank = static_cast<uint64_t>(percentile * total);
        uint64_t cumulative = 0;
        
        for (size_t i = 0; i < NUM_BUCKETS; ++i) {
            uint64_t bucket_count = buckets_[i];
            if (cumulative + bucket_count >= target_rank) {
                // Linear interpolation within bucket
                double bucket_lower = get_bucket_lower_bound(i);
                double bucket_upper = get_bucket_upper_bound(i);
                double fraction = (double)(target_rank - cumulative) / bucket_count;
                return bucket_lower + fraction * (bucket_upper - bucket_lower);
            }
            cumulative += bucket_count;
        }
        
        // Handle overflow case (percentile in overflow bucket)
        return get_bucket_upper_bound(NUM_BUCKETS - 1) * 2.0;  // Estimate
    }
    
    std::tuple<double, double, double> calculate_standard_percentiles() const {
        return {calculate_percentile(0.50), calculate_percentile(0.95), calculate_percentile(0.99)};
    }
    
    size_t sample_count() const { return total_samples_; }
    bool has_samples() const { return total_samples_ > 0; }
    
    // Get histogram data for detailed analysis
    std::vector<std::pair<double, uint64_t>> get_histogram() const {
        std::vector<std::pair<double, uint64_t>> result;
        for (size_t i = 0; i < NUM_BUCKETS; ++i) {
            uint64_t count = buckets_[i];
            if (count > 0) {
                result.emplace_back(get_bucket_center(i), count);
            }
        }
        return result;
    }
    
private:
    static double get_bucket_lower_bound(size_t bucket_idx) {
        if (bucket_idx == 0) return 0.0;
        return MIN_LATENCY_US * std::pow(LOG_BASE, bucket_idx - 1);
    }
    
    static double get_bucket_upper_bound(size_t bucket_idx) {
        return MIN_LATENCY_US * std::pow(LOG_BASE, bucket_idx);
    }
};

} // namespace util