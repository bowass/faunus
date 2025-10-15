#!/bin/bash

echo "============================================"
echo "CPU Core Isolation Setup for Faunus RDMA"
echo "============================================"
echo

# Get number of available cores
CORES=$(nproc)
echo "Available CPU cores: $CORES"

# Suggest isolation for 4 cores (example)
ISOLATED_CORES="0,1,2,3"
echo "Suggested cores to isolate: $ISOLATED_CORES"
echo

echo "To set up CPU isolation for accurate RDMA simulations:"
echo "1. Edit /etc/default/grub and add to GRUB_CMDLINE_LINUX:"
echo "   isolcpus=$ISOLATED_CORES nohz_full=$ISOLATED_CORES rcu_nocbs=$ISOLATED_CORES"
echo
echo "2. Update GRUB configuration and reboot:"
echo "   sudo update-grub"
echo "   sudo reboot"
echo
echo "3. After reboot, verify isolation:"
echo "   cat /proc/cmdline | grep isolcpus"
echo "   cat /sys/devices/system/cpu/isolated"
echo
echo "4. Then set cpu_isolation_required: true in your config"
echo

# Check current isolation status
echo "Current isolation status:"
if [ -f /sys/devices/system/cpu/isolated ]; then
    ISOLATED=$(cat /sys/devices/system/cpu/isolated)
    if [ -z "$ISOLATED" ]; then
        echo "  No cores currently isolated"
    else
        echo "  Isolated cores: $ISOLATED"
    fi
else
    echo "  Isolation file not found (older kernel?)"
fi

echo
echo "Example Faunus configuration for isolated cores:"
echo "----------------------------------------"
cat << 'EOF'
num_cs: 4
threads_per_cs: 1
maintenance_cs: 0
cpu_binding_enabled: true
cpu_binding_start_core: 0
cpu_isolation_required: true  # Requires isolated cores
EOF