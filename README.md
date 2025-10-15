
# Faunus RDMA Simulation Framework

## Overview
Faunus simulates a disaggregated memory system using RDMA-like operations. It models compute servers (CSs) with multiple worker threads and memory servers (MSs) with exposed memory, communicating via simulated RDMA verbs (READ, WRITE, CAS, FAA). The framework is designed for correctness, atomicity, and realistic multithreaded behavior, including verb coalescing and RTT simulation.

## Features
- **Compute Servers (CS):** Each with multiple worker threads performing RDMA operations.
- **Memory Servers (MS):** Expose memory, minimal computation.
- **RDMA Operations:** READ, WRITE, ATOMIC (CAS, FAA), with atomicity and thread safety.
- **RDMA Manager:** Maps global addresses to memory servers and offsets, supports single and batch operations.
- **Verb Coalescing:** Batch multiple RDMA operations to save RTT.
- **Metrics:** Per-thread statistics (ops, latency, min/max latency), RTT simulation, throughput, plus JSON exports for offline analysis.
- **Maintenance Optimization:** Queued-set data structure prevents duplicate maintenance requests, reducing unnecessary work.
- **Extensible:** Easily add new RDMA verbs or server types.

## Build Instructions

1. **Dependencies:**
	- C++17 or newer
	- POSIX threads (Linux)

2. **Build:**
	```sh
	make kv_test
	```
	This produces the executable `faunus_sim`.

3. **Clean:**
	```sh
	make clean
	```

## Usage

Run the Faunus index workload simulation:
```sh
./kv_test path/to/config.yaml
```

The harness performs three phases:

1. **Warm-up:** A shared pool of inserts (`warmup_inserts` in the YAML config) seeds the tree so subsequent operations can target existing keys.
2. **Mixed workload:** Each compute thread gets a fixed amount of operations to perform, and repeatedly samples an operation type according to the configured `operation_mix` ratios (insert/read/update/delete), and generates key/value pools using local key/value pool.
3. **Reporting:** Per-thread latency stats (avg/min/max), success counters, throughput, and RDMA verb counts/RTT aggregates are printed. Matching JSON artifacts are written to `thread_stats/` (one file per worker plus a `summary.json`) so you can plot results without scraping stdout. The summary now includes the measured workload window, overall operations-per-second, and per-operation throughput alongside the latency distribution. Metrics are still accessible programmatically through `ThreadStats` and `RDMAManager::collect_stats()`.


## Code Structure

- `kv_test.cpp` — RDMA workload harness with shared operation pools and detailed metrics.
- `main.cpp` — Minimal smoke test for the RDMA plumbing.
- `compute_server.hpp/cpp` — Compute server logic, worker threads, per-thread stats.
- `memory_server.hpp/cpp` — Memory server logic, exposes RDMA memory.
- `rdma_simulation.hpp/cpp` — Implements RDMA verbs and atomicity.
- `rdma_manager.hpp/cpp` — Maps global addresses, manages single/batch RDMA operations, simulates RTT.
- `Makefile` — Build instructions.

## Extending the Framework
- Add new RDMA verbs by extending `RDMAOpType` and updating `rdma_simulation` and `rdma_manager`.
- Change server counts, memory sizes, RTT, workload size, and operation mix in `faunus_config.yaml`.
- Activate canned YCSB workloads via `workload: ycsb_a` .. `ycsb_f` in the YAML config, or derive new patterns by tweaking `operation_mix`.
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

### Visualizing results

After running `kv_test`, the JSON metrics under `thread_stats/` can be summarized
and plotted with:

```sh
python3 scripts/visualize_stats.py thread_stats
```

When `matplotlib` is available, the script writes a set of PNG charts next to the
JSON exports and always prints a textual digest of throughput, success ratios, and
latencies.

## License
MIT

# Maintenance Compute Servers

The YAML config now supports the following options:

```
maintenance_cs: <number of maintenance compute servers>
threads_per_maintenance_cs: <threads per maintenance compute server>
```

If these are set to nonzero values, the system will launch maintenance compute servers, each running the `maintenance_worker` function (if implemented by the index).

## Maintenance Queue Optimization

Faunus includes a **queued-set** optimization for maintenance operations that prevents duplicate requests from being enqueued. This is particularly important for B+ tree maintenance where multiple threads might detect the same node needs splitting or merging.

### Key Benefits:
- **Duplicate Prevention:** Only one maintenance request per unique (operation, address) pair is queued
- **Race-Free:** Thread-safe implementation using atomic operations and mutex protection
- **Efficiency:** Reduces unnecessary maintenance work by ~90% in high-contention scenarios
- **Compatibility:** Drop-in replacement for regular maintenance queues

### Usage:
```cpp
// Create queued-sets instead of regular queues
FaunusIndex::set_maintenance_queued_sets(
    FaunusIndex::create_maintenance_queued_sets(num_maintenance_cs)
);

// Request SMO operations (automatically deduplicated)
bool success = index.request_smo(FaunusMaintenanceRPC::SPLIT, leaf_address);
```

### Implementation:
The `QueuedSet<T, K, KeyExtractor>` template provides:
- `try_enqueue()`: Only adds if key doesn't exist, returns true/false
- `enqueue()`: Force adds (used for STOP commands)
- `wait_dequeue()`: Removes from both queue and uniqueness set
- Thread-safe pending key tracking with `std::unordered_set`

This optimization is especially beneficial in write-heavy workloads where many threads might simultaneously detect that the same B+ tree node needs maintenance.

## Workload Configuration

The YAML configuration accepts additional fields that shape the shared operation pools:

```yaml
total_ops: 20000           # how many mixed operations to execute across all threads
warmup_inserts: 1000       # shared inserts executed once before the mixed phase
workload: ycsb_a           # optional preset (ycsb_a .. ycsb_f)
operation_mix:
	insert: 0.5
	read:   0.3
	update: 0.1
	delete: 0.1
```

All workers pull from the same queue, so slow threads don’t cap throughput. Keys created by inserts are placed in a shared lock-free slot pool guarded only by a capacity growth mutex; reads/updates/deletes sample uniformly from active slots. This keeps the key distribution realistic while remaining efficient on a single machine.

# TODO
- Expose per-thread RDMA verb breakdowns
- Faunus implementation
	- Is there a bug in the current implementation?
	- Think about a way to optimize splits, including making clients trigger less splits
		- Maybe change watermarks?
	- Implement `update`, maybe also `delete`
- Sherman + Marlin implementation
- Store latencies per-thread
- Visualization
	- Plot throughput/latency graphs
	- Create a tikz plot generator
