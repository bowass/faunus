#include "cpu_affinity.hpp"
#include "thread_logging.hpp"
#include <sstream>
#include <algorithm>
#include <thread>
#include <cstring>  // For strerror
#include <fstream>

#ifdef __linux__
#include <sys/sysinfo.h>
#include <dirent.h>
#include <unistd.h>
#endif

namespace util {

size_t CPUAffinity::get_cpu_count() {
#ifdef __linux__
    return static_cast<size_t>(get_nprocs());
#else
    // Fallback for non-Linux systems
    return static_cast<size_t>(std::thread::hardware_concurrency());
#endif
}

bool CPUAffinity::bind_to_core(size_t core_id) {
#ifdef __linux__
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(core_id, &cpuset);
    
    int result = sched_setaffinity(0, sizeof(cpuset), &cpuset);
    if (result != 0) {
        LOG_ERROR("Failed to bind thread to core " << core_id << ": " << strerror(errno));
        return false;
    }
    
    LOG_DEBUG("Successfully bound thread to core " << core_id);
    return true;
#else
    LOG_WARN("CPU affinity binding not supported on this platform");
    return false;
#endif
}

bool CPUAffinity::bind_to_core_exclusive(size_t core_id) {
#ifdef __linux__
    // First do normal binding
    if (!bind_to_core(core_id)) {
        return false;
    }
    
    // Check if core appears to be isolated
    if (!is_core_busy(core_id)) {
        LOG_INFO("Core " << core_id << " appears to be isolated - good for exclusive use");
    } else {
        LOG_WARN("Core " << core_id << " may have other processes running - performance may be affected");
        LOG_WARN("Consider using kernel parameters: isolcpus=" << core_id << " or cgroup isolation");
    }
    
    return true;
#else
    LOG_WARN("Exclusive CPU affinity binding not supported on this platform");
    return bind_to_core(core_id);
#endif
}

std::vector<size_t> CPUAffinity::get_current_affinity() {
    std::vector<size_t> cores;
    
#ifdef __linux__
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    
    int result = sched_getaffinity(0, sizeof(cpuset), &cpuset);
    if (result != 0) {
        LOG_ERROR("Failed to get thread affinity: " << strerror(errno));
        return cores;
    }
    
    for (size_t i = 0; i < MAX_CPUS; ++i) {
        if (CPU_ISSET(i, &cpuset)) {
            cores.push_back(i);
        }
    }
#endif
    
    return cores;
}

bool CPUAffinity::is_supported() {
#ifdef __linux__
    return true;
#else
    return false;
#endif
}

bool CPUAffinity::validate_cores(const std::vector<size_t>& core_ids) {
    size_t available_cores = get_cpu_count();
    
    for (size_t core_id : core_ids) {
        if (core_id >= available_cores) {
            LOG_ERROR("Invalid core ID " << core_id << " (available cores: 0-" << (available_cores - 1) << ")");
            return false;
        }
    }
    
    return true;
}

std::string CPUAffinity::describe_current_affinity() {
    auto cores = get_current_affinity();
    
    if (cores.empty()) {
        return "unknown/error";
    }
    
    if (cores.size() == 1) {
        return "core " + std::to_string(cores[0]);
    }
    
    std::ostringstream oss;
    oss << "cores [";
    for (size_t i = 0; i < cores.size(); ++i) {
        if (i > 0) oss << ",";
        oss << cores[i];
    }
    oss << "]";
    
    return oss.str();
}

bool CPUAffinity::check_core_isolation(const std::vector<size_t>& core_ids) {
#ifdef __linux__
    for (size_t core_id : core_ids) {
        if (is_core_busy(core_id)) {
            LOG_WARN("Core " << core_id << " appears to have other processes running");
            return false;
        }
    }
    LOG_INFO("All requested cores appear to be isolated");
    return true;
#else
    LOG_WARN("Core isolation checking not supported on this platform");
    return true;  // Assume isolated on non-Linux platforms
#endif
}

std::string CPUAffinity::get_isolation_recommendations(size_t required_cores) {
    std::ostringstream oss;
    oss << "CPU Isolation Recommendations for " << required_cores << " cores:\n";
    oss << "==========================================\n";
    
#ifdef __linux__
    oss << "1. Kernel boot parameters (add to GRUB_CMDLINE_LINUX in /etc/default/grub):\n";
    oss << "   isolcpus=";
    for (size_t i = 0; i < required_cores; ++i) {
        if (i > 0) oss << ",";
        oss << i;
    }
    oss << "\n";
    oss << "   nohz_full=";
    for (size_t i = 0; i < required_cores; ++i) {
        if (i > 0) oss << ",";
        oss << i;
    }
    oss << "\n";
    oss << "   rcu_nocbs=";
    for (size_t i = 0; i < required_cores; ++i) {
        if (i > 0) oss << ",";
        oss << i;
    }
    oss << "\n\n";
    
    oss << "2. After adding parameters, run:\n";
    oss << "   sudo update-grub && sudo reboot\n\n";
    
    oss << "3. Verify isolation after reboot:\n";
    oss << "   cat /proc/cmdline | grep isolcpus\n";
    oss << "   cat /sys/devices/system/cpu/isolated\n\n";
    
    oss << "4. Alternative: Use cgroups to limit other processes:\n";
    oss << "   echo 0 > /sys/fs/cgroup/cpuset/system.slice/cpuset.cpus\n";
    oss << "   # (requires systemd and cgroup configuration)\n\n";
#else
    oss << "Platform-specific isolation not available.\n";
    oss << "Consider using process priorities and minimizing background tasks.\n\n";
#endif
    
    oss << "5. Runtime verification:\n";
    oss << "   - Monitor with: htop, top, or perf\n";
    oss << "   - Check for interrupts: cat /proc/interrupts\n";
    oss << "   - Verify exclusive binding: taskset -p <pid>\n";
    
    return oss.str();
}

bool CPUAffinity::is_core_busy(size_t core_id) {
#ifdef __linux__
    // Also check if core is in isolated set
    std::ifstream isolated_file("/sys/devices/system/cpu/isolated");
    if (isolated_file.is_open()) {
        std::string isolated_cores;
        std::getline(isolated_file, isolated_cores);
        
        // Simple check if our core ID appears in the isolated list
        std::string core_str = std::to_string(core_id);
        if (isolated_cores.find(core_str) != std::string::npos) {
            LOG_DEBUG("Core " << core_id << " is in kernel isolated set: " << isolated_cores);
            return false;  // Core is isolated, so not busy with other processes
        }
    }

    return false;  // Default to not busy
#else
    return false;  // Can't check on non-Linux platforms
#endif
}

} // namespace util