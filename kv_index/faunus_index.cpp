#include "faunus_index.hpp"
#include "../util/maintenance_queue.hpp"
#include "../util/thread_stats.hpp"
#include <cassert>
#include <cstring>
#include <cassert>
#include <thread>
#include <chrono>
#include <vector>
#include <atomic>
#include <map>
#include <iomanip>

#include "../util/logging.hpp"
#include "../util/profiler.hpp"
#include "../util/precise_sleep.hpp"

using namespace faunus_index_internal;

std::vector<std::shared_ptr<FaunusMaintenanceQueue>> FaunusIndex::s_faunus_maintenance_queues;
std::vector<std::shared_ptr<FaunusMaintenanceQueuedSet>> FaunusIndex::s_faunus_maintenance_queued_sets;

FaunusIndex::FaunusIndex(std::shared_ptr<RDMAManager> rdma_mgr, 
                        std::shared_ptr<LocalAllocator> allocator, 
                        GlobalAddress root_offset_pointer, 
                        std::shared_ptr<IndexCacheBase> cache,
                        std::shared_ptr<local_locks::LocalLockManager> local_lock_mgr)
    : KVIndex(rdma_mgr, allocator, root_offset_pointer, cache, local_lock_mgr) {
    // Extract the FaunusCache from the wrapper if provided
    if (cache_) {
        auto wrapper = std::dynamic_pointer_cast<FaunusCacheWrapper>(cache_);
        if (wrapper) {
            faunus_cache_ = wrapper->get_cache();
        }
    }
}

std::shared_ptr<IndexCacheBase> FaunusIndex::create_cache(size_t cache_size_bytes) const {
    // Estimate number of entries based on cache size
    size_t estimated_entries = std::max(size_t(1), cache_size_bytes / 2048);
    auto faunus_cache = std::make_shared<FaunusCache>(estimated_entries);
    return std::make_shared<FaunusCacheWrapper>(faunus_cache);
}

bool FaunusIndex::initialize(size_t num_maintenance_queues) {
    root_offset_pointer_ = allocator_->allocate(sizeof(GlobalAddress));
    GlobalAddress root_offset = allocator_->allocate(sizeof(LeafNode));
    bool success;

    LeafNode root{};
    root.header.fence = {Key::min(), Key::max()};
    success = rdma_write_object(*rdma_mgr_, root_offset, root);
    if (!success) {
        return false;
    }

    success = update_root_offset(root_offset);

    // initialize maintenance
    if (num_maintenance_queues > 0) {
        FaunusIndex::set_maintenance_queues(FaunusIndex::create_maintenance_queues(num_maintenance_queues));
    }
    return success;
}

bool FaunusIndex::finalize(size_t num_maintenance_threads_per_queue) {
    stop_maintenance(num_maintenance_threads_per_queue);
    return true;
}

std::set<size_t> FaunusIndex::get_required_sizes_static() {
    std::set<size_t> v{sizeof(GlobalAddress), sizeof(Value), sizeof(LeafNode), sizeof(InternalNode), sizeof(KVItem)};
    {
        std::ostringstream oss;
        oss << "FaunusIndex requires sizes: ";
        for (auto s : v) oss << s << " ";
    }
    return v;
}

std::set<size_t> FaunusIndex::get_required_sizes() const {
    return get_required_sizes_static();
}

GlobalAddress FaunusIndex::get_root_offset_pointer() const {
    return root_offset_pointer_;
}

GlobalAddress FaunusIndex::get_root_offset() const {
    size_t root_offset;
    GlobalAddress gaddr = get_root_offset_pointer();

    RDMAOp op{RDMAOpType::READ, gaddr};
    op.op.read.buffer = reinterpret_cast<uint8_t*>(&root_offset);
    op.op.read.bytes = sizeof(size_t);
    if (!rdma_mgr_->perform_op(op)) {
        throw std::runtime_error("Failed to read root offset"); 
    }
    GlobalAddress result(root_offset);

    return result;
}

GlobalAddress FaunusIndex::update_root_offset(GlobalAddress new_root_offset) {
    GlobalAddress old_root_offset = get_root_offset();
    uint64_t expected = old_root_offset;
    uint64_t desired = new_root_offset;

    RDMAOp op{RDMAOpType::CAS, root_offset_pointer_};

    op.op.cas.expected = reinterpret_cast<uint64_t>(&expected);
    op.op.cas.desired = desired;

    // Retry CAS until success
    if (!rdma_mgr_->perform_op(op)) return false;
    assert(expected == old_root_offset);
    return expected == old_root_offset;
}

