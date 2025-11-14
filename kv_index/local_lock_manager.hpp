#pragma once
#include <memory>
#include <thread>
#include <atomic>
#include <vector>
#include <cassert>
#include "../rdma/global_address.hpp"
#include "../util/thread_logging.hpp"

namespace local_locks {

// Sherman's constants
constexpr uint64_t kLockChipMemSize = 256 * 1024;
constexpr uint64_t kNumOfLock = kLockChipMemSize / sizeof(uint64_t);
constexpr uint8_t kMaxHandOverTime = 0;  // DISABLED to test if handovers cause deadlock

/**
 * @brief Per-waiter handover flag (like HOCL's wait queue entry)
 */
struct HandoverFlag {
    std::atomic<bool> handed_over{false};
};

/**
 * @brief Local lock node for intra-CS serialization WITH handovers
 * 
 * Ticket lock for local serialization + per-waiter handover flags.
 * - ticket_lock: Lower 32 bits = next ticket, Upper 32 bits = currently serving
 * - handover_count: Number of consecutive handovers (reset when no waiters)
 * - handover_flags: Per-ticket handover communication (circular buffer)
 */
struct LocalLockNode {
    std::atomic<uint64_t> ticket_lock{0};     // Lower 32: next ticket, Upper 32: current serving
    std::atomic<uint8_t> handover_count{0};   // Consecutive handovers (reset on break)
    
    // Per-waiter handover flags (circular buffer indexed by ticket % buffer size)
    static constexpr size_t kHandoverFlagSlots = 256;  // Support up to 256 concurrent waiters
    HandoverFlag handover_flags[kHandoverFlagSlots];
};

/**
 * @brief Sherman-style local lock manager using fixed arrays and ticket locks
 */
class LocalLockManager {
private:
    // Fixed array of lock nodes (Sherman style) - indexed by lock address
    std::unique_ptr<LocalLockNode[]> local_locks_;
    uint64_t cs_rdma_tag_;  // Shared RDMA tag for all users of this LocalLockManager

    // Convert GlobalAddress to lock index (Sherman's method)
    inline size_t get_lock_index(GlobalAddress addr) const {
        return (addr.server_index() * kNumOfLock + (addr.offset() % kNumOfLock));
    }

public:
    LocalLockManager(size_t num_ms) : local_locks_(new LocalLockNode[num_ms * kNumOfLock]) {
        // Initialize all lock nodes
        for (size_t ms = 0; ms < num_ms; ++ms) {
            for (size_t i = 0; i < kNumOfLock; ++i) {
                auto &node = local_locks_[ms * kNumOfLock + i];
                node.ticket_lock.store(0);
                node.handover_count.store(0);
                for (size_t j = 0; j < LocalLockNode::kHandoverFlagSlots; ++j) {
                    node.handover_flags[j].handed_over.store(false);
                }
            }
        }
        
        // Generate CS-level RDMA tag (shared by all threads using this LocalLockManager)
        // Use our own pointer address as unique CS identifier
        std::hash<void*> hasher;
        uint64_t hash_val = hasher(static_cast<void*>(this));
        cs_rdma_tag_ = (hash_val & 0xFFFFFFFFFFFFFFFFULL);
        if (cs_rdma_tag_ == 0) {
            cs_rdma_tag_ = 1;
        }
        
        LOG_DEBUG("LocalLockManager initialized with CS RDMA tag=" << std::hex << cs_rdma_tag_ << std::dec);
    }

    ~LocalLockManager() = default;
    
    // Get the shared RDMA tag for this compute server
    uint64_t get_cs_rdma_tag() const { return cs_rdma_tag_; }

    /**
     * @brief Acquire local lock for intra-CS serialization
     * @return true if RDMA lock was handed over (skip RDMA acquire), false otherwise
     * 
     * Protocol (like HOCL):
     * 1. Get ticket and wait for local lock
     * 2. Check our per-waiter handover flag
     * 3. Clear our flag for next time
     * 4. Return true if handed over (caller skips RDMA), false if need RDMA acquire
     */
    bool acquire(GlobalAddress addr) {
        size_t lock_idx = get_lock_index(addr);
        auto &node = local_locks_[lock_idx];

        // Get ticket for local lock
        uint64_t lock_val = node.ticket_lock.fetch_add(1);
        uint32_t my_ticket = static_cast<uint32_t>(lock_val & 0xFFFFFFFFu);
        uint32_t current_serving = static_cast<uint32_t>((lock_val >> 32) & 0xFFFFFFFFu);
        
        // Wait for our turn
        while (my_ticket != current_serving) {
            std::this_thread::yield();
            current_serving = node.ticket_lock.load(std::memory_order_acquire) >> 32;
        }

        // We now hold the local lock
        // Check OUR specific handover flag (set by previous holder if they handed over to us)
        size_t my_flag_idx = my_ticket % LocalLockNode::kHandoverFlagSlots;
        bool was_handed_over = node.handover_flags[my_flag_idx].handed_over.load(std::memory_order_acquire);
        
        // Clear flag for next time this slot is used
        if (was_handed_over) {
            node.handover_flags[my_flag_idx].handed_over.store(false, std::memory_order_relaxed);
        }
        
        return was_handed_over;
    }

    /**
     * @brief Release local lock with optional handover
     * @return true if handover (keep RDMA lock), false if should release RDMA
     * 
     * Protocol (like HOCL):
     * 1. Check if another thread is waiting on local lock
     * 2. If yes and under handover limit: set next waiter's flag, handover
     * 3. If no waiters or limit reached: break chain
     * 4. Release local lock
     * 
     * On handover: Next waiter's flag is set BEFORE we release local lock (no race!)
     * On break: Caller releases RDMA lock
     */
    bool release(GlobalAddress addr) {
        size_t lock_idx = get_lock_index(addr);
        auto &node = local_locks_[lock_idx];

        // Check if we should handover
        uint64_t lock_val = node.ticket_lock.load(std::memory_order_acquire);
        uint32_t current_serving = static_cast<uint32_t>((lock_val >> 32) & 0xFFFFFFFFu);
        uint32_t next_ticket = static_cast<uint32_t>(lock_val & 0xFFFFFFFFu);
        uint8_t handovers = node.handover_count.load(std::memory_order_acquire);

        // Check if someone is waiting:
        // - current_serving = us (the holder)
        // - next_ticket = next to be issued
        // - Next waiter has ticket current_serving+1
        // - So waiter exists if next_ticket > current_serving+1
        bool has_waiter = (next_ticket > current_serving + 1);
        bool under_limit = (handovers < kMaxHandOverTime);
        bool should_handover = has_waiter && under_limit;

        if (should_handover) {
            // Handover: set next waiter's flag BEFORE releasing local lock
            uint32_t next_waiter_ticket = current_serving + 1;
            size_t next_flag_idx = next_waiter_ticket % LocalLockNode::kHandoverFlagSlots;
            node.handover_flags[next_flag_idx].handed_over.store(true, std::memory_order_release);
            
            // Increment handover counter
            node.handover_count.fetch_add(1, std::memory_order_release);
        } else {
            // Break chain: reset handover counter
            node.handover_count.store(0, std::memory_order_release);
        }

        // Always release local lock (advance serving ticket)
        node.ticket_lock.fetch_add(1ULL << 32, std::memory_order_release);

        return should_handover;
    }
};

} // namespace local_locks