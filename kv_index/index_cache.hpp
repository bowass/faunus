#pragma once

#include "faunus_index.hpp"
#include <unordered_map>
#include <atomic>
#include <optional>
#include <chrono>
#include <vector>
#include <memory>
#include <thread>
#include <mutex>

namespace faunus_index_internal {

// Forward declaration for lock-free cache entry
struct LockFreeCacheEntry;

// Atomic pointer wrapper for cache entries
using AtomicCacheEntryPtr = std::atomic<LockFreeCacheEntry*>;

struct LockFreeCacheEntry {
    GlobalAddress address;
    InternalNode node;
    std::atomic<LockFreeCacheEntry*> next; // For chaining in hash table
    std::atomic<uint32_t> access_time; // Compact timestamp (32-bit seconds since epoch)
    std::atomic<bool> valid;
    
    LockFreeCacheEntry(GlobalAddress addr, const InternalNode& n) 
        : address(addr), node(n), next(nullptr), valid(true) {
        touch();
    }
        
    // Check if key falls within this node's fence range
    bool contains_key(const Key& key) const {
        return key >= node.header.fence.first && key < node.header.fence.second;
    }
    
    // Atomic validity check
    bool is_valid() const {
        return valid.load(std::memory_order_acquire);
    }
    
    void invalidate() {
        valid.store(false, std::memory_order_release);
    }
    
    void touch() {
        // Use 32-bit timestamp to reduce memory overhead
        auto now = std::chrono::steady_clock::now();
        auto epoch_time = std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch()).count();
        access_time.store(static_cast<uint32_t>(epoch_time), std::memory_order_relaxed);
    }
    
    uint32_t get_access_time() const {
        return access_time.load(std::memory_order_relaxed);
    }
};

class IndexCache {
public:
    explicit IndexCache(size_t max_entries = 32768, size_t target_level = 1);
    ~IndexCache();
    
    // Search for a cached InternalNode that contains the given key
    // Returns the node if found and valid, nullopt otherwise
    std::optional<std::pair<GlobalAddress, InternalNode>> search(const Key& key);
    
    // Add an InternalNode to the cache (only if it's at the target level)
    void add(GlobalAddress address, const InternalNode& node);
    
    // Invalidate a specific cache entry by address
    void invalidate(GlobalAddress address);
    
    // Invalidate all entries that might contain the given key
    void invalidate_key_range(const Key& key);
    
    // Invalidate all entries (useful for consistency)
    void invalidate_all();
    
    // Get cache statistics
    struct Stats {
        size_t hits = 0;
        size_t misses = 0;
        size_t entries = 0;
        size_t evictions = 0;
        size_t invalid_ranges = 0; // Count of invalidations due to invalid ranges
        
        double hit_rate() const {
            return (hits + misses) == 0 ? 0.0 : static_cast<double>(hits) / static_cast<double>(hits + misses);
        }
    };
    Stats get_stats() const;
    void reset_stats();
    
    // Get the target level for caching
    size_t get_target_level() const { return target_level_; }

    // Thread finalization - call before thread exits to aggregate stats
    static void finalize_thread_stats();
private:
    // Lock-free hash table implementation
    static constexpr size_t HASH_TABLE_SIZE = 1024; // Power of 2 for fast modulo
    static constexpr size_t MAX_CHAIN_LENGTH = 8;   // Max chain length before resize
    
    std::vector<AtomicCacheEntryPtr> hash_table_;
    std::atomic<size_t> entry_count_;
    size_t max_entries_;
    size_t target_level_;
    
    // Statistics (optimized to avoid contention)
    struct alignas(64) ThreadLocalStats { // Cache line aligned
        size_t hits = 0;
        size_t misses = 0;
        size_t evictions = 0;
        size_t invalid_ranges = 0;
    };
    
    // Use static thread_local storage for stats
    static thread_local ThreadLocalStats tl_stats_;
    
    // Global aggregate stats (thread-safe)
    static std::mutex stats_registry_mutex_;
    static std::vector<ThreadLocalStats*> stats_registry_;
    
    // Global aggregate stats (updated when threads finalize)
    static ThreadLocalStats global_aggregate_stats_;
    
    // Memory management for lock-free operations
    std::atomic<LockFreeCacheEntry*> retired_entries_; // For epoch-based reclamation
    
    // Simple memory pool to reduce allocation overhead
    static constexpr size_t POOL_SIZE = 2048;
    alignas(LockFreeCacheEntry) char entry_pool_storage_[POOL_SIZE * sizeof(LockFreeCacheEntry)];
    std::atomic<size_t> pool_index_{0};
    bool use_pool_{false}; // Disable pool by default for simplicity
    
    // Hash function for keys (using key range start)
    size_t hash_key(const Key& key) const;
    size_t hash_address(GlobalAddress addr) const;
    
    // Lock-free operations
    LockFreeCacheEntry* find_entry_by_key(const Key& key) const;
    LockFreeCacheEntry* find_entry_by_address(GlobalAddress address) const;
    
    // Memory reclamation
    void retire_entry(LockFreeCacheEntry* entry);
    void cleanup_retired_entries();
    
    // Eviction policy (approximate LRU using timestamps)
    void try_evict_if_needed();
    LockFreeCacheEntry* find_oldest_entry() const;
    
    // Fast stats updates (thread-local when possible)
    void ensure_thread_registered() const;
    void increment_hits() const;
    void increment_misses() const;
    void increment_evictions() const;
    void increment_invalid_ranges() const;
    
    // Memory pool management
    LockFreeCacheEntry* allocate_entry(GlobalAddress addr, const InternalNode& node);
    void deallocate_entry(LockFreeCacheEntry* entry);
};

} // namespace faunus_index_internal