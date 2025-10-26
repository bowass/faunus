#!/usr/bin/env python3
"""
Performance Graph Generator

This script generates comparison graphs for different KV-index implementations
across various workloads, skewness levels, thread counts, key sizes, etc.

Usage:
    python3 generate_graphs.py --config experiments.yaml --output graphs/
    python3 generate_graphs.py --single-graph throughput_vs_threads --data results.json --output plot.png
"""

import sys
import os
import json
import yaml
import argparse
import subprocess
import matplotlib.pyplot as plt
import matplotlib.patches as mpatches
import numpy as np
from pathlib import Path
from typing import Dict, List, Any, Tuple, Optional
from dataclasses import dataclass, field
from enum import Enum
import seaborn as sns

# Set up matplotlib for publication-quality plots
plt.style.use('seaborn-v0_8-whitegrid')
sns.set_palette("husl")

class GraphType(Enum):
    THROUGHPUT_LATENCY = "throughput_latency"
    THROUGHPUT_VS_THREADS = "throughput_vs_threads"
    THROUGHPUT_VS_KEY_SIZE = "throughput_vs_key_size"
    THROUGHPUT_VS_MAINTENANCE = "throughput_vs_maintenance"

@dataclass
class ExperimentConfig:
    """Configuration for a single experiment"""
    name: str
    index_type: str
    config_file: str
    parameters: Dict[str, Any] = field(default_factory=dict)
    
@dataclass
class ExperimentSeries:
    """A series of experiments varying one parameter"""
    name: str
    experiments: List[ExperimentConfig]
    vary_parameter: str
    parameter_values: List[Any]
    
@dataclass
class GraphConfig:
    """Configuration for generating a graph"""
    graph_type: GraphType
    title: str
    series: List[ExperimentSeries]
    output_file: str
    tikz_output: Optional[str] = None
    
