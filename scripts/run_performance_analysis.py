#!/usr/bin/env python3
"""
Performance Analysis Wrapper Script

This script runs the KV-index benchmark multiple times with a given configuration
and collects performance statistics including throughput and latency percentiles.

Usage:
    python3 run_performance_analysis.py <config_file> <num_runs> [output_file]

Example:
    python3 run_performance_analysis.py config/single_thread.yaml 5
    python3 run_performance_analysis.py config/single_thread.yaml 10 results.txt
"""

import sys
import os
import json
import subprocess
import statistics
import argparse
from pathlib import Path
from typing import Dict, List, Any
import time


class PerformanceAnalyzer:
    def __init__(self, config_file: str, num_runs: int, output_file: str = None):
        self.config_file = config_file
        self.num_runs = num_runs
        self.output_file = output_file
        self.results = []
        
        # Check if config file exists
        if not os.path.exists(config_file):
            raise FileNotFoundError(f"Config file not found: {config_file}")
        
        # Check if kv_test binary exists
        if not os.path.exists("./kv_test"):
            raise FileNotFoundError("kv_test binary not found. Please run 'make kv_test' first.")
    
    def run_single_experiment(self, run_number: int) -> Dict[str, Any]:
        """Run a single experiment and collect results"""
        print(f"Running experiment {run_number + 1}/{self.num_runs}...")
        
        # Run the kv_test binary
        try:
            result = subprocess.run(
                ["./kv_test", self.config_file],
                capture_output=True,
                text=True,
                timeout=300  # 5 minute timeout
            )
            
            if result.returncode != 0:
                print(f"Warning: Run {run_number + 1} failed with return code {result.returncode}")
                print(f"stderr: {result.stderr}")
                return None
        
        except subprocess.TimeoutExpired:
            print(f"Warning: Run {run_number + 1} timed out")
            return None
        except Exception as e:
            print(f"Warning: Run {run_number + 1} failed with exception: {e}")
            return None
        
        # Parse the summary.json file
        summary_file = Path("thread_stats/summary.json")
        if not summary_file.exists():
            print(f"Warning: Summary file not found for run {run_number + 1}")
            return None
        
        try:
            with open(summary_file, 'r') as f:
                summary_data = json.load(f)
            return summary_data
        except Exception as e:
            print(f"Warning: Failed to parse summary file for run {run_number + 1}: {e}")
            return None
    
    def extract_metrics(self, summary_data: Dict[str, Any]) -> Dict[str, float]:
        """Extract key performance metrics from summary data"""
        metrics = {}
        
        # Extract overall operation counts
        metrics['total_attempted'] = summary_data.get('total_attempted', 0)
        metrics['total_succeeded'] = summary_data.get('total_succeeded', 0)
        metrics['elapsed_sec'] = summary_data.get('elapsed_sec', 0.0)
        
        # Extract throughput metrics from the correct location
        throughput_data = summary_data.get('throughput', {})
        if throughput_data:
            metrics['attempted_throughput'] = throughput_data.get('attempted_ops_per_sec', 0.0)
            metrics['succeeded_throughput'] = throughput_data.get('succeeded_ops_per_sec', 0.0)
            
            # Extract per-operation throughput
            per_op_throughput = throughput_data.get('per_operation', {})
            for op_name, op_data in per_op_throughput.items():
                if isinstance(op_data, dict):
                    metrics[f'{op_name}_attempted_throughput'] = op_data.get('attempted_ops_per_sec', 0.0)
                    metrics[f'{op_name}_succeeded_throughput'] = op_data.get('succeeded_ops_per_sec', 0.0)
        
        # Extract per-client throughput if available
        per_client_data = summary_data.get('per_client_throughput', {})
        if per_client_data:
            metrics['sum_attempted_throughput'] = per_client_data.get('sum_attempted_ops_per_sec', 0.0)
            metrics['sum_succeeded_throughput'] = per_client_data.get('sum_succeeded_ops_per_sec', 0.0)
        
        # Extract latency metrics from per_operation_latency
        latency_data = summary_data.get('per_operation_latency', {})
        for op_name, op_data in latency_data.items():
            if isinstance(op_data, dict) and op_data.get('sample_count', 0) > 0:
                # Extract percentiles
                if op_data.get('p50_latency_us') is not None:
                    metrics[f'{op_name}_p50_us'] = op_data['p50_latency_us']
                if op_data.get('p95_latency_us') is not None:
                    metrics[f'{op_name}_p95_us'] = op_data['p95_latency_us']
                if op_data.get('p99_latency_us') is not None:
                    metrics[f'{op_name}_p99_us'] = op_data['p99_latency_us']
                if op_data.get('avg_latency_us') is not None:
                    metrics[f'{op_name}_avg_us'] = op_data['avg_latency_us']
                if op_data.get('min_latency_us') is not None:
                    metrics[f'{op_name}_min_us'] = op_data['min_latency_us']
                if op_data.get('max_latency_us') is not None:
                    metrics[f'{op_name}_max_us'] = op_data['max_latency_us']
        
        # Extract operation counts
        op_counts = summary_data.get('operation_counts', {})
        for op_name, count in op_counts.items():
            metrics[f'{op_name}_count'] = count
        
        # Extract cache statistics
        cache_data = summary_data.get('cache', {})
        if cache_data:
            metrics['cache_hit_rate'] = cache_data.get('total_hit_rate', 0.0)
            metrics['cache_hits'] = cache_data.get('total_hits', 0)
            metrics['cache_misses'] = cache_data.get('total_misses', 0)
            metrics['cache_entries'] = cache_data.get('total_entries', 0)
            metrics['cache_evictions'] = cache_data.get('total_evictions', 0)
        
        # Extract RDMA statistics
        rdma_data = summary_data.get('rdma', {})
        if rdma_data:
            metrics['total_rtt_ms'] = rdma_data.get('total_rtt_ms', 0.0)
            op_counts = rdma_data.get('op_counts', [])
            if len(op_counts) >= 4:
                metrics['rdma_read_ops'] = op_counts[0]
                metrics['rdma_write_ops'] = op_counts[1]
                metrics['rdma_cas_ops'] = op_counts[2]
                metrics['rdma_faa_ops'] = op_counts[3]
        
        # Extract enhanced per-operation RDMA metrics
        per_op_rdma = summary_data.get('per_operation_rdma_metrics', {})
        for op_name, op_data in per_op_rdma.items():
            if isinstance(op_data, dict):
                # RTT distribution
                rtt_dist = op_data.get('rtt_distribution', {})
                if rtt_dist:
                    metrics[f'{op_name}_avg_rtts_per_op'] = rtt_dist.get('average', 0.0)
                    metrics[f'{op_name}_total_rtt_samples'] = rtt_dist.get('total_samples', 0)
                
                # RDMA operations per B+Tree operation
                rdma_ops = op_data.get('rdma_ops_per_operation', {})
                for rdma_type, rdma_stats in rdma_ops.items():
                    if isinstance(rdma_stats, dict):
                        metrics[f'{op_name}_avg_{rdma_type.lower()}_per_op'] = rdma_stats.get('average', 0.0)
                        metrics[f'{op_name}_total_{rdma_type.lower()}_ops'] = rdma_stats.get('total_samples', 0)
                
                # Bytes transferred
                bytes_data = op_data.get('bytes_transferred', {})
                if bytes_data:
                    metrics[f'{op_name}_avg_bytes_read'] = bytes_data.get('read_avg_bytes', 0.0)
                    metrics[f'{op_name}_total_bytes_read'] = bytes_data.get('read_total_bytes', 0)
                    metrics[f'{op_name}_avg_bytes_written'] = bytes_data.get('written_avg_bytes', 0.0)
                    metrics[f'{op_name}_total_bytes_written'] = bytes_data.get('written_total_bytes', 0)
                
                # Retry distribution
                retry_dist = op_data.get('retry_distribution', {})
                if retry_dist:
                    metrics[f'{op_name}_avg_retries_per_op'] = retry_dist.get('average', 0.0)
                    metrics[f'{op_name}_total_retry_samples'] = retry_dist.get('total_samples', 0)
            
        return metrics
    
    def run_experiments(self):
        """Run all experiments and collect results"""
        print(f"Starting {self.num_runs} runs with config: {self.config_file}")
        print("=" * 60)
        
        for i in range(self.num_runs):
            summary_data = self.run_single_experiment(i)
            if summary_data:
                metrics = self.extract_metrics(summary_data)
                self.results.append(metrics)
            
            # Small delay between runs to avoid resource conflicts
            if i < self.num_runs - 1:
                time.sleep(1)
        
        print(f"Completed {len(self.results)}/{self.num_runs} successful runs")
    
    def calculate_statistics(self) -> Dict[str, Dict[str, float]]:
        """Calculate statistics for all collected metrics"""
        if not self.results:
            return {}
        
        # Get all metric names
        all_metrics = set()
        for result in self.results:
            all_metrics.update(result.keys())
        
        stats = {}
        for metric in all_metrics:
            values = [result[metric] for result in self.results if metric in result]
            if values:
                stats[metric] = {
                    'mean': statistics.mean(values),
                    'median': statistics.median(values),
                    'stdev': statistics.stdev(values) if len(values) > 1 else 0.0,
                    'min': min(values),
                    'max': max(values),
                    'count': len(values)
                }
        
        return stats
    
    def format_output(self, stats: Dict[str, Dict[str, float]]) -> str:
        """Format the statistics into a readable report"""
        if not stats:
            return "No valid results collected.\n"
        
        report = []
        report.append(f"Performance Analysis Report")
        report.append(f"Config file: {self.config_file}")
        report.append(f"Successful runs: {len(self.results)}/{self.num_runs}")
        report.append("=" * 80)
        
        # Group metrics by category
        throughput_metrics = {k: v for k, v in stats.items() if 'throughput' in k}
        latency_metrics = {k: v for k, v in stats.items() if any(x in k for x in ['_p50_', '_p95_', '_p99_', '_avg_', '_min_', '_max_']) and '_us' in k}
        cache_metrics = {k: v for k, v in stats.items() if 'cache_' in k}
        rdma_metrics = {k: v for k, v in stats.items() if k.startswith('rdma_') or 'rtt' in k}
        count_metrics = {k: v for k, v in stats.items() if k.endswith('_count') or k in ['total_attempted', 'total_succeeded']}
        other_metrics = {k: v for k, v in stats.items() if k not in throughput_metrics and k not in latency_metrics and k not in cache_metrics and k not in rdma_metrics and k not in count_metrics}
        
        # Format throughput section (filter out zero values for cleaner output)
        if throughput_metrics:
            non_zero_throughput = {k: v for k, v in throughput_metrics.items() if v['mean'] > 0.0}
            if non_zero_throughput:
                report.append("\nTHROUGHPUT METRICS (ops/sec)")
                report.append("-" * 40)
                for metric, stat in non_zero_throughput.items():
                    report.append(f"{metric:25s}: {stat['mean']:>10.2f} ± {stat['stdev']:>8.2f} (min: {stat['min']:>8.2f}, max: {stat['max']:>8.2f})")
        
        # Format latency section
        if latency_metrics:
            report.append("\nLATENCY METRICS (microseconds)")
            report.append("-" * 40)
            for metric, stat in latency_metrics.items():
                report.append(f"{metric:25s}: {stat['mean']:>10.2f} ± {stat['stdev']:>8.2f} (min: {stat['min']:>8.2f}, max: {stat['max']:>8.2f})")
        
        # Format cache section
        if cache_metrics:
            report.append("\nCACHE METRICS")
            report.append("-" * 40)
            for metric, stat in cache_metrics.items():
                if 'hit_rate' in metric:
                    report.append(f"{metric:25s}: {stat['mean']:>10.4f} ± {stat['stdev']:>8.4f} (min: {stat['min']:>8.4f}, max: {stat['max']:>8.4f})")
                else:
                    report.append(f"{metric:25s}: {stat['mean']:>10.0f} ± {stat['stdev']:>8.0f} (min: {stat['min']:>8.0f}, max: {stat['max']:>8.0f})")
        
        # Format operation counts section
        if count_metrics:
            report.append("\nOPERATION COUNTS")
            report.append("-" * 40)
            for metric, stat in count_metrics.items():
                report.append(f"{metric:25s}: {stat['mean']:>10.0f} ± {stat['stdev']:>8.0f} (min: {stat['min']:>8.0f}, max: {stat['max']:>8.0f})")

        # Format RDMA metrics section
        if rdma_metrics:
            report.append("\nRDMA METRICS")
            report.append("-" * 40)
            for metric, stat in rdma_metrics.items():
                if 'rtt' in metric:
                    report.append(f"{metric:25s}: {stat['mean']:>10.3f} ± {stat['stdev']:>8.3f} (min: {stat['min']:>8.3f}, max: {stat['max']:>8.3f}) ms")
                else:
                    report.append(f"{metric:25s}: {stat['mean']:>10.0f} ± {stat['stdev']:>8.0f} (min: {stat['min']:>8.0f}, max: {stat['max']:>8.0f})")
        
        # Format other metrics
        if other_metrics:
            report.append("\nOTHER METRICS")
            report.append("-" * 40)
            for metric, stat in other_metrics.items():
                if 'elapsed' in metric:
                    report.append(f"{metric:25s}: {stat['mean']:>10.3f} ± {stat['stdev']:>8.3f} (min: {stat['min']:>8.3f}, max: {stat['max']:>8.3f}) sec")
                else:
                    report.append(f"{metric:25s}: {stat['mean']:>10.2f} ± {stat['stdev']:>8.2f} (min: {stat['min']:>8.2f}, max: {stat['max']:>8.2f})")
        
        return "\n".join(report) + "\n"
    
    def run_analysis(self):
        """Run the complete performance analysis"""
        self.run_experiments()
        stats = self.calculate_statistics()
        report = self.format_output(stats)
        
        # Print to console
        print("\n" + "=" * 80)
        print(report)
        
        # Save to file if specified
        if self.output_file:
            with open(self.output_file, 'w') as f:
                f.write(report)
            print(f"Results saved to: {self.output_file}")


def main():
    parser = argparse.ArgumentParser(
        description="Run KV-index performance analysis with multiple iterations",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
Examples:
  python3 run_performance_analysis.py config/single_thread.yaml 5
  python3 run_performance_analysis.py config/single_thread.yaml 10 results.txt
        """
    )
    
    parser.add_argument('config_file', help='Path to the YAML configuration file')
    parser.add_argument('num_runs', type=int, help='Number of experiment runs')
    parser.add_argument('output_file', nargs='?', help='Optional output file for results')
    
    args = parser.parse_args()
    
    if args.num_runs < 1:
        print("Error: Number of runs must be at least 1")
        sys.exit(1)
    
    try:
        analyzer = PerformanceAnalyzer(args.config_file, args.num_runs, args.output_file)
        analyzer.run_analysis()
    except Exception as e:
        print(f"Error: {e}")
        sys.exit(1)


if __name__ == "__main__":
    main()