# FlexProf on Ramulator2

This guide explains how to build and run FlexProf (a secure DRAM memory controller with profiling-based read/write optimization) on Ramulator2.

## Overview

FlexProf uses **offline profiling** to determine the optimal read/write ratio for each workload, then schedules memory requests according to a pre-computed pattern that maximizes performance while maintaining security isolation between domains.

### Key Components

| Component | File | Description |
|-----------|------|-------------|
| FlexProf Controller | `src/dram_controller/impl/flexprof_controller.cpp` | Per-domain queues + pattern-based scheduling |
| FlexProf Scheduler | `src/dram_controller/impl/scheduler/flexprof_scheduler.cpp` | FRFCFS fallback scheduler |
| **FlexProf Trace Frontend** | `src/frontend/impl/memory_trace/flexprof_trace.cpp` | **Native USIMM trace reader** |
| Trace Converter (Native) | `convert_flexprof_traces_v2.py` | Converts with domain_id preservation |
| Trace Converter (Legacy) | `convert_flexprof_traces.py` | Converts to SimpleO3 format (lossy) |
| Config Template (Native) | `flexprof_native_config.yaml` | **Recommended configuration** |
| Config Template (Legacy) | `flexprof_config.yaml` | SimpleO3-based configuration |

---

## Trace Format Comparison

### FlexProf (USIMM) Native Format

```
<non_mem_ops> R <hex_addr> <pc> <domain_id>   # Read
<non_mem_ops> W <hex_addr> <domain_id>        # Write
```

**Example:**
```
0 W 0x19ea075380 2           # 0 bubbles, Write to addr, domain 2
72 R 0x15009ca3c0 0x6bb3c0 2 # 72 bubbles, Read from addr, PC, domain 2
```

### Field Meanings

| Field | Meaning | Used By |
|-------|---------|---------|
| `non_mem_ops` | CPU cycles before this memory op | Timing simulation |
| `R/W` | Read or Write operation | **Queue selection (critical!)** |
| `hex_addr` | Physical memory address | Address mapping |
| `pc` | Program counter (reads only) | Unused in scheduling |
| `domain_id` | Security domain (0-6 for 7 domains) | **Per-domain queue routing** |

### How FlexProf Uses Domain IDs

1. **Trace parsing**: `domain_id` extracted from each line
2. **Queue routing**: Requests go to `domain_read_queues[domain_id]` or `domain_write_queues[domain_id]`
3. **Pattern scheduling**: Pattern file specifies `<domain_id> <op> <bank>` per turn
4. **Turn execution**: Only domain matching current pattern turn can issue requests

---

## Two Approaches: Native vs SimpleO3

### Approach 1: Native FlexProf Traces (RECOMMENDED)

Uses `FlexProfTrace` frontend to read USIMM format directly:

```yaml
Frontend:
  impl: FlexProfTrace
  path: traces/flexprof/lbm_combined.trace
```

**Advantages:**
- ✅ Preserves domain_id from trace
- ✅ Preserves R/W distinction
- ✅ Single trace file for all domains
- ✅ Accurate representation of original FlexProf

**Trace conversion:**
```bash
python3 convert_flexprof_traces_v2.py --mode native \
    --input ../flexprof/input/domains/lbm \
    --output traces/flexprof/lbm_combined.trace
```

### Approach 2: SimpleO3 Format (Legacy)

Uses standard SimpleO3 frontend with separate trace per domain:

```yaml
Frontend:
  impl: SimpleO3
  traces:
    - traces/flexprof/lbm_domain0.trace  # core 0 = domain 0
    - traces/flexprof/lbm_domain1.trace  # core 1 = domain 1
    ...
```

**Limitations:**
- ⚠️ Domain ID derived from core/file index
- ⚠️ Loses explicit R/W distinction
- ⚠️ Requires 7 separate trace files

---

## Prerequisites

1. **C++20 compiler** (g++-12 or clang++-15)
2. **CMake** (3.14+)
3. **Git LFS** (for FlexProf pattern files)

```bash
# Install dependencies (Ubuntu)
sudo apt update
sudo apt install g++-12 cmake git-lfs
```

---

## Step 1: Build Ramulator2 with FlexProf

