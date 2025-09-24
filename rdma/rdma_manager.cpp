#include "../rdma/rdma_manager.hpp"
#include <thread>
#include <chrono>
#include <thread>
#include <chrono>
#include <iostream>

// Private helper to execute mapped RDMA operation
bool RDMAManager::execute_rdma(RDMAOp& op) {
    // std::cout << "type: " << static_cast<int>(op.type) << " gaddr: " << static_cast<uint64_t>(op.addr) << std::endl;
    size_t local_addr;
    MemoryServer* server = get_server(op.addr, local_addr);
    if (!server) return false;
    RDMAOp local_op = op;
    local_op.addr = local_addr;
    bool result = false;
    switch (op.type) {
        case RDMAOpType::READ:
            result = server->get_rdma().rdma_read(local_op);
            break;
        case RDMAOpType::WRITE:
            result = server->get_rdma().rdma_write(local_op);
            break;
        case RDMAOpType::CAS:
            result = server->get_rdma().rdma_cas(local_op);
            break;
        case RDMAOpType::FAA:
            server->get_rdma().rdma_faa(local_op);
            result = true;
            break;
    }
    op.end_time = local_op.end_time;
    return result;
}


RDMAManager::RDMAManager(const std::vector<MemoryServer*>& mem_servers, size_t mem_per_server, double base_rtt_us)
    : mem_servers_(mem_servers), mem_per_server_(mem_per_server), base_rtt_us_(base_rtt_us) {}

MemoryServer* RDMAManager::get_server(const GlobalAddress& gaddr, size_t& local_addr) {
    uint8_t server_id = gaddr.server_index();
    local_addr = gaddr.offset();
    if (server_id < mem_servers_.size()) {
        return mem_servers_[server_id];
    }
    return nullptr;
}


bool RDMAManager::perform_op(RDMAOp& op) {
    std::this_thread::sleep_for(std::chrono::microseconds(static_cast<int64_t>(base_rtt_us_ / 2)));
    op.start_time = std::chrono::high_resolution_clock::now();
    bool result = execute_rdma(op);
    std::this_thread::sleep_for(std::chrono::microseconds(static_cast<int64_t>(base_rtt_us_ / 2)));
    op.end_time = std::chrono::high_resolution_clock::now();
    return result;
}


void RDMAManager::perform_batch(std::vector<RDMAOp*>& ops) {
    std::this_thread::sleep_for(std::chrono::microseconds(static_cast<int64_t>(base_rtt_us_ / 2)));
    auto batch_start = std::chrono::high_resolution_clock::now();
    for (auto* op : ops) {
        op->start_time = batch_start;
        execute_rdma(*op);
    }
    std::this_thread::sleep_for(std::chrono::microseconds(static_cast<int64_t>(base_rtt_us_ / 2)));
    auto batch_end = std::chrono::high_resolution_clock::now();
    for (auto* op : ops) {
        op->end_time = batch_end;
    }
}
