#pragma once
#include <string>
#include <array>
#include <map>
#include <algorithm>
#include <cctype>
#include <yaml-cpp/yaml.h>

#include "../util/logging.hpp"


struct DistributionConfig {
    enum class Type { Uniform, Skewed };

    Type type = Type::Uniform;
    size_t key_space = 1'000'000; // default logical key space per client
    double zipf = 0.99;           // skew factor when type == Skewed

    static Type parse_type(const std::string& raw) {
        std::string lower = raw;
        std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
        });
        if (lower == "uniform") return Type::Uniform;
        if (lower == "skewed") return Type::Skewed;
        return Type::Uniform;
    }
    static const char* to_string(Type type) {
        switch (type) {
            case Type::Uniform: return "uniform";
            case Type::Skewed: return "skewed";
        }
        return "uniform";
    }
};

struct IndexConfig {
    size_t num_cs = 8;
    size_t threads_per_cs = 8;
    size_t num_ms = 8;
    size_t mem_per_ms = 512 * 1024;
    size_t base_rtt_ns = 3000;
    size_t key_size = 8;
    size_t value_size = 8;
    size_t kv_per_thread = 1000;
    size_t initial_slabs_per_size = 1024;
    thread_log::LogLevel log_level = thread_log::LOG_INFO;
    // CPU affinity configuration
    bool cpu_binding_enabled = false;
    size_t cpu_binding_start_core = 0;  // First core to use for CS binding
    bool cpu_isolation_required = false;  // Require isolated cores for binding
    // Maintenance compute servers
    size_t maintenance_cs = 0;
    size_t threads_per_maintenance_cs = 0;
    size_t total_ops = 50000;
    size_t ops_per_client = 0; // derived if absent
    size_t warmup_inserts = 1000;
    std::array<double, 4> operation_mix = {0.5, 0.3, 0.1, 0.1}; // insert, read, update, delete
    std::string workload;
    // Index implementation to use: enum for safe/fast selection in code
    enum class IndexType { Faunus, Sherman };
    IndexType index_type = IndexType::Faunus;
    // Keep the raw name for informational/logging purposes
    std::string index = "faunus";
    DistributionConfig distribution;
    bool use_cache = true;
    size_t max_cs_cache_size_kb = 32;
};

