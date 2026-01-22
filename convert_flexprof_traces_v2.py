#!/usr/bin/env python3
"""
Convert FlexProf (USIMM) traces to Ramulator2 format.

This script provides TWO conversion modes:

1. NATIVE MODE (recommended): Keep FlexProf format, use FlexProfTrace frontend
   - Preserves domain_id, R/W distinction
   - Output: Same format as input (just copies/filters)

2. SIMPLEO3 MODE: Convert to SimpleO3 format (lossy)
   - Uses separate files per domain
   - Loses explicit R/W distinction for standalone writes
   
Usage:
  # Native mode - single combined trace
  python3 convert_flexprof_traces_v2.py --mode native \
      --input ../flexprof/input/domains/lbm \
      --output traces/flexprof/lbm_combined.trace
  
  # SimpleO3 mode - separate traces per domain  
  python3 convert_flexprof_traces_v2.py --mode simpleo3 \
      --input ../flexprof/input/domains/lbm \
      --output traces/flexprof \
      --benchmark lbm
"""

import os
import sys
import argparse
from pathlib import Path


def convert_native(input_path: str, output_file: str, max_lines: int = None, 
                   domain_offset: int = 0):
    """
    Native conversion - preserve FlexProf format.
    Optionally adjust domain IDs with an offset (for mixing benchmarks).
    """
    line_count = 0
    
    with open(input_path, 'r') as fin, open(output_file, 'a') as fout:
        for line in fin:
            if max_lines and line_count >= max_lines:
                break
            
            parts = line.strip().split()
            if len(parts) < 3:
                continue
            
            non_mem_ops = parts[0]
            op_type = parts[1]
            addr = parts[2]
            
            if op_type == 'R':
                # Read: <bubbles> R <addr> <pc> <domain_id>
                if len(parts) >= 5:
                    pc = parts[3]
                    domain_id = int(parts[4]) + domain_offset
                    fout.write(f"{non_mem_ops} R {addr} {pc} {domain_id}\n")
                else:
                    fout.write(f"{non_mem_ops} R {addr} 0x0 {domain_offset}\n")
            elif op_type == 'W':
                # Write: <bubbles> W <addr> <domain_id>
                if len(parts) >= 4:
                    domain_id = int(parts[3]) + domain_offset
                    fout.write(f"{non_mem_ops} W {addr} {domain_id}\n")
                else:
                    fout.write(f"{non_mem_ops} W {addr} {domain_offset}\n")
            
            line_count += 1
    
    return line_count


def convert_simpleo3(input_file: str, output_file: str, max_lines: int = None):
    """
    SimpleO3 conversion - separate files per domain.
    Note: This loses explicit R/W distinction.
    """
    line_count = 0
    
    with open(input_file, 'r') as fin, open(output_file, 'w') as fout:
        for line in fin:
            if max_lines and line_count >= max_lines:
                break
            
            parts = line.strip().split()
            if len(parts) < 3:
                continue
            
            non_mem_ops = parts[0]
            op_type = parts[1]
            addr_hex = parts[2]
            
            # Convert hex address to integer
            try:
                addr = int(addr_hex, 16) if addr_hex.startswith('0x') else int(addr_hex)
            except ValueError:
                continue
            
            # SimpleO3 format: <bubbles> <addr>
            fout.write(f"{non_mem_ops} {addr}\n")
            line_count += 1
    
    return line_count


def convert_directory_native(input_dir: str, output_file: str, max_lines: int = None):
    """Convert all core traces in a directory to a single combined trace."""
    # Remove existing output file
    if os.path.exists(output_file):
        os.remove(output_file)
    
    total_lines = 0
    for i in range(8):  # Up to 8 cores/domains
        core_trace = os.path.join(input_dir, f"core_{i}-2")
        if os.path.exists(core_trace):
            # In domain traces, all entries have domain_id=0, 
            # but we want to assign based on core number
            count = convert_native_with_domain_override(
                core_trace, output_file, max_lines, domain_id=i
            )
            total_lines += count
            print(f"  Added core_{i}-2: {count} lines (domain {i})")
    
    return total_lines


def convert_native_with_domain_override(input_path: str, output_file: str, 
                                        max_lines: int, domain_id: int):
    """
    Native conversion where we override the domain_id in the trace
    with the specified domain_id (based on core number).
    """
    line_count = 0
    
    with open(input_path, 'r') as fin, open(output_file, 'a') as fout:
        for line in fin:
            if max_lines and line_count >= max_lines:
                break
            
            parts = line.strip().split()
            if len(parts) < 3:
                continue
            
            non_mem_ops = parts[0]
            op_type = parts[1]
            addr = parts[2]
            
            if op_type == 'R':
                # Read: <bubbles> R <addr> <pc> <domain_id>
                pc = parts[3] if len(parts) > 3 else "0x0"
                fout.write(f"{non_mem_ops} R {addr} {pc} {domain_id}\n")
            elif op_type == 'W':
                # Write: <bubbles> W <addr> <domain_id>
                fout.write(f"{non_mem_ops} W {addr} {domain_id}\n")
            
            line_count += 1
    
    return line_count


