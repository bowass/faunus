#include "sherman_index.hpp"
#include <thread>

using namespace sherman_index_internal;

thread_local GlobalAddress path_stack[kMaxLevelOfTree];

ShermanIndex::ShermanIndex(std::shared_ptr<RDMAManager> rdma_mgr, 
                          std::shared_ptr<LocalAllocator> allocator, 
                          GlobalAddress root_offset_pointer, 
                          std::shared_ptr<IndexCacheBase> cache,
                          std::shared_ptr<local_locks::LocalLockManager> local_lock_mgr)
    : KVIndex(rdma_mgr, allocator, root_offset_pointer, cache, local_lock_mgr) {
    // TODO: assert we init lock_manager in kv_test.cpp
    LOG_DEBUG("ShermanIndex created with " 
              << (local_lock_mgr ? "shared" : "no") << " local lock manager");
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
    if (local_lock_mgr_) {
        // First, acquire local lock (blocking)
        bool was_handover = local_lock_mgr_->acquire(lock_address, std::this_thread::get_id());
        
        if (was_handover) {
            // Got local lock via handover - RDMA lock already held by CS
            // LOG_DEBUG("Acquired local lock via handover for address " << std::hex << lock_address.raw);
            return true;
        } else {
            // Got local lock but need to acquire RDMA lock too
            // LOG_DEBUG("Acquired local lock, now acquiring RDMA lock for address " << std::hex << lock_address.raw);
            bool rdma_acquired = false;
            for (int attempt = 0; attempt < 1000000; ++attempt) {
                if (attempt > 10000) std::cout << "DAMN ATTEMPT = " << attempt << std::endl;
                if (rdma_try_acquire_lock(*rdma_mgr_, lock_address)) {
                    rdma_acquired = true;
                    break;
                }
            }
            
            if (rdma_acquired) {
                // LOG_DEBUG("Acquired both local and RDMA lock for address " << std::hex << lock_address.raw);
                return true;
            } else {
                // Failed to get RDMA lock - release local lock and fail
                LOG_ERROR("Failed to acquire RDMA lock, releasing local lock for address " << std::hex << lock_address.raw);
                local_lock_mgr_->release(lock_address, std::this_thread::get_id());
                return false;
            }
        }
    }
    
    // No local lock manager - acquire RDMA lock directly
    // LOG_DEBUG("Attempting RDMA-only lock for address " << std::hex << lock_address.raw);
    for (int attempt = 0; attempt < 1000000; ++attempt) {
        if (rdma_try_acquire_lock(*rdma_mgr_, lock_address)) {
            if (attempt > 10000) std::cout << "DAMN ATTEMPT = " << attempt << std::endl;
            // LOG_DEBUG("Acquired RDMA-only lock for address " << std::hex << lock_address.raw);
            return true;
        }
    }
    
    LOG_ERROR("Failed to acquire any lock for address " << std::hex << lock_address.raw << " after many attempts");
    assert(false && "Failed to acquire lock after many attempts");
    return false;
}

void ShermanIndex::unlock_address(GlobalAddress lock_address) {
    if (local_lock_mgr_) {
        // Check if we can handover before releasing
        bool can_handover = local_lock_mgr_->can_handover(lock_address);
        
        if (!can_handover) {
            // No handover - also release RDMA lock
            // LOG_DEBUG("Released local lock, also releasing RDMA lock for address " << std::hex << lock_address.raw);
            rdma_release_lock(*rdma_mgr_, lock_address);
        } else {
            // Handover enabled - keep RDMA lock for next thread
            // LOG_DEBUG("Released local lock with handover enabled for address " << std::hex << lock_address.raw);
        }
        // Release local lock
        local_lock_mgr_->release(lock_address, std::this_thread::get_id());
    } else {
        // No local lock manager - release RDMA lock directly
        // LOG_DEBUG("No local lock manager, releasing RDMA lock for address " << std::hex << lock_address.raw);
        rdma_release_lock(*rdma_mgr_, lock_address);
    }
}

