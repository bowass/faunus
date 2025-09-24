#pragma once
#include "memory_server.hpp"
#include "rdma_manager.hpp"
#include <queue>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <atomic>
#include <future>
#include <vector>
#include <unordered_map>
#include "local_allocator.hpp"

/**
 * @brief Simulated RPC Allocator communicating with memory servers via message queues.
 */
class RPCAllocator : public Allocator {
public:
    RPCAllocator(std::vector<MemoryServer*>& servers)
        : servers_(servers) {}
    // Allocate a chunk, returns global address offset or -1 if none available
    int64_t allocate(size_t chunk_size) override {
        size_t server_idx = pick_server(chunk_size);
        std::promise<size_t> promise;
        std::future<size_t> fut = promise.get_future();
        servers_[server_idx]->enqueue_alloc(chunk_size, promise);
        size_t offset = fut.get();
        GlobalAddress gaddr(server_idx, offset);
        return gaddr.raw;
    }
    void free(size_t chunk_size, int64_t offset) override {
        size_t server_idx = extract_server(offset);
        size_t local_offset = GlobalAddress(offset).offset();
        servers_[server_idx]->enqueue_free(local_offset);
    }
private:
    std::vector<MemoryServer*>& servers_;
    size_t pick_server(size_t chunk_size) {
        // Simple round-robin or hash
        static std::atomic<size_t> rr{0};
        return rr++ % servers_.size();
    }
    size_t extract_server(int64_t offset) {
        // Extract server index from GlobalAddress
        GlobalAddress gaddr(offset);
        return gaddr.server_index();
    }
};
