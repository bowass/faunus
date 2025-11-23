#pragma once
#include "../externals/skiplist/include/skiplist.h"
#include "../externals/concurrentqueue/concurrentqueue.h"
#include <atomic>
#include <cstdint>
#include <vector>
#include <thread>
#include <random>
#include <cassert>

#include "../rdma/local_allocator.hpp"
#include "../rdma/simple_allocator.hpp"

template<typename KeyT, typename ItemT>
struct Entry {
    KeyT start, end;
    ItemT item;
    skiplist_node node_;
    std::atomic<uint64_t> lastAccess{0};

    Entry(KeyT s, KeyT e, const ItemT& v)
        : start(s), end(e), item(v) { skiplist_init_node(&node_); }
};

// for timestamp-based eviction
inline uint64_t next_timestamp() noexcept {
    // Compile-time constants (not global)
    constexpr uint64_t LOCAL_BITS = 10;
    constexpr uint64_t LOCAL_MASK = (1ULL << LOCAL_BITS) - 1;

    // One atomic epoch shared across the program
    static std::atomic<uint64_t> epoch{1};

    // One TLS counter per thread
    thread_local uint64_t local = 0;

    uint64_t l = local++;

    // Fast path: no rollover
    if (l <= LOCAL_MASK) {
        return (epoch.load(std::memory_order_relaxed) << LOCAL_BITS) | l;
    }

    // Slow path: rollover the local counter and bump epoch
    local = 0;
    uint64_t e = epoch.fetch_add(1, std::memory_order_relaxed) + 1;

    return e << LOCAL_BITS;
}

template<typename KeyT, typename ItemT>
class RangeCache {
public:
    using EntryT = Entry<KeyT, ItemT>;

    struct RetiredItem { int64_t offset; uint64_t retire_epoch; };
    struct ThreadRecord {
        std::atomic<ThreadRecord*> next{nullptr};
        std::atomic<uint64_t> epoch{0};
    };

    RangeCache(size_t maxEntries,
               uint64_t accessUpdateInterval = 64,
               unsigned sampling_steps = 8)
        : maxEntries_(maxEntries),
          entrySlab_(std::make_shared<SlabAllocator>(sizeof(EntryT), maxEntries, std::make_shared<SimpleAllocator>())),
          accessUpdateInterval_(accessUpdateInterval),
          sampling_steps_(sampling_steps)
    {
        skiplist_init(&list_, &RangeCache::cmpFunc);
        list_.aux = this;
        global_epoch_.store(1, std::memory_order_release);
    }

    ~RangeCache() {
        RetiredItem ri;
        while (retired_q_.try_dequeue(ri))
            freeEntryToSlabOffset(ri.offset);
    }

    // ---------------- Public API ----------------

    EntryT* search(const KeyT& key) {
        ThreadGuard guard(this);

        EntryT query(key, key, ItemT{});
        skiplist_node* node = skiplist_find_smaller_or_equal(&list_, &query.node_);
        if (!node || node == &list_.tail) return nullptr;

        EntryT* e = _get_entry(node, EntryT, node_);
        if (key >= e->start && key < e->end) {
            updateAccess(e);
            return e;
        }
        return nullptr;
    }

    void add(const KeyT& start, const KeyT& end, const ItemT& item) {
        ThreadGuard guard(this);

        int attempts = 0;
        while (attempts++ < 3) {
            EntryT* e = allocEntry(start, end, item);
            if (e) {
                skiplist_insert(&list_, &e->node_);
                return;
            }
            try_reclaim_budgeted(64);
            evict_power_of_two();
        }
        // Drop entry if allocation fails
    }

    void invalidate(EntryT* e) {
        if (!e) return;
        ThreadGuard guard(this);

        if (skiplist_erase_node(&list_, &e->node_) != 0) return;
        retire_entry(reinterpret_cast<int64_t>(e));
    }

private:
    // ---------------- Slab helpers ----------------
    EntryT* allocEntry(const KeyT& s, const KeyT& e, const ItemT& item) {
        int64_t off = entrySlab_->allocate_nogrowth();
        if (off == -1) return nullptr;
        void* ptr = reinterpret_cast<void*>(off);
        return new (ptr) EntryT(s, e, item);
    }

    void freeEntryToSlabOffset(int64_t off) {
        void* ptr = reinterpret_cast<void*>(off);
        EntryT* e = reinterpret_cast<EntryT*>(ptr);
        e->~EntryT();
        entrySlab_->free(off);
    }

    // ---------------- Thread registry ----------------
    ThreadRecord* ensure_thread_record() {
        if (tls_tr_) return tls_tr_;
        ThreadRecord* tr = new ThreadRecord();
        tr->epoch.store(0, std::memory_order_relaxed);

        ThreadRecord* old = registry_head_.load(std::memory_order_acquire);
        do { tr->next.store(old, std::memory_order_relaxed); }
        while (!registry_head_.compare_exchange_weak(old, tr,
                    std::memory_order_release, std::memory_order_acquire));

        tls_tr_ = tr;
        return tr;
    }

    // ---------------- Thread guard ----------------
    struct ThreadGuard {
        ThreadRecord* tr;
        RangeCache* cache;
        ThreadGuard(RangeCache* c) : cache(c) {
            tr = cache->ensure_thread_record();
            uint64_t e = cache->global_epoch_.load(std::memory_order_acquire);
            tr->epoch.store(e, std::memory_order_release);
        }
        ~ThreadGuard() { tr->epoch.store(0, std::memory_order_release); }
    };

