#!/usr/bin/env python3
"""
DRAM Test Results Analyzer for Ramulator2

This script parses and compares test results from different configurations
(baseline, static, dynamic) and displays key DRAM performance metrics.
"""

import os
import re
import sys
import argparse
from collections import defaultdict
from pathlib import Path
from typing import Dict, List, Tuple, Optional
import statistics


class ResultParser:
    """Parse Ramulator2 result files."""
    
    def __init__(self, filepath: str):
        self.filepath = filepath
        self.filename = os.path.basename(filepath)
        self.metrics = {}
        self.parse()
    
    def parse(self):
        """Parse the result file and extract metrics."""
        with open(self.filepath, 'r') as f:
            content = f.read()
        
        # Parse key-value metrics using regex
        # Pattern matches lines like "  metric_name: value"
        pattern = r'^\s*([a-zA-Z_][a-zA-Z0-9_]*(?:_\d+)?)\s*:\s*([0-9.]+)\s*$'
        
        for match in re.finditer(pattern, content, re.MULTILINE):
            key = match.group(1)
            value_str = match.group(2)
            try:
                # Try to parse as int first, then float
                if '.' in value_str:
                    value = float(value_str)
                else:
                    value = int(value_str)
                self.metrics[key] = value
            except ValueError:
                pass
        
        # Extract trace info (reads/writes from trace)
        trace_pattern = r'Loaded (\d+) entries across (\d+) domains'
        match = re.search(trace_pattern, content)
        if match:
            self.metrics['trace_total_entries'] = int(match.group(1))
            self.metrics['trace_num_domains'] = int(match.group(2))
        
        # Extract controller type
        impl_pattern = r'Controller:\s*\n\s*impl:\s*(\w+)'
        match = re.search(impl_pattern, content)
        if match:
            self.metrics['controller_impl'] = match.group(1)
    
    def get_workload_and_config(self) -> Tuple[str, str, str]:
        """Extract workload name and configuration from filename."""
        # Pattern: {workload}_{config}_{timestamp}.txt
        name = self.filename.replace('.txt', '')
        parts = name.rsplit('_', 2)  # Split from right to get timestamp parts
        
        if len(parts) >= 3:
            # Remove timestamp (last two parts: date and time)
            config_and_workload = '_'.join(parts[:-2])
            timestamp = '_'.join(parts[-2:])
        else:
            config_and_workload = name
            timestamp = ''
        
        # Determine config type
        if '_baseline_' in name or name.endswith('_baseline'):
            config = 'baseline'
            workload = config_and_workload.replace('_baseline', '')
        elif '_static_' in name or name.endswith('_static'):
            config = 'static'
            workload = config_and_workload.replace('_static', '')
        elif '_dynamic_' in name or name.endswith('_dynamic'):
            config = 'dynamic'
            workload = config_and_workload.replace('_dynamic', '')
        else:
            config = 'unknown'
            workload = config_and_workload
        
        return workload, config, timestamp


