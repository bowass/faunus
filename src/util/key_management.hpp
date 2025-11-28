#pragma once

#include <vector>
#include <limits>
#include <random>
#include "kv_index/kv_index.hpp"
#include "util/profiler.hpp"

namespace util {

/**
 * Generates a key from global key space using distribution sampler.
 * This creates realistic key distributions with potential thread contention.
 */
template<typename RNG>
inline Key sample_key_from_global_space(RNG& rng, const util::KeySelectionSampler& sampler, size_t key_space_size) {
    Key key{};
    do {
        size_t key_index = sampler.sample(rng);
        if (key_index >= key_space_size) {
            key_index = key_index % key_space_size;  // Wrap around for safety
        }
        // Use a different hash than encode_key to avoid correlation
        uint64_t raw = key_index * 0x517CC1B727220A95ULL;
        for (size_t i = 0; i < key.size(); ++i) {
            key.data()[i] = static_cast<uint8_t>(raw & 0xFF);
            raw = (raw >> 8) | (raw << 56);
        }
    } while (key == Key::min() || key == Key::max());
    return key;
}

/**
 * Generates a random value for testing purposes.
 */
inline Value generate_random_value(std::mt19937_64& gen) {
    // Profiler::Scoped timer("client.generate_value");
    Value value;
    do {
        std::uniform_int_distribution<uint8_t> dis(0, 255);
        for (size_t i = 0; i < value.size(); ++i) {
            value.data()[i] = dis(gen);
        }
    } while (value == Value::min());
    return value;
}

} // namespace util