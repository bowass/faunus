#pragma once
#include <string>
#include <vector>
#include <cstdint>
#include <memory>
#include <set>

#include "array_var.hpp"
#include "../rdma/global_address.hpp"

using Key = ArrayVar<8>;
using Value = ArrayVar<8>;

// Forward declarations
class RDMAManager;
class LocalAllocator;
class ThreadStatsTracker;
namespace local_locks {
    class LocalLockManager;
}

/**
 * @brief Abstract base class for index caches
 * This allows different index implementations to use different cache types
 * while providing a common interface for cache management.
 */
class IndexCacheBase {
public:
    virtual ~IndexCacheBase() = default;
    
    // Optional: Add common cache interface methods if needed
    // virtual void clear() = 0;
    // virtual size_t size() const = 0;
};

struct KVItem {
    Key key;
    Value value;
};

inline std::ostream& operator<<(std::ostream& os, const KVItem& kv_item) {
    os << "KVItem(key=" << kv_item.key << ", value=" << kv_item.value << ")";
    return os;
}

class RDMAManager; // forward declaration
class LocalAllocator; // forward declaration

/**
 * @brief Abstract base class for a scalable key-value index over RDMA.
 */
class KVIndex {
protected:
    std::shared_ptr<RDMAManager> rdma_mgr_;
    std::shared_ptr<LocalAllocator> allocator_;
    GlobalAddress root_offset_pointer_;
    std::shared_ptr<IndexCacheBase> cache_;
    std::shared_ptr<local_locks::LocalLockManager> local_lock_mgr_;
    ThreadStatsTracker* stats_tracker_ = nullptr;  // Optional RDMA operation tracking
public:
    KVIndex(std::shared_ptr<RDMAManager> rdma_mgr, 
            std::shared_ptr<LocalAllocator> allocator, 
            GlobalAddress root_offset_pointer, 
            std::shared_ptr<IndexCacheBase> cache = nullptr,
            std::shared_ptr<local_locks::LocalLockManager> local_lock_mgr = nullptr)
        : rdma_mgr_(rdma_mgr), allocator_(allocator), root_offset_pointer_(root_offset_pointer), 
          cache_(cache), local_lock_mgr_(local_lock_mgr) {}
    virtual ~KVIndex() = default;
    
    /**
     * @brief Set the statistics tracker for RDMA operation monitoring
     * @param tracker Pointer to ThreadStatsTracker for recording operations
     */
    void set_stats_tracker(ThreadStatsTracker* tracker) {
        stats_tracker_ = tracker;
    }
    
    /**
     * @brief Factory method to create an appropriate cache for this index type
     * @param cache_size_bytes Maximum cache size in bytes
     * @return Shared pointer to cache instance, or nullptr if caching not supported
     */
    virtual std::shared_ptr<IndexCacheBase> create_cache(size_t cache_size_bytes) const { return nullptr; }
    /**
     * @brief Read the value for a given key.
     * @param key Pointer to key array
     * @param value_out Pointer to output value array
     * @return true if found, false otherwise
     */
    virtual bool read(const Key& key, Value& value_out) = 0;
    /**
     * @brief Insert a key-value pair.
     * @param key Pointer to key array
     * @param value Pointer to value array
     * @return true if successful, false otherwise
     */
    virtual bool insert(const Key& key, const Value& value) = 0;
    /**
     * @brief Delete a key-value pair.
     * @param key Pointer to key array
     * @return true if successful, false otherwise
     */
    virtual bool del(const Key& key) = 0;
    /**
     * @brief Update the value for a given key.
     * @param key Pointer to key array
     * @param value Pointer to new value array
     * @return true if successful, false otherwise
     */
    virtual bool update(const Key& key, const Value& value) = 0;

    /**
     * @brief Optional maintenance worker. Override in derived classes if needed.
     * @param thread_id Maintenance thread id (0..N-1)
     */
    virtual void maintenance_worker(size_t cs_id, size_t thread_id) {}

    /**
     * @brief Initialization function (for root init etc.). Override if needed.
     * @param optional parameter
     */
    virtual bool initialize(size_t) { return false; }

    /**
     * @brief Finalization function (for root init etc.). Override if needed.
     * @param optional parameter
     */
    virtual bool finalize(size_t) { return false; }

    /**
     * @brief Get the global address of the root offset pointer.
     * @return GlobalAddress of the root offset pointer
     */
    virtual GlobalAddress get_root_offset_pointer() const = 0;

    /**
     * @brief Get the required memory slab sizes for this index implementation.
     * @return Set of required memory allocation sizes in bytes
     */
    virtual std::set<size_t> get_required_sizes() const = 0;
};
