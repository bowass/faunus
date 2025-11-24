#!/usr/bin/env python3
"""
Comprehensive Experiment Runner

This script executes complete experiment workflows based on configuration files:
1. Runs all experiments defined in experiments.yaml
2. Collects results and generates CSV files
3. Creates TikZ plots for publication

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
import itertools
import time
from pathlib import Path
from typing import Dict, List, Any, Optional, Tuple
from dataclasses import dataclass, field
import statistics

from plotting_module import generate_plots


@dataclass
class ExperimentConfig:
    """Configuration for a single experiment"""
    name: str
    config_file: str
    parameter_value: Any
    parameters: Dict[str, Any] = field(default_factory=dict)
    compile_params: Dict[str, Any] = field(default_factory=dict)  # Compile-time parameters (KEY_SIZE, etc.)
    

@dataclass
class ExperimentResult:
    """Results from a single experiment"""
    config: ExperimentConfig
    metrics: Dict[str, float]
    success: bool
    error_message: Optional[str] = None


class ExperimentRunner:
    def __init__(self, experiments_config: str):
        self.experiments_config = experiments_config

        # Load experiment configuration
        with open(self.experiments_config, 'r') as f:
            self.config = yaml.safe_load(f)

        self.output_dir = Path(self.config['paths']['output_dir'])
        self.num_runs = self.config['meta']['runs_per_experiment']

        # Create output directories
        self.output_dir.mkdir(exist_ok=True)
        (self.output_dir / "csv").mkdir(exist_ok=True)
        (self.output_dir / "plots").mkdir(exist_ok=True)
        (self.output_dir / "raw_data").mkdir(exist_ok=True)

        build_config = self.config['build']
        self.executable = build_config['executable']
        self.build_command = build_config['build_command']
        self.build_target = build_config['build_target']
        self.clean_command = build_config['clean_command']
        if isinstance(self.clean_command, str):
            self.clean_command = self.clean_command.split()

        self.summary_file_path = self.config['paths']['summary_file']

        # Verify executable exists
        if not Path(self.executable).exists():
            print(f"Executable '{self.executable}' not found - building.")
            self.clean_build([])
    
    def modify_config_file(self, parameters: Dict[str, Any]) -> str:
        """Create a modified config file with experiment parameters"""
        # If no base_config specified, start with empty dict (will be populated with defaults)
        config_data = {}
        
        print(f"  Applying {len(parameters)} parameter overrides")
        
        # Apply defaults from experiment config (always apply, parameters will override)
        defaults = self.config.get('defaults', {})
        defaults_applied = 0
        for key, value in defaults.items():
            if '.' in key:
                    # Handle nested keys like "cache.enabled"
                    keys = key.split('.')
                    current = config_data
                    for i, k in enumerate(keys[:-1]):
                        if k not in current:
                            current[k] = {}
                        elif not isinstance(current[k], dict):
                            print(f"  Warning: Cannot apply default for {key}: '{k}' is not a dict")
                            break
                        current = current[k]
                    else:
                        # Set default value
                        current[keys[-1]] = value
                        defaults_applied += 1
            else:
                # Simple key - always set default
                config_data[key] = value
                defaults_applied += 1
        
        if defaults_applied > 0:
            print(f"  Applied {defaults_applied} default values")
        
        # Apply parameter modifications with validation
        modifications_applied = []
        for key_path, value in parameters.items():
            # Handle nested keys like "distribution.type"
            keys = key_path.split('.')
            current = config_data
            
            # Navigate/create nested structure
            for i, key in enumerate(keys[:-1]):
                if key not in current:
                    current[key] = {}
                elif not isinstance(current[key], dict):
                    raise ValueError(
                        f"Cannot set {key_path}: '{key}' at level {i} is {type(current[key])}, not a dict"
                    )
                current = current[key]
            
            # Set the final value
            final_key = keys[-1]
            old_value = current.get(final_key, '<not set>')
            
            # If setting a dict value, merge it with existing dict instead of replacing
            if isinstance(value, dict) and isinstance(old_value, dict):
                # Deep merge: update existing dict with new values
                merged = old_value.copy()
                merged.update(value)
                current[final_key] = merged
                modifications_applied.append((key_path, old_value, merged))
            else:
                # Simple value replacement
                current[final_key] = value
                modifications_applied.append((key_path, old_value, value))
        
        # TODO: remove these asserts later
        # Validate critical fields exist
        required_fields = ['num_cs', 'threads_per_cs', 'num_ms', 'warmup_inserts', 'ops_per_client']
        missing_fields = [field for field in required_fields if field not in config_data]
        assert not missing_fields, f"Config missing required fields: {missing_fields}"
        
        # Validate index field
        if 'index' in config_data:
            assert config_data['index'] in ['sherman', 'faunus'], \
                f"Invalid index: {config_data['index']}, must be 'sherman' or 'faunus'"
        
        # Validate nested structures
        if 'distribution' in config_data:
            assert isinstance(config_data['distribution'], dict), \
                f"'distribution' must be dict, got {type(config_data['distribution'])}"
            # Only validate type if it's present
            if 'type' in config_data['distribution']:
                assert config_data['distribution']['type'] in ['uniform', 'skewed'], \
                    f"Invalid distribution type: {config_data['distribution']['type']}"
        
        if 'cache' in config_data:
            assert isinstance(config_data['cache'], dict), \
                f"'cache' must be dict, got {type(config_data['cache'])}"
            assert 'enabled' in config_data['cache'], "'cache' must have 'enabled'"
            assert isinstance(config_data['cache']['enabled'], bool), \
                f"cache.enabled must be bool, got {type(config_data['cache']['enabled'])}"
        
        if 'operation_mix' in config_data:
            assert isinstance(config_data['operation_mix'], dict), \
                f"'operation_mix' must be dict, got {type(config_data['operation_mix'])}"
            # Check that values sum to ~1.0
            total = sum(config_data['operation_mix'].values())
            assert 0.99 <= total <= 1.01, \
                f"operation_mix values must sum to 1.0, got {total}"
        
        # Create temporary config file
        temp_file = tempfile.NamedTemporaryFile(mode='w', suffix='.yaml', delete=False)
        yaml.dump(config_data, temp_file, default_flow_style=False)
        temp_file.close()
        
        # Verify file was created
        assert os.path.exists(temp_file.name), f"Failed to create temp config: {temp_file.name}"
        assert os.path.getsize(temp_file.name) > 0, f"Temp config is empty: {temp_file.name}"
        
        print(f"  Generated config: {temp_file.name}")
        
        return temp_file.name

    def clean_build(self, build_flags: List[str]) -> bool:
        """Compile the executable with given compile parameters"""
        # Clean
        print(f"    Running: {' '.join(self.clean_command)}")
        result = subprocess.run(self.clean_command, capture_output=True, text=True, timeout=60)
        if result.returncode != 0:
            print(f"    Clean command failed: {result.stderr}")
            return False

        build_cmd = [self.build_command, self.build_target, '-j'] + build_flags
        print(f"    Running: {' '.join(build_cmd)}")
        result = subprocess.run(build_cmd, capture_output=True, text=True, timeout=300)

        if result.returncode != 0:
            print(f"    Build failed: {result.stderr[:500]}")
            return False
    
        print("    Recompilation successful")
        return True

    def recompile_if_needed(self, compile_params: Dict[str, Any]) -> bool:
        """
        Recompile executable if compile parameters are specified.
        Returns True if recompilation was successful, False otherwise.
        """
        if not compile_params:
            return True  # No recompilation needed
        
        print(f"  Recompiling with parameters: {compile_params}")
        
        # Build command with compile flags
        build_flags = []
        for param_name, param_value in compile_params.items():
            if param_value == "undefined":
                # Skip this parameter (don't define it)
                print(f"    Skipping {param_name} (undefined)")
                continue
            build_flags.append(f"{param_name}={param_value}")
        
        # Clean and rebuild
        try:
            return self.clean_build(build_flags)
            
        except subprocess.TimeoutExpired:
            print("    Recompilation timed out")
            return False
        except Exception as e:
            print(f"    Recompilation error: {e}")
            return False
    
    def run_single_experiment(self, experiment: ExperimentConfig) -> ExperimentResult:
        """Run a single experiment with the given configuration"""
        print(f"Running experiment: {experiment.name}")
        
        try:
            # Recompile if needed
            if experiment.compile_params:
                if not self.recompile_if_needed(experiment.compile_params):
                    return ExperimentResult(
                        config=experiment,
                        metrics={},
                        success=False,
                        error_message="Recompilation failed"
                    )
            
            # Create modified config file
            temp_config = self.modify_config_file(experiment.parameters)
            
            # Save config for reference
            config_save_path = self.output_dir / "raw_data" / f"{experiment.name}_config.yaml"
            config_save_path.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy(temp_config, config_save_path)
            
            # Run multiple times and average results (if num_runs > 1)
            all_metrics = []
            
            for run_idx in range(self.num_runs):
                if self.num_runs > 1:
                    print(f"  Run {run_idx + 1}/{self.num_runs}...")
                
                # Run executable directly
                cmd = [self.executable, temp_config]
                
                # retry in a loop
                max_failing_attempts = 10
                success = False
                for attempt in range(max_failing_attempts):
                    print(f"    Attempt {attempt}/{max_failing_attempts}")
                    secs_to_sleep = 10
                    print(f"    Sleeping for {secs_to_sleep}s")
                    time.sleep(secs_to_sleep)
                    result = subprocess.run(cmd, capture_output=True, text=True, timeout=600)
                    
                    if result.returncode == 0:
                        print(f"    Successful attempt!")
                        success = True
                        break
                    stderr_msg = result.stderr.strip()
                    stdout_msg = result.stdout.strip()
                    print(f"    Warning: Run {run_idx + 1} failed (exit code {result.returncode})")
                    if stderr_msg:
                        print(f"    STDERR: {stderr_msg[:300]}")
                    if stdout_msg:
                        print(f"    STDOUT (last 200 chars): ...{stdout_msg[-200:]}")
                
                if not success:
                    continue
                
                # Parse the latest summary file
                summary_file = Path(self.summary_file_path)
                if not summary_file.exists():
                    print(f"    Warning: Run {run_idx + 1} - Summary file not found")
                    continue
                
                with open(summary_file, 'r') as f:
                    summary_data = json.load(f)
                
                # Extract metrics
                all_metrics.append(summary_data)
                
                # Save raw data for this run
                if self.num_runs > 1:
                    raw_data_file = self.output_dir / "raw_data" / f"{experiment.name}_run{run_idx + 1}.json"
                else:
                    raw_data_file = self.output_dir / "raw_data" / f"{experiment.name}.json"
                
                with open(raw_data_file, 'w') as f:
                    json.dump(summary_data, f, indent=2)

            # Clean up temp config
            os.unlink(temp_config)

            if not all_metrics:
                return ExperimentResult(
                    config=experiment,
                    metrics={},
                    success=False,
                    error_message=f"All {self.num_runs} runs failed"
                )
            
            # Average metrics across runs
            if len(all_metrics) == 1:
                averaged_metrics = all_metrics[0]
            else:
                averaged_metrics = self._average_metrics(all_metrics)
                print(f"  Averaged {len(all_metrics)} successful runs")
            
            return ExperimentResult(
                config=experiment,
                metrics=averaged_metrics,
                success=True
            )
            
        except subprocess.TimeoutExpired:
            return ExperimentResult(
                config=experiment,
                metrics={},
                success=False,
                error_message="Experiment timed out after 600s"
            )
        except Exception as e:
            import traceback
            return ExperimentResult(
                config=experiment,
                metrics={},
                success=False,
                error_message=f"{str(e)}\n{traceback.format_exc()}"
            )
    
    def _average_metrics(self, metrics_list: List[Dict[str, float]]) -> Dict[str, float]:
        """Average metrics across multiple runs"""
        if not metrics_list:
            return {}
        
        averaged = {}
        all_keys = set()
        for m in metrics_list:
            all_keys.update(m.keys())
        
        for key in all_keys:
            values = [m.get(key, 0.0) for m in metrics_list if key in m]
            if values:
                # Skip non-numeric values
                if not all(isinstance(v, (int, float)) for v in values):
                    # Just copy the first value for non-numeric fields
                    averaged[key] = values[0]
                    continue
                    
                averaged[key] = statistics.mean(values)
        
        return averaged
    
    def get_metric_by_path(self, data: Dict[str, Any], path: str) -> Any:
        """
        Extract a metric from nested JSON using dot notation.
        
        Examples:
            'throughput.succeeded_ops_per_sec' -> data['throughput']['succeeded_ops_per_sec']
            'cache.total_hit_rate' -> data['cache']['total_hit_rate']
            'per_operation_latency.read.p99_latency_us' -> data['per_operation_latency']['read']['p99_latency_us']
        
        Also handles flat keys with dots (e.g., 'cache.max_size_kb' as a single key)
        """
        # First check if path exists as a direct key (for parameter dicts)
        if isinstance(data, dict) and path in data:
            return data[path]
        
        # Otherwise navigate nested structure
        keys = path.split('.')
        current = data
        
        for key in keys:
            if isinstance(current, dict):
                current = current.get(key)
                if current is None:
                    return None
            else:
                return None
        
        return current
    
    def expand_experiments(self, experiment_config: Dict[str, Any]) -> List[ExperimentConfig]:
        """
        Expand experiment configuration with variables dimensions.
        New format: variables accepts arbitrary config paths (e.g., 'cache.enabled', 'distribution.type')
        and generates cartesian product of all dimensions.
        Supports compile_param flag for parameters requiring recompilation (KEY_SIZE, VALUE_SIZE, etc.)
        """
        experiments = []
        exp_name = experiment_config['name']
        
        # Get variables dimensions (new generalized format)
        # Support both 'variables' and 'variables' keys
        variables = experiment_config.get('variables', [])
        
        if variables:
            # Extract parameter paths, values, and compile flags
            param_paths = []
            param_values_list = []
            compile_flags = []  # Track which parameters require compilation
            
            for dim in variables:
                # Support both old 'dimension' and new 'parameter' keys
                param_path = dim.get('parameter', dim.get('dimension'))
                values = dim['values']
                is_compile_param = dim.get('compile_param', False)
                
                param_paths.append(param_path)
                param_values_list.append(values)
                compile_flags.append(is_compile_param)
            
            # Generate cartesian product of all parameter combinations
            for combination in itertools.product(*param_values_list):
                # Build parameter dict for this combination
                combo_params = {}
                compile_params = {}  # Separate dict for compile-time parameters
                combo_label_parts = []
                
                for param_path, value, is_compile in zip(param_paths, combination, compile_flags):
                    if is_compile:
                        # Compile parameter - track separately
                        compile_params[param_path] = value
                    else:
                        # Runtime parameter - add to combo_params
                        combo_params[param_path] = value
                    
                    # Create label component (use last part of path and value)
                    param_name = param_path.split('.')[-1]
                    combo_label_parts.append(f"{param_name}_{value}")
                
                # Create experiment name
                combo_label = '_'.join(combo_label_parts)
                experiment_name = f"{exp_name}_{combo_label}"
                
                experiments.append(ExperimentConfig(
                    name=experiment_name,
                    config_file=None,  # No base_config, use global defaults
                    parameter_value=combo_label,
                    parameters=combo_params,
                    compile_params=compile_params  # Track compile parameters separately
                ))
        else:
            # Simple experiment with no variations - just use defaults
            experiments.append(ExperimentConfig(
                name=exp_name,
                config_file=None,
                parameter_value=exp_name,
                parameters={}
            ))
        
        return experiments
    
    def run_experiment_series(self, experiment_config: Dict[str, Any]) -> Dict[str, List[ExperimentResult]]:
        """Run all experiments for a single experiment series"""
        exp_name = experiment_config['name']
        print(f"\n{'='*80}")
        print(f"Experiment Series: {exp_name}")
        print(f"Description: {experiment_config.get('description', 'N/A')}")
        print(f"Output Type: {experiment_config.get('output_type', 'N/A')}")
        
        # Check if experiment is enabled
        if not experiment_config.get('enabled', True):
            print(f"SKIPPED: Experiment is disabled")
            return {}
        
        # Check if experiment requires special handling
        if experiment_config.get('requires_recompile', False):
            print(f"WARNING: This experiment requires recompilation with different compile parameters")
            print(f"  Note: Use compile_param: true in variables for automatic recompilation")
            return {}
        
        print(f"{'='*80}")
        
        # Expand experiments based on configuration
        experiments = self.expand_experiments(experiment_config)
        
        print(f"\nTotal experiments to run: {len(experiments)}")
        
        # Group results by series for better organization
        all_results = {}
        
        # Run experiments
        for i, experiment in enumerate(experiments, 1):
            print(f"\n[{i}/{len(experiments)}] Running: {experiment.name}")
            result = self.run_single_experiment(experiment)
            
            # Group by base series name
            series_key = experiment.name.split('_')[1] if '_' in experiment.name else experiment.name
            if series_key not in all_results:
                all_results[series_key] = []
            all_results[series_key].append(result)
            
            if result.success:
                throughput = result.metrics.get('throughput.attempted_ops_per_sec', 0)
                p50 = result.metrics.get('global_latency.p50_latency_us', 0)
                p99 = result.metrics.get('global_latency.p99_latency_us', 0)
                print(f"  ✓ Success: {throughput:.0f} ops/s, P50={p50:.2f}µs, P99={p99:.2f}µs")
            else:
                print(f"  ✗ FAILED: {result.error_message}")
        
        return all_results
 
    def _clean_label(self, label: str, exp_name: str, group_by_params: List[str]) -> str:
        """
        Clean experiment label by removing common prefixes and group_by parameters.
        Used consistently across all plot types.
        """
        # Remove experiment name prefix
        if label.startswith(exp_name + '_'):
            label = label[len(exp_name) + 1:]

        # Remove group_by parameter values from label
        for param_path in group_by_params:
            param_name = param_path.split('.')[-1]
            # Remove "paramname_value_" patterns
            parts = label.split('_')
            clean_parts = []
            skip_next = False
            for i, part in enumerate(parts):
                if skip_next:
                    skip_next = False
                    continue
                if part == param_name and i + 1 < len(parts):
                    skip_next = True
                    continue
                clean_parts.append(part)
            label = '_'.join(clean_parts)
        
        # Clean up any leading/trailing underscores
        label = label.strip('_')
        
        return label if label else 'unknown'

    def dump_experiment_results(self, exp_name: str, exp_results: Dict[str, List[ExperimentResult]]):
        """Dump experiment results to raw_data directory as one JSON file per experiment"""
        output_file = self.output_dir / "raw_data" / f"{exp_name}_results.json"
        
        # Convert results to serializable format
        serialized = []
        for series_key, results in exp_results.items():
            for result in results:
                if result.success:
                    # Merge runtime and compile parameters for convenience
                    all_params = {**result.config.parameters, **result.config.compile_params}
                    serialized.append({
                        'name': result.config.name,
                        'parameters': all_params,
                        'metrics': result.metrics
                    })
        
        with open(output_file, 'w') as f:
            json.dump(serialized, f, indent=2)
        
        print(f"  Saved {len(serialized)} results to {output_file}")

    def run_all_experiments(self, experiment_filter: Optional[str] = None):
        """Run all experiments defined in the configuration"""
        print(f"\n{'='*80}")
        print(f"FAUNUS EXPERIMENT RUNNER")
        print(f"{'='*80}")
        print(f"Configuration: {self.experiments_config}")
        print(f"Output directory: {self.output_dir}")
        print(f"Runs per experiment: {self.num_runs}")
        if experiment_filter:
            print(f"Filter: Running only experiments matching '{experiment_filter}'")
        print(f"{'='*80}\n")
        
        # Get experiment list
        experiments_list = self.config.get('experiments', self.config.get('graphs', []))
        
        # Filter experiments if requested
        if experiment_filter:
            experiments_list = [
                exp for exp in experiments_list 
                if experiment_filter.lower() in exp.get('name', '').lower()
            ]
            print(f"Filtered to {len(experiments_list)} experiments\n")
        
        # Run experiments for each series
        total_experiments = len(experiments_list)
        for idx, experiment_config in enumerate(experiments_list, 1):
            exp_name = experiment_config.get('name', f'experiment_{idx}')
            print(f"\n{'#'*80}")
            print(f"# Experiment {idx}/{total_experiments}: {exp_name}")
            print(f"{'#'*80}")
            
            try:
                exp_results = self.run_experiment_series(experiment_config)
                self.dump_experiment_results(exp_name, exp_results)
                
            except Exception as e:
                print(f"\nERROR: Experiment {exp_name} failed: {e}")
                import traceback
                traceback.print_exc()
                continue
        
        print("\n" + "=" * 80)
        print("EXPERIMENT RUN COMPLETED")
        print("=" * 80)
        print(f"Results available in: {self.output_dir}")
        print(f"  - CSV data: {self.output_dir}/csv/")
        print(f"  - Raw data: {self.output_dir}/raw_data/")
        print("=" * 80)


def main():
    parser = argparse.ArgumentParser(
        description="Run comprehensive experiments and generate publication plots",
        formatter_class=argparse.RawDescriptionHelpFormatter
    )
    
    parser.add_argument('--config', required=True, help='Path to experiments YAML configuration')
    parser.add_argument('--output-dir', default='experiment_results', help='Output directory for results')
    parser.add_argument('--filter', type=str, help='Run only experiments matching this name filter')
    parser.add_argument('--run-only', action='store_true', help='Skip plot generation')
    parser.add_argument('--plot-only', action='store_true', help='Skip experiment execution, only generate plots from existing data')
    
    args = parser.parse_args()
    
    if not Path(args.config).exists():
        print(f"Error: Configuration file not found: {args.config}")
        sys.exit(1)
    
    try:
        if not args.plot_only:
            runner = ExperimentRunner(
                experiments_config=args.config
            )
            runner.run_all_experiments(experiment_filter=args.filter)

        if not args.run_only:
            # Load config to get experiment definitions
            with open(args.config, 'r') as f:
                config = yaml.safe_load(f)

            data_dir = Path(args.output_dir)
            assert data_dir.exists(), f"Data root directory does not exist: {data_dir}"
            
            # Filter experiments if requested
            experiments_list = config.get('experiments', [])
            if args.filter:
                experiments_list = [
                    exp for exp in experiments_list 
                    if args.filter.lower() in exp.get('name', '').lower()
                ]
            
            # Generate plots for each experiment
            for exp_config in experiments_list:
                exp_name = exp_config['name']
                results_file = data_dir / "raw_data" / f"{exp_name}_results.json"
                
                if not results_file.exists():
                    print(f"Skipping {exp_name}: no results file found")
                    continue
                
                # Load results
                with open(results_file, 'r') as f:
                    results = json.load(f)
                
                # Generate plots for each output configuration
                outputs = exp_config.get('outputs', [])
                for output_cfg in outputs:
                    print(f"Generating plot: {exp_name} / {output_cfg['name']}")
                    generate_plots(output_cfg, results, str(data_dir / "plots"))


    except Exception as e:
        print(f"Error: {e}")
        import traceback
        traceback.print_exc()
        sys.exit(1)


if __name__ == "__main__":
    main()