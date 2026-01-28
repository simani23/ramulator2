#!/bin/bash
# =============================================================================
# FlexProf Configuration Test Script
# =============================================================================
# This script runs all SST-format traces through three different controller configs:
#   1. Baseline (Generic controller - no FlexProf)
#   2. FlexProf Static (pre-configured turn-based scheduling)
#   3. FlexProf Dynamic (runtime-adaptable scheduling)
#
# SST trace format: <R|W> <address> <domain_id>
# =============================================================================

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
CYAN='\033[0;36m'
NC='\033[0m' # No Color

# Configuration
RAMULATOR="./ramulator2"
RESULTS_DIR="test_results"
TIMESTAMP=$(date +%Y%m%d_%H%M%S)

# Config templates
CONFIGS=("baseline" "static" "dynamic")
CONFIG_FILES=("test_configs/baseline_sst.yaml" "test_configs/static_sst.yaml" "test_configs/dynamic_sst.yaml")

# Check if ramulator2 binary exists
if [ ! -f "$RAMULATOR" ]; then
    echo -e "${RED}Error: ramulator2 binary not found!${NC}"
    echo "Please build the project first with: mkdir build && cd build && cmake .. && make"
    exit 1
fi

# Create results directory
mkdir -p "$RESULTS_DIR"

# Function to check if a trace file is SST format (R/W <addr> <domain_id>)
is_sst_format() {
    local trace_file="$1"
    # Check first non-comment, non-empty line for SST format pattern
    local first_data_line=$(grep -v '^#' "$trace_file" | grep -v '^$' | head -1)
    if [[ "$first_data_line" =~ ^[RWrw] ]]; then
        return 0  # true - is SST format
    fi
    return 1  # false - not SST format
}

# Function to get max domain ID from trace
get_max_domain() {
    local trace_file="$1"
    local max_domain=0
    while read -r line; do
        # Skip comments and empty lines
        [[ "$line" =~ ^# ]] && continue
        [[ -z "$line" ]] && continue
        # Extract domain ID (third field)
        local domain=$(echo "$line" | awk '{print $3}')
        if [[ "$domain" =~ ^[0-9]+$ ]] && [ "$domain" -gt "$max_domain" ]; then
            max_domain=$domain
        fi
    done < "$trace_file"
    echo $((max_domain + 1))  # Return count (0-indexed to count)
}

# Function to run a single trace through a single config
run_single_test() {
    local trace_file="$1"
    local config_name="$2"
    local config_file="$3"
    local trace_basename=$(basename "$trace_file" .trace)
    local output_file="$RESULTS_DIR/${trace_basename}_${config_name}_${TIMESTAMP}.txt"
    
    # Get number of domains from trace
    local num_domains=$(get_max_domain "$trace_file")
    
    # Run ramulator with trace path override
    if "$RAMULATOR" -f "$config_file" \
        -p "Frontend.path=$trace_file" \
        -p "Controller.num_domains=$num_domains" \
        > "$output_file" 2>&1; then
        echo -e "    ${GREEN}PASS${NC}"
        return 0
    else
        echo -e "    ${RED}FAIL${NC}"
        echo "    Error: $(tail -3 "$output_file" | head -1)"
        return 1
    fi
}

# Find all SST-format trace files
echo -e "${BLUE}============================================================${NC}"
echo -e "${BLUE}FlexProf Configuration Test - All SST Traces${NC}"
echo -e "${BLUE}============================================================${NC}"
echo ""

# Collect SST format traces
SST_TRACES=()

# Check example_sst_trace.trace
if [ -f "example_sst_trace.trace" ] && is_sst_format "example_sst_trace.trace"; then
    SST_TRACES+=("example_sst_trace.trace")
fi

# Check traces/static/ directory
if [ -d "traces/static" ]; then
    for trace in traces/static/*.trace; do
        if [ -f "$trace" ] && is_sst_format "$trace"; then
            SST_TRACES+=("$trace")
        fi
    done
fi

# Check for any other .trace files in root that might be SST format
for trace in *.trace; do
    if [ -f "$trace" ] && [ "$trace" != "example_sst_trace.trace" ] && is_sst_format "$trace"; then
        SST_TRACES+=("$trace")
    fi
done

if [ ${#SST_TRACES[@]} -eq 0 ]; then
    echo -e "${RED}No SST-format traces found!${NC}"
    echo "SST format: <R|W> <address> <domain_id>"
    exit 1
fi

echo -e "Found ${CYAN}${#SST_TRACES[@]}${NC} SST-format trace(s):"
for trace in "${SST_TRACES[@]}"; do
    num_domains=$(get_max_domain "$trace")
    echo "  - $trace (${num_domains} domains)"
done
echo ""
echo "Results directory: $RESULTS_DIR"
echo ""

# Track results
TOTAL_TESTS=0
PASSED=0
FAILED=0

# Run each trace through each config
for trace in "${SST_TRACES[@]}"; do
    trace_basename=$(basename "$trace" .trace)
    echo -e "${BLUE}------------------------------------------------------------${NC}"
    echo -e "${BLUE}Trace: ${CYAN}$trace${NC}"
    echo -e "${BLUE}------------------------------------------------------------${NC}"
    
    for i in "${!CONFIGS[@]}"; do
        config_name="${CONFIGS[$i]}"
        config_file="${CONFIG_FILES[$i]}"
        
        printf "  %-12s: " "$config_name"
        ((TOTAL_TESTS++))
        
        if run_single_test "$trace" "$config_name" "$config_file"; then
            ((PASSED++))
        else
            ((FAILED++))
        fi
    done
    echo ""
done

# Summary
echo -e "${BLUE}============================================================${NC}"
echo -e "${BLUE}Test Summary${NC}"
echo -e "${BLUE}============================================================${NC}"
echo "Total tests: $TOTAL_TESTS"
echo -e "Passed: ${GREEN}$PASSED${NC}"
echo -e "Failed: ${RED}$FAILED${NC}"
echo ""
echo "Results saved to: $RESULTS_DIR/"
echo ""

if [ $FAILED -eq 0 ]; then
    echo -e "${GREEN}All tests passed!${NC}"
    exit 0
else
    echo -e "${RED}Some tests failed!${NC}"
    exit 1
fi
