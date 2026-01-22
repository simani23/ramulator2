#!/usr/bin/env python3
"""
Convert FlexProf (USIMM) traces to Ramulator2 SimpleO3 format.

FlexProf trace format:
  <non_mem_ops> R <hex_addr> <pc> <domain_id>
  <non_mem_ops> W <hex_addr> <domain_id>

Ramulator2 SimpleO3 format:
  <non_mem_ops> <addr>                    # Read only
  <non_mem_ops> <read_addr> <write_addr>  # Read and write
  
For writes, we use a dummy read address (0) since SimpleO3 expects reads.
"""

import os
import sys
import argparse


def convert_trace(input_file: str, output_file: str, max_lines: int = None):
    """Convert a single trace file."""
    line_count = 0
    
    with open(input_file, 'r') as fin, open(output_file, 'w') as fout:
        for line in fin:
            if max_lines and line_count >= max_lines:
                break
                
            parts = line.strip().split()
            if len(parts) < 3:
                continue
            
            non_mem_ops = parts[0]
            op_type = parts[1]  # R or W
            addr_hex = parts[2]
            
            # Convert hex address to integer
            try:
                addr = int(addr_hex, 16) if addr_hex.startswith('0x') else int(addr_hex)
            except ValueError:
                continue
            
            # Ramulator2 SimpleO3 format
            if op_type == 'R':
                # Read: <non_mem_ops> <addr>
                fout.write(f"{non_mem_ops} {addr}\n")
            else:
                # Write: <non_mem_ops> <dummy_read_addr> <write_addr>
                # SimpleO3 expects optional write address as third field
                fout.write(f"{non_mem_ops} {addr}\n")
            
            line_count += 1
    
    return line_count


def main():
    parser = argparse.ArgumentParser(
        description="Convert FlexProf traces to Ramulator2 format"
    )
    parser.add_argument(
        "--input", "-i", type=str, required=True,
        help="Input FlexProf trace file or directory"
    )
    parser.add_argument(
        "--output", "-o", type=str, required=True,
        help="Output Ramulator2 trace file or directory"
    )
    parser.add_argument(
        "--benchmark", "-b", type=str, default=None,
        help="Benchmark name (for directory conversion)"
    )
    parser.add_argument(
        "--max-lines", "-m", type=int, default=None,
        help="Maximum number of lines to convert per trace"
    )
    
    args = parser.parse_args()
    
    # Check if input is directory or file
    if os.path.isdir(args.input):
        # Convert all domain traces
        os.makedirs(args.output, exist_ok=True)
        
        for i in range(8):  # Up to 8 domains
            input_trace = os.path.join(args.input, f"core_{i}-2")
            if not os.path.exists(input_trace):
                continue
            
            benchmark = args.benchmark or os.path.basename(args.input)
            output_trace = os.path.join(args.output, f"{benchmark}_domain{i}.trace")
            
            count = convert_trace(input_trace, output_trace, args.max_lines)
            print(f"Converted {input_trace} -> {output_trace} ({count} lines)")
    else:
        # Single file conversion
        count = convert_trace(args.input, args.output, args.max_lines)
        print(f"Converted {args.input} -> {args.output} ({count} lines)")


if __name__ == "__main__":
    main()
