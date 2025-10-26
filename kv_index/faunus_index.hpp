#pragma once
#include "kv_index.hpp"
#include "array_var.hpp"
#include "../rdma/rdma_manager.hpp"
#include "../rdma/local_allocator.hpp"
#include "../util/thread_rand.hpp"
#include "../util/maintenance_queue.hpp"
#include "../util/queued_set.hpp"
#include "../util/thread_logging.hpp"
#include "../util/profiler.hpp"
// #include "../kv_index/index_cache.hpp"
#include "../cache/range_cache.hpp"
#include <vector>
#include <set>
#include <string>
#include <cstdint>
#include <memory>

// TODO: move this to a better place
#define OFFSET_OF_ARRAY_ELEM(type, member, index) \
    (offsetof(type, member) + sizeof(((type*)0)->member[0]) * (index))

namespace faunus_index_internal {
    constexpr int branch_factor = 32;
    struct Fingerprint {
        uint16_t value : 12;
    
        Fingerprint(uint16_t v = 0) : value(v & 0x0FFF) {}
        Fingerprint(const Key& key) {
            // Simple hash function: XOR all bytes and take lower 12 bits
            uint16_t hash = 0;
            for (size_t i = 0; i < key.size(); ++i) {
                hash ^= key.data()[i];
            }
            value = hash & 0x0FFF;
        }
        operator uint16_t() const { return value; }
        bool operator==(const Fingerprint& other) const { return value == other.value; }
        bool operator!=(const Fingerprint& other) const { return value != other.value; }
    };

    inline std::ostream& operator<<(std::ostream& os, const Fingerprint& fp) {
        os << "Fingerprint(" << fp.value << ")";
        return os;
    }

    struct KVBlock {
        uint64_t raw = 0;

        KVBlock(uint64_t raw=0) : raw(raw) {}
        KVBlock(Fingerprint fp, GlobalAddress ptr, bool free, bool locked) {
            setFingerprint(fp);
            setAddr(ptr);
            setFree(free);
            setLocked(locked);
        }

        Fingerprint getFingerprint() const { return raw >> 52; }
        GlobalAddress getAddr() const { return GlobalAddress((raw & 0x000FFFFFFFFFFFFFULL) >> 2); }
        bool isFree() const { return (raw & 0x2) >> 1; }
        bool isLocked() const { return raw & 0x1; }

        void setFingerprint(Fingerprint fp) { raw = (raw & 0x000FFFFFFFFFFFFFULL) | (static_cast<uint64_t>(fp) << 52); }
        void setAddr(GlobalAddress ptr) { raw = (raw & 0xFFF000000000003ULL) | ((static_cast<uint64_t>(ptr.raw) << 2) & 0x000FFFFFFFFFFFCULL); }
        void setFree(bool free) { raw = (raw & 0xFFFFFFFFFFFFFFFDULL) | (static_cast<uint64_t>(free) << 1); }
        void setLocked(bool locked) { raw = (raw & 0xFFFFFFFFFFFFFFFEULL) | static_cast<uint64_t>(locked); }    

        operator uint64_t() const { return raw; }
    };

    static_assert(sizeof(KVBlock) == sizeof(uint64_t));

    inline std::ostream& operator<<(std::ostream& os, const KVBlock& kvb) {
        os << "KVBlock(fp=" << kvb.getFingerprint() << ", ptr=" << kvb.getAddr() << ", free=" << kvb.isFree() << ", locked=" << kvb.isLocked() << ")";
        return os;
    }

    struct Header {
        uint64_t lock;
        uint64_t level;
        std::pair<Key, Key> fence;
        uint64_t last_index;
        GlobalAddress sibling;
    };

    inline std::ostream& operator<<(std::ostream& os, const Header& hdr) {
        os << "Header(lock=" << hdr.lock << ", level=" << hdr.level << ", fence=(" << hdr.fence.first << ", " << hdr.fence.second << "), last_index=" << hdr.last_index << ", sibling=" << hdr.sibling << ")";
        return os;
    }

    struct LeafNode {
        Header header;
        KVBlock kv_blocks[branch_factor];

