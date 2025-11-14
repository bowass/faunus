#include "sherman_index.hpp"
#include <thread>

#include "../util/precise_sleep.hpp"

using namespace sherman_index_internal;

thread_local GlobalAddress path_stack[kMaxLevelOfTree];

ShermanIndex::ShermanIndex(std::shared_ptr<RDMAManager> rdma_mgr,
                          std::shared_ptr<LocalAllocator> allocator,
                          GlobalAddress root_offset_pointer, 
                          std::shared_ptr<IndexCacheBase> cache,
                          std::shared_ptr<local_locks::LocalLockManager> local_lock_mgr)
    : KVIndex(rdma_mgr, allocator, root_offset_pointer, cache, local_lock_mgr) {
    
    // Get CS-level RDMA tag from LocalLockManager (shared by all threads on this CS)
    if (local_lock_mgr) {
        cs_rdma_tag_ = local_lock_mgr->get_cs_rdma_tag();
        LOG_DEBUG("ShermanIndex @" << static_cast<void*>(this) << " using shared CS RDMA tag=" << std::hex << cs_rdma_tag_ 
                  << std::dec << " from LocalLockManager @" << local_lock_mgr.get());
    } else {
        // Fallback: generate unique tag per instance (no handovers possible)
        std::hash<void*> hasher;
        uint64_t hash_val = hasher(static_cast<void*>(this));
        cs_rdma_tag_ = (hash_val & 0xFFFFFFFFFFFFFFFFULL);
        if (cs_rdma_tag_ == 0) {
            cs_rdma_tag_ = 1;
        }
        LOG_DEBUG("ShermanIndex @" << static_cast<void*>(this) << " created with unique RDMA tag=" 
                  << std::hex << cs_rdma_tag_ << std::dec << " (NO local lock manager - handovers disabled!)");
    }
    if (cache_) {
        auto wrapper = std::dynamic_pointer_cast<ShermanCacheWrapper>(cache_);
        if (wrapper) {
            sherman_cache_ = wrapper->get_cache();
        }
    }
}

std::shared_ptr<IndexCacheBase> ShermanIndex::create_cache(size_t cache_size_bytes) const {
    // Estimate number of entries based on cache size
    // Each InternalPage + overhead is roughly 1KB-2KB, so use conservative estimate
    size_t estimated_entries = std::max(size_t(1), cache_size_bytes / 2048);
    auto sherman_cache = std::make_shared<ShermanCache>(estimated_entries);
    return std::make_shared<ShermanCacheWrapper>(sherman_cache);
}

bool ShermanIndex::initialize(size_t) {
    // Allocate root node using local allocator (should be set before construction)
    LOG_DEBUG("ShermanIndex initializing... " << allocator_);
    root_offset_pointer_ = allocator_->allocate(sizeof(GlobalAddress));
    LOG_DEBUG("Allocated root offset pointer at " << std::hex << root_offset_pointer_.raw << std::dec);
    size_t root_offset = allocator_->allocate(kLeafPageSize);
    LOG_DEBUG("Allocated root node at offset " << std::hex << root_offset << std::dec);

    LeafPage root{};
    root.set_consistent();

    assert(rdma_write_object(*rdma_mgr_, root_offset, root));

    RDMAOp op{RDMAOpType::CAS, root_offset_pointer_};
    uint64_t expected = 0;
    op.op.cas.expected = reinterpret_cast<uint64_t>(&expected);
    op.op.cas.desired = uint64_t(root_offset);
    assert(rdma_mgr_->perform_op(op));

    LOG_DEBUG("Verifying root offset: " << get_root_offset() << " should be " << std::hex << root_offset << std::dec);

    return true;
}

