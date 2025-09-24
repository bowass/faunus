#pragma once
#include <limits>
#include <vector>
#include <thread>
#include <functional>
#include "rdma_simulation.hpp"
#include "rdma_manager.hpp"
#include "local_allocator.hpp"

/**
 * @brief Per-thread statistics for RDMA operations.
 */
struct ThreadStats {
    size_t ops = 0;
    double latency_us = 0.0;       ///< Total latency in microseconds
    double min_latency_us = std::numeric_limits<double>::max(); ///< Minimum observed latency
    double max_latency_us = 0.0;   ///< Maximum observed latency
};

/**
 * @brief General compute server that runs user-defined worker tasks in parallel threads.
 */
class ComputeServer {
public:
    /**
     * @brief Worker function type: receives thread id, stats, and RDMA manager reference.
     */
    using WorkerFunc = std::function<void(int, ThreadStats&, RDMAManager&)>;
    /**
     * @brief Construct a ComputeServer with a given number of threads and worker function.
     * @param id Server identifier
     * @param num_threads Number of worker threads
     * @param rdma_mgr Reference to RDMA manager
     * @param worker Worker function to execute in each thread
     */
    ComputeServer(int id, int num_threads, RDMAManager& rdma_mgr, WorkerFunc worker,
                 std::unique_ptr<LocalAllocator> allocator);
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
    RDMAManager& rdma_mgr_;
    std::vector<std::thread> workers_;
    std::vector<ThreadStats> stats_;
    WorkerFunc worker_;
    std::unique_ptr<LocalAllocator> allocator_;
public:
    LocalAllocator* get_allocator() const { return allocator_.get(); }
};
