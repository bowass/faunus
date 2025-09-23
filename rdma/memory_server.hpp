#pragma once
#include "rdma_simulation.hpp"

/**
 * @brief Simulated memory server providing RDMA access to a memory region.
 */
class MemoryServer {
public:
    /**
     * @brief Construct a MemoryServer with a given memory size.
     * @param memory_size Number of bytes in the memory region
     */
    MemoryServer(size_t memory_size);
    /**
     * @brief Get reference to the RDMA simulation interface.
     * @return Reference to RDMASimulation
     */
    RDMASimulation& get_rdma();
private:
    RDMASimulation rdma_;
};