void ShermanIndex::lock_and_read_page(GlobalAddress lock_addr, GlobalAddress page_address, size_t page_size, void* page_buffer) {
    // Lock the page
    // not asserting - may fail
    try_lock_address(lock_addr);

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

    // Check if we can keep the lock for handover
    bool can_handover = false;
    if (local_lock_mgr_) {
        can_handover = local_lock_mgr_->can_handover(lock_addr);
    }

    if (!can_handover) {
        // No handover - unlock the page via RDMA
        ops.push_back(RDMAOp{RDMAOpType::WRITE, lock_addr});
        // TODO: note that if zero is not static, compiler optimized and probably does not initialize correctly
        static uint64_t zero = 0;
        ops.back().op.write.buffer = reinterpret_cast<const uint8_t*>(&zero);
        ops.back().op.write.bytes = sizeof(uint64_t);

        // std::cout << "HELLO THIS WILLREMOVE DE BUG" << std::endl;
        // TODO: if i remove this print, zero contains 1 WTFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFF
        // for (size_t i = 0; i < std::min(ops.back().op.write.bytes, size_t(32)); ++i) {
        //     std::cout << std::hex << static_cast<int>(ops.back().op.write.buffer[i]) << " ";
        // }
        // std::cout << std::endl;
    }
    else {
        // std::cout << "Handing over lock - not unlocking remote lock" << std::endl;
    }

    // std::cout << "Number of ops to perform: " << ops.size() << std::endl;
    assert(rdma_mgr_->perform_batch(ops));
    if (local_lock_mgr_) {
        local_lock_mgr_->release(lock_addr, std::this_thread::get_id());
    }
}

bool ShermanIndex::search_node(GlobalAddress node_address, const Key& key, SearchResult& result, bool from_cache /*= false*/) {
    int counter = 0;
    LeafPage page;
re_read:
    if (++counter > 100) {
        std::cout << "FICK " << std::endl;
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }    
    assert(rdma_read_object(*rdma_mgr_, node_address, page));
    if (!page.check_consistent()) {
            goto re_read;
    }

    // std::cout << "[Key " << key << "] At node " << node_address << ": [" << page.hdr.lowest << ", " << page.hdr.highest << ")"
    //           << ", level=" << (int)page.hdr.level << ", leftmost=" << page.hdr.leftmost_ptr
    //           << ", sibling=" << page.hdr.sibling_ptr << std::endl;

    result.is_leaf = (page.hdr.leftmost_ptr == GlobalAddress::Null());
    result.level = page.hdr.level;

    path_stack[result.level] = node_address;

    if (key >= page.hdr.highest) {
        result.slibing = page.hdr.sibling_ptr;
        return true;
    }
    assert(key >= page.hdr.lowest);

    if (result.is_leaf) {
        if (from_cache && (key < page.hdr.lowest || key >= page.hdr.highest)) {
            return false;
        }
        // search in leaf
        for (int i = 0; i < kLeafCardinality; ++i) {
            auto &r = page.records[i];
            // TODO value null
            if (r.key == key && r.value != Value::min() && r.f_version == r.r_version) {
                result.val = r.value;
                return true;
            }
        }
    }
    // internal node
    else {
        assert(result.level != 0);
        assert(!from_cache);
        // if (result.level == 1 && cache_) {
        //     // add page to cache
        // };

        auto internal_page = reinterpret_cast<InternalPage*>(&page);
        // std::cout << "At internal node:";
        // internal_page->verbose_debug();
        auto cnt = internal_page->hdr.last_index + 1;
        if (key < internal_page->records[0].key) {
            result.next_level = internal_page->hdr.leftmost_ptr;
        } else {
            bool found = false;
            for (int i = 1; !found && (i < cnt); ++i) {
                // std::cout << "Comparing " << key << " to " << internal_page->records[i].key << std::endl;
                if (key < internal_page->records[i].key) {
                    result.next_level = internal_page->records[i - 1].ptr;
                    found = true;
                }
            }
            if (!found) {
                result.next_level = internal_page->records[cnt - 1].ptr;
            }
        }
    }

    // std::cout << "Done searching with next_level: " << result.next_level << std::endl;

    return true;
}

inline void ShermanIndex::before_operation() {
    for (size_t i = 0; i < kMaxLevelOfTree; ++i) {
        path_stack[i] = GlobalAddress::Null();
    }
}

