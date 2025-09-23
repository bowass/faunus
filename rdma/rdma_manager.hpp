
#pragma once
#include "memory_server.hpp"
#include "rdma_simulation.hpp"
#include <vector>
#include <cstdint>

/**
 * @brief Global address for mapping to memory servers and offsets.
 */
struct GlobalAddress {
    std::uint64_t raw;
    GlobalAddress(std::uint8_t index, std::uint64_t off)
        : raw((static_cast<std::uint64_t>(index) << 56) | (off & 0x00FFFFFFFFFFFFFFULL)) {}
    GlobalAddress(std::uint64_t address) : raw(address) {}
    std::uint8_t server_index() const { return static_cast<std::uint8_t>(raw >> 56); }
    std::uint64_t offset() const { return raw & 0x00FFFFFFFFFFFFFFULL; }
};

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
     * @brief Perform a single RDMA operation at a global address.
     * @param op RDMAOp descriptor
     * @param gaddr Global address
     * @return true if successful, false otherwise
     */
    bool perform_op(RDMAOp& op, const GlobalAddress& gaddr);
    // Perform a batch of RDMA operations
    /**
     * @brief Perform a batch of RDMA operations at global addresses.
     * @param ops Vector of (RDMAOp, GlobalAddress) pairs
     */
    void perform_batch(std::vector<std::pair<RDMAOp&, GlobalAddress>>& ops);
private:
    std::vector<MemoryServer*> mem_servers_;
    size_t mem_per_server_;
    double base_rtt_us_;
    // Helper
    /**
     * @brief Get memory server and local address from global address.
     * @param gaddr Global address
     * @param local_addr Output: local address
     * @return Pointer to MemoryServer
     */
    MemoryServer* get_server(const GlobalAddress& gaddr, size_t& local_addr);
    /**
     * @brief Execute RDMA operation on mapped memory server.
     * @param op RDMAOp descriptor
     * @param gaddr Global address
     * @return true if successful, false otherwise
     */
    bool execute_rdma(RDMAOp& op, const GlobalAddress& gaddr);
};
