
# Faunus RDMA Simulation Framework

## Overview
Faunus simulates a disaggregated memory system using RDMA-like operations. It models compute servers (CSs) with multiple worker threads and memory servers (MSs) with exposed memory, communicating via simulated RDMA verbs (READ, WRITE, CAS, FAA). The framework is designed for correctness, atomicity, and realistic multithreaded behavior, including verb coalescing and RTT simulation.

## Features
- **Compute Servers (CS):** Each with multiple worker threads performing RDMA operations.
- **Memory Servers (MS):** Expose memory, minimal computation.
- **RDMA Operations:** READ, WRITE, ATOMIC (CAS, FAA), with atomicity and thread safety.
- **RDMA Manager:** Maps global addresses to memory servers and offsets, supports single and batch operations.
- **Verb Coalescing:** Batch multiple RDMA operations to save RTT.
- **Metrics:** Per-thread statistics (ops, latency, min/max latency), RTT simulation, throughput.
- **Extensible:** Easily add new RDMA verbs or server types.

## Build Instructions

1. **Dependencies:**
	- C++17 or newer
	- POSIX threads (Linux)

2. **Build:**
	```sh
	make
	```
	This produces the executable `faunus_sim`.

3. **Clean:**
	```sh
	make clean
	```

## Usage

Run the simulation:
```sh
./faunus_sim
```

The output includes per-thread statistics, single and batch RDMA operation latencies, and overall metrics.

## Code Structure

- `main.cpp` — Entry point, sets up servers, manager, and runs tests.
- `compute_server.hpp/cpp` — Compute server logic, worker threads, per-thread stats.
- `memory_server.hpp/cpp` — Memory server logic, exposes RDMA memory.
- `rdma_simulation.hpp/cpp` — Implements RDMA verbs and atomicity.
- `rdma_manager.hpp/cpp` — Maps global addresses, manages single/batch RDMA operations, simulates RTT.
- `Makefile` — Build instructions.

## Extending the Framework
- Add new RDMA verbs by extending `RDMAOpType` and updating `rdma_simulation` and `rdma_manager`.
- Change server counts, memory sizes, or RTT in `main.cpp`.
- Add new metrics to `ThreadStats` in `compute_server.hpp`.

## Example Output
```
CS 0:
  Thread 0: ops=1000, avg_latency(us)=12.34, min_latency(us)=10.01, max_latency(us)=15.67
  Thread 1: ops=1000, avg_latency(us)=12.12, min_latency(us)=10.02, max_latency(us)=15.45
...
Testing single RDMA operations...
Single WRITE latency(us): 11.23
Testing batch RDMA operations...
Batch WRITE 0 latency(us): 10.98
Batch WRITE 1 latency(us): 11.01
...
```

## License
MIT