        LeafNode() : header{} {
            for (size_t i = 0; i < branch_factor; i++) {
                kv_blocks[i] = KVBlock{0, 0, true, false};
            }
        }
    };

    inline std::ostream& operator<<(std::ostream& os, const LeafNode& leaf) {
        os << "LeafNode(" << leaf.header << ", kv_blocks=[";
        for (size_t i = 0; i < branch_factor; ++i) {
            os << leaf.kv_blocks[i];
            if (i < branch_factor - 1) os << ", ";
        }
        os << "])";
        return os;
    }

    struct InternalEntry {
        Key key;
        GlobalAddress child;
    };

    inline std::ostream& operator<<(std::ostream& os, const InternalEntry& entry) {
        os << "InternalEntry(key=" << entry.key << ", child=" << entry.child << ")";
        return os;
    }

    struct InternalNode {
        Header header;
        InternalEntry entries[branch_factor];
    };

    inline std::ostream& operator<<(std::ostream& os, const InternalNode& inode) {
        os << "InternalNode(" << inode.header << ", entries=[";
        for (size_t i = 0; i < branch_factor; ++i) {
            os << inode.entries[i];
            if (i < branch_factor - 1) os << ", ";
        }
        os << "])";
        return os;
    }

    struct FaunusMaintenanceRPC {
        enum OpType { SPLIT, MERGE, STOP } op;
        GlobalAddress leaf_address;
        std::promise<bool> result; // maybe redundant
    };

    // Key type for uniqueness in the queued-set
    struct FaunusMaintenanceKey {
        FaunusMaintenanceRPC::OpType op;
        GlobalAddress leaf_address;
        
        bool operator==(const FaunusMaintenanceKey& other) const {
            return op == other.op && leaf_address.raw == other.leaf_address.raw;
        }
    };

    // Hash function for FaunusMaintenanceKey
    struct FaunusMaintenanceKeyHash {
        size_t operator()(const FaunusMaintenanceKey& key) const {
            // Combine hash of op and leaf_address
            return std::hash<int>{}(static_cast<int>(key.op)) ^ 
                   (std::hash<uint64_t>{}(key.leaf_address.raw) << 1);
        }
    };

    // Key extractor for FaunusMaintenanceRPC
    struct FaunusMaintenanceKeyExtractor {
        FaunusMaintenanceKey operator()(const FaunusMaintenanceRPC& rpc) const {
            return {rpc.op, rpc.leaf_address};
        }
    };

    using FaunusMaintenanceQueue = MaintenanceQueue<FaunusMaintenanceRPC>;
    using FaunusMaintenanceQueuedSet = QueuedSet<FaunusMaintenanceRPC, FaunusMaintenanceKey, FaunusMaintenanceKeyExtractor, FaunusMaintenanceKeyHash>;

    enum FindNodeResult {
        FOUND = 0,
        LOCKED = 1,
        NO_SUCH_LEVEL = 2,
        UNKNOWN = 3,
    };
};

using namespace faunus_index_internal;

// Explicit template instantiation for InternalNode (IndexCache alias)
// Instantiate the template for the InternalNode used by FaunusIndex so
// callers can create shared_ptr<IndexCacheBase> from LockFreeSkiplistCache<InternalNode>.
// Explicit instantiation for InternalNode is provided in index_cache.cpp
// extern template class LockFreeSkiplistCache<InternalNode>;

using FaunusCacheItem = std::pair<GlobalAddress, InternalNode>;
using FaunusCache = RangeCache<Key, FaunusCacheItem>;

// FaunusCache wrapper that inherits from IndexCacheBase
class FaunusCacheWrapper : public IndexCacheBase {
    std::shared_ptr<FaunusCache> cache_;
public:
    explicit FaunusCacheWrapper(std::shared_ptr<FaunusCache> cache) : cache_(cache) {}
    std::shared_ptr<FaunusCache> get_cache() const { return cache_; }
};

/**
 * Faunus
 */
