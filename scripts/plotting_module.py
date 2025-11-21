"""
Minimal plotting module for experiment results
"""

import os
import matplotlib.pyplot as plt
import numpy as np
from pathlib import Path
from typing import Any, Dict, List
from collections import defaultdict

def commonprefix(strings: List[str]) -> str:
    """Find common prefix among a list of strings"""
    return os.path.commonprefix(strings)

def commonsuffix(strings: List[str]) -> str:
    """Find common suffix among a list of strings"""
    reversed_strings = [s[::-1] for s in strings]
    return commonprefix(reversed_strings)[::-1]

def remove_common_affix(names: List[str]) -> List[str]:
    """Remove common prefix and suffix from a list of names"""
    prefix = commonprefix(names)
    suffix = commonsuffix(names)
    
    trimmed_names = []
    for name in names:
        trimmed = name
        if prefix and name.startswith(prefix):
            trimmed = trimmed[len(prefix):]
        if suffix and name.endswith(suffix):
            trimmed = trimmed[:-len(suffix)]
        trimmed_names.append(trimmed)
    
    return trimmed_names

def _get_nested(d: Dict, path: str, default=None):
    """Get nested dict value using dot notation"""
    if path in d:
        return d[path]
    
    parts = path.split('.')
    current = d
    for part in parts:
        if not isinstance(current, dict) or part not in current:
            return default
        current = current[part]
    return current


def _group_data(results: List[Dict], group_by: List[str]):
    """Group results by specified parameters"""
    if not group_by:
        return {(): results}
    
    groups = {}
    for result in results:
        key = tuple(_get_nested(result['parameters'], param) for param in group_by)
        if key not in groups:
            groups[key] = []
        groups[key].append(result)
    return groups


def _extract_cdf_from_histogram(histogram: List[Dict]):
    """Convert histogram to CDF data"""
    if not histogram:
        return [], []
    
    # Convert values to numeric (handle both int and str) and sort by value
    data_pairs = []
    for item in histogram:
        value = item['value']
        # Convert to numeric if it's a string
        if isinstance(value, str):
            try:
                value = int(value) if value.isdigit() else float(value)
            except ValueError:
                continue
        data_pairs.append((value, item['count']))
    
    # Sort by numeric value
    data_pairs.sort(key=lambda x: x[0])
    
    # Deduplicate: sum counts for same value
    deduplicated = {}
    for value, count in data_pairs:
        deduplicated[value] = deduplicated.get(value, 0) + count
    
    values = list(deduplicated.keys())
    counts = [deduplicated[v] for v in values]
    
    total = sum(counts)
    if total == 0:
        return [], []
    
    cumulative = np.cumsum(counts) / total
    return values, cumulative.tolist()