class MetricsComparator:
    """Compare metrics across different configurations."""
    
    # Key DRAM metrics to display
    KEY_METRICS = [
        # Memory System Level
        ('total_num_read_requests', 'Total Read Requests'),
        ('total_num_write_requests', 'Total Write Requests'),
        ('memory_system_cycles', 'Memory System Cycles'),
        
        # Latency
        ('avg_read_latency_0', 'Avg Read Latency'),
        ('read_latency_0', 'Total Read Latency'),
        
        # Queue Stats
        ('queue_len_avg_0', 'Avg Queue Length'),
        ('read_queue_len_avg_0', 'Avg Read Queue Length'),
        ('write_queue_len_avg_0', 'Avg Write Queue Length'),
        
        # Row Buffer Stats
        ('row_hits_0', 'Row Hits'),
        ('row_misses_0', 'Row Misses'),
        ('row_conflicts_0', 'Row Conflicts'),
        ('read_row_hits_0', 'Read Row Hits'),
        ('read_row_misses_0', 'Read Row Misses'),
        ('read_row_conflicts_0', 'Read Row Conflicts'),
        ('write_row_hits_0', 'Write Row Hits'),
        ('write_row_misses_0', 'Write Row Misses'),
        ('write_row_conflicts_0', 'Write Row Conflicts'),
    ]
    
    FLEXPROF_METRICS = [
        ('flexprof_dyn_total_turns_0', 'Total Turns'),
        ('flexprof_dyn_read_turns_0', 'Read Turns'),
        ('flexprof_dyn_write_turns_0', 'Write Turns'),
        ('flexprof_dyn_idle_turns_0', 'Idle Turns'),
        ('flexprof_dyn_adaptation_events_0', 'Adaptation Events'),
        ('flexprof_static_total_turns_0', 'Total Turns (Static)'),
        ('flexprof_static_read_turns_0', 'Read Turns (Static)'),
        ('flexprof_static_write_turns_0', 'Write Turns (Static)'),
        ('flexprof_static_idle_turns_0', 'Idle Turns (Static)'),
    ]
    
    def __init__(self, results_dir: str, timestamp_filter: Optional[str] = None):
        self.results_dir = results_dir
        self.timestamp_filter = timestamp_filter
        self.results = defaultdict(dict)  # workload -> config -> ResultParser
        self.load_results()
    
    def load_results(self):
        """Load all result files from the directory."""
        results_path = Path(self.results_dir)
        
        for file_path in results_path.glob('*.txt'):
            parser = ResultParser(str(file_path))
            workload, config, timestamp = parser.get_workload_and_config()
            
            # Apply timestamp filter if specified
            if self.timestamp_filter and self.timestamp_filter not in timestamp:
                continue
            
            # Store result (if multiple timestamps, keep the latest)
            key = (workload, config)
            if key not in self.results[workload] or timestamp > self.results[workload][config][1]:
                self.results[workload][config] = (parser, timestamp)
    
    def get_available_workloads(self) -> List[str]:
        """Get list of available workloads."""
        return sorted(self.results.keys())
    
    def get_configs_for_workload(self, workload: str) -> List[str]:
        """Get available configurations for a workload."""
        return sorted(self.results[workload].keys())
    
    def compute_derived_metrics(self, metrics: Dict) -> Dict:
        """Compute derived metrics like IPC, hit rate, etc."""
        derived = {}
        
        # Row buffer hit rate
        hits = metrics.get('row_hits_0', 0)
        misses = metrics.get('row_misses_0', 0)
        conflicts = metrics.get('row_conflicts_0', 0)
        total_accesses = hits + misses + conflicts
        if total_accesses > 0:
            derived['row_buffer_hit_rate'] = hits / total_accesses * 100
            derived['row_buffer_miss_rate'] = misses / total_accesses * 100
            derived['row_buffer_conflict_rate'] = conflicts / total_accesses * 100
        
        # Bandwidth utilization (requests per cycle)
        cycles = metrics.get('memory_system_cycles', 0)
        total_reqs = metrics.get('total_num_read_requests', 0) + metrics.get('total_num_write_requests', 0)
        if cycles > 0:
            derived['requests_per_cycle'] = total_reqs / cycles
            derived['bandwidth_util'] = total_reqs / cycles * 100
        
        # Read/Write ratio
        reads = metrics.get('total_num_read_requests', 0)
        writes = metrics.get('total_num_write_requests', 0)
        if writes > 0:
            derived['read_write_ratio'] = reads / writes
        elif reads > 0:
            derived['read_write_ratio'] = float('inf')
        
        # Average queue occupancy per request
        if total_reqs > 0:
            queue_len = metrics.get('queue_len_avg_0', 0)
            derived['avg_queue_occupancy'] = queue_len
        
        return derived
    
    def print_comparison_table(self, workload: str):
        """Print a comparison table for a specific workload."""
        configs = self.get_configs_for_workload(workload)
        if not configs:
            print(f"No results found for workload: {workload}")
            return
        
        print(f"\n{'='*80}")
        print(f"Workload: {workload}")
        print(f"{'='*80}")
        
        # Header
        header = f"{'Metric':<35}"
        for config in ['baseline', 'static', 'dynamic']:
            if config in configs:
                header += f"{config:>14}"
        print(header)
        print('-' * 80)
        
        # Get metrics for each config
        config_metrics = {}
        for config in configs:
            parser, _ = self.results[workload][config]
            config_metrics[config] = parser.metrics
        
        # Print key metrics
        for metric_key, metric_name in self.KEY_METRICS:
            row = f"{metric_name:<35}"
            has_value = False
            for config in ['baseline', 'static', 'dynamic']:
                if config in configs:
                    value = config_metrics[config].get(metric_key, 'N/A')
                    if value != 'N/A':
                        has_value = True
                        if isinstance(value, float):
                            row += f"{value:>14.2f}"
                        else:
                            row += f"{value:>14}"
                    else:
                        row += f"{'N/A':>14}"
            if has_value:
                print(row)
        
        # Print derived metrics
        print('-' * 80)
        print("Derived Metrics:")
        
        derived_metrics = {}
        for config in configs:
            derived_metrics[config] = self.compute_derived_metrics(config_metrics[config])
        
        derived_names = [
            ('row_buffer_hit_rate', 'Row Buffer Hit Rate (%)'),
            ('row_buffer_miss_rate', 'Row Buffer Miss Rate (%)'),
            ('row_buffer_conflict_rate', 'Row Buffer Conflict Rate (%)'),
            ('requests_per_cycle', 'Requests/Cycle'),
            ('bandwidth_util', 'Bandwidth Utilization (%)'),
            ('read_write_ratio', 'Read/Write Ratio'),
        ]
        
        for key, name in derived_names:
            row = f"{name:<35}"
            for config in ['baseline', 'static', 'dynamic']:
                if config in configs:
                    value = derived_metrics[config].get(key, 'N/A')
                    if value != 'N/A':
                        if value == float('inf'):
                            row += f"{'inf':>14}"
                        else:
                            row += f"{value:>14.2f}"
                    else:
                        row += f"{'N/A':>14}"
            print(row)
        
        # Print FlexProf specific metrics if applicable
        has_flexprof = False
        for config in configs:
            if config in ['static', 'dynamic']:
                for metric_key, _ in self.FLEXPROF_METRICS:
                    if metric_key in config_metrics.get(config, {}):
                        has_flexprof = True
                        break
        
        if has_flexprof:
            print('-' * 80)
            print("FlexProf Metrics:")
            for metric_key, metric_name in self.FLEXPROF_METRICS:
                row = f"{metric_name:<35}"
                has_value = False
                for config in ['baseline', 'static', 'dynamic']:
                    if config in configs:
                        value = config_metrics[config].get(metric_key, 'N/A')
                        if value != 'N/A':
                            has_value = True
                            if isinstance(value, float):
                                row += f"{value:>14.2f}"
                            else:
                                row += f"{value:>14}"
                        else:
                            row += f"{'N/A':>14}"
                if has_value:
                    print(row)
        
        # Print performance comparison vs baseline
        if 'baseline' in configs and len(configs) > 1:
            print('-' * 80)
            print("Performance vs Baseline:")
            baseline_cycles = config_metrics['baseline'].get('memory_system_cycles', 0)
            baseline_latency = config_metrics['baseline'].get('avg_read_latency_0', 0)
            
            for config in ['static', 'dynamic']:
                if config in configs:
                    cycles = config_metrics[config].get('memory_system_cycles', 0)
                    latency = config_metrics[config].get('avg_read_latency_0', 0)
                    
                    if baseline_cycles > 0 and cycles > 0:
                        speedup = baseline_cycles / cycles
                        print(f"  {config}: Cycles Speedup = {speedup:.2f}x", end='')
                        if speedup > 1:
                            print(f" ({(speedup-1)*100:.1f}% faster)")
                        elif speedup < 1:
                            print(f" ({(1-speedup)*100:.1f}% slower)")
                        else:
                            print(" (same)")
                    
                    if baseline_latency > 0 and latency > 0:
                        lat_improvement = (baseline_latency - latency) / baseline_latency * 100
                        print(f"  {config}: Latency Change = {lat_improvement:+.1f}%")
    
    def print_summary_table(self):
        """Print a summary table across all workloads."""
        print("\n" + "="*100)
        print("SUMMARY: All Workloads Comparison")
        print("="*100)
        
        # Group workloads
        standard = []
        read_heavy = []
        write_heavy = []
        combined = []
        
        for workload in self.get_available_workloads():
            if 'all_domains' in workload:
                combined.append(workload)
            elif 'read_heavy' in workload:
                read_heavy.append(workload)
            elif 'write_heavy' in workload:
                write_heavy.append(workload)
            else:
                standard.append(workload)
        
        groups = [
            ("Standard Workloads", standard),
            ("Read-Heavy Workloads", read_heavy),
            ("Write-Heavy Workloads", write_heavy),
            ("Combined/Multi-Domain", combined),
        ]
        
        for group_name, workloads in groups:
            if not workloads:
                continue
            
            print(f"\n{group_name}:")
            print(f"{'Workload':<25} {'Config':<10} {'Cycles':>10} {'Avg Lat':>10} {'Hit Rate':>10} {'Req/Cyc':>10}")
            print("-" * 80)
            
            for workload in sorted(workloads):
                configs = self.get_configs_for_workload(workload)
                for config in ['baseline', 'static', 'dynamic']:
                    if config not in configs:
                        continue
                    
                    parser, _ = self.results[workload][config]
                    metrics = parser.metrics
                    derived = self.compute_derived_metrics(metrics)
                    
                    cycles = metrics.get('memory_system_cycles', 'N/A')
                    latency = metrics.get('avg_read_latency_0', 'N/A')
                    hit_rate = derived.get('row_buffer_hit_rate', 'N/A')
                    req_cyc = derived.get('requests_per_cycle', 'N/A')
                    
                    row = f"{workload:<25} {config:<10}"
                    row += f"{cycles:>10}" if cycles != 'N/A' else f"{'N/A':>10}"
                    row += f"{latency:>10.2f}" if isinstance(latency, (int, float)) else f"{'N/A':>10}"
                    row += f"{hit_rate:>10.1f}" if hit_rate != 'N/A' else f"{'N/A':>10}"
                    row += f"{req_cyc:>10.3f}" if req_cyc != 'N/A' else f"{'N/A':>10}"
                    print(row)
    
    def export_csv(self, output_file: str):
        """Export all metrics to a CSV file."""
        import csv
        
        # Collect all unique metric keys
        all_metrics = set()
        for workload in self.results:
            for config in self.results[workload]:
                parser, _ = self.results[workload][config]
                all_metrics.update(parser.metrics.keys())
        
        all_metrics = sorted(all_metrics)
        
        with open(output_file, 'w', newline='') as f:
            writer = csv.writer(f)
            
            # Header
            header = ['workload', 'config', 'timestamp'] + all_metrics
            writer.writerow(header)
            
            # Data rows
            for workload in sorted(self.results.keys()):
                for config in sorted(self.results[workload].keys()):
                    parser, timestamp = self.results[workload][config]
                    row = [workload, config, timestamp]
                    for metric in all_metrics:
                        row.append(parser.metrics.get(metric, ''))
                    writer.writerow(row)
        
        print(f"Results exported to: {output_file}")


