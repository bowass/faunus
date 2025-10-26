#pragma once
#include "kv_index.hpp"
#include "local_lock_manager.hpp"
#include "array_var.hpp"
#include "../rdma/rdma_manager.hpp"
#include "../rdma/local_allocator.hpp"
#include "../util/thread_rand.hpp"
#include "../util/thread_logging.hpp"
#include "../util/profiler.hpp"
#include <vector>
#include <set>
#include <string>
#include <cstdint>
#include <memory>
#include <cassert>

namespace sherman_index_internal {
    constexpr uint64_t kMaxLevelOfTree = 7;
    constexpr uint32_t kInternalPageSize = 1024;
    constexpr uint32_t kLeafPageSize = 1024;
    constexpr uint64_t kLockChipMemSize = 256 * 1024;
    constexpr uint64_t kNumOfLock = kLockChipMemSize / sizeof(uint64_t);

    // class ShermanIndex;

    class Header {
        // private:
        public:
        GlobalAddress leftmost_ptr;
        GlobalAddress sibling_ptr;
        uint8_t level;
        int16_t last_index;
        Key lowest;
        Key highest;

        friend class InternalPage;
        friend class LeafPage;
        // friend class ShermanIndex;
        // friend class IndexCache; // removed - external cache is IndexCacheBase

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
        // TODO: removed attribute packed, front and rear versions are full bytes
        // uint8_t f_version : 4;
        uint8_t f_version;
        Key key;
        Value value;
        // uint8_t r_version : 4;
        uint8_t r_version;

        LeafEntry() {
            f_version = 0;
            r_version = 0;
            value = Value::min();
            key = Key::min();
        }
    } __attribute__((packed));

    static_assert(sizeof(LeafEntry) == sizeof(Key) + sizeof(Value) + 2 * sizeof(uint8_t));

    constexpr int kInternalCardinality = (kInternalPageSize - sizeof(Header) -
                                        sizeof(uint8_t) * 2 - sizeof(uint64_t)) /
                                        sizeof(InternalEntry);

    constexpr int kLeafCardinality =
        (kLeafPageSize - sizeof(Header) - sizeof(uint8_t) * 2 - sizeof(uint64_t)) /
        sizeof(LeafEntry);

    class InternalPage {
    // private:
    public:
        union {
            uint32_t crc;
            uint64_t embedding_lock;
            uint64_t index_cache_freq;
        };

        uint8_t front_version;
        Header hdr;
        InternalEntry records[kInternalCardinality];

        uint8_t padding[3];
        uint8_t rear_version;

        friend class ShermanIndex;
        // friend class IndexCache; // removed - external cache is IndexCacheBase

    public:
    // this is called when tree grows
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

            // std::cout << "LeafPage consistency check: " << (int)rear_version << " == " << (int)front_version << std::endl;

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
    // private:
    public:
        union {
            uint32_t crc;
            uint64_t embedding_lock;
        };
        uint8_t front_version;
        Header hdr;
        LeafEntry records[kLeafCardinality];

        uint8_t padding[7];
        uint8_t rear_version;

        friend class ShermanIndex;

    public:
        LeafPage(uint32_t level = 0) {
            hdr.level = level;
            records[0].value = Value::min();

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

            // std::cout << "LeafPage consistency check: " << (int)rear_version << " == " << (int)front_version << std::endl;

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
                std::cout << this->records[i].key << ": " << this->records[i].value << ", ";
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

/**
 * Faunus
 */
class ShermanIndex : public KVIndex {
public:
    ShermanIndex(std::shared_ptr<RDMAManager> rdma_mgr, 
                 std::shared_ptr<LocalAllocator> allocator, 
                 GlobalAddress root_offset_pointer = 0, 
                 std::shared_ptr<IndexCacheBase> cache = nullptr,
                 std::shared_ptr<local_locks::LocalLockManager> local_lock_mgr = nullptr);
    bool insert(const Key& key, const Value& value);
    bool read(const Key& key, Value& value_out);
    bool update(const Key& key, const Value& value);
    bool del(const Key& key);

    // Optional maintenance worker: processes RPCs from a per-CS queue
    void maintenance_worker(size_t thread_id, size_t cs_id);

    bool initialize(size_t);
    GlobalAddress get_root_offset_pointer() const;
    static std::set<size_t> get_required_sizes_static();
    std::set<size_t> get_required_sizes() const;
    // Print the entire tree from root
    void print_tree(size_t offset = 0, int depth = 0, bool show_kv = true);
private:
    // TODO: generalize index cache
    // std::shared_ptr<IndexCache> cache_;

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
};