class PerformanceGraphGenerator:
    def __init__(self, output_dir: str = "graphs"):
        self.output_dir = Path(output_dir)
        self.output_dir.mkdir(exist_ok=True)
        self.results_cache = {}
        
    def run_experiment(self, config: ExperimentConfig, runs: int = 3) -> Dict[str, Any]:
        """Run a single experiment and return the results"""
        cache_key = f"{config.name}_{hash(str(config.parameters))}"
        
        if cache_key in self.results_cache:
            print(f"Using cached results for {config.name}")
            return self.results_cache[cache_key]
            
        print(f"Running experiment: {config.name}")
        
        # Create temporary config file with parameters
        temp_config = self._create_temp_config(config)
        
        try:
            # Run the performance analysis
            cmd = ["python3", "run_performance_analysis.py", temp_config, str(runs)]
            result = subprocess.run(cmd, capture_output=True, text=True, timeout=1800)
            
            if result.returncode != 0:
                print(f"Experiment {config.name} failed: {result.stderr}")
                return None
                
            # Parse the results from the last run's summary.json
            summary_file = Path("thread_stats/summary.json")
            if summary_file.exists():
                with open(summary_file, 'r') as f:
                    data = json.load(f)
                self.results_cache[cache_key] = data
                return data
            else:
                print(f"No results found for {config.name}")
                return None
                
        except Exception as e:
            print(f"Error running experiment {config.name}: {e}")
            return None
        finally:
            # Clean up temp config
            if os.path.exists(temp_config):
                os.remove(temp_config)
    
    def _create_temp_config(self, config: ExperimentConfig) -> str:
        """Create a temporary config file with experiment parameters"""
        # Load base config
        with open(config.config_file, 'r') as f:
            base_config = yaml.safe_load(f)
        
        # Apply parameter overrides
        for key, value in config.parameters.items():
            if '.' in key:
                # Handle nested keys like "operation_mix.insert"
                keys = key.split('.')
                current = base_config
                for k in keys[:-1]:
                    if k not in current:
                        current[k] = {}
                    current = current[k]
                current[keys[-1]] = value
            else:
                base_config[key] = value
        
        # Write temporary config
        temp_file = f"temp_config_{config.name}_{os.getpid()}.yaml"
        with open(temp_file, 'w') as f:
            yaml.dump(base_config, f)
        
        return temp_file
    
    def run_experiment_series(self, series: ExperimentSeries, runs: int = 3) -> List[Tuple[Any, Dict[str, Any]]]:
        """Run a series of experiments and return results"""
        results = []
        
        for i, experiment in enumerate(series.experiments):
            param_value = series.parameter_values[i]
            result = self.run_experiment(experiment, runs)
            if result:
                results.append((param_value, result))
            
        return results
    
    def extract_metrics(self, data: Dict[str, Any]) -> Dict[str, float]:
        """Extract key metrics from experiment results"""
        metrics = {}
        
        # Throughput metrics
        throughput_data = data.get('throughput', {})
        if throughput_data:
            metrics['attempted_throughput'] = throughput_data.get('attempted_ops_per_sec', 0.0)
            metrics['succeeded_throughput'] = throughput_data.get('succeeded_ops_per_sec', 0.0)
        
        # Latency metrics (focus on insert operations for now)
        latency_data = data.get('per_operation_latency', {})
        for op in ['insert', 'read', 'update', 'delete']:
            op_data = latency_data.get(op, {})
            if op_data and op_data.get('sample_count', 0) > 0:
                metrics[f'{op}_p50'] = op_data.get('p50_latency_us', 0.0)
                metrics[f'{op}_p99'] = op_data.get('p99_latency_us', 0.0)
                metrics[f'{op}_avg'] = op_data.get('avg_latency_us', 0.0)
        
        # Cache metrics
        cache_data = data.get('cache', {})
        if cache_data:
            metrics['cache_hit_rate'] = cache_data.get('total_hit_rate', 0.0)
        
        # RDMA metrics
        rdma_data = data.get('rdma', {})
        if rdma_data:
            metrics['total_rtt_ms'] = rdma_data.get('total_rtt_ms', 0.0)
        
        return metrics
    
    def generate_throughput_latency_graph(self, graph_config: GraphConfig):
        """Generate throughput vs latency scatter plot"""
        plt.figure(figsize=(10, 6))
        
        colors = plt.cm.Set1(np.linspace(0, 1, len(graph_config.series)))
        
        for i, series in enumerate(graph_config.series):
            results = self.run_experiment_series(series)
            
            throughputs = []
            p50_latencies = []
            p99_latencies = []
            
            for param_value, data in results:
                metrics = self.extract_metrics(data)
                throughputs.append(metrics.get('succeeded_throughput', 0) / 1000)  # Convert to Kops/sec
                p50_latencies.append(metrics.get('insert_p50', 0))
                p99_latencies.append(metrics.get('insert_p99', 0))
            
            if throughputs and p50_latencies:
                plt.scatter(p50_latencies, throughputs, label=f'{series.name} (P50)', 
                           color=colors[i], alpha=0.7, s=60, marker='o')
                plt.scatter(p99_latencies, throughputs, label=f'{series.name} (P99)', 
                           color=colors[i], alpha=0.7, s=60, marker='^')
        
        plt.xlabel('Latency (μs)')
        plt.ylabel('Throughput (Kops/sec)')
        plt.title(graph_config.title)
        plt.legend()
        plt.grid(True, alpha=0.3)
        
        output_file = self.output_dir / graph_config.output_file
        plt.savefig(output_file, dpi=300, bbox_inches='tight')
        print(f"Saved throughput vs latency graph: {output_file}")
        
        if graph_config.tikz_output:
            self._export_tikz(plt.gcf(), self.output_dir / graph_config.tikz_output)
        
        plt.close()
    
    def generate_throughput_vs_threads_graph(self, graph_config: GraphConfig):
        """Generate throughput vs number of threads line plot"""
        plt.figure(figsize=(10, 6))
        
        colors = plt.cm.Set1(np.linspace(0, 1, len(graph_config.series)))
        
        for i, series in enumerate(graph_config.series):
            results = self.run_experiment_series(series)
            
            thread_counts = []
            throughputs = []
            
            for param_value, data in results:
                metrics = self.extract_metrics(data)
                thread_counts.append(param_value)
                throughputs.append(metrics.get('succeeded_throughput', 0) / 1000)  # Convert to Kops/sec
            
            if thread_counts and throughputs:
                # Sort by thread count for proper line plotting
                sorted_data = sorted(zip(thread_counts, throughputs))
                thread_counts, throughputs = zip(*sorted_data)
                
                plt.plot(thread_counts, throughputs, label=series.name, 
                        color=colors[i], marker='o', linewidth=2, markersize=6)
        
        plt.xlabel('Number of Threads')
        plt.ylabel('Throughput (Kops/sec)')
        plt.title(graph_config.title)
        plt.legend()
        plt.grid(True, alpha=0.3)
        
        output_file = self.output_dir / graph_config.output_file
        plt.savefig(output_file, dpi=300, bbox_inches='tight')
        print(f"Saved throughput vs threads graph: {output_file}")
        
        if graph_config.tikz_output:
            self._export_tikz(plt.gcf(), self.output_dir / graph_config.tikz_output)
        
        plt.close()
    
    def generate_throughput_vs_key_size_graph(self, graph_config: GraphConfig):
        """Generate throughput vs key size line plot"""
        plt.figure(figsize=(10, 6))
        
        colors = plt.cm.Set1(np.linspace(0, 1, len(graph_config.series)))
        
        for i, series in enumerate(graph_config.series):
            results = self.run_experiment_series(series)
            
            key_sizes = []
            throughputs = []
            
            for param_value, data in results:
                metrics = self.extract_metrics(data)
                key_sizes.append(param_value)
                throughputs.append(metrics.get('succeeded_throughput', 0) / 1000)  # Convert to Kops/sec
            
            if key_sizes and throughputs:
                # Sort by key size for proper line plotting
                sorted_data = sorted(zip(key_sizes, throughputs))
                key_sizes, throughputs = zip(*sorted_data)
                
                plt.plot(key_sizes, throughputs, label=series.name, 
                        color=colors[i], marker='s', linewidth=2, markersize=6)
        
        plt.xlabel('Key Size (bytes)')
        plt.ylabel('Throughput (Kops/sec)')
        plt.title(graph_config.title)
        plt.legend()
        plt.grid(True, alpha=0.3)
        
        output_file = self.output_dir / graph_config.output_file
        plt.savefig(output_file, dpi=300, bbox_inches='tight')
        print(f"Saved throughput vs key size graph: {output_file}")
        
        if graph_config.tikz_output:
            self._export_tikz(plt.gcf(), self.output_dir / graph_config.tikz_output)
        
        plt.close()
    
    def generate_throughput_vs_maintenance_graph(self, graph_config: GraphConfig):
        """Generate throughput vs maintenance threads line plot"""
        plt.figure(figsize=(10, 6))
        
        colors = plt.cm.Set1(np.linspace(0, 1, len(graph_config.series)))
        
        for i, series in enumerate(graph_config.series):
            results = self.run_experiment_series(series)
            
            maintenance_counts = []
            throughputs = []
            
            for param_value, data in results:
                metrics = self.extract_metrics(data)
                maintenance_counts.append(param_value)
                throughputs.append(metrics.get('succeeded_throughput', 0) / 1000)  # Convert to Kops/sec
            
            if maintenance_counts and throughputs:
                # Sort by maintenance count for proper line plotting
                sorted_data = sorted(zip(maintenance_counts, throughputs))
                maintenance_counts, throughputs = zip(*sorted_data)
                
                plt.plot(maintenance_counts, throughputs, label=series.name, 
                        color=colors[i], marker='d', linewidth=2, markersize=6)
        
        plt.xlabel('Number of Maintenance Threads')
        plt.ylabel('Throughput (Kops/sec)')
        plt.title(graph_config.title)
        plt.legend()
        plt.grid(True, alpha=0.3)
        
        output_file = self.output_dir / graph_config.output_file
        plt.savefig(output_file, dpi=300, bbox_inches='tight')
        print(f"Saved throughput vs maintenance threads graph: {output_file}")
        
        if graph_config.tikz_output:
            self._export_tikz(plt.gcf(), self.output_dir / graph_config.tikz_output)
        
        plt.close()
    
    def generate_graph(self, graph_config: GraphConfig):
        """Generate a graph based on the configuration"""
        print(f"Generating {graph_config.graph_type.value} graph: {graph_config.title}")
        
        if graph_config.graph_type == GraphType.THROUGHPUT_LATENCY:
            self.generate_throughput_latency_graph(graph_config)
        elif graph_config.graph_type == GraphType.THROUGHPUT_VS_THREADS:
            self.generate_throughput_vs_threads_graph(graph_config)
        elif graph_config.graph_type == GraphType.THROUGHPUT_VS_KEY_SIZE:
            self.generate_throughput_vs_key_size_graph(graph_config)
        elif graph_config.graph_type == GraphType.THROUGHPUT_VS_MAINTENANCE:
            self.generate_throughput_vs_maintenance_graph(graph_config)
        else:
            print(f"Unknown graph type: {graph_config.graph_type}")
    
    def _export_tikz(self, fig, output_file: Path):
        """Export matplotlib figure to TikZ format"""
        try:
            import tikzplotlib
            tikzplotlib.save(str(output_file), figure=fig)
            print(f"Exported TikZ plot: {output_file}")
        except ImportError:
            print("tikzplotlib not installed. Install with: pip install tikzplotlib")
        except Exception as e:
            print(f"Error exporting TikZ: {e}")

