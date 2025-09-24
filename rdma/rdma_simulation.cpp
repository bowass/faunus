

#include "../rdma/rdma_simulation.hpp"
#include <cstring>
#include <cassert>
#include <iostream>

RDMASimulation::RDMASimulation(size_t memory_size)
    : memory_(memory_size, 0) {}


bool RDMASimulation::rdma_read(RDMAOp& op) {
    assert(op.type == RDMAOpType::READ);
    op.start_time = std::chrono::high_resolution_clock::now();
    if (op.addr + op.op.read.bytes > memory_.size() || !op.op.read.buffer) {
        op.end_time = std::chrono::high_resolution_clock::now();
        return false;
    }
    std::copy(memory_.begin() + op.addr, memory_.begin() + op.addr + op.op.read.bytes, op.op.read.buffer);
    op.end_time = std::chrono::high_resolution_clock::now();
    return true;
}


bool RDMASimulation::rdma_write(RDMAOp& op) {
    assert(op.type == RDMAOpType::WRITE);
    op.start_time = std::chrono::high_resolution_clock::now();
    if (op.addr + op.op.write.bytes > memory_.size() || !op.op.write.buffer) {
        op.end_time = std::chrono::high_resolution_clock::now();
        std::cout << "Write failed: addr=" << op.addr << " bytes=" << op.op.write.bytes << " memsize=" << memory_.size() << std::endl;
        return false;
    }
    std::copy(op.op.write.buffer, op.op.write.buffer + op.op.write.bytes, memory_.begin() + op.addr);
    op.end_time = std::chrono::high_resolution_clock::now();
    return true;
}


bool RDMASimulation::rdma_cas(RDMAOp& op) {
    assert(op.type == RDMAOpType::CAS);
    op.start_time = std::chrono::high_resolution_clock::now();
    // std::this_thread::sleep_for(std::chrono::microseconds(faunus_config::CAS_DELAY_US));
    bool result = false;
    if (op.addr + sizeof(uint64_t) <= memory_.size()) {
        uint64_t* ptr = reinterpret_cast<uint64_t*>(&memory_[op.addr]);
        uint64_t expected = op.op.cas.expected;
        result = (__atomic_compare_exchange_n(ptr, &expected, op.op.cas.desired, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST));
    }
    op.end_time = std::chrono::high_resolution_clock::now();
    return result;
}


uint64_t RDMASimulation::rdma_faa(RDMAOp& op) {
    assert(op.type == RDMAOpType::FAA);
    op.start_time = std::chrono::high_resolution_clock::now();
    uint64_t old = 0;
    if (op.addr + sizeof(uint64_t) <= memory_.size()) {
        uint64_t* ptr = reinterpret_cast<uint64_t*>(&memory_[op.addr]);
        old = __atomic_fetch_add(ptr, op.op.faa.increment, __ATOMIC_SEQ_CST);
    }
    op.end_time = std::chrono::high_resolution_clock::now();
    return old;
}


void RDMASimulation::coalesce_ops(const std::vector<RDMAOp>& ops) {
    for (auto& op : const_cast<std::vector<RDMAOp>&>(ops)) {
        switch (op.type) {
            case RDMAOpType::READ:
                rdma_read(op);
                break;
            case RDMAOpType::WRITE:
                rdma_write(op);
                break;
            case RDMAOpType::CAS:
                rdma_cas(op);
                break;
            case RDMAOpType::FAA:
                rdma_faa(op);
                break;
        }
    }
}
