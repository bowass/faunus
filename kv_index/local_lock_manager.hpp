#pragma once
#include <atomic>
#include <unordered_map>
#include <memory>
#include <mutex>
#include <thread>
#include <chrono>
#include "../rdma/global_address.hpp"
#include "../util/thread_logging.hpp"

namespace local_locks {

/**
 * @brief Local lock state for a specific page/address
 * Uses ticket-based locking for fairness among threads in the same CS
 */
struct LocalLockNode {
    std::atomic<uint64_t> ticket_lock{0};  // Ticket lock: high 32 bits = now_serving, low 32 bits = next_ticket
    std::atomic<bool> hand_over{false};    // True if lock can be handed over to local threads
    std::atomic<uint8_t> hand_time{0};     // Number of times lock has been handed over locally
    std::atomic<std::thread::id> owner{std::thread::id()};  // Current lock owner thread
    std::atomic<std::chrono::steady_clock::time_point*> acquired_time{nullptr};  // When lock was acquired
    
    // Maximum local handovers before releasing to RDMA
    static constexpr uint8_t MAX_HAND_TIME = 5;
    
    // Timeout for local locks (prevent deadlocks)
    static constexpr auto LOCAL_LOCK_TIMEOUT = std::chrono::milliseconds(100);
    
    bool try_acquire_local(std::thread::id thread_id) {
        // Get ticket
        uint64_t ticket = ticket_lock.fetch_add(1) & 0xFFFFFFFF;
        uint64_t current = ticket_lock.load();
        uint32_t now_serving = (current >> 32) & 0xFFFFFFFF;
        
        // Wait for our turn or timeout
        auto start = std::chrono::steady_clock::now();
        while (now_serving != ticket) {
            if (std::chrono::steady_clock::now() - start > LOCAL_LOCK_TIMEOUT) {
                // Timeout - abandon this ticket
                return false;
            }
            std::this_thread::yield();
            current = ticket_lock.load();
            now_serving = (current >> 32) & 0xFFFFFFFF;
        }
        
        // We have the lock
        owner.store(thread_id);
        auto* time_ptr = new std::chrono::steady_clock::time_point(std::chrono::steady_clock::now());
        acquired_time.store(time_ptr);
        return true;
    }
    
    void release_local(std::thread::id thread_id) {
        if (owner.load() != thread_id) {
            LOG_ERROR("Thread " << thread_id << " trying to release lock not owned by it");
            return;
        }
        
        // Clean up acquired time
        auto* time_ptr = acquired_time.exchange(nullptr);
        delete time_ptr;
        
        owner.store(std::thread::id());
        
        // Increment now_serving to release next waiter
        uint64_t current = ticket_lock.load();
        uint64_t new_val = ((current >> 32) + 1) << 32 | (current & 0xFFFFFFFF);
        ticket_lock.store(new_val);
    }
    
    bool is_owned_by(std::thread::id thread_id) const {
        return owner.load() == thread_id;
    }
    
    bool has_timed_out() const {
        auto* time_ptr = acquired_time.load();
        if (time_ptr == nullptr) return false;
        return std::chrono::steady_clock::now() - *time_ptr > LOCAL_LOCK_TIMEOUT;
    }
};

/**
 * @brief Manages local locks for threads within the same ComputeServer
 * 
 * This provides a shared memory space for coordinating locks among threads
 * in the same CS, reducing RDMA operations for lock acquisition.
 */
class LocalLockManager {
private:
    // Hash table of local locks indexed by GlobalAddress
    std::unordered_map<uint64_t, std::unique_ptr<LocalLockNode>> local_locks_;
    mutable std::mutex lock_map_mutex_;  // Protects the hash table itself (mutable for const methods)
    
    // Statistics
    std::atomic<uint64_t> local_acquisitions_{0};
    std::atomic<uint64_t> local_handovers_{0};
    std::atomic<uint64_t> rdma_fallbacks_{0};
    std::atomic<uint64_t> timeouts_{0};

public:
    /**
     * @brief Check if a local lock can be handed over
     * @param addr The global address to check
     * @return true if handover is possible
     */
    bool can_handover(GlobalAddress addr) const {
        uint64_t addr_key = addr.raw; 
        std::lock_guard<std::mutex> guard(lock_map_mutex_);
        auto it = local_locks_.find(addr_key);
        if (it == local_locks_.end()) {
            return false;
        }
        
        const LocalLockNode* lock_node = it->second.get();
        std::cout << "Checking handover for address " << std::hex << addr_key 
                  << ": hand_over=" << lock_node->hand_over.load() 
                  << ", hand_time=" << static_cast<int>(lock_node->hand_time.load()) << std::dec << std::endl;
        return lock_node->hand_over.load() && 
               lock_node->hand_time.load() < LocalLockNode::MAX_HAND_TIME &&
               !lock_node->has_timed_out();
    }