inline IndexConfig load_config(const std::string& yaml_path) {
    IndexConfig cfg;
    YAML::Node node = YAML::LoadFile(yaml_path);
    if (node["num_cs"]) cfg.num_cs = node["num_cs"].as<size_t>();
    if (node["threads_per_cs"]) cfg.threads_per_cs = node["threads_per_cs"].as<size_t>();
    if (node["num_ms"]) cfg.num_ms = node["num_ms"].as<size_t>();
    if (node["mem_per_ms"]) cfg.mem_per_ms = node["mem_per_ms"].as<size_t>();
    if (node["base_rtt_ns"]) cfg.base_rtt_ns = node["base_rtt_ns"].as<size_t>();
    if (node["key_size"]) cfg.key_size = node["key_size"].as<size_t>();
    if (node["value_size"]) cfg.value_size = node["value_size"].as<size_t>();
    if (node["kv_per_thread"]) cfg.kv_per_thread = node["kv_per_thread"].as<size_t>();
    if (node["initial_slabs_per_size"]) cfg.initial_slabs_per_size = node["initial_slabs_per_size"].as<size_t>();
    if (node["log_level"]) cfg.log_level = thread_log::log_level_from_string(node["log_level"].as<std::string>());
    if (node["cpu_binding_enabled"]) cfg.cpu_binding_enabled = node["cpu_binding_enabled"].as<bool>();
    if (node["cpu_binding_start_core"]) cfg.cpu_binding_start_core = node["cpu_binding_start_core"].as<size_t>();
    if (node["cpu_isolation_required"]) cfg.cpu_isolation_required = node["cpu_isolation_required"].as<bool>();
    if (node["maintenance_cs"]) cfg.maintenance_cs = node["maintenance_cs"].as<size_t>();
    if (node["threads_per_maintenance_cs"]) cfg.threads_per_maintenance_cs = node["threads_per_maintenance_cs"].as<size_t>();
    if (node["total_ops"]) cfg.total_ops = node["total_ops"].as<size_t>();
    if (node["ops_per_client"]) cfg.ops_per_client = node["ops_per_client"].as<size_t>();
    if (node["warmup_inserts"]) cfg.warmup_inserts = node["warmup_inserts"].as<size_t>();
    if (node["workload"]) cfg.workload = node["workload"].as<std::string>();
    else if (node["ycsb_workload"]) cfg.workload = node["ycsb_workload"].as<std::string>();

    if (node["index"]) {
        std::string raw = node["index"].as<std::string>();
        // normalize to lowercase
        std::transform(raw.begin(), raw.end(), raw.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        cfg.index = raw;
        if (raw == "sherman") cfg.index_type = IndexConfig::IndexType::Sherman;
        else cfg.index_type = IndexConfig::IndexType::Faunus;
    }

    if (!cfg.workload.empty()) {
        static const std::map<std::string, std::array<double, 4>> ycsb_presets = {
            {"ycsb_a", {0.0, 0.5, 0.5, 0.0}},
            {"ycsb_b", {0.0, 0.95, 0.05, 0.0}},
            {"ycsb_c", {0.0, 1.0, 0.0, 0.0}},
            {"ycsb_d", {0.05, 0.95, 0.0, 0.0}},
            {"ycsb_e", {0.05, 0.95, 0.0, 0.0}},
            {"ycsb_f", {0.0, 0.5, 0.5, 0.0}},
        };

        std::string preset = cfg.workload;
        std::transform(preset.begin(), preset.end(), preset.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (preset.size() == 1) {
            preset = std::string("ycsb_") + preset;
        } else if (preset.rfind("ycsb_", 0) != 0) {
            preset = std::string("ycsb_") + preset;
        }
        auto it = ycsb_presets.find(preset);
        if (it != ycsb_presets.end()) {
            cfg.operation_mix = it->second;
        } else {
            LOG_WARN("Unknown workload preset '" << cfg.workload << "'. Falling back to explicit operation_mix.");
        }
    }

    if (node["cache"]) {
        auto cache_node = node["cache"];
        if (cache_node["enabled"]) cfg.use_cache = cache_node["enabled"].as<bool>();
        if (cache_node["max_size_kb"]) cfg.max_cs_cache_size_kb = cache_node["max_size_kb"].as<size_t>();
    }

    if (node["operation_mix"]) {
        std::map<std::string, double> mix = node["operation_mix"].as<std::map<std::string, double>>();
        auto get_ratio = [&](const std::string& key, double def) {
            auto it = mix.find(key);
            return it == mix.end() ? def : it->second;
        };
        cfg.operation_mix = {
            get_ratio("insert", cfg.operation_mix[0]),
            get_ratio("read", cfg.operation_mix[1]),
            get_ratio("update", cfg.operation_mix[2]),
            get_ratio("delete", cfg.operation_mix[3])
        };
        double total = cfg.operation_mix[0] + cfg.operation_mix[1] + cfg.operation_mix[2] + cfg.operation_mix[3];
        if (total <= 0.0) {
            cfg.operation_mix = {1.0, 0.0, 0.0, 0.0};
        } else {
            for (auto& r : cfg.operation_mix) r /= total;
        }
    }

    if (cfg.ops_per_client == 0) {
        size_t total_clients = std::max<size_t>(1, cfg.num_cs * cfg.threads_per_cs);
        cfg.ops_per_client = cfg.total_ops > 0 ? std::max<size_t>(1, cfg.total_ops / total_clients) : cfg.kv_per_thread;
    }

    if (node["distribution"]) {
        auto dist_node = node["distribution"];
        if (dist_node["type"]) {
            cfg.distribution.type = DistributionConfig::parse_type(dist_node["type"].as<std::string>());
        }
        if (dist_node["key_space"]) {
            cfg.distribution.key_space = dist_node["key_space"].as<size_t>();
        }
        if (dist_node["zipf"]) {
            cfg.distribution.zipf = dist_node["zipf"].as<double>();
        }
    }

    // Clamp skew parameter to sane bounds (theta must be in (0,1))
    if (cfg.distribution.type == DistributionConfig::Type::Skewed) {
        constexpr double kEpsilon = 1e-6;
        if (cfg.distribution.zipf <= 0.0) cfg.distribution.zipf = 0.5;
        if (cfg.distribution.zipf >= 1.0) cfg.distribution.zipf = 1.0 - kEpsilon;
    }

    // Ensure key space is large enough for inserts per client
    size_t minimum_space = cfg.warmup_inserts + cfg.ops_per_client;
    if (cfg.distribution.key_space < minimum_space) {
        cfg.distribution.key_space = minimum_space;
        LOG_WARN("distribution.key_space too small for requested workload; expanding to " << minimum_space);
    }
    
    // Validate CPU binding configuration
    if (cfg.cpu_binding_enabled) {
        // We need validation of CPU cores, but we'll do this in the main code
        // since we need to include the CPU affinity utility
        LOG_INFO("CPU binding enabled: starting from core " << cfg.cpu_binding_start_core);
    }
    
    // Add more fields as needed
    return cfg;
}
