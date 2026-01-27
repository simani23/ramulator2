# FlexProf on Ramulator2

This guide explains how to build and run FlexProf (a secure DRAM memory controller with profiling-based read/write optimization) on Ramulator2.

## Overview

FlexProf provides **memory-controller-level temporal isolation** between security domains (VMs, tenants, containers, etc.) to prevent timing-based side-channel attacks. It achieves this through per-domain request queues and turn-based scheduling.

This repository includes **three FlexProf controller implementations** with different scheduling strategies:

| Controller | Description | Use Case |
|------------|-------------|----------|
| `FlexProf` | Original pattern-based scheduling from offline profiling | Research reproduction, known workloads |
| `FlexProfStatic` | Static turn allocation configured at startup | Predictable workloads, strict isolation |
| `FlexProfDynamic` | Runtime-adaptable scheduling with optional auto-tuning | SST integration, varying workloads |

---

## FlexProf Controller Implementations

### 1. FlexProf (Pattern-Based) - Original Implementation

The original FlexProf controller uses **offline profiling** to determine the optimal read/write scheduling pattern for each workload. A pattern file (`.8pattern`) specifies which domain, operation type, and bank to serve at each scheduling turn.

**Key Characteristics:**
- Pre-computed scheduling pattern from offline profiling
- Pattern specifies: `<domain_id> <operation> <bank>` per turn
- Bank partitioning for spatial isolation
- Fixed turn length (11 cycles for reads, 12 cycles for writes)

**Configuration:**

```yaml
Controller:
  impl: FlexProf
  num_domains: 7                                    # Number of security domains
  alteration: 4                                      # Bank rotation factor
  pattern_file: ../flexprof/input/patterns/lbm.8pattern  # Required: pattern file
  
  Scheduler:
    impl: FlexProf
```

**Pattern File Format:**
```
<domain_id> <operation> <bank>
```
- `domain_id`: 0 to num_domains-1
- `operation`: 0 = Read, 1 = Write
- `bank`: Target bank (0 to alteration-1)

**Example pattern file:**
```
0 0 0    # Domain 0, Read, Bank 0
1 0 1    # Domain 1, Read, Bank 1
2 0 2    # Domain 2, Read, Bank 2
0 1 3    # Domain 0, Write, Bank 3
```

**Config file:** `flexprof_config.yaml` or `flexprof_native_config.yaml`

---

### 2. FlexProfStatic - Static Turn-Based Scheduling

The FlexProfStatic controller uses **static pre-configured** turn allocations and read/write biases. Parameters are set at startup and remain fixed throughout execution.

**Key Characteristics:**
- Turn schedule computed once at initialization
- Per-domain turn allocation (more turns = more bandwidth)
- Per-domain R/W bias (ratio of read vs write turns)
- Deterministic, repeating schedule
- No runtime adaptation

**Configuration:**

```yaml
Controller:
  impl: FlexProfStatic
  
  num_domains: 4                    # Number of security domains
  default_turn_allocation: 1        # Default turns per domain
  default_rw_bias: 0.5              # Default read/write bias (0.0-1.0)
  queue_size: 64                    # Per-domain queue size
  
  # Static per-domain turn allocations (higher = more bandwidth)
  domain_turn_allocations: [2, 1, 1, 2]
  
  # Static per-domain R/W biases
  # 0.0 = all writes, 0.5 = balanced, 1.0 = all reads
  domain_rw_biases: [0.8, 0.3, 0.5, 1.0]
  
  Scheduler:
    impl: FRFCFS
```

**How the Schedule is Built:**

With `allocations = [2, 1, 1, 2]` and `biases = [0.8, 0.3, 0.5, 1.0]`:

| Turn | Domain | Type | Explanation |
|------|--------|------|-------------|
| 0 | 0 | Read | Domain 0 gets 2 turns, 80% reads |
| 1 | 0 | Read | Domain 0's second turn |
| 2 | 1 | Write | Domain 1 gets 1 turn, 30% reads → write |
| 3 | 2 | Read | Domain 2 gets 1 turn, 50% → alternates |
| 4 | 3 | Read | Domain 3 gets 2 turns, 100% reads |
| 5 | 3 | Read | Domain 3's second turn |
| (repeats) | | | |

**Config file:** `flexprof_static_config.yaml`

---

### 3. FlexProfDynamic - Runtime-Adaptable Scheduling

The FlexProfDynamic controller supports **runtime configuration** of scheduling parameters. It can adapt to changing workload characteristics either through an API (for SST integration) or automatically based on queue pressure.

**Key Characteristics:**
- Turn allocations and R/W biases can change at runtime
- Implements `IFlexProfDynamicConfig` interface for external control
- Optional automatic adaptation based on queue pressure
- Designed for SST integration