```bash
cd /home/simmi/research/ramulator2

# Create build directory
mkdir -p build
cd build

# Configure and build
cmake ..
make -j$(nproc)

# Copy executable to root
cp ramulator2 ../
cd ..
```

---

## Step 2: Prepare FlexProf Pattern Files

The pattern files are stored in Git LFS in the flexprof repository:

```bash
cd /home/simmi/research/flexprof

# Install and pull LFS files
git lfs install
git lfs pull

# Verify patterns are available
ls -la input/patterns/*.8pattern
```

### Available Benchmarks

| Category | Benchmarks |
|----------|------------|
| SPEC CPU | bwaves, cactuBSSN, cam4, deepsjeng, fotonik3d, gcc, lbm, mcf, namd, omnetpp, perl, roms, xalanc |
| NPB | bt, cg, dc, ep, ft, is, lu, mg, sp, ua |
| Mixes | runmix1-10 |

---

## Step 3: Convert Traces to Ramulator2 Format

### Option A: Native Format (Recommended)

```bash
cd /home/simmi/research/ramulator2
mkdir -p traces/flexprof

# Convert single benchmark (all domains combined)
python3 convert_flexprof_traces_v2.py --mode native \
    --input ../flexprof/input/domains/lbm \
    --output traces/flexprof/lbm_combined.trace \
    --max-lines 1000000

# Convert a mix workload
python3 convert_flexprof_traces_v2.py --mode native --mix \
    --input ../flexprof/input/mix4 \
    --output traces/flexprof/mix4_combined.trace \
    --max-lines 1000000
```

### Option B: SimpleO3 Format (Legacy)

```bash
cd /home/simmi/research/ramulator2
mkdir -p traces/flexprof

# Convert lbm benchmark (separate file per domain)
python3 convert_flexprof_traces_v2.py --mode simpleo3 \
    --input ../flexprof/input/domains/lbm \
    --output traces/flexprof \
    --benchmark lbm \
    --max-lines 1000000
```

### Batch Conversion (All Benchmarks - Native)

```bash
#!/bin/bash
cd /home/simmi/research/ramulator2
mkdir -p traces/flexprof

for benchmark in lbm mcf gcc namd perl bwaves cactuBSSN cam4 cg dc deepsjeng ep fotonik3d ft is lu mg omnetpp roms sp ua xalanc bt; do
    if [ -d "../flexprof/input/domains/$benchmark" ]; then
        python3 convert_flexprof_traces_v2.py --mode native \
            --input "../flexprof/input/domains/$benchmark" \
            --output "traces/flexprof/${benchmark}_combined.trace" \
            --max-lines 1000000
        echo "Converted: $benchmark"
    fi
done
```

---

## Step 4: Run FlexProf Simulation

### Native Mode (Recommended)

```bash
cd /home/simmi/research/ramulator2

# Using native FlexProf trace format
./ramulator2 -f flexprof_native_config.yaml
```

### SimpleO3 Mode (Legacy)

```bash
./ramulator2 -f flexprof_config.yaml
```

### Custom Configuration (Native)

Edit `flexprof_native_config.yaml`:

```yaml
Frontend:
  impl: FlexProfTrace
  clock_ratio: 8
  max_requests: 10000000
  path: traces/flexprof/lbm_combined.trace  # Combined trace with domain IDs

MemorySystem:
  Controller:
    impl: FlexProf
    num_domains: 7                                    # Number of security domains
    alteration: 4                                      # Bank rotation factor
    pattern_file: ../flexprof/input/patterns/lbm.8pattern  # Pattern file path
```

### Running Different Benchmarks

