#include "fg_index.hpp"
#include <cassert>
#include <cstring>
#include <cassert>

#include "../util/thread_logging.hpp"

FGIndex::FGIndex(std::shared_ptr<RDMAManager> rdma_mgr, std::shared_ptr<LocalAllocator> allocator, GlobalAddress root_offset_pointer)
    : rdma_mgr_(rdma_mgr), allocator_(allocator), root_offset_pointer_(root_offset_pointer) {
    LOG_DEBUG("FGIndex created");
}

bool FGIndex::initialize() {
    // Allocate root node using local allocator (should be set before construction)
    LOG_DEBUG("FGIndex initializing... " << allocator_);
    root_offset_pointer_ = allocator_->allocate(sizeof(GlobalAddress));
    LOG_DEBUG("Allocated root offset pointer at " << std::hex << root_offset_pointer_.raw << std::dec);
    size_t root_offset = allocate_node();
    LOG_DEBUG("Allocated root node at offset " << std::hex << root_offset << std::dec);

    Node root{};
    root.is_leaf = true;
    root.num_keys = 0;
    rdma_write_node(root_offset, root);

    // debug
    // rdma_read_node(root_offset, root);
    // assert(root.is_leaf);

    update_root_offset(root_offset);

    LOG_DEBUG("Verifying root offset: " << get_root_offset() << " should be " << std::hex << root_offset << std::dec);

    return true;
}

std::set<size_t> FGIndex::get_required_sizes() {
    std::set<size_t> v{sizeof(GlobalAddress), sizeof(Value), sizeof(Node)};
    {
        std::ostringstream oss;
        oss << "FGIndex requires sizes: ";
        for (auto s : v) oss << s << " ";
        LOG_DEBUG(oss.str());
    }
    return v;
}

GlobalAddress FGIndex::get_root_offset_pointer() const {
    LOG_DEBUG("Root offset pointer is at " << std::hex << root_offset_pointer_.raw << std::dec);
    return root_offset_pointer_;
}