**Configuration:**

```yaml
Controller:
  impl: FlexProfDynamic
  
  num_domains: 8                    # Number of security domains
  default_turn_allocation: 1        # Default turns per domain
  default_rw_bias: 0.6              # Default read bias (60% reads)
  queue_size: 64                    # Per-domain queue size
  
  # Automatic adaptation (optional)
  auto_adapt: false                 # Enable queue pressure-based adaptation
  adaptation_interval: 10000        # Cycles between adaptations (0 = disabled)
  
  # Initial per-domain configuration (optional, can be changed at runtime)
  # domain_turn_allocations: [2, 1, 1, 1, 1, 1, 1, 1]
  # domain_rw_biases: [0.7, 0.5, 0.6, 0.4, 0.6, 0.5, 0.6, 0.5]
  
  Scheduler:
    impl: FRFCFS
```

**Runtime Configuration API (for SST):**

```cpp
// Get the controller and cast to configuration interface
auto* controller = memory_system->get_controller();
if (auto* flexprof = dynamic_cast<IFlexProfDynamicConfig*>(controller)) {
    // Set turn allocation for domain 0 (more turns = more bandwidth)
    flexprof->set_domain_turn_allocation(0, 3);
    
    // Set R/W bias for domain 0 (0.8 = 80% read turns)
    flexprof->set_domain_rw_bias(0, 0.8);
    
    // Query current configuration
    int turns = flexprof->get_domain_turn_allocation(0);
    float bias = flexprof->get_domain_rw_bias(0);
    
    // Monitor queue pressure
    auto [read_q, write_q] = flexprof->get_domain_queue_occupancy(0);
}
```

**Auto-Adaptation:**

When `auto_adapt: true`, the controller automatically:
1. Monitors queue pressure per domain
2. Redistributes turn allocations based on demand
3. Adjusts R/W biases based on read vs write queue depths

**Config file:** `flexprof_dynamic_config.yaml`

---

## Trace Formats and Frontends

FlexProf supports multiple trace formats through different frontends:

| Frontend | Format | Domain Assignment | Best For |
|----------|--------|-------------------|----------|
| `SSTTrace` | R/W addr domain | Per-entry in trace | SST integration |
| `FlexProfTrace` | USIMM native | Per-entry in trace | Original FlexProf traces |
| `FlexProfMultiTrace` | SimpleO3 | Per trace file | Multiple standard traces |
| `FlexProfLoadStoreTrace` | LD/ST format | Optional per-entry | Mixed workloads |

---

### SSTTrace Frontend (Recommended for SST Integration)

The simplest format for SST integration. Each entry specifies operation, address, and domain.

**Trace Format:**
```
<R|W> <address> <domain_id>
```

**Example trace (`example_sst_trace.trace`):**
```
# Domain 0: Read-heavy (web server)
R 0x10000000 0
R 0x10001000 0
W 0x10003000 0

# Domain 1: Write-heavy (database)
W 0x20000000 1
W 0x20001000 1
R 0x20002000 1

# Domain 2: Balanced (computation)
R 0x30000000 2
W 0x30001000 2
```

**Configuration:**
```yaml
Frontend:
  impl: SSTTrace
  clock_ratio: 8
  max_requests: 0         # 0 = run until trace completes
  wrap_trace: false       # Don't loop the trace
  path: example_sst_trace.trace
```

**Supported operation formats:** `R`, `W`, `READ`, `WRITE`, `LD`, `ST` (case-insensitive)

**Config file:** `flexprof_static_config.yaml`

---

### FlexProfMultiTrace Frontend

Uses multiple standard ramulator2 traces (SimpleO3 format), where each trace file represents a different security domain.

**Trace Format (per file):**
```
<bubble_count> <addr>                    # Read only
<bubble_count> <load_addr> <store_addr>  # Read + Write
```

**Example:**
```
0 0x7fff1234
5 0x7fff5678 0x7fff9abc
12 0x7fff0000
```

**Configuration:**
```yaml
Frontend:
  impl: FlexProfMultiTrace
  clock_ratio: 8
  max_requests: 10000000
  scheduling_mode: round_robin  # or interleaved
  traces:
    - example_inst_domain0.trace   # Domain 0
    - example_inst_domain1.trace   # Domain 1
    - example_inst_domain2.trace   # Domain 2
    # ... one file per domain
```

**Config file:** `flexprof_multitrace_config.yaml` or `flexprof_dynamic_config.yaml`

---

### FlexProfTrace Frontend (Native USIMM Format)

Reads original FlexProf/USIMM format traces with embedded domain IDs.