FindNodeResult FaunusIndex::find_node(const Key& key, GlobalAddress& node_address, Entry<Key, FaunusCacheItem>* &cached_entry, size_t level, bool from_smo, bool use_cache) {
    bool use_cached_node = false;
    InternalNode cached_node;
    GlobalAddress cached_address;
    cached_entry = nullptr;
    bool got_from_cache = false;

    // Try cache first before starting tree traversal
    if (faunus_cache_ && use_cache) {
        cached_entry = faunus_cache_->search(key);
        if (cached_entry) {
            got_from_cache = true;
            cached_node = cached_entry->item.second;
            cached_address = cached_entry->item.first;

            assert(key >= cached_node.header.fence.first && key < cached_node.header.fence.second);
            if (cached_node.header.level >= level && (!cached_node.header.lock || from_smo)) {
                node_address = cached_address;
                use_cached_node = true;
                if (cached_node.header.level == level) {
                    return FindNodeResult::FOUND;
                }
            }
            else {
                faunus_cache_->invalidate(cached_entry);
            }
        }
    }

    // Start tree traversal from root or cached node
    if (!use_cached_node) {
        node_address = get_root_offset();
    }
    bool in_root = !use_cached_node;

    InternalNode node;
    while (true) {
        // Use cached node on first iteration if available, otherwise read from RDMA
        if (use_cached_node) {
            node = cached_node;
            use_cached_node = false; // Only use cache on first iteration
        } else {
            rdma_read_object(*rdma_mgr_, node_address, node);
            // Add to cache if it's at the target cache level (one level above leaves, or 2 if SMO)
            if (faunus_cache_ && !node.header.lock && (node.header.level == uint64_t(1 + from_smo))) {
                faunus_cache_->add(node.header.fence.first, node.header.fence.second, {node_address, node});
            }
        }

        // if locked and not from smo, or another smo has locked an ancestor
        // TODO: even if from_smo, we should not process if the node is locked
        if (node.header.lock && (!from_smo || node.header.level > level)) {
            if (got_from_cache) {
                faunus_cache_->invalidate(cached_entry);
            }
            return FindNodeResult::LOCKED;
        }
        
        // found node at the desired level
        // NOTE: this is rare enough such that we do not return the read node, even though we have it
        if (node.header.level == level) {
            return FindNodeResult::FOUND;
        }

        // Leaf root and we split
        if (node.header.level < level) {
            // we should only get here if we split a root
            assert(in_root);
            // Invalidate cache since tree structure has changed
            if (got_from_cache) {
                faunus_cache_->invalidate(cached_entry);
            }
            return FindNodeResult::NO_SUCH_LEVEL;
        }

        // find next level
        bool found = false;
        GlobalAddress next_address;
        for (size_t i = 0; !found && (i <= node.header.last_index); i++) {
            if (key < node.entries[i].key) {
                next_address = node.entries[i].child;
                found = true;
            }
        }
        if (!found) {
            next_address = node.entries[node.header.last_index].child;
        }
        assert(node_address != next_address);
        node_address = next_address;
        in_root = false;
        // Found - node_address is the right child
        if (node.header.level == level + 1) {
            return FindNodeResult::FOUND;
        }
    }
    return FindNodeResult::UNKNOWN; // should never reach here
}

std::vector<std::pair<size_t, GlobalAddress>> get_fingerprint_collisions(const LeafNode& leaf, const Fingerprint& fp, bool& found_smo) {
    std::vector<std::pair<size_t, GlobalAddress>> candidates;
    found_smo = false;
    for (size_t i = 0; i < branch_factor; i++) {
        if (leaf.kv_blocks[i].isLocked()) {
            found_smo = true;
        }
        if (!leaf.kv_blocks[i].isFree() && leaf.kv_blocks[i].getFingerprint() == fp) {
            candidates.emplace_back(i, leaf.kv_blocks[i].getAddr());
        }
    }
    return candidates;
}

std::vector<std::pair<size_t, KVItem>> FaunusIndex::get_candidate_kvs(const LeafNode& leaf, const Fingerprint& fp, bool& success, bool from_insert/* = false*/, bool from_read/* = false*/) {
    bool found_smo = false;
    auto candidates_entries = get_fingerprint_collisions(leaf, fp, found_smo);

    if (found_smo && !from_read) {
        success = false;
        return {};
    }

    success = true;
    // in insert, no duplicate entries to delete
    if (from_insert && candidates_entries.size() == 1) {
        return {};
    }

    if (candidates_entries.empty()) {
        return {};
    }
    // read all candidate KVItems in one batch
    std::vector<GlobalAddress> candidates_pointers;
    for (auto& [idx, addr] : candidates_entries) {
        candidates_pointers.push_back(addr);
    }

    std::vector<KVItem> candidate_kvs;    
    std::vector<RDMAOp> ops;
    get_read_batch(*rdma_mgr_, candidates_pointers, candidate_kvs, ops);
    // non-read, we do not need the full KVItem
    // NOTE: this assumes that Key is at the start of KVItem
    if (!from_read) {
        for (auto& op: ops) {
            op.op.read.bytes = sizeof(Key);
        }
    }

    assert(rdma_mgr_->perform_batch(ops));

    assert(candidate_kvs.size() == candidates_entries.size());

    std::vector<std::pair<size_t, KVItem>> candidates;
    for (size_t i = 0; i < candidates_entries.size(); i++) {
        candidates.emplace_back(candidates_entries[i].first, candidate_kvs[i]);
    }
    return candidates;
}

bool FaunusIndex::try_update_kvblock(GlobalAddress kvblock_address, KVBlock& expected_kvb, const KVBlock& new_kvblock) {
    KVBlock tmp_expected_kvb = expected_kvb;
    // continue if fingerprint remained the same, and not free or locked
    // --> the only thing that can change is the addr
    while (tmp_expected_kvb.getFingerprint() == expected_kvb.getFingerprint() && !tmp_expected_kvb.isFree() && !tmp_expected_kvb.isLocked()) {
        expected_kvb = tmp_expected_kvb;
        RDMAOp op{RDMAOpType::CAS, kvblock_address};
        op.op.cas.expected = reinterpret_cast<uint64_t>(&tmp_expected_kvb);
        op.op.cas.desired = new_kvblock.raw;
        assert(rdma_mgr_->perform_op(op));
        // if succeeded - good!
        if (tmp_expected_kvb == expected_kvb) {
            GlobalAddress old_kvitem_address = expected_kvb.getAddr();
            allocator_->free(sizeof(KVItem), old_kvitem_address);
            return true;
        }
    }
    // either SMO or entry was deleted - start from scratch
    return false;
}

