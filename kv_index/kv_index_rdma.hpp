// #pragma once
// #include "kv_index.hpp"
// #include "../rdma/rdma_manager.hpp"
// #include "../rdma/memory_server.hpp"
// #include <mutex>
// #include <string>
// #include <cstring>

// /**
//  * @brief RDMA-integrated KV-index implementation (simple demonstration).
//  *        Stores key-value pairs in RDMA memory via RDMAManager.
//  */
// class KVIndexRDMA : public KVIndex {
// public:
//     KVIndexRDMA(RDMAManager& rdma_mgr)
//         : rdma_mgr_(rdma_mgr), entry_mutexes_(max_keys_) {
//         // Allocate space for key map directly on the memory server
//         size_t local_address = 0;
//         keymap_offset_ = rdma_mgr_.get_server(GlobalAddress{0}, local_address)->allocate(keymap_bytes_);
//         next_offset_ = keymap_offset_ + keymap_bytes_;
//     }

//     // Optional maintenance worker (no-op by default)
//     void maintenance_worker(int thread_id) override {}

//     struct KeyMeta {
//         char key[64]; // fixed max key size
//         size_t offset;
//         size_t value_len;
//         bool valid;
//     };

//     // Key map is stored as an array of KeyMeta on the memory server
//     static constexpr size_t max_keys_ = 4096;
//     static constexpr size_t keymap_bytes_ = max_keys_ * sizeof(KeyMeta);

//     // Array-based API required by KVIndex
//     bool read(const uint8_t* key, uint8_t* value_out) override {
//         std::string key_str(reinterpret_cast<const char*>(key), 64);
//         std::string value;
//         if (!read(key_str, value)) return false;
//         memcpy(value_out, value.data(), value.size());
//         return true;
//     }

//     bool insert(const uint8_t* key, const uint8_t* value) override {
//         std::string key_str(reinterpret_cast<const char*>(key), 64);
//         std::string value_str(reinterpret_cast<const char*>(value), 64); // assumes value size 64
//         return insert(key_str, value_str);
//     }

//     bool del(const uint8_t* key) override {
//         std::string key_str(reinterpret_cast<const char*>(key), 64);
//         return del(key_str);
//     }

//     bool update(const uint8_t* key, const uint8_t* value) override {
//         std::string key_str(reinterpret_cast<const char*>(key), 64);
//         std::string value_str(reinterpret_cast<const char*>(value), 64); // assumes value size 64
//         return update(key_str, value_str);
//     }

//     // String-based API for legacy/test code
//     bool read(const std::string& key, std::string& value) {
//         int idx = find_key_idx(key);
//         if (idx < 0) return false;
//         std::lock_guard<std::mutex> entry_lock(entry_mutexes_[idx]);
//         KeyMeta meta;
//         if (!read_keymeta(idx, meta)) return false;
//         value.resize(meta.value_len);
//         GlobalAddress gaddr(server_idx_, meta.offset);
//         RDMAOp op{std::chrono::high_resolution_clock::time_point{}, std::chrono::high_resolution_clock::time_point{}, RDMAOpType::READ, gaddr};
//         op.op.read.buffer = reinterpret_cast<uint8_t*>(&value[0]);
//         op.op.read.bytes = meta.value_len;
//         return rdma_mgr_.perform_op(op);
//     }

//     bool insert(const std::string& key, const std::string& value) {
//         int idx = find_key_idx(key);
//         if (idx >= 0) return false;
//         idx = find_free_idx();
//         if (idx < 0) return false;
//         std::lock_guard<std::mutex> entry_lock(entry_mutexes_[idx]);
//         KeyMeta meta;
//         memset(&meta, 0, sizeof(KeyMeta));
//         strncpy(meta.key, key.c_str(), sizeof(meta.key) - 1);
//         meta.offset = next_offset_;
//         meta.value_len = value.size();
//         meta.valid = true;
//         GlobalAddress gaddr(server_idx_, next_offset_);
//         RDMAOp op{std::chrono::high_resolution_clock::time_point{}, std::chrono::high_resolution_clock::time_point{}, RDMAOpType::WRITE, gaddr};
//         op.op.write.buffer = reinterpret_cast<const uint8_t*>(value.data());
//         op.op.write.bytes = value.size();
//         if (!rdma_mgr_.perform_op(op)) return false;
//         if (!write_keymeta(idx, meta)) return false;
//         next_offset_ += value.size();
//         return true;
//     }

