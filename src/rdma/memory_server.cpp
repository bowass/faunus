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
    alloc_queue_.enqueue(std::make_pair(size, &promise));
    queue_cv_.notify_one();
}

// Enqueue free request
void MemoryServer::enqueue_free(size_t offset) {
    free_queue_.enqueue(offset);
    queue_cv_.notify_one();
}

// Worker loop for processing allocation/free requests
void MemoryServer::worker_loop() {
    while (!stop_) {
        bool has_work = false;
        
        // Process alloc requests
        std::pair<size_t, std::promise<size_t>*> alloc_item;
        while (alloc_queue_.try_dequeue(alloc_item)) {
            has_work = true;
            size_t offset = allocate(alloc_item.first);
            alloc_item.second->set_value(offset);
        }
        
        // Process free requests
        size_t free_offset;
        while (free_queue_.try_dequeue(free_offset)) {
            has_work = true;
            free(free_offset);
        }
        
        // If no work was done, wait for notification
        if (!has_work && !stop_) {
            std::unique_lock<std::mutex> lock(queue_mu_);
            queue_cv_.wait_for(lock, std::chrono::milliseconds(1), [this]{ 
                return stop_.load(); 
            });
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