bool FaunusIndex::try_delete_kvblock(GlobalAddress kvblock_address, KVBlock& expected_kvb) {
    RDMAOp op{RDMAOpType::CAS, kvblock_address};
    op.op.cas.expected = reinterpret_cast<uint64_t>(&expected_kvb);
    KVBlock new_kvb;
    new_kvb.setDifferentRandomFpAddr();
    // preserve lock bit
    new_kvb.setLocked(expected_kvb.isLocked());
    new_kvb.setFree(true);
    op.op.cas.desired = new_kvb.raw;
    KVBlock old_expected = expected_kvb;
    assert(rdma_mgr_->perform_op(op));
    // If CAS succeeded, free the KVItem
    if (old_expected == op.op.cas.expected) {
        GlobalAddress kvitem_address = old_expected.getAddr();
        allocator_->free(sizeof(KVItem), kvitem_address);
        return true;
    }
    return false;
}

bool FaunusIndex::handle_local_remove_dupes(const Key& key, GlobalAddress leaf_address, LeafNode& leaf, bool& found, Value& value_out, KVBlock new_kvblock/* = 0*/, bool from_insert/* = false*/, bool from_read/* = false*/, bool from_update/* = false*/) {
    if (key < leaf.header.fence.first || key >= leaf.header.fence.second) {
        return false;
    }
    // readers do not care if leaf is locked
    if (leaf.header.lock && !from_read) {
        return false;
    }

    Fingerprint fp(key);
    bool success;
    // pairs of (index in leaf, KVItem)
    auto candidate_kvs = get_candidate_kvs(leaf, fp, success, from_insert, from_read);
    if (candidate_kvs.empty()) {
        return success;
    }
    // check for exact key match
    for (size_t i = 0; i < candidate_kvs.size(); i++) {
        if (candidate_kvs[i].second.key == key) {
            if (!found) {
                found = true;
                value_out = candidate_kvs[i].second.value;
                if (from_update) {
                    size_t index = candidate_kvs[i].first;
                    GlobalAddress kbvlock_address = leaf_address + OFFSET_OF_ARRAY_ELEM(LeafNode, kv_blocks, index);
                    return try_update_kvblock(kbvlock_address, leaf.kv_blocks[index], new_kvblock);
                }
            }
            else {
                size_t index = candidate_kvs[i].first;
                GlobalAddress kbvlock_address = leaf_address + OFFSET_OF_ARRAY_ELEM(LeafNode, kv_blocks, index);
                try_delete_kvblock(kbvlock_address, leaf.kv_blocks[index]);
            }
        }
    }
    return true;
}

bool FaunusIndex::read(const Key& key, Value& value_out) {
    GlobalAddress leaf_address;
    bool success;
    Entry<Key, FaunusCacheItem>* cached_entry = nullptr;
    for (size_t attempt = 0; attempt < 1000000; attempt++) {
        if (attempt > 0) {
            stats_tracker_->record_retry();
            if (cached_entry != nullptr) {
                cached_entry = nullptr;
                stats_tracker_->record_cache_miss();
            }
        }
        // find leaf address - use cached entry only on first attempt
        auto find_result = find_node(key, leaf_address, cached_entry, 0, false, attempt == 0);
        assert(find_result != FindNodeResult::UNKNOWN && find_result != FindNodeResult::NO_SUCH_LEVEL);
        if (find_result != FindNodeResult::FOUND) continue; // retry

        // read leaf
        LeafNode leaf;
        rdma_read_object(*rdma_mgr_, leaf_address, leaf);

        // search for candidate entries
        // remove duplicate entires
        // returns found entries in found and value_out
        bool found = false;
        success = handle_local_remove_dupes(key, leaf_address, leaf, found, value_out, 0, false, true, false);
        if (!success) continue; // retry
        if (cached_entry) {
            stats_tracker_->record_cache_hit();
        }
        return found;
    }
    // deadlock???
    assert(false);
    return false;    
}

// Lightweight 64-bit mixer (to avoid correlated seeds)
static inline uint64_t mix64(uint64_t x) {
    x ^= x >> 33;
    x *= 0xff51afd7ed558ccdULL;
    x ^= x >> 33;
    x *= 0xc4ceb9fe1a85ec53ULL;
    x ^= x >> 33;
    return x;
}

