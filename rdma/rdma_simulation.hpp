#pragma once
#include <atomic>
#include <cstdint>
#include <vector>
#include <chrono>
#include <mutex>
#include <thread>
#include <functional>

#include "global_address.hpp"

// RDMA operation types
/**
 * @brief Types of RDMA operations supported by the simulation.
 */
enum class RDMAOpType { READ, WRITE, CAS, FAA };

// RDMA operation descriptor
/**
 * @brief Descriptor for an RDMA operation, including type, address, and operation-specific data.
 */
struct RDMAOp {
    std::chrono::high_resolution_clock::time_point start_time; ///< Operation start timestamp
    std::chrono::high_resolution_clock::time_point end_time;   ///< Operation end timestamp
    RDMAOpType type;
    GlobalAddress addr;
    union {
        struct { // For READ
            uint8_t* buffer;
            size_t bytes;
        } read;
        struct { // For WRITE
            const uint8_t* buffer;
            size_t bytes;
        } write;
        struct { // For CAS
            std::uint64_t expected;
            std::uint64_t desired;
        } cas;
        struct { // For FAA
            std::uint64_t increment;
        } faa;
    } op;
};

// RDMA simulation interface
/**
 * @brief Simulates RDMA operations on a byte-addressable memory region.
 *
 * Provides atomic and non-atomic RDMA verbs, including read, write, compare-and-swap (CAS), and fetch-and-add (FAA).
 * Supports operation coalescing for batch execution.
 */
class RDMASimulation {
public:
    /**
     * @brief Construct a new RDMASimulation object with a given memory size.
     * @param memory_size Number of bytes in the simulated memory region.
     */
    RDMASimulation(size_t memory_size);
    // Simulate RDMA operations
    /**
     * @brief Simulate an RDMA READ operation.
     * @param op RDMAOp descriptor (type must be READ)
     * @return true if successful, false otherwise
     */
    bool rdma_read(RDMAOp& op);
    /**
     * @brief Simulate an RDMA WRITE operation.
     * @param op RDMAOp descriptor (type must be WRITE)
     * @return true if successful, false otherwise
     */
    bool rdma_write(RDMAOp& op);
    /**
     * @brief Simulate an RDMA CAS (compare-and-swap) operation.
     * @param op RDMAOp descriptor (type must be CAS)
     * @return true if successful, false otherwise
     */
    bool rdma_cas(RDMAOp& op);
    /**
     * @brief Simulate an RDMA FAA (fetch-and-add) operation.
     * @param op RDMAOp descriptor (type must be FAA)
     * @return previous value at address
     */
    std::uint64_t rdma_faa(RDMAOp& op);
    // Coalescing support
    /**
     * @brief Coalesce and execute a batch of RDMA operations.
     * @param ops Vector of RDMAOp descriptors
     */
    void coalesce_ops(const std::vector<RDMAOp>& ops);
private:
    std::vector<uint8_t> memory_;
};
