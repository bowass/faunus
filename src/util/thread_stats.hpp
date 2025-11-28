#pragma once

#include <array>
#include <vector>
#include <limits>
#include <cmath>
#include <cstdint>
#include "util/histogram_latency_sampler.hpp"
#include "rdma/rdma_simulation.hpp"

namespace stats {

/**
 * @brief Histogram recorder for counting discrete values (0-254, with overflow at 255)
 * Used for recording RTT counts, RDMA operation counts per B+Tree operation
 */
class DiscreteHistogram {
private:
    static constexpr size_t NUM_BUCKETS = 256;  // 0-254 + overflow
    std::array<uint64_t, NUM_BUCKETS> buckets_;
    uint64_t total_samples_;
    
public:
    DiscreteHistogram() : total_samples_(0) {
        buckets_.fill(0);
    }
    
    void record(uint32_t value) {
        size_t bucket_idx = std::min(static_cast<size_t>(value), NUM_BUCKETS - 1);
        buckets_[bucket_idx]++;
        total_samples_++;
    }
    
    uint64_t get_count(uint32_t value) const {
        size_t bucket_idx = std::min(static_cast<size_t>(value), NUM_BUCKETS - 1);
        return buckets_[bucket_idx];
    }
    
    uint64_t get_overflow_count() const {
        return buckets_[NUM_BUCKETS - 1];
    }
    
    uint64_t total_samples() const { return total_samples_; }
    
    double get_average() const {
        if (total_samples_ == 0) return 0.0;
        uint64_t sum = 0;
        for (size_t i = 0; i < NUM_BUCKETS - 1; ++i) {
            sum += i * buckets_[i];
        }
        // For overflow bucket, estimate as 255 (conservative)
        sum += 255 * buckets_[NUM_BUCKETS - 1];
        return static_cast<double>(sum) / total_samples_;
    }
    
    std::vector<std::pair<uint32_t, uint64_t>> get_distribution() const {
        std::vector<std::pair<uint32_t, uint64_t>> result;
        for (size_t i = 0; i < NUM_BUCKETS; ++i) {
            if (buckets_[i] > 0) {
                result.emplace_back(static_cast<uint32_t>(i), buckets_[i]);
            }
        }
        return result;
    }
    
    void reset() {
        buckets_.fill(0);
        total_samples_ = 0;
    }
};

/**
 * @brief Logarithmic histogram for byte counts (powers of 2: 1B, 2B, 4B, 8B, ..., up to GB range)
 * Used for recording total bytes read/written per B+Tree operation
 */
class LogarithmicByteHistogram {
private:
    static constexpr size_t NUM_BUCKETS = 32;  // Covers 1B to 4GB range
    std::array<uint64_t, NUM_BUCKETS> buckets_;
    uint64_t total_samples_;
    uint64_t total_bytes_;
    
    static size_t get_bucket_index(uint64_t bytes) {
        if (bytes == 0) return 0;
        // Find the highest bit set (log2)
        size_t bucket = 0;
        while (bytes > (1ULL << bucket) && bucket < NUM_BUCKETS - 1) {
            bucket++;
        }
        return bucket;
    }
    
    static uint64_t get_bucket_center(size_t bucket_idx) {
        if (bucket_idx == 0) return 0;
        return 1ULL << (bucket_idx - 1);
    }
    
public:
    LogarithmicByteHistogram() : total_samples_(0), total_bytes_(0) {
        buckets_.fill(0);
    }
    
    void record(uint64_t bytes) {
        size_t bucket_idx = get_bucket_index(bytes);
        buckets_[bucket_idx]++;
        total_samples_++;
        total_bytes_ += bytes;
    }
    
    uint64_t total_samples() const { return total_samples_; }
    uint64_t total_bytes() const { return total_bytes_; }
    
    double get_average_bytes() const {
        if (total_samples_ == 0) return 0.0;
        return static_cast<double>(total_bytes_) / total_samples_;
    }
    
    std::vector<std::pair<uint64_t, uint64_t>> get_distribution() const {
        std::vector<std::pair<uint64_t, uint64_t>> result;
        for (size_t i = 0; i < NUM_BUCKETS; ++i) {
            if (buckets_[i] > 0) {
                uint64_t value;
                if (i == 0) value = 0;
                else value = (1ULL << i);
                result.emplace_back(value, buckets_[i]);
            }
        }
        return result;
    }
    
    void reset() {
        buckets_.fill(0);
        total_samples_ = 0;
        total_bytes_ = 0;
    }
};

/**
 * @brief Cache hit/miss tracker
 */
class CacheStats {
private:
    uint64_t hits_;
    uint64_t misses_;
    
public:
    CacheStats() : hits_(0), misses_(0) {}
    
