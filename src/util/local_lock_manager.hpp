#pragma once
#include "externals/concurrentqueue/concurrentqueue.h"
#include "rdma/global_address.hpp"
#include "util/thread_logging.hpp"

#include <atomic>
#include <cassert>
#include <memory>
#include <thread>
#include <vector>

namespace local_locks {

// Sherman's constants
constexpr uint64_t kLockChipMemSize = 256 * 1024;
constexpr uint64_t kNumOfLock = kLockChipMemSize / sizeof(uint64_t);
constexpr uint8_t kMaxHandOverTime = 8;

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
    std::atomic<uint64_t> ticket_lock{0};
    std::atomic<uint8_t> handover_count{0};
    static constexpr size_t kHandoverFlagSlots = 1024;
    HandoverFlag handover_flags[kHandoverFlagSlots];
};

class LocalLockManager {
private:
    std::unique_ptr<LocalLockNode[]> local_locks_;
    uint64_t cs_rdma_tag_;  // Shared RDMA tag

    inline size_t get_lock_index(GlobalAddress addr) const {
        return addr.server_index() * kNumOfLock + (addr.offset() % kNumOfLock);
    }

public:
    LocalLockManager(size_t num_ms) : local_locks_(new LocalLockNode[num_ms * kNumOfLock]) {
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

        std::hash<void*> hasher;
        uint64_t hash_val = hasher(static_cast<void*>(this));
        cs_rdma_tag_ = (hash_val & 0xFFFFFFFFFFFFFFFFULL);
        if (cs_rdma_tag_ == 0) cs_rdma_tag_ = 1;
    }

    uint64_t get_cs_rdma_tag() const { return cs_rdma_tag_; }

    // Acquire local lock. Return true if handed-over (skip RDMA)
    bool acquire(GlobalAddress addr) {
        size_t idx = get_lock_index(addr);
        auto &node = local_locks_[idx];

        uint64_t lock_val = node.ticket_lock.fetch_add(1, std::memory_order_acquire);
        uint32_t my_ticket = static_cast<uint32_t>(lock_val & 0xFFFFFFFFu);

        while (true) {
            uint32_t current_serving = static_cast<uint32_t>(node.ticket_lock.load(std::memory_order_acquire) >> 32);
            if (my_ticket == current_serving) break;
            std::this_thread::yield();
        }

        // Detect handover from previous owner
        size_t slot = my_ticket % LocalLockNode::kHandoverFlagSlots;
        bool was_handed_over = node.handover_flags[slot].handed_over.load(std::memory_order_acquire);
        node.handover_flags[slot].handed_over.store(false, std::memory_order_release);

        return was_handed_over;
    }

    // Release local lock. Return true if handover (keep RDMA lock)
    bool release(GlobalAddress addr) {
        size_t idx = get_lock_index(addr);
        auto &node = local_locks_[idx];

        uint64_t lock_val = node.ticket_lock.load(std::memory_order_acquire);
        uint32_t current_serving = static_cast<uint32_t>(lock_val >> 32);
        uint32_t next_ticket = static_cast<uint32_t>(lock_val & 0xFFFFFFFFu);

        uint32_t waiters = next_ticket - current_serving - 1;

        if (waiters > 0 && node.handover_count.load(std::memory_order_acquire) < kMaxHandOverTime) {
            // Handover: mark next ticket as handed-over
            size_t next_slot = (current_serving + 1) % LocalLockNode::kHandoverFlagSlots;
            node.handover_flags[next_slot].handed_over.store(true, std::memory_order_release);

            node.handover_count.fetch_add(1, std::memory_order_release);

            // Advance serving to let next thread proceed
            node.ticket_lock.fetch_add(1ULL << 32, std::memory_order_release);

            return true;  // keep RDMA lock
        } else {
            // No handover: reset handover_count
            node.handover_count.store(0, std::memory_order_release);
            node.ticket_lock.fetch_add(1ULL << 32, std::memory_order_release);

            return false;  // caller must release RDMA
        }
    }

    // Optional helper for checking handover without releasing
    bool can_hand_over(GlobalAddress addr) const {
        size_t idx = get_lock_index(addr);
        auto &node = local_locks_[idx];

        uint64_t lock_val = node.ticket_lock.load(std::memory_order_acquire);
        uint32_t current_serving = static_cast<uint32_t>(lock_val >> 32);
        uint32_t next_ticket = static_cast<uint32_t>(lock_val & 0xFFFFFFFFu);
        uint32_t waiters = next_ticket - current_serving - 1;

        return waiters > 0 && node.handover_count.load(std::memory_order_acquire) < kMaxHandOverTime;
    }
};


} // namespace local_locks