    /**
     * @brief Blocking acquire of local lock for the given address
     * @param addr The global address to lock
     * @param thread_id The requesting thread ID  
     * @return true if handover (RDMA lock already held), false if fresh acquisition needed
     */
    bool acquire(GlobalAddress addr, std::thread::id thread_id) {
        uint64_t addr_key = addr.raw;
        
        // Find or create the lock node
        LocalLockNode* lock_node = nullptr;
        bool was_handover = false;
        {
            std::lock_guard<std::mutex> guard(lock_map_mutex_);
            auto it = local_locks_.find(addr_key);
            if (it == local_locks_.end()) {
                // Create new lock node - this is not a handover
                local_locks_[addr_key] = std::make_unique<LocalLockNode>();
                lock_node = local_locks_[addr_key].get();
                was_handover = false;
            } else {
                lock_node = it->second.get();
                
                // Check if lock has timed out
                if (lock_node->has_timed_out()) {
                    LOG_WARN("Local lock for address " << std::hex << addr_key << " has timed out, creating fresh lock");
                    timeouts_.fetch_add(1);
                    // Create fresh lock node
                    local_locks_[addr_key] = std::make_unique<LocalLockNode>();
                    lock_node = local_locks_[addr_key].get();
                    was_handover = false;
                } else {
                    // This is a potential handover if hand_over flag is set
                    was_handover = lock_node->hand_over.load();
                }
            }
        }
        
        // Blocking acquire of the local lock
        while (!lock_node->try_acquire_local(thread_id)) {
            std::this_thread::yield();
        }
        
        local_acquisitions_.fetch_add(1);
        if (was_handover) {
            local_handovers_.fetch_add(1);
        }
        return was_handover;
    }

    /**
     * @brief Release a local lock
     * @param addr The global address to unlock
     * @param thread_id The releasing thread ID
     */
    void release(GlobalAddress addr, std::thread::id thread_id) {
        uint64_t addr_key = addr.raw;
        
        std::lock_guard<std::mutex> guard(lock_map_mutex_);
        auto it = local_locks_.find(addr_key);
        if (it == local_locks_.end()) {
            LOG_ERROR("Trying to release non-existent local lock for address " << std::hex << addr_key);
            return;
        }
        
        LocalLockNode* lock_node = it->second.get();
        
        if (!lock_node->is_owned_by(thread_id)) {
            LOG_ERROR("Thread " << thread_id << " trying to release lock for address " 
                     << std::hex << addr_key << " not owned by it");
            return;
        }
        
        lock_node->release_local(thread_id);
        
        // Check if we can enable handover (have room for more handovers and no timeout)
        if (lock_node->hand_time.load() < LocalLockNode::MAX_HAND_TIME && 
            !lock_node->has_timed_out()) {
            // Enable handover - next thread can acquire without RDMA
            lock_node->hand_over.store(true);
            lock_node->hand_time.fetch_add(1);
        } else {
            // No more handovers - remove lock from map to force RDMA for next acquisition
            local_locks_.erase(it);
        }
    }
    
    /**
     * @brief Force cleanup of all local locks (called when CS shuts down)
     */
    void cleanup_all_locks() {
        std::lock_guard<std::mutex> guard(lock_map_mutex_);
        for (auto& [addr, lock_node] : local_locks_) {
            // Clean up any allocated time pointers
            auto* time_ptr = lock_node->acquired_time.exchange(nullptr);
            delete time_ptr;
        }
        local_locks_.clear();
    }
    
    /**
     * @brief Get statistics about local lock usage
     */
    struct Stats {
        uint64_t local_acquisitions;
        uint64_t local_handovers;
        uint64_t rdma_fallbacks;
        uint64_t timeouts;
        size_t active_locks;
        
        double handover_rate() const {
            return local_acquisitions > 0 ? 
                static_cast<double>(local_handovers) / local_acquisitions : 0.0;
        }
        
        double rdma_fallback_rate() const {
            uint64_t total_attempts = local_acquisitions + rdma_fallbacks;
            return total_attempts > 0 ? 
                static_cast<double>(rdma_fallbacks) / total_attempts : 0.0;
        }
    };
    
    Stats get_stats() const {
        std::lock_guard<std::mutex> guard(lock_map_mutex_);
        return Stats{
            local_acquisitions_.load(),
            local_handovers_.load(),
            rdma_fallbacks_.load(),
            timeouts_.load(),
            local_locks_.size()
        };
    }
};

} // namespace local_locks