bool ShermanIndex::try_lock_address(GlobalAddress lock_address) {
    // Use CS-level tag (shared by all threads on this compute server)
    uint64_t tag = cs_rdma_tag_;
    
    // Step 1: Acquire local lock for intra-CS serialization
    bool is_handover = false;
    if (local_lock_mgr_) {
        // Acquire local lock - returns true if RDMA already held (handover)
        is_handover = local_lock_mgr_->acquire(lock_address);
        
        // Track local lock acquisition
        if (stats_tracker_) {
            stats_tracker_->record_local_lock_acquisition(is_handover);
        }
        
        // If handover, we're done - RDMA lock already held by our CS
        if (is_handover) {
            // LOG_DEBUG("[CS:" << tag << "] Handover for addr=" << std::hex << lock_address.raw << std::dec);
            return true;
        }
    }
    
    // Step 2: Acquire RDMA lock (only if not handed over)
    // LOG_DEBUG("[CS:" << tag << "] Trying RDMA lock for addr=" << std::hex << lock_address.raw << std::dec);
    
    uint64_t retry_cnt = 0;
    uint64_t pre_conflict_tag = 0;
    uint64_t current_value = 0;
    
    while (true) {
        retry_cnt++;

        // Try CAS: 0 -> tag
        uint64_t expected_val = 0;
        RDMAOp cas_op{RDMAOpType::CAS, lock_address};
        cas_op.op.cas.expected = reinterpret_cast<uint64_t>(&expected_val);
        cas_op.op.cas.desired = tag;
        
        // Perform the CAS - expected_val will be updated with current value
        bool op_success = rdma_mgr_->perform_op(cas_op);
        assert(op_success && "RDMA operation should not fail");
        
        if (expected_val == 0) {
            // LOG_DEBUG("[CS:" << tag << "] Acquired RDMA lock for addr=" << std::hex << lock_address.raw << std::dec << " after " << retry_cnt << " retries");
            return true;
        }

        // CAS failed - someone else holds the lock (or we already hold it)  
        current_value = expected_val;
        
        // Check if we already hold this lock (self-ownership detection)
        // This should NOT happen in correct code, but handle gracefully
        if (current_value == tag) {
            // LOG_DEBUG("Thread already holds lock at address " << std::hex << lock_address.raw 
                    //  << " with tag " << std::hex << tag << " - returning success");
            return true;
        }
        
        // Check retry limit AFTER self-ownership check
        if (retry_cnt > 1000000) {
            std::cout << "DEADLOCK: retry=" << retry_cnt 
                      << " pre_tag=" << pre_conflict_tag 
                      << " current=" << current_value 
                      << " my_tag=" << tag
                      << " addr=" << std::hex << lock_address.raw << std::dec
                      << std::endl;
            LOG_ERROR("Deadlock detected at address " << std::hex << lock_address.raw 
                     << " - locked by tag " << std::hex << current_value 
                     << " (our tag: " << std::hex << tag << ")");
            
            // Match Sherman: assert instead of throwing exception
            assert(false && "Deadlock detected in RDMA locking");
            return false;
        }
        
        // Sherman's critical optimization: reset retry counter when lock holder changes
        // This ensures fairness and prevents starvation when lock holder changes
        if (current_value != pre_conflict_tag) {
            retry_cnt = 0;  // Reset retry counter for new lock holder
            pre_conflict_tag = current_value;
        }
        
        // Validate the conflict tag
        assert(current_value != 0 && "Lock should not be 0 if CAS failed");
        // std::cout << std::this_thread::get_id() << " ... waiting lock=" << lock_address << std::endl;
        // util::precise_sleep_us(10);
    }
}

void ShermanIndex::unlock_address(GlobalAddress lock_address) {
    // Use CS-level tag (shared by all threads on this compute server)
    uint64_t tag = cs_rdma_tag_;
    
    if (local_lock_mgr_) {
        // Check if we should handover to next waiter on this CS
        bool handover = local_lock_mgr_->release(lock_address);
        
        if (handover) {
            // Handover: keep RDMA lock held, next thread will skip RDMA acquire
            // LOG_DEBUG("[CS:" << tag << "] Handover for addr=" << std::hex << lock_address.raw << std::dec);
            // RDMA lock stays held - don't release
        } else {
            // No handover: release RDMA lock
            // LOG_DEBUG("[CS:" << tag << "] Releasing RDMA lock for addr=" << std::hex << lock_address.raw << std::dec);
            rdma_cas_release_lock(*rdma_mgr_, lock_address);
        }
    } else {
        // No local lock manager - just release RDMA
        rdma_cas_release_lock(*rdma_mgr_, lock_address);
    }
}

