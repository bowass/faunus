#pragma once
#include <vector>
#include <unordered_map>
#include <mutex>
#include <memory>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <set>

#include "../util/logging.hpp"

using size_t = std::size_t;
using int64_t = std::int64_t;


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
    int64_t allocate() {
        std::lock_guard<std::mutex> lock(mu_);
        int64_t offset = get_free_slab_nolock();
        if (offset != -1) return offset;
        // No free slab, grow
        LOG_DEBUG("[SlabAllocator] Growing slab for size " << slab_size_ << " by " << num_slabs_);
        get_slabs_nolock(num_slabs_);
        offset = get_free_slab_nolock();
        if (offset == -1) {
            LOG_DEBUG("[SlabAllocator] Still failed to allocate after growth for size " << slab_size_);
        }
        return offset;
    }
    // Free a chunk by offset
    void free(int64_t offset) {
        std::lock_guard<std::mutex> lock(mu_);
        free_list_.push_back(offset);
        --used_chunks_;
    }
    size_t free_count() const { return free_list_.size(); }
    size_t slab_size() const { return slab_size_; }
private:
    int64_t get_free_slab_nolock() {
        if (!free_list_.empty()) {
            int64_t offset = free_list_.back();
            free_list_.pop_back();
            ++used_chunks_;
            return offset;
        }
        return -1; // No free slab available
    }
    void get_slabs_nolock(size_t slabs_to_add) {
        int64_t chunk_size = slabs_to_add * slab_size_;
        int64_t new_chunk = allocator_->allocate(chunk_size);
        auto new_offsets = get_offsets_from_chunk(new_chunk, chunk_size);
        for (auto off : new_offsets) free_list_.push_back(off);
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
    std::vector<int64_t> free_list_;
    size_t used_chunks_ = 0;
    std::mutex mu_;
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
            LOG_DEBUG("Creating slab for size " << sz << " at " << this);
            slabs_[sz] = std::make_unique<SlabAllocator>(sz, num_chunks_per_slab, allocator);
        }
    }
    // Thread-safe allocation for a given size
    // TODO: is map thread-safe? are the allocate() and free() in SlabAllocator thread-safe?
    int64_t allocate(size_t size) {
        LOG_DEBUG("Slabs at " << this);
        auto it = slabs_.find(size);
        if (it == slabs_.end()) {
            LOG_ERROR("[LocalAllocator] No slab for size " << size);
            return -1;
        }
        LOG_DEBUG("[LocalAllocator] Allocating size " << size);
        int64_t result = it->second->allocate();
        if (result == -1) {
            LOG_ERROR("[LocalAllocator] Allocation failed for size " << size);
        }
        return result;
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
