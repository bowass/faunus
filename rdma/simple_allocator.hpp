#pragma once

#include "local_allocator.hpp"
#include <mutex>
#include <unordered_set>
#include <new>
#include <cstdint>
#include <cstddef>

// TODO: this is shitty, if we'll use it - replace the mutex with a better mechanism

// A simple in-process Allocator implementation that uses operator new/delete
// to back chunk allocations. It returns the pointer value cast to int64_t
// as the 'offset' expected by the existing SlabAllocator/LocalAllocator
// interfaces. This is intended for single-process/local testing.
class SimpleAllocator : public Allocator {
public:
    SimpleAllocator() = default;
    ~SimpleAllocator() override {
        // Free any outstanding allocations to avoid leaks in tests.
        std::lock_guard<std::mutex> lk(mu_);
        for (auto p : allocs_) {
            void *ptr = reinterpret_cast<void*>(p);
            ::operator delete(ptr);
        }
        allocs_.clear();
    }

    // Allocate 'chunk_size' bytes; return pointer-as-int64 offset, or -1 on failure
    int64_t allocate(size_t chunk_size) override {
        try {
            void *p = ::operator new(chunk_size);
            uintptr_t v = reinterpret_cast<uintptr_t>(p);
            {
                std::lock_guard<std::mutex> lk(mu_);
                allocs_.insert(v);
            }
            return static_cast<int64_t>(v);
        } catch (const std::bad_alloc &) {
            return -1;
        }
    }

    // Free the chunk at the given offset (pointer-as-int64). If not owned,
    // this becomes a no-op.
    void free(size_t /*chunk_size*/, int64_t offset) override {
        uintptr_t v = static_cast<uintptr_t>(offset);
        std::lock_guard<std::mutex> lk(mu_);
        auto it = allocs_.find(v);
        if (it == allocs_.end()) return; // unknown pointer
        void *p = reinterpret_cast<void*>(v);
        ::operator delete(p);
        allocs_.erase(it);
    }

private:
    std::mutex mu_;
    std::unordered_set<uintptr_t> allocs_;
};
