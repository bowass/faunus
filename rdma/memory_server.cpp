#include "memory_server.hpp"


// Track allocations and frees

MemoryServer::MemoryServer(size_t memory_size)
    : rdma_(memory_size), next_offset_(0), stop_(false) {
    worker_ = std::thread([this]{ worker_loop(); });
}

MemoryServer::~MemoryServer() {
    stop_ = true;
    queue_cv_.notify_all();
    if (worker_.joinable()) worker_.join();
}
// Enqueue allocation request
void MemoryServer::enqueue_alloc(size_t size, std::promise<size_t>& promise) {
    {
        std::lock_guard<std::mutex> lock(queue_mu_);
        alloc_queue_.emplace(size, &promise);
    }
    queue_cv_.notify_one();
}

// Enqueue free request
void MemoryServer::enqueue_free(size_t offset) {
    {
        std::lock_guard<std::mutex> lock(queue_mu_);
        free_queue_.push(offset);
    }
    queue_cv_.notify_one();
}

// Worker loop for processing allocation/free requests
void MemoryServer::worker_loop() {
    while (!stop_) {
        std::unique_lock<std::mutex> lock(queue_mu_);
        queue_cv_.wait(lock, [this]{ return stop_ || !alloc_queue_.empty() || !free_queue_.empty(); });
        if (stop_) break;
        // Process alloc requests
        while (!alloc_queue_.empty()) {
            auto [size, promise_ptr] = alloc_queue_.front();
            alloc_queue_.pop();
            lock.unlock();
            size_t offset = allocate(size);
            promise_ptr->set_value(offset);
            lock.lock();
        }
        // Process free requests
        while (!free_queue_.empty()) {
            size_t offset = free_queue_.front();
            free_queue_.pop();
            lock.unlock();
            free(offset);
            lock.lock();
        }
    }
}

// Allocate memory, return offset
size_t MemoryServer::allocate(size_t size) {
    std::lock_guard<std::mutex> lock(mu_);
    size_t offset = next_offset_;
    next_offset_ += size;
    allocated_.insert(offset);
    return offset;
}

// Free memory by offset
void MemoryServer::free(size_t offset) {
    std::lock_guard<std::mutex> lock(mu_);
    allocated_.erase(offset);
}

RDMASimulation& MemoryServer::get_rdma() {
    return rdma_;
}
