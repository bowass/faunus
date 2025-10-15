#include "faunus_index.hpp"
#include "index_cache.hpp"
#include "../util/maintenance_queue.hpp"
#include <cassert>
#include <cstring>
#include <cassert>
#include <thread>
#include <vector>
#include <atomic>
#include <map>
#include <iomanip>
#include <chrono>

#include "../util/logging.hpp"
#include "../util/profiler.hpp"

using namespace faunus_index_internal;

std::vector<std::shared_ptr<FaunusMaintenanceQueue>> FaunusIndex::s_faunus_maintenance_queues;
std::vector<std::shared_ptr<FaunusMaintenanceQueuedSet>> FaunusIndex::s_faunus_maintenance_queued_sets;

FaunusIndex::FaunusIndex(std::shared_ptr<RDMAManager> rdma_mgr, std::shared_ptr<LocalAllocator> allocator, GlobalAddress root_offset_pointer, std::shared_ptr<faunus_index_internal::IndexCache> cache)
    : rdma_mgr_(rdma_mgr), allocator_(allocator), cache_(cache), root_offset_pointer_(root_offset_pointer) {
    LOG_DEBUG("FaunusIndex created with cache: " << (cache_ ? "enabled" : "disabled"));
}

bool FaunusIndex::initialize() {
    root_offset_pointer_ = allocator_->allocate(sizeof(GlobalAddress));
    GlobalAddress root_offset = allocator_->allocate(sizeof(LeafNode));
    bool success;

    LeafNode root{};
    root.header.fence = {Key::min(), Key::max()};
    LOG_INFO("Root node: " << root);
    success = rdma_write_object(*rdma_mgr_, root_offset, root);
    if (!success) {
        LOG_ERROR("Failed to write root node during initialization");
        return false;
    }

    success = update_root_offset(root_offset);

    return success;
}

std::set<size_t> FaunusIndex::get_required_sizes() {
    std::set<size_t> v{sizeof(GlobalAddress), sizeof(Value), sizeof(LeafNode), sizeof(InternalNode), sizeof(KVItem)};
    {
        std::ostringstream oss;
        oss << "FaunusIndex requires sizes: ";
        for (auto s : v) oss << s << " ";
        LOG_DEBUG(oss.str());
    }
    return v;
}

GlobalAddress FaunusIndex::get_root_offset_pointer() const {
    LOG_DEBUG("Root offset pointer is at " << std::hex << root_offset_pointer_.raw << std::dec);
    return root_offset_pointer_;
}

GlobalAddress FaunusIndex::get_root_offset() const {
    size_t root_offset;
    GlobalAddress gaddr = get_root_offset_pointer();
    LOG_DEBUG("Getting root offset from pointer at " << std::hex << gaddr.raw << std::dec);
    RDMAOp op{RDMAOpType::READ, gaddr};
    op.op.read.buffer = reinterpret_cast<uint8_t*>(&root_offset);
    op.op.read.bytes = sizeof(size_t);
    if (!rdma_mgr_->perform_op(op)) {
        throw std::runtime_error("Failed to read root offset"); 
    }
    LOG_DEBUG("Current root offset is " << std::hex << root_offset << std::dec);
    return GlobalAddress(root_offset);
}

GlobalAddress FaunusIndex::update_root_offset(GlobalAddress new_root_offset) {
    LOG_DEBUG("root_offset is updated to" << new_root_offset);
    GlobalAddress old_root_offset = get_root_offset();
    uint64_t expected = old_root_offset;
    uint64_t desired = new_root_offset;

    RDMAOp op{RDMAOpType::CAS, root_offset_pointer_};

    op.op.cas.expected = reinterpret_cast<uint64_t>(&expected);
    op.op.cas.desired = reinterpret_cast<uint64_t>(desired);

    // Retry CAS until success
    if (!rdma_mgr_->perform_op(op)) return false;
    assert(expected == old_root_offset);
    LOG_DEBUG("Root offset updated from " << std::hex << old_root_offset.raw << " to " << new_root_offset.raw << std::dec);
    return expected == old_root_offset;
}