void ShermanIndex::lock_and_read_page(GlobalAddress lock_addr, GlobalAddress page_address, size_t page_size, void* page_buffer) {
    // Lock the page - should succeed with pure RDMA locking
    bool lock_success = try_lock_address(lock_addr);
    if (!lock_success) {
        LOG_ERROR("Failed to acquire lock at address " << std::hex << lock_addr.raw 
                 << " - unexpected lock contention");
        throw std::runtime_error("Unable to acquire page lock after maximum retries");
    }

    RDMAOp op{RDMAOpType::READ, page_address};
    op.op.read.buffer = reinterpret_cast<uint8_t*>(page_buffer);
    op.op.read.bytes = page_size;
    assert(rdma_mgr_->perform_op(op));
}

void ShermanIndex::write_page_and_unlock(void* page_buffer, GlobalAddress page_address, size_t page_size, GlobalAddress lock_addr) {
    // TODO: sync vs async
    // Rewrite page
    std::vector<RDMAOp> ops;
    ops.push_back(RDMAOp{RDMAOpType::WRITE, page_address});
    ops.back().op.write.buffer = reinterpret_cast<const uint8_t*>(page_buffer);
    ops.back().op.write.bytes = page_size;

    assert(rdma_mgr_->perform_batch(ops));
    
    // Release locks using unlock_address to ensure proper logging and coordination
    unlock_address(lock_addr);
}

bool ShermanIndex::search_node(GlobalAddress node_address, const Key& key, SearchResult& result, bool from_cache /*= false*/) {
    int counter = 0;
    LeafPage page;
    GlobalAddress lock_address = node_address;
    
re_read:
    if (++counter > 100) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
    
    // Read the page first - using LeafPageSize like original Sherman
    assert(rdma_read_object(*rdma_mgr_, node_address, page));
    
    if (!page.check_consistent()) {
        goto re_read;
    }

    // TODO: debug
    memset(&result, 0, sizeof(result));
    result.is_leaf = (page.hdr.leftmost_ptr == GlobalAddress::Null());
    result.level = page.hdr.level;

    path_stack[result.level] = node_address;

    if (key >= page.hdr.highest) {
        result.slibing = page.hdr.sibling_ptr;
        // Unlock before following sibling to avoid deadlocks
        // unlock_address(lock_address);
        assert(result.slibing != GlobalAddress::Null());
        assert(result.slibing != node_address);
        return true;
    }
    assert(key >= page.hdr.lowest);

    if (result.is_leaf) {
        if (from_cache && (key < page.hdr.lowest || key >= page.hdr.highest)) {
            // unlock_address(lock_address);
            return false;
        }
        assert(result.level == 0);
        // search in leaf - now with out-of-place records
        Fingerprint target_fp(key);
        for (int i = 0; i < kLeafCardinality; ++i) {
            auto &r = page.records[i];
            if (r.is_empty()) {
                continue;
            }
            // Fast fingerprint comparison first
            if (r.fingerprint == target_fp) {
                // Read the KVItem to verify full key
                KVItem kv_item;
                if (!rdma_read_object(*rdma_mgr_, r.kv_ptr, kv_item)) {
                    continue; // Read failed, skip this entry
                }
                if (kv_item.key == key) {
                    result.val = kv_item.value;
                    // unlock_address(lock_address);
                    return true;
                }
            }
        }
        // unlock_address(lock_address);
    }
    // internal node
    else {
        assert(result.level != 0);
        assert(!from_cache);

        auto internal_page = reinterpret_cast<InternalPage*>(&page);
        if (!internal_page->check_consistent()) {
            goto re_read;
        }
        if (result.level == 1 && sherman_cache_) {
            sherman_cache_->add(page.hdr.lowest, page.hdr.highest, {node_address, *internal_page});
        }

        auto cnt = internal_page->hdr.last_index + 1;
        if (key < internal_page->records[0].key) {
            result.next_level = internal_page->hdr.leftmost_ptr;
        } else {
            bool found = false;
            for (int i = 1; !found && (i < cnt); ++i) {
                if (key < internal_page->records[i].key) {
                    result.next_level = internal_page->records[i - 1].ptr;
                    found = true;
                }
            }
            if (!found) {
                result.next_level = internal_page->records[cnt - 1].ptr;
            }
        }
        // Unlock after reading internal node data
        // unlock_address(lock_address);
    }

    return true;
}

