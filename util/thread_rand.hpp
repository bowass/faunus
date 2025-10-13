#pragma once
#include <random>
#include <thread>
#include <cstdint>

namespace faunus_util {
    // Per-thread random generator for 64-bit values
    inline uint64_t thread_rand64() {
        thread_local std::mt19937_64 rng(std::hash<std::thread::id>{}(std::this_thread::get_id()));
        return rng();
    }
}
