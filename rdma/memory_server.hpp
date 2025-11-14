#pragma once
#include "rdma_simulation.hpp"
#include "../externals/concurrentqueue/concurrentqueue.h"
#include <unordered_set>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <atomic>
#include <utility>
#include <future>

/**
 * @brief Simulated memory server providing RDMA access to a memory region.
 */
class MemoryServer {
public:
    MemoryServer(size_t memory_size);
    ~MemoryServer();
    RDMASimulation& get_rdma();
    size_t allocate(size_t size);
    void free(size_t offset);
    // Queue allocation/free requests
    void enqueue_alloc(size_t size, std::promise<size_t>& promise);
    void enqueue_free(size_t offset);
private:
    RDMASimulation rdma_;
    size_t next_offset_ = 0;
    std::unordered_set<size_t> allocated_;
    std::mutex mu_;
    // Worker thread and queue
    std::thread worker_;
    moodycamel::ConcurrentQueue<std::pair<size_t, std::promise<size_t>*>> alloc_queue_;
    moodycamel::ConcurrentQueue<size_t> free_queue_;
    std::mutex queue_mu_;
    std::condition_variable queue_cv_;
    std::atomic<bool> stop_ = false;
    void worker_loop();
};
