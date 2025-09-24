#include <iostream>
#include <cassert>
#include <string>
#include <vector>
#include "../rdma/rdma_manager.hpp"
#include "../rdma/memory_server.hpp"
#include "../rdma/rpc_allocator.hpp"
#include "../rdma/compute_server.hpp"
#include "../config/faunus_config.hpp"
#include "../kv_index/kv_index_rdma.hpp"

int main(int argc, char* argv[]) {
    std::string config_path = "faunus_config.yaml";
    if (argc > 1) config_path = argv[1];
    // Load configuration from YAML file
    FaunusConfig config = load_faunus_config(config_path);
    const int num_ms = config.num_ms;
    const size_t mem_per_server = config.mem_per_ms;
    const double base_rtt_us = config.base_rtt_us;

    // Create memory servers
    std::vector<MemoryServer*> mem_servers;
    for (int i = 0; i < num_ms; ++i) {
        mem_servers.push_back(new MemoryServer(mem_per_server));
    }
    // Create RDMA manager
    RDMAManager rdma_mgr(mem_servers, mem_per_server, base_rtt_us);

    std::cout << "\n====================== KV-index RDMA Stress Test ======================\n";
    const int num_cs = config.num_cs;
    const int threads_per_cs = config.threads_per_cs;
    const int kv_per_thread = 1000;
    std::vector<std::unique_ptr<ComputeServer>> compute_servers;
    std::atomic<size_t> total_ops{0};
    std::atomic<size_t> total_failures{0};
    auto kv_index = std::make_shared<KVIndexRDMA>(rdma_mgr);
    std::mutex kv_mutex;
    for (int cs_id = 0; cs_id < num_cs; ++cs_id) {
        auto worker = [cs_id, threads_per_cs, kv_per_thread, &total_ops, &total_failures, kv_index, &kv_mutex](int tid, ThreadStats& stat, RDMAManager& rdma_mgr) {
            std::vector<std::pair<std::string, std::string>> kv_pairs;
            for (int i = 0; i < kv_per_thread; ++i) {
                kv_pairs.emplace_back("key_" + std::to_string(cs_id) + "_" + std::to_string(tid) + "_" + std::to_string(i),
                                     "val_" + std::to_string(cs_id) + "_" + std::to_string(tid) + "_" + std::to_string(i));
            }
            // Insert
            for (const auto& kv : kv_pairs) {
                bool ok;
                {
                    std::lock_guard<std::mutex> lock(kv_mutex);
                    ok = kv_index->insert(kv.first, kv.second);
                }
                total_ops++;
                assert(ok);
                if (!ok) total_failures++;
            }
            // Read
            for (const auto& kv : kv_pairs) {
                std::string val;
                bool ok;
                {
                    std::lock_guard<std::mutex> lock(kv_mutex);
                    ok = kv_index->read(kv.first, val);
                }
                total_ops++;
                if (val[val.size() - 1] == '8' && val[val.size() - 2] == '3' && val[val.size() - 3] == '1')
                if (val != kv.second) std::cout << "Mismatch: expected " << kv.second << ", got " << val << std::endl;
                if (!ok || val != kv.second) total_failures++;
            }
            // Update
            for (const auto& kv : kv_pairs) {
                std::string new_val = "A" + kv.second.substr(1);
                bool ok;
                {
                    std::lock_guard<std::mutex> lock(kv_mutex);
                    ok = kv_index->update(kv.first, new_val.substr(0, kv.second.size()));
                }
                total_ops++;
                assert(ok);
                if (!ok) total_failures++;
                std::string val;
                bool ok2;
                {
                    std::lock_guard<std::mutex> lock(kv_mutex);
                    ok2 = kv_index->read(kv.first, val);
                }
                assert(ok2);
                total_ops++;
                if (!ok2 || val != new_val.substr(0, kv.second.size())) total_failures++;
            }
            // Delete
            for (const auto& kv : kv_pairs) {
                bool ok;
                {
                    std::lock_guard<std::mutex> lock(kv_mutex);
                    ok = kv_index->del(kv.first);
                }
                total_ops++;
                if (!ok) total_failures++;
                assert(ok);
                std::string val;
                bool ok2;
                {
                    std::lock_guard<std::mutex> lock(kv_mutex);
                    ok2 = kv_index->read(kv.first, val);
                }
                total_ops++;
                assert(!ok2);
            }
            stat.ops = kv_per_thread * 6; // insert, read, update, read-after-update, delete, read-after-delete
        };
        compute_servers.push_back(std::make_unique<ComputeServer>(cs_id, threads_per_cs, rdma_mgr, worker, nullptr));
    }
    for (auto& cs : compute_servers) cs->start();
    for (auto& cs : compute_servers) cs->join();
    std::cout << "[KVIndexRDMA] Stress test complete. Total ops: " << total_ops << ", failures: " << total_failures << std::endl;
    for (auto ms : mem_servers) delete ms;
    return 0;
}