bool FaunusIndex::insert(const Key& key, const Value& value) {
    bool success;
    bool wrote_kvitem = false;
    bool leaf_read = false; // the leaf was recently re-read after a failing CAS
    Fingerprint fp(key);
    KVItem kvitem{key, value};
    GlobalAddress leaf_address;
    GlobalAddress kvitem_address = allocator_->allocate(sizeof(KVItem));
    KVBlock kvb{fp, kvitem_address, false, false};
    LeafNode leaf;
    Entry<Key, FaunusCacheItem>* cached_entry = nullptr;

    for (size_t attempt = 0; attempt < 1000000; attempt++) {
        // assuming KVItem is written if leaf_read is true
        if (!leaf_read) {
            if (attempt > 0) {
                stats_tracker_->record_retry();
                if (faunus_cache_ && cached_entry != nullptr) {
                    faunus_cache_->invalidate(cached_entry);
                }
                // did not get here because of the cache
                else {
                    PAUSE();
                }
            }
            cached_entry = nullptr;

            FindNodeResult find_result = find_node(key, leaf_address, cached_entry, 0, false, attempt == 0);
            assert(find_result != FindNodeResult::UNKNOWN && find_result != FindNodeResult::NO_SUCH_LEVEL);
            if (find_result != FindNodeResult::FOUND) {
                continue; // retry
            }

            // read leaf and write KVItem if needed
            std::vector<RDMAOp> ops;
            // read leaf
            ops.push_back(RDMAOp{RDMAOpType::READ, leaf_address});
            ops.back().op.read.buffer = reinterpret_cast<uint8_t*>(&leaf);
            ops.back().op.read.bytes = sizeof(LeafNode);

            // write KVItem to already allocated space
            if (!wrote_kvitem) {
                ops.push_back(RDMAOp{RDMAOpType::WRITE, kvitem_address});
                ops.back().op.write.buffer = reinterpret_cast<uint8_t*>(&kvitem);
                ops.back().op.write.bytes = sizeof(KVItem);
                wrote_kvitem = true;
            }

            assert(rdma_mgr_->perform_batch(ops));
            assert(leaf.header.level == 0);

            // smo or bad range - retry
            if (leaf.header.lock || key < leaf.header.fence.first || key >= leaf.header.fence.second) {
                continue;
            }
        }
        leaf_read = false;

        // using from_insert == false, we do not care about duplicates here
        auto candidate_kvs = get_candidate_kvs(leaf, fp, success, false, false);
        // LOG_DEBUG("k=" << key << " god cand kvs");
        // smo - retry
        if (candidate_kvs.empty() && !success) {
            continue;
        }
        // search for an existing key - to update
        for (size_t i = 0; i < candidate_kvs.size(); i++) {
            // key already exists - cancel insertion
            if (candidate_kvs[i].second.key == key) {
                size_t index = candidate_kvs[i].first;
                GlobalAddress kvblock_address = leaf_address + OFFSET_OF_ARRAY_ELEM(LeafNode, kv_blocks, index);
                success = try_update_kvblock(kvblock_address, leaf.kv_blocks[index], kvb);
                if (cached_entry != nullptr) {
                    stats_tracker_->record_cache_hit();
                }
                else {
                    stats_tracker_->record_cache_miss();
                }
                return success;
            }
        }

        const uint64_t tid_hash = (uint64_t)std::hash<std::thread::id>{}(std::this_thread::get_id());
        uint64_t seed = mix64(tid_hash ^ attempt);

        bool found_free = false;
        bool found_locked = false;
        success = false;
        for (size_t i = 0; !found_free && !(found_locked) && (i < branch_factor); i++) {
            // each thread traverses the entries in a different order based on its id
            size_t index = (i ^ seed) & (branch_factor - 1);
            // break to continue in the mainloop
            if (leaf.kv_blocks[index].isLocked()) {
                found_locked = true;
            }
            else if (leaf.kv_blocks[index].isFree()) {
                found_free = true;
                // update KVBlock using CAS and re-read the leaf back-to-back
                GlobalAddress kvblock_address = leaf_address + OFFSET_OF_ARRAY_ELEM(LeafNode, kv_blocks, index);
                std::vector<RDMAOp> ops;
                ops.push_back(RDMAOp{RDMAOpType::CAS, kvblock_address});
                uint64_t expected = leaf.kv_blocks[index].raw;
                ops.back().op.cas.expected = reinterpret_cast<uint64_t>(&expected);
                ops.back().op.cas.desired = kvb.raw;

                ops.push_back(RDMAOp{RDMAOpType::READ, leaf_address});
                ops.back().op.read.buffer = reinterpret_cast<uint8_t*>(&leaf);
                ops.back().op.read.bytes = sizeof(LeafNode);

                uint64_t old_excpected = expected;
                assert(rdma_mgr_->perform_batch(ops));

                // successful insertion
                if (old_excpected == expected) {
                    success = true;
                }
                leaf_read = !leaf.header.lock && key >= leaf.header.fence.first && key < leaf.header.fence.second;
            }
        }

        // did not find a free KVBlock, or failed CAS
        // successful insertions will incur SMO
        // continue retrying
        if (!success) {
// NOTE: this tried to replace the preemptive SMO - seems to worsen performance
// #ifndef FAUNUS_MAINTENANCE_ENABLED
//             // split if needed
//             if (!found_free) {
//                 split_leaf(leaf_address);
//             }
// #endif
            continue;
        }
        // leaf is already updated
        // SMO already dealt with duplicates
        if (leaf.header.lock || key < leaf.header.fence.first || key >= leaf.header.fence.second) {
            if (cached_entry != nullptr) {
                stats_tracker_->record_cache_hit();
            }
            else {
                stats_tracker_->record_cache_miss();
            }
            return true;
        }
        // trigger SMO if needed
        size_t num_used = 0;
        for (size_t i = 0; i < branch_factor; i++) {
            if (!leaf.kv_blocks[i].isFree()) num_used++;
        }
        // Trigger split when utilization exceeds watermark (configurable at compile-time)
        if (num_used > static_cast<size_t>(branch_factor * FAUNUS_SPLIT_WATERMARK)) {
#ifdef FAUNUS_MAINTENANCE_ENABLED
            success = request_smo(FaunusMaintenanceRPC::SPLIT, leaf_address);
#else
            split_leaf(leaf_address);
#endif // FAUNUS_MAINTENANCE_ENABLED
            assert(success);
            if (cached_entry != nullptr) {
                stats_tracker_->record_cache_hit();
            }
            else {
                stats_tracker_->record_cache_miss();
            }
            return true;
        }
        // else, handle duplicates manually
        else {
            bool found = false;
            Value dummy;
            handle_local_remove_dupes(key, leaf_address, leaf, found, dummy, 0, true, false, false);
            if (cached_entry != nullptr) {
                stats_tracker_->record_cache_hit();
            }
            else {
                stats_tracker_->record_cache_miss();
            }
            return true;
        }
    }
    std::cout << "Probably deadlocked after 100000 attempts to insert key " << key << ", leafnode " << leaf_address << " at thread " << std::this_thread::get_id() << std::endl;
    assert(false);
    return false; // deadlock???
}

bool FaunusIndex::del(const Key& key) {
    return false;
}

