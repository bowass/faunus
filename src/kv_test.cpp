#include <iostream>
#include "../util/thread_logging.hpp"
#include <cassert>
#include <string>
#include <vector>
#include <array>
#include <random>
#include <atomic>
#include <chrono>
#include <numeric>
#include <optional>
#include <iomanip>
#include <cstring>
#include <algorithm>
#include <limits>
#include <filesystem>
#include <fstream>
#include <unordered_map>
#include <condition_variable>
#include <mutex>
#include "../rdma/rdma_manager.hpp"
#include "../rdma/memory_server.hpp"
#include "../rdma/rpc_allocator.hpp"
#include "../rdma/compute_server.hpp"
#include "../config/config.hpp"
#include "../kv_index/faunus_index.hpp"
#include "../kv_index/fg_index.hpp"
#include "../util/profiler.hpp"
#include "../util/distribution.hpp"
#include "../util/key_management.hpp"
#include "../util/benchmark.hpp"
#include "../util/cpu_affinity.hpp"

// Using utility classes from dedicated headers

int main(int argc, char* argv[]) {
    // Setup thread log directory (delete previous logs)
    thread_log::setup_log_dir();

    std::string config_path = "config.yaml";
    if (argc > 1) config_path = argv[1];
    // Load configuration from YAML file
    IndexConfig config = load_config(config_path);
    const size_t num_ms = config.num_ms;
    const size_t mem_per_server = config.mem_per_ms;
    const double base_rtt_ns = config.base_rtt_ns;

    set_log_level(config.log_level);
    thread_log::setup_thread_log();
    Profiler::reset();

    // Small factory to create KVIndex implementations based on config.index
    auto make_index = [&config](std::shared_ptr<RDMAManager> rdma_mgr, std::shared_ptr<LocalAllocator> allocator, GlobalAddress root_offset_pointer, std::shared_ptr<IndexCacheBase> cache) -> std::unique_ptr<KVIndex> {
        switch (config.index_type) {
            case IndexConfig::IndexType::Faunus:
                return std::make_unique<FaunusIndex>(rdma_mgr, allocator, root_offset_pointer, cache);
            default:
                LOG_WARN("Unhandled index type - defaulting to FaunusIndex");
                return std::make_unique<FaunusIndex>(rdma_mgr, allocator, root_offset_pointer, cache);
        }
    };
    
    // Validate CPU binding configuration
    if (config.cpu_binding_enabled) {
        if (!util::CPUAffinity::is_supported()) {
            LOG_ERROR("CPU binding requested but not supported on this platform");
            return 1;
        }
        
        size_t available_cores = util::CPUAffinity::get_cpu_count();
        size_t required_cores = config.num_cs + config.maintenance_cs;  // Total CS + maintenance CS
        size_t max_core = config.cpu_binding_start_core + required_cores - 1;
        
        if (max_core >= available_cores) {
            LOG_ERROR("CPU binding configuration requires cores " 
                << config.cpu_binding_start_core << "-" << max_core 
                << " but only " << available_cores << " cores available (0-" << (available_cores-1) << ")");
            return 1;
        }
        
        LOG_INFO("CPU binding enabled: will bind " << config.num_cs 
            << " compute servers and " << config.maintenance_cs << " maintenance servers to cores " 
            << config.cpu_binding_start_core << "-" << max_core 
            << " (total cores available: " << available_cores << ")");
            
        // Check for core isolation if required
        if (config.cpu_isolation_required) {
            std::vector<size_t> required_core_ids;
            for (size_t i = 0; i < required_cores; ++i) {
                required_core_ids.push_back(config.cpu_binding_start_core + i);
            }
            
            if (!util::CPUAffinity::check_core_isolation(required_core_ids)) {
                LOG_ERROR("CPU isolation required but cores are not properly isolated");
                LOG_ERROR("To setup core isolation, run the following and reboot:");
                LOG_ERROR("\n" << util::CPUAffinity::get_isolation_recommendations(required_cores));
                return 1;
            }
            
            LOG_INFO("All required cores are properly isolated - excellent for accurate simulations");
        } else {
            LOG_WARN("CPU isolation not enforced - other processes may interfere with timing");
            LOG_WARN("For best results, enable cpu_isolation_required and setup kernel isolation");
        }
    }

    // Create memory servers
    std::vector<std::shared_ptr<MemoryServer>> mem_servers;
    for (size_t i = 0; i < num_ms; ++i) {
        mem_servers.push_back(std::make_shared<MemoryServer>(mem_per_server));
    }
    // Create RDMA manager
    auto rdma_mgr = std::make_shared<RDMAManager>(mem_servers, mem_per_server, base_rtt_ns);

    LOG_INFO("\n====================== KV-index RDMA Stress Test ======================");
    const size_t num_cs = config.num_cs;
    const size_t threads_per_cs = config.threads_per_cs;
    const size_t initial_slabs_per_size = config.initial_slabs_per_size;
    const size_t num_maintenance_cs = config.maintenance_cs;
    const size_t threads_per_maintenance_cs = config.threads_per_maintenance_cs;
    std::vector<std::shared_ptr<ComputeServer>> compute_servers;
    std::vector<std::shared_ptr<ComputeServer>> maintenance_compute_servers;

    // Get required sizes from the configured index type
    std::set<size_t> sizes;
    {
        auto temp_index = make_index(nullptr, nullptr, 0, nullptr);
        sizes = temp_index->get_required_sizes();
    }
    {
        LOG_DEBUG("Required slab sizes for index: ");
        std::ostringstream oss;
        for (auto s : sizes) oss << s << " ";
        LOG_DEBUG(oss.str());
    }

    // Initialize Index
    GlobalAddress root_offset_ptr = 0;
    {
        auto rpc_allocator = std::make_shared<RPCAllocator>(mem_servers);
        auto local_allocator = std::make_shared<LocalAllocator>(sizes, initial_slabs_per_size, rpc_allocator);

        auto init_index = make_index(rdma_mgr, local_allocator, 0, nullptr);
        init_index->initialize(num_maintenance_cs);
        // KVIndex::get_root_offset_pointer() is part of the KVIndex interface — call directly
        root_offset_ptr = init_index->get_root_offset_pointer();
        LOG_INFO("Initialized index with root at global address " << std::hex << root_offset_ptr << std::dec);
    }

    // Maintenance compute servers and queues
    // Create caches per maintenance compute server (shared among threads in the same CS)
    std::vector<std::shared_ptr<IndexCacheBase>> mcs_caches(num_cs);
    
    for (size_t mcs_id = 0; mcs_id < num_cs; ++mcs_id) {
        LOG_INFO("Setting up cache for MCS " << mcs_id);
        // Create cache using the factory method from a sample index
        auto sample_index = make_index(rdma_mgr, nullptr, 0, nullptr);
        // mcs_caches[mcs_id] = sample_index->create_cache(64 * 1024 * 1024); // 64MB
        LOG_INFO("Created cache for MCS " << mcs_id << " targeting level 2");
    }

    LOG_INFO("Launching " << num_maintenance_cs << " maintenance compute servers, " << threads_per_maintenance_cs << " threads each.");
    assert(num_maintenance_cs == 0 || threads_per_maintenance_cs > 0);

    for (size_t mcs_id = 0; mcs_id < num_maintenance_cs; ++mcs_id) {
        auto rpc_allocator = std::make_shared<RPCAllocator>(mem_servers);
        auto local_allocator = std::make_shared<LocalAllocator>(sizes, initial_slabs_per_size, rpc_allocator);
        // TODO: for some reason MCS caches makes stuff REAL slow
        // auto cache = mcs_caches[mcs_id];
        auto cache = nullptr;

        auto maintenance_worker = [mcs_id, root_offset_ptr, cache, &make_index](size_t tid, ThreadStats& stat, std::shared_ptr<RDMAManager> rdma_mgr, std::shared_ptr<LocalAllocator> allocator) {
            auto index = make_index(rdma_mgr, allocator, root_offset_ptr, cache);
            // if the index implementation provides a maintenance_worker override, call it
            index->maintenance_worker(mcs_id, tid);
        };
        
        // CPU binding for maintenance CS: they get cores after the main CS
        std::optional<size_t> mcs_core = std::nullopt;
        if (config.cpu_binding_enabled) {
            mcs_core = config.cpu_binding_start_core + config.num_cs + mcs_id;
        }
        
        maintenance_compute_servers.push_back(std::make_shared<ComputeServer>(mcs_id, threads_per_maintenance_cs, rdma_mgr, local_allocator, maintenance_worker, mcs_core, config.cpu_isolation_required));
    }

    for (auto& mcs : maintenance_compute_servers) mcs->start();

    // Removed redundant atomic counters to eliminate unnecessary thread synchronization:
    // These were causing performance overhead without providing useful functionality
    // since operation counts are already tracked per-thread in ThreadStats
    
    std::vector<std::vector<util::WorkerSummary>> worker_summaries(num_cs, std::vector<util::WorkerSummary>(threads_per_cs));

    util::OperationPicker op_picker(config.operation_mix);

    const std::filesystem::path stats_dir("thread_stats");
    if (std::filesystem::exists(stats_dir)) {
        std::filesystem::remove_all(stats_dir);
    }
    std::filesystem::create_directories(stats_dir);

    const size_t total_clients = num_cs * threads_per_cs;
    const size_t warmup_total = config.warmup_inserts;
    const size_t ops_per_client = config.ops_per_client;

    // Create warmup barrier to synchronize all clients before starting main iterations
    // Custom barrier implementation for C++17 compatibility
    std::atomic<size_t> warmup_counter(0);
    std::mutex warmup_mutex;
    std::condition_variable warmup_cv;

    // Create caches per compute server (shared among threads in the same CS)
    // Cache internal nodes at level 1 (one level above leaves)
    std::vector<std::shared_ptr<IndexCacheBase>> cs_caches(num_cs);
    for (size_t cs_id = 0; cs_id < num_cs; ++cs_id) {
        // Create cache using the factory method from a sample index
        auto sample_index = make_index(rdma_mgr, nullptr, 0, nullptr);
        cs_caches[cs_id] = sample_index->create_cache(config.max_cs_cache_size_kb * 1024);
        LOG_INFO("Created cache for CS " << cs_id << " targeting level 1");
    }

    auto benchmark_start = std::chrono::steady_clock::now();
    for (size_t cs_id = 0; cs_id < num_cs; ++cs_id) {
        auto rpc_allocator = std::make_shared<RPCAllocator>(mem_servers);
        auto local_allocator = std::make_shared<LocalAllocator>(sizes, initial_slabs_per_size, rpc_allocator);
        auto cache = cs_caches[cs_id];
        if (!config.use_cache) {
            cache = nullptr;
        }

	    std::cout << "Using cache? " << (cache != nullptr) << std::endl;
        auto worker = [cs_id, root_offset_ptr, &op_picker, &worker_summaries, &warmup_counter, &warmup_mutex, &warmup_cv, total_clients,
                       ops_per_client, warmup_total, &config, cache, &benchmark_start, &make_index](int tid, ThreadStats& stat,
                                              std::shared_ptr<RDMAManager> rdma_mgr,
                                              std::shared_ptr<LocalAllocator> allocator) {
            auto kv_index = make_index(rdma_mgr, allocator, root_offset_ptr, cache);
            const size_t global_client_id = static_cast<size_t>(cs_id) * config.threads_per_cs + static_cast<size_t>(tid);
            const size_t warmup_per_client = warmup_total / total_clients + (global_client_id < (warmup_total % total_clients) ? 1 : 0);

            std::mt19937 op_rng(std::random_device{}() + tid + cs_id * 997);
            std::mt19937_64 value_rng(std::random_device{}() ^ (static_cast<uint64_t>(global_client_id) << 16));
            auto& summary = worker_summaries[cs_id][tid];

            util::KeySelectionSampler sampler(config.distribution);
            util::LocalKeySet key_set(config.distribution.key_space);
            util::OperationRecorder recorder(stat);
            uint64_t sequence = 0;

            auto perform_insert = [&](const Key& key, const Value& value, bool update_local_state) {
                auto start = std::chrono::high_resolution_clock::now();
                bool ok = kv_index->insert(key, value);
                auto end = std::chrono::high_resolution_clock::now();
                recorder.record_result(OperationKind::Insert, start, end, ok);
                if (ok) {
                    if (update_local_state) {
                        if (key_set.add_new(key)) {
                            sampler.update_active_count(key_set.active_count());
                        } else {
                            LOG_WARN("Client " << global_client_id << " failed to track inserted key due to capacity");
                        }
                    }
                }
                return ok;
            };

            auto perform_read = [&](const Key& key) {
                Value val{};
                auto start = std::chrono::high_resolution_clock::now();
                bool ok = kv_index->read(key, val);
                auto end = std::chrono::high_resolution_clock::now();
                recorder.record_result(OperationKind::Read, start, end, ok);
                return ok;
            };

            auto perform_update = [&](const Key& key) {
                Value new_value = util::generate_random_value(value_rng);
                auto start = std::chrono::high_resolution_clock::now();
                bool ok = kv_index->update(key, new_value);
                auto end = std::chrono::high_resolution_clock::now();
                recorder.record_result(OperationKind::Update, start, end, ok);
                return ok;
            };

            auto perform_delete = [&](size_t slot, const Key& key) {
                auto start = std::chrono::high_resolution_clock::now();
                bool ok = kv_index->del(key);
                auto end = std::chrono::high_resolution_clock::now();
                recorder.record_result(OperationKind::Delete, start, end, ok);
                if (ok) {
                    key_set.deactivate(slot);
                    sampler.update_active_count(key_set.active_count());
                }
                return ok;
            };

            // Warm-up inserts specific to this client
            for (size_t i = 0; i < warmup_per_client; ++i) {
                if (!key_set.can_insert()) break;
                Key key = util::encode_key(global_client_id, sequence++);
                Value value = util::generate_random_value(value_rng);
                bool inserted = perform_insert(key, value, true);
                if (!inserted) {
                    // Stop warmup if insert fails consistently
                    break;
                }
            }

            // reset stats
            recorder.reset();
            sampler.update_active_count(key_set.active_count());

            // Wait for all threads to complete warmup before starting main benchmark
            // Custom barrier implementation for C++17 compatibility
            {
                std::unique_lock<std::mutex> lock(warmup_mutex);
                size_t count = warmup_counter.fetch_add(1) + 1;
                if (count == total_clients) {
                    // Last thread to arrive - notify all waiting threads
                    warmup_cv.notify_all();
                    benchmark_start = std::chrono::steady_clock::now();
                } else {
                    // Wait for all threads to arrive
                    warmup_cv.wait(lock, [&] { return warmup_counter.load() == total_clients; });
                }
            }

            auto select_active_slot = [&]() -> std::optional<size_t> {
                size_t active = key_set.active_count();
                if (active == 0) return std::nullopt;
                sampler.update_active_count(active);
                size_t pick = sampler.sample(op_rng);
                if (pick >= active) pick = active - 1;
                return key_set.slot_from_active_index(pick);
            };

            auto main_start = std::chrono::steady_clock::now();
            for (size_t op_idx = 0; op_idx < ops_per_client; ++op_idx) {
                OperationKind desired = op_picker.weighted_pick(op_rng);
                OperationKind actual = desired;

                auto ensure_valid_operation = [&]() -> bool {
                    if (actual == OperationKind::Insert && !key_set.can_insert()) {
                        if (key_set.has_active()) {
                            actual = OperationKind::Update;
                        } else {
                            return false;
                        }
                    }
                    if (actual != OperationKind::Insert && !key_set.has_active()) {
                        if (key_set.can_insert()) {
                            actual = OperationKind::Insert;
                        } else {
                            return false;
                        }
                    }
                    return true;
                };

                if (!ensure_valid_operation()) {
                    continue;
                }

                bool success = false;
                switch (actual) {
                    case OperationKind::Insert: {
                        Key key = util::encode_key(global_client_id, sequence++);
                        Value value = util::generate_random_value(value_rng);
                        success = perform_insert(key, value, true);
                        break;
                    }
                    case OperationKind::Read: {
                        auto slot_opt = select_active_slot();
                        if (slot_opt) {
                            const Key& key = key_set.key_at_slot(*slot_opt);
                            success = perform_read(key);
                        }
                        break;
                    }
                    case OperationKind::Update: {
                        auto slot_opt = select_active_slot();
                        if (slot_opt) {
                            const Key& key = key_set.key_at_slot(*slot_opt);
                            success = perform_update(key);
                        }
                        break;
                    }
                    case OperationKind::Delete: {
                        auto slot_opt = select_active_slot();
                        if (slot_opt) {
                            const Key& key = key_set.key_at_slot(*slot_opt);
                            success = perform_delete(*slot_opt, key);
                        }
                        break;
                    }
                    case OperationKind::Count:
                        continue;
                }

                summary.attempted++;
                if (success) summary.succeeded++;
            }
            auto main_end = std::chrono::steady_clock::now();
            summary.elapsed_sec = std::chrono::duration_cast<std::chrono::duration<double>>(main_end - main_start).count();
            if (summary.elapsed_sec > 0.0) {
                summary.attempted_throughput = static_cast<double>(summary.attempted) / summary.elapsed_sec;
                summary.succeeded_throughput = static_cast<double>(summary.succeeded) / summary.elapsed_sec;
            }

            auto rdma_stats_snapshot = rdma_mgr->collect_stats();
            LOG_INFO("[ThreadSummary] CS " << cs_id << " thread " << tid
                     << " ops:" << summary.succeeded
                     << " attempted:" << summary.attempted
                     << " throughput_ops_per_sec:" << summary.succeeded_throughput
                     << " RDMA total ns snapshot: " << rdma_stats_snapshot.total_rtt_ns);

            // Finalize cache stats before thread exits
            // if (cache) cache->finalize_thread_stats();
            Profiler::publish_thread_stats();
        };

        // Determine CPU core binding for this CS
        std::optional<size_t> core_id = std::nullopt;
        if (config.cpu_binding_enabled) {
            core_id = config.cpu_binding_start_core + cs_id;
        }

        compute_servers.push_back(std::make_shared<ComputeServer>(cs_id, threads_per_cs, rdma_mgr, local_allocator, worker, core_id, config.cpu_isolation_required));
    }

    for (auto& cs : compute_servers) cs->start();
    for (auto& cs : compute_servers) cs->join();

    // Finalize by creating a dummy index of the configured type and calling finalize
    auto dummy_index = make_index(nullptr, nullptr, 0, nullptr);
    dummy_index->finalize(threads_per_maintenance_cs);

    // Wait for maintenance compute servers to finish as well
    for (auto& mcs : maintenance_compute_servers) mcs->join();

    auto benchmark_end = std::chrono::steady_clock::now();
    double elapsed_seconds = std::chrono::duration_cast<std::chrono::duration<double>>(benchmark_end - benchmark_start).count();
    // Collect global stats
    size_t total_attempted = 0;
    size_t total_succeeded = 0;
    double sum_attempted_throughput = 0.0;
    double sum_succeeded_throughput = 0.0;
    
    // Calculate operation counts from per-thread statistics instead of removed atomic counters
    size_t total_insert_count = 0;
    size_t total_read_count = 0;
    size_t total_update_count = 0;
    size_t total_delete_count = 0;
    
    for (size_t cs_id = 0; cs_id < compute_servers.size(); ++cs_id) {
        const auto& thread_stats_vec = compute_servers[cs_id]->get_thread_stats();
        for (size_t tid = 0; tid < threads_per_cs; ++tid) {
            const auto& summary = worker_summaries[cs_id][tid];
            total_attempted += summary.attempted;
            total_succeeded += summary.succeeded;
            sum_attempted_throughput += summary.attempted_throughput;
            sum_succeeded_throughput += summary.succeeded_throughput;
            
            // Sum operation counts from thread statistics
            const auto& thread_stat = thread_stats_vec[tid];
            total_insert_count += thread_stat.per_op[static_cast<size_t>(OperationKind::Insert)].successes + 
                                  thread_stat.per_op[static_cast<size_t>(OperationKind::Insert)].failures;
            total_read_count += thread_stat.per_op[static_cast<size_t>(OperationKind::Read)].successes + 
                                thread_stat.per_op[static_cast<size_t>(OperationKind::Read)].failures;
            total_update_count += thread_stat.per_op[static_cast<size_t>(OperationKind::Update)].successes + 
                                  thread_stat.per_op[static_cast<size_t>(OperationKind::Update)].failures;
            total_delete_count += thread_stat.per_op[static_cast<size_t>(OperationKind::Delete)].successes + 
                                  thread_stat.per_op[static_cast<size_t>(OperationKind::Delete)].failures;
        }
    }

    Profiler::publish_thread_stats();
    auto profiler_sections = Profiler::snapshot();

    auto rdma_stats = rdma_mgr->collect_stats();
    auto total_rtt_ms = static_cast<double>(rdma_stats.total_rtt_ns) / 1'000'000.0;
    std::cout << "\n[KVIndexRDMA] Stress test complete." << std::endl;
    std::cout << "Total operations attempted: " << total_attempted << " succeeded: " << total_succeeded << std::endl;
    if (elapsed_seconds > 0.0) {
        double attempted_throughput = static_cast<double>(total_attempted) / elapsed_seconds;
        double succeeded_throughput = static_cast<double>(total_succeeded) / elapsed_seconds;
      std::cout << std::fixed << std::setprecision(3)
            << "Elapsed workload time (s): " << elapsed_seconds
            << " | Attempted throughput (ops/s): " << attempted_throughput
            << " | Succeeded throughput (ops/s): " << succeeded_throughput << std::endl
            << std::defaultfloat;
    }
    std::cout << std::fixed << std::setprecision(3)
          << "Sum per-client attempted throughput (ops/s): " << sum_attempted_throughput
          << " | Sum per-client succeeded throughput (ops/s): " << sum_succeeded_throughput << std::endl
          << std::defaultfloat;
    std::cout << "Operation counts -> inserts: " << total_insert_count << ", reads: " << total_read_count
              << ", updates: " << total_update_count << ", deletes: " << total_delete_count << std::endl;
    std::cout << "RDMA ops by type:";
    for (size_t i = 0; i < rdma_stats.op_counts.size(); ++i) {
        std::cout << " " << rdma_stats.op_counts[i];
    }
    std::cout << " total RTT(ms): " << total_rtt_ms << std::endl;

    // Collect and display cache statistics
    size_t total_cache_hits = 0;
    size_t total_cache_misses = 0;
    size_t total_cache_entries = 0;
    size_t total_cache_evictions = 0;
    size_t total_invalid_ranges = 0;
    
    // TODO: are the caches stats global for all CSs?
    // for (size_t cs_id = 0; cs_id < num_cs; ++cs_id) {
    //     auto cache_stats = cs_caches[cs_id]->get_stats();
    //     total_cache_hits += cache_stats.hits;
    //     total_cache_misses += cache_stats.misses;
    //     total_cache_entries += cache_stats.entries;
    //     total_cache_evictions += cache_stats.evictions;
    //     total_invalid_ranges += cache_stats.invalid_ranges;
        
    //     std::cout << "CS " << cs_id << " cache: hits=" << cache_stats.hits 
    //               << ", misses=" << cache_stats.misses 
    //               << ", hit_rate=" << std::fixed << std::setprecision(3) << cache_stats.hit_rate()
    //               << ", entries=" << cache_stats.entries
    //               << ", evictions=" << cache_stats.evictions 
    //               << ", invalid_ranges=" << cache_stats.invalid_ranges << std::endl;
    // }
    
    // for (size_t mcs_id = 0; mcs_id < num_maintenance_cs; ++mcs_id) {
    //     auto cache_stats = mcs_caches[mcs_id]->get_stats();
    //     total_cache_hits += cache_stats.hits;
    //     total_cache_misses += cache_stats.misses;
    //     total_cache_entries += cache_stats.entries;
    //     total_cache_evictions += cache_stats.evictions;
    //     total_invalid_ranges += cache_stats.invalid_ranges;
        
    //     std::cout << "MCS " << mcs_id << " cache: hits=" << cache_stats.hits 
    //               << ", misses=" << cache_stats.misses 
    //               << ", hit_rate=" << std::fixed << std::setprecision(3) << cache_stats.hit_rate()
    //               << ", entries=" << cache_stats.entries
    //               << ", evictions=" << cache_stats.evictions 
    //               << ", invalid_ranges=" << cache_stats.invalid_ranges << std::endl;
    // }

    double overall_hit_rate = (total_cache_hits + total_cache_misses) == 0 ? 0.0 : 
        static_cast<double>(total_cache_hits) / static_cast<double>(total_cache_hits + total_cache_misses);
    std::cout << "Total cache: hits=" << total_cache_hits 
              << ", misses=" << total_cache_misses 
              << ", hit_rate=" << std::fixed << std::setprecision(3) << overall_hit_rate
              << ", entries=" << total_cache_entries
              << ", evictions=" << total_cache_evictions 
              << ", invalid_ranges=" << total_invalid_ranges << std::endl << std::defaultfloat;

    if (!profiler_sections.empty()) {
        std::vector<std::pair<std::string, ProfileStats>> sorted_sections(profiler_sections.begin(), profiler_sections.end());
        std::sort(sorted_sections.begin(), sorted_sections.end(), [](const auto& a, const auto& b) {
            return a.second.total_ns > b.second.total_ns;
        });

        std::cout << "\n[Profiler] Top sections:" << std::endl;
        size_t display_count = std::min<size_t>(50, sorted_sections.size());
        for (size_t i = 0; i < display_count; ++i) {
            const auto& [name, stats] = sorted_sections[i];
            double avg_ns = stats.count == 0 ? 0.0 : static_cast<double>(stats.total_ns) / static_cast<double>(stats.count);
            std::cout << "  - " << name << ": count=" << stats.count
                      << ", total_ms=" << (static_cast<double>(stats.total_ns) / 1'000'000.0)
                      << ", avg_ns=" << avg_ns << std::endl;
        }
    }

    // Optionally print per-thread latency stats
    // Using util::operation_name instead of local lambda

    std::array<util::AggregatedOpStats, static_cast<size_t>(OperationKind::Count)> op_totals{};
    
    // Collect all latency samples for percentile calculation
    std::array<std::vector<double>, static_cast<size_t>(OperationKind::Count)> all_samples;

    for (size_t cs_id = 0; cs_id < compute_servers.size(); ++cs_id) {
        for (size_t tid = 0; tid < threads_per_cs; ++tid) {
            const auto& thread_stat = compute_servers[cs_id]->get_thread_stats()[tid];
            const auto& summary = worker_summaries[cs_id][tid];
            std::cout << "CS " << cs_id << " Thread " << tid << ":";
            for (size_t op_idx = 0; op_idx < static_cast<size_t>(OperationKind::Count); ++op_idx) {
                const auto& entry = thread_stat.per_op[op_idx];
                if (entry.successes == 0 && entry.failures == 0) continue;
                std::cout << " op" << op_idx << " success=" << entry.successes << "/" << entry.successes + entry.failures;
                if (entry.successes > 0) {
                    double avg = entry.total_latency_us / entry.successes;
                    std::cout << " avg_us=" << avg << " min_us=" << entry.min_latency_us << " max_us=" << entry.max_latency_us;
                }
            }
            std::cout << std::endl;

            std::filesystem::path json_path = stats_dir / ("cs_" + std::to_string(cs_id) + "_thread_" + std::to_string(tid) + ".json");
            std::ofstream json_out(json_path);
            if (!json_out) {
                LOG_ERROR("Failed to open JSON stats file " << json_path);
                continue;
            }
            json_out << std::fixed << std::setprecision(3);
            json_out << "{\n";
            json_out << "  \"compute_server\": " << cs_id << ",\n";
            json_out << "  \"thread\": " << tid << ",\n";
            json_out << "  \"attempted\": " << summary.attempted << ",\n";
            json_out << "  \"succeeded\": " << summary.succeeded << ",\n";
            json_out << "  \"elapsed_sec\": " << summary.elapsed_sec << ",\n";
            json_out << "  \"attempted_ops_per_sec\": " << summary.attempted_throughput << ",\n";
            json_out << "  \"succeeded_ops_per_sec\": " << summary.succeeded_throughput << ",\n";
            json_out << "  \"operations\": {\n";
            for (size_t op_idx = 0; op_idx < static_cast<size_t>(OperationKind::Count); ++op_idx) {
                const auto& entry = thread_stat.per_op[op_idx];
                json_out << "    \"" << util::operation_name(op_idx) << "\": {\n";
                json_out << "      \"successes\": " << entry.successes << ",\n";
                json_out << "      \"failures\": " << entry.failures << ",\n";
                json_out << "      \"avg_latency_us\": ";
                if (entry.successes > 0) {
                    json_out << (entry.total_latency_us / entry.successes);
                } else {
                    json_out << "null";
                }
                json_out << ",\n";
                json_out << "      \"min_latency_us\": ";
                if (entry.successes > 0) {
                    json_out << entry.min_latency_us;
                } else {
                    json_out << "null";
                }
                json_out << ",\n";
                json_out << "      \"max_latency_us\": ";
                if (entry.successes > 0) {
                    json_out << entry.max_latency_us;
                } else {
                    json_out << "null";
                }
                json_out << ",\n";
                json_out << "      \"total_latency_us\": " << entry.total_latency_us << ",\n";
                
                // Calculate percentiles for this thread
                auto thread_samples = entry.latency_samples.get_samples();
                if (!thread_samples.empty()) {
                    auto [p50, p95, p99] = util::PercentileCalculator::calculate_standard(thread_samples);
                    json_out << "      \"p50_latency_us\": " << p50 << ",\n";
                    json_out << "      \"p95_latency_us\": " << p95 << ",\n";
                    json_out << "      \"p99_latency_us\": " << p99 << ",\n";
                } else {
                    json_out << "      \"p50_latency_us\": null,\n";
                    json_out << "      \"p95_latency_us\": null,\n";
                    json_out << "      \"p99_latency_us\": null,\n";
                }
                json_out << "      \"sample_count\": " << entry.latency_samples.sample_count() << "\n";
                json_out << "    }";
                if (op_idx + 1 < static_cast<size_t>(OperationKind::Count)) json_out << ",";
                json_out << "\n";

                auto& agg_entry = op_totals[op_idx];
                agg_entry.successes += entry.successes;
                agg_entry.failures += entry.failures;
                agg_entry.total_latency_us += entry.total_latency_us;
                if (entry.successes > 0) {
                    agg_entry.min_latency_us = std::min(agg_entry.min_latency_us, entry.min_latency_us);
                    agg_entry.max_latency_us = std::max(agg_entry.max_latency_us, entry.max_latency_us);
                }
                
                // Collect latency samples for percentile calculation
                auto samples = entry.latency_samples.get_samples();
                all_samples[op_idx].insert(all_samples[op_idx].end(), samples.begin(), samples.end());
            }
            json_out << "  }\n";
            json_out << "}\n";
        }
    }

    // Calculate percentiles for all operations using collected samples
    for (size_t op_idx = 0; op_idx < static_cast<size_t>(OperationKind::Count); ++op_idx) {
        auto& agg_entry = op_totals[op_idx];
        auto& samples = all_samples[op_idx];
        
        if (!samples.empty()) {
            auto [p50, p95, p99] = util::PercentileCalculator::calculate_standard(samples);
            agg_entry.p50_latency_us = p50;
            agg_entry.p95_latency_us = p95;
            agg_entry.p99_latency_us = p99;
            agg_entry.sample_count = samples.size();
        }
    }

    std::ofstream summary_out(stats_dir / "summary.json");
    if (summary_out) {
        summary_out << std::fixed << std::setprecision(3);
        summary_out << "{\n";
        summary_out << "  \"total_attempted\": " << total_attempted << ",\n";
        summary_out << "  \"total_succeeded\": " << total_succeeded << ",\n";
        summary_out << "  \"elapsed_sec\": " << elapsed_seconds << ",\n";
        summary_out << "  \"operation_counts\": {\n";
        summary_out << "    \"insert\": " << total_insert_count << ",\n";
        summary_out << "    \"read\": " << total_read_count << ",\n";
        summary_out << "    \"update\": " << total_update_count << ",\n";
        summary_out << "    \"delete\": " << total_delete_count << "\n";
        summary_out << "  },\n";
        summary_out << "  \"profiling\": {\n";
        size_t prof_idx = 0;
        for (const auto& [name, stats] : profiler_sections) {
            summary_out << "    \"" << name << "\": {\n";
            summary_out << "      \"count\": " << stats.count << ",\n";
            summary_out << "      \"total_ns\": " << stats.total_ns << ",\n";
            double avg_ns = stats.count == 0 ? 0.0 : static_cast<double>(stats.total_ns) / static_cast<double>(stats.count);
            summary_out << "      \"avg_ns\": " << avg_ns << "\n";
            summary_out << "    }";
            if (++prof_idx < profiler_sections.size()) summary_out << ",";
            summary_out << "\n";
        }
        summary_out << "  },\n";
    summary_out << "  \"per_client_throughput\": {\n";
    summary_out << "    \"sum_attempted_ops_per_sec\": " << sum_attempted_throughput << ",\n";
    summary_out << "    \"sum_succeeded_ops_per_sec\": " << sum_succeeded_throughput << "\n";
    summary_out << "  },\n";
        summary_out << "  \"per_operation_latency\": {\n";
        for (size_t op_idx = 0; op_idx < static_cast<size_t>(OperationKind::Count); ++op_idx) {
            const auto& agg = op_totals[op_idx];
            summary_out << "    \"" << util::operation_name(op_idx) << "\": {\n";
            summary_out << "      \"successes\": " << agg.successes << ",\n";
            summary_out << "      \"failures\": " << agg.failures << ",\n";
            summary_out << "      \"avg_latency_us\": ";
            if (agg.successes > 0) {
                summary_out << (agg.total_latency_us / agg.successes);
            } else {
                summary_out << "null";
            }
            summary_out << ",\n";
            summary_out << "      \"min_latency_us\": ";
            if (agg.successes > 0) {
                summary_out << agg.min_latency_us;
            } else {
                summary_out << "null";
            }
            summary_out << ",\n";
            summary_out << "      \"max_latency_us\": ";
            if (agg.successes > 0) {
                summary_out << agg.max_latency_us;
            } else {
                summary_out << "null";
            }
            summary_out << ",\n";
            summary_out << "      \"p50_latency_us\": ";
            if (agg.sample_count > 0) {
                summary_out << agg.p50_latency_us;
            } else {
                summary_out << "null";
            }
            summary_out << ",\n";
            summary_out << "      \"p95_latency_us\": ";
            if (agg.sample_count > 0) {
                summary_out << agg.p95_latency_us;
            } else {
                summary_out << "null";
            }
            summary_out << ",\n";
            summary_out << "      \"p99_latency_us\": ";
            if (agg.sample_count > 0) {
                summary_out << agg.p99_latency_us;
            } else {
                summary_out << "null";
            }
            summary_out << ",\n";
            summary_out << "      \"sample_count\": " << agg.sample_count << "\n";
            summary_out << "    }";
            if (op_idx + 1 < static_cast<size_t>(OperationKind::Count)) summary_out << ",";
            summary_out << "\n";
        }
        summary_out << "  },\n";
        summary_out << "  \"rdma\": {\n";
        summary_out << "    \"total_rtt_ms\": " << total_rtt_ms << ",\n";
        summary_out << "    \"op_counts\": [";
        for (size_t i = 0; i < rdma_stats.op_counts.size(); ++i) {
            summary_out << rdma_stats.op_counts[i];
            if (i + 1 < rdma_stats.op_counts.size()) summary_out << ", ";
        }
        summary_out << "]\n";
        summary_out << "  },\n";
        summary_out << "  \"throughput\": {\n";
        double attempted_throughput = elapsed_seconds > 0.0 ? static_cast<double>(total_attempted) / elapsed_seconds : 0.0;
        double succeeded_throughput = elapsed_seconds > 0.0 ? static_cast<double>(total_succeeded) / elapsed_seconds : 0.0;
        summary_out << "    \"window_sec\": " << elapsed_seconds << ",\n";
        summary_out << "    \"attempted_ops_per_sec\": " << attempted_throughput << ",\n";
        summary_out << "    \"succeeded_ops_per_sec\": " << succeeded_throughput << ",\n";
        summary_out << "    \"per_operation\": {\n";
        auto write_throughput_entry = [&](const char* name, size_t attempted_count, size_t success_count, bool is_last) {
            double attempted_rate = elapsed_seconds > 0.0 ? static_cast<double>(attempted_count) / elapsed_seconds : 0.0;
            double success_rate = elapsed_seconds > 0.0 ? static_cast<double>(success_count) / elapsed_seconds : 0.0;
            summary_out << "      \"" << name << "\": {\n";
            summary_out << "        \"attempted_ops_per_sec\": " << attempted_rate << ",\n";
            summary_out << "        \"succeeded_ops_per_sec\": " << success_rate << "\n";
            summary_out << "      }";
            if (!is_last) summary_out << ",";
            summary_out << "\n";
        };
        write_throughput_entry("insert", total_insert_count, op_totals[static_cast<size_t>(OperationKind::Insert)].successes, false);
        write_throughput_entry("read", total_read_count, op_totals[static_cast<size_t>(OperationKind::Read)].successes, false);
        write_throughput_entry("update", total_update_count, op_totals[static_cast<size_t>(OperationKind::Update)].successes, false);
        write_throughput_entry("delete", total_delete_count, op_totals[static_cast<size_t>(OperationKind::Delete)].successes, true);
        summary_out << "    }\n";
        summary_out << "  },\n";
        summary_out << "  \"cache\": {\n";
        summary_out << "    \"total_hits\": " << total_cache_hits << ",\n";
        summary_out << "    \"total_misses\": " << total_cache_misses << ",\n";
        summary_out << "    \"total_hit_rate\": " << overall_hit_rate << ",\n";
        summary_out << "    \"total_entries\": " << total_cache_entries << ",\n";
        summary_out << "    \"total_evictions\": " << total_cache_evictions << ",\n";
        summary_out << "    \"total_invalid_ranges\": " << total_invalid_ranges << ",\n";
        summary_out << "    \"per_cs\": [\n";
        // for (size_t cs_id = 0; cs_id < num_cs; ++cs_id) {
        //     auto cache_stats = cs_caches[cs_id]->get_stats();
        //     summary_out << "      {\n";
        //     summary_out << "        \"cs_id\": " << cs_id << ",\n";
        //     summary_out << "        \"hits\": " << cache_stats.hits << ",\n";
        //     summary_out << "        \"misses\": " << cache_stats.misses << ",\n";
        //     summary_out << "        \"hit_rate\": " << cache_stats.hit_rate() << ",\n";
        //     summary_out << "        \"entries\": " << cache_stats.entries << ",\n";
        //     summary_out << "        \"evictions\": " << cache_stats.evictions << ",\n";
        //     summary_out << "        \"invalid_ranges\": " << cache_stats.invalid_ranges << "\n";
        //     summary_out << "      }";
        //     if (cs_id + 1 < num_cs) summary_out << ",";
        //     summary_out << "\n";
        // }
        summary_out << "    ]\n";
        summary_out << "  }\n";
        summary_out << "}\n";
    } else {
        LOG_ERROR("Failed to write aggregated summary JSON to " << (stats_dir / "summary.json"));
    }

    if (!profiler_sections.empty()) {
        std::ofstream profiling_out(stats_dir / "profiling.json");
        if (profiling_out) {
            profiling_out << std::fixed << std::setprecision(3);
            profiling_out << "{\n";
            profiling_out << "  \"sections\": [\n";
            size_t idx = 0;
            std::vector<std::pair<std::string, ProfileStats>> sorted_sections(profiler_sections.begin(), profiler_sections.end());
            std::sort(sorted_sections.begin(), sorted_sections.end(), [](const auto& a, const auto& b) {
                return a.first < b.first;
            });
            for (const auto& [name, stats] : sorted_sections) {
                double avg_ns = stats.count == 0 ? 0.0 : static_cast<double>(stats.total_ns) / static_cast<double>(stats.count);
                profiling_out << "    {\n";
                profiling_out << "      \"name\": \"" << name << "\",\n";
                profiling_out << "      \"count\": " << stats.count << ",\n";
                profiling_out << "      \"total_ns\": " << stats.total_ns << ",\n";
                profiling_out << "      \"total_ms\": " << (static_cast<double>(stats.total_ns) / 1'000'000.0) << ",\n";
                profiling_out << "      \"avg_ns\": " << avg_ns << "\n";
                profiling_out << "    }";
                if (++idx < sorted_sections.size()) profiling_out << ",";
                profiling_out << "\n";
            }
            profiling_out << "  ]\n";
            profiling_out << "}\n";
        } else {
            LOG_ERROR("Failed to write profiling JSON to " << (stats_dir / "profiling.json"));
        }
    }

    // At program end, close thread log
    thread_log::close_thread_log();
    return 0;
}
