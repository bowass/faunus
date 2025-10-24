#pragma once
#include "memory_server.hpp"
#include "rdma_simulation.hpp"
#include <vector>
#include <cstdint>
#include <any>
#include <typeinfo>
#include <cassert>
#include <array>
#include <atomic>
#include <memory>
#include <mutex>
#include <thread>
#include "../util/profiler.hpp"


/**
 * @brief RDMA manager for mapping global addresses and performing RDMA operations.
 */
class RDMAManager {
public:
    struct RDMAThreadStatsSnapshot {
        std::thread::id thread_id{};
        std::array<uint64_t, static_cast<size_t>(RDMAOpType::FAA) + 1> op_counts{};
        uint64_t total_rtt_ns = 0;
    };

    struct RDMAStats {
        std::array<uint64_t, static_cast<size_t>(RDMAOpType::FAA) + 1> op_counts{};
        uint64_t total_rtt_ns = 0;
        std::vector<RDMAThreadStatsSnapshot> per_thread;
    };

    // Allocate memory on a given server
    size_t allocate_on_server(std::uint8_t server_idx, size_t size) {
        return mem_servers_.at(server_idx)->allocate(size);
    }
public:
    /**
     * @brief Construct an RDMA manager with memory servers and configuration.
     * @param mem_servers Vector of memory server pointers
     * @param mem_per_server Memory size per server
     * @param base_rtt_ns Base round-trip time in nanoseconds
     */
    RDMAManager(const std::vector<std::shared_ptr<MemoryServer>>& mem_servers, size_t mem_per_server, uint64_t base_rtt_ns = 1000);
    // Perform a single RDMA operation
    /**
     * @brief Perform a single RDMA operation using the global address in RDMAOp.
     * @param op RDMAOp descriptor
     * @return true if successful, false otherwise
     */
    bool perform_op(RDMAOp& op);
    // Perform a batch of RDMA operations
    /**
     * @brief Perform a batch of RDMA operations using the global address in each RDMAOp.
     * @param ops Vector of RDMAOp references
     */
    bool perform_batch(std::vector<RDMAOp>& ops);
    RDMAStats collect_stats() const;
    void reset_stats();
    /**
     * @brief Get memory server and local address from global address.
     * @param gaddr Global address
     * @param local_addr Output: local address
     * @return Pointer to MemoryServer
     */
    std::shared_ptr<MemoryServer> get_server(const GlobalAddress& gaddr, size_t& local_addr);
private:
    std::vector<std::shared_ptr<MemoryServer>> mem_servers_;
    size_t mem_per_server_;
    const uint64_t base_rtt_ns_;
    static constexpr uint64_t SIMULATED_BW_BPS = 12500000000ULL; // 100Gbps
    struct RDMAThreadStats {
        RDMAThreadStats() : thread_id(std::this_thread::get_id()) {}
        std::thread::id thread_id;
        std::array<uint64_t, static_cast<size_t>(RDMAOpType::FAA) + 1> op_counts{};
        uint64_t total_rtt_ns{0};
    };
    RDMAThreadStats& ensure_thread_stats() const;
    mutable std::mutex stats_mutex_;
    mutable std::vector<std::unique_ptr<RDMAThreadStats>> thread_stats_;

    // Helper to acquire lock and measure wait time
    std::pair<std::unique_lock<std::mutex>, uint64_t> acquire_server_lock(const std::shared_ptr<MemoryServer>& server);
    // Helper for integer bandwidth delay calculation
    uint64_t calculate_bw_delay_ns(const RDMAOp& op) const;

    // Helper
    /**
     * @brief Execute RDMA operation on mapped memory server.
     * @param op RDMAOp descriptor
     * @return true if successful, false otherwise
     */
    bool execute_rdma(RDMAOp& op);
};

template<typename T>
bool rdma_read_object(RDMAManager& rdma_mgr, GlobalAddress gaddr, T& obj) {
    Profiler::Scoped scope("rdma.read_object");
    RDMAOp op{RDMAOpType::READ, gaddr};
    op.op.read.buffer = reinterpret_cast<uint8_t*>(&obj);
    op.op.read.bytes = sizeof(T);
    return rdma_mgr.perform_op(op);
}

template<typename T>
bool rdma_write_object(RDMAManager& rdma_mgr, GlobalAddress gaddr, const T& obj) {
    Profiler::Scoped scope("rdma.write_object");
    RDMAOp op{RDMAOpType::WRITE, gaddr};
    op.op.write.buffer = reinterpret_cast<const uint8_t*>(&obj);
    op.op.write.bytes = sizeof(T);
    return rdma_mgr.perform_op(op);
}

template<typename T>
bool rdma_read_batch(RDMAManager& rdma_mgr, std::vector<GlobalAddress> gaddrs, std::vector<T>& objs) {
    Profiler::Scoped scope("rdma.read_batch");
    std::vector<RDMAOp> ops;
    objs.resize(gaddrs.size());
    for (size_t i = 0; i < gaddrs.size(); ++i) {
        ops.push_back(RDMAOp{RDMAOpType::READ, gaddrs[i]});
        ops.back().op.read.buffer = reinterpret_cast<uint8_t*>(&objs[i]);
        ops.back().op.read.bytes = sizeof(T);
    }
    return rdma_mgr.perform_batch(ops);
}

bool rdma_try_acquire_lock(RDMAManager& rdma_mgr, GlobalAddress lock_address);

bool rdma_release_lock(RDMAManager& rdma_mgr, GlobalAddress lock_address);