```bash
# Run mcf benchmark
./ramulator2 -c "
Frontend:
  impl: SimpleO3
  clock_ratio: 8
  num_expected_insts: 10000000
  traces:
    - traces/flexprof/mcf_domain0.trace
    - traces/flexprof/mcf_domain1.trace
    - traces/flexprof/mcf_domain2.trace
    - traces/flexprof/mcf_domain3.trace
    - traces/flexprof/mcf_domain4.trace
    - traces/flexprof/mcf_domain5.trace
    - traces/flexprof/mcf_domain6.trace
  Translation:
    impl: RandomTranslation
    max_addr: 2147483648
MemorySystem:
  impl: GenericDRAM
  clock_ratio: 3
  DRAM:
    impl: DDR4
    org:
      preset: DDR4_8Gb_x8
      channel: 1
      rank: 8
    timing:
      preset: DDR4_2400R
  Controller:
    impl: FlexProf
    num_domains: 7
    alteration: 4
    pattern_file: ../flexprof/input/patterns/mcf.8pattern
    Scheduler:
      impl: FlexProf
    RefreshManager:
      impl: AllBank
    RowPolicy:
      impl: ClosedRowPolicy
      cap: 4
  AddrMapper:
    impl: RoBaRaCoCh
"
```

---

## Step 5: Compare with Baseline

To compare FlexProf performance against a baseline (non-secure) scheduler:

### Baseline Configuration

```yaml
# baseline_config.yaml
MemorySystem:
  Controller:
    impl: Generic          # Use generic controller instead of FlexProf
    Scheduler:
      impl: FRFCFS         # Standard FR-FCFS scheduler
```

### Run Comparison

```bash
# Run FlexProf
./ramulator2 -f flexprof_config.yaml > results_flexprof.txt

# Run Baseline
./ramulator2 -f baseline_config.yaml > results_baseline.txt

# Compare cycles
grep "total_cycles" results_flexprof.txt results_baseline.txt
```

---

## Configuration Parameters

### FlexProf Controller Parameters

| Parameter | Type | Default | Description |
|-----------|------|---------|-------------|
| `num_domains` | int | 7 | Number of security domains |
| `alteration` | int | 4 | Bank rotation factor for pattern |
| `pattern_file` | string | required | Path to .8pattern file |

### Pattern File Format

```
<domain_id> <operation> <bank>
```

- `domain_id`: 0 to num_domains-1
- `operation`: 0 = Read, 1 = Write
- `bank`: Bank to target (0 to alteration-1)

Example:
```
0 0 0    # Domain 0, Read, Bank 0
1 0 1    # Domain 1, Read, Bank 1
2 0 2    # Domain 2, Read, Bank 2
0 1 3    # Domain 0, Write, Bank 3
```

---

## Trace Format Reference

### FlexProf (USIMM) Format

```
<non_mem_ops> R <hex_addr> <pc> <domain_id>
<non_mem_ops> W <hex_addr> <domain_id>
```

### Ramulator2 SimpleO3 Format

```
<non_mem_ops> <addr>
```

---

## Troubleshooting

### Pattern file not found

```bash
# Check pattern file exists
ls -la ../flexprof/input/patterns/lbm.8pattern

# If it shows "version https://git-lfs.github.com/spec/v1", run:
cd ../flexprof && git lfs pull
```

### No traces found

```bash
# Check traces were converted
ls -la traces/flexprof/

# Re-run conversion
python3 convert_flexprof_traces.py --input ../flexprof/input/domains/lbm --output traces/flexprof --benchmark lbm
```

### Build errors

```bash
# Clean and rebuild
cd build
rm -rf *
cmake ..
make -j$(nproc)
```

---

## File Locations

```
ramulator2/
├── src/dram_controller/impl/
│   ├── flexprof_controller.cpp      # FlexProf controller implementation
│   └── scheduler/
│       └── flexprof_scheduler.cpp   # FlexProf scheduler
├── traces/flexprof/                  # Converted traces (create this)
├── flexprof_config.yaml             # Example configuration
├── convert_flexprof_traces.py       # Trace converter script
└── FLEXPROF_README.md               # This file

flexprof/
├── input/
│   ├── domains/<benchmark>/         # Original traces
│   └── patterns/<benchmark>.8pattern # Profiled patterns
└── profile/<benchmark>/             # Profiling results
```

---

## Performance Metrics

FlexProf reports these statistics:

| Metric | Description |
|--------|-------------|
| `flexprof_pattern_turns_X` | Number of pattern cycles completed |
| `domain_Y_reads` | Reads issued for domain Y |
| `domain_Y_writes` | Writes issued for domain Y |
| `total_read_latency_X` | Total read latency (cycles) |
| `num_read_reqs_X` | Total read requests |
| `num_write_reqs_X` | Total write requests |