bool ShermanIndex::insert(const Key& key, const Value& value) {
    before_operation();
    // if (cache_) {
    //     GlobalAddress cached_address;
    //     auto cached_result_any = cache_->search_any(key);
    //     if (cached_result_any.has_value()) {
    //         cached_address = cached_result_any.value().first;
    //         // TODO: more params
    //         auto root = get_root_offset();
    //         if (insert_to_leaf(cached_address, key, value, root, 0, true)) {
    //             // increase cache hits
    //             return true;
    //         }
    //         // cache stale - TODO invalidate the entry itself
    //         cache_->invalidate(cached_address);
    //     }
    //     // increase cache miss
    // }
    GlobalAddress root = get_root_offset();
    GlobalAddress p = root;
    SearchResult result;
next:
    std::cout << "GOTO FC NEXT" << std::endl;
    if (!search_node(p, key, result)) {
        p = get_root_offset();
        std::cout << "R " << p << std::endl;
        // sleep(1)!!!!! seconds!!!! A LOT!!!!
        goto next;
    }
    if (!result.is_leaf) {
        assert(result.level != 0);
        if (result.slibing != GlobalAddress::Null()) {
            p = result.slibing;
            std::cout << "S " << std::this_thread::get_id() << " " << p << std::endl;
            goto next;
        }
        p = result.next_level;
        if (result.level != 1) {
            std::cout << "L " << (int)result.level << std::endl;
            goto next;
        }
    }
    std::cout << "[Key " << key << "] Found leaf at " << p << std::endl;
    insert_to_leaf(p, key, value, root, 0);
    std::cout << "[Key " << key << "] Inserted leaf at " << p << std::endl;
    return true;
}

// TODO: IMPORTANT: we need to support out-of-place records

bool ShermanIndex::insert_to_leaf(GlobalAddress leaf_address, const Key& key, const Value& value, GlobalAddress root, int level, bool from_cache) {
    // std::cout << "At insert_to_leaf(" << leaf_address << ", " << key << ", " << value << ", " << root << ", " << level << ", " << from_cache << ")" << std::endl;
    LeafPage page;
    // Using embedded locks
    GlobalAddress lock_address = leaf_address;

    // rdma_read_object(*rdma_mgr_, leaf_address, page);
    // page.check_consistent();
    // page.debug();

    lock_and_read_page(lock_address, leaf_address, kLeafPageSize, &page);
    // page.debug();

    assert(page.hdr.level == level);
    assert(page.check_consistent());
    if (from_cache && (key < page.hdr.lowest || key >= page.hdr.highest)) {
        // std::cout << "from_cache fail: " << key << " not in [" << page.hdr.lowest << ", " << page.hdr.highest << ")" << std::endl;
        unlock_address(lock_address);
        return false;
    }
    if (key >= page.hdr.highest) {
        std::cout << key << " >= " << page.hdr.highest << std::endl;
        page.hdr.debug();
        std::cout << std::endl;
        unlock_address(lock_address);
        assert(page.hdr.sibling_ptr != GlobalAddress::Null());
        insert_to_leaf(page.hdr.sibling_ptr, key, value, root, level);
        return true;
    }
    assert(key >= page.hdr.lowest);

    int cnt = 0;
    int empty_index = -1;
    char *update_addr = nullptr;
    for (int i = 0; i < kLeafCardinality; ++i) {
        auto &r = page.records[i];
        // TODO: add Value::Null()
        if (r.value != Value::min()) {
            cnt++;
            if (r.key == key) {
                r.value = value;
                r.f_version++;
                r.r_version = r.f_version;
                update_addr = (char *)&r;
                break;
            }
        }
        else if (empty_index == -1) {
            empty_index = i;
        }
    }
    // TODO: unsure about that
    assert(cnt != kLeafCardinality);

    // Update value
    if (update_addr == nullptr) {
        assert(empty_index != -1);

        auto &r = page.records[empty_index];
        r.key = key;
        r.value = value;
        r.f_version++;
        r.r_version = r.f_version;

        update_addr = (char *)&r;
        cnt++;
    }

    bool need_split = (cnt == kLeafCardinality);
    // std::cout << "After insert attempt, cnt=" << cnt << ", need_split=" << need_split << std::endl;
    if (!need_split) {
        assert(update_addr);
        GlobalAddress remote_update_address = uint64_t(leaf_address) + (update_addr - (char *)&page);
        write_page_and_unlock(update_addr, remote_update_address, sizeof(LeafEntry), lock_address);
        // std::cout << "Updated leaf at " << leaf_address << " with key " << key << " and value " << value << std::endl;
        // debug reading address
        // rdma_read_object(*rdma_mgr_, leaf_address, page);
        // std::cout << "After update + unlock:" << std::endl;
        // page.verbose_debug();
        return true;
    }
    // std::cout << "SPLITTIN" << std::endl;
    // split
    std::sort(page.records, page.records + kLeafCardinality,
            [](const LeafEntry &a, const LeafEntry &b) { return a.key < b.key; });
    Key split_key;
    GlobalAddress sibling_addr = allocator_->allocate(kLeafPageSize);
    LeafPage sibling(page.hdr.level);

    int m = cnt / 2;
    split_key = page.records[m].key;
    assert(split_key > page.hdr.lowest);
    assert(split_key < page.hdr.highest);
    for (int i = m; i < cnt; ++i) {
        sibling.records[i - m].key = page.records[i].key;
        sibling.records[i - m].value = page.records[i].value;
        page.records[i].key = 0;
        page.records[i].value = Value::min(); // TODO: value null
    }
    page.hdr.last_index -= (cnt - m);
    sibling.hdr.last_index += (cnt - m);

    // update fence
    sibling.hdr.lowest = split_key;
    sibling.hdr.highest = page.hdr.highest;
    page.hdr.highest = split_key;

    // link
    sibling.hdr.sibling_ptr = page.hdr.sibling_ptr;
    page.hdr.sibling_ptr = sibling_addr;

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
        assert(from_cache);
        search_and_insert_to_internal(split_key, sibling_addr, level + 1);
    }
    return true;
}