bool FGIndex::insert(const Key& key, const Value& value) {
    struct StackEntry { size_t offset; size_t child_idx; };
    std::vector<StackEntry> path;
    Node node;
    size_t cur_offset = get_root_offset();
    LOG_DEBUG("Starting insert of key " << key << " at root offset " << std::hex << cur_offset << std::dec);
    rdma_read_node(cur_offset, node);

    while (true) {
        LOG_DEBUG("Traversing to node at offset " << std::hex << cur_offset << std::dec);
        LOG_DEBUG(key);
        RDMALockGuard node_lock(*this, cur_offset);
        if (!node_lock.locked()) {
            LOG_ERROR("NODE LOCKED");
            return false;
        }
        if (!rdma_read_node(cur_offset, node)) return false;
        size_t pos = 0;
        while (pos < node.num_keys && !(key < node.keys[pos])) pos++;
        path.push_back({cur_offset, pos});
        if (node.is_leaf) break;
        cur_offset = node.children[pos];
        LOG_DEBUG("Descending to child " << pos << " at offset " << std::hex << cur_offset << std::dec);
        LOG_DEBUG("am i a leaf? " << node.is_leaf);
    }
    LOG_DEBUG("Found leaf node at offset " << std::hex << cur_offset << std::dec << " with " << node.num_keys << " keys.");
    // Insert in leaf
    int leaf_pos = path.back().child_idx;
    size_t leaf_offset = path.back().offset;
    RDMALockGuard leaf_lock(*this, leaf_offset);
    if (!leaf_lock.locked()) return false;
    if (!rdma_read_node(leaf_offset, node)) return false;
    // assert that key does not already exist
    for (size_t i = 0; i < node.num_keys; ++i) {
        if (key == node.keys[i]) {
            // Key already exists
            return false;
        }
    }
    LOG_DEBUG("Did not find dup key.");
    if (node.num_keys < branch_factor) {
        for (int i = node.num_keys; i > leaf_pos; --i) {
            node.keys[i] = node.keys[i-1];
            node.values[i] = node.values[i-1];
        }
        node.keys[leaf_pos] = key;
        node.values[leaf_pos] = value;
        node.num_keys++;

        return rdma_write_node(leaf_offset, node);
    }
    // Split leaf node
    Node new_leaf{};
    new_leaf.is_leaf = true;
    int mid = branch_factor / 2;
    // Move upper half to new leaf
    for (int i = mid; i < branch_factor; ++i) {
        new_leaf.keys[i-mid] = node.keys[i];
        new_leaf.values[i-mid] = node.values[i];
    }
    new_leaf.num_keys = branch_factor - mid;
    node.num_keys = mid;
    // Insert new key into split leaf
    if (leaf_pos < mid) {
        for (int i = node.num_keys; i > leaf_pos; --i) {
            node.keys[i] = node.keys[i-1];
            node.values[i] = node.values[i-1];
        }
        node.keys[leaf_pos] = key;
        node.values[leaf_pos] = value;
        node.num_keys++;
    } else {
        int new_pos = leaf_pos - mid;
        for (int i = new_leaf.num_keys; i > new_pos; --i) {
            new_leaf.keys[i] = new_leaf.keys[i-1];
            new_leaf.values[i] = new_leaf.values[i-1];
        }
        new_leaf.keys[new_pos] = key;
        new_leaf.values[new_pos] = value;
        new_leaf.num_keys++;
    }

    // Assert: keys in new_leaf are >= keys in node
    for (size_t i = 0; i < node.num_keys; ++i) {
        for (size_t j = 0; j < new_leaf.num_keys; ++j) {
            if (new_leaf.keys[j] < node.keys[i]) {
                LOG_ERROR("Key order violated during split: new_leaf key " << new_leaf.keys[j] << " < node key " << node.keys[i]);
                print_tree();
            }
            assert(!(new_leaf.keys[j] < node.keys[i]));
        }
    }

    // Write both leaves
    size_t new_leaf_offset = allocate_node();
    rdma_write_node(new_leaf_offset, new_leaf);
    rdma_write_node(leaf_offset, node);

    // Prepare split key for parent
    Key split_key(new_leaf.keys[0]);
    // Propagate split up the tree
    for (int i = path.size() - 2; i >= 0; --i) {
        size_t parent_offset = path[i].offset;
        leaf_offset = parent_offset; // update leaf offset for root split
        int child_idx = path[i].child_idx;
        RDMALockGuard child_lock(*this, new_leaf_offset);
        RDMALockGuard parent_lock(*this, parent_offset);
        if (!parent_lock.locked() || !child_lock.locked()) return false;
        Node parent;
        if (!rdma_read_node(parent_offset, parent)) return false;
        if (parent.num_keys < branch_factor) {
            for (int j = parent.num_keys; j > child_idx; --j) {
                parent.keys[j] = parent.keys[j-1];
                parent.children[j+1] = parent.children[j];
            }
            parent.keys[child_idx] = split_key;
            parent.children[child_idx+1] = new_leaf_offset;
            parent.num_keys++;
            rdma_write_node(parent_offset, parent);
            return true;
        }
        // Split internal node (fix: sort keys/children after insertion)
        // Gather all keys/children
        Key all_keys[9];
        size_t all_children[10];
        int total_keys = parent.num_keys;
        int total_children = total_keys + 1;
        for (int k = 0; k < total_keys; ++k) {
        all_keys[k] = Key(parent.keys[k]);
        }
        for (int k = 0; k < total_children; ++k) {
            all_children[k] = parent.children[k];
        }
        // Insert split_key/child
        int insert_idx = child_idx;
        for (int k = total_keys; k > insert_idx; --k) {
        all_keys[k] = Key(all_keys[k-1]);
            all_children[k+1] = all_children[k];
        }
        all_keys[insert_idx] = Key(split_key);
        all_children[insert_idx+1] = new_leaf_offset;
        total_keys++;
        total_children++;
        // Split
        int mid = total_keys / 2;
        parent.num_keys = mid;
        for (int k = 0; k < mid; ++k) {
        parent.keys[k] = Key(all_keys[k]);
        }
        for (int k = 0; k < mid+1; ++k) {
            parent.children[k] = all_children[k];
        }
        // New internal
        Node new_internal{};
        new_internal.is_leaf = false;
        new_internal.num_keys = total_keys - mid;
        for (size_t k = 0; k < new_internal.num_keys; ++k) {
        new_internal.keys[k] = Key(all_keys[mid+k]);
        }
        for (size_t k = 0; k < new_internal.num_keys+1; ++k) {
            new_internal.children[k] = all_children[mid+k];
        }

        // Assert: for each key, left child < key, right child >= key
        for (size_t k = 0; k < parent.num_keys; ++k) {
            size_t left_child = parent.children[k];
            size_t right_child = parent.children[k+1];
            Node left_node, right_node;
            if (rdma_read_node(left_child, left_node)) {
                for (size_t i = 0; i < left_node.num_keys; ++i) {
                    assert(left_node.keys[i] < parent.keys[k]);
                }
            }
            if (rdma_read_node(right_child, right_node)) {
                for (size_t i = 0; i < right_node.num_keys; ++i) {
                    assert(!(right_node.keys[i] < parent.keys[k]));
                }
            }
        }
        size_t new_internal_offset = allocate_node();
        LOG_DEBUG("[FGIndex::insert] Allocated new internal node at offset " << new_internal_offset);
        rdma_write_node(new_internal_offset, new_internal);
        rdma_write_node(parent_offset, parent);
        // Prepare for next propagation
        split_key = new_internal.keys[0];
        new_leaf_offset = new_internal_offset;
    }
    LOG_DEBUG("Splitting root node");
    // Split root
    Node new_root{};
    new_root.is_leaf = false;
    new_root.num_keys = 1;
    new_root.keys[0] = split_key;
    new_root.children[0] = leaf_offset;
    new_root.children[1] = new_leaf_offset;
    size_t new_root_offset = allocate_node();
    rdma_write_node(new_root_offset, new_root);
    update_root_offset(new_root_offset);
    return true;
}

