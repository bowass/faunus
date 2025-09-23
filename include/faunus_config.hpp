#pragma once

namespace faunus_config {
    // RDMA operation delays (in microseconds)
    constexpr int CAS_DELAY_US = 50; // CAS operation extra cost
    constexpr int BASE_RTT_US = 15;  // Base RTT for RDMA manager
    // Add more config constants as needed
}
