#!/usr/bin/env python3
"""
Comprehensive Experiment Runner

This script executes complete experiment workflows based on configuration files:
1. Runs all experiments defined in experiments.yaml
2. Collects results and generates CSV files
3. Creates TikZ plots for publication
4. Generates performance analysis reports

Usage:
    python3 experiment_runner.py --config experiments.yaml
    python3 experiment_runner.py --config experiments.yaml --skip-graphs
    python3 experiment_runner.py --config experiments.yaml --output-dir custom_results/
"""

import sys
import os
import json
import yaml
import argparse
import subprocess
import shutil
import tempfile
import time
from pathlib import Path
from typing import Dict, List, Any, Optional, Tuple
from dataclasses import dataclass, field
from concurrent.futures import ThreadPoolExecutor, as_completed
import csv
import statistics


@dataclass
class ExperimentConfig:
    """Configuration for a single experiment"""
    name: str
    index_type: str
    config_file: str
    parameter_value: Any
    parameters: Dict[str, Any] = field(default_factory=dict)
    

@dataclass
class ExperimentResult:
    """Results from a single experiment"""
    config: ExperimentConfig
    metrics: Dict[str, float]
    success: bool
    error_message: Optional[str] = None


class ExperimentRunner:
    def __init__(self, experiments_config: str, output_dir: str = "experiment_results", 
                 num_runs_per_experiment: int = 3, parallel_experiments: int = 1):
        self.experiments_config = experiments_config
        self.output_dir = Path(output_dir)
        self.num_runs = num_runs_per_experiment
        self.parallel_experiments = parallel_experiments
        
        # Create output directories
        self.output_dir.mkdir(exist_ok=True)
        (self.output_dir / "csv").mkdir(exist_ok=True)
        (self.output_dir / "plots").mkdir(exist_ok=True)
        (self.output_dir / "reports").mkdir(exist_ok=True)
        (self.output_dir / "raw_data").mkdir(exist_ok=True)
        
        # Load experiment configuration
        with open(experiments_config, 'r') as f:
            self.config = yaml.safe_load(f)
        
        # Verify kv_test binary exists
        if not Path("./kv_test").exists():
            raise FileNotFoundError("kv_test binary not found. Please run 'make kv_test' first.")
    
    def modify_config_file(self, base_config: str, parameters: Dict[str, Any]) -> str:
        """Create a modified config file with experiment parameters"""
        # Load base config
        with open(base_config, 'r') as f:
            config_data = yaml.safe_load(f)
        
        # Apply parameter modifications
        for key_path, value in parameters.items():
            # Handle nested keys like "distribution.type"
            keys = key_path.split('.')
            current = config_data
            for key in keys[:-1]:
                if key not in current:
                    current[key] = {}
                current = current[key]
            current[keys[-1]] = value
        
        # Create temporary config file
        temp_file = tempfile.NamedTemporaryFile(mode='w', suffix='.yaml', delete=False)
        yaml.dump(config_data, temp_file, default_flow_style=False)
        temp_file.close()
        
        return temp_file.name
    
    def run_single_experiment(self, experiment: ExperimentConfig) -> ExperimentResult:
        """Run a single experiment with the given configuration"""
        print(f"Running experiment: {experiment.name}")
        
        try:
            # Create modified config file
            temp_config = self.modify_config_file(experiment.config_file, experiment.parameters)
            
            # Run the performance analysis script
            cmd = [
                sys.executable, "scripts/run_performance_analysis.py",
                temp_config, str(self.num_runs)
            ]
            
            result = subprocess.run(cmd, capture_output=True, text=True, timeout=600)
            
            # Clean up temp config
            os.unlink(temp_config)
            
            if result.returncode != 0:
                return ExperimentResult(
                    config=experiment,
                    metrics={},
                    success=False,
                    error_message=f"Command failed: {result.stderr}"
                )
            
            # Parse the latest summary.json file
            summary_file = Path("thread_stats/summary.json")
            if not summary_file.exists():
                return ExperimentResult(
                    config=experiment,
                    metrics={},
                    success=False,
                    error_message="Summary file not found"
                )
            
            with open(summary_file, 'r') as f:
                summary_data = json.load(f)
            
            # Extract metrics using the same logic as run_performance_analysis.py
            metrics = self.extract_metrics(summary_data)
            
            # Save raw data
            raw_data_file = self.output_dir / "raw_data" / f"{experiment.name}.json"
            with open(raw_data_file, 'w') as f:
                json.dump(summary_data, f, indent=2)
            
            return ExperimentResult(
                config=experiment,
                metrics=metrics,
                success=True
            )
            
        except Exception as e:
            return ExperimentResult(
                config=experiment,
                metrics={},
                success=False,
                error_message=str(e)
            )
    
    def extract_metrics(self, summary_data: Dict[str, Any]) -> Dict[str, float]:
        """Extract metrics from summary data (same logic as run_performance_analysis.py)"""
        metrics = {}
        
        # Basic metrics
        metrics['total_attempted'] = summary_data.get('total_attempted', 0)
        metrics['total_succeeded'] = summary_data.get('total_succeeded', 0)
        metrics['elapsed_sec'] = summary_data.get('elapsed_sec', 0.0)
        
        # Throughput metrics
        throughput_data = summary_data.get('throughput', {})
        if throughput_data:
            metrics['attempted_throughput'] = throughput_data.get('attempted_ops_per_sec', 0.0)
            metrics['succeeded_throughput'] = throughput_data.get('succeeded_ops_per_sec', 0.0)
        
        # Latency metrics
        latency_data = summary_data.get('per_operation_latency', {})
        for op_name, op_data in latency_data.items():
            if isinstance(op_data, dict) and op_data.get('sample_count', 0) > 0:
                if op_data.get('p50_latency_us') is not None:
                    metrics[f'{op_name}_p50_us'] = op_data['p50_latency_us']
                if op_data.get('p95_latency_us') is not None:
                    metrics[f'{op_name}_p95_us'] = op_data['p95_latency_us']
                if op_data.get('p99_latency_us') is not None:
                    metrics[f'{op_name}_p99_us'] = op_data['p99_latency_us']
        
        # RDMA metrics
        rdma_data = summary_data.get('rdma', {})
        if rdma_data:
            metrics['total_rtt_ms'] = rdma_data.get('total_rtt_ms', 0.0)
            op_counts = rdma_data.get('op_counts', [])
            if len(op_counts) >= 4:
                metrics['rdma_read_ops'] = op_counts[0]
                metrics['rdma_write_ops'] = op_counts[1]
                metrics['rdma_cas_ops'] = op_counts[2]
                metrics['rdma_faa_ops'] = op_counts[3]
        
        # Enhanced per-operation RDMA metrics
        per_op_rdma = summary_data.get('per_operation_rdma_metrics', {})
        for op_name, op_data in per_op_rdma.items():
            if isinstance(op_data, dict):
                # RTT distribution
                rtt_dist = op_data.get('rtt_distribution', {})
                if rtt_dist:
                    metrics[f'{op_name}_avg_rtts_per_op'] = rtt_dist.get('average', 0.0)
                
                # RDMA operations per B+Tree operation
                rdma_ops = op_data.get('rdma_ops_per_operation', {})
                for rdma_type, rdma_stats in rdma_ops.items():
                    if isinstance(rdma_stats, dict):
                        metrics[f'{op_name}_avg_{rdma_type.lower()}_per_op'] = rdma_stats.get('average', 0.0)
                
                # Bytes transferred
                bytes_data = op_data.get('bytes_transferred', {})
                if bytes_data:
                    metrics[f'{op_name}_avg_bytes_read'] = bytes_data.get('read_avg_bytes', 0.0)
                    metrics[f'{op_name}_avg_bytes_written'] = bytes_data.get('written_avg_bytes', 0.0)
        
        return metrics
    
    def run_experiment_series(self, graph_config: Dict[str, Any]) -> Dict[str, List[ExperimentResult]]:
        """Run all experiments for a single graph"""
        graph_type = graph_config['type']
        print(f"\n=== Running experiments for graph: {graph_config['title']} ===")
        
        all_results = {}
        
        for series_config in graph_config['series']:
            series_name = series_config['name']
            print(f"\nRunning series: {series_name}")
            
            experiments = []
            for exp_config in series_config['experiments']:
                experiment = ExperimentConfig(
                    name=exp_config['name'],
                    index_type=exp_config['index_type'],
                    config_file=exp_config['config_file'],
                    parameter_value=exp_config['parameter_value'],
                    parameters=exp_config.get('parameters', {})
                )
                experiments.append(experiment)
            
            # Run experiments (sequential for now to avoid resource conflicts)
            series_results = []
            for experiment in experiments:
                result = self.run_single_experiment(experiment)
                series_results.append(result)
                
                if result.success:
                    print(f"  ✓ {experiment.name}: succeeded_throughput={result.metrics.get('succeeded_throughput', 0):.0f} ops/s")
                else:
                    print(f"  ✗ {experiment.name}: FAILED - {result.error_message}")
            
            all_results[series_name] = series_results
        
        return all_results
    
    def generate_csv_data(self, graph_config: Dict[str, Any], results: Dict[str, List[ExperimentResult]]) -> str:
        """Generate CSV data for a graph"""
        csv_file = self.output_dir / "csv" / f"{graph_config['type']}.csv"
        
        # Determine what metrics to include based on graph type
        metric_columns = ['succeeded_throughput', 'insert_p50_us', 'insert_p95_us', 'insert_p99_us']
        
        # Add RDMA metrics for detailed analysis
        rdma_columns = [
            'rdma_read_ops', 'rdma_write_ops', 'rdma_cas_ops', 'rdma_faa_ops',
            'insert_avg_rtts_per_op', 'insert_avg_read_per_op', 'insert_avg_write_per_op',
            'insert_avg_bytes_read', 'insert_avg_bytes_written'
        ]
        
        with open(csv_file, 'w', newline='') as f:
            writer = csv.writer(f)
            
            # Write header
            header = ['series', 'param_value', 'experiment_name'] + metric_columns + rdma_columns
            writer.writerow(header)
            
            # Write data rows
            for series_name, series_results in results.items():
                for result in series_results:
                    if result.success:
                        row = [
                            series_name,
                            result.config.parameter_value,
                            result.config.name
                        ]
                        
                        # Add metric values
                        for col in metric_columns + rdma_columns:
                            row.append(result.metrics.get(col, 0))
                        
                        writer.writerow(row)
        
        return str(csv_file)
    
    def generate_tikz_plot(self, graph_config: Dict[str, Any], csv_file: str):
        """Generate TikZ plot from CSV data"""
        output_file = self.output_dir / "plots" / graph_config['tikz_output']
        
        # Run the TikZ generator
        cmd = [
            sys.executable, "scripts/generate_tikz.py",
            "--data", csv_file,
            "--type", "line",
            "--full-latex",
            "--output", str(output_file)
        ]
        
        try:
            subprocess.run(cmd, check=True, capture_output=True, text=True)
            print(f"Generated TikZ plot: {output_file}")
        except subprocess.CalledProcessError as e:
            print(f"Failed to generate TikZ plot: {e}")
    
    def generate_summary_report(self, all_results: Dict[str, Dict[str, List[ExperimentResult]]]):
        """Generate a comprehensive summary report"""
        report_file = self.output_dir / "reports" / "summary.md"
        
        with open(report_file, 'w') as f:
            f.write("# Experiment Summary Report\n\n")
            f.write(f"Generated on: {time.strftime('%Y-%m-%d %H:%M:%S')}\n")
            f.write(f"Configuration: {self.experiments_config}\n")
            f.write(f"Runs per experiment: {self.num_runs}\n\n")
            
            for graph_name, graph_results in all_results.items():
                f.write(f"## {graph_name}\n\n")
                
                for series_name, series_results in graph_results.items():
                    f.write(f"### {series_name}\n\n")
                    f.write("| Experiment | Status | Throughput (ops/s) | P99 Latency (μs) | RDMA Ops/Op |\n")
                    f.write("|------------|--------|-------------------|------------------|-------------|\n")
                    
                    for result in series_results:
                        status = "✓" if result.success else "✗"
                        throughput = result.metrics.get('succeeded_throughput', 0) if result.success else 0
                        p99_latency = result.metrics.get('insert_p99_us', 0) if result.success else 0
                        avg_rdma = result.metrics.get('insert_avg_read_per_op', 0) + result.metrics.get('insert_avg_write_per_op', 0) if result.success else 0
                        
                        f.write(f"| {result.config.name} | {status} | {throughput:.0f} | {p99_latency:.1f} | {avg_rdma:.1f} |\n")
                    
                    f.write("\n")
        
        print(f"Generated summary report: {report_file}")
    
    def run_all_experiments(self, skip_graphs: bool = False):
        """Run all experiments defined in the configuration"""
        print(f"Starting experiment runner with config: {self.experiments_config}")
        print(f"Output directory: {self.output_dir}")
        print(f"Runs per experiment: {self.num_runs}")
        print("=" * 80)
        
        all_results = {}
        
        # Run experiments for each graph
        for graph_config in self.config['graphs']:
            graph_results = self.run_experiment_series(graph_config)
            all_results[graph_config['title']] = graph_results
            
            # Generate CSV and plots
            if not skip_graphs:
                csv_file = self.generate_csv_data(graph_config, graph_results)
                self.generate_tikz_plot(graph_config, csv_file)
        
        # Generate summary report
        self.generate_summary_report(all_results)
        
        print("\n" + "=" * 80)
        print("Experiment run completed successfully!")
        print(f"Results available in: {self.output_dir}")
        print(f"  - CSV data: {self.output_dir}/csv/")
        print(f"  - TikZ plots: {self.output_dir}/plots/")
        print(f"  - Reports: {self.output_dir}/reports/")
        print(f"  - Raw data: {self.output_dir}/raw_data/")


