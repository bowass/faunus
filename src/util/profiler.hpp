#pragma once

#include <chrono>
#include <cstdint>
#include <string>
#include <unordered_map>

struct ProfileStats {
    uint64_t count = 0;
    uint64_t total_ns = 0;

    double average_ns() const {
        return count == 0 ? 0.0 : static_cast<double>(total_ns) / static_cast<double>(count);
    }
};

// #define ENABLE_PROFILING

#ifdef ENABLE_PROFILING

class Profiler {
public:
    class Scoped {
    public:
        using Clock = std::chrono::high_resolution_clock;
        explicit Scoped(const char* name) : name_(name), start_(Clock::now()) {}
        explicit Scoped(std::string name) : name_storage_(std::move(name)), name_(name_storage_.c_str()), start_(Clock::now()) {}
        ~Scoped();
    private:
        std::string name_storage_{};
        const char* name_;
        Clock::time_point start_;
    };

    static void add_sample(const char* name, uint64_t duration_ns);
    static void add_sample(const std::string& name, uint64_t duration_ns) {
        add_sample(name.c_str(), duration_ns);
    }

    static void publish_thread_stats();
    static std::unordered_map<std::string, ProfileStats> snapshot();
    static void reset();
};

#else // ENABLE_PROFILING

class Profiler {
public:
    class Scoped {
    public:
        explicit Scoped(const char* /*name*/) {}
        explicit Scoped(std::string /*name*/) {}
        ~Scoped() {}
    };
    static void add_sample(const char* /*name*/, uint64_t /*duration_ns*/) {}
    static void publish_thread_stats() {}
    static std::unordered_map<std::string, ProfileStats> snapshot() { return {}; }
    static void reset() {}
};

#endif // ENABLE_PROFILING
