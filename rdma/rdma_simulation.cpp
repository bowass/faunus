#include "../rdma/rdma_simulation.hpp"
#include <cstring>
#include <cassert>
#include <iostream>

#include "../util/logging.hpp"

RDMASimulation::RDMASimulation(size_t memory_size)
    : memory_(memory_size, 0) {}


bool RDMASimulation::rdma_read(RDMAOp& op) {
    Profiler::Scoped scope("rdma.read");
    assert(op.type == RDMAOpType::READ);
    
    if (op.addr + op.op.read.bytes > memory_.size() || !op.op.read.buffer) {
        assert(false);
        return false;
    }
    
    std::copy(memory_.begin() + op.addr, memory_.begin() + op.addr + op.op.read.bytes, op.op.read.buffer);
    
    return true;
}


bool RDMASimulation::rdma_write(RDMAOp& op) {
    Profiler::Scoped scope("rdma.write");
    assert(op.type == RDMAOpType::WRITE);
    
    if (op.addr + op.op.write.bytes > memory_.size() || !op.op.write.buffer) {
        return false;
    }
    
    // std::cout << "RDMA_WRITE to " << std::hex << op.addr << " size " << std::dec << op.op.write.bytes << ", data: ";
    // for (size_t i = 0; i < std::min(op.op.write.bytes, size_t(32)); ++i) {
    //     std::cout << std::hex << static_cast<int>(op.op.write.buffer[i]) << " ";
    // }
    // std::cout << std::endl;
    std::copy(op.op.write.buffer, op.op.write.buffer + op.op.write.bytes, memory_.begin() + op.addr);
    // std::cout << "Done write." << std::endl;

    return true;
}


bool RDMASimulation::rdma_cas(RDMAOp& op) {
    Profiler::Scoped scope("rdma.cas");
    assert(op.type == RDMAOpType::CAS);

    // std::cout << "RDMA_CAS to " << std::hex << op.addr << ", expected: " << std::hex << *reinterpret_cast<uint64_t*>(op.op.cas.expected) << ", desired: " << std::hex << op.op.cas.desired << std::endl;
    if (op.addr + sizeof(uint64_t) > memory_.size()) {
        return false;
    }
    
    uint64_t* ptr = reinterpret_cast<uint64_t*>(&memory_[op.addr]);
    __atomic_compare_exchange_n(ptr, reinterpret_cast<void*>(op.op.cas.expected), op.op.cas.desired, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);

    return true;
}


uint64_t RDMASimulation::rdma_faa(RDMAOp& op) {
    Profiler::Scoped scope("rdma.faa");
    assert(op.type == RDMAOpType::FAA);
    
    uint64_t old = 0;
    if (op.addr + sizeof(uint64_t) <= memory_.size()) {
        uint64_t* ptr = reinterpret_cast<uint64_t*>(&memory_[op.addr]);
        old = __atomic_fetch_add(ptr, op.op.faa.increment, __ATOMIC_SEQ_CST);
    }
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