def main():
    parser = argparse.ArgumentParser(
        description="Run comprehensive experiments and generate publication plots",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
Examples:
  python3 experiment_runner.py --config experiments.yaml
  python3 experiment_runner.py --config experiments.yaml --output-dir custom_results/
  python3 experiment_runner.py --config experiments.yaml --runs 5 --skip-graphs
        """
    )
    
    parser.add_argument('--config', required=True, help='Path to experiments YAML configuration')
    parser.add_argument('--output-dir', default='experiment_results', help='Output directory for results')
    parser.add_argument('--runs', type=int, default=3, help='Number of runs per experiment')
    parser.add_argument('--parallel', type=int, default=1, help='Number of parallel experiments')
    parser.add_argument('--skip-graphs', action='store_true', help='Skip graph generation')
    
    args = parser.parse_args()
    
    if not Path(args.config).exists():
        print(f"Error: Configuration file not found: {args.config}")
        sys.exit(1)
    
    try:
        runner = ExperimentRunner(
            experiments_config=args.config,
            output_dir=args.output_dir,
            num_runs_per_experiment=args.runs,
            parallel_experiments=args.parallel
        )
        runner.run_all_experiments(skip_graphs=args.skip_graphs)
    except Exception as e:
        print(f"Error: {e}")
        sys.exit(1)


if __name__ == "__main__":
    main()