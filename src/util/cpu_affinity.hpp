#pragma once

#include <vector>
#include <string>

#ifdef __linux__
#include <sched.h>
#include <unistd.h>
#include <errno.h>
#endif

namespace util {

/**
 * @brief CPU affinity utilities for binding threads to specific cores.
 * 
 * Provides functionality to bind threads to dedicated CPU cores for more
 * accurate performance measurements by reducing thread migration overhead.
 * Supports core isolation to minimize interference from other processes.
 */
class CPUAffinity {
public:
    /**
     * @brief Get the number of available CPU cores on the system.
     * @return Number of CPU cores, or 0 if detection fails
     */
    static size_t get_cpu_count();
    
    /**
     * @brief Bind the current thread to a specific CPU core.
     * @param core_id The CPU core ID to bind to (0-indexed)
     * @return true if successful, false otherwise
     */
    static bool bind_to_core(size_t core_id);
    
    /**
     * @brief Bind the current thread exclusively to a specific CPU core.
     * This attempts to isolate the core from other processes by setting
     * the thread's affinity and optionally checking for isolation.
     * @param core_id The CPU core ID to bind to exclusively
     * @return true if successful, false otherwise
     */
    static bool bind_to_core_exclusive(size_t core_id);
    
    /**
     * @brief Get the current CPU affinity of the calling thread.
     * @return Vector of core IDs the thread is bound to, empty if error
     */
    static std::vector<size_t> get_current_affinity();
    
    /**
     * @brief Check if core binding is supported on this platform.
     * @return true if CPU affinity is supported, false otherwise
     */
    static bool is_supported();
    
    /**
     * @brief Validate that a set of core IDs are available on the system.
     * @param core_ids Vector of core IDs to validate
     * @return true if all cores are valid, false otherwise
     */
    static bool validate_cores(const std::vector<size_t>& core_ids);
    
    /**
     * @brief Check if cores appear to be isolated from other processes.
     * @param core_ids Vector of core IDs to check for isolation
     * @return true if cores appear isolated, false otherwise
     */
    static bool check_core_isolation(const std::vector<size_t>& core_ids);
    
    /**
     * @brief Get system isolation recommendations for optimal performance.
     * @param required_cores Number of cores needed for binding
     * @return String with isolation setup recommendations
     */
    static std::string get_isolation_recommendations(size_t required_cores);
    
    /**
     * @brief Get a string description of the current CPU affinity.
     * @return String describing which cores the thread is bound to
     */
    static std::string describe_current_affinity();

private:
    static constexpr size_t MAX_CPUS = 1024;  // Maximum supported CPU cores
    
    /**
     * @brief Check if a specific core is currently busy with other processes.
     * @param core_id Core ID to check
     * @return true if core appears busy, false if it seems available
     */
    static bool is_core_busy(size_t core_id);
};

} // namespace util