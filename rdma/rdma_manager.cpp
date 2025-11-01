#include "../rdma/rdma_manager.hpp"
#include <chrono>
#include <iostream>
#include <thread>
#include <unordered_map>

#include "../util/logging.hpp"
#include "../util/precise_sleep.hpp"

// Forward declaration to avoid circular dependency
class ThreadStatsTracker;

// Thread-local storage for stats tracker
thread_local ThreadStatsTracker* RDMAManager::thread_stats_tracker_ = nullptr;

void RDMAManager::set_thread_stats_tracker(ThreadStatsTracker* tracker) {
    thread_stats_tracker_ = tracker;
}

std::pair<std::unique_lock<std::mutex>, uint64_t> RDMAManager::acquire_server_lock(const std::shared_ptr<MemoryServer>& server) {
    auto start = std::chrono::high_resolution_clock::now();
    
    // Acquire the server's queue lock to model contention/serialization
    std::unique_lock<std::mutex> lock(server->get_rdma().get_queue_mutex()); 

    auto end = std::chrono::high_resolution_clock::now();
    uint64_t contention_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();
    
    return std::make_pair(std::move(lock), contention_ns);
}

// Helper for integer bandwidth delay calculation
uint64_t RDMAManager::calculate_bw_delay_ns(const RDMAOp& op) const {
    size_t bytes = 0;
    switch (op.type) {
        case RDMAOpType::READ:
        case RDMAOpType::WRITE:
            bytes = op.op.write.bytes; // WRITE struct is used for all bulk transfers
            break;
        case RDMAOpType::CAS:
        case RDMAOpType::FAA:
            bytes = sizeof(uint64_t);
            break;
    }
    // Delay (ns) = Bytes * (1,000,000,000 ns/s) / BW (B/s)
    // Using long double intermediate to avoid overflow before the final division, 
    // but the result is a precise integer nanosecond duration.
    return (uint64_t)(((long double)bytes * 1000000000.0L) / SIMULATED_BW_BPS);
}