inline void ShermanIndex::before_operation() {
    for (size_t i = 0; i < kMaxLevelOfTree; ++i) {
        path_stack[i] = GlobalAddress::Null();
    }
}

GlobalAddress ShermanIndex::get_leaf_from_cache_entry(const Entry<Key, ShermanCacheItem>& entry, const Key& key) {
    GlobalAddress node_address = entry.item.first;
    InternalPage page = entry.item.second;

    if (entry.start > key || entry.end <= key) {
        return GlobalAddress::Null();
    }
    if (key < page.records[0].key) {
        return page.hdr.leftmost_ptr;
    }
    for (int i = 1; i <= page.hdr.last_index; i++) {
        if (key < page.records[i].key) {
            return page.records[i - 1].ptr;
        }
    }
    return page.records[page.hdr.last_index].ptr;
}

bool ShermanIndex::insert(const Key& key, const Value& value) {
    before_operation();
    if (sherman_cache_) {
        auto cached_entry = sherman_cache_->search(key);
        if (cached_entry) {
            GlobalAddress cached_address = get_leaf_from_cache_entry(*cached_entry, key);
            auto root = get_root_offset();

            // Cache should always point to leaf pages (level 0)
            if (insert_to_leaf(cached_address, key, value, root, 0, true)) {
                // Cache hit - successfully used cached entry
                if (stats_tracker_) {
                    stats_tracker_->record_cache_hit();
                }
                return true;
            }
            // Cache entry was stale - invalidate it
            sherman_cache_->invalidate(cached_entry);
        }
        if (stats_tracker_) {
            stats_tracker_->record_cache_miss();
        }
    }

    // Add retry limit to prevent infinite loops
    int retry_count = 0;
    const int MAX_RETRIES = 1000000;
    
    GlobalAddress root = get_root_offset();
    GlobalAddress p = root;
    SearchResult result;
    
next:
    if (retry_count > 0) {
        stats_tracker_->record_retry();
    }
    if (++retry_count > MAX_RETRIES) {
        return false;
    }    
    if (!search_node(p, key, result)) {
        p = get_root_offset();
        // Add small delay to reduce contention
        if (retry_count % 100 == 0) {
            std::this_thread::yield();
        }
        goto next;
    }
    if (!result.is_leaf) {
        assert(result.level != 0);
        if (result.slibing != GlobalAddress::Null()) {
            p = result.slibing;
            goto next;
        }
        p = result.next_level;
        if (result.level != 1) {
            goto next;
        }
    }
    // Always call insert_to_leaf with level 0 (hardcoded like original Sherman)
    insert_to_leaf(p, key, value, root, 0);
    return true;
}

// TODO: IMPORTANT: we need to support out-of-place records