    void record_hit() { hits_++; }
    void record_miss() { misses_++; }
    
    uint64_t hits() const { return hits_; }
    uint64_t misses() const { return misses_; }
    uint64_t total_accesses() const { return hits_ + misses_; }
    
    double hit_rate() const {
        uint64_t total = total_accesses();
        return total > 0 ? static_cast<double>(hits_) / total : 0.0;
    }
    
    void reset() {
        hits_ = 0;
        misses_ = 0;
    }
};

} // namespace stats

/**
 * @brief Enhanced per-thread statistics for B+Tree operations with detailed RDMA metrics
 */
enum class OperationKind : uint8_t {
    Insert = 0,
    Read = 1,
    Update = 2,
    Delete = 3,
    Count
};

// Use histogram-based latency sampler for scalability
using LatencySampler = util::HistogramLatencySampler;

struct ThreadStats {
    struct OperationEntry {
        // Basic operation metrics
        uint64_t successes = 0;
        uint64_t failures = 0;
        LatencySampler latency_samples;  // For percentile calculation and averages
        
        // RDMA operation counts per B+Tree operation (histogram: 0-254 RTTs per operation)
        stats::DiscreteHistogram rtt_counts_per_op;
        
        // RDMA operation type counts per B+Tree operation
        std::array<stats::DiscreteHistogram, 4> rdma_op_counts_per_op; // READ, WRITE, CAS, FAA
        
        // Bytes transferred per B+Tree operation (logarithmic histogram)
        stats::LogarithmicByteHistogram bytes_read_per_op;
        stats::LogarithmicByteHistogram bytes_written_per_op;
        
        // Cache hit/miss statistics
        stats::CacheStats cache_stats;
        
        // Retry count histogram (0-254 retries per operation)
        stats::DiscreteHistogram retry_counts_per_op;
        
        // Local lock statistics (per-thread, no expensive atomics)
        uint64_t local_lock_acquisitions = 0;
        uint64_t local_lock_handovers = 0;
        
        void record_operation(double latency_us, bool success) {
            if (success) {
                successes++;
                latency_samples.add_sample(latency_us);
            } else {
                failures++;
            }
        }
        
        void record_rtt_count(uint32_t rtt_count) {
            rtt_counts_per_op.record(rtt_count);
        }
        
        void record_rdma_operation(RDMAOpType op_type, uint32_t count) {
            size_t type_idx = static_cast<size_t>(op_type);
            if (type_idx < rdma_op_counts_per_op.size()) {
                rdma_op_counts_per_op[type_idx].record(count);
            }
        }
        
        void record_bytes_read(uint64_t bytes) {
            bytes_read_per_op.record(bytes);
        }
        
        void record_bytes_written(uint64_t bytes) {
            bytes_written_per_op.record(bytes);
        }
        
        void record_cache_hit() {
            cache_stats.record_hit();
        }
        
        void record_cache_miss() {
            cache_stats.record_miss();
        }
        
        void record_retry_count(uint32_t retry_count) {
            retry_counts_per_op.record(retry_count);
        }
        
        void record_local_lock_acquisition(bool was_handover) {
            local_lock_acquisitions++;
            if (was_handover) {
                local_lock_handovers++;
            }
        }
        
        void reset() {
            successes = 0;
            failures = 0;
            latency_samples = LatencySampler();
            rtt_counts_per_op.reset();
            for (auto& hist : rdma_op_counts_per_op) {
                hist.reset();
            }
            bytes_read_per_op.reset();
            bytes_written_per_op.reset();
            cache_stats.reset();
            retry_counts_per_op.reset();
            local_lock_acquisitions = 0;
            local_lock_handovers = 0;
        }
    };

    std::array<OperationEntry, static_cast<size_t>(OperationKind::Count)> per_op{};

    // Helper methods for recording complete operations
    void record_operation(OperationKind kind, double latency_us, bool success) {
        per_op[static_cast<size_t>(kind)].record_operation(latency_us, success);
    }
    
    void record_rtt_count(OperationKind kind, uint32_t rtt_count) {
        per_op[static_cast<size_t>(kind)].record_rtt_count(rtt_count);
    }
    
    void record_rdma_operation(OperationKind kind, RDMAOpType op_type, uint32_t count) {
        per_op[static_cast<size_t>(kind)].record_rdma_operation(op_type, count);
    }
    
    void record_bytes_read(OperationKind kind, uint64_t bytes) {
        per_op[static_cast<size_t>(kind)].record_bytes_read(bytes);
    }
    
