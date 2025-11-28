#pragma once
#include "kv_index.hpp"
#include "cache/range_cache.hpp"
#include "rdma/global_address.hpp"
#include "rdma/rdma_manager.hpp"
#include "rdma/local_allocator.hpp"
#include "util/array_var.hpp"
#include "util/local_lock_manager.hpp"
#include "util/thread_logging.hpp"
#include "util/thread_rand.hpp"
#include "util/profiler.hpp"

#include <cassert>
#include <cstdint>
#include <memory>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace sherman_index_internal {
    constexpr uint64_t kMaxLevelOfTree = 7;
    constexpr uint32_t kInternalPageSize = 1024;
    constexpr uint32_t kLeafPageSize = 1024;
    constexpr uint64_t kLockChipMemSize = 256 * 1024;
    constexpr uint64_t kNumOfLock = kLockChipMemSize / sizeof(uint64_t);

    // Fingerprint: 12-bit hash of key for fast comparison
    // NOTE: defining this struct as __attribute__((packed)) breaks stuff
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
        os << "FP(" << fp.value << ")";
        return os;
    }

    class Header {
        public:
        GlobalAddress leftmost_ptr;
        GlobalAddress sibling_ptr;
        uint8_t level;
        int16_t last_index;
        Key lowest;
        Key highest;

        friend class InternalPage;
        friend class LeafPage;

        public:
        Header() {
            leftmost_ptr = GlobalAddress::Null();
            sibling_ptr = GlobalAddress::Null();
            last_index = -1;
            lowest = Key::min();
            highest = Key::max();
        }

        void debug() const {
            std::cout << "leftmost=" << leftmost_ptr << ", "
                    << "sibling=" << sibling_ptr << ", "
                    << "level=" << (int)level << ","
                    << "cnt=" << last_index + 1 << ","
                    << "range=[" << lowest << " - " << highest << "]";
        }
    } __attribute__((packed));

    static_assert(sizeof(Header) == 2 * sizeof(Key) + 2 * sizeof(GlobalAddress) + sizeof(uint8_t) + sizeof(int16_t));

    class InternalEntry {
    public:
        Key key;
        GlobalAddress ptr;

        InternalEntry() {
            ptr = GlobalAddress::Null();
            key = Key::min();
        }
    } __attribute__((packed));

    class LeafEntry {
    public:
        Fingerprint fingerprint;       // 12-bit hash of key
        uint16_t padding : 4;          // Align to 16 bits
        GlobalAddress kv_ptr;          // Pointer to separately-allocated KVItem
        
        LeafEntry() : fingerprint(0), padding(0), kv_ptr(GlobalAddress::Null()) {}
        
        LeafEntry(const Fingerprint& fp, GlobalAddress ptr) 
            : fingerprint(fp), padding(0), kv_ptr(ptr) {}
        
        bool is_empty() const {
            return kv_ptr == GlobalAddress::Null();
        }
    } __attribute__((packed));

    constexpr int kInternalCardinality = (kInternalPageSize - sizeof(Header) -
                                        sizeof(uint8_t) * 2 - sizeof(uint64_t)) /
                                        sizeof(InternalEntry);

    constexpr int kLeafCardinality =
        (kLeafPageSize - sizeof(Header) - sizeof(uint8_t) * 2 - sizeof(uint64_t)) /
        sizeof(LeafEntry);

    // Calculate required padding for LeafPage to reach exactly kLeafPageSize
    constexpr size_t kLeafPagePadding = kLeafPageSize 
        - sizeof(uint64_t)  // embedding_lock
        - sizeof(uint8_t)   // front_version
        - sizeof(Header)
        - sizeof(LeafEntry) * kLeafCardinality
        - sizeof(uint8_t);  // rear_version

    // Calculate required padding for InternalPage to reach exactly kInternalPageSize
    constexpr size_t kInternalPagePadding = kInternalPageSize
        - sizeof(uint64_t)  // embedding_lock
        - sizeof(uint8_t)   // front_version
        - sizeof(Header)
        - sizeof(InternalEntry) * kInternalCardinality
        - sizeof(uint8_t);  // rear_version

    class InternalPage {
    public:
        union {
            uint32_t crc;
            uint64_t embedding_lock;
            uint64_t index_cache_freq;
        };

        uint8_t front_version;
        Header hdr;
        InternalEntry records[kInternalCardinality];

        uint8_t padding[kInternalPagePadding];
        uint8_t rear_version;

        friend class ShermanIndex;

    public:
        InternalPage(GlobalAddress left, const Key &key, GlobalAddress right,
                    uint32_t level = 0) {
            hdr.leftmost_ptr = left;
            hdr.level = level;
            records[0].key = key;
            records[0].ptr = right;
            records[1].ptr = GlobalAddress::Null();

            hdr.last_index = 0;

            front_version = 0;
            rear_version = 0;
        
            embedding_lock = 0;
        }

        InternalPage(uint32_t level = 0) {
            hdr.level = level;
            records[0].ptr = GlobalAddress::Null();

            front_version = 0;
            rear_version = 0;

            embedding_lock = 0;
        }

        void set_consistent() {
            front_version++;
            rear_version = front_version;
        }

        bool check_consistent() const {
            bool succ = true;
            succ = succ && (rear_version == front_version);

            return succ;
        }

        void debug() const {
            std::cout << "InternalPage@ ";
            hdr.debug();
            std::cout << "version: [" << (int)front_version << ", " << (int)rear_version
                    << "] lock: " << embedding_lock << std::endl;
        }

        void verbose_debug() const {
            this->debug();
            for (int i = 0; i < this->hdr.last_index + 1; ++i) {
                std::cout << this->records[i].key << ", " << this->records[i].ptr << ", ";
            }
            std::cout << std::endl;
        }

    } __attribute__((packed));

    static_assert(sizeof(InternalPage) == kInternalPageSize);

    class LeafPage {
    public:
        union {
            uint32_t crc;
            uint64_t embedding_lock;
        };
        uint8_t front_version;
        Header hdr;
        LeafEntry records[kLeafCardinality];

        uint8_t padding[kLeafPagePadding];
        uint8_t rear_version;

        friend class ShermanIndex;

    public:
        LeafPage(uint32_t level = 0) {
            hdr.level = level;
            // Initialize all records as empty (with null pointers)
            for (int i = 0; i < kLeafCardinality; ++i) {
                records[i] = LeafEntry();
            }

            front_version = 0;
            rear_version = 0;

            embedding_lock = 0;
        }

        void set_consistent() {
            front_version++;
            rear_version = front_version;
        }

        bool check_consistent() const {
            bool succ = true;
            succ = succ && (rear_version == front_version);

            return succ;
        }

        void debug() const {
            std::cout << "LeafPage@ ";
            hdr.debug();
            std::cout << "version: [" << (int)front_version << ", " << (int)rear_version
                    << "], lock: " << embedding_lock << std::endl;
        }

        void verbose_debug() const {
            this->debug();
            for (int i = 0; i < kLeafCardinality; ++i) {
                if (!this->records[i].is_empty()) {
                    std::cout << "fp=" << this->records[i].fingerprint 
                              << " ptr=" << this->records[i].kv_ptr << ", ";
                } else {
                    std::cout << "empty, ";
                }
            }
            std::cout << std::endl;
        }
    } __attribute__((packed));

    static_assert(sizeof(LeafPage) == kLeafPageSize);

    struct LocalLockNode {
        std::atomic<uint64_t> ticket_lock;
        bool hand_over;
        uint8_t hand_time;
    };

    struct Request {
        bool is_search;
        Key k;
        Value v;
    };

    struct SearchResult {
        bool is_leaf;
        uint8_t level;
        GlobalAddress slibing;
        GlobalAddress next_level;
        Value val;
    };
};

