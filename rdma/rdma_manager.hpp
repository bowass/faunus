
#pragma once
#include "memory_server.hpp"
#include "rdma_simulation.hpp"
#include <vector>
#include <cstdint>


/**
 * @brief RDMA manager for mapping global addresses and performing RDMA operations.
 */
class RDMAManager {
public:
    /**
     * @brief Construct an RDMA manager with memory servers and configuration.
     * @param mem_servers Vector of memory server pointers
     * @param mem_per_server Memory size per server
     * @param base_rtt_us Base round-trip time in microseconds
     */
    RDMAManager(const std::vector<MemoryServer*>& mem_servers, size_t mem_per_server, double base_rtt_us = 1.0);
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
    void perform_batch(std::vector<RDMAOp*>& ops);
    /**
     * @brief Get memory server and local address from global address.
     * @param gaddr Global address
     * @param local_addr Output: local address
     * @return Pointer to MemoryServer
     */
    MemoryServer* get_server(const GlobalAddress& gaddr, size_t& local_addr);
private:
    std::vector<MemoryServer*> mem_servers_;
    size_t mem_per_server_;
    double base_rtt_us_;
    // Helper
    /**
     * @brief Execute RDMA operation on mapped memory server.
     * @param op RDMAOp descriptor
     * @return true if successful, false otherwise
     */
    bool execute_rdma(RDMAOp& op);
};