def load_experiment_config(config_file: str) -> List[GraphConfig]:
    """Load experiment configuration from YAML file"""
    with open(config_file, 'r') as f:
        config = yaml.safe_load(f)
    
    graphs = []
    
    for graph_cfg in config.get('graphs', []):
        series_list = []
        
        for series_cfg in graph_cfg.get('series', []):
            experiments = []
            param_values = []
            
            for exp_cfg in series_cfg.get('experiments', []):
                experiments.append(ExperimentConfig(
                    name=exp_cfg['name'],
                    index_type=exp_cfg['index_type'],
                    config_file=exp_cfg['config_file'],
                    parameters=exp_cfg.get('parameters', {})
                ))
                param_values.append(exp_cfg.get('parameter_value'))
            
            series_list.append(ExperimentSeries(
                name=series_cfg['name'],
                experiments=experiments,
                vary_parameter=series_cfg['vary_parameter'],
                parameter_values=param_values
            ))
        
        graphs.append(GraphConfig(
            graph_type=GraphType(graph_cfg['type']),
            title=graph_cfg['title'],
            series=series_list,
            output_file=graph_cfg['output_file'],
            tikz_output=graph_cfg.get('tikz_output')
        ))
    
    return graphs

def main():
    parser = argparse.ArgumentParser(
        description="Generate performance comparison graphs for KV-index implementations",
        formatter_class=argparse.RawDescriptionHelpFormatter
    )
    
    parser.add_argument('--config', help='YAML configuration file for experiments')
    parser.add_argument('--output', default='graphs', help='Output directory for graphs')
    parser.add_argument('--runs', type=int, default=3, help='Number of runs per experiment')
    
    args = parser.parse_args()
    
    if not args.config:
        print("Error: --config is required")
        sys.exit(1)
    
    if not os.path.exists(args.config):
        print(f"Error: Config file not found: {args.config}")
        sys.exit(1)
    
    try:
        generator = PerformanceGraphGenerator(args.output)
        graph_configs = load_experiment_config(args.config)
        
        print(f"Loaded {len(graph_configs)} graph configurations")
        
        for graph_config in graph_configs:
            generator.generate_graph(graph_config)
        
        print(f"All graphs generated in: {args.output}")
        
    except Exception as e:
        print(f"Error: {e}")
        sys.exit(1)

if __name__ == "__main__":
    main()