bool FaunusIndex::trylock_node(GlobalAddress node_address) {
    return rdma_try_acquire_lock(*rdma_mgr_, node_address + offsetof(LeafNode, header) + offsetof(Header, lock));
}

RDMAOp FaunusIndex::get_release_node_op(GlobalAddress node_address) {
    GlobalAddress lock_address = node_address + offsetof(LeafNode, header) + offsetof(Header, lock);
    RDMAOp op{RDMAOpType::FAA, lock_address};
    op.op.faa.increment = -1;

    return op;
}

std::vector<RDMAOp> FaunusIndex::get_lock_unlock_kvblocks(GlobalAddress leaf_address, bool to_lock) {
    std::vector<RDMAOp> ops;
    for (size_t i = 0; i < branch_factor; i++) {
        ops.push_back(RDMAOp{RDMAOpType::FAA, leaf_address + OFFSET_OF_ARRAY_ELEM(LeafNode, kv_blocks, i)});
        ops.back().op.faa.increment = to_lock ? 1 : -1; // set lock bit
    }
    return ops;
}

bool FaunusIndex::split_leaf(GlobalAddress leaf_address) {
    // Lock the leaf
    bool success = trylock_node(leaf_address);
    if (!success) {
        return false;
    }

    // Acquire KVBlock locks and read leaf
    auto ops = get_lock_unlock_kvblocks(leaf_address, true);

    LeafNode leaf;
    ops.push_back(RDMAOp{RDMAOpType::READ, leaf_address});
    ops.back().op.read.buffer = reinterpret_cast<uint8_t*>(&leaf);
    ops.back().op.read.bytes = sizeof(LeafNode);

    assert(rdma_mgr_->perform_batch(ops));

    assert(leaf.header.lock);

    // Read all KVItems in the leaf
    std::vector<GlobalAddress> item_pointers;
    std::vector<size_t> taken_indices;
    for (size_t i = 0; i < branch_factor; i++) {
        assert(leaf.kv_blocks[i].isLocked());
        if (!leaf.kv_blocks[i].isFree()) {
            item_pointers.push_back(leaf.kv_blocks[i].getAddr());
            taken_indices.push_back(i);
        }
    }

    // Assuming Key is first in KVItem - reading keys only
    std::vector<Key> leaf_keys;
    if (!rdma_read_batch(*rdma_mgr_, item_pointers, leaf_keys)) {
        assert(false);
    }

    // Remove duplicate keys and sort
    std::map<Key, KVBlock> entries_map;
    for (size_t i = 0; i < leaf_keys.size(); i++) {
        entries_map[leaf_keys[i]] = leaf.kv_blocks[taken_indices[i]];
    }

    // Test - validating fence
    for (auto& [k, v] : entries_map) {
        assert(k >= leaf.header.fence.first && k < leaf.header.fence.second);
    }

    std::vector<std::pair<Key, KVBlock>> entries(entries_map.begin(), entries_map.end());

    for (size_t i = 0; i < entries.size() - 1; i++) {
        assert(entries[i].first < entries[i + 1].first);
    }

    if (entries.size() <= branch_factor / 2) {
        // release kvblocks and node
        auto ops = get_lock_unlock_kvblocks(leaf_address, false);
        ops.push_back(get_release_node_op(leaf_address));

        assert(rdma_mgr_->perform_batch(ops));
        return false;
    }

    // Allocate new leaf
    GlobalAddress new_leaf_address = allocator_->allocate(sizeof(LeafNode));
    LeafNode new_leaf{};

    size_t middle = entries.size() / 2;
    Key middle_key = entries[middle].first;

    // Distribute entries    
    for (size_t i = 0; i < middle; i++) {
        leaf.kv_blocks[i] = entries[i].second;
        leaf.kv_blocks[i].setLocked(false);
        new_leaf.kv_blocks[i] = entries[i + middle].second;
        new_leaf.kv_blocks[i].setLocked(false);
    }

    if (entries.size() % 2 != 0) {
        new_leaf.kv_blocks[middle] = entries.back().second;
        new_leaf.kv_blocks[middle].setLocked(false);
    }

    // Clear the rest of the entries
    for (size_t i = middle; i < branch_factor; i++) {
        // set to random value, unlocked, free
        leaf.kv_blocks[i].setDifferentRandomFpAddr();
        leaf.kv_blocks[i].setLocked(false);
        leaf.kv_blocks[i].setFree(true);
        if (i > middle || entries.size() % 2 == 0) {
            new_leaf.kv_blocks[i].setDifferentRandomFpAddr();
            new_leaf.kv_blocks[i].setLocked(false);
            new_leaf.kv_blocks[i].setFree(true);
        }
    }

    // Update fences
    new_leaf.header.fence = {middle_key, leaf.header.fence.second};
    leaf.header.fence.second = middle_key;

    // Write sibling and leaf
    std::vector<GlobalAddress> addresses{new_leaf_address, leaf_address};
    std::vector<LeafNode> leaves{new_leaf, leaf};
    assert(rdma_write_batch(*rdma_mgr_, addresses, leaves));

    // Insert new fence into parent
    success = insert_internal_entry(middle_key, new_leaf_address, 1);
    assert(success);

    // Release node lock
    auto release_op = get_release_node_op(leaf_address);
    assert(rdma_mgr_->perform_op(release_op));
    return true;
}