**Trace Format:**
```
<non_mem_ops> R <hex_addr> <pc> <domain_id>   # Read
<non_mem_ops> W <hex_addr> <domain_id>        # Write
```

**Example:**
```
0 W 0x19ea075380 2           # 0 bubbles, Write to addr, domain 2
72 R 0x15009ca3c0 0x6bb3c0 2 # 72 bubbles, Read from addr, PC, domain 2
```

**Configuration:**
```yaml
Frontend:
  impl: FlexProfTrace
  clock_ratio: 8
  max_requests: 10000000
  path: traces/flexprof/lbm_combined.trace
```

**Config file:** `flexprof_native_config.yaml`

---

### FlexProfLoadStoreTrace Frontend

Supports standard LD/ST format with optional domain IDs per entry.

**Trace Format:**
```
LD <addr> [<domain_id>]    # Load (read)
ST <addr> [<domain_id>]    # Store (write)
```

**Example:**
```
LD 0x7fff1234 0      # Read to domain 0
ST 0x7fff5678 0      # Write to domain 0
LD 0x8fff0000 1      # Read to domain 1
```

**Configuration:**
```yaml
Frontend:
  impl: FlexProfLoadStoreTrace
  clock_ratio: 8
  max_requests: 10000000
  default_domain: 0       # Domain for entries without explicit domain
  path: traces/mixed_domains.trace
```

**Config file:** `flexprof_loadstore_config.yaml`

---

## Quick Start Guide

### Prerequisites

```bash
# Install dependencies (Ubuntu)
sudo apt update
sudo apt install g++-12 cmake git-lfs
```

### Build

```bash
cd /path/to/ramulator2
mkdir -p build && cd build
cmake ..
make -j$(nproc)
cp ramulator2 ../
```

### Running FlexProf Simulations

#### Scenario 1: SST-Style Traces with Static Scheduling

Best for: Testing isolation with predictable scheduling

```bash
./ramulator2 -f flexprof_static_config.yaml
```

Uses `SSTTrace` frontend with `FlexProfStatic` controller. The example trace (`example_sst_trace.trace`) simulates 4 domains with different workload patterns.

#### Scenario 2: Multi-Domain Traces with Dynamic Scheduling

Best for: SST integration development, adaptive workloads

```bash
./ramulator2 -f flexprof_dynamic_config.yaml
```

Uses `FlexProfMultiTrace` frontend with `FlexProfDynamic` controller. Requires trace files for each domain (`example_inst_domain0.trace` through `example_inst_domain7.trace`).

#### Scenario 3: Original FlexProf with Pattern Files

Best for: Reproducing FlexProf research results

```bash
# First, prepare pattern files from flexprof repository
cd ../flexprof
git lfs pull

# Convert traces
cd ../ramulator2
python3 convert_flexprof_traces_v2.py --mode native \
    --input ../flexprof/input/domains/lbm \
    --output traces/flexprof/lbm_combined.trace

# Run simulation
./ramulator2 -f flexprof_native_config.yaml
```

#### Scenario 4: Using Standard Ramulator2 Traces

Best for: Using existing ramulator2 benchmarks with FlexProf isolation

```bash
./ramulator2 -f flexprof_multitrace_config.yaml
```

Configure trace paths in the YAML file to point to your existing traces.

---

## Controller Comparison

| Feature | FlexProf | FlexProfStatic | FlexProfDynamic |
|---------|----------|----------------|-----------------|
| **Scheduling** | Pattern file | Pre-computed at init | Runtime adaptable |
| **Turn allocation** | From pattern | Static config | Configurable at runtime |
| **R/W bias** | From pattern | Static config | Configurable at runtime |
| **Bank partitioning** | Yes | No | No |
| **SST integration** | Limited | Good | Best |
| **Auto-adaptation** | No | No | Yes (optional) |
| **Pattern file required** | Yes | No | No |
| **Use case** | Research | Predictable workloads | Production/SST |

---

## Configuration Files Summary

| Config File | Controller | Frontend | Description |
|-------------|------------|----------|-------------|
| `flexprof_config.yaml` | FlexProf | SimpleO3 | Legacy: 7 separate trace files |
| `flexprof_native_config.yaml` | FlexProf | FlexProfTrace | Native USIMM format |
| `flexprof_static_config.yaml` | FlexProfStatic | SSTTrace | SST format with static scheduling |
| `flexprof_dynamic_config.yaml` | FlexProfDynamic | FlexProfMultiTrace | Dynamic scheduling |
| `flexprof_multitrace_config.yaml` | FlexProf | FlexProfMultiTrace | Multiple SimpleO3 traces |
| `flexprof_loadstore_config.yaml` | FlexProf | FlexProfLoadStoreTrace | LD/ST format |

---

## Example Traces

