# Faunus: Lock-Free Distributed B+Tree for RDMA Disaggregated Memory

[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](https://opensource.org/licenses/MIT)

A research-oriented C++17 simulator for evaluating B+Tree indices optimized for disaggregated memory architectures with RDMA.

## Overview

**Simulation Platform:** Faunus provides a high-fidelity RDMA simulation layer modeling one-sided operations (READ, WRITE, CAS, FAA) with realistic latency characteristics, enabling rapid prototyping and controlled experimental evaluation without physical hardware dependencies. The simulator enforces network round-trip times and supports batched operations to accurately reflect disaggregated memory performance.

**Faunus Design:** Faunus is an optimized B+Tree that minimizes network round-trips through an aggressive lock-free approach, while utilizing operation batching, and supports optional client-side caching and asynchronous background maintenance. The index is designed ground-up for remote memory access patterns, prioritizing RTT reduction over CPU efficiency.

**Baseline:** We implement the [Sherman](https://github.com/thustorage/Sherman) B+Tree design in our simulation platform to serve as a baseline for comparison, following their RDMA-based distributed index approach.

## System Requirements

- **Compiler**: GCC 7+ or Clang 6+ with C++17 support
- **Dependencies**: `yaml-cpp`, `make`
- **Optional**: Python 3 with `matplotlib`, `numpy`, `pyyaml` for experiment automation

## Building

```bash
git clone --recursive https://github.com/bowass/faunus.git
cd faunus
make kv_test
```

**Build Configuration:**
Compile-time parameters can be set via environment variables:
```bash
make kv_test KEY_SIZE=16 VALUE_SIZE=128 FAUNUS_MAINTENANCE_ENABLED=1
```

## Running

**Basic usage:**
```bash
./kv_test config/simple_faunus.yaml
```
Results are written to `thread_stats/` as JSON files.

**Experiment automation:**
```bash
python3 scripts/experiment_runner.py --config experiments/experiments.yaml
python3 scripts/experiment_runner.py --config experiments/experiments.yaml --plot-only
```

## License

MIT License

Copyright (c) 2025 Faunus Contributors

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

**THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.**

---

**Disclaimer**: This is research software intended for experimental evaluation. It is not recommended for production use. No warranties are provided regarding correctness, performance, or suitability for any particular purpose. Users assume all risks and responsibilities when using this software.