FindNodeResult FaunusIndex::find_node(const Key& key, GlobalAddress& node_address, size_t level, bool from_smo) {
    Profiler::Scoped scope("faunus.find_node");
    bool use_cached_node = false;
    InternalNode cached_node;
    GlobalAddress cached_address;
    
    // Try cache first before starting tree traversal
    if (cache_) {
        Profiler::Scoped scope("faunus.find_node.cache");
        auto cached_result = cache_->search(key);
        if (cached_result.has_value()) {
            cached_address = cached_result.value().first;
            cached_node = cached_result.value().second;
            LOG_DEBUG("Cache hit for key " << key << " -> node " << cached_address);
            
            // Verify the cached node is still valid by checking if we're at the right level
            if (cached_node.header.level == level) {
                // Check if node is locked - if so, invalidate and fall through
                if (cached_node.header.lock && !from_smo) {
                    LOG_DEBUG("Cached node is locked, invalidating and falling through to tree traversal");
                    cache_->invalidate(cached_address);
                    cache_->invalidate_key_range(key); // Also invalidate any parent pointers
                } else {
                    // Verify the cached node still contains the key in its fence
                    if (key >= cached_node.header.fence.first && key < cached_node.header.fence.second) {
                        node_address = cached_address;
                        return FindNodeResult::FOUND;
                    } else {
                        LOG_DEBUG("Cached node fence no longer contains key, invalidating");
                        cache_->invalidate(cached_address);
                        cache_->invalidate_key_range(key); // Invalidate broader range due to fence change
                    }
                }
            } else if (cached_node.header.level < level) {
                LOG_DEBUG("Cached node level too low, probably root split, invalidating");
                cache_->invalidate_key_range(key);
                return FindNodeResult::NO_SUCH_LEVEL;
            } else {
                // cached_node.header.level > level, start traversal from cached node
                // But first verify the fence is still valid
                if (key >= cached_node.header.fence.first && key < cached_node.header.fence.second) {
                    use_cached_node = true;
                    node_address = cached_address;
                    LOG_DEBUG("Cached node level higher than target, starting traversal from cached node");
                } else {
                    LOG_DEBUG("Cached node fence invalid, invalidating and starting from root");
                    cache_->invalidate(cached_address);  // Invalidate specific entry
                    cache_->invalidate_key_range(key);   // Invalidate broader range
                }
            }
        }
    }
    
    // Start tree traversal from root or cached node
    if (!use_cached_node) {
        Profiler::Scoped scope("faunus.find_node.get_root_offset");
        node_address = get_root_offset();
    }
    bool in_root = !use_cached_node;
    
    while (true) {
        InternalNode node;
        
        // Use cached node on first iteration if available, otherwise read from RDMA
        if (use_cached_node) {
            node = cached_node;
            use_cached_node = false; // Only use cache on first iteration
        } else {
            rdma_read_object(*rdma_mgr_, node_address, node);
        }
        
        // Add to cache if it's at the target cache level (one level above leaves)
        if (cache_ && node.header.level == cache_->get_target_level()) {
            cache_->add(node_address, node);
            LOG_DEBUG("Added node " << node_address << " to cache (level " << node.header.level << ")");
        }
        
        LOG_DEBUG("At node " << node_address << " with header: " << node.header);
        
        // if locked and not from smo, or another smo has locked an ancestor
        if (node.header.lock && (!from_smo || node.header.level > level)) {
            LOG_DEBUG("Node is locked, fail");
            // If this node is locked, invalidate any cache entries that might depend on it
            if (cache_) {
                cache_->invalidate(node_address);     // Invalidate specific locked node
                cache_->invalidate_key_range(key);    // Invalidate broader range
            }
            return FindNodeResult::LOCKED;
        }
        
        // found node at the desired level
        if (node.header.level == level) {
            return FindNodeResult::FOUND;
        }
        
        // Leaf root and we split
        if (node.header.level < level) {
            LOG_DEBUG("Leaf root found, probably in a root split");
            // we should only get here if we split a root
            assert(in_root);
            // Invalidate cache since tree structure has changed
            if (cache_) {
                cache_->invalidate(node_address);     // Invalidate specific stale root
                cache_->invalidate_key_range(key);    // Invalidate broader range
            }
            return FindNodeResult::NO_SUCH_LEVEL;
        }
        
        // find next level
        // TODO: binary search
        bool found = false;
        GlobalAddress next_address;
        {
        Profiler::Scoped scope("faunus.find_node.search_next");
        for (size_t i = 0; !found && (i <= node.header.last_index); i++) {
            if (key < node.entries[i].key) {
                next_address = node.entries[i].child;
                found = true;
            }
        }
        }
        if (!found) {
            next_address = node.entries[node.header.last_index].child;
        }
        
        // Validate that the next node we're about to traverse contains the key
        // If not, the cache or tree structure is inconsistent
        if (cache_ && next_address.raw != 0) {
            // Read the next node header to validate fence (only header, not full node)
            Header next_header;
            RDMAOp op{RDMAOpType::READ, next_address + offsetof(InternalNode, header)};
            op.op.read.buffer = reinterpret_cast<uint8_t*>(&next_header);
            op.op.read.bytes = sizeof(Header);
            
            if (rdma_mgr_->perform_op(op)) {
                // Check if the key is actually within the fence of the next node
                if (key < next_header.fence.first || key >= next_header.fence.second) {
                    LOG_DEBUG("Next node fence does not contain key - tree structure inconsistent, invalidating cache");
                    cache_->invalidate(node_address);      // Invalidate current node
                    cache_->invalidate_key_range(key);     // Invalidate broader range
                    // Restart the search from root
                    node_address = get_root_offset();
                    in_root = true;
                    continue;
                }
            }
        }
        
        node_address = next_address;
        in_root = false;
    }
    return FindNodeResult::UNKNOWN; // should never reach here
}

std::vector<std::pair<size_t, GlobalAddress>> get_fingerprint_collisions(const LeafNode& leaf, const Fingerprint& fp, bool& found_smo) {
    std::vector<std::pair<size_t, GlobalAddress>> candidates;
    found_smo = false;
    for (size_t i = 0; i < branch_factor; i++) {
        if (leaf.kv_blocks[i].isLocked()) {
            // std::cout << "Found SMO true" << std::endl;
            found_smo = true;
        }
        if (!leaf.kv_blocks[i].isFree() && leaf.kv_blocks[i].getFingerprint() == fp) {
            candidates.emplace_back(i, leaf.kv_blocks[i].getAddr());
        }
    }
    // LOG_WARN("Found SMO? " << found_smo);
    return candidates;
}