class FaunusIndex : public KVIndex {
private:
    std::shared_ptr<FaunusCache> faunus_cache_; // Keep the specific type for internal use
public:
    FaunusIndex(std::shared_ptr<RDMAManager> rdma_mgr, 
                 std::shared_ptr<LocalAllocator> allocator, 
                 GlobalAddress root_offset_pointer = 0, 
                 std::shared_ptr<IndexCacheBase> cache = nullptr,
                 std::shared_ptr<local_locks::LocalLockManager> local_lock_mgr = nullptr);
    
    std::shared_ptr<IndexCacheBase> create_cache(size_t cache_size_bytes) const override;
    
    bool insert(const Key& key, const Value& value);
    bool read(const Key& key, Value& value_out);
    bool update(const Key& key, const Value& value);
    bool del(const Key& key);

    // Optional maintenance worker: processes RPCs from a per-CS queue
    void maintenance_worker(size_t thread_id, size_t cs_id);

    bool initialize(size_t num_maintenance_queues=0);
    bool finalize(size_t num_maintenance_threads_per_queue=0);

    GlobalAddress get_root_offset_pointer() const;
    std::set<size_t> get_required_sizes() const override;
    static std::set<size_t> get_required_sizes_static(); // Keep static version for compatibility

    // Print the entire tree from root
    void print_tree(size_t offset = 0, int depth = 0, bool show_kv = true);
private:
    static std::vector<std::shared_ptr<FaunusMaintenanceQueue>> s_faunus_maintenance_queues;
    static std::vector<std::shared_ptr<FaunusMaintenanceQueuedSet>> s_faunus_maintenance_queued_sets;

    // Set/get helpers for per-CS maintenance queues (legacy)
    // TODO: get rid of that?
    static std::vector<std::shared_ptr<FaunusMaintenanceQueue>> create_maintenance_queues(size_t num_queues);
    static void set_maintenance_queues(const std::vector<std::shared_ptr<FaunusMaintenanceQueue>>& queues);
    static std::shared_ptr<FaunusMaintenanceQueue> get_maintenance_queue(size_t cs_id);
    static void stop_maintenance(size_t threads_per_queue);
    static size_t num_maintenance_queues();
    
    // Set/get helpers for per-CS maintenance queued-sets (new, prevents duplicates)
    static std::vector<std::shared_ptr<FaunusMaintenanceQueuedSet>> create_maintenance_queued_sets(size_t num_queues);
    static void set_maintenance_queued_sets(const std::vector<std::shared_ptr<FaunusMaintenanceQueuedSet>>& queued_sets);
    static std::shared_ptr<FaunusMaintenanceQueuedSet> get_maintenance_queued_set(size_t cs_id);
    static void stop_maintenance_queued_sets(size_t threads_per_queue);
    static size_t num_maintenance_queued_sets();

    GlobalAddress get_root_offset() const;
    GlobalAddress update_root_offset(GlobalAddress new_root_offset);
    // regard success only if RV is empty
    // from_insert is for the duplicate validation during insert - only at the end
    std::vector<std::pair<size_t, KVItem>> get_candidate_kvs(const LeafNode& leaf, const Fingerprint& fp, bool& sucess, bool from_insert=false, bool from_read=false);

    FindNodeResult find_node(const Key& key, GlobalAddress& node_address, Entry<Key, FaunusCacheItem>* &cached_entry, size_t level = 0, bool from_smo = false, bool use_cache = true);
    bool handle_local_remove_dupes(const Key& key, GlobalAddress leaf_address, const LeafNode& leaf, bool& found, Value& value_out, KVBlock new_kvblock=0, bool from_insert=false, bool from_read=false, bool from_update=false);

    bool request_smo(FaunusMaintenanceRPC::OpType op, GlobalAddress leaf_address);

    bool trylock_node(GlobalAddress node_address);
    bool release_node(GlobalAddress node_address);
    // to_lock: true to lock, false to unlock
    bool lock_unlock_kvblocks(GlobalAddress leaf_address, bool to_lock);

    bool split_leaf(GlobalAddress leaf_address);
    bool insert_internal_entry(const Key& key, GlobalAddress new_child_addr, size_t level);

    bool split_internal_node(GlobalAddress node_address, InternalNode& node, const Key& key, const GlobalAddress& new_child_addr);
    bool setup_new_root(const Key& key, GlobalAddress right_child, size_t level);
};