def convert_directory_simpleo3(input_dir: str, output_dir: str, benchmark: str,
                               max_lines: int = None):
    """Convert all core traces to separate SimpleO3 format files."""
    os.makedirs(output_dir, exist_ok=True)
    
    total_lines = 0
    for i in range(8):
        input_trace = os.path.join(input_dir, f"core_{i}-2")
        if not os.path.exists(input_trace):
            continue
        
        output_trace = os.path.join(output_dir, f"{benchmark}_domain{i}.trace")
        count = convert_simpleo3(input_trace, output_trace, max_lines)
        total_lines += count
        print(f"  Converted core_{i}-2 -> {benchmark}_domain{i}.trace ({count} lines)")
    
    return total_lines


def convert_mix_native(mix_dir: str, output_file: str, max_lines: int = None):
    """
    Convert a mix directory where each file represents a different benchmark
    assigned to different domains.
    
    Mix files are named like: <benchmark><domain_id>
    e.g., mix4/ contains: ep2, fotonik3d6, gcc1, lu0, namd3, roms5, xalanc4
    """
    if os.path.exists(output_file):
        os.remove(output_file)
    
    total_lines = 0
    
    for trace_file in sorted(os.listdir(mix_dir)):
        trace_path = os.path.join(mix_dir, trace_file)
        if not os.path.isfile(trace_path):
            continue
        
        # Extract domain ID from filename (last character before extension)
        # e.g., "ep2" -> domain 2, "gcc1" -> domain 1
        try:
            domain_id = int(trace_file[-1])
        except ValueError:
            print(f"  Warning: Cannot extract domain ID from {trace_file}, skipping")
            continue
        
        count = convert_native(trace_path, output_file, max_lines, domain_offset=0)
        total_lines += count
        print(f"  Added {trace_file}: {count} lines (domain {domain_id})")
    
    return total_lines


def main():
    parser = argparse.ArgumentParser(
        description="Convert FlexProf traces to Ramulator2 format",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=__doc__
    )
    parser.add_argument(
        "--mode", "-m", choices=["native", "simpleo3"], default="native",
        help="Conversion mode: 'native' (recommended) or 'simpleo3'"
    )
    parser.add_argument(
        "--input", "-i", required=True,
        help="Input FlexProf trace file or directory"
    )
    parser.add_argument(
        "--output", "-o", required=True,
        help="Output trace file (native) or directory (simpleo3)"
    )
    parser.add_argument(
        "--benchmark", "-b", default=None,
        help="Benchmark name (for simpleo3 directory conversion)"
    )
    parser.add_argument(
        "--max-lines", "-n", type=int, default=None,
        help="Maximum lines per trace file"
    )
    parser.add_argument(
        "--mix", action="store_true",
        help="Input is a mix directory (e.g., input/mix4/)"
    )
    
    args = parser.parse_args()
    
    if args.mode == "native":
        if args.mix:
            # Mix directory conversion
            print(f"Converting mix directory {args.input} to {args.output} (native mode)")
            total = convert_mix_native(args.input, args.output, args.max_lines)
        elif os.path.isdir(args.input):
            # Domain directory conversion
            print(f"Converting directory {args.input} to {args.output} (native mode)")
            total = convert_directory_native(args.input, args.output, args.max_lines)
        else:
            # Single file conversion
            print(f"Converting {args.input} to {args.output} (native mode)")
            total = convert_native(args.input, args.output, args.max_lines)
        print(f"Total: {total} lines written to {args.output}")
        
    else:  # simpleo3 mode
        if os.path.isdir(args.input):
            benchmark = args.benchmark or os.path.basename(args.input.rstrip('/'))
            print(f"Converting directory {args.input} to {args.output}/ (simpleo3 mode)")
            total = convert_directory_simpleo3(args.input, args.output, benchmark, args.max_lines)
            print(f"Total: {total} lines across all domain files")
        else:
            print(f"Converting {args.input} to {args.output} (simpleo3 mode)")
            total = convert_simpleo3(args.input, args.output, args.max_lines)
            print(f"Total: {total} lines")


if __name__ == "__main__":
    main()