bool FGIndex::read(const Key& key, Value& value_out) {
    Node node;
    size_t cur_offset = get_root_offset();
    while (true) {
        RDMALockGuard node_lock(*this, cur_offset);
        if (!node_lock.locked()) return false;
        if (!rdma_read_node(cur_offset, node)) return false;
        size_t pos = 0;
        Key k(key);
        while (pos < node.num_keys && !(k < node.keys[pos])) pos++;
        if (node.is_leaf) {
            for (size_t i = 0; i < node.num_keys; ++i) {
                if (k == node.keys[i]) {
                    value_out = node.values[i];
                    return true;
                }
            }
            return false;
        } else {
            cur_offset = node.children[pos];
        }
    }
}

bool FGIndex::update(const Key& key, const Value& value) {
    Node node;
    size_t cur_offset = get_root_offset();
    while (true) {
        RDMALockGuard node_lock(*this, cur_offset);
        if (!node_lock.locked()) return false;
        if (!rdma_read_node(cur_offset, node)) return false;
        size_t pos = 0;
        Key k(key);
        while (pos < node.num_keys && !(k < node.keys[pos])) pos++;
        if (node.is_leaf) {
            for (size_t i = 0; i < node.num_keys; ++i) {
                if (k == node.keys[i]) {
                    node.values[i] = Value(value);
                    return rdma_write_node(cur_offset, node);
                }
            }
            return false;
        } else {
            cur_offset = node.children[pos];
        }
    }
}

bool FGIndex::del(const Key& key) {
    Node node;
    size_t cur_offset = get_root_offset();
    while (true) {
        RDMALockGuard node_lock(*this, cur_offset);
        if (!node_lock.locked()) return false;
        if (!rdma_read_node(cur_offset, node)) return false;
        size_t pos = 0;
        Key k(key);
        while (pos < node.num_keys && !(k < node.keys[pos])) pos++;
        if (node.is_leaf) {
            for (size_t i = 0; i < node.num_keys; ++i) {
                if (k == node.keys[i]) {
                    // Remove key from leaf
                    for (size_t j = i; j < node.num_keys - 1; ++j) {
                        node.keys[j] = node.keys[j+1];
                        node.values[j] = node.values[j+1];
                    }
                    node.num_keys--;
                    return rdma_write_node(cur_offset, node);
                }
            }
            return false;
        } else {
            cur_offset = node.children[pos];
        }
    }
}

GlobalAddress FGIndex::get_root_offset() const {
    size_t root_offset;
    GlobalAddress gaddr = get_root_offset_pointer();
    LOG_DEBUG("Getting root offset from pointer at " << std::hex << gaddr.raw << std::dec);
    RDMAOp op{std::chrono::high_resolution_clock::time_point{}, std::chrono::high_resolution_clock::time_point{}, RDMAOpType::READ, gaddr};
    op.op.read.buffer = reinterpret_cast<uint8_t*>(&root_offset);
    op.op.read.bytes = sizeof(size_t);
    if (!rdma_mgr_->perform_op(op)) {
        throw std::runtime_error("Failed to read root offset"); 
    }
    LOG_DEBUG("Current root offset is " << std::hex << root_offset << std::dec);
    return GlobalAddress(root_offset);
}

