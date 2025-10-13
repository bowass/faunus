#pragma once
#include <atomic>
#include <cstdint>
#include <vector>
#include <chrono>
#include <mutex>
#include <thread>
#include <functional>

#include "global_address.hpp"

#include "../util/profiler.hpp"

// Fast timing utilities to reduce chrono overhead
namespace rdma_timing {
    // Use CPU cycle counter for much faster timing (x86/x64 only)
    inline uint64_t fast_timestamp() {
        #if defined(__x86_64__) || defined(_M_X64) || defined(__i386) || defined(_M_IX86)
            return __builtin_ia32_rdtsc();
        #else
            // Fallback to nanoseconds since epoch for other architectures
            auto now = std::chrono::high_resolution_clock::now();
            return std::chrono::duration_cast<std::chrono::nanoseconds>(now.time_since_epoch()).count();
        #endif
    }
    
    // Convert cycles to nanoseconds (approximate - depends on CPU frequency)
    // For simulation purposes, we can use a rough conversion or disable if not needed
    inline double cycles_to_ns(uint64_t cycles, double cpu_ghz = 3.0) {
        return static_cast<double>(cycles) / cpu_ghz;
    }
    
    // Check if high-precision timing is enabled (can be disabled for performance)
    inline bool timing_enabled() {
        // Can be controlled by environment variable or compile flag
        #ifdef RDMA_NO_TIMING
            return false; // Compile-time disable for maximum performance
        #else
            static bool enabled = std::getenv("RDMA_NO_TIMING") == nullptr; 
            return enabled;
        #endif
    }
    
    // Utility to disable timing at runtime for performance testing
    void set_timing_enabled(bool enabled);
}

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
    // Use faster CPU cycle counter instead of chrono for better performance
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
            uint64_t expected;
            uint64_t desired;
        } cas;
        struct { // For FAA
            int64_t increment;
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