bool ShermanIndex::insert_to_leaf(GlobalAddress leaf_address, const Key& key, const Value& value, GlobalAddress root, int level, bool from_cache) {
    LeafPage page;
    // Using embedded locks
    GlobalAddress lock_address = leaf_address;

    lock_and_read_page(lock_address, leaf_address, kLeafPageSize, &page);

    // assert(page.hdr.level == level);
    if (!(page.hdr.level == level)) {
        page.debug();
        assert(0);
    }
    assert(page.check_consistent());
    if (from_cache && (key < page.hdr.lowest || key >= page.hdr.highest)) {
        unlock_address(lock_address);
        return false;
    }
    if (key >= page.hdr.highest) {
        unlock_address(lock_address);
        assert(page.hdr.sibling_ptr != GlobalAddress::Null());
        insert_to_leaf(page.hdr.sibling_ptr, key, value, root, level);
        return true;
    }
    assert(key >= page.hdr.lowest);

    int cnt = 0;
    int empty_index = -1;
    Fingerprint target_fp(key);
    
    for (int i = 0; i < kLeafCardinality; ++i) {
        auto &r = page.records[i];
        if (!r.is_empty()) {
            cnt++;
            // Check fingerprint first, then verify key
            if (r.fingerprint == target_fp) {
                KVItem kv_item;
                if (rdma_read_object(*rdma_mgr_, r.kv_ptr, kv_item)) {
                    if (kv_item.key == key) {
                        // Key already exists - update value (insert-or-update semantics)
                        kv_item.value = value;
                        rdma_write_object(*rdma_mgr_, r.kv_ptr, kv_item);
                        unlock_address(lock_address);
                        return true;
                    }
                }
            }
        }
        else if (empty_index == -1) {
            empty_index = i;
        }
    }

    // Insert new item
    assert(cnt != kLeafCardinality);
    assert(empty_index != -1);
    
    // Allocate KVItem storage
    GlobalAddress kv_item_addr = allocator_->allocate(sizeof(KVItem));
    KVItem kv_item{key, value};
    rdma_write_object(*rdma_mgr_, kv_item_addr, kv_item);
    
    // Update leaf entry with fingerprint and pointer
    auto &r = page.records[empty_index];
    r.fingerprint = target_fp;
    r.kv_ptr = kv_item_addr;
    cnt++;

    bool need_split = (cnt == kLeafCardinality);
    if (!need_split) {
        // Write back just the modified entry
        char *update_addr = (char *)&r;
        GlobalAddress remote_update_address = uint64_t(leaf_address) + (update_addr - (char *)&page);
        write_page_and_unlock(update_addr, remote_update_address, sizeof(LeafEntry), lock_address);
        return true;
    }
    
    // Need to split - first sort all records by reading keys
    std::vector<std::pair<Key, LeafEntry>> keyed_entries;
    for (int i = 0; i < kLeafCardinality; ++i) {
        if (!page.records[i].is_empty()) {
            KVItem item;
            if (rdma_read_object(*rdma_mgr_, page.records[i].kv_ptr, item)) {
                keyed_entries.push_back({item.key, page.records[i]});
            }
        }
    }
    std::sort(keyed_entries.begin(), keyed_entries.end(),
            [](const auto &a, const auto &b) { return a.first < b.first; });
    
    // Verify we have all entries (no RDMA read failures)
    assert(keyed_entries.size() == static_cast<size_t>(cnt));
    
    // Copy sorted entries back to page
    for (size_t i = 0; i < keyed_entries.size(); ++i) {
        page.records[i] = keyed_entries[i].second;
    }
    for (size_t i = keyed_entries.size(); i < kLeafCardinality; ++i) {
        page.records[i] = LeafEntry();
    }

    // Now perform the split
    Key split_key;
    GlobalAddress sibling_addr = allocator_->allocate(kLeafPageSize);
    LeafPage sibling(page.hdr.level);

    size_t actual_cnt = keyed_entries.size();
    int m = actual_cnt / 2;
    // Use the key from our sorted vector (already read)
    split_key = keyed_entries[m].first;
    
    assert(split_key > page.hdr.lowest);
    assert(split_key < page.hdr.highest);
    
    // Copy fingerprint+pointer pairs (not full KVItems) to sibling
    for (size_t i = m; i < actual_cnt; ++i) {
        sibling.records[i - m] = page.records[i];
        page.records[i] = LeafEntry(); // Clear entry
    }
    page.hdr.last_index -= (actual_cnt - m);
    sibling.hdr.last_index += (actual_cnt - m);

    // update fence
    sibling.hdr.lowest = split_key;
    sibling.hdr.highest = page.hdr.highest;
    page.hdr.highest = split_key;

    // link
    sibling.hdr.sibling_ptr = page.hdr.sibling_ptr;
    page.hdr.sibling_ptr = sibling_addr;
    
    // Validate sibling pointers to prevent cycles
    assert(sibling_addr != leaf_address);
    assert(sibling.hdr.sibling_ptr != leaf_address);
    assert(sibling.hdr.sibling_ptr != sibling_addr);

    // write sibling
    sibling.set_consistent();
    rdma_write_object(*rdma_mgr_, sibling_addr, sibling);

    // write original page
    page.set_consistent();
    write_page_and_unlock(&page, leaf_address, kLeafPageSize, lock_address);

    // update root
    if (root == leaf_address) {
        if (update_root_offset(leaf_address, split_key, sibling_addr, level + 1, root)) {
            return true;
        }
    }
    // insert new key upwards
    auto up_level = path_stack[level + 1];
    if (up_level != GlobalAddress::Null()) {
        insert_to_internal(up_level, split_key, sibling_addr, root, level + 1);
    }
    else {
        // TODO: this assert fails for some reason
        // assert(from_cache);
        search_and_insert_to_internal(split_key, sibling_addr, level + 1);
    }
    return true;
}

