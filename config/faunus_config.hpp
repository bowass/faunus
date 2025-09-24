#pragma once
#include <string>
#include <yaml-cpp/yaml.h>

struct FaunusConfig {
    int num_cs = 8;
    int threads_per_cs = 8;
    int num_ms = 8;
    int mem_per_ms = 512 * 1024;
    int cas_delay_us = 50;
    int base_rtt_us = 15;
    // Add more config fields as needed
};

inline FaunusConfig load_faunus_config(const std::string& yaml_path) {
    FaunusConfig cfg;
    YAML::Node node = YAML::LoadFile(yaml_path);
    if (node["num_cs"]) cfg.num_cs = node["num_cs"].as<int>();
    if (node["threads_per_cs"]) cfg.threads_per_cs = node["threads_per_cs"].as<int>();
    if (node["num_ms"]) cfg.num_ms = node["num_ms"].as<int>();
    if (node["mem_per_ms"]) cfg.mem_per_ms = node["mem_per_ms"].as<int>();
    if (node["cas_delay_us"]) cfg.cas_delay_us = node["cas_delay_us"].as<int>();
    if (node["base_rtt_us"]) cfg.base_rtt_us = node["base_rtt_us"].as<int>();
    // Add more fields as needed
    return cfg;
}