| Trace File | Format | Description |
|------------|--------|-------------|
| `example_sst_trace.trace` | SSTTrace | 4 domains with varied workload patterns |
| `example_inst_domain[0-7].trace` | SimpleO3 | 8 domain traces for multi-trace testing |
| `example_inst.trace` | SimpleO3 | Single standard ramulator2 trace |

---

## Spatial Isolation Mechanism

All FlexProf controllers maintain **per-domain queues** for temporal isolation:

```
┌─────────────────────────────────────────────────────────────┐
│                    FlexProf Controller                       │
├─────────────────────────────────────────────────────────────┤
│  Domain 0:  [Read Queue 0]  [Write Queue 0]                 │
│  Domain 1:  [Read Queue 1]  [Write Queue 1]                 │
│  Domain 2:  [Read Queue 2]  [Write Queue 2]                 │
│  ...                                                         │
│  Domain N:  [Read Queue N]  [Write Queue N]                 │
├─────────────────────────────────────────────────────────────┤
│                    Turn-Based Scheduler                      │
│  ┌─────────────────────────────────────────────────────┐    │
│  │ Only the current turn's domain can issue requests   │    │
│  │ → No cross-domain timing interference               │    │
│  └─────────────────────────────────────────────────────┘    │
└─────────────────────────────────────────────────────────────┘
```

**Isolation Guarantees:**
1. **Request routing**: Requests go to domain-specific queues
2. **Turn-based access**: Only one domain can issue requests per turn
3. **No cross-domain forwarding**: Write-to-read forwarding only within same domain
4. **Predictable timing**: Turn lengths are deterministic

---

## Statistics and Metrics

FlexProf controllers report these statistics:

| Metric | Description |
|--------|-------------|
| `num_read_reqs_X` | Total read requests on channel X |
| `num_write_reqs_X` | Total write requests on channel X |
| `flexprof_*_total_turns_X` | Total scheduling turns completed |
| `flexprof_*_read_turns_X` | Read-prioritized turns |
| `flexprof_*_write_turns_X` | Write-prioritized turns |
| `domain_Y_reads` | Reads issued for domain Y |
| `domain_Y_writes` | Writes issued for domain Y |
| `domain_Y_turns_served` | Turns served for domain Y |
| `total_read_latency_X` | Total read latency (cycles) |

---

## File Locations

```
ramulator2/
├── src/
│   ├── dram_controller/
│   │   ├── flexprof_dynamic.h              # Dynamic config interface
│   │   └── impl/
│   │       ├── flexprof_controller.cpp     # Original pattern-based
│   │       ├── flexprof_static_controller.cpp   # Static turn-based
│   │       └── flexprof_dynamic_controller.cpp  # Dynamic/adaptive
│   └── frontend/impl/memory_trace/
│       ├── flexprof_trace.cpp              # Native USIMM format
│       ├── flexprof_multi_trace.cpp        # Multiple SimpleO3 traces
│       ├── flexprof_loadstore_trace.cpp    # LD/ST format
│       └── sst_trace.cpp                   # SST format
├── flexprof_config.yaml                    # Legacy configuration
├── flexprof_native_config.yaml             # Native USIMM configuration
├── flexprof_static_config.yaml             # Static scheduling config
├── flexprof_dynamic_config.yaml            # Dynamic scheduling config
├── flexprof_multitrace_config.yaml         # Multi-trace config
├── flexprof_loadstore_config.yaml          # LoadStore config
├── example_sst_trace.trace                 # Example SST format trace
├── example_inst_domain[0-7].trace          # Example per-domain traces
├── convert_flexprof_traces.py              # Legacy trace converter
├── convert_flexprof_traces_v2.py           # Native trace converter
└── FLEXPROF_README.md                      # This file
```

---

## Troubleshooting

### Pattern file not found (FlexProf controller only)

```bash
# Check pattern file exists and is not LFS pointer
ls -la ../flexprof/input/patterns/lbm.8pattern

# If it shows "version https://git-lfs.github.com/spec/v1":
cd ../flexprof && git lfs pull
```

### Domain ID out of range

Ensure `num_domains` in the controller config matches your trace's domain IDs. Domain IDs should be 0 to (num_domains - 1).

### No requests being scheduled

1. Check that trace file paths are correct
2. Verify trace format matches the frontend
3. Check console output for parsing errors/warnings

### Build errors

```bash
cd build
rm -rf *
cmake ..
make -j$(nproc)
```

---

## References

- Original FlexProf paper: [FlexProf: Profiling-Based Memory Controller for Secure DRAM](https://link-to-paper)
- Ramulator2 documentation: [GitHub Repository](https://github.com/CMU-SAFARI/ramulator2)
