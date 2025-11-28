#pragma once
#include "externals/concurrentqueue/concurrentqueue.h"
#include "util/thread_logging.hpp"
#include "util/profiler.hpp"

#include <cstddef>
#include <cstdint>
#include <iostream>
#include <mutex>
#include <memory>
#include <set>
#include <unordered_map>
#include <vector>

/**
 * @brief Abstract Allocator interface for chunk management.
 */
class Allocator {
public:
    virtual ~Allocator() = default;
    // Allocate a chunk, returns offset or -1 if none available
    virtual int64_t allocate(size_t chunk_size) = 0;
    // Free a chunk by offset
    virtual void free(size_t chunk_size, int64_t offset) = 0;
};

/**
 * @brief Thread-safe slab allocator for fixed-size chunks, backed by an Allocator.
 */
class SlabAllocator {
public:
    SlabAllocator(size_t slab_size, size_t num_slabs, std::shared_ptr<Allocator> allocator)
        : slab_size_(slab_size), num_slabs_(num_slabs), allocator_(allocator) {
        get_slabs_nolock(num_slabs_);
    }
    // Allocate a chunk, with exponential growth if needed
    int64_t allocate_nogrowth() {
        // Profiler::Scoped scope("SlabAllocator::allocate_nogrowth");
        int64_t offset;
        if (free_list_.try_dequeue(offset)) {
            return offset;
        }
        return -1; // No free slab available
    }
    int64_t allocate() {
        int64_t offset;
        if (free_list_.try_dequeue(offset)) {
            return offset;
        }
        // No free slab available: try to grow, but serialize growth to
        // avoid multiple threads performing expensive backing allocations.
        std::lock_guard<std::mutex> growth_lock(growth_mu_);
        // After acquiring the growth lock, try dequeue again in case
        // another thread already grew the slab.
        if (free_list_.try_dequeue(offset)) {
            return offset;
        }
        // Grow slabs and populate the queue
        get_slabs_nolock(num_slabs_);
        if (free_list_.try_dequeue(offset)) {
            return offset;
        }
        // Failed to allocate after grow
        return -1;
    }
    // Free a chunk by offset
    void free(int64_t offset) {
        free_list_.enqueue(offset);
    }
    size_t free_count() const { return free_list_.size_approx(); }
    size_t slab_size() const { return slab_size_; }
private:
    int64_t get_free_slab_nolock() {
        int64_t offset;
        if (free_list_.try_dequeue(offset)) {
            return offset;
        }
        return -1; // No free slab available
    }
    void get_slabs_nolock(size_t slabs_to_add) {
        int64_t chunk_size = static_cast<int64_t>(slabs_to_add) * static_cast<int64_t>(slab_size_);
        int64_t new_chunk = allocator_->allocate(static_cast<size_t>(chunk_size));
        if (new_chunk == -1) return;
        auto new_offsets = get_offsets_from_chunk(new_chunk, static_cast<size_t>(chunk_size));
        for (auto off : new_offsets) free_list_.enqueue(off);
        num_slabs_ += new_offsets.size();
    }
    std::vector<int64_t> get_offsets_from_chunk(int64_t start_offset, size_t chunk_size) {
        std::vector<int64_t> offsets;
        for (size_t offset = start_offset; offset < start_offset + chunk_size; offset += slab_size_) {
            offsets.push_back(offset);
        }
        return offsets;
    }
    size_t slab_size_;
    size_t num_slabs_;
    moodycamel::ConcurrentQueue<int64_t> free_list_;
    // Mutex used only for serializing slab growth
    std::mutex growth_mu_;
    std::shared_ptr<Allocator> allocator_;
};

/**
 * @brief LocalAllocator manages multiple slab allocators for a single CS.
 */
class LocalAllocator {
public:
    // sizes: list of chunk sizes, num_chunks: number of chunks per size
    LocalAllocator(const std::set<size_t>& sizes, size_t num_chunks_per_slab, std::shared_ptr<Allocator> allocator)
        : allocator_(allocator) {
        for (auto sz : sizes) {
            slabs_[sz] = std::make_unique<SlabAllocator>(sz, num_chunks_per_slab, allocator);
        }
    }
    // Thread-safe allocation for a given size
    int64_t allocate(size_t size) {
        auto it = slabs_.find(size);
        if (it == slabs_.end()) {
            return -1;
        }
        return it->second->allocate();
    }
    // Thread-safe free for a given size
    void free(size_t size, int64_t offset) {
        auto it = slabs_.find(size);
        if (it != slabs_.end()) it->second->free(offset);
    }
private:
    std::unordered_map<size_t, std::unique_ptr<SlabAllocator>> slabs_;
    std::shared_ptr<Allocator> allocator_;
};
