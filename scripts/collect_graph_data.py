#!/usr/bin/env python3
"""
Simple Graph Data Collector

This script runs experiments and collects data for graphing.
It outputs CSV files that can be easily imported into graphing tools.

Usage:
    python3 collect_graph_data.py --config experiments.yaml --output data/
"""

import sys
import os
import json
import subprocess
import csv
import argparse
from pathlib import Path
from typing import Dict, List, Any, Tuple
import tempfile

def load_simple_config(config_file: str) -> Dict[str, Any]:
    """Load a simplified experiment configuration"""
    # For now, we'll use a simple Python format instead of YAML
    # This avoids the external dependency
    
    config = {
        "experiments": [
            {
                "name": "throughput_vs_threads",
                "series": [
                    {
                        "name": "faunus_uniform",
                        "base_config": "config/single_thread.yaml",
                        "vary_param": "threads",
                        "param_values": [1, 2, 4, 8],
                        "param_configs": [
                            {"num_cs": 1, "threads_per_cs": 1},
                            {"num_cs": 1, "threads_per_cs": 2}, 
                            {"num_cs": 2, "threads_per_cs": 2},
                            {"num_cs": 2, "threads_per_cs": 4}
                        ]
                    },
                    {
                        "name": "faunus_skewed",
                        "base_config": "config/single_thread.yaml",
                        "vary_param": "threads",
                        "param_values": [1, 2, 4, 8],
                        "param_configs": [
                            {"num_cs": 1, "threads_per_cs": 1, "distribution": {"type": "skewed"}},
                            {"num_cs": 1, "threads_per_cs": 2, "distribution": {"type": "skewed"}}, 
                            {"num_cs": 2, "threads_per_cs": 2, "distribution": {"type": "skewed"}},
                            {"num_cs": 2, "threads_per_cs": 4, "distribution": {"type": "skewed"}}
                        ]
                    }
                ]
            },
            {
                "name": "throughput_vs_maintenance", 
                "series": [
                    {
                        "name": "faunus_maintenance",
                        "base_config": "config/single_thread.yaml",
                        "vary_param": "maintenance_threads",
                        "param_values": [0, 1, 2, 4],
                        "param_configs": [
                            {"maintenance_cs": 0, "threads_per_maintenance_cs": 0},
                            {"maintenance_cs": 1, "threads_per_maintenance_cs": 1},
                            {"maintenance_cs": 1, "threads_per_maintenance_cs": 2},
                            {"maintenance_cs": 2, "threads_per_maintenance_cs": 2}
                        ]
                    }
                ]
            }
        ]
    }
    
    return config

