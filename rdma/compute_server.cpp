#include "compute_server.hpp"
#include "rdma_manager.hpp"
#include "../util/thread_logging.hpp"


ComputeServer::ComputeServer(int id, int num_threads, std::shared_ptr<RDMAManager> rdma_mgr, std::shared_ptr<LocalAllocator> allocator, WorkerFunc worker)
    : id_(id), num_threads_(num_threads), rdma_mgr_(rdma_mgr), allocator_(allocator), stats_(num_threads), worker_(std::move(worker)) {}

void ComputeServer::start() {
    for (int i = 0; i < num_threads_; ++i) {
        workers_.emplace_back([this, i]() {
            faunus_log::setup_thread_log();
            worker_(i, stats_[i], rdma_mgr_, allocator_);
            faunus_log::close_thread_log();
        });
    }
}

void ComputeServer::join() {
    for (auto& t : workers_) t.join();
}
