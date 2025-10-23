#pragma once
#include "../externals/concurrentqueue/concurrentqueue.h"
#include <condition_variable>
#include <mutex>
#include <optional>
#include <chrono>

// Generic, thread-safe queue for maintenance RPCs
// T should be the MaintenanceRPC type for a specific index

template <typename T>
class MaintenanceQueue {
public:
    // Enqueue a new RPC request
    void enqueue(T&& rpc) {
        queue_.enqueue(std::move(rpc));
    }

    // Try to dequeue an RPC request (non-blocking)
    bool try_dequeue(T& rpc) {
        return queue_.try_dequeue(rpc);
    }

    // Wait for and dequeue an RPC request (blocking)
    T wait_dequeue() {
        T rpc;
        // Try a few times with exponential backoff before falling back to condition variable
        for (int attempts = 0; attempts < 1000; ++attempts) {
            if (queue_.try_dequeue(rpc)) {
                return rpc;
            }
            // Small yield to avoid busy waiting
            std::this_thread::yield();
        }
        
        // Fall back to condition variable waiting
        while (!queue_.try_dequeue(rpc)) {
            std::this_thread::sleep_for(std::chrono::microseconds(100));
        }
        return rpc;
    }

    // Check if the queue is empty (approximate due to lock-free nature)
    bool empty() const {
        T dummy;
        return !const_cast<moodycamel::ConcurrentQueue<T>&>(queue_).try_dequeue(dummy);
    }

private:
    moodycamel::ConcurrentQueue<T> queue_;
};