def generate_plots(output_cfg: Dict[str, Any], results: List[Dict], output_dir: str):
    """Generate plots based on output configuration"""
    os.makedirs(output_dir, exist_ok=True)
    
    plot_name = output_cfg.get('name', 'plot')
    title = output_cfg.get('title', plot_name)
    series_by = output_cfg.get('series_by', [])
    x_axis_cfg = output_cfg.get('x_axis', {})
    y_axes_cfg = output_cfg.get('y_axes', [])
    annotate = output_cfg.get('annotate_values', False)
    cdf_ranges = output_cfg.get('cdf_ranges', None)  # Global CDF range filter for all CDF plots in this output
    
    groups = _group_data(results, series_by)
    
    for group_key, group_results in groups.items():
        if group_key:
            group_suffix = '_'.join(f"{v}" for v in group_key)
            filename = f"{plot_name}_{group_suffix}.png"
            plot_title = f"{title} ({', '.join(str(v) for v in group_key)})"
        else:
            filename = f"{plot_name}.png"
            plot_title = title
        
        x_metric = x_axis_cfg.get('metric')
        
        # Check if any y-axis is a CDF plot
        has_cdf = any(y.get('plot_type') == 'cdf' for y in y_axes_cfg)
        is_categorical = x_metric is None and not has_cdf
        
        fig, ax_left = plt.subplots(figsize=(10, 6))
        ax_right = None
        
        left_axes = [y for y in y_axes_cfg if y.get('axis', 'left') == 'left']
        right_axes = [y for y in y_axes_cfg if y.get('axis') == 'right']
        
        if right_axes:
            ax_right = ax_left.twinx()
        
        if has_cdf:
            # CDF plot: don't use x_values, let _plot_metric handle it
            x_positions = list(range(len(group_results)))
            for y_cfg in left_axes:
                _plot_metric(ax_left, group_results, x_positions, y_cfg, annotate, cdf_ranges)
            for y_cfg in right_axes:
                _plot_metric(ax_right, group_results, x_positions, y_cfg, annotate, cdf_ranges)
        elif is_categorical:
            x_positions = list(range(len(group_results)))
            
            # Check if custom label format is specified
            label_format = x_axis_cfg.get('label_format')
            if label_format:
                # Format labels using parameters
                import re
                x_labels = []
                for r in group_results:
                    label = label_format
                    
                    # First replace from parameters dict
                    for key, value in r['parameters'].items():
                        label = label.replace(f"{{{key}}}", str(value))
                    
                    # Extract remaining placeholders from name (for compile params)
                    # Name format: "exp_KEY_SIZE_8_VALUE_SIZE_16_..."
                    remaining_placeholders = re.findall(r'\{(\w+)\}', label)
                    if remaining_placeholders:
                        name_parts = r['name'].split('_')
                        for param_name in remaining_placeholders:
                            # Find param_name in name_parts and get next value
                            try:
                                idx = name_parts.index(param_name)
                                if idx + 1 < len(name_parts):
                                    param_value = name_parts[idx + 1]
                                    label = label.replace(f"{{{param_name}}}", param_value)
                            except ValueError:
                                pass  # Parameter not found in name
                    
                    x_labels.append(label)
            else:
                x_labels = [r['name'] for r in group_results]
                x_labels = remove_common_affix(x_labels)

            for y_cfg in left_axes:
                _plot_metric(ax_left, group_results, x_positions, y_cfg, annotate, cdf_ranges)
            for y_cfg in right_axes:
                _plot_metric(ax_right, group_results, x_positions, y_cfg, annotate, cdf_ranges)
            
            ax_left.set_xticks(x_positions)
            ax_left.set_xticklabels(x_labels, rotation=45, ha='right')
        else:
            x_values = [_get_nested(r['parameters'], x_metric, _get_nested(r['metrics'], x_metric)) 
                       for r in group_results]
            sorted_pairs = sorted(zip(x_values, group_results))
            x_values = [x for x, _ in sorted_pairs]
            sorted_results = [r for _, r in sorted_pairs]
            
            use_log_scale = x_axis_cfg.get('log_scale', False)
            
            # Check if we have bar plots with log scale - need special handling
            has_bars = any(y.get('plot_type') == 'bar' for y in left_axes + right_axes)
            if use_log_scale and has_bars:
                # Use categorical positions with log-scale labels
                x_positions = list(range(len(x_values)))
                for y_cfg in left_axes:
                    _plot_metric(ax_left, sorted_results, x_positions, y_cfg, annotate, cdf_ranges)
                for y_cfg in right_axes:
                    _plot_metric(ax_right, sorted_results, x_positions, y_cfg, annotate, cdf_ranges)
                
                # Set custom tick labels showing actual x values
                ax_left.set_xticks(x_positions)
                ax_left.set_xticklabels([f'{int(x)}' if x >= 1024 else f'{x}' for x in x_values], 
                                       rotation=45, ha='right')
            else:
                for y_cfg in left_axes:
                    _plot_metric(ax_left, sorted_results, x_values, y_cfg, annotate, cdf_ranges)
                for y_cfg in right_axes:
                    _plot_metric(ax_right, sorted_results, x_values, y_cfg, annotate, cdf_ranges)
                
                if use_log_scale:
                    ax_left.set_xscale('log', base=2)
        
        ax_left.set_xlabel(x_axis_cfg.get('label', 'X'))
        if left_axes:
            ax_left.set_ylabel(left_axes[0].get('label', ''))
        if right_axes and ax_right:
            ax_right.set_ylabel(right_axes[0].get('label', ''))
        
        # Improved legend placement
        lines1, labels1 = ax_left.get_legend_handles_labels()
        if ax_right:
            lines2, labels2 = ax_right.get_legend_handles_labels()
            # Place legend outside plot area to avoid obscuring data
            ax_left.legend(lines1 + lines2, labels1 + labels2, loc='upper left', bbox_to_anchor=(1.15, 1))
        else:
            ax_left.legend(bbox_to_anchor=(1.05, 1), loc='upper left')
        
        plt.title(plot_title)
        ax_left.grid(True, alpha=0.3)
        plt.tight_layout()
        
        output_path = Path(output_dir) / filename
        plt.savefig(output_path, dpi=300, bbox_inches='tight')
        plt.close()
        print(f"  Saved: {output_path}")


