#pragma once

#include <chrono>
#include <thread>

namespace util {

/**
 * High-precision sleep utilities for sub-millisecond durations.
 * 
 * Standard std::this_thread::sleep_for() is inaccurate for microsecond-level
 * sleeps due to OS scheduler limitations. This utility provides more precise
 * timing using hybrid approaches.
 */
class PreciseSleep {
public:
    /**
     * Sleep for the specified duration with high precision.
     * Uses hybrid approach: sleep_for for larger durations, busy-wait for precision.
     * 
     * @param duration Duration to sleep in microseconds
     */
    static void sleep_microseconds(double duration_us) {
        if (duration_us <= 0.0) return;
        
        auto start = std::chrono::high_resolution_clock::now();
        auto duration_ns = std::chrono::nanoseconds(static_cast<int64_t>(duration_us * 1000.0));
        auto target = start + duration_ns;
        
        // For very short durations (< 50us), use pure busy-wait
        if (duration_us < 50.0) {
            busy_wait_until(target);
            return;
        }
        
        // For longer durations, sleep for most of it, then busy-wait for precision
        // Leave the last 10-20 microseconds for busy-waiting
        double sleep_us = duration_us - 15.0;  // Sleep for all but last 15us
        if (sleep_us > 0.0) {
            auto sleep_duration = std::chrono::microseconds(static_cast<int64_t>(sleep_us));
            std::this_thread::sleep_for(sleep_duration);
        }
        
        // Busy-wait for the remaining time for precision
        busy_wait_until(target);
    }
    
    /**
     * Sleep for the specified duration in nanoseconds.
     */
    static void sleep_nanoseconds(double duration_ns) {
        sleep_microseconds(duration_ns / 1000.0);
    }

private:
    /**
     * Busy-wait until the target time, with periodic yields to be CPU-friendly.
     */
    static void busy_wait_until(std::chrono::high_resolution_clock::time_point target) {
        constexpr int YIELD_INTERVAL = 100;  // Yield every 100 iterations
        int iterations = 0;
        
        while (std::chrono::high_resolution_clock::now() < target) {
            if (++iterations % YIELD_INTERVAL == 0) {
                std::this_thread::yield();  // Give other threads a chance
            }
            // Light busy-wait - modern CPUs handle this efficiently
        }
    }
};

/**
 * Convenience function for microsecond-precision sleep.
 * 
 * @param duration_us Duration to sleep in microseconds
 */
inline void precise_sleep_us(double duration_us) {
    PreciseSleep::sleep_microseconds(duration_us);
}

/**
 * Convenience function for nanosecond-precision sleep.
 * 
 * @param duration_ns Duration to sleep in nanoseconds
 */
inline void precise_sleep_ns(double duration_ns) {
    PreciseSleep::sleep_nanoseconds(duration_ns);
}

} // namespace util