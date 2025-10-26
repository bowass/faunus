#pragma once
#include <cstring>
#include <iostream>
#include <algorithm>
#include <cstdint>

template <size_t N>
class alignas(1) ArrayVar {
protected:
    uint8_t data_[N];
public:
    ArrayVar() { std::fill(data_, data_ + N, 0); }
    // Construct from an integer. Interpreted as big-endian; the most
    // significant byte goes into data_[0]. Only the lowest N bytes of the
    // provided value are used (for N <= 8). For N > 8 the high bytes of the
    // integer are ignored.
    ArrayVar(uint64_t v) {
        std::fill(data_, data_ + N, 0);
        // Fill from the least-significant byte into the end of the array
        // so that data_ is big-endian: data_[0] is MSB.
        for (size_t i = 0; i < N; ++i) {
            size_t idx = N - 1 - i;
            if (i < sizeof(v)) {
                data_[idx] = static_cast<uint8_t>(v & 0xFFu);
                v >>= 8;
            } else {
                data_[idx] = 0;
            }
        }
    }
    ArrayVar(const uint8_t* src, size_t size) {
        std::fill(data_, data_ + N, 0);
        std::copy(src, src + std::min(size, N), data_);
    }
    ArrayVar(const ArrayVar& other) {
        std::copy(other.data_, other.data_ + N, data_);
    }
    ArrayVar& operator=(const ArrayVar& other) {
        if (this != &other) {
            std::copy(other.data_, other.data_ + N, data_);
        }
        return *this;
    }
    ~ArrayVar() = default;

    bool operator==(const ArrayVar& other) const {
        return compare(other) == 0;
    }
    bool operator<(const ArrayVar& other) const {
        return compare(other) < 0;
    }
    bool operator<=(const ArrayVar& other) const {
        return compare(other) <= 0;
    }
    bool operator!=(const ArrayVar& other) const {
        return compare(other) != 0;
    }
    bool operator>(const ArrayVar& other) const {
        return compare(other) > 0;
    }
    bool operator>=(const ArrayVar& other) const {
        return compare(other) >= 0;
    }

    // Add an integer (big-endian semantics). The integer is added to the
    // multi-byte big-endian value stored in data_. Only the low-order
    // sizeof(uint64_t) bytes of the provided integer are considered; for
    // arrays longer than 8 bytes the higher bytes remain unchanged except for
    // carry propagation.
    ArrayVar& operator+=(uint64_t v) {
        uint16_t carry = 0;
        // add starting from least-significant byte (end of array)
        for (size_t i = 0; i < N; ++i) {
            size_t idx = N - 1 - i;
            uint16_t add = carry;
            if (i < sizeof(v)) add += static_cast<uint8_t>(v & 0xFFu);
            uint16_t sum = static_cast<uint16_t>(data_[idx]) + add;
            data_[idx] = static_cast<uint8_t>(sum & 0xFFu);
            carry = (sum > 0xFFu) ? 1 : 0;
            if (i < sizeof(v)) v >>= 8;
        }
        // carry beyond the array is discarded (wrap-around behaviour)
        return *this;
    }

    ArrayVar& operator-=(uint64_t v) {
        int borrow = 0;
        for (size_t i = 0; i < N; ++i) {
            size_t idx = N - 1 - i;
            int sub = borrow;
            if (i < sizeof(v)) sub += static_cast<int>(static_cast<uint8_t>(v & 0xFFu));
            int cur = static_cast<int>(data_[idx]);
            int res = cur - sub;
            if (res < 0) {
                res += 256;
                borrow = 1;
            } else {
                borrow = 0;
            }
            data_[idx] = static_cast<uint8_t>(res & 0xFF);
            if (i < sizeof(v)) v >>= 8;
        }
        // borrow beyond array is discarded (wrap-around behaviour)
        return *this;
    }

    ArrayVar operator+(uint64_t v) const {
        ArrayVar r(*this);
        r += v;
        return r;
    }

    ArrayVar operator-(uint64_t v) const {
        ArrayVar r(*this);
        r -= v;
        return r;
    }
    friend std::ostream& operator<<(std::ostream& os, const ArrayVar& arr) {
        os << "{";
        for (size_t i = 0; i < N; ++i)
            os << std::hex << int(arr.data_[i]) << " ";
        return os << std::dec << "}";
    }

    size_t size() const { return N; }
    uint8_t* data() { return data_; }
    const uint8_t* data() const { return data_; }
    static const ArrayVar& min() {
        static ArrayVar min_val;
        return min_val;
    }
    static const ArrayVar& max() {
        static ArrayVar max_val([](ArrayVar a) { std::fill(a.data_, a.data_ + N, 0xFF); return a; }(ArrayVar()));
        return max_val;
    }
private:
    int compare(const ArrayVar& other) const {
        return std::memcmp(data_, other.data_, N);
    }
} __attribute__((packed));

namespace std {
template <size_t N>
struct hash<ArrayVar<N>> {
    size_t operator()(const ArrayVar<N>& a) const {
        size_t h = 0;
        for (size_t i = 0; i < N; ++i) {
            h = h * 31 + a.data()[i];
        }
        return h;
    }
};
}