std::vector<std::pair<size_t, KVItem>> FaunusIndex::get_candidate_kvs(const LeafNode& leaf, const Fingerprint& fp, bool& success, bool from_insert/* = false*/, bool from_read/* = false*/) {
    Profiler::Scoped scope("faunus.get_candidate_kvs");
    bool found_smo = false;
    auto candidates_entries = get_fingerprint_collisions(leaf, fp, found_smo);

    if (found_smo && !from_read) {
        // LOG_WARN("Found SMO in candidates, fail");
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
    // TODO: optional - for non-read, can read only keys
    std::vector<KVItem> candidate_kvs;
    assert(rdma_read_batch(*rdma_mgr_, candidates_pointers, candidate_kvs));
    assert(candidate_kvs.size() == candidates_entries.size());

    std::vector<std::pair<size_t, KVItem>> candidates;
    for (size_t i = 0; i < candidates_entries.size(); i++) {
        candidates.emplace_back(candidates_entries[i].first, candidate_kvs[i]);
    }
    return candidates;
}

bool FaunusIndex::handle_local_remove_dupes(const Key& key, GlobalAddress leaf_address, const LeafNode& leaf, bool& found, Value& value_out, bool from_insert/* = false*/, bool from_read/* = false*/) {
    Profiler::Scoped scope("faunus.handle_remove_dupes");
    if (key < leaf.header.fence.first || key >= leaf.header.fence.second) {
        LOG_DEBUG("Key " << key << " out of fence (" << leaf.header.fence.first << ", " << leaf.header.fence.second << "), fail");
        return false;
    }
    // readers do not care if leaf is locked
    if (leaf.header.lock && !from_read) {
        LOG_DEBUG("Leaf is locked, fail");
        return false;
    }

    Fingerprint fp(key);
    bool success;
    // pairs of (index in leaf, KVItem)
    auto candidate_kvs = get_candidate_kvs(leaf, fp, success, from_insert, from_read);
    if (candidate_kvs.empty()) {
        if (success && from_read) {
            LOG_ERROR("No candidate KVs found for key " << key << " with fingerprint " << fp << " in leaf " << leaf_address);
        }
        return success;
    }
    // check for exact key match
    for (size_t i = 0; i < candidate_kvs.size(); i++) {
        if (candidate_kvs[i].second.key == key) {
            if (!found) {
                found = true;
                value_out = candidate_kvs[i].second.value;
                LOG_INFO("Found key " << key << " " << Fingerprint(key) << " in leaf " << leaf_address << " at index " << candidate_kvs[i].first << " with value " << value_out);
            }
            else {
                size_t index = candidate_kvs[i].first;
                GlobalAddress kbvlock_address = leaf_address + OFFSET_OF_ARRAY_ELEM(LeafNode, kv_blocks, index);
                // set random value in KVBlock
                RDMAOp op{RDMAOpType::CAS, kbvlock_address};
                op.op.cas.expected = reinterpret_cast<uint64_t>(&leaf.kv_blocks[index]);
                KVBlock new_kvb = faunus_util::thread_rand64();
                // preserve lock and free bits
                new_kvb.setLocked(leaf.kv_blocks[index].isLocked());
                new_kvb.setFree(leaf.kv_blocks[index].isFree());
                op.op.cas.desired = new_kvb.raw;
                LOG_DEBUG("Removing duplicate entry at index " << index << " in leaf " << leaf_address << " by setting fingerprint to random value " << op.op.cas.desired);
                assert(rdma_mgr_->perform_op(op));
                // TODO: free KVItem space
            }
        }
    }
    return true;
}

bool FaunusIndex::read(const Key& key, Value& value_out) {
    GlobalAddress leaf_address;
    bool success;
    for (size_t attempt = 0; attempt < 1000000; attempt++) {
        // find leaf address
        auto find_result = find_node(key, leaf_address);
        assert(find_result != FindNodeResult::UNKNOWN && find_result != FindNodeResult::NO_SUCH_LEVEL);
        if (find_result != FindNodeResult::FOUND) continue; // retry

        // read leaf
        LeafNode leaf;
        rdma_read_object(*rdma_mgr_, leaf_address, leaf);

        // search for candidate entries
        // remove duplicate entires
        // returns found entries in found and value_out
        bool found = false;
        success = handle_local_remove_dupes(key, leaf_address, leaf, found, value_out, false, true);
        if (!success) continue; // retry
        return found;
    }
    // deadlock???
    assert(false);
    return false;    
}

bool FaunusIndex::insert(const Key& key, const Value& value) {
    Profiler::Scoped total_scope("faunus.insert.total");
    // allocate KVItem + read leaf
    LOG_WARN("Starting insert of key " << key << " with value " << value);
    bool success;
    bool wrote_kvitem = false;
    Fingerprint fp(key);
    KVItem kvitem{key, value};
    GlobalAddress leaf_address;
    GlobalAddress kvitem_address = allocator_->allocate(sizeof(KVItem));
    KVBlock kvb{fp, kvitem_address, false, false};
    LeafNode leaf;
    for (size_t attempt = 0; attempt < 1000000; attempt++) {
        // Yield after continue to reduce contention
        if (attempt > 0) {
            // std::cout << "Insert retry attempt " << attempt << " for key " << key << std::endl;
            std::this_thread::yield();
            // std::this_thread::sleep_for(std::chrono::microseconds(10));
        }
        // LOG_WARN("HEREH " << attempt);
        // find leaf address
        LOG_DEBUG("Before find_node");
        FindNodeResult find_result = find_node(key, leaf_address);
        assert(find_result != FindNodeResult::UNKNOWN && find_result != FindNodeResult::NO_SUCH_LEVEL);
        LOG_DEBUG("After find_node: " << find_result);
        if (find_result != FindNodeResult::FOUND) {
            // print_tree();
            if (attempt > 10000) LOG_ERROR("Find result of " << key << ", attempt = " << attempt);
            else LOG_WARN("Find result of " << key << ", attempt = " << attempt);
            continue; // retry
        }

        // read leaf and write KVItem if needed
        std::vector<RDMAOp*> ops;
        // read leafkvitem_address
        ops.push_back(new RDMAOp{RDMAOpType::READ, leaf_address});
        ops.back()->op.read.buffer = reinterpret_cast<uint8_t*>(&leaf);
        ops.back()->op.read.bytes = sizeof(LeafNode);

        // write KVItem to already allocated space
        if (!wrote_kvitem) {
            ops.push_back(new RDMAOp{RDMAOpType::WRITE, kvitem_address});
            ops.back()->op.write.buffer = reinterpret_cast<uint8_t*>(&kvitem);
            ops.back()->op.write.bytes = sizeof(KVItem);
            wrote_kvitem = true;
        }

        LOG_DEBUG("Performing batch RDMA operations for insert");

        {
            Profiler::Scoped scope("faunus.insert.perform_batch");
            assert(rdma_mgr_->perform_batch(ops));
        }
        assert(leaf.header.level == 0);

        LOG_DEBUG("Read + write KB batch done");
        
        // smo or bad range - retry
        if (leaf.header.lock || key < leaf.header.fence.first || key >= leaf.header.fence.second) {
            if (attempt > 10000) LOG_ERROR("Leaf address: " << leaf_address << ", lock: " << leaf.header.lock << ". Key: " << key << ", Fence: (" << leaf.header.fence.first << ", " << leaf.header.fence.second << "), retrying");
            else LOG_WARN("Leaf lock: " << leaf.header.lock << ". Key: " << key << ", Fence: (" << leaf.header.fence.first << ", " << leaf.header.fence.second << "), retrying");
            if (cache_) {
                cache_->invalidate_key_range(key);     // Invalidate broader range including parent pointers
            }
            continue;
        }

        LOG_DEBUG("Getting candidate KVs");
        // using from_insert == false, we do not care about duplicates here
        auto candidate_kvs = get_candidate_kvs(leaf, fp, success, false, false);
        // smo - retry
        if (candidate_kvs.empty() && !success) {
            if (attempt > 10000) LOG_WARN("Stuck here, leaf address: " << leaf_address);
            else LOG_WARN("Stuck here, leaf address: " << leaf_address);
            // SMO detected, invalidate cache since tree structure is changing
            if (cache_) {
                cache_->invalidate(leaf_address);
                cache_->invalidate_key_range(key);
            }
            continue;
        }
        // search for an existing key
        for (size_t i = 0; i < candidate_kvs.size(); i++) {
            // key already exists - cancel insertion
            if (candidate_kvs[i].second.key == key) {
                assert(0);
                return false;
            }
        }

        success = false;
        for (size_t i = 0; !success && (i < branch_factor); i++) {
            // the re-read leaf contained a locked KVBlock
            // break to continue in the mainloop
            if (leaf.kv_blocks[i].isLocked()) {
                LOG_DEBUG("SMO Detected");
                break;
            }
            if (leaf.kv_blocks[i].isFree()) {
                Profiler::Scoped cas_block_scope("faunus.insert.cas_block");
                LOG_WARN("Free entry: " << i << ", CAS");
                // update KVBlock using CAS and re-read the leaf back-to-back
                GlobalAddress kvblock_address = leaf_address + OFFSET_OF_ARRAY_ELEM(LeafNode, kv_blocks, i);
                std::vector<RDMAOp*> ops;
                ops.push_back(new RDMAOp{RDMAOpType::CAS, kvblock_address});
                uint64_t expected = leaf.kv_blocks[i].raw;
                ops.back()->op.cas.expected = reinterpret_cast<uint64_t>(&expected);
                ops.back()->op.cas.desired = kvb.raw;

                ops.push_back(new RDMAOp{RDMAOpType::READ, leaf_address});
                ops.back()->op.read.buffer = reinterpret_cast<uint8_t*>(&leaf);
                ops.back()->op.read.bytes = sizeof(LeafNode);

                LOG_WARN("Inserting new entry at index " << i << " in leaf " << leaf_address << " with KVBlock " << kvb.raw);
                uint64_t old_excpected = expected;
                {
                    Profiler::Scoped scope("faunus.insert.cas_batch");
                    assert(rdma_mgr_->perform_batch(ops));
                }

                // successful insertion
                if (old_excpected == expected) {
                    LOG_WARN("Successfully inserted new entry at index " << i << " in leaf " << leaf_address << " with KVBlock " << kvb.raw);
                    success = true;
                }
                else {
                    LOG_WARN("CAS Failed: " << static_cast<KVBlock>(expected) << " (expected was " << static_cast<KVBlock>(old_excpected) << ")" );
                }

                // after re-reading - assert leaf is still valid
                if (leaf.header.lock || key < leaf.header.fence.first || key >= leaf.header.fence.second) {
                    LOG_DEBUG("After re-read, leaf is locked or key out of fence, retry");
                    // Invalidate cache since leaf state changed after our operation
                    if (cache_) {
                        cache_->invalidate(leaf_address);
                        cache_->invalidate_key_range(key);
                    }
                    break;
                }
            }
        }

        // did not find a free KVBlock
        // successful insertions will incur SMO
        // continue retrying
        if (!success) {
            LOG_WARN("Failed to insert key " << key << " in leaf " << leaf_address << ", no free KVBlock found");
            continue;
        }
        // leaf is already updated
        // SMO already dealt with duplicates
        if (leaf.header.lock || key < leaf.header.fence.first || key >= leaf.header.fence.second) {
            // Even though insertion succeeded, leaf changed - invalidate cache
            if (cache_) {
                cache_->invalidate(leaf_address);
                cache_->invalidate_key_range(key);
            }
            return true;
        }
        // trigger SMO if needed
        size_t num_used = 0;
        for (size_t i = 0; i < branch_factor; i++) {
            if (!leaf.kv_blocks[i].isFree()) num_used++;
        }
        // TODO: change threshold to constant, and make it configurable
        if (num_used > branch_factor * 3 / 4) {
            LOG_DEBUG("Leaf at " << leaf_address << " is over 75% full, need SMO");
            {
                Profiler::Scoped scope("faunus.insert.request_smo");
                success = request_smo(FaunusMaintenanceRPC::SPLIT, leaf_address);
            }
            assert(success);
            // Invalidate cache after SMO since tree structure will change
            if (cache_) {
                cache_->invalidate(leaf_address);
                cache_->invalidate_key_range(key);
            }
            return true;
        }
        // else, handle duplicates manually
        else {
            bool found = false;
            Value dummy;
            handle_local_remove_dupes(key, leaf_address, leaf, found, dummy, true, false);
            return true;
        }
    }
    std::cout << "Probably deadlocked after 100000 attempts to insert key " << key << ", leafnode " << leaf_address << " at thread " << std::this_thread::get_id() << std::endl;
    assert(false);
    return false; // deadlock???
}

bool FaunusIndex::update(const Key& key, const Value& value) {
    return false;
}

bool FaunusIndex::del(const Key& key) {
    return false;
}

bool FaunusIndex::trylock_node(GlobalAddress node_address) {
    // LOG_WARN("Trylock node at " << node_address);
    return rdma_try_acquire_lock(*rdma_mgr_, node_address + offsetof(LeafNode, header) + offsetof(Header, lock));
}

bool FaunusIndex::release_node(GlobalAddress node_address) {
    // LOG_WARN("Release node at " << node_address);
    return rdma_release_lock(*rdma_mgr_, node_address + offsetof(LeafNode, header) + offsetof(Header, lock));
}

bool FaunusIndex::lock_unlock_kvblocks(GlobalAddress leaf_address, bool to_lock) {
    std::vector<RDMAOp*> ops;
    for (size_t i = 0; i < branch_factor; i++) {
        ops.push_back(new RDMAOp{RDMAOpType::FAA, leaf_address + OFFSET_OF_ARRAY_ELEM(LeafNode, kv_blocks, i)});
        LOG_WARN("Locking KVBlock at " << ops.back()->addr);
        ops.back()->op.faa.increment = to_lock ? 1 : -1; // set lock bit
    }
    return rdma_mgr_->perform_batch(ops);
}

bool FaunusIndex::split_leaf(GlobalAddress leaf_address) {
    Profiler::Scoped total_scope("faunus.split_leaf");
    // Lock the leaf
    // LOG_WARN("Splitting leaf at " << leaf_address);
    bool success = trylock_node(leaf_address);
    if (!success) {
        LOG_DEBUG("SMO Already executing on leaf " << leaf_address << ", fail to lock");
        return false;
    }

    // TODO: coalesce with the following leaf read in a single RTT
    // Acquire KVBlock locks
    success = lock_unlock_kvblocks(leaf_address, true);
    assert(success);

    // Read leaf
    LeafNode leaf;
    rdma_read_object(*rdma_mgr_, leaf_address, leaf);
    assert(leaf.header.lock);

    // Read all KVItems in the leaf
    std::vector<GlobalAddress> item_pointers;
    for (size_t i = 0; i < branch_factor; i++) {
        assert(leaf.kv_blocks[i].isLocked());
        if (!leaf.kv_blocks[i].isFree()) {
            item_pointers.push_back(leaf.kv_blocks[i].getAddr());
        }
    }

    // TODO: it is sufficient to read only keys for split
    std::vector<KVItem> leaf_kvs;
    assert(rdma_read_batch(*rdma_mgr_, item_pointers, leaf_kvs));

    // Remove duplicate keys and sort
    std::map<Key, KVBlock> entries_map;
    for (size_t i = 0; i < leaf_kvs.size(); i++) {
        entries_map[leaf_kvs[i].key] = leaf.kv_blocks[i];
    }

    // Test - validating fence
    for (auto& [k, v] : entries_map) {
        if (!(k >= leaf.header.fence.first && k < leaf.header.fence.second)) {
            LOG_ERROR("LeafNode: " << leaf_address << " Key " << k << " out of fence (" << leaf.header.fence.first << ", " << leaf.header.fence.second << ")");
            // print_tree();
        }
        assert(k >= leaf.header.fence.first && k < leaf.header.fence.second);
    }

    std::vector<std::pair<Key, KVBlock>> entries(entries_map.begin(), entries_map.end());
    LOG_DEBUG("After removing duplicates, " << entries.size() << " unique keys remain");

    if (entries.size() <= branch_factor / 2) {
        LOG_WARN("Not enough entries to split leaf " << leaf_address << ", only " << entries.size() << " unique keys found, need more than " << branch_factor / 2);
        // read leaf before releasing
        LeafNode test_leaf;
        rdma_read_object(*rdma_mgr_, leaf_address, test_leaf);
        assert(test_leaf.header.lock);
        for (size_t i = 0; i < branch_factor; i++) {
            assert(test_leaf.kv_blocks[i].isLocked());
        }

        // release kvblocks
        assert(lock_unlock_kvblocks(leaf_address, false));

        // TEST: re-read leaf and check if still locked
        rdma_read_object(*rdma_mgr_, leaf_address, test_leaf);
        assert(test_leaf.header.lock);
        for (size_t i = 0; i < branch_factor; i++) {
            assert(!test_leaf.kv_blocks[i].isLocked());
        }
        // release node
        assert(release_node(leaf_address));
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
        LOG_INFO("Odd number of entries, middle entry goes to new leaf");
        new_leaf.kv_blocks[middle] = entries.back().second;
        new_leaf.kv_blocks[middle].setLocked(false);
    }

    // debug print
    LOG_INFO("Splitting leaf " << leaf_address << " into new leaf " << new_leaf_address);
    LOG_INFO("Middle key: " << middle_key);
    for (size_t i = 0; i < entries.size(); i++) {
        LOG_INFO("Entry " << i << ": " << entries[i].first << " -> " << entries[i].second);
    }

    // Clear the rest of the entries
    for (size_t i = middle; i < branch_factor; i++) {
        // set to random value, unlocked, free
        leaf.kv_blocks[i] = faunus_util::thread_rand64();
        leaf.kv_blocks[i].setLocked(false);
        leaf.kv_blocks[i].setFree(true);
        if (i > middle || entries.size() % 2 == 0) {
            new_leaf.kv_blocks[i] = faunus_util::thread_rand64();
            new_leaf.kv_blocks[i].setLocked(false);
            new_leaf.kv_blocks[i].setFree(true);
        }
    }

    // Update fences
    new_leaf.header.fence = {middle_key, leaf.header.fence.second};
    leaf.header.fence.second = middle_key;

    // TODO: .sibling usage???
    // if did not find any, remove .sibling property

    // Write sibling leaf first
    success = rdma_write_object(*rdma_mgr_, new_leaf_address, new_leaf);
    assert(success);

    // print_tree();
    // LOG_WARN("I AM DOING IT");

    // Write back original leaf
    success = rdma_write_object(*rdma_mgr_, leaf_address, leaf);
    assert(success);

    // LOG_WARN("Wrote back split leaves");
    // print_tree();

    // LOG_WARN("New leaf header: " << new_leaf.header);
    // Insert new fence into parent
    success = insert_internal_entry(middle_key, new_leaf_address, 1);
    assert(success);
    // LOG_WARN("Inserted new fence into parent");

    // Release node lock
    assert(release_node(leaf_address));
    // Invalidate cache entries that might be affected by the split
    if (cache_) {
        // Invalidate any cached nodes that might contain keys in the affected range
        for (const auto& [key, kvblock] : entries_map) {
            cache_->invalidate_key_range(key);
        }
        LOG_DEBUG("Invalidated cache entries for split leaf range");
    }

    LOG_WARN("Finished splitting leaf at " << leaf_address << " into new leaf " << new_leaf_address);
    // print_tree();
    return true;
}

bool FaunusIndex::insert_internal_entry(const Key& key, GlobalAddress new_child_addr, size_t level) {
    Profiler::Scoped total_scope("faunus.insert_internal_entry");
    bool success;
    GlobalAddress node_address;
    InternalNode node;
    for (size_t attempt = 0; attempt < 1000000; attempt++) {
        if (attempt > 1000) {
            std::cout << "Attempting to find leaf address for key " << key << " at level " << level << ", attempt = " << attempt << std::endl;
        }
        // find leaf address
        LOG_DEBUG("Before find_node");
        // from SMO true!!!
        // LOG_WARN("Searching for interval node at level " << level << " for key " << key << " to insert new child " << new_child_addr);
        FindNodeResult find_result;
        {
            Profiler::Scoped scope("faunus.insert_internal_entry.find_node");
            find_result = find_node(key, node_address, level, true);
        }
        assert(find_result != FindNodeResult::UNKNOWN);
        if (find_result == FindNodeResult::NO_SUCH_LEVEL) {
            // need to create a new root
            LOG_DEBUG("No such level found, need to create a new root");
            return setup_new_root(key, new_child_addr, level);
        }
        if (find_result != FindNodeResult::FOUND) {
            LOG_WARN("Find node failed with result " << find_result << ", retrying");
            continue; // retry
        }
        LOG_DEBUG("After find_node: " << find_result);
        // print_tree();

        // Lock and read node
        std::vector<RDMAOp*> ops;
        // lock node
        uint64_t expected = 0;
        uint64_t desired = 1;
        GlobalAddress lock_address = node_address + offsetof(InternalNode, header) + offsetof(Header, lock);
        ops.push_back(new RDMAOp{RDMAOpType::CAS, lock_address});
        ops.back()->op.cas.expected = reinterpret_cast<uint64_t>(&expected);
        ops.back()->op.cas.desired = desired;

        // read node
        ops.push_back(new RDMAOp{RDMAOpType::READ, node_address});
        ops.back()->op.read.buffer = reinterpret_cast<uint8_t*>(&node);
        ops.back()->op.read.bytes = sizeof(InternalNode);

        LOG_DEBUG("Performing batch RDMA operations for insert_internal_entry");
        assert(rdma_mgr_->perform_batch(ops));

        // LOG_WARN("After locking and reading internal node at " << node_address << " with expected " << expected << " and desired " << desired);
        // print_tree();

        // successful lock
        if (expected != 0) {
            LOG_DEBUG("Node already locked, retrying");
            // LOG_WARN("SsSSS" << node_address << " " << node.header << " " << expected);
            // print_tree();
            continue;
        }
        assert(node.header.level == level);

        // LOG_WARN("Locked node at " << node_address << " with header: " << node.header);
        if (key < node.header.fence.first || key >= node.header.fence.second) {
            // LOG_DEBUG("Key " << key << " out of fence (" << node.header.fence.first << ", " << node.header.fence.second << "), retry");
            // release lock and retry
            assert(rdma_release_lock(*rdma_mgr_, lock_address));
            continue;
        }
        // LOG_WARN("Passed fence check");

        // print_tree();
        // Search for an existing key
        bool found = false;
        for (int i = node.header.last_index; i >= 0; i--) {
            // LOG_WARN("i = " << i << ", last_index = " << node.header.last_index);
            // LOG_WARN("Checking entry: i = " << i << ", key = " << key << ", node.entries[i].key = " << node.entries[i].key << " child = " << node.entries[i].child);
            if (node.entries[i].key == key) {
                // LOG_WARN("Found existing key in internal node, no need to insert: i = " << i << ", key = " << key);
                found = true;
                break;
            }
        }

        bool need_split = !found && (node.header.last_index + 1 >= branch_factor);
        // LOG_WARN("Need split: " << need_split << ", found existing key: " << found << ", last_index: " << node.header.last_index);
        if (need_split) {
            split_internal_node(node_address, node, key, new_child_addr);
        }
        else if (!found) {
            // LOG_WARN("Did not find existing key, inserting new entry");
            // Search for the right spot to insert
            size_t index = 0;
            for (int i = node.header.last_index - 1; i >= 0; i--) {
                if (node.entries[i].key < key) {
                    index = i + 1;
                    break;
                }
            }
            // LOG_WARN("Inserting new entry at index " << index << " in internal node " << node_address << " with key " << key << " and child " << new_child_addr);
            assert(node.header.last_index + 1 < branch_factor);
            for (size_t i = node.header.last_index + 1; i > index; i--) {
                // LOG_WARN("Shifting entry from index " << (i - 1) << " to index " << i);
                node.entries[i - 1].key = node.entries[i - 2].key;
                node.entries[i].child = node.entries[i - 1].child;
            }
            assert(index + 1 < branch_factor);
            node.entries[index + 1].child = new_child_addr;
            node.entries[index].key = key;
            // LOG_WARN("Inserted new entry at index " << index << " in internal node " << node_address << " with key " << key << " and child " << new_child_addr);
            node.header.last_index++;

            // Write back node
            success = rdma_write_object(*rdma_mgr_, node_address, node);
            assert(success);
        }

        // release lock
        assert(rdma_release_lock(*rdma_mgr_, lock_address));

        if (cache_ && !need_split) {
            cache_->invalidate_key_range(key);
            LOG_DEBUG("Invalidated cache entries for internal node update");
        }
        return true;
    }
    return false; // deadlock???
}

bool FaunusIndex::split_internal_node(GlobalAddress node_address, InternalNode& node, const Key& key, const GlobalAddress& new_child_addr) {
    // copy entries
    // std::vector<std::
    std::vector<InternalEntry> entries;
    for (size_t i = 0; i <= node.header.last_index; i++) {
        entries.push_back(node.entries[i]);
    }
    // add new entry
    entries.emplace_back();

    // Search for the right sport
    size_t index = 0;
    for (int i = node.header.last_index - 1; i >= 0; i--) {
        if (node.entries[i].key < key) {
            index = i + 1;
            break;
        }
    }

    // Shift entries to the right
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

    // Write new node first
    bool success = rdma_write_object(*rdma_mgr_, new_node_address, new_node);
    assert(success);

    // Write back original node
    success = rdma_write_object(*rdma_mgr_, node_address, node);
    assert(success);

    // Insert new key
    success = insert_internal_entry(middle_key, new_node_address, node.header.level + 1);
    assert(success);

    // Invalidate cache entries that might be affected by the internal node split
    if (cache_) {
        cache_->invalidate_key_range(key);
        cache_->invalidate_key_range(middle_key);
        LOG_DEBUG("Invalidated cache entries for internal node split");
    }

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

    // TODO: not updating child fences, should already be correct according to key
    // write new root
    bool success = rdma_write_object(*rdma_mgr_, new_root_address, new_root);
    assert(success);

    // update root pointer
    success = update_root_offset(new_root_address);
    assert(success);
    
    // Invalidate all cache entries since root has changed - tree structure completely changed
    // if (cache_) {
    //     cache_->invalidate_all();
    //     LOG_DEBUG("Invalidated all cache entries due to new root creation");
    // }
    
    // return false;
    return true;
}

bool FaunusIndex::request_smo(FaunusMaintenanceRPC::OpType op, GlobalAddress leaf_address) {
    // Prefer queued-sets if available (prevents duplicates)
    size_t num_queued_sets = num_maintenance_queued_sets();
    if (num_queued_sets > 0) {
        size_t queue_idx = std::hash<uint64_t>{}(leaf_address.raw) % num_queued_sets;
        auto queued_set = get_maintenance_queued_set(queue_idx);
        if (queued_set) {
            FaunusMaintenanceRPC rpc{.op=op, .leaf_address=leaf_address};
            {
                Profiler::Scoped scope("faunus.request_smo.try_enqueue");
                bool enqueued = queued_set->try_enqueue(std::move(rpc));
                LOG_DEBUG("SMO request for leaf " << leaf_address << " (op=" << op << ") " 
                         << (enqueued ? "enqueued" : "already pending") << " to queued-set " << queue_idx);
            }
            return true; // Always return true since either it was enqueued or already pending
        }
    }
    
    // Fallback to regular maintenance queues (legacy behavior)
    size_t num_queues = num_maintenance_queues();
    if (num_queues == 0) return false;
    size_t queue_idx = std::hash<uint64_t>{}(leaf_address.raw) % num_queues;
    auto queue = get_maintenance_queue(queue_idx);
    if (!queue) {
        return false;
    }
    FaunusMaintenanceRPC rpc{.op=op, .leaf_address=leaf_address};
    {
        Profiler::Scoped scope("faunus.request_smo.enqueue");
        queue->enqueue(std::move(rpc));
    }
    LOG_DEBUG("SMO request for leaf " << leaf_address << " (op=" << op << ") enqueued to regular queue " << queue_idx);
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
    
    // Try to get a queued-set first (preferred), then fall back to regular queue
    auto queued_set = get_maintenance_queued_set(cs_id);
    auto queue = get_maintenance_queue(cs_id);
    
    if (!queued_set && !queue) {
        LOG_ERROR("No maintenance queue or queued-set set for FaunusIndex (cs_id=" << cs_id << ")");
        return;
    }
    
    std::string queue_type = queued_set ? "queued-set" : "queue";
    LOG_INFO("FaunusIndex maintenance worker started (CS " << cs_id << ", thread " << thread_id << ", using " << queue_type << ")");
    
    while (true) {
        FaunusMaintenanceRPC rpc;
        
        // Dequeue from the appropriate source
        if (queued_set) {
            rpc = queued_set->wait_dequeue();
        } else {
            rpc = queue->wait_dequeue();
        }
        
        if (rpc.op == FaunusMaintenanceRPC::STOP) {
            LOG_INFO("FaunusIndex maintenance worker stopping (CS " << cs_id << ", thread " << thread_id << ")");
            break;
        }
        
        bool result = false;
        switch (rpc.op) {
            case FaunusMaintenanceRPC::SPLIT:
                LOG_WARN("Dealing with SPLIT request for leaf " << rpc.leaf_address);
                result = split_leaf(rpc.leaf_address);
                break;
            case FaunusMaintenanceRPC::MERGE:
                // result = merge_leaf(rpc.leaf_address);
                assert(false && "Merge not implemented yet");
                break;  
            case FaunusMaintenanceRPC::STOP:
                return;
        }
        rpc.result.set_value(result);
    }
    // Finalize cache stats before thread exits
    faunus_index_internal::IndexCache::finalize_thread_stats();
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
              << ", last_index=" << node.header.last_index << ", sibling=" << node.header.sibling << std::endl;
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
                    if (!((item.key >= leaf.header.fence.first) && (item.key < leaf.header.fence.second))) {
                        LOG_ERROR("Should have: " << leaf.header.fence.first << " <= " << item.key << " < " << leaf.header.fence.second);
                    }
                    assert((item.key >= leaf.header.fence.first) && (item.key < leaf.header.fence.second));
                } else {
                    std::cout << ", [Failed to read KVItem]";
                }
            }
            std::cout << std::endl;
        }
        // if (show_kv) {
        //     // Read and print all KVItems in the leaf
        //     std::vector<GlobalAddress> item_ptrs;
        //     for (size_t i = 0; i < branch_factor; ++i) {
        //         const auto& kvb = leaf.kv_blocks[i];
        //         if (!kvb.isFree() && kvb.getAddr().raw != 0) {
        //             item_ptrs.push_back(kvb.getAddr());
        //         }
        //     }
        //     if (!item_ptrs.empty()) {
        //         std::vector<KVItem> kv_items(item_ptrs.size());
        //         if (rdma_read_batch(*rdma_mgr_, item_ptrs, kv_items)) {
        //             std::cout << std::setw(depth * 2 + 4) << " " << "KV-Items:" << std::endl;
        //             for (size_t i = 0; i < kv_items.size(); ++i) {
        //                 std::cout << std::setw(depth * 2 + 6) << " " << kv_items[i] << std::endl;
        //             }
        //         } else {
        //             std::cout << std::setw(depth * 2 + 4) << " " << "[Failed to read KVItems]" << std::endl;
        //         }
        //     } else {
        //         std::cout << std::setw(depth * 2 + 4) << " " << "(no KVItems)" << std::endl;
        //     }
        // }
        return;
    }
    // Otherwise, recursively print children
    for (size_t i = 0; i <= node.header.last_index && i < branch_factor; ++i) {
        print_tree(node.entries[i].child.raw, depth + 1, show_kv);
    }
}