def _plot_metric(ax, results, x_values, y_cfg, annotate, cdf_ranges=None):
    """Plot a single metric
    
    Args:
        ax: Matplotlib axis to plot on
        results: List of result dictionaries
        x_values: X-axis values (or categorical positions)
        y_cfg: Y-axis configuration dict
        annotate: Whether to add value annotations
        cdf_ranges: Global CDF range filter (list of {percentile_start, percentile_end} dicts)
                   Applied to ALL CDF plots in this output config
    """
    metric = y_cfg['metric']
    plot_type = y_cfg.get('plot_type', 'line')
    label = y_cfg.get('metric_label', y_cfg.get('label', metric))
    
    if plot_type == 'cdf':
        # CDF plot: ignore x_values, use histogram data

        x_labels = remove_common_affix([r['name'] for r in results])

        for i, (exp_label, result) in enumerate(zip(x_labels, results)):
            histogram_data = _get_nested(result['metrics'], metric, [])
            # Handle both direct histogram list and nested histogram structure
            if isinstance(histogram_data, dict) and 'histogram' in histogram_data:
                histogram = histogram_data['histogram']
            else:
                histogram = histogram_data
            
            x_cdf, y_cdf = _extract_cdf_from_histogram(histogram)
            if x_cdf:
                # # Build label: combine metric label with index name
                # name_parts = result['name'].split('_')
                # # Look for index name (sherman/faunus)
                # index_name = None
                # for part in name_parts:
                #     if part in ['sherman', 'faunus']:
                #         index_name = part.capitalize()
                #         break
                
                result_label = f"{exp_label} - {label}"

                # Apply CDF range filtering if specified
                # Filters by x-axis value ranges (e.g., show RTT 0-5 and 20-30, skip 5-20)
                if cdf_ranges:
                    # Use a set to track which indices to include (avoids duplicates if ranges overlap)
                    indices_to_include = set()
                    
                    overall_min_x = float('inf')
                    overall_max_x = float('-inf')

                    for range_spec in cdf_ranges:
                        x_start = range_spec.get('x_min', float('-inf'))
                        overall_min_x = min(overall_min_x, x_start)
                        x_end = range_spec.get('x_max', float('inf'))
                        overall_max_x = max(overall_max_x, x_end)
                        
                        # Find indices where x value is within this range
                        for j, x_val in enumerate(x_cdf):
                            if x_start <= x_val <= x_end:
                                indices_to_include.add(j)
                    
                    # Sort indices and extract corresponding x/y values
                    sorted_indices = sorted(indices_to_include)
                    if sorted_indices:
                        x_cdf = [x_cdf[i] for i in sorted_indices]
                        y_cdf = [y_cdf[i] for i in sorted_indices]
                    
                    ax.set_xlim(overall_min_x * 0.9, overall_max_x * 1.1)

                ax.plot(x_cdf, y_cdf, marker='o', markersize=2, label=result_label, linewidth=2, markevery=5)
                
                # Add annotations for CDF if requested (show key percentiles)
                if annotate and len(x_cdf) > 0:
                    # Annotate key percentiles: p50, p95, p99
                    for percentile in [0.5, 0.95, 0.99]:
                        # Find closest CDF point to this percentile
                        idx = min(range(len(y_cdf)), key=lambda i: abs(y_cdf[i] - percentile))
                        if abs(y_cdf[idx] - percentile) < 0.05:  # Within 5% of target
                            ax.annotate(f'p{int(percentile*100)}={x_cdf[idx]:.0f}',
                                      xy=(x_cdf[idx], y_cdf[idx]),
                                      xytext=(10, -5), textcoords='offset points',
                                      fontsize=7, alpha=0.7)
        
        # Make x-axis ticks more dense for CDF plots
        ax.xaxis.set_major_locator(plt.MaxNLocator(nbins=15))
        
        # Y-axis always 0 to 1 for CDF plots (cumulative probability)
        # ax.set_ylim(0, 1)
        
        return
    
    y_values = [_get_nested(r['metrics'], metric, 0) for r in results]
    
    if plot_type == 'bar':
        # For bar plots, x_values are either actual values or categorical positions
        # If x_values are integers 0,1,2,... it's categorical (log scale bars)
        is_categorical_bar = (isinstance(x_values, list) and len(x_values) > 1 and 
                             all(isinstance(x, int) and x == i for i, x in enumerate(x_values)))
        
        if is_categorical_bar:
            # Categorical positions (used for log-scale bars)
            ax.bar(x_values, y_values, label=label, alpha=0.7, width=0.7)
            if annotate:
                for x, y in zip(x_values, y_values):
                    ax.text(x, y, f'{y:.0f}', ha='center', va='bottom', fontsize=8)
        else:
            # Regular bar plot
            ax.bar(x_values, y_values, label=label, alpha=0.7)
            if annotate:
                for x, y in zip(x_values, y_values):
                    ax.text(x, y, f'{y:.0f}', ha='center', va='bottom', fontsize=8)
    else:
        ax.plot(x_values, y_values, marker='o', label=label, linewidth=2, markersize=6)
        if annotate:
            for x, y in zip(x_values, y_values):
                ax.text(x, y, f'{y:.1f}', ha='center', va='bottom', fontsize=8)