GlobalAddress FGIndex::update_root_offset(GlobalAddress new_root_offset) {
    LOG_DEBUG("root_offset is updated to" << new_root_offset);
    GlobalAddress old_root_offset = get_root_offset();
    uint64_t expected = old_root_offset;
    uint64_t desired = new_root_offset;

    RDMAOp op{std::chrono::high_resolution_clock::time_point{}, std::chrono::high_resolution_clock::time_point{}, RDMAOpType::CAS, root_offset_pointer_};

    op.op.cas.expected = reinterpret_cast<uint64_t>(&expected);
    op.op.cas.desired = reinterpret_cast<uint64_t>(desired);

    // Retry CAS until success
    if (!rdma_mgr_->perform_op(op)) return false;
    assert(expected == old_root_offset);
    LOG_DEBUG("Root offset updated from " << std::hex << old_root_offset.raw << " to " << new_root_offset.raw << std::dec);
    return expected == old_root_offset;
}

bool FGIndex::rdma_read_node(size_t offset, Node& node) {
    GlobalAddress gaddr(offset);
    RDMAOp op{std::chrono::high_resolution_clock::time_point{}, std::chrono::high_resolution_clock::time_point{}, RDMAOpType::READ, gaddr};
    op.op.read.buffer = reinterpret_cast<uint8_t*>(&node);
    op.op.read.bytes = sizeof(Node);
    return rdma_mgr_->perform_op(op);
}

bool FGIndex::rdma_write_node(size_t offset, const Node& node) {
    GlobalAddress gaddr(offset);
    RDMAOp op{std::chrono::high_resolution_clock::time_point{}, std::chrono::high_resolution_clock::time_point{}, RDMAOpType::WRITE, gaddr};
    op.op.write.buffer = reinterpret_cast<const uint8_t*>(&node);
    op.op.write.bytes = sizeof(Node);
    return rdma_mgr_->perform_op(op);
}

bool FGIndex::acquire_rdma_lock(size_t offset) {
    GlobalAddress lock_addr(offset);
    uint64_t expected = 0;
    uint64_t desired = 1;

    RDMAOp op{std::chrono::high_resolution_clock::time_point{}, std::chrono::high_resolution_clock::time_point{}, RDMAOpType::CAS, lock_addr};
    op.op.cas.expected = reinterpret_cast<uint64_t>(&expected);
    op.op.cas.desired = desired;

    // Retry CAS until success
    while (true) {
        if (!rdma_mgr_->perform_op(op)) return false;
        if (expected == 0) return true; // lock acquired
        // Optionally back off or yield to reduce contention
        std::this_thread::yield();
    }
}

bool FGIndex::release_rdma_lock(size_t offset) {
    GlobalAddress lock_addr(offset);
    uint64_t val = 0;

    RDMAOp op{std::chrono::high_resolution_clock::time_point{}, std::chrono::high_resolution_clock::time_point{}, RDMAOpType::WRITE, lock_addr};
    op.op.write.buffer = reinterpret_cast<uint8_t*>(&val);
    op.op.write.bytes = sizeof(uint64_t);

    return rdma_mgr_->perform_op(op);
}

size_t FGIndex::allocate_node() {
    // Allocate space for a node using the local allocator
    // This should be replaced with the actual local allocator call
    // Example: return local_allocator_->allocate(sizeof(Node));
    // For now, fallback to RDMA manager default (update as needed)
    return allocator_->allocate(sizeof(Node));
}

size_t FGIndex::allocate_value(size_t value_size) {
    // Allocate space for a value using the local allocator
    // Example: return local_allocator_->allocate(value_size);
    // For now, fallback to RDMA manager default (update as needed)
    return allocator_->allocate(value_size);
}

void FGIndex::print_tree(size_t offset, int depth, bool show_kv) {
    if (depth == 0) offset = get_root_offset();
    Node node;
    if (!rdma_read_node(offset, node)) {
        std::cout << std::string(depth*2, ' ') << "[Error reading node at offset " << offset << "]\n";
        return;
    }
    std::cout << std::string(depth*2, ' ');
    std::cout << (node.is_leaf ? "Leaf" : "Internal") << " Node @ " << offset << " keys: ";
    for (size_t i = 0; i < node.num_keys; ++i) {
        std::cout << "[" << node.keys[i] << "] ";
    }
    std::cout << std::dec << "\n";
    if (node.is_leaf && show_kv) {
        std::cout << std::string(depth*2+2, ' ') << "KV-Items:";
        for (size_t i = 0; i < node.num_keys; ++i) {
            std::cout << "\n" << std::string(depth*2+4, ' ')
                      << "Key: " << node.keys[i] << "  Value: " << node.values[i];
        }
        if (node.num_keys == 0) {
            std::cout << " (empty)";
        }
        std::cout << "\n";
    }
    if (!node.is_leaf) {
        for (size_t i = 0; i < node.num_keys+1; ++i) {
            print_tree(node.children[i], depth+1, show_kv);
        }
    }
}