bool ShermanIndex::read(const Key& key, Value& value_out) {
    GlobalAddress root = get_root_offset();
    GlobalAddress p = root;
    bool from_cache = false;
    Entry<Key, ShermanCacheItem>* cached_entry;

    // Try cache lookup first
    if (sherman_cache_) {
        cached_entry = sherman_cache_->search(key);
        if (cached_entry) {
            p = get_leaf_from_cache_entry(*cached_entry, key);
            from_cache = true;
        } else {
            if (stats_tracker_) {
                stats_tracker_->record_cache_miss();
            }
        }
    }
    SearchResult result;
next:
    if (!search_node(p, key, result)) {
        if (from_cache) {
            // Invalidate stale cache entry and retry from root
            if (sherman_cache_) {
                if (cached_entry) {
                    sherman_cache_->invalidate(cached_entry);
                    stats_tracker_->record_cache_miss();
                }
            }
            from_cache = false;
            p = root;
        }
        else {
            // should be shit
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
        goto next;
    }
    else if (cached_entry) {
        if (stats_tracker_) {
            stats_tracker_->record_cache_hit();
        }
        cached_entry = nullptr;
    }
    if (result.is_leaf) {
        // TODO value null
        if (result.val != Value::min()) {
            value_out = result.val;
            return true;
        }
        if (result.slibing != GlobalAddress::Null()) {
            p = result.slibing;
            goto next;
        }
        return false;
    }
    else {
        p = (result.slibing == GlobalAddress::Null()) ? result.next_level : result.slibing;
        goto next;
    }
}

bool ShermanIndex::del(const Key& key) {
    assert(false && "Not implemented yet");
}

void ShermanIndex::insert_to_internal(GlobalAddress page_addr, const Key& key, GlobalAddress v, GlobalAddress root, int level) {
    GlobalAddress lock_addr = page_addr; // in-placed lock
    InternalPage page;

    lock_and_read_page(lock_addr, page_addr, kInternalPageSize, &page);

    assert(page.hdr.level == level);
    assert(page.check_consistent());
    if (key >= page.hdr.highest) {
        unlock_address(lock_addr);
        assert(page.hdr.sibling_ptr != GlobalAddress::Null());
        insert_to_internal(page.hdr.sibling_ptr, key, v, root, level);
        return;
    }
    assert(key >= page.hdr.lowest);

    int16_t cnt = page.hdr.last_index + 1;
    bool is_update = false;
    uint16_t insert_index = 0;
    for (int i = cnt - 1; i >= 0; --i) {
        if (page.records[i].key == key) {
            // update
            page.records[i].ptr = v;
            is_update = true;
            break;
        }
        else if (page.records[i].key < key) {
            insert_index = i + 1;
            break;
        }
    }
    // TODO: idk about that
    assert(cnt != kInternalCardinality);

    if (!is_update) {
        for (int i = cnt; i > insert_index; --i) {
            // page.records[i] = page.records[i - 1];
            // TODO: debug
            page.records[i].key = page.records[i - 1].key;
            page.records[i].ptr = page.records[i - 1].ptr;
        }
        page.records[insert_index].key = key;
        page.records[insert_index].ptr = v;
        page.hdr.last_index++;
    }

    cnt = page.hdr.last_index + 1;
    bool need_split = (cnt == kInternalCardinality);
    if (!need_split) {
        page.set_consistent();
        write_page_and_unlock(&page, page_addr, kInternalPageSize, lock_addr);
        return;
    }
    // split
    GlobalAddress sibling_addr = allocator_->allocate(kInternalPageSize);
    InternalPage sibling(page.hdr.level);
    int m = cnt / 2;
    Key split_key = page.records[m].key;
    assert(split_key > page.hdr.lowest);
    assert(split_key < page.hdr.highest);
    for (int i = m + 1; i < cnt; ++i) {
        // TODO: debug
        sibling.records[i - (m + 1)].key = page.records[i].key;
        sibling.records[i - (m + 1)].ptr = page.records[i].ptr;
    }
    page.hdr.last_index -= (cnt - m);
    sibling.hdr.last_index += (cnt - (m + 1));

    sibling.hdr.leftmost_ptr = page.records[m].ptr;
    // update fence
    sibling.hdr.lowest = split_key;
    sibling.hdr.highest = page.hdr.highest;
    page.hdr.highest = split_key;

    // link
    sibling.hdr.sibling_ptr = page.hdr.sibling_ptr;
    page.hdr.sibling_ptr = sibling_addr;

    // Validate sibling pointers to prevent cycles in internal nodes
    assert(sibling_addr != page_addr);
    assert(sibling.hdr.sibling_ptr != page_addr);
    assert(sibling.hdr.sibling_ptr != sibling_addr);

    // write sibling
    sibling.set_consistent();
    assert(rdma_write_object(*rdma_mgr_, sibling_addr, sibling));
    
    // write original page
    page.set_consistent();
    write_page_and_unlock(&page, page_addr, kInternalPageSize, lock_addr);

    if (root == page_addr) {
        if (update_root_offset(page_addr, split_key, sibling_addr, level + 1, root)) {
            return;
        }
    }
    // insert new key upwards
    auto up_level = path_stack[level + 1];
    if (up_level != GlobalAddress::Null()) {
        insert_to_internal(up_level, split_key, sibling_addr, root, level + 1);
    }
    else {
        assert(false);
    }
}


void ShermanIndex::search_and_insert_to_internal(const Key& key, GlobalAddress v, int level) {
    auto root = get_root_offset();
    GlobalAddress p = root;
    SearchResult result;
next:
    if (!search_node(p, key, result)) {
        p = get_root_offset();
        // sleep(1)!!!!! seconds!!!! A LOT!!!!
        goto next;
    }
    // TODO: should be only with root
    if (result.level == level) {
        insert_to_internal(p, key, v, root, level);
        return;
    }
    assert(result.level != 0);
    // if (result.level == 0) {
    //     std::this_thread::sleep_for(std::chrono::seconds(reinterpret_cast<uint64_t>(&result) % 7));
    //     std::cout << result.val << " " << p << " " << result.is_leaf << " " << result.next_level << " " << result.slibing << std::endl;
    //     assert(0);
    // }
    if (result.slibing != GlobalAddress::Null()) {
        p = result.slibing;
        goto next;
    }
    p = result.next_level;
    if (result.level != level + 1) {
        goto next;
    }
 
    insert_to_internal(p, key, v, root, level);
}

GlobalAddress ShermanIndex::get_root_offset_pointer() const {
    return root_offset_pointer_;
}

std::set<size_t> ShermanIndex::get_required_sizes_static() {
    // Include KVItem for out-of-place storage
    return {sizeof(GlobalAddress), kLeafPageSize, kInternalPageSize, sizeof(KVItem)};
}

std::set<size_t> ShermanIndex::get_required_sizes() const {
    return ShermanIndex::get_required_sizes_static();
}

void ShermanIndex::maintenance_worker(size_t cs_id, size_t thread_id) {
    // Sherman index maintenance worker implementation
    // This can be extended later for specific maintenance tasks
    // LOG_DEBUG("Sherman maintenance worker started for CS " << cs_id << " thread " << thread_id);
    
    // For now, just sleep to avoid busy waiting
    // In a real implementation, this would process maintenance requests
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
}

void ShermanIndex::print_tree(size_t offset, int depth, bool show_kv) {

}

GlobalAddress ShermanIndex::get_root_offset() const {
    GlobalAddress root;
    assert(rdma_read_object(*rdma_mgr_, get_root_offset_pointer(), root));
    return root;
}

bool ShermanIndex::update_root_offset(GlobalAddress left, const Key& key, GlobalAddress right, int level, GlobalAddress old_root) {
    GlobalAddress new_root_address = allocator_->allocate(kInternalPageSize);
    InternalPage new_root(left, key, right, level);
    new_root.set_consistent();

    assert(rdma_write_object(*rdma_mgr_, new_root_address, new_root));
    RDMAOp op{RDMAOpType::CAS, root_offset_pointer_};
    GlobalAddress tmp_old_root = old_root;
    op.op.cas.expected = reinterpret_cast<uint64_t>(&tmp_old_root);
    op.op.cas.desired = uint64_t(new_root_address);

    assert(rdma_mgr_->perform_op(op));
    return old_root == tmp_old_root;
}