// Private helper to execute mapped RDMA operation
bool RDMAManager::execute_rdma(RDMAOp& op) {
    // Profiler::Scoped scope("rdma.execute");
    LOG_DEBUG("Executing RDMA operation: type=" << static_cast<int>(op.type) << ", gaddr=" << std::hex << static_cast<GlobalAddress>(op.addr).raw << std::dec);
    RDMAOp local_op = op;
    size_t local_addr;
    auto server = get_server(op.addr, local_addr);
    if (!server) return false;
    local_op.addr = local_addr;
    bool result = false;
    switch (op.type) {
        case RDMAOpType::READ:
            {
                // Profiler::Scoped op_scope("rdma.execute.read");
                // std::cout << "will read" << std::endl;
                result = server->get_rdma().rdma_read(local_op);
                // std::cout << "read results = " << result << std::endl;
            }
            break;
        case RDMAOpType::WRITE:
            {
                // std::cout << "Writing to " << op.addr << ", bytes: " << op.op.write.bytes << std::endl;
                // Profiler::Scoped op_scope("rdma.execute.write");
                result = server->get_rdma().rdma_write(local_op);
            }
            break;
        case RDMAOpType::CAS:
            {
                // Profiler::Scoped op_scope("rdma.execute.cas");
                result = server->get_rdma().rdma_cas(local_op);
            }
            break;
        case RDMAOpType::FAA:
            {
                // Profiler::Scoped op_scope("rdma.execute.faa");
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

RDMAManager::RDMAManager(const std::vector<std::shared_ptr<MemoryServer>>& mem_servers, size_t mem_per_server, uint64_t base_rtt_ns)
    : mem_servers_(mem_servers), mem_per_server_(mem_per_server), base_rtt_ns_(base_rtt_ns) {
    reset_stats();
}

std::shared_ptr<MemoryServer> RDMAManager::get_server(const GlobalAddress& gaddr, size_t& local_addr) {
    // Profiler::Scoped scope("rdma.get_server");
    uint8_t server_id = gaddr.server_index();
    local_addr = gaddr.offset();
    
    if (server_id < mem_servers_.size()) {
        return mem_servers_[server_id];
    }
    
    return nullptr;
}


bool RDMAManager::perform_op(RDMAOp& op) {
    // Profiler::Scoped scope("rdma.perform_op");
    
    // Debug check: ensure op.addr is a valid global address (server_index < num_servers)
    #ifndef NDEBUG
    uint8_t server_id = op.addr.server_index();
    assert(server_id < mem_servers_.size() && "RDMAOp.addr must be a valid global address");
    #endif

    // First RTT half
    size_t local_addr;
    auto server = get_server(op.addr, local_addr);
    if (!server) return false;

    const uint64_t one_way_ns = base_rtt_ns_ / 2;

    util::precise_sleep_ns(one_way_ns);

    // acquiring simulates contention delay
    // auto [server_lock, contention_ns] = acquire_server_lock(server);
    // server_lock.unlock();
    // TODO: THIS SERIALIZES EVERYTHING - MAY BE VERY BAD
    const uint64_t contention_ns = 0; // TODO: set contention_ns from above

    const uint64_t bw_delay_ns = calculate_bw_delay_ns(op);

    // Simulate bandwidth delay
    util::precise_sleep_ns(bw_delay_ns);

    bool result = execute_rdma(op);
    
    // Second RTT half
    // TODO: add async write support
    util::precise_sleep_ns(one_way_ns + bw_delay_ns);

    auto& stats = ensure_thread_stats();
    stats.op_counts[static_cast<size_t>(op.type)]++;
    // TODO: add async write support
    stats.total_rtt_ns += 2 * one_way_ns + contention_ns;
    
    // Track operation in enhanced stats if tracker is set
    if (thread_stats_tracker_) {
        thread_stats_tracker_->record_rdma_op(op);
    }
    
    return result;
}


bool RDMAManager::perform_batch(std::vector<RDMAOp>& ops) {
    if (ops.empty()) {
        return true;
    }

    // Debug check: ensure all ops have valid global addresses

    // Profiler::Scoped scope("rdma.perform_batch");
    auto& stats = ensure_thread_stats();
    
    // 1. Simulate Batch Issue Time (Wall-clock sleep for the outgoing trip)
    const uint64_t one_way_ns = base_rtt_ns_ / 2;
    util::precise_sleep_ns(one_way_ns); 

    // --- Critical Path Tracking ---
    uint64_t max_server_path_ns = 0; // Max time spent on a single server's path (Contention + BW)
    uint64_t max_return_trip_ns = 0; // Max return trip for synchronous ops

    // Stores the total accumulated execution time (Contention + BW) for each server.
    // This models the serialization of requests *at the server* for this specific batch.
    std::unordered_map<std::shared_ptr<MemoryServer>, uint64_t> server_busy_until_ns;
    
    // 2. Execution and Contention Modeling Phase (Serial Loop to issue/model ops)
    for (RDMAOp& op : ops) {
        size_t local_addr;
        auto server = get_server(op.addr, local_addr);
        if (!server) {
            std::cout << "Failed to get server for RDMAOp: " << (int)op.type << " " << op.addr << std::endl;
            std::this_thread::sleep_for(std::chrono::milliseconds(1000));
            return false;
        }

        // --- Contention Measurement (Control Plane Handshake) ---
        // Acquire lock just to measure wait time, then release immediately.
        // This models the contention for the server's control-plane resource.
        // TODO: we acquire in a serial manner, assert that this is acceptable
        // auto [server_lock, contention_ns] = acquire_server_lock(server);
        // server_lock.unlock(); // Release immediately (crucial fix)
        uint64_t contention_ns = 0; // TODO: set contention_ns from above

        // --- Execution Latency (BW Delay) ---
        uint64_t bw_delay_ns = calculate_bw_delay_ns(op);

        // Accumulate time on the specific server's path (models serial execution at the server)
        server_busy_until_ns[server] += bw_delay_ns;

        // --- Execution Phase (Local Copy) ---
        RDMAOp local_op = op;
        local_op.addr = local_addr;
        bool result = execute_rdma(op); // Instantaneous local execution
        if (!result) {
            std::cout << "RDMAOp failed: " << (int)op.type << " " << op.addr << std::endl;
            std::this_thread::sleep_for(std::chrono::milliseconds(1000));
            return false;
        }

        // --- RTT and Stats Update ---
        uint64_t op_return_trip_ns = 0;

        op_return_trip_ns = one_way_ns;
        // Track the maximum return trip time for the final wall-clock wait
        max_return_trip_ns = std::max(max_return_trip_ns, op_return_trip_ns);

        // Stats update (Individual op latency is calculated for accumulation)
        stats.op_counts[static_cast<size_t>(op.type)]++;
        // stats.total_contention_ns += contention_ns;
        
        // Total latency attributed to this single operation for statistics
        uint64_t op_total_latency_ns = one_way_ns + contention_ns + bw_delay_ns + op_return_trip_ns;
        stats.total_rtt_ns += op_total_latency_ns;
    }

    // 3. Determine and Apply Critical Path Delay (Wall-clock wait)
    // The client thread now waits for the duration of the execution time on the slowest server.
    for (const auto& pair : server_busy_until_ns) {
        max_server_path_ns = std::max(max_server_path_ns, pair.second);
    }
    
    // Wait for the total time required by the slowest server's serial execution path.
    // This correctly models the parallel execution of the batch.
    util::precise_sleep_ns(max_server_path_ns); 
    
    // 4. Simulate Final Return Trip Wait
    // Wait for the completion of the slowest synchronous request.
    util::precise_sleep_ns(max_return_trip_ns);

        // Track operation in enhanced stats if tracker is set
    if (thread_stats_tracker_) {
        thread_stats_tracker_->record_rdma_ops(ops);
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
    // Profiler::Scoped scope("rdma.try_acquire_lock");
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
    // Profiler::Scoped scope("rdma.release_lock");
    RDMAOp op{RDMAOpType::FAA, lock_address};
    op.op.faa.increment = -1;

    // return value of FAA is the previous value
    return rdma_mgr.perform_op(op) == 1;
}