bool ShermanIndex::read(const Key& key, Value& value_out) {
    GlobalAddress root = get_root_offset();
    GlobalAddress p = root;
    bool from_cache = false;
    // if (cache_) {
    //     GlobalAddress cached_address;
    //     auto cached_result_any = cache_->search_any(key);
    //     if (cached_result_any.has_value()) {
    //         p = cached_result_any.value().first;
    //         from_cache = true;
    //         // TODO: more params
    //         // increase cache hits
    //     }
    //     else {} // increase cache miss
    // }
    SearchResult result;
next:
    if (!search_node(p, key, result)) {
        if (from_cache) {
            // TODO: invalidate entry
            from_cache = false;
            p = root;
        }
        else {
            // should be shit
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
        goto next;
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

bool ShermanIndex::update(const Key& key, const Value& value) {
    return insert(key, value);
}

bool ShermanIndex::del(const Key& key) {
    assert(false && "Not implemented yet");
}

void ShermanIndex::insert_to_internal(GlobalAddress page_addr, const Key& key, GlobalAddress v, GlobalAddress root, int level) {
    GlobalAddress lock_addr = page_addr; // in-placed lock
    InternalPage page;

    // TODO: debug, tmp
    // rdma_read_object(*rdma_mgr_, page_addr, page);
    // page.verbose_debug();

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
            page.records[i] = page.records[i - 1];
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
        sibling.records[i - (m + 1)] = page.records[i];
    }
    page.hdr.last_index -= (cnt - (m + 1));
    sibling.hdr.last_index += (cnt - (m + 1));

    sibling.hdr.leftmost_ptr = page.records[m].ptr;
    // update fence
    sibling.hdr.lowest = split_key;
    sibling.hdr.highest = page.hdr.highest;
    page.hdr.highest = split_key;

    // link
    sibling.hdr.sibling_ptr = page.hdr.sibling_ptr;
    page.hdr.sibling_ptr = sibling_addr;

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
    assert(result.level != 0);
    if (result.slibing != GlobalAddress::Null()) {
        p = result.slibing;
        goto next;
    }
    p = result.next_level;
    if (result.level != level + 1) {
        goto next;
    }
 
    insert_to_internal(p, key, v, root, level + 1);
}

GlobalAddress ShermanIndex::get_root_offset_pointer() const {
    return root_offset_pointer_;
}

std::set<size_t> ShermanIndex::get_required_sizes_static() {
    // TODO: assert nothing more
    return {sizeof(GlobalAddress), kLeafPageSize, kInternalPageSize};
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
    op.op.cas.expected = reinterpret_cast<uint64_t>(&old_root);
    op.op.cas.desired = uint64_t(new_root_address);

    // TODO: later will not be assert - just return, bc multithreaded
    assert(rdma_mgr_->perform_op(op));
    std::cout << "Updated root offset from " << old_root << " to " << new_root_address << std::endl;
    return true;
}
