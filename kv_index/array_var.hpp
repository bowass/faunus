
#pragma once
#include <cstring>
#include <iostream>
#include <algorithm>

template <size_t N>
class ArrayVar {
protected:
    uint8_t data_[N];
public:
    ArrayVar() { std::fill(data_, data_ + N, 0); }
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
};