def main():
    parser = argparse.ArgumentParser(
        description='Analyze and compare DRAM test results from Ramulator2',
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
Examples:
  %(prog)s                          # Show summary of all results
  %(prog)s -w domain0               # Show detailed comparison for domain0
  %(prog)s -w all                   # Show detailed comparison for all workloads
  %(prog)s --csv results.csv        # Export all metrics to CSV
  %(prog)s -t 20260128_010656       # Filter by timestamp
        """
    )
    
    parser.add_argument('-d', '--dir', default='test_results',
                        help='Directory containing result files (default: test_results)')
    parser.add_argument('-w', '--workload', default=None,
                        help='Specific workload to analyze (use "all" for all workloads)')
    parser.add_argument('-t', '--timestamp', default=None,
                        help='Filter results by timestamp substring')
    parser.add_argument('--csv', default=None,
                        help='Export results to CSV file')
    parser.add_argument('--list', action='store_true',
                        help='List available workloads and exit')
    
    args = parser.parse_args()
    
    # Initialize comparator
    comparator = MetricsComparator(args.dir, args.timestamp)
    
    if args.list:
        print("Available workloads:")
        for workload in comparator.get_available_workloads():
            configs = comparator.get_configs_for_workload(workload)
            print(f"  {workload}: {', '.join(configs)}")
        return
    
    if args.csv:
        comparator.export_csv(args.csv)
        return
    
    if args.workload:
        if args.workload.lower() == 'all':
            for workload in comparator.get_available_workloads():
                comparator.print_comparison_table(workload)
        else:
            comparator.print_comparison_table(args.workload)
    else:
        # Default: print summary
        comparator.print_summary_table()
        print("\n" + "-"*80)
        print("Use '-w <workload>' for detailed comparison of a specific workload.")
        print("Use '-w all' for detailed comparison of all workloads.")
        print("Use '--list' to see all available workloads.")


if __name__ == '__main__':
    main()