class DataCollector:
    def __init__(self, output_dir: str = "graph_data"):
        self.output_dir = Path(output_dir)
        self.output_dir.mkdir(exist_ok=True)
        
    def run_single_experiment(self, base_config: str, parameters: Dict[str, Any], runs: int = 3) -> Dict[str, Any]:
        """Run a single experiment and return results"""
        
        # Create temporary config with parameters
        temp_config = self._create_temp_config(base_config, parameters)
        
        try:
            print(f"Running experiment with parameters: {parameters}")
            
            # Run performance analysis
            cmd = ["python3", "run_performance_analysis.py", temp_config, str(runs)]
            result = subprocess.run(cmd, capture_output=True, text=True, timeout=1800)
            
            if result.returncode != 0:
                print(f"Experiment failed: {result.stderr}")
                return None
            
            # Read results
            summary_file = Path("thread_stats/summary.json")
            if summary_file.exists():
                with open(summary_file, 'r') as f:
                    return json.load(f)
            
            return None
            
        except Exception as e:
            print(f"Error running experiment: {e}")
            return None
        finally:
            if os.path.exists(temp_config):
                os.remove(temp_config)
    
    def _create_temp_config(self, base_config: str, parameters: Dict[str, Any]) -> str:
        """Create temporary config file with parameter overrides"""
        
        # Read base config as text and do simple replacements
        # This is a simplified approach that works for basic cases
        with open(base_config, 'r') as f:
            config_text = f.read()
        
        # Simple parameter replacement
        for key, value in parameters.items():
            if key == "num_cs":
                config_text = config_text.replace(f"num_cs: 2", f"num_cs: {value}")
            elif key == "threads_per_cs":
                config_text = config_text.replace(f"threads_per_cs: 2", f"threads_per_cs: {value}")
            elif key == "maintenance_cs":
                config_text = config_text.replace(f"maintenance_cs: 1", f"maintenance_cs: {value}")
            elif key == "threads_per_maintenance_cs":
                config_text = config_text.replace(f"threads_per_maintenance_cs: 1", f"threads_per_maintenance_cs: {value}")
        
        # Handle nested parameters like distribution type
        if "distribution" in parameters:
            dist_type = parameters["distribution"]["type"]
            config_text = config_text.replace("type: skewed", f"type: {dist_type}")
        
        # Write to temporary file
        temp_fd, temp_path = tempfile.mkstemp(suffix='.yaml', text=True)
        with os.fdopen(temp_fd, 'w') as f:
            f.write(config_text)
        
        return temp_path
    
    def extract_key_metrics(self, data: Dict[str, Any]) -> Dict[str, float]:
        """Extract key metrics from experiment results"""
        metrics = {}
        
        # Throughput
        throughput_data = data.get('throughput', {})
        if throughput_data:
            metrics['throughput_kops'] = throughput_data.get('succeeded_ops_per_sec', 0.0) / 1000
        
        # Latency (focus on insert for now)
        latency_data = data.get('per_operation_latency', {})
        insert_data = latency_data.get('insert', {})
        if insert_data and insert_data.get('sample_count', 0) > 0:
            metrics['p50_latency_us'] = insert_data.get('p50_latency_us', 0.0)
            metrics['p99_latency_us'] = insert_data.get('p99_latency_us', 0.0)
            metrics['avg_latency_us'] = insert_data.get('avg_latency_us', 0.0)
        
        # Operation counts
        metrics['total_ops'] = data.get('total_succeeded', 0)
        
        # RDMA stats
        rdma_data = data.get('rdma', {})
        if rdma_data:
            metrics['total_rtt_ms'] = rdma_data.get('total_rtt_ms', 0.0)
            op_counts = rdma_data.get('op_counts', [])
            if len(op_counts) >= 4:
                metrics['rdma_reads'] = op_counts[0]
                metrics['rdma_writes'] = op_counts[1] 
                metrics['rdma_cas'] = op_counts[2]
                metrics['rdma_faa'] = op_counts[3]
        
        # Cache stats  
        cache_data = data.get('cache', {})
        if cache_data:
            metrics['cache_hit_rate'] = cache_data.get('total_hit_rate', 0.0)
        
        return metrics
    
    def collect_series_data(self, experiment_config: Dict[str, Any], runs: int = 3):
        """Collect data for all series in an experiment"""
        
        for experiment in experiment_config["experiments"]:
            exp_name = experiment["name"]
            print(f"\n=== Collecting data for {exp_name} ===")
            
            csv_file = self.output_dir / f"{exp_name}.csv"
            
            with open(csv_file, 'w', newline='') as f:
                writer = None
                
                for series in experiment["series"]:
                    series_name = series["name"]
                    print(f"Processing series: {series_name}")
                    
                    param_values = series["param_values"]
                    param_configs = series["param_configs"]
                    
                    for i, (param_value, param_config) in enumerate(zip(param_values, param_configs)):
                        print(f"  Running {series_name} with {series['vary_param']}={param_value}")
                        
                        result = self.run_single_experiment(
                            series["base_config"], 
                            param_config, 
                            runs
                        )
                        
                        if result:
                            metrics = self.extract_key_metrics(result)
                            
                            # Add metadata
                            row_data = {
                                'series': series_name,
                                'param_name': series['vary_param'],
                                'param_value': param_value,
                                **metrics
                            }
                            
                            # Write CSV header on first row
                            if writer is None:
                                writer = csv.DictWriter(f, fieldnames=row_data.keys())
                                writer.writeheader()
                            
                            writer.writerow(row_data)
                            f.flush()  # Ensure data is written immediately
                            
                            print(f"    → Throughput: {metrics.get('throughput_kops', 0):.1f} Kops/sec, "
                                  f"P50: {metrics.get('p50_latency_us', 0):.1f}μs")
                        else:
                            print(f"    → Experiment failed")
            
            print(f"Data saved to: {csv_file}")
    
    def generate_simple_plots(self):
        """Generate simple text-based plots for verification"""
        
        print("\n=== Simple Plot Generation ===")
        
        for csv_file in self.output_dir.glob("*.csv"):
            print(f"\nData from {csv_file.name}:")
            
            with open(csv_file, 'r') as f:
                reader = csv.DictReader(f)
                
                # Group by series
                series_data = {}
                for row in reader:
                    series = row['series']
                    if series not in series_data:
                        series_data[series] = []
                    series_data[series].append(row)
                
                # Print simple table
                for series, data in series_data.items():
                    print(f"\n  {series}:")
                    print(f"    {'Param':<10} {'Throughput':<12} {'P50 Lat':<10} {'P99 Lat':<10}")
                    print(f"    {'-'*10} {'-'*12} {'-'*10} {'-'*10}")
                    
                    for row in data:
                        param_val = row['param_value']
                        throughput = float(row.get('throughput_kops', 0))
                        p50 = float(row.get('p50_latency_us', 0))
                        p99 = float(row.get('p99_latency_us', 0))
                        
                        print(f"    {param_val:<10} {throughput:<12.1f} {p50:<10.1f} {p99:<10.1f}")

def main():
    parser = argparse.ArgumentParser(description="Collect performance data for graphing")
    
    parser.add_argument('--config', help='Experiment configuration file (not used yet)')
    parser.add_argument('--output', default='graph_data', help='Output directory for CSV files')
    parser.add_argument('--runs', type=int, default=3, help='Number of runs per experiment')
    parser.add_argument('--simple-plots', action='store_true', help='Generate simple text plots')
    
    args = parser.parse_args()
    
    collector = DataCollector(args.output)
    
    # Load hardcoded config for now
    config = load_simple_config(args.config)
    
    # Collect all data
    collector.collect_series_data(config, args.runs)
    
    if args.simple_plots:
        collector.generate_simple_plots()
    
    print(f"\nAll data collected in: {args.output}")
    print("You can now import the CSV files into your preferred graphing tool.")

if __name__ == "__main__":
    main()