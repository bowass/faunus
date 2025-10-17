#include "index_cache.hpp"
#include "../util/profiler.hpp"
#include "../util/logging.hpp"
#include <algorithm>
#include <thread>

namespace faunus_index_internal {

// Thread-local stats to avoid contention
thread_local IndexCache::ThreadLocalStats IndexCache::tl_stats_;

// Global stats registry for aggregation
std::mutex IndexCache::stats_registry_mutex_;
std::vector<IndexCache::ThreadLocalStats*> IndexCache::stats_registry_;

// Global aggregate stats (updated when threads finalize)
IndexCache::ThreadLocalStats IndexCache::global_aggregate_stats_;

IndexCache::IndexCache(size_t max_entries, size_t target_level) 
    : hash_table_(HASH_TABLE_SIZE), entry_count_(0), max_entries_(max_entries), 
      target_level_(target_level), retired_entries_(nullptr) {
    if (max_entries_ == 0) {
        max_entries_ = 1; // Minimum cache size
    }
    
    // Initialize hash table with nullptrs
    for (auto& bucket : hash_table_) {
        bucket.store(nullptr, std::memory_order_relaxed);
    }
    
    // LOG_INFO("Lock-free IndexCache created with max_entries=" << max_entries_ << ", target_level=" << target_level_);
}

IndexCache::~IndexCache() {
    invalidate_all();
    cleanup_retired_entries();
}

size_t IndexCache::hash_key(const Key& key) const {
    // Fast hash function optimized for ArrayVar<8> keys
    // Use the first 8 bytes directly if available
    if (key.size() >= 8) {
        const uint64_t* data = reinterpret_cast<const uint64_t*>(key.data());
        return (*data) & (HASH_TABLE_SIZE - 1);
    }
    
    // Optimized fallback for smaller keys
    if (key.size() >= 4) {
        const uint32_t* data = reinterpret_cast<const uint32_t*>(key.data());
        return (*data) & (HASH_TABLE_SIZE - 1);
    }
    
    // Fallback for very small keys
    size_t hash = 0;
    const uint8_t* data = key.data();
    for (size_t i = 0; i < key.size(); ++i) {
        hash = hash * 31 + data[i];
    }
    return hash & (HASH_TABLE_SIZE - 1);
}

size_t IndexCache::hash_address(GlobalAddress addr) const {
    // Hash the raw address
    return (addr.raw >> 3) & (HASH_TABLE_SIZE - 1); // Assume 8-byte alignment
}

std::optional<std::pair<GlobalAddress, InternalNode>> IndexCache::search(const Key& key) {
    Profiler::Scoped scope("cache.search_lockfree");
    
    // Fast path: direct hash lookup
    size_t bucket_idx = hash_key(key);
    LockFreeCacheEntry* current = hash_table_[bucket_idx].load(std::memory_order_acquire);
    
    // Check first few entries in the primary bucket (most likely hits)
    for (int i = 0; i < 3 && current != nullptr; ++i) {
        if (current->is_valid() && current->contains_key(key)) {
            // Check if entry is too old (simple staleness detection)
            uint32_t current_time = static_cast<uint32_t>(
                std::chrono::duration_cast<std::chrono::seconds>(
                    std::chrono::steady_clock::now().time_since_epoch()).count());
            uint32_t entry_age = current_time - current->get_access_time();
            
            // If entry is older than 60 seconds, consider it potentially stale
            if (entry_age > 60) {
                current->invalidate();
                increment_misses();
                break; // Continue to comprehensive search
            }
            
            current->touch(); // Update access time
            increment_hits();
            return std::make_pair(current->address, current->node);
        }
        current = current->next.load(std::memory_order_acquire);
    }
    
    // If not found in first few entries, fall back to comprehensive search
    LockFreeCacheEntry* entry = find_entry_by_key(key);
    if (entry && entry->is_valid()) {
        // Check staleness for comprehensive search results too
        uint32_t current_time = static_cast<uint32_t>(
            std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count());
        uint32_t entry_age = current_time - entry->get_access_time();
        
        if (entry_age > 60) {
            entry->invalidate();
            increment_misses();
            return std::nullopt;
        }
        
        entry->touch(); // Update access time
        increment_hits();
        return std::make_pair(entry->address, entry->node);
    }
    
    increment_misses();
    return std::nullopt;
}

LockFreeCacheEntry* IndexCache::find_entry_by_key(const Key& key) const {
    Profiler::Scoped scope("cache.find_entry_by_key");
    
    // Hash the key to find the most likely bucket
    size_t bucket_idx = hash_key(key);
    
    // Search the primary bucket first
    LockFreeCacheEntry* current = hash_table_[bucket_idx].load(std::memory_order_acquire);
    size_t chain_length = 0;
    
    while (current != nullptr && chain_length < MAX_CHAIN_LENGTH) {
        // Prefetch the next entry while we process the current one
        if (current->next.load(std::memory_order_relaxed) != nullptr) {
            __builtin_prefetch(current->next.load(std::memory_order_relaxed), 0, 1);
        }
        
        if (current->is_valid() && current->contains_key(key)) {
            return current;
        }
        current = current->next.load(std::memory_order_acquire);
        ++chain_length;
    }
    
    // If not found in primary bucket, check only immediate neighbors
    // Most key ranges should be well-distributed, so limit neighbor search
    for (int offset = -1; offset <= 1; ++offset) {
        if (offset == 0) continue; // Already checked primary bucket
        
        size_t neighbor_idx = (bucket_idx + offset + HASH_TABLE_SIZE) % HASH_TABLE_SIZE;
        current = hash_table_[neighbor_idx].load(std::memory_order_acquire);
        chain_length = 0;
        
        // Limit chain search for neighbors to reduce overhead
        while (current != nullptr && chain_length < 4) {
            if (current->is_valid() && current->contains_key(key)) {
                return current;
            }
            current = current->next.load(std::memory_order_acquire);
            ++chain_length;
        }
    }
    
    return nullptr;
}

LockFreeCacheEntry* IndexCache::find_entry_by_address(GlobalAddress address) const {
    size_t bucket_idx = hash_address(address);
    LockFreeCacheEntry* current = hash_table_[bucket_idx].load(std::memory_order_acquire);
    size_t chain_length = 0;
    
    while (current != nullptr && chain_length < MAX_CHAIN_LENGTH) {
        if (current->address.raw == address.raw) {
            return current;
        }
        current = current->next.load(std::memory_order_acquire);
        ++chain_length;
    }
    
    return nullptr;
}

void IndexCache::add(GlobalAddress address, const InternalNode& node) {
    // Only cache nodes at the target level - early return without profiling
    if (node.header.level != target_level_) {
        return;
    }
    
    Profiler::Scoped scope("cache.add_lockfree");
    
    // Fast path: check current count first to avoid expensive operations
    size_t current_count = entry_count_.load(std::memory_order_relaxed);
    if (current_count >= max_entries_) {
        // Try quick eviction first
        try_evict_if_needed();
        // If still full after eviction, don't add
        if (entry_count_.load(std::memory_order_relaxed) >= max_entries_) {
            return;
        }
    }
    
    // Hash based on the node's fence range for better locality
    size_t bucket_idx = hash_key(node.header.fence.first);
    
    // Quick check if entry already exists in primary bucket only
    LockFreeCacheEntry* current = hash_table_[bucket_idx].load(std::memory_order_acquire);
    for (int i = 0; i < 3 && current != nullptr; ++i) {
        if (current->address.raw == address.raw) {
            // Update existing entry in-place (fast path)
            current->node = node;
            current->touch();
            current->increment_version(); // Increment version to indicate update
            current->valid.store(true, std::memory_order_release);
            return;
        }
        current = current->next.load(std::memory_order_acquire);
    }
    
    // Create new entry using memory pool
    auto* new_entry = allocate_entry(address, node);
    if (!new_entry) {
        return; // Pool exhausted
    }
    
    // Insert at head of chain using direct bucket index
    LockFreeCacheEntry* expected = hash_table_[bucket_idx].load(std::memory_order_acquire);
    do {
        new_entry->next.store(expected, std::memory_order_relaxed);
    } while (!hash_table_[bucket_idx].compare_exchange_weak(
        expected, new_entry, std::memory_order_release, std::memory_order_acquire));
    
    entry_count_.fetch_add(1, std::memory_order_relaxed);
}

void IndexCache::invalidate(GlobalAddress address) {
    Profiler::Scoped scope("cache.invalidate_lockfree");
    
    LockFreeCacheEntry* entry = find_entry_by_address(address);
    if (entry != nullptr) {
        entry->invalidate();
        // Note: We don't immediately remove from hash table to avoid ABA problems
        // The entry will be cleaned up during eviction or shutdown
    }
}

void IndexCache::invalidate_key_range(const Key& key) {
    Profiler::Scoped scope("cache.invalidate_key_range_lockfree");
    
    size_t invalidated = 0;
    
    // Hash the key to find the most likely bucket
    size_t bucket_idx = hash_key(key);
    
    // Search the primary bucket
    auto invalidate_in_bucket = [&](size_t idx, size_t max_chain) {
        LockFreeCacheEntry* current = hash_table_[idx].load(std::memory_order_acquire);
        size_t chain_length = 0;
        
        while (current != nullptr && chain_length < max_chain) {
            if (current->is_valid() && current->contains_key(key)) {
                current->invalidate();
                invalidated++;
            }
            current = current->next.load(std::memory_order_acquire);
            ++chain_length;
        }
    };
    
    // Check primary bucket thoroughly
    invalidate_in_bucket(bucket_idx, MAX_CHAIN_LENGTH);
    
    // For better coverage, check a wider range of buckets since fence ranges
    // can overlap with different hash buckets due to their range nature
    for (int offset = -3; offset <= 3; ++offset) {
        if (offset == 0) continue; // Already checked primary bucket
        
        size_t neighbor_idx = (bucket_idx + offset + HASH_TABLE_SIZE) % HASH_TABLE_SIZE;
        invalidate_in_bucket(neighbor_idx, 4); // Reduced chain length for neighbors
    }
    
    // If we still haven't found many entries, do a broader search
    // This is important for correctness when fence ranges don't align with hash distribution
    if (invalidated == 0) {
        // Sample every 8th bucket for a broader search
        for (size_t sample_idx = 0; sample_idx < HASH_TABLE_SIZE; sample_idx += 8) {
            LockFreeCacheEntry* current = hash_table_[sample_idx].load(std::memory_order_acquire);
            size_t chain_length = 0;
            
            while (current != nullptr && chain_length < 2) { // Very limited chain search
                if (current->is_valid() && current->contains_key(key)) {
                    current->invalidate();
                    invalidated++;
                }
                current = current->next.load(std::memory_order_acquire);
                ++chain_length;
            }
        }
    }
    
    if (invalidated > 0) {
        tl_stats_.invalid_ranges += invalidated;
    }
}

void IndexCache::invalidate_all() {
    Profiler::Scoped scope("cache.invalidate_all_lockfree");
    
    // Invalidate all entries
    for (size_t bucket_idx = 0; bucket_idx < HASH_TABLE_SIZE; ++bucket_idx) {
        LockFreeCacheEntry* current = hash_table_[bucket_idx].load(std::memory_order_acquire);
        
        while (current != nullptr) {
            current->invalidate();
            LockFreeCacheEntry* next = current->next.load(std::memory_order_acquire);
            retire_entry(current);
            current = next;
        }
        
        hash_table_[bucket_idx].store(nullptr, std::memory_order_release);
    }
    
    entry_count_.store(0, std::memory_order_relaxed);
    cleanup_retired_entries();
}

void IndexCache::try_evict_if_needed() {
    size_t current_count = entry_count_.load(std::memory_order_relaxed);
    if (current_count < max_entries_) {
        return;
    }
    
    // Fast eviction: find any invalid entry first (cheaper than LRU)
    for (size_t bucket_idx = 0; bucket_idx < HASH_TABLE_SIZE; bucket_idx += 64) { // Skip buckets for speed
        LockFreeCacheEntry* current = hash_table_[bucket_idx].load(std::memory_order_acquire);
        for (int i = 0; i < 2 && current != nullptr; ++i) { // Check only first 2 entries
            if (!current->is_valid()) {
                current->invalidate(); // Mark for cleanup
                increment_evictions();
                return;
            }
            current = current->next.load(std::memory_order_acquire);
        }
    }
    
    // If no invalid entries found, fall back to LRU (but limit search)
    LockFreeCacheEntry* oldest = find_oldest_entry();
    if (oldest != nullptr) {
        oldest->invalidate();
        increment_evictions();
    }
}

LockFreeCacheEntry* IndexCache::find_oldest_entry() const {
    LockFreeCacheEntry* oldest = nullptr;
    uint32_t oldest_time = UINT32_MAX; // Start with max value, look for minimum
    
    // Sample only a subset of buckets for performance
    for (size_t bucket_idx = 0; bucket_idx < HASH_TABLE_SIZE; bucket_idx += 32) { // Sample every 32nd bucket
        LockFreeCacheEntry* current = hash_table_[bucket_idx].load(std::memory_order_acquire);
        size_t chain_length = 0;
        
        while (current != nullptr && chain_length < 4) { // Limit chain search to 4 entries
            if (current->is_valid()) {
                uint32_t access_time = current->get_access_time();
                if (access_time < oldest_time) {
                    oldest_time = access_time;
                    oldest = current;
                }
            }
            current = current->next.load(std::memory_order_acquire);
            ++chain_length;
        }
    }
    
    return oldest;
}

void IndexCache::retire_entry(LockFreeCacheEntry* entry) {
    // Simple retire mechanism - add to retired list
    LockFreeCacheEntry* expected = retired_entries_.load(std::memory_order_acquire);
    do {
        entry->next.store(expected, std::memory_order_relaxed);
    } while (!retired_entries_.compare_exchange_weak(
        expected, entry, std::memory_order_release, std::memory_order_acquire));
}

void IndexCache::cleanup_retired_entries() {
    // Clean up all retired entries
    LockFreeCacheEntry* current = retired_entries_.exchange(nullptr, std::memory_order_acquire);
    
    while (current != nullptr) {
        LockFreeCacheEntry* next = current->next.load(std::memory_order_relaxed);
        deallocate_entry(current); // Use pool-aware deallocation
        current = next;
    }
}

// Ensure current thread's stats are registered for global aggregation
void IndexCache::ensure_thread_registered() const {
    static thread_local bool registered = false;
    if (!registered) {
        std::lock_guard<std::mutex> lock(stats_registry_mutex_);
        stats_registry_.push_back(&tl_stats_);
        registered = true;
    }
}

// Fast thread-local stats methods (no global overhead during hot path)
void IndexCache::increment_hits() const {
    ensure_thread_registered();
    tl_stats_.hits++;
}

void IndexCache::increment_misses() const {
    ensure_thread_registered();
    tl_stats_.misses++;
}

void IndexCache::increment_evictions() const {
    ensure_thread_registered();
    tl_stats_.evictions++;
}

void IndexCache::increment_invalid_ranges() const {
    ensure_thread_registered();
    tl_stats_.invalid_ranges++;
}

// Thread finalization - aggregates thread-local stats into global stats
void IndexCache::finalize_thread_stats() {
    std::lock_guard<std::mutex> lock(stats_registry_mutex_);
    
    // Add current thread's stats to global aggregate
    global_aggregate_stats_.hits += tl_stats_.hits;
    global_aggregate_stats_.misses += tl_stats_.misses;
    global_aggregate_stats_.evictions += tl_stats_.evictions;
    global_aggregate_stats_.invalid_ranges += tl_stats_.invalid_ranges;
    
    // Remove current thread from registry to prevent future access
    auto it = std::find(stats_registry_.begin(), stats_registry_.end(), &tl_stats_);
    if (it != stats_registry_.end()) {
        stats_registry_.erase(it);
    }
}

// Memory pool management for reduced allocation overhead
LockFreeCacheEntry* IndexCache::allocate_entry(GlobalAddress addr, const InternalNode& node) {
    if (use_pool_) {
        size_t idx = pool_index_.fetch_add(1, std::memory_order_relaxed);
        if (idx < POOL_SIZE) {
            // Use placement new to initialize the entry in the pool storage
            LockFreeCacheEntry* entry_ptr = reinterpret_cast<LockFreeCacheEntry*>(
                entry_pool_storage_ + idx * sizeof(LockFreeCacheEntry)
            );
            return new (entry_ptr) LockFreeCacheEntry(addr, node);
        }
        // Pool exhausted, fall back to regular allocation
        use_pool_ = false;
    }
    
    // Regular heap allocation fallback
    return new LockFreeCacheEntry(addr, node);
}

void IndexCache::deallocate_entry(LockFreeCacheEntry* entry) {
    // Check if this entry is from our pool storage
    char* entry_char = reinterpret_cast<char*>(entry);
    if (entry_char >= entry_pool_storage_ && 
        entry_char < entry_pool_storage_ + POOL_SIZE * sizeof(LockFreeCacheEntry)) {
        // Entry is from pool, just call destructor (no need to free)
        entry->~LockFreeCacheEntry();
    } else {
        // Entry is from heap, delete it
        delete entry;
    }
}

IndexCache::Stats IndexCache::get_stats() const {
    Stats result;
    
    // Use the global aggregate (safe after threads have called finalize_thread_stats)
    {
        std::lock_guard<std::mutex> lock(stats_registry_mutex_);
        result.hits = global_aggregate_stats_.hits;
        result.misses = global_aggregate_stats_.misses;
        result.evictions = global_aggregate_stats_.evictions;
        result.invalid_ranges = global_aggregate_stats_.invalid_ranges;
        
        // Also add stats from any currently active threads
        for (const auto* thread_stats : stats_registry_) {
            if (thread_stats) {
                result.hits += thread_stats->hits;
                result.misses += thread_stats->misses;
                result.evictions += thread_stats->evictions;
                result.invalid_ranges += thread_stats->invalid_ranges;
            }
        }
    }
    
    result.entries = entry_count_.load(std::memory_order_relaxed);
    return result;
}

void IndexCache::reset_stats() {
    // Reset both thread-local and global aggregate stats
    std::lock_guard<std::mutex> lock(stats_registry_mutex_);
    
    // Reset global aggregate
    global_aggregate_stats_.hits = 0;
    global_aggregate_stats_.misses = 0;
    global_aggregate_stats_.evictions = 0;
    global_aggregate_stats_.invalid_ranges = 0;
    
    // Reset thread-local stats for currently active threads
    for (auto* thread_stats : stats_registry_) {
        if (thread_stats) {
            thread_stats->hits = 0;
            thread_stats->misses = 0;
            thread_stats->evictions = 0;
            thread_stats->invalid_ranges = 0;
        }
    }
}

} // namespace faunus_index_internal