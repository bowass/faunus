#pragma once
#include <cstdint>
#include <iostream>

/**
 * @brief Global address for mapping to memory servers and offsets.
 */
struct GlobalAddress {
    uint64_t raw : 50;
    uint64_t padding: 12;
    GlobalAddress(uint8_t index, uint64_t off)
        : raw((static_cast<uint64_t>(index) << 42) | (off & 0x0003FFFFFFFFFFULL)) {}
    GlobalAddress(uint64_t address=0) : raw(address) {}
    uint8_t server_index() const { return static_cast<uint8_t>(raw >> 42); }
    uint64_t offset() const { return raw & 0x0003FFFFFFFFFFULL; }
    operator uintptr_t() const { return static_cast<uintptr_t>(raw); }
    operator uint8_t*() const { return reinterpret_cast<uint8_t*>(raw); }

    static GlobalAddress Null() { return GlobalAddress(0); }
    bool operator==(const GlobalAddress& other) const {
        return raw == other.raw;
    }
    bool operator!=(const GlobalAddress& other) const {
        return !(raw == other.raw);
    }
} __attribute__((packed));

static_assert(sizeof(GlobalAddress) == 8);

inline std::ostream& operator<<(std::ostream& os, const GlobalAddress& gaddr) {
    os << "(" << int(gaddr.server_index()) << ", 0x" << std::hex << gaddr.offset() << std::dec << ")";
    return os;
}