bool FaunusIndex::insert_internal_entry(const Key& key, GlobalAddress new_child_addr, size_t level) {
    GlobalAddress node_address;
    InternalNode node;
    Entry<Key, FaunusCacheItem>* cached_entry = nullptr;
    for (size_t attempt = 0; attempt < 1000000; attempt++) {
        // if (attempt > 0) {
        //     // TODO: maybe sleep or yield?
        //     if (faunus_cache_ && cached_entry != nullptr) {
        //         faunus_cache_->invalidate(cached_entry);
        //     }
        //     else {
        //         PAUSE();
        //     }
        // }
        // find leaf address
        // TODO: use cached entry node
        cached_entry = nullptr;
        FindNodeResult find_result = find_node(key, node_address, cached_entry, level, true, attempt == 0);
        assert(find_result != FindNodeResult::UNKNOWN);
        if (find_result == FindNodeResult::NO_SUCH_LEVEL) {
            // need to create a new root
            return setup_new_root(key, new_child_addr, level);
        }
        if (find_result != FindNodeResult::FOUND) {
            continue; // retry
        }

        // Track cache hit/miss for maintenance operations
        if (stats_tracker_) {
            if (cached_entry != nullptr) {
                stats_tracker_->record_cache_hit();
            } else {
                stats_tracker_->record_cache_miss();
            }
        }

        // Lock and read node
        std::vector<RDMAOp> ops;
        // lock node
        uint64_t expected = 0;
        uint64_t desired = 1;
        GlobalAddress lock_address = node_address + offsetof(InternalNode, header) + offsetof(Header, lock);
        ops.push_back(RDMAOp{RDMAOpType::CAS, lock_address});
        ops.back().op.cas.expected = reinterpret_cast<uint64_t>(&expected);
        ops.back().op.cas.desired = desired;

        // read node
        ops.push_back(RDMAOp{RDMAOpType::READ, node_address});
        ops.back().op.read.buffer = reinterpret_cast<uint8_t*>(&node);
        ops.back().op.read.bytes = sizeof(InternalNode);

        assert(rdma_mgr_->perform_batch(ops));

        // successful lock
        if (expected != 0) {
            continue;
        }
        assert(node.header.level == level);

        if (key < node.header.fence.first || key >= node.header.fence.second) {
            // release lock and retry
            assert(rdma_release_lock(*rdma_mgr_, lock_address));
            continue;
        }

        // Search for an existing key
        bool found = false;
        for (int i = node.header.last_index; i >= 0; i--) {
            if (node.entries[i].key == key) {
                found = true;
                break;
            }
        }

        std::vector<RDMAOp> batch_ops;

        bool need_split = !found && (node.header.last_index + 1 >= branch_factor);
        if (need_split) {
            split_internal_node(node_address, node, key, new_child_addr);
        }
        else if (!found) {
            // Search for the right spot to insert
            size_t index = 0;
            for (int i = node.header.last_index - 1; i >= 0; i--) {
                if (node.entries[i].key < key) {
                    index = i + 1;
                    break;
                }
            }
            assert(node.header.last_index + 1 < branch_factor);
            for (size_t i = node.header.last_index + 1; i > index; i--) {
                node.entries[i - 1].key = node.entries[i - 2].key;
                node.entries[i].child = node.entries[i - 1].child;
            }
            assert(index + 1 < branch_factor);
            node.entries[index + 1].child = new_child_addr;
            node.entries[index].key = key;
            node.header.last_index++;

            // Write back node
            batch_ops.push_back(RDMAOp{RDMAOpType::WRITE, node_address});
            batch_ops.back().op.write.buffer = reinterpret_cast<const uint8_t*>(&node);
            batch_ops.back().op.write.bytes = sizeof(InternalNode);
        }

        // release lock and write if needed
        batch_ops.push_back(get_release_node_op(node_address));
        assert(rdma_mgr_->perform_batch(batch_ops));

        return true;
    }
    return false; // deadlock???
}

bool FaunusIndex::split_internal_node(GlobalAddress node_address, InternalNode& node, const Key& key, const GlobalAddress& new_child_addr) {
    // copy entries
    std::vector<InternalEntry> entries;
    for (size_t i = 0; i <= node.header.last_index; i++) {
        entries.push_back(node.entries[i]);
    }
    // add new entry
    entries.emplace_back();

    // search for the right spot
    size_t index = 0;
    for (int i = node.header.last_index - 1; i >= 0; i--) {
        if (node.entries[i].key < key) {
            index = i + 1;
            break;
        }
    }

    // shift entries to the right
    for (size_t i = entries.size() - 1; i > index; i--) {
        entries[i - 1].key = entries[i - 2].key;
        entries[i].child = entries[i - 1].child;
    }

    entries[index + 1].child = new_child_addr;
    entries[index].key = key;

    InternalNode new_node{};
    new_node.header.level = node.header.level;

    assert(entries.size() == branch_factor + 1);
    size_t middle = branch_factor / 2;

    Key middle_key = entries[middle].key;

    // Distribute entries
    for (size_t i = middle + 1; i < entries.size(); i++) {
        new_node.entries[i - middle - 1].key = entries[i].key;
        new_node.entries[i - middle - 1].child = entries[i].child;
        if (i < branch_factor) {
            node.entries[i] = {};
        }
    }

    for (size_t i = 0; i <= middle; i++) {
        node.entries[i] = entries[i];
    }

    node.header.last_index = middle;
    assert(node.header.last_index < branch_factor - 1);
    new_node.header.last_index = branch_factor - middle - 1;

    // Update fences
    new_node.header.fence = {middle_key, node.header.fence.second};
    node.header.fence.second = middle_key;

    // Allocate new internal node
    GlobalAddress new_node_address = allocator_->allocate(sizeof(InternalNode));

    std::vector<GlobalAddress> addresses{new_node_address, node_address};
    std::vector<InternalNode> leaves{new_node, node};
    assert(rdma_write_batch(*rdma_mgr_, addresses, leaves));

    // Insert new key
    bool success = insert_internal_entry(middle_key, new_node_address, node.header.level + 1);
    assert(success);

    return false;
}

