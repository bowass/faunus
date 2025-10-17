#include "../rdma/rdma_manager.hpp"
#include <chrono>
#include <iostream>
#include <thread>
#include <unordered_map>

#include "../util/logging.hpp"
#include "../util/precise_sleep.hpp"

// Private helper to execute mapped RDMA operation
bool RDMAManager::execute_rdma(RDMAOp& op) {
    Profiler::Scoped scope("rdma.execute");
    // LOG_DEBUG("Executing RDMA operation: type=" << static_cast<int>(op.type) << ", gaddr=" << std::hex << static_cast<GlobalAddress>(op.addr).raw << std::dec);
    size_t local_addr;
    auto server = get_server(op.addr, local_addr);
    if (!server) return false;
    RDMAOp local_op = op;
    local_op.addr = local_addr;
    bool result = false;
    switch (op.type) {
        case RDMAOpType::READ:
            {
                Profiler::Scoped op_scope("rdma.execute.read");
                // std::cout << "will read" << std::endl;
                result = server->get_rdma().rdma_read(local_op);
                // std::cout << "read results = " << result << std::endl;
            }
            break;
        case RDMAOpType::WRITE:
            {
                Profiler::Scoped op_scope("rdma.execute.write");
                result = server->get_rdma().rdma_write(local_op);
            }
            break;
        case RDMAOpType::CAS:
            {
                Profiler::Scoped op_scope("rdma.execute.cas");
                result = server->get_rdma().rdma_cas(local_op);
            }
            break;
        case RDMAOpType::FAA:
            {
                Profiler::Scoped op_scope("rdma.execute.faa");
                server->get_rdma().rdma_faa(local_op);
                result = true;
            }
            break;
    }
    return result;
}


RDMAManager::RDMAThreadStats& RDMAManager::ensure_thread_stats() const {
    thread_local std::unordered_map<const RDMAManager*, RDMAThreadStats*> tls_stats;
    auto it = tls_stats.find(this);
    if (it != tls_stats.end()) {
        return *it->second;
    }

    auto stats = std::make_unique<RDMAThreadStats>();
    RDMAThreadStats* raw = stats.get();
    {
        std::lock_guard<std::mutex> lock(stats_mutex_);
        thread_stats_.push_back(std::move(stats));
    }
    tls_stats[this] = raw;
    return *raw;
}

RDMAManager::RDMAManager(const std::vector<std::shared_ptr<MemoryServer>>& mem_servers, size_t mem_per_server, double base_rtt_us)
    : mem_servers_(mem_servers), mem_per_server_(mem_per_server), base_rtt_us_(base_rtt_us) {
    reset_stats();
}

std::shared_ptr<MemoryServer> RDMAManager::get_server(const GlobalAddress& gaddr, size_t& local_addr) {
    Profiler::Scoped scope("rdma.get_server");
    uint8_t server_id = gaddr.server_index();
    local_addr = gaddr.offset();
    if (server_id < mem_servers_.size()) {
        // std::cout << "Mapping to server " << static_cast<int>(server_id) << " local_addr: " << std::hex << local_addr << std::dec << std::endl;
        return mem_servers_[server_id];
    }
    return nullptr;
}


bool RDMAManager::perform_op(RDMAOp& op) {
    Profiler::Scoped scope("rdma.perform_op");
    util::precise_sleep_us(base_rtt_us_ / 2.0);
    bool result = execute_rdma(op);
    util::precise_sleep_us(base_rtt_us_ / 2.0);
    auto& stats = ensure_thread_stats();
    stats.op_counts[static_cast<size_t>(op.type)]++; // Simple increment, no atomic needed
    stats.total_rtt_ns++; // Simple increment, no atomic needed
    return result;
}


bool RDMAManager::perform_batch(std::vector<RDMAOp>& ops) {
    Profiler::Scoped scope("rdma.perform_batch");
    // LOG_DEBUG("Performing batch of size " << ops.size());
    util::precise_sleep_us(base_rtt_us_ / 2.0);
    for (auto op : ops) {
        if (!execute_rdma(op)) {
            return false;
        }
    }
    util::precise_sleep_us(base_rtt_us_ / 2.0);
    auto& stats = ensure_thread_stats();
    for (auto op : ops) {
        stats.op_counts[static_cast<size_t>(op.type)]++;
        stats.total_rtt_ns++;
    }
    return true;
}

RDMAManager::RDMAStats RDMAManager::collect_stats() const {
    RDMAStats stats;
    std::lock_guard<std::mutex> lock(stats_mutex_);
    stats.per_thread.reserve(thread_stats_.size());
    for (const auto& entry : thread_stats_) {
        if (!entry) continue;
        RDMAThreadStatsSnapshot snapshot;
        snapshot.thread_id = entry->thread_id;
        for (size_t i = 0; i < snapshot.op_counts.size(); ++i) {
            auto count = entry->op_counts[i];
            snapshot.op_counts[i] = count;
            stats.op_counts[i] += count;
        }
        auto total = entry->total_rtt_ns;
        snapshot.total_rtt_ns = total;
        stats.total_rtt_ns += total;
        stats.per_thread.push_back(std::move(snapshot));
    }
    return stats;
}

void RDMAManager::reset_stats() {
    std::lock_guard<std::mutex> lock(stats_mutex_);
    for (auto& entry : thread_stats_) {
        if (!entry) continue;
        for (auto& counter : entry->op_counts) {
            counter = 0;
        }
        entry->total_rtt_ns = 0;
    }
}

bool rdma_try_acquire_lock(RDMAManager& rdma_mgr, GlobalAddress lock_address) {
    Profiler::Scoped scope("rdma.try_acquire_lock");
    uint64_t expected = 0;
    uint64_t desired = 1;

    RDMAOp op{RDMAOpType::CAS, lock_address};
    op.op.cas.expected = reinterpret_cast<uint64_t>(&expected);
    op.op.cas.desired = desired;

    if (!rdma_mgr.perform_op(op)) {
        assert(false);
    }
    return expected == 0;
}

bool rdma_release_lock(RDMAManager& rdma_mgr, GlobalAddress lock_address) {
    Profiler::Scoped scope("rdma.release_lock");
    RDMAOp op{RDMAOpType::FAA, lock_address};
    op.op.faa.increment = -1;

    // return value of FAA is the previous value
    return rdma_mgr.perform_op(op) == 1;
}
