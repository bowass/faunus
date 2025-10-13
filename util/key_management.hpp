#pragma once

#include <vector>
#include <limits>
#include <random>
#include "../kv_index/kv_index.hpp"
#include "../util/profiler.hpp"

namespace faunus_util {

/**
 * Manages a local set of keys with efficient slot-based storage and reuse.
 * Supports adding keys, deactivating them, and sampling from active keys.
 */
class LocalKeySet {
public:
    explicit LocalKeySet(size_t max_slots)
        : max_slots_(max_slots == 0 ? std::numeric_limits<size_t>::max() : max_slots) {}

    bool has_active() const {
        return !active_slots_.empty();
    }

    bool can_insert() const {
        return !free_slots_.empty() || keys_.size() < max_slots_;
    }

    size_t active_count() const {
        return active_slots_.size();
    }

    size_t slot_from_active_index(size_t idx) const {
        return active_slots_[idx];
    }

    const Key& key_at_slot(size_t slot) const {
        return keys_[slot];
    }

    bool add_new(const Key& key) {
        if (!free_slots_.empty()) {
            size_t slot = free_slots_.back();
            free_slots_.pop_back();
            keys_[slot] = key;
            active_slots_.push_back(slot);
            slot_positions_[slot] = active_slots_.size() - 1;
            return true;
        }
        if (keys_.size() >= max_slots_) {
            return false;
        }
        size_t slot = keys_.size();
        keys_.push_back(key);
        active_slots_.push_back(slot);
        slot_positions_.push_back(active_slots_.size() - 1);
        return true;
    }

    void deactivate(size_t slot) {
        if (slot >= slot_positions_.size()) return;
        size_t pos = slot_positions_[slot];
        if (pos == std::numeric_limits<size_t>::max()) return;
        size_t last_slot = active_slots_.back();
        active_slots_[pos] = last_slot;
        slot_positions_[last_slot] = pos;
        active_slots_.pop_back();
        slot_positions_[slot] = std::numeric_limits<size_t>::max();
        free_slots_.push_back(slot);
    }

private:
    size_t max_slots_;
    std::vector<Key> keys_;
    std::vector<size_t> active_slots_;
    std::vector<size_t> slot_positions_;
    std::vector<size_t> free_slots_;
};

/**
 * Generates a deterministic key from client ID and sequence number.
 */
inline Key encode_key(uint64_t client_id, uint64_t sequence) {
    Profiler::Scoped timer("client.encode_key");
    Key key{};
    uint64_t raw = client_id * 0x9E3779B97F4A7C15ULL;
    raw ^= (sequence + 0xBF58476D1CE4E5B9ULL);
    for (size_t i = 0; i < key.size(); ++i) {
        key.data()[i] = static_cast<uint8_t>(raw & 0xFF);
        raw = (raw >> 8) | (raw << 56);
    }
    return key;
}

/**
 * Generates a random value for testing purposes.
 */
inline Value generate_random_value(std::mt19937_64& gen) {
    Profiler::Scoped timer("client.generate_value");
    Value value;
    std::uniform_int_distribution<uint8_t> dis(0, 255);
    for (size_t i = 0; i < value.size(); ++i) {
        value.data()[i] = dis(gen);
    }
    return value;
}

} // namespace faunus_util