using namespace sherman_index_internal;

using ShermanCacheItem = std::pair<GlobalAddress, InternalPage>;
using ShermanCache = RangeCache<Key, ShermanCacheItem>;

// ShermanCache wrapper that inherits from IndexCacheBase
class ShermanCacheWrapper : public IndexCacheBase {
    std::shared_ptr<ShermanCache> cache_;
public:
    explicit ShermanCacheWrapper(std::shared_ptr<ShermanCache> cache) : cache_(cache) {}
    std::shared_ptr<ShermanCache> get_cache() const { return cache_; }
};

class ShermanIndex : public KVIndex {
    std::shared_ptr<ShermanCache> sherman_cache_;
    uint64_t cs_rdma_tag_;  // Single RDMA tag shared by all threads on this CS
public:
    ShermanIndex(std::shared_ptr<RDMAManager> rdma_mgr, 
                 std::shared_ptr<LocalAllocator> allocator, 
                 GlobalAddress root_offset_pointer = 0, 
                 std::shared_ptr<IndexCacheBase> cache = nullptr,
                 std::shared_ptr<local_locks::LocalLockManager> local_lock_mgr = nullptr);
    bool insert(const Key& key, const Value& value);
    bool read(const Key& key, Value& value_out);
    bool del(const Key& key);

    // Unused
    void maintenance_worker(size_t thread_id, size_t cs_id) {}

    bool initialize(size_t);
    GlobalAddress get_root_offset_pointer() const;
    std::set<size_t> get_required_sizes() const;
    
    // Cache factory method
    std::shared_ptr<IndexCacheBase> create_cache(size_t cache_size_bytes) const override;
    
    // Print the entire tree from root
    void print_tree(size_t offset = 0, int depth = 0, bool show_kv = true);
private:
    GlobalAddress get_root_offset() const;
    bool update_root_offset(GlobalAddress left, const Key& key, GlobalAddress right, int level, GlobalAddress old_root);

    bool try_lock_address(GlobalAddress lock_address);
    void unlock_address(GlobalAddress lock_address);
    
    void lock_and_read_page(GlobalAddress lock_addr, GlobalAddress page_address, size_t page_size, void* page_buffer);
    void write_page_and_unlock(void* page_buffer, GlobalAddress page_address, size_t page_size, GlobalAddress lock_addr);

    bool search_node(GlobalAddress node_address, const Key& key, SearchResult& result, bool from_cache=false);
    bool insert_to_leaf(GlobalAddress leaf_address, const Key& key, const Value& value, GlobalAddress root, int level, bool from_cache=false);

    void insert_to_internal(GlobalAddress page_addr, const Key& key, GlobalAddress v, GlobalAddress root, int level);
    void search_and_insert_to_internal(const Key& key, GlobalAddress v, int level);

    inline void before_operation();
    GlobalAddress get_leaf_from_cache_entry(const Entry<Key, ShermanCacheItem>& entry, const Key& key);
};
