#pragma once
#include "../concurrentqueue/concurrentqueue.h"
#include <unordered_set>
#include <mutex>
#include <condition_variable>
#include <chrono>
#include <functional>
#include <thread>

// Thread-safe queued-set that prevents duplicate entries from being enqueued
// T should be the RPC type, K should be the key type for uniqueness checking
// KeyExtractor should extract the key from T for uniqueness comparison

template <typename T, typename K, typename KeyExtractor, typename KeyHash = std::hash<K>>
class QueuedSet {
public:
    explicit QueuedSet(KeyExtractor key_extractor = KeyExtractor{}) 
        : key_extractor_(key_extractor) {}

    // Enqueue a new RPC request only if it doesn't already exist
    // Returns true if enqueued, false if already exists
    bool try_enqueue(T&& rpc) {
        K key = key_extractor_(rpc);
        
        {
            std::lock_guard<std::mutex> lock(set_mutex_);
            
            // Check if this key already exists in the set
            auto result = pending_keys_.insert(key);
            if (!result.second) {
                // Key already exists, don't enqueue
                return false;
            }
        }
        
        // Key is new, enqueue the RPC
        queue_.enqueue(std::move(rpc));
        return true;
    }

    // Force enqueue (always add, used for STOP commands etc.)
    void enqueue(T&& rpc) {
        K key = key_extractor_(rpc);
        
        {
            std::lock_guard<std::mutex> lock(set_mutex_);
            pending_keys_.insert(key);
        }
        
        queue_.enqueue(std::move(rpc));
    }

    // Try to dequeue an RPC request (non-blocking)
    bool try_dequeue(T& rpc) {
        if (queue_.try_dequeue(rpc)) {
            // Successfully dequeued, remove from pending set
            K key = key_extractor_(rpc);
            {
                std::lock_guard<std::mutex> lock(set_mutex_);
                pending_keys_.erase(key);
            }
            return true;
        }
        return false;
    }

    // Wait for and dequeue an RPC request (blocking)
    T wait_dequeue() {
        T rpc;
        
        // Try a few times with exponential backoff before falling back to sleep
        for (int attempts = 0; attempts < 1000; ++attempts) {
            if (try_dequeue(rpc)) {
                return rpc;
            }
            std::this_thread::yield();
        }
        
        // Fall back to periodic checking
        while (!try_dequeue(rpc)) {
            std::this_thread::sleep_for(std::chrono::microseconds(100));
        }
        
        return rpc;
    }

    // Check if the queue is empty (approximate)
    bool empty() const {
        T dummy;
        return !const_cast<moodycamel::ConcurrentQueue<T>&>(queue_).try_dequeue(dummy);
    }

    // Get the current number of pending unique keys (for monitoring)
    size_t pending_count() const {
        std::lock_guard<std::mutex> lock(set_mutex_);
        return pending_keys_.size();
    }

private:
    moodycamel::ConcurrentQueue<T> queue_;
    mutable std::mutex set_mutex_;
    std::unordered_set<K, KeyHash> pending_keys_;
    KeyExtractor key_extractor_;
};
