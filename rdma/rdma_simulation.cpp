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
        // LOG_ERROR("Read failed: addr=" << op.addr << " bytes=" << op.op.read.bytes << " memsize=" << memory_.size());
        assert(false);
        return false;
    }
    
    // LOG_DEBUG("Writing real addresses: [" << static_cast<void*>(memory_.data() + op.addr) << ", " << static_cast<void*>(memory_.data() + op.addr + op.op.read.bytes) << ") to buffer at " << static_cast<void*>(op.op.read.buffer));
    std::copy(memory_.begin() + op.addr, memory_.begin() + op.addr + op.op.read.bytes, op.op.read.buffer);
    
    return true;
}


bool RDMASimulation::rdma_write(RDMAOp& op) {
    Profiler::Scoped scope("rdma.write");
    assert(op.type == RDMAOpType::WRITE);
    
    if (op.addr + op.op.write.bytes > memory_.size() || !op.op.write.buffer) {
        // LOG_ERROR("Write failed: addr=" << op.addr << " bytes=" << op.op.write.bytes << " memsize=" << memory_.size());
        return false;
    }
    
    // LOG_DEBUG("Writing buffer at " << static_cast<const void*>(op.op.write.buffer) << " to real addresses: [" << static_cast<void*>(memory_.data() + op.addr) << ", " << static_cast<void*>(memory_.data() + op.addr + op.op.write.bytes) << ")");
    std::copy(op.op.write.buffer, op.op.write.buffer + op.op.write.bytes, memory_.begin() + op.addr);
    
    return true;
}


bool RDMASimulation::rdma_cas(RDMAOp& op) {
    Profiler::Scoped scope("rdma.cas");
    assert(op.type == RDMAOpType::CAS);
    
    // std::this_thread::sleep_for(std::chrono::microseconds(config::CAS_DELAY_US));
    bool result = false;
    if (op.addr + sizeof(uint64_t) <= memory_.size()) {
        result = true;
        uint64_t* ptr = reinterpret_cast<uint64_t*>(&memory_[op.addr]);
        uint64_t expected = *reinterpret_cast<const uint64_t*>(op.op.cas.expected);
        // LOG_DEBUG("CAS at addr " << std::hex << op.addr << " expected " << expected << " desired " << op.op.cas.desired << std::dec);
        // LOG_WARN("CAS at addr " << std::hex << ptr << " expected " << reinterpret_cast<uint64_t*>(expected) << " desired " << reinterpret_cast<uint64_t*>(op.op.cas.desired) << std::dec);
        __atomic_compare_exchange_n(ptr, &expected, op.op.cas.desired, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
        *reinterpret_cast<uint64_t*>(op.op.cas.expected) = expected;
    }
    
    if (!result) {
        // LOG_ERROR("CAS failed at addr " << std::hex << op.addr << " expected " << *reinterpret_cast<const uint64_t*>(op.op.cas.expected) << " desired " << op.op.cas.desired << std::dec);
    }
    return result;
}


uint64_t RDMASimulation::rdma_faa(RDMAOp& op) {
    Profiler::Scoped scope("rdma.faa");
    assert(op.type == RDMAOpType::FAA);
    
    uint64_t old = 0;
    if (op.addr + sizeof(uint64_t) <= memory_.size()) {
        uint64_t* ptr = reinterpret_cast<uint64_t*>(&memory_[op.addr]);
        // LOG_WARN("FAA at addr " << std::hex << ptr << " increment " << op.op.faa.increment << std::dec << std::endl);
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
