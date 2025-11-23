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
    // LOG_DEBUG("Executing RDMA operation: type=" << static_cast<int>(op.type) << ", gaddr=" << std::hex << static_cast<GlobalAddress>(op.addr).raw << std::dec);
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
    : mem_servers_(mem_servers), mem_per_server_(mem_per_server), base_rtt_ns_(base_rtt_ns), sleep_enabled_(true) {
    reset_stats();
}

bool RDMAManager::is_sleep_enabled() const {
    return sleep_enabled_;
}

void RDMAManager::set_sleep(bool sleep_enabled) {
    sleep_enabled_ = sleep_enabled;
}

inline void RDMAManager::sleep_ns(const uint64_t interval) {
    if (sleep_enabled_) {
        util::precise_sleep_ns(interval);
    }
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
    assert(server_id < mem_servers_.size());
    #endif

    // First RTT half
    size_t local_addr;
    auto server = get_server(op.addr, local_addr);
    if (!server) return false;

    const uint64_t one_way_ns = base_rtt_ns_ / 2;

    const uint64_t bw_delay_ns = calculate_bw_delay_ns(op);

    auto server_delay = get_server_time_delay(server, bw_delay_ns);

    uint64_t pre_op_sleep = one_way_ns + server_delay;

    sleep_ns(pre_op_sleep);

    bool result = execute_rdma(op);
    
    uint64_t post_op_sleep = one_way_ns + (op.type == RDMAOpType::READ ? bw_delay_ns : 0);
    // Second RTT half
    sleep_ns(post_op_sleep);

    auto& stats = ensure_thread_stats();
    stats.op_counts[static_cast<size_t>(op.type)]++;
    stats.total_rtt_ns += pre_op_sleep + post_op_sleep;

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
    sleep_ns(one_way_ns); 

    // --- Critical Path Tracking ---
    uint64_t max_server_path_ns = 0; // Max time spent on a single server's path (Contention + BW)

    // 2. Execution and Contention Modeling Phase (Serial Loop to issue/model ops)
    for (RDMAOp& op : ops) {
        size_t local_addr;
        auto server = get_server(op.addr, local_addr);
        if (!server) {
            std::cout << "Failed to get server for RDMAOp: " << (int)op.type << " " << op.addr << std::endl;
            std::this_thread::sleep_for(std::chrono::milliseconds(1000));
            return false;
        }

        // --- Execution Latency (BW Delay) ---
        uint64_t bw_delay_ns = calculate_bw_delay_ns(op);

        uint64_t server_delay = get_server_time_delay(server, bw_delay_ns);

        max_server_path_ns = std::max(max_server_path_ns, server_delay);

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
        // Stats update (Individual op latency is calculated for accumulation)
        stats.op_counts[static_cast<size_t>(op.type)]++;
        
        // Total latency attributed to this single operation for statistics
        stats.total_rtt_ns += one_way_ns + server_delay + one_way_ns;
    }

    // 4. Simulate Final Return Trip Wait
    // Wait for the completion of the slowest synchronous request.
    sleep_ns(max_server_path_ns + one_way_ns);

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

    assert(rdma_mgr.perform_op(op));

    // CAS succeeded if the expected value was indeed 0 (unlocked)
    return expected == 0;
}

bool rdma_release_lock(RDMAManager& rdma_mgr, GlobalAddress lock_address) {
    // Profiler::Scoped scope("rdma.release_lock");
    RDMAOp op{RDMAOpType::FAA, lock_address};
    op.op.faa.increment = -1;

    // return value of FAA is the previous value
    return rdma_mgr.perform_op(op) == 1;
}

// CAS-based lock release for Sherman-style tagged locks
bool rdma_cas_release_lock(RDMAManager& rdma_mgr, GlobalAddress lock_address) {
    // For releasing a lock we own, use unconditional WRITE to set it to 0
    // This is safe because we hold the lock, so no one else should be modifying it
    // READ+CAS approach has a race condition where lock could change between READ and CAS
    RDMAOp op{RDMAOpType::WRITE, lock_address};
    static uint64_t zero = 0;
    op.op.write.buffer = reinterpret_cast<const uint8_t*>(&zero);
    op.op.write.bytes = sizeof(uint64_t);
    
    return rdma_mgr.perform_op(op);
}

/**
 * @brief Retrieves the current time in nanoseconds from a steady (monotonic) clock.
 * @return uint64_t The current time, in nanoseconds.
 */
inline uint64_t get_current_time_ns() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()
    ).count();
}

// Helper function to calculate and absorb the queueing delay for a single op
// Returns the total time the operation will spend *at the server* (T_queue + T_trans)
uint64_t RDMAManager::get_server_time_delay(
    const std::shared_ptr<MemoryServer>& server, 
    uint64_t T_trans) 
{
    // 1. Get the atomic finish time tracker for the target server
    auto it = server_finish_times_.find(server);
    if (it == server_finish_times_.end()) {
        // Initialize the finish time if not found (assuming this happens at startup)
        // For robustness: initialize if missing. Use 0 as the server is free at time 0.
        it = server_finish_times_.emplace(server, 0ULL).first;
    }
    
    std::atomic<uint64_t>& finish_time_ref = it->second;
    
    // 2. Get the current wall-clock time (in ns)
    // NOTE: This function call MUST be extremely fast (e.g., a simple read from a high-res timer).
    const uint64_t T_now = get_current_time_ns(); // ASSUMED FAST HELPER
    
    uint64_t T_prev_finish = finish_time_ref.load();
    uint64_t T_expected_finish;
    
    // --- Atomic Compare-and-Swap Loop for Zero Overhead Contention ---
    do {
        // T_prev_finish holds the expected completion time of the last queued job.
        
        // Calculate when the server is actually free to start the current job.
        // It must be at least T_now, otherwise the server is currently idle/free.
        // The max() operation prevents T_queue from becoming negative.
        uint64_t T_server_free = (T_prev_finish > T_now) ? T_prev_finish : T_now;
        
        // Calculate the new time the server will be busy until (T_server_free + T_trans)
        // This is the value we try to write back to the atomic.
        T_expected_finish = T_server_free + T_trans;
        
    } while (!finish_time_ref.compare_exchange_weak(T_prev_finish, T_expected_finish));
    
    // 3. Calculate Queueing Delay (T_queue)
    // The delay is the time remaining until the server clears the job that was previously last (T_prev_finish).
    // If T_prev_finish <= T_now, the server is idle, and T_queue is 0.
    const uint64_t T_queue = (T_prev_finish > T_now) ? (T_prev_finish - T_now) : 0ULL;
        
    // The return value for perform_op/perform_batch is T_queue + T_trans,
    // which represents the total time this operation takes up on the server path.
    return T_queue + T_trans;
}