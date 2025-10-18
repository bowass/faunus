#include "compute_server.hpp"
#include "rdma_manager.hpp"
#include "../util/thread_logging.hpp"
#include "../util/cpu_affinity.hpp"


ComputeServer::ComputeServer(int id, int num_threads, std::shared_ptr<RDMAManager> rdma_mgr, std::shared_ptr<LocalAllocator> allocator, WorkerFunc worker, std::optional<size_t> core_id, bool use_exclusive_binding)
    : id_(id), num_threads_(num_threads), rdma_mgr_(rdma_mgr), allocator_(allocator), stats_(num_threads), worker_(std::move(worker)), core_id_(core_id), use_exclusive_binding_(use_exclusive_binding) {}

void ComputeServer::start() {
    for (int i = 0; i < num_threads_; ++i) {
        workers_.emplace_back([this, i]() {
            thread_log::setup_thread_log();
            
            // Bind to CPU core if specified
            if (core_id_.has_value()) {
                bool bind_success = false;
                if (use_exclusive_binding_) {
                    bind_success = util::CPUAffinity::bind_to_core_exclusive(core_id_.value());
                    if (bind_success) {
                        LOG_INFO("CS " << id_ << " thread " << i << " exclusively bound to core " << core_id_.value());
                    } else {
                        LOG_ERROR("CS " << id_ << " thread " << i << " failed to exclusively bind to core " << core_id_.value());
                    }
                } else {
                    bind_success = util::CPUAffinity::bind_to_core(core_id_.value());
                    if (bind_success) {
                        LOG_INFO("CS " << id_ << " thread " << i << " bound to core " << core_id_.value());
                    } else {
                        LOG_ERROR("CS " << id_ << " thread " << i << " failed to bind to core " << core_id_.value());
                    }
                }
            }
            
            worker_(i, stats_[i], rdma_mgr_, allocator_);
            thread_log::close_thread_log();
        });
    }
}

void ComputeServer::join() {
    for (auto& t : workers_) t.join();
}
