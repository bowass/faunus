#pragma once
#include <string>
#include <vector>
#include <cstdint>

#include "array_var.hpp"

// TODO: make it configurable using IndexConfig
using Key = ArrayVar<8>;
using Value = ArrayVar<8>;

struct KVItem {
    Key key;
    Value value;
};

inline std::ostream& operator<<(std::ostream& os, const KVItem& kv_item) {
    os << "KVItem(key=" << kv_item.key << ", value=" << kv_item.value << ")";
    return os;
}

class GlobalAddress; // forward declaration

/**
 * @brief Abstract base class for a scalable key-value index over RDMA.
 */
class KVIndex {
public:
    virtual ~KVIndex() = default;
    /**
     * @brief Read the value for a given key.
     * @param key Pointer to key array
     * @param value_out Pointer to output value array
     * @return true if found, false otherwise
     */
    virtual bool read(const Key& key, Value& value_out) = 0;
    /**
     * @brief Insert a key-value pair.
     * @param key Pointer to key array
     * @param value Pointer to value array
     * @return true if successful, false otherwise
     */
    virtual bool insert(const Key& key, const Value& value) = 0;
    /**
     * @brief Delete a key-value pair.
     * @param key Pointer to key array
     * @return true if successful, false otherwise
     */
    virtual bool del(const Key& key) = 0;
    /**
     * @brief Update the value for a given key.
     * @param key Pointer to key array
     * @param value Pointer to new value array
     * @return true if successful, false otherwise
     */
    virtual bool update(const Key& key, const Value& value) = 0;

    /**
     * @brief Optional maintenance worker. Override in derived classes if needed.
     * @param thread_id Maintenance thread id (0..N-1)
     */
    virtual void maintenance_worker(size_t cs_id, size_t thread_id) {}

    /**
     * @brief Initialization function (for root init etc.). Override if needed.
     * @param optional parameter
     */
    virtual bool initialize(size_t) { return false; }

    /**
     * @brief Finalization function (for root init etc.). Override if needed.
     * @param optional parameter
     */
    virtual bool finalize(size_t) { return false; }

    /**
     * @brief Get the global address of the root offset pointer.
     * @return GlobalAddress of the root offset pointer
     */
    virtual GlobalAddress get_root_offset_pointer() const = 0;
};