bool FaunusIndex::setup_new_root(const Key& key, GlobalAddress right_child, size_t level) {
    // allocate new root
    GlobalAddress new_root_address = allocator_->allocate(sizeof(InternalNode));
    InternalNode new_root{};
    new_root.header.level = level;
    new_root.header.fence = {Key::min(), Key::max()};
    new_root.header.last_index = 1;

    new_root.entries[0].key = key;
    new_root.entries[0].child = get_root_offset();
    new_root.entries[1].child = right_child;

    // NOTE: assuming that child fences are already correct
    // NOTE: writing the new root and updating the root offset can be done in the same RTT, negligible
    // write new root
    bool success = rdma_write_object(*rdma_mgr_, new_root_address, new_root);
    assert(success);

    // update root pointer
    success = update_root_offset(new_root_address);
    assert(success);
    
    return true;
}

uint64_t hash_int(uint64_t x) {
    x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ULL;
    x = (x ^ (x >> 27)) * 0x94d049bb133111ebULL;
    x = x ^ (x >> 31);
    return x;
}

bool FaunusIndex::request_smo(FaunusMaintenanceRPC::OpType op, GlobalAddress leaf_address) {
#ifdef FAUNUS_MAINTENANCE_ENABLED
    // Prefer queued-sets if available (prevents duplicates)
    // size_t num_queued_sets = num_maintenance_queued_sets();
    // // size_t num_queues = num_maintenance_queues();
    // if (num_queued_sets > 0) {
    //     size_t queue_idx = std::hash<uint64_t>{}(leaf_address.raw) % num_queued_sets;
    //     auto queued_set = get_maintenance_queued_set(queue_idx);
    //     if (queued_set) {
    //         FaunusMaintenanceRPC rpc{.op=op, .leaf_address=leaf_address};
    //         {
    //             Profiler::Scoped scope("faunus.request_smo.try_enqueue");
    //             queued_set->try_enqueue(std::move(rpc));
    //             // bool enqueued = 
    //         }
    //         return true; // Always return true since either it was enqueued or already pending
    //     }
    // }
    
    // Fallback to regular maintenance queues (legacy behavior)
    size_t num_queues = num_maintenance_queues();
    if (num_queues == 0) return std::cout << "got num_queues 0" << std::endl, false;
    size_t queue_idx = hash_int(leaf_address.raw) % num_queues;
    auto queue = get_maintenance_queue(queue_idx);
    if (!queue) {
        return std::cout << "got null queue" << std::endl, false;
        // return false;
    }
    FaunusMaintenanceRPC rpc{.op=op, .leaf_address=leaf_address};
    {
        queue->enqueue(std::move(rpc));
    }
#endif // FAUNUS_MAINTENANCE_ENABLED
    return true;
}

void FaunusIndex::set_maintenance_queues(const std::vector<std::shared_ptr<FaunusMaintenanceQueue>>& queues) {
    s_faunus_maintenance_queues = queues;
}

std::vector<std::shared_ptr<FaunusMaintenanceQueue>> FaunusIndex::create_maintenance_queues(size_t num_queues) {
    std::vector<std::shared_ptr<FaunusMaintenanceQueue>> queues;
    for (size_t i = 0; i < num_queues; i++) {
        queues.push_back(std::make_shared<FaunusMaintenanceQueue>());
    }
    return queues;
}

std::shared_ptr<FaunusMaintenanceQueue> FaunusIndex::get_maintenance_queue(size_t cs_id) {
    if (cs_id < s_faunus_maintenance_queues.size())
        return s_faunus_maintenance_queues[cs_id];
    return nullptr;
}

void FaunusIndex::stop_maintenance(size_t threads_per_queue) {
    for (auto& queue : s_faunus_maintenance_queues) {
        if (!queue) continue;
        for (size_t i = 0; i < threads_per_queue; ++i) {
            FaunusMaintenanceRPC rpc{};
            rpc.op = FaunusMaintenanceRPC::STOP;
            queue->enqueue(std::move(rpc));
        }
    }
    stop_maintenance_queued_sets(threads_per_queue);
}

size_t FaunusIndex::num_maintenance_queues() {
    return s_faunus_maintenance_queues.size();
}

// Queued-set methods for duplicate prevention
std::vector<std::shared_ptr<FaunusMaintenanceQueuedSet>> FaunusIndex::create_maintenance_queued_sets(size_t num_queues) {
    std::vector<std::shared_ptr<FaunusMaintenanceQueuedSet>> queued_sets;
    for (size_t i = 0; i < num_queues; i++) {
        queued_sets.push_back(std::make_shared<FaunusMaintenanceQueuedSet>());
    }
    return queued_sets;
}

void FaunusIndex::set_maintenance_queued_sets(const std::vector<std::shared_ptr<FaunusMaintenanceQueuedSet>>& queued_sets) {
    s_faunus_maintenance_queued_sets = queued_sets;
}

std::shared_ptr<FaunusMaintenanceQueuedSet> FaunusIndex::get_maintenance_queued_set(size_t cs_id) {
    if (cs_id < s_faunus_maintenance_queued_sets.size())
        return s_faunus_maintenance_queued_sets[cs_id];
    return nullptr;
}

void FaunusIndex::stop_maintenance_queued_sets(size_t threads_per_queue) {
    for (auto& queued_set : s_faunus_maintenance_queued_sets) {
        if (!queued_set) continue;
        for (size_t i = 0; i < threads_per_queue; ++i) {
            FaunusMaintenanceRPC rpc{};
            rpc.op = FaunusMaintenanceRPC::STOP;
            // Use regular enqueue for STOP commands to ensure they're always processed
            queued_set->enqueue(std::move(rpc));
        }
    }
}

size_t FaunusIndex::num_maintenance_queued_sets() {
    return s_faunus_maintenance_queued_sets.size();
}

void FaunusIndex::maintenance_worker(size_t cs_id, size_t thread_id) {
    std::cout << "Maintenance worker started (CS " << cs_id << ", thread " << thread_id << ") TID " << std::this_thread::get_id() << std::endl;
    
    // Verify that stats tracker is set up for cache hit/miss tracking
    if (stats_tracker_ == nullptr) {
        std::cout << "Warning: Maintenance worker has no stats tracker - cache statistics will not be recorded" << std::endl;
    } else {
        std::cout << "Maintenance worker has stats tracker enabled for cache hit/miss tracking" << std::endl;
    }
    
    // Try to get a queued-set first (preferred), then fall back to regular queue
    auto queued_set = get_maintenance_queued_set(cs_id);
    auto queue = get_maintenance_queue(cs_id);
    
    if (!queued_set && !queue) {
        return;
    }
    
    std::string queue_type = queued_set ? "queued-set" : "queue";
    
    size_t operations_processed = 0;
    
    while (true) {
        FaunusMaintenanceRPC rpc;
        
        // Dequeue from the appropriate source
        if (queued_set) {
            rpc = queued_set->wait_dequeue();
        } else {
            rpc = queue->wait_dequeue();
        }
        
        if (rpc.op == FaunusMaintenanceRPC::STOP) {
            break;
        }
        
        bool result = false;
        auto start_time = std::chrono::high_resolution_clock::now();
        
        // Track maintenance operations as Insert operations for cache statistics
        if (stats_tracker_) {
            stats_tracker_->begin_operation(OperationKind::Insert);
        }
        
        switch (rpc.op) {
            case FaunusMaintenanceRPC::SPLIT:
                // split_leaf internally calls find_node which tracks cache statistics
                result = split_leaf(rpc.leaf_address);
                operations_processed++;
                break;
            case FaunusMaintenanceRPC::MERGE:
                // result = merge_leaf(rpc.leaf_address);
                assert(false && "Merge not implemented yet");
                break;  
            case FaunusMaintenanceRPC::STOP:
                return;
        }
        
        auto end_time = std::chrono::high_resolution_clock::now();
        auto latency_us = std::chrono::duration<double, std::micro>(end_time - start_time).count();
        
        // Complete operation tracking
        if (stats_tracker_) {
            stats_tracker_->end_operation(latency_us, result);
        }
        
        rpc.result.set_value(result);
    }
    
    std::cout << "Maintenance worker (CS " << cs_id << ", thread " << thread_id 
              << ") processed " << operations_processed << " operations" << std::endl;
    
    Profiler::publish_thread_stats();
}

void FaunusIndex::print_tree(size_t offset, int depth, bool show_kv) {
    if (depth == 0) {
        offset = get_root_offset().raw;
        std::cout << "FaunusIndex Tree Structure:" << std::endl;
    }
    // Print the tree recursively from the given node offset
    if (depth > 32) {
        std::cout << std::setw(depth * 2) << " " << "[Max depth exceeded]" << std::endl;
        return;
    }
    // Read node header to determine if leaf or internal
    InternalNode node;
    if (!rdma_read_object(*rdma_mgr_, GlobalAddress(offset), node)) {
        std::cout << std::setw(depth * 2) << " " << "[Failed to read node at offset " << offset << "]" << std::endl;
        return;
    }
    std::cout << std::setw(depth * 2) << " " << "Node @ offset 0x" << std::hex << offset << std::dec << ": level=" << node.header.level
              << ", lock=" << node.header.lock << ", fence=[" << node.header.fence.first << ", " << node.header.fence.second << "]"
              << ", last_index=" << node.header.last_index << std::endl;
    for (size_t i = 0; i <= node.header.last_index && i < branch_factor; ++i) {
        std::cout << std::setw(depth * 2 + 2) << " " << "Entry " << i << ": key=" << node.entries[i].key << ", child=" << node.entries[i].child << std::endl;
    }
    // If this is a leaf node (level == 0), print leaf contents
    if (node.header.level == 0) {
        LeafNode leaf;
        if (!rdma_read_object(*rdma_mgr_, GlobalAddress(offset), leaf)) {
            std::cout << std::setw(depth * 2) << " " << "[Failed to read leaf at offset " << offset << "]" << std::endl;
            return;
        }
        std::cout << std::setw(depth * 2 + 2) << " " << "Leaf contents:" << std::endl;
        for (size_t i = 0; i < branch_factor; ++i) {
            const auto& kvb = leaf.kv_blocks[i];
            std::cout << std::setw(depth * 2 + 4) << " " << "KVBlock " << i << ": fp=" << kvb.getFingerprint()
                      << ", addr=" << kvb.getAddr() << ", free=" << kvb.isFree() << ", lock=" << kvb.isLocked();
            if (show_kv && !kvb.isFree() && kvb.getAddr().raw != 0) {
                KVItem item;
                if (rdma_read_object(*rdma_mgr_, kvb.getAddr(), item)) {
                    std::cout << ", key=" << item.key << ", value=" << item.value;
                    assert((item.key >= leaf.header.fence.first) && (item.key < leaf.header.fence.second));
                } else {
                    std::cout << ", [Failed to read KVItem]";
                }
            }
            std::cout << std::endl;
        }
        return;
    }
    // Otherwise, recursively print children
    for (size_t i = 0; i <= node.header.last_index && i < branch_factor; ++i) {
        print_tree(node.entries[i].child.raw, depth + 1, show_kv);
    }
}
