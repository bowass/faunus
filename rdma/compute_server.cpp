#include "compute_server.hpp"
#include "rdma_manager.hpp"


ComputeServer::ComputeServer(int id, int num_threads, RDMAManager& rdma_mgr, WorkerFunc worker,
                             std::unique_ptr<LocalAllocator> allocator)
    : id_(id), num_threads_(num_threads), rdma_mgr_(rdma_mgr), stats_(num_threads), worker_(std::move(worker)), allocator_(std::move(allocator)) {}

void ComputeServer::start() {
    for (int i = 0; i < num_threads_; ++i) {
        workers_.emplace_back([this, i]() {
            worker_(i, stats_[i], rdma_mgr_);
        });
    }
}

void ComputeServer::join() {
    for (auto& t : workers_) t.join();
}