    // ---------------- Retire / Reclaim ----------------
    void retire_entry(int64_t offset) {
        retired_q_.enqueue(RetiredItem{offset, global_epoch_.load(std::memory_order_acquire)});
    }

    void try_reclaim_budgeted(size_t budget) {
        std::vector<RetiredItem> batch;
        batch.reserve(budget);
        RetiredItem ri; size_t n = 0;
        while (n < budget && retired_q_.try_dequeue(ri)) { batch.push_back(ri); ++n; }
        if (batch.empty()) return;

        uint64_t min_epoch = scan_min_epoch();
        std::vector<RetiredItem> to_reenqueue;
        for (auto &r : batch) {
            if (r.retire_epoch < min_epoch) freeEntryToSlabOffset(r.offset);
            else to_reenqueue.push_back(r);
        }
        for (auto &r : to_reenqueue) retired_q_.enqueue(r);
        if (!to_reenqueue.empty())
            global_epoch_.fetch_add(1, std::memory_order_acq_rel);
    }

    uint64_t scan_min_epoch() {
        uint64_t min_epoch = UINT64_MAX;
        for (ThreadRecord* cur = registry_head_.load(std::memory_order_acquire); cur; cur = cur->next.load(std::memory_order_acquire)) {
            uint64_t e = cur->epoch.load(std::memory_order_acquire);
            if (e != 0 && e < min_epoch) min_epoch = e;
        }
        return min_epoch == UINT64_MAX ? global_epoch_.load(std::memory_order_acquire) : min_epoch;
    }

    // ---------------- Eviction ----------------
    inline EntryT* pick_victim_sample() {
        uint32_t total = list_.num_entries;
        if (total == 0)
            return nullptr;

        // 1) Pick global index
        uint32_t r = tls_xorshift() % total;

        // 2) Map index -> skiplist layer
        uint32_t accum = 0;
        int lvl = 0;

        // Unrolled small loop – fast in hot cache
        for (int L = 0; L <= list_.top_layer; ++L) {
            accum += list_.layer_entries[L];
            if (r < accum) {
                lvl = L;
                break;
            }
        }

        // 3) Pick random node within chosen level
        uint32_t idx = tls_xorshift() % list_.layer_entries[lvl];

        skiplist_node* x = &list_.head;
        while (idx-- && x->next[lvl] != &list_.tail)
            x = x->next[lvl];

        x = x->next[lvl];
        if (!x || x == &list_.tail)
            return nullptr;

        return _get_entry(x, EntryT, node_);
    }

    void evict_power_of_two() {
        if (entrySlab_->free_count() > 0) return;
        EntryT* v1 = pick_victim_sample();
        if (!v1) return;
        EntryT* v2 = pick_victim_sample();
        if (!v2) return;

        uint64_t a1 = v1->lastAccess.load(std::memory_order_relaxed);
        uint64_t a2 = v2->lastAccess.load(std::memory_order_relaxed);

        EntryT* victim = (a1 < a2) ? v1 : v2;
        // std::cout << "Eviction: " << a1 << " vs " << a2 << std::endl;

        if (skiplist_erase_node(&list_, &victim->node_) != 0) return;
        retire_entry(reinterpret_cast<int64_t>(victim));
    }

    // ---------------- Access ----------------
    void updateAccess(EntryT* e) {
        uint64_t c = tls_local_access_counter_++;
        if ((c & (accessUpdateInterval_ - 1)) == 0)
            e->lastAccess.store(next_timestamp(), std::memory_order_relaxed);
    }

    // ---------------- Compare ----------------
    static int cmpFunc(skiplist_node* a, skiplist_node* b, void*) {
        EntryT* ea = _get_entry(a, EntryT, node_);
        EntryT* eb = _get_entry(b, EntryT, node_);
        return (ea->start < eb->start) ? -1 : (ea->start > eb->start) ? 1 : 0;
    }

private:
    skiplist_raw list_;
    size_t maxEntries_;
    std::shared_ptr<SlabAllocator> entrySlab_;
    uint64_t accessUpdateInterval_;
    unsigned sampling_steps_;

    std::atomic<uint64_t> global_epoch_{0};
    std::atomic<ThreadRecord*> registry_head_{nullptr};
    moodycamel::ConcurrentQueue<RetiredItem> retired_q_;

    static thread_local ThreadRecord* tls_tr_;
    static thread_local uint64_t tls_local_access_counter_;
    static thread_local uint32_t tls_xorshift_state_;

    uint32_t tls_xorshift() {
        uint32_t x = tls_xorshift_state_;
        if (x == 0) x = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(this) ^ std::random_device{}());
        x ^= x << 13; x ^= x >> 17; x ^= x << 5;
        tls_xorshift_state_ = x; return x;
    }
};

template<typename K, typename V> thread_local typename RangeCache<K,V>::ThreadRecord* RangeCache<K,V>::tls_tr_ = nullptr;
template<typename K, typename V> thread_local uint64_t RangeCache<K,V>::tls_local_access_counter_ = 0;
template<typename K, typename V> thread_local uint32_t RangeCache<K,V>::tls_xorshift_state_ = 0;
