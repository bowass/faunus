#pragma once
#include "kv_index.hpp"
#include "array_var.hpp"
#include "../rdma/rdma_manager.hpp"
#include "../rdma/local_allocator.hpp"
#include <vector>
#include <set>
#include <string>
#include <cstdint>
#include "../util/thread_logging.hpp"

/**
 * FGIndex: Baseline disaggregated memory tree-based KV-index for RDMA networks
 * Based on: "Designing Distributed Tree-based Index Structures for Fast RDMA-capable Networks"
 * All index nodes and data are stored on memory servers; all access via RDMA.
 */
class FGIndex : public KVIndex {
    friend class RDMALockGuard;
public:
    FGIndex(std::shared_ptr<RDMAManager> rdma_mgr, std::shared_ptr<LocalAllocator> allocator, GlobalAddress root_offset_pointer = 0);
    bool insert(const Key& key, const Value& value);
    bool read(const Key& key, Value& value_out);
    bool update(const Key& key, const Value& value);
    bool del(const Key& key);

    // Optional maintenance worker (no-op by default)
    void maintenance_worker(size_t cd_id, size_t thread_id) override {}

    bool initialize();
    GlobalAddress get_root_offset_pointer() const;
    std::set<size_t> get_required_sizes() const override;
    static std::set<size_t> get_required_sizes_static(); // Keep static version for compatibility
    // Print the entire tree from root
    void print_tree(size_t offset = 0, int depth = 0, bool show_kv = true);
private:
    static constexpr int branch_factor = 8;
    struct Node {
        uint64_t lock;
        bool is_leaf;
        size_t num_keys;
        Key keys[branch_factor];
        size_t children[branch_factor + 1];
        Value values[branch_factor];

        Node()
            : lock(0), is_leaf(false), num_keys(0), children{0} {}

        Node(const Node& other)
            : lock(other.lock), is_leaf(other.is_leaf), num_keys(other.num_keys) {
            for (size_t i = 0; i < 8; ++i) keys[i] = other.keys[i];
            for (size_t i = 0; i < 9; ++i) children[i] = other.children[i];
            for (size_t i = 0; i < 8; ++i) values[i] = other.values[i];
        }

        Node& operator=(const Node& other) {
            if (this != &other) {
                lock = other.lock;
                is_leaf = other.is_leaf;
                num_keys = other.num_keys;
                for (size_t i = 0; i < 8; ++i) keys[i] = other.keys[i];
                for (size_t i = 0; i < 9; ++i) children[i] = other.children[i];
                for (size_t i = 0; i < 8; ++i) values[i] = other.values[i];
            }
            return *this;
        }

        ~Node() = default;
    };

    // Helper methods for RDMA node access
    bool rdma_read_node(size_t offset, Node& node);
    bool rdma_write_node(size_t offset, const Node& node);
    bool acquire_rdma_lock(size_t offset);
    bool release_rdma_lock(size_t offset);
    size_t allocate_node();
    size_t allocate_value(size_t value_size);

    GlobalAddress get_root_offset() const;
    GlobalAddress update_root_offset(GlobalAddress new_root_offset);
};

class RDMALockGuard {
public:
    RDMALockGuard(FGIndex& index, size_t offset) : index_(index), offset_(offset), locked_(false) {
        // std::cout << "Acquiring lock for node at offset " << std::hex << offset_ << std::dec << std::endl;
        locked_ = index_.acquire_rdma_lock(offset_);
        // std::cout << "Lock " << (locked_ ? "acquired" : "failed") << " for node at offset " << std::hex << offset_ << std::dec << std::endl;
    }
    ~RDMALockGuard() {
        // std::cout << "Releasing lock for node at offset " << std::hex << offset_ << std::dec << std::endl;
        if (locked_) index_.release_rdma_lock(offset_);
        // std::cout << "Lock released for node at offset " << std::hex << offset_ << std::dec << std::endl;
    }
    bool locked() const { return locked_; }
private:
    FGIndex& index_;
    size_t offset_;
    bool locked_;
};
