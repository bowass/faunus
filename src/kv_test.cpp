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
#include "../rdma/rdma_manager.hpp"
#include "../rdma/memory_server.hpp"
#include "../rdma/rpc_allocator.hpp"
#include "../rdma/compute_server.hpp"
#include "../config/faunus_config.hpp"
#include "../kv_index/faunus_index.hpp"
#include "../kv_index/index_cache.hpp"
#include "../util/profiler.hpp"
#include "../util/distribution.hpp"
#include "../util/key_management.hpp"
#include "../util/benchmark.hpp"

// Using utility classes from dedicated headers

int main(int argc, char* argv[]) {
    // Setup thread log directory (delete previous logs)
    faunus_log::setup_log_dir();

    std::string config_path = "faunus_config.yaml";
    if (argc > 1) config_path = argv[1];
    // Load configuration from YAML file
    FaunusConfig config = load_faunus_config(config_path);
    const size_t num_ms = config.num_ms;
    const size_t mem_per_server = config.mem_per_ms;
    const double base_rtt_us = config.base_rtt_us;

    set_log_level(config.log_level);
    faunus_log::setup_thread_log();
    Profiler::reset();

    // Create memory servers
    std::vector<std::shared_ptr<MemoryServer>> mem_servers;
    for (size_t i = 0; i < num_ms; ++i) {
        mem_servers.push_back(std::make_shared<MemoryServer>(mem_per_server));
    }
    // Create RDMA manager
    auto rdma_mgr = std::make_shared<RDMAManager>(mem_servers, mem_per_server, base_rtt_us);

    LOG_INFO("\n====================== KV-index RDMA Stress Test ======================");
    const size_t num_cs = config.num_cs;
    const size_t threads_per_cs = config.threads_per_cs;
    const size_t initial_slabs_per_size = config.initial_slabs_per_size;
    const size_t num_maintenance_cs = config.maintenance_cs;
    const size_t threads_per_maintenance_cs = config.threads_per_maintenance_cs;
    std::vector<std::shared_ptr<ComputeServer>> compute_servers;
    std::vector<std::shared_ptr<ComputeServer>> maintenance_compute_servers;

    std::set<size_t> sizes = FaunusIndex::get_required_sizes();
    {
        LOG_DEBUG("Required slab sizes for FaunusIndex: ");
        std::ostringstream oss;
        for (auto s : sizes) oss << s << " ";
        LOG_DEBUG(oss.str());
    }

    // Initialize Index
    GlobalAddress root_offset_ptr = 0;
    {
        auto rpc_allocator = std::make_shared<RPCAllocator>(mem_servers);
        auto local_allocator = std::make_shared<LocalAllocator>(sizes, initial_slabs_per_size, rpc_allocator);

        FaunusIndex init_index(rdma_mgr, local_allocator, 0, nullptr);
        init_index.initialize();
        root_offset_ptr = init_index.get_root_offset_pointer();
        LOG_INFO("Initialized FaunusIndex with root at global address " << std::hex << root_offset_ptr << std::dec);
    }

    // Maintenance compute servers and queues
    LOG_INFO("Launching " << num_maintenance_cs << " maintenance compute servers, " << threads_per_maintenance_cs << " threads each.");
    assert(num_maintenance_cs == 0 || threads_per_maintenance_cs > 0);
    for (size_t mcs_id = 0; mcs_id < num_maintenance_cs; ++mcs_id) {
        auto rpc_allocator = std::make_shared<RPCAllocator>(mem_servers);
        auto local_allocator = std::make_shared<LocalAllocator>(sizes, initial_slabs_per_size, rpc_allocator);

        auto maintenance_worker = [mcs_id, root_offset_ptr](size_t tid, ThreadStats& stat, std::shared_ptr<RDMAManager> rdma_mgr, std::shared_ptr<LocalAllocator> allocator) {
            // Maintenance workers don't need cache since they work on different data
            FaunusIndex index(rdma_mgr, allocator, root_offset_ptr, nullptr);
            index.maintenance_worker(mcs_id, tid);
        };
        maintenance_compute_servers.push_back(std::make_shared<ComputeServer>(mcs_id, threads_per_maintenance_cs, rdma_mgr, local_allocator, maintenance_worker));
    }

    // Use queued-sets to prevent duplicate maintenance requests (more efficient)
    if (num_maintenance_cs > 0) {
        FaunusIndex::set_maintenance_queued_sets(FaunusIndex::create_maintenance_queued_sets(num_maintenance_cs));
        // FaunusIndex::set_maintenance_queues(FaunusIndex::create_maintenance_queues(num_maintenance_cs));
        LOG_INFO("Using maintenance queued-sets (prevents duplicate SMO requests) for " << num_maintenance_cs << " maintenance servers");
    }
    for (auto& mcs : maintenance_compute_servers) mcs->start();
    
    std::atomic<size_t> insert_count{0};
    std::atomic<size_t> read_count{0};
    std::atomic<size_t> update_count{0};
    std::atomic<size_t> delete_count{0};

    std::vector<std::vector<faunus_util::WorkerSummary>> worker_summaries(num_cs, std::vector<faunus_util::WorkerSummary>(threads_per_cs));

    faunus_util::OperationPicker op_picker(config.operation_mix);

    const std::filesystem::path stats_dir("thread_stats");
    if (std::filesystem::exists(stats_dir)) {
        std::filesystem::remove_all(stats_dir);
    }
    std::filesystem::create_directories(stats_dir);

    const size_t total_clients = num_cs * threads_per_cs;
    const size_t warmup_total = config.warmup_inserts;
    const size_t ops_per_client = config.ops_per_client;

    // Create caches per compute server (shared among threads in the same CS)
    // Cache internal nodes at level 1 (one level above leaves)
    std::vector<std::shared_ptr<faunus_index_internal::IndexCache>> cs_caches(num_cs);
    for (size_t cs_id = 0; cs_id < num_cs; ++cs_id) {
        cs_caches[cs_id] = std::make_shared<faunus_index_internal::IndexCache>(32768, 1); // 1024 entries, level 1
        LOG_INFO("Created cache for CS " << cs_id << " targeting level 1");
    }

    for (size_t cs_id = 0; cs_id < num_cs; ++cs_id) {
        auto rpc_allocator = std::make_shared<RPCAllocator>(mem_servers);
        auto local_allocator = std::make_shared<LocalAllocator>(sizes, initial_slabs_per_size, rpc_allocator);
        auto cache = cs_caches[cs_id];

        auto worker = [cs_id, root_offset_ptr, &op_picker, &worker_summaries, &insert_count,
                       &read_count, &update_count, &delete_count, ops_per_client, warmup_total,
                       total_clients, &config, cache](int tid, ThreadStats& stat,
                                              std::shared_ptr<RDMAManager> rdma_mgr,
                                              std::shared_ptr<LocalAllocator> allocator) {
            auto kv_index = FaunusIndex(rdma_mgr, allocator, root_offset_ptr, cache);
            const size_t global_client_id = static_cast<size_t>(cs_id) * config.threads_per_cs + static_cast<size_t>(tid);
            const size_t warmup_per_client = warmup_total / total_clients + (global_client_id < (warmup_total % total_clients) ? 1 : 0);

            std::mt19937 op_rng(std::random_device{}() + tid + cs_id * 997);
            std::mt19937_64 value_rng(std::random_device{}() ^ (static_cast<uint64_t>(global_client_id) << 16));
            auto& summary = worker_summaries[cs_id][tid];

            faunus_util::KeySelectionSampler sampler(config.distribution);
            faunus_util::LocalKeySet key_set(config.distribution.key_space);
            faunus_util::OperationRecorder recorder(stat);
            uint64_t sequence = 0;

            auto perform_insert = [&](const Key& key, const Value& value, bool update_local_state) {
                auto start = std::chrono::high_resolution_clock::now();
                bool ok = kv_index.insert(key, value);
                auto end = std::chrono::high_resolution_clock::now();
                recorder.record_result(OperationKind::Insert, start, end, ok);
                if (ok) {
                    insert_count.fetch_add(1, std::memory_order_relaxed);
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
                bool ok = kv_index.read(key, val);
                auto end = std::chrono::high_resolution_clock::now();
                recorder.record_result(OperationKind::Read, start, end, ok);
                read_count.fetch_add(1, std::memory_order_relaxed);
                return ok;
            };

            auto perform_update = [&](const Key& key) {
                Value new_value = faunus_util::generate_random_value(value_rng);
                auto start = std::chrono::high_resolution_clock::now();
                bool ok = kv_index.update(key, new_value);
                auto end = std::chrono::high_resolution_clock::now();
                recorder.record_result(OperationKind::Update, start, end, ok);
                update_count.fetch_add(1, std::memory_order_relaxed);
                return ok;
            };

            auto perform_delete = [&](size_t slot, const Key& key) {
                auto start = std::chrono::high_resolution_clock::now();
                bool ok = kv_index.del(key);
                auto end = std::chrono::high_resolution_clock::now();
                recorder.record_result(OperationKind::Delete, start, end, ok);
                if (ok) {
                    delete_count.fetch_add(1, std::memory_order_relaxed);
                    key_set.deactivate(slot);
                    sampler.update_active_count(key_set.active_count());
                }
                return ok;
            };

            // Warm-up inserts specific to this client
            for (size_t i = 0; i < warmup_per_client; ++i) {
                if (!key_set.can_insert()) break;
                Key key = faunus_util::encode_key(global_client_id, sequence++);
                Value value = faunus_util::generate_random_value(value_rng);
                bool inserted = perform_insert(key, value, true);
                if (!inserted) {
                    // Stop warmup if insert fails consistently
                    break;
                }
            }

            sampler.update_active_count(key_set.active_count());

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
                        Key key = faunus_util::encode_key(global_client_id, sequence++);
                        Value value = faunus_util::generate_random_value(value_rng);
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
            faunus_index_internal::IndexCache::finalize_thread_stats();
            Profiler::publish_thread_stats();
        };

        compute_servers.push_back(std::make_shared<ComputeServer>(cs_id, threads_per_cs, rdma_mgr, local_allocator, worker));
    }

    auto benchmark_start = std::chrono::steady_clock::now();
    for (auto& cs : compute_servers) cs->start();
    for (auto& cs : compute_servers) cs->join();
    auto benchmark_end = std::chrono::steady_clock::now();
    double elapsed_seconds = std::chrono::duration_cast<std::chrono::duration<double>>(benchmark_end - benchmark_start).count();
    // Collect global stats
    size_t total_attempted = 0;
    size_t total_succeeded = 0;
    double sum_attempted_throughput = 0.0;
    double sum_succeeded_throughput = 0.0;
    for (const auto& per_cs : worker_summaries) {
        for (const auto& summary : per_cs) {
            total_attempted += summary.attempted;
            total_succeeded += summary.succeeded;
            sum_attempted_throughput += summary.attempted_throughput;
            sum_succeeded_throughput += summary.succeeded_throughput;
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
    std::cout << "Operation counts -> inserts: " << insert_count.load() << ", reads: " << read_count.load()
              << ", updates: " << update_count.load() << ", deletes: " << delete_count.load() << std::endl;
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
    
    for (size_t cs_id = 0; cs_id < num_cs; ++cs_id) {
        auto cache_stats = cs_caches[cs_id]->get_stats();
        total_cache_hits += cache_stats.hits;
        total_cache_misses += cache_stats.misses;
        total_cache_entries += cache_stats.entries;
        total_cache_evictions += cache_stats.evictions;
        total_invalid_ranges += cache_stats.invalid_ranges;
        
        std::cout << "CS " << cs_id << " cache: hits=" << cache_stats.hits 
                  << ", misses=" << cache_stats.misses 
                  << ", hit_rate=" << std::fixed << std::setprecision(3) << cache_stats.hit_rate()
                  << ", entries=" << cache_stats.entries
                  << ", evictions=" << cache_stats.evictions 
                  << ", invalid_ranges=" << cache_stats.invalid_ranges << std::endl;
    }
    
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
        size_t display_count = std::min<size_t>(20, sorted_sections.size());
        for (size_t i = 0; i < display_count; ++i) {
            const auto& [name, stats] = sorted_sections[i];
            double avg_ns = stats.count == 0 ? 0.0 : static_cast<double>(stats.total_ns) / static_cast<double>(stats.count);
            std::cout << "  - " << name << ": count=" << stats.count
                      << ", total_ms=" << (static_cast<double>(stats.total_ns) / 1'000'000.0)
                      << ", avg_ns=" << avg_ns << std::endl;
        }
    }

    // Optionally print per-thread latency stats
    // Using faunus_util::operation_name instead of local lambda

    std::array<faunus_util::AggregatedOpStats, static_cast<size_t>(OperationKind::Count)> op_totals{};

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
                json_out << "    \"" << faunus_util::operation_name(op_idx) << "\": {\n";
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
                json_out << "      \"total_latency_us\": " << entry.total_latency_us << "\n";
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
            }
            json_out << "  }\n";
            json_out << "}\n";
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
        summary_out << "    \"insert\": " << insert_count.load() << ",\n";
        summary_out << "    \"read\": " << read_count.load() << ",\n";
        summary_out << "    \"update\": " << update_count.load() << ",\n";
        summary_out << "    \"delete\": " << delete_count.load() << "\n";
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
            summary_out << "    \"" << faunus_util::operation_name(op_idx) << "\": {\n";
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
            summary_out << "\n    }";
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
        write_throughput_entry("insert", insert_count.load(), op_totals[static_cast<size_t>(OperationKind::Insert)].successes, false);
        write_throughput_entry("read", read_count.load(), op_totals[static_cast<size_t>(OperationKind::Read)].successes, false);
        write_throughput_entry("update", update_count.load(), op_totals[static_cast<size_t>(OperationKind::Update)].successes, false);
        write_throughput_entry("delete", delete_count.load(), op_totals[static_cast<size_t>(OperationKind::Delete)].successes, true);
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
        for (size_t cs_id = 0; cs_id < num_cs; ++cs_id) {
            auto cache_stats = cs_caches[cs_id]->get_stats();
            summary_out << "      {\n";
            summary_out << "        \"cs_id\": " << cs_id << ",\n";
            summary_out << "        \"hits\": " << cache_stats.hits << ",\n";
            summary_out << "        \"misses\": " << cache_stats.misses << ",\n";
            summary_out << "        \"hit_rate\": " << cache_stats.hit_rate() << ",\n";
            summary_out << "        \"entries\": " << cache_stats.entries << ",\n";
            summary_out << "        \"evictions\": " << cache_stats.evictions << ",\n";
            summary_out << "        \"invalid_ranges\": " << cache_stats.invalid_ranges << "\n";
            summary_out << "      }";
            if (cs_id + 1 < num_cs) summary_out << ",";
            summary_out << "\n";
        }
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

    if (num_maintenance_cs > 0 && threads_per_maintenance_cs > 0) {
        FaunusIndex::stop_maintenance(threads_per_maintenance_cs);
    }
    for (auto& mcs : maintenance_compute_servers) mcs->join();

    // At program end, close thread log
    faunus_log::close_thread_log();
    return 0;
}
