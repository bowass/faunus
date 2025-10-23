#pragma once
#include "../externals/skiplist/include/skiplist.h"
#include <atomic>
#include <cstdint>
#include <cassert>
#include <vector>
#include <thread>

/*
TODO:
- implement allocation more efficiently, using SlabAllocator
- improve eviction policy?
*/

template<typename KeyT, typename ItemT>
struct Entry {
    KeyT start;
    KeyT end;
    ItemT item;
    skiplist_node node_;
    std::atomic<uint64_t> lastAccess{0};

    Entry(KeyT s, KeyT e, const ItemT& v) : start(s), end(e), item(v) {
        skiplist_init_node(&node_);
    }

    ~Entry() = default; // no node free here
};

template<typename KeyT, typename ItemT>
class RangeCache {
public:
    using EntryT = Entry<KeyT, ItemT>;

    explicit RangeCache(size_t maxEntries, uint64_t accessUpdateInterval = 64)
        : maxEntries_(maxEntries), accessUpdateInterval_(accessUpdateInterval) {
        skiplist_init(&list_, &RangeCache::cmpFunc);
        list_.aux = this;
        entryCount_.store(0, std::memory_order_relaxed);
    }

    ~RangeCache() {
        // clear();
    }

    EntryT* search(const KeyT& key) {
        EntryT query(key, key, ItemT{});
        skiplist_node* node = skiplist_find_smaller_or_equal(&list_, &query.node_);
        if (!node) return nullptr;

        EntryT* e = _get_entry(node, EntryT, node_);
        if (key >= e->start && key <= e->end) {
            updateAccess(e);
            return e;
        }
        return nullptr;
    }

    void add(const KeyT& start, const KeyT& end, const ItemT& item) {
        auto* e = new EntryT(start, end, item);

        while (true) {
            size_t cnt = entryCount_.load(std::memory_order_relaxed);
            if (cnt < maxEntries_) {
                if (entryCount_.compare_exchange_weak(cnt, cnt + 1,
                                                      std::memory_order_relaxed)) {
                    skiplist_insert(&list_, &e->node_);
                    return;
                }
            } else {
                evictOldest();
            }
        }
    }

    void invalidate(EntryT* e) {
        if (!e) return;
        // unsuccessful erase returns non-zero
        if (skiplist_erase_node(&list_, &e->node_)) return;
        delete e;
        entryCount_.fetch_sub(1, std::memory_order_relaxed);
    }

    void clear() {
        skiplist_node* node = skiplist_begin(&list_);
        while (node != &list_.tail) {
            skiplist_node* next = skiplist_next(&list_, node);
            EntryT* e = _get_entry(node, EntryT, node_);
            delete e;
            node = next;
        }
        skiplist_free(&list_);
        entryCount_.store(0, std::memory_order_relaxed);
    }

private:
    skiplist_raw list_;
    size_t maxEntries_;
    std::atomic<size_t> entryCount_;
    uint64_t accessUpdateInterval_;
    std::atomic<uint64_t> accessCounter_{0};

    static int cmpFunc(skiplist_node* a, skiplist_node* b, void* aux) {
        auto* ea = _get_entry(a, EntryT, node_);
        auto* eb = _get_entry(b, EntryT, node_);
        if (ea->start < eb->start) return -1;
        if (ea->start > eb->start) return 1;
        return 0;
    }

    void evictOldest() {
        skiplist_node* node = skiplist_begin(&list_);
        if (!node || node == &list_.tail) return;
        EntryT* e = _get_entry(node, EntryT, node_);
        if (skiplist_erase_node(&list_, node)) {
            delete e;
            entryCount_.fetch_sub(1, std::memory_order_relaxed);
        }
    }

    void updateAccess(EntryT* e) {
        uint64_t counter = accessCounter_.fetch_add(1, std::memory_order_relaxed);
        if ((counter & (accessUpdateInterval_ - 1)) == 0) {
            e->lastAccess.store(counter, std::memory_order_relaxed);
        }
    }
};