//     bool del(const std::string& key) {
//         int idx = find_key_idx(key);
//         if (idx < 0) return false;
//         std::lock_guard<std::mutex> entry_lock(entry_mutexes_[idx]);
//         KeyMeta meta;
//         if (!read_keymeta(idx, meta)) return false;
//         meta.valid = false;
//         return write_keymeta(idx, meta);
//     }

//     bool update(const std::string& key, const std::string& value) {
//         int idx = find_key_idx(key);
//         if (idx < 0) return false;
//         std::lock_guard<std::mutex> entry_lock(entry_mutexes_[idx]);
//         KeyMeta meta;
//         if (!read_keymeta(idx, meta)) return false;
//         if (value.size() > meta.value_len) return false;
//         GlobalAddress gaddr(server_idx_, meta.offset);
//         RDMAOp op{std::chrono::high_resolution_clock::time_point{}, std::chrono::high_resolution_clock::time_point{}, RDMAOpType::WRITE, gaddr};
//         op.op.write.buffer = reinterpret_cast<const uint8_t*>(value.data());
//         op.op.write.bytes = value.size();
//         bool ok = rdma_mgr_.perform_op(op);
//         if (ok) {
//             meta.value_len = value.size();
//             return write_keymeta(idx, meta);
//         }
//         return false;
//     }

// private:
//     // Helper to find key index in keymap
//     int find_key_idx(const std::string& key) {
//         KeyMeta meta;
//         for (size_t i = 0; i < max_keys_; ++i) {
//             std::lock_guard<std::mutex> entry_lock(entry_mutexes_[i]);
//             if (!read_keymeta(i, meta)) continue;
//             if (meta.valid && strncmp(meta.key, key.c_str(), sizeof(meta.key)) == 0) return i;
//         }
//         return -1;
//     }
//     int find_free_idx() {
//         KeyMeta meta;
//         for (size_t i = 0; i < max_keys_; ++i) {
//             std::lock_guard<std::mutex> entry_lock(entry_mutexes_[i]);
//             if (!read_keymeta(i, meta)) return i; // treat unreadable as free
//             if (!meta.valid) return i;
//         }
//         return -1;
//     }
//     bool read_keymeta(size_t idx, KeyMeta& meta) {
//         GlobalAddress gaddr(server_idx_, keymap_offset_ + idx * sizeof(KeyMeta));
//         RDMAOp op{std::chrono::high_resolution_clock::time_point{}, std::chrono::high_resolution_clock::time_point{}, RDMAOpType::READ, gaddr};
//         op.op.read.buffer = reinterpret_cast<uint8_t*>(&meta);
//         op.op.read.bytes = sizeof(KeyMeta);
//         return rdma_mgr_.perform_op(op);
//     }
//     bool write_keymeta(size_t idx, const KeyMeta& meta) {
//         GlobalAddress gaddr(server_idx_, keymap_offset_ + idx * sizeof(KeyMeta));
//         RDMAOp op{std::chrono::high_resolution_clock::time_point{}, std::chrono::high_resolution_clock::time_point{}, RDMAOpType::WRITE, gaddr};
//         op.op.write.buffer = reinterpret_cast<const uint8_t*>(&meta);
//         op.op.write.bytes = sizeof(KeyMeta);
//         return rdma_mgr_.perform_op(op);
//     }

//     RDMAManager& rdma_mgr_;
//     std::uint8_t server_idx_ = 0;
//     size_t keymap_offset_ = 0;
//     size_t next_offset_ = 0;
//     std::vector<std::mutex> entry_mutexes_;
// };
