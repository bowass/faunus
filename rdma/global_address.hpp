#include <cstdint>
#include <iostream>

/**
 * @brief Global address for mapping to memory servers and offsets.
 */
struct GlobalAddress {
    uint64_t raw;
    GlobalAddress(uint8_t index, uint64_t off)
        : raw((static_cast<uint64_t>(index) << 56) | (off & 0x00FFFFFFFFFFFFFFULL)) {}
    GlobalAddress(uint64_t address) : raw(address) {}
    uint8_t server_index() const { return static_cast<uint8_t>(raw >> 56); }
    uint64_t offset() const { return raw & 0x00FFFFFFFFFFFFFFULL; }
    operator uintptr_t() const { return static_cast<uintptr_t>(raw); }
    operator uint8_t*() const { return reinterpret_cast<uint8_t*>(raw); }
};

inline std::ostream& operator<<(std::ostream& os, const GlobalAddress& gaddr) {
    os << "(" << int(gaddr.server_index()) << ", 0x" << std::hex << gaddr.offset() << std::dec << ")";
    return os;
}
