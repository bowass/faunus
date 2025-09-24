#include <iostream>
#include <cstring>
#include <cassert>
#include <iomanip>
#include "../rdma/compute_server.hpp"
#include "../rdma/memory_server.hpp"
#include "../rdma/rdma_manager.hpp"
#include "../rdma/rpc_allocator.hpp"
#include "../config/faunus_config.hpp"
#include "../kv_index/kv_index_rdma.hpp"

int main(int argc, char* argv[]) {
    std::string config_path = "faunus_config.yaml";
    if (argc > 1) config_path = argv[1];
    // Load configuration from YAML file
    FaunusConfig config = load_faunus_config(config_path);

    const int threads_per_cs = config.threads_per_cs;
    const int num_cs = config.num_cs;
    const int num_ms = config.num_ms;
    const size_t mem_per_server = config.mem_per_ms;
    const double base_rtt_us = config.base_rtt_us;

    // Create memory servers
    std::vector<MemoryServer*> mem_servers;
    for (int i = 0; i < num_ms; ++i) {
        mem_servers.push_back(new MemoryServer(mem_per_server));
    }

    // Create RPC allocator
    RPCAllocator rpc_alloc(mem_servers);

    // Create RDMA manager
    RDMAManager rdma_mgr(mem_servers, mem_per_server, base_rtt_us);

    // -------------------- RDMA/Allocator/Compute Server Stress Tests --------------------
    std::cout << "\n================ RDMA/Allocator/Compute Server Stress Tests ================\n";
    // ...existing code for compute servers, RDMA ops, CAS/FAA stress tests...
    std::vector<int64_t> allocated;
    const size_t alloc_count = 10000;
    const size_t chunk_size = 64;
    uint8_t test_data[chunk_size];
    memset(test_data, 0xAB, chunk_size);
    std::vector<size_t> chunk_sizes = {8, 64, 256, 1024};
    std::vector<std::unique_ptr<ComputeServer>> compute_servers;
    for (int cs_id = 0; cs_id < num_cs; ++cs_id) {
        auto local_alloc = std::make_unique<LocalAllocator>(chunk_sizes, 128, &rpc_alloc);
        LocalAllocator* alloc_ptr = local_alloc.get();
        auto compute_worker = [alloc_ptr, chunk_size, alloc_count, threads_per_cs, test_data](int tid, ThreadStats& stat, RDMAManager& rdma_mgr) {
            size_t per_thread = alloc_count / threads_per_cs;
            std::vector<int64_t> thread_allocs;
            if (!alloc_ptr) {
                std::cerr << "[Error] LocalAllocator is null in worker!" << std::endl;
                return;
            }
            stat.latency_us = 0.0;
            stat.min_latency_us = std::numeric_limits<double>::max();
            stat.max_latency_us = 0.0;
            for (size_t i = 0; i < per_thread; ++i) {
                int64_t addr = alloc_ptr->allocate(chunk_size);
                thread_allocs.push_back(addr);
            }
            // RDMA writes
            for (size_t i = 0; i < thread_allocs.size(); ++i) {
                GlobalAddress gaddr(0, thread_allocs[i]);
                RDMAOp op{std::chrono::high_resolution_clock::time_point{}, std::chrono::high_resolution_clock::time_point{}, RDMAOpType::WRITE, 0, gaddr};
                op.op.write.buffer = test_data;
                op.op.write.bytes = chunk_size;
                assert(rdma_mgr.perform_op(op));
                double us = std::chrono::duration_cast<std::chrono::duration<double, std::micro>>(op.end_time - op.start_time).count();
                stat.latency_us += us;
                if (us < stat.min_latency_us) stat.min_latency_us = us;
                if (us > stat.max_latency_us) stat.max_latency_us = us;
            }
            // RDMA reads
            for (size_t i = 0; i < thread_allocs.size(); ++i) {
                GlobalAddress gaddr(0, thread_allocs[i]);
                RDMAOp op{std::chrono::high_resolution_clock::time_point{}, std::chrono::high_resolution_clock::time_point{}, RDMAOpType::READ, 0, gaddr};
                uint8_t read_buf[chunk_size];
                op.op.read.buffer = read_buf;
                op.op.read.bytes = chunk_size;
                rdma_mgr.perform_op(op);
                double us = std::chrono::duration_cast<std::chrono::duration<double, std::micro>>(op.end_time - op.start_time).count();
                stat.latency_us += us;
                if (us < stat.min_latency_us) stat.min_latency_us = us;
                if (us > stat.max_latency_us) stat.max_latency_us = us;
                for (size_t j = 0; j < chunk_size; ++j) assert(read_buf[j] == 0xAB);
                if (read_buf[0] != 0xAB) {
                    std::cerr << "[Error] Data mismatch at thread " << tid << ", idx " << i << ", data " << int(read_buf[0]) << std::endl;
                    for (size_t j = 0; j < chunk_size; ++j) {
                        std::cerr << std::hex << std::setw(2) << std::setfill('0') << int(read_buf[j]) << " ";
                    }
                    std::cerr << std::dec << std::endl;
                    break;
                }
            }
            // Free half
            for (size_t i = 0; i < thread_allocs.size(); i += 2) {
                alloc_ptr->free(chunk_size, thread_allocs[i]);
            }
            stat.ops = thread_allocs.size();
        };
    compute_servers.push_back(std::make_unique<ComputeServer>(cs_id, threads_per_cs, rdma_mgr, compute_worker, std::move(local_alloc)));
    }
    std::cout << "[Test] Running compute server threads..." << std::endl;
    for (auto& cs : compute_servers) cs->start();
    for (auto& cs : compute_servers) cs->join();
    auto print_stats = [](const std::string& label, const std::vector<ThreadStats>& stats) {
        double total_ops = 0, total_latency = 0;
        double min_lat = std::numeric_limits<double>::max(), max_lat = 0;
        for (size_t t = 0; t < stats.size(); ++t) {
            total_ops += stats[t].ops;
            total_latency += stats[t].latency_us;
            if (stats[t].min_latency_us < min_lat) min_lat = stats[t].min_latency_us;
            if (stats[t].max_latency_us > max_lat) max_lat = stats[t].max_latency_us;
        }
        std::cout << label << ":\n";
        std::cout << "  Total ops: " << total_ops << std::endl;
        std::cout << "  Avg latency per op (us): " << std::fixed << std::setprecision(2) << (total_latency / total_ops) << std::endl;
        std::cout << "  Min latency (us): " << min_lat << std::endl;
        std::cout << "  Max latency (us): " << max_lat << std::endl;
    };
    for (size_t i = 0; i < compute_servers.size(); ++i) {
        print_stats("ComputeServer " + std::to_string(i) + " test", compute_servers[i]->get_thread_stats());
    }
    std::cout << "[Test] Allocation and RDMA test complete." << std::endl;
    // --- RDMA CAS and FAA Stress Test ---
    std::cout << "[StressTest] Starting RDMA CAS and FAA stress test..." << std::endl;
    const int cas_threads = 8;
    const int faa_threads = 8;
    const int cas_ops_per_thread = 10000;
    const int faa_ops_per_thread = 10000;
    int64_t shared_addr = mem_servers[0]->allocate(8); // 8 bytes for atomic ops
    GlobalAddress shared_gaddr(0, shared_addr);
    uint64_t initial = 0;
    RDMAOp init_op{std::chrono::high_resolution_clock::time_point{}, std::chrono::high_resolution_clock::time_point{}, RDMAOpType::WRITE, 0, shared_gaddr};
    init_op.op.write.buffer = reinterpret_cast<uint8_t*>(&initial);
    init_op.op.write.bytes = sizeof(uint64_t);
    assert(rdma_mgr.perform_op(init_op));
    std::vector<std::thread> cas_threads_vec;
    std::atomic<uint64_t> cas_success{0};
    for (int t = 0; t < cas_threads; ++t) {
        cas_threads_vec.emplace_back([&, t]() {
            for (int i = 0; i < cas_ops_per_thread; ++i) {
                RDMAOp op{std::chrono::high_resolution_clock::time_point{}, std::chrono::high_resolution_clock::time_point{}, RDMAOpType::CAS, 0, shared_gaddr};
                op.op.cas.expected = i;
                op.op.cas.desired = i + 1;
                bool rv = rdma_mgr.perform_op(op);
                if (rv) cas_success++;
            }
        });
    }
    for (auto& th : cas_threads_vec) th.join();
    std::cout << "[StressTest] CAS success count: " << cas_success << " (expected: " << cas_ops_per_thread << ")" << std::endl;
    std::vector<std::thread> faa_threads_vec;
    std::atomic<uint64_t> faa_total{0};
    for (int t = 0; t < faa_threads; ++t) {
        faa_threads_vec.emplace_back([&, t]() {
            for (int i = 0; i < faa_ops_per_thread; ++i) {
                RDMAOp op{std::chrono::high_resolution_clock::time_point{}, std::chrono::high_resolution_clock::time_point{}, RDMAOpType::FAA, 0, shared_gaddr};
                op.op.faa.increment = 1;
                rdma_mgr.perform_op(op);
                faa_total++;
            }
        });
    }
    for (auto& th : faa_threads_vec) th.join();
    std::cout << "[StressTest] FAA total increments: " << faa_total << " (expected: " << faa_threads * faa_ops_per_thread << ")" << std::endl;

    // -------------------- KV-index RDMA Tests --------------------
    std::cout << "\n====================== KV-index RDMA Tests ======================\n";
    KVIndexRDMA kv_index(rdma_mgr);
    std::vector<std::pair<std::string, std::string>> kv_pairs = {
        {"key1", "value1"},
        {"key2", "value2"},
        {"key3", "value3"},
        {"key4", "value4"}
    };
    for (const auto& kv : kv_pairs) {
        bool ok = kv_index.insert(kv.first, kv.second);
        std::cout << "Insert(" << kv.first << ", " << kv.second << ") -> " << (ok ? "OK" : "FAIL") << std::endl;
        assert(ok);
    }
    for (const auto& kv : kv_pairs) {
        std::string val;
        bool ok = kv_index.read(kv.first, val);
        std::cout << "Read(" << kv.first << ") -> " << (ok ? val : "FAIL") << std::endl;
        assert(ok && val == kv.second);
    }
    for (const auto& kv : kv_pairs) {
        std::string new_val = "A" + kv.second.substr(1);
        bool ok = kv_index.update(kv.first, new_val.substr(0, kv.second.size())); // Only overwrite, not resize
        std::cout << "Update(" << kv.first << ", " << new_val << ") -> " << (ok ? "OK" : "FAIL") << std::endl;
        std::string val;
        bool ok2 = kv_index.read(kv.first, val);
        std::cout << "Read after update(" << kv.first << ") -> " << (ok2 ? val : "FAIL") << std::endl;
        assert(ok && ok2 && val == new_val.substr(0, kv.second.size()));
    }
    for (const auto& kv : kv_pairs) {
        bool ok = kv_index.del(kv.first);
        std::cout << "Delete(" << kv.first << ") -> " << (ok ? "OK" : "FAIL") << std::endl;
        std::string val;
        bool ok2 = kv_index.read(kv.first, val);
        std::cout << "Read after delete(" << kv.first << ") -> " << (ok2 ? val : "NOT FOUND") << std::endl;
        assert(ok && !ok2);
    }
    std::cout << "[KVIndexRDMA] Baseline test complete." << std::endl;

    // Clean up
    for (auto ms : mem_servers) delete ms;
    return 0;
}
