#pragma once
#include <limits>
#include <vector>
#include <thread>
#include <functional>
#include <array>
#include "rdma_simulation.hpp"
#include "rdma_manager.hpp"
#include "local_allocator.hpp"

/**
 * @brief Per-thread statistics for RDMA operations.
 */
enum class OperationKind : uint8_t {
    Insert = 0,
    Read = 1,
    Update = 2,
    Delete = 3,
    Count
};

struct ThreadStats {
    struct Entry {
        size_t successes = 0;
        size_t failures = 0;
        double total_latency_us = 0.0;
        double min_latency_us = std::numeric_limits<double>::max();
        double max_latency_us = 0.0;

        void record(double latency_us, bool success) {
            if (success) {
                successes++;
                total_latency_us += latency_us;
                if (latency_us < min_latency_us) min_latency_us = latency_us;
                if (latency_us > max_latency_us) max_latency_us = latency_us;
            } else {
                failures++;
            }
        }
    };

    std::array<Entry, static_cast<size_t>(OperationKind::Count)> per_op{};

    void record(OperationKind kind, double latency_us, bool success) {
        per_op[static_cast<size_t>(kind)].record(latency_us, success);
    }
};

/**
 * @brief General compute server that runs user-defined worker tasks in parallel threads.
 */
class ComputeServer {
public:
    /**
     * @brief Worker function type: receives thread id, stats, RDMA manager and allocator reference.
     */
    using WorkerFunc = std::function<void(int, ThreadStats&, std::shared_ptr<RDMAManager> , std::shared_ptr<LocalAllocator>)>;
    /**
     * @brief Construct a ComputeServer with a given number of threads and worker function.
     * @param id Server identifier
     * @param num_threads Number of worker threads
     * @param rdma_mgr Reference to RDMA manager
     * @param allocator Local allocator
     * @param worker Worker function to execute in each thread
     */
    ComputeServer(int id, int num_threads, std::shared_ptr<RDMAManager> rdma_mgr, std::shared_ptr<LocalAllocator> allocator, WorkerFunc worker);
    /**
     * @brief Start all worker threads.
     */
    void start();
    /**
     * @brief Join all worker threads.
     */
    void join();
    // Metrics
    /**
     * @brief Get per-thread statistics.
     * @return Vector of ThreadStats
     */
    const std::vector<ThreadStats>& get_thread_stats() const { return stats_; }
private:
    int id_;
    int num_threads_;
    std::shared_ptr<RDMAManager> rdma_mgr_;
    std::shared_ptr<LocalAllocator> allocator_;
    std::vector<std::thread> workers_;
    std::vector<ThreadStats> stats_;
    WorkerFunc worker_;
public:
    LocalAllocator* get_allocator() const { return allocator_.get(); }
};