    void record_bytes_written(OperationKind kind, uint64_t bytes) {
        per_op[static_cast<size_t>(kind)].record_bytes_written(bytes);
    }
    
    void record_cache_hit(OperationKind kind) {
        per_op[static_cast<size_t>(kind)].record_cache_hit();
    }
    
    void record_cache_miss(OperationKind kind) {
        per_op[static_cast<size_t>(kind)].record_cache_miss();
    }
    
    void record_retry_count(OperationKind kind, uint32_t retry_count) {
        per_op[static_cast<size_t>(kind)].record_retry_count(retry_count);
    }
    
    void record_local_lock_acquisition(OperationKind kind, bool was_handover) {
        per_op[static_cast<size_t>(kind)].record_local_lock_acquisition(was_handover);
    }

    void reset() {
        for (auto& entry : per_op) {
            entry.reset();
        }
    }
};

/**
 * @brief Thread-local statistics tracker that automatically captures RDMA operations
 * 
 * This class should be used by B+Tree operations to track all relevant metrics.
 * It provides a simple interface that's called at the beginning and end of each operation.
 */
class ThreadStatsTracker {
private:
    ThreadStats& stats_;
    OperationKind current_op_;
    bool tracking_active_;
    
    // Temporary counters for current operation
    uint32_t rtt_count_;
    std::array<uint32_t, 4> rdma_op_counts_;  // READ, WRITE, CAS, FAA
    uint64_t bytes_read_;
    uint64_t bytes_written_;
    uint32_t retry_count_;
    
public:
    explicit ThreadStatsTracker(ThreadStats& stats) 
        : stats_(stats), tracking_active_(false), rtt_count_(0), 
          bytes_read_(0), bytes_written_(0), retry_count_(0) {
        rdma_op_counts_.fill(0);
    }
    
    // Call at the beginning of a B+Tree operation
    void begin_operation(OperationKind kind) {
        current_op_ = kind;
        tracking_active_ = true;
        rtt_count_ = 0;
        rdma_op_counts_.fill(0);
        bytes_read_ = 0;
        bytes_written_ = 0;
        retry_count_ = 0;
    }
    
    // Call at the end of a B+Tree operation
    void end_operation(double latency_us, bool success) {
        if (!tracking_active_) return;
        
        // Record the operation itself
        stats_.record_operation(current_op_, latency_us, success);
        
        // Record accumulated RDMA metrics
        stats_.record_rtt_count(current_op_, rtt_count_);
        for (size_t i = 0; i < rdma_op_counts_.size(); ++i) {
            if (rdma_op_counts_[i] > 0) {
                stats_.record_rdma_operation(current_op_, static_cast<RDMAOpType>(i), rdma_op_counts_[i]);
            }
        }
        stats_.record_bytes_read(current_op_, bytes_read_);
        stats_.record_bytes_written(current_op_, bytes_written_);
        stats_.record_retry_count(current_op_, retry_count_);
        
        tracking_active_ = false;
    }
    
    // Called by RDMA operations to update counters
    void record_rdma_op(const RDMAOp& op) {
        if (!tracking_active_) return;
        
        rtt_count_++;
        size_t type_idx = static_cast<size_t>(op.type);
        if (type_idx < rdma_op_counts_.size()) {
            rdma_op_counts_[type_idx]++;
        }
            
        if (op.type == RDMAOpType::READ) {
            bytes_read_ += op.op.read.bytes;
        } else if (op.type == RDMAOpType::WRITE) {
            bytes_written_ += op.op.write.bytes;
        }
    }

    void record_rdma_ops(const std::vector<RDMAOp>& ops) {
        if (!tracking_active_) return;
        
        rtt_count_++;
        for (auto& op : ops) {
            size_t type_idx = static_cast<size_t>(op.type);
            if (type_idx < rdma_op_counts_.size()) {
                rdma_op_counts_[type_idx]++;
            }
            
            if (op.type== RDMAOpType::READ) {
                bytes_read_ += op.op.read.bytes;
            } else if (op.type == RDMAOpType::WRITE) {
                bytes_written_ += op.op.write.bytes;
            }
        }
    }
    
    void record_cache_hit() {
        if (tracking_active_) {
            stats_.record_cache_hit(current_op_);
        }
    }
    
    void record_cache_miss() {
        if (tracking_active_) {
            stats_.record_cache_miss(current_op_);
        }
    }
    
    void record_retry() {
        if (tracking_active_) {
            retry_count_++;
        }
    }
    
    void record_local_lock_acquisition(bool was_handover) {
        if (tracking_active_) {
            stats_.record_local_lock_acquisition(current_op_, was_handover);
        }
    }
    
    ThreadStats& get_stats() { return stats_; }
};