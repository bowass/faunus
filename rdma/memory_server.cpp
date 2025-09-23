#include "../include/memory_server.hpp"

MemoryServer::MemoryServer(size_t memory_size)
    : rdma_(memory_size) {}

RDMASimulation& MemoryServer::get_rdma() {
    return rdma_;
}
