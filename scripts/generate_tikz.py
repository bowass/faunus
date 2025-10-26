#!/usr/bin/env python3
"""
TikZ Plot Generator

This script generates TikZ/PGFPlots code from CSV data files.
The generated code can be included in LaTeX documents for publication-quality plots.

Usage:
    python3 generate_tikz.py --data graph_data/throughput_vs_threads.csv --type line --output plot.tex
    python3 generate_tikz.py --data graph_data/ --all --output plots/
"""

import sys
import os
import csv
import argparse
from pathlib import Path
from typing import Dict, List, Any, Tuple
from collections import defaultdict

class TikZGenerator:
    def __init__(self):
        self.colors = [
            'blue', 'red', 'green', 'orange', 'purple', 'brown', 'pink', 'gray', 'olive', 'cyan'
        ]
        self.markers = [
            'o', 'square', 'triangle', 'diamond', 'pentagon', 'star', '+', 'x', '|', '-'
        ]
    
    def _escape_latex(self, text: str) -> str:
        """Escape special LaTeX characters in text"""
        # Escape underscores and other special characters
        text = text.replace('_', '\\_')
        text = text.replace('&', '\\&')
        text = text.replace('%', '\\%')
        text = text.replace('#', '\\#')
        return text
    
    def load_csv_data(self, csv_file: str) -> Dict[str, List[Dict[str, Any]]]:
        """Load CSV data and group by series"""
        data = defaultdict(list)
        
        with open(csv_file, 'r') as f:
            reader = csv.DictReader(f)
            for row in reader:
                series = row['series']
                data[series].append(row)
        
        # Sort each series by parameter value
        for series in data:
            data[series].sort(key=lambda x: float(x['param_value']))
        
        return dict(data)
    
    def generate_line_plot(self, data: Dict[str, List[Dict[str, Any]]], 
                          title: str, xlabel: str, ylabel: str, 
                          x_key: str = 'param_value', y_key: str = 'succeeded_throughput') -> str:
        """Generate TikZ code for a line plot"""
        
        tikz_code = []
        tikz_code.append("\\begin{tikzpicture}")
        tikz_code.append("\\begin{axis}[")
        tikz_code.append(f"    title={{{title}}},")
        tikz_code.append(f"    xlabel={{{xlabel}}},")
        tikz_code.append(f"    ylabel={{{ylabel}}},")
        tikz_code.append("    xmin=0,")
        tikz_code.append("    grid=major,")
        tikz_code.append("    legend pos=north west,")
        tikz_code.append("    width=10cm,")
        tikz_code.append("    height=6cm,")
        tikz_code.append("    mark size=2.5pt,")
        tikz_code.append("    line width=1.2pt")
        tikz_code.append("]")
        
        for i, (series_name, series_data) in enumerate(data.items()):
            color = self.colors[i % len(self.colors)]
            marker = self.markers[i % len(self.markers)]
            
            tikz_code.append("")
            tikz_code.append(f"\\addplot[")
            tikz_code.append(f"    color={color},")
            tikz_code.append(f"    mark={marker},")
            tikz_code.append("    mark options={solid}")
            tikz_code.append("] coordinates {")
            
            for row in series_data:
                x_val = float(row[x_key])
                # Support different throughput metrics
                y_val = float(row.get(y_key, row.get('throughput_kops', 0)))
                tikz_code.append(f"    ({x_val}, {y_val})")
            
            tikz_code.append("};")
            tikz_code.append(f"\\addlegendentry{{{self._escape_latex(series_name)}}}")
        
        tikz_code.append("")
        tikz_code.append("\\end{axis}")
        tikz_code.append("\\end{tikzpicture}")
        
        return "\n".join(tikz_code)
    
    def generate_scatter_plot(self, data: Dict[str, List[Dict[str, Any]]], 
                             title: str, xlabel: str, ylabel: str,
                             x_key: str = 'insert_p99_us', y_key: str = 'succeeded_throughput') -> str:
        """Generate TikZ code for a scatter plot (e.g., throughput vs latency)"""
        
        tikz_code = []
        tikz_code.append("\\begin{tikzpicture}")
        tikz_code.append("\\begin{axis}[")
        tikz_code.append(f"    title={{{title}}},")
        tikz_code.append(f"    xlabel={{{xlabel}}},")
        tikz_code.append(f"    ylabel={{{ylabel}}},")
        tikz_code.append("    grid=major,")
        tikz_code.append("    legend pos=north west,")
        tikz_code.append("    width=10cm,")
        tikz_code.append("    height=6cm,")
        tikz_code.append("    mark size=3pt,")
        tikz_code.append("    only marks")
        tikz_code.append("]")
        
        for i, (series_name, series_data) in enumerate(data.items()):
            color = self.colors[i % len(self.colors)]
            marker = self.markers[i % len(self.markers)]
            
            tikz_code.append("")
            tikz_code.append(f"\\addplot[")
            tikz_code.append(f"    color={color},")
            tikz_code.append(f"    mark={marker},")
            tikz_code.append("    mark options={solid}")
            tikz_code.append("] coordinates {")
            
            for row in series_data:
                # Support both new and old metric names
                x_val = float(row.get(x_key, row.get('p50_latency_us', 0)))
                y_val = float(row.get(y_key, row.get('throughput_kops', 0)))
                if x_val > 0 and y_val > 0:  # Only plot valid data points
                    tikz_code.append(f"    ({x_val}, {y_val})")
            
            tikz_code.append("};")
            tikz_code.append(f"\\addlegendentry{{{self._escape_latex(series_name)}}}")
        
        tikz_code.append("")
        tikz_code.append("\\end{axis}")
        tikz_code.append("\\end{tikzpicture}")
        
        return "\n".join(tikz_code)
    
    def generate_bar_chart(self, data: Dict[str, List[Dict[str, Any]]], 
                          title: str, xlabel: str, ylabel: str,
                          x_key: str = 'param_value', y_key: str = 'throughput_kops') -> str:
        """Generate TikZ code for a bar chart"""
        
        tikz_code = []
        tikz_code.append("\\begin{tikzpicture}")
        tikz_code.append("\\begin{axis}[")
        tikz_code.append(f"    title={{{title}}},")
        tikz_code.append(f"    xlabel={{{xlabel}}},")
        tikz_code.append(f"    ylabel={{{ylabel}}},")
        tikz_code.append("    ybar,")
        tikz_code.append("    bar width=15pt,")
        tikz_code.append("    grid=major,")
        tikz_code.append("    legend pos=north west,")
        tikz_code.append("    width=10cm,")
        tikz_code.append("    height=6cm,")
        tikz_code.append("    symbolic x coords={" + ",".join([str(row[x_key]) for row in next(iter(data.values()))]) + "},")
        tikz_code.append("    xtick=data")
        tikz_code.append("]")
        
        for i, (series_name, series_data) in enumerate(data.items()):
            color = self.colors[i % len(self.colors)]
            
            tikz_code.append("")
            tikz_code.append(f"\\addplot[")
            tikz_code.append(f"    fill={color},")
            tikz_code.append(f"    draw={color}")
            tikz_code.append("] coordinates {")
            
            for row in series_data:
                x_val = row[x_key]
                y_val = float(row.get(y_key, 0))
                tikz_code.append(f"    ({x_val}, {y_val})")
            
            tikz_code.append("};")
            tikz_code.append(f"\\addlegendentry{{{self._escape_latex(series_name)}}}")
        
        tikz_code.append("")
        tikz_code.append("\\end{axis}")
        tikz_code.append("\\end{tikzpicture}")
        
        return "\n".join(tikz_code)
    
    def create_full_latex_document(self, tikz_code: str, title: str) -> str:
        """Create a complete LaTeX document with the TikZ plot"""
        
        latex_doc = f"""\\documentclass{{article}}
\\usepackage{{pgfplots}}
\\usepackage{{tikz}}
\\pgfplotsset{{compat=1.18}}

\\title{{{title}}}
\\author{{Performance Analysis}}
\\date{{\\today}}

\\begin{{document}}

\\maketitle

\\begin{{center}}
{tikz_code}
\\end{{center}}

\\end{{document}}"""
        
        return latex_doc
    
    def determine_plot_config(self, csv_file: str) -> Tuple[str, str, str, str, str, str]:
        """Determine plot configuration based on CSV filename and content"""
        
        filename = Path(csv_file).stem
        
        if "throughput_vs_threads" in filename:
            return (
                "line",
                "Throughput vs Number of Threads",
                "Number of Threads",
                "Throughput (Kops/sec)",
                "param_value",
                "throughput_kops"
            )
        elif "throughput_vs_maintenance" in filename:
            return (
                "line",
                "Throughput vs Maintenance Threads",
                "Number of Maintenance Threads", 
                "Throughput (Kops/sec)",
                "param_value",
                "throughput_kops"
            )
        elif "throughput_vs_key_size" in filename:
            return (
                "line",
                "Throughput vs Key Size",
                "Key Size (bytes)",
                "Throughput (Kops/sec)",
                "param_value", 
                "throughput_kops"
            )
        elif "latency" in filename:
            return (
                "scatter",
                "Throughput vs Latency",
                "Latency (μs)",
                "Throughput (Kops/sec)",
                "p50_latency_us",
                "throughput_kops"
            )
        else:
            return (
                "line",
                "Performance Comparison",
                "Parameter",
                "Throughput (Kops/sec)",
                "param_value",
                "throughput_kops"
            )

def main():
    parser = argparse.ArgumentParser(description="Generate TikZ plots from CSV data")
    
    parser.add_argument('--data', help='CSV data file or directory')
    parser.add_argument('--type', choices=['line', 'scatter', 'bar'], help='Plot type')
    parser.add_argument('--output', help='Output file or directory')
    parser.add_argument('--all', action='store_true', help='Process all CSV files in directory')
    parser.add_argument('--full-latex', action='store_true', help='Generate complete LaTeX document')
    parser.add_argument('--title', help='Plot title')
    parser.add_argument('--xlabel', help='X-axis label')
    parser.add_argument('--ylabel', help='Y-axis label')
    
    args = parser.parse_args()
    
    if not args.data:
        print("Error: --data is required")
        sys.exit(1)
    
    generator = TikZGenerator()
    
    data_path = Path(args.data)
    
    if args.all and data_path.is_dir():
        # Process all CSV files in directory
        output_dir = Path(args.output) if args.output else Path("tikz_plots")
        output_dir.mkdir(exist_ok=True)
        
        for csv_file in data_path.glob("*.csv"):
            print(f"Processing {csv_file.name}...")
            
            data = generator.load_csv_data(str(csv_file))
            if not data:
                print(f"  No data found in {csv_file.name}")
                continue
            
            # Auto-determine plot configuration
            plot_type, title, xlabel, ylabel, x_key, y_key = generator.determine_plot_config(str(csv_file))
            
            # Generate TikZ code
            if plot_type == "line":
                tikz_code = generator.generate_line_plot(data, title, xlabel, ylabel, x_key, y_key)
            elif plot_type == "scatter":
                tikz_code = generator.generate_scatter_plot(data, title, xlabel, ylabel, x_key, y_key)
            elif plot_type == "bar":
                tikz_code = generator.generate_bar_chart(data, title, xlabel, ylabel, x_key, y_key)
            
            # Output file
            output_file = output_dir / f"{csv_file.stem}.tex"
            
            if args.full_latex:
                content = generator.create_full_latex_document(tikz_code, title)
            else:
                content = tikz_code
            
            with open(output_file, 'w') as f:
                f.write(content)
            
            print(f"  Generated: {output_file}")
    
    elif data_path.is_file() and data_path.suffix == '.csv':
        # Process single CSV file
        data = generator.load_csv_data(str(data_path))
        if not data:
            print("No data found in CSV file")
            sys.exit(1)
        
        # Use provided parameters or auto-determine
        if args.type and args.title and args.xlabel and args.ylabel:
            plot_type = args.type
            title = args.title
            xlabel = args.xlabel
            ylabel = args.ylabel
            x_key = "param_value"
            y_key = "throughput_kops"
        else:
            plot_type, title, xlabel, ylabel, x_key, y_key = generator.determine_plot_config(str(data_path))
        
        # Generate TikZ code
        if plot_type == "line":
            tikz_code = generator.generate_line_plot(data, title, xlabel, ylabel, x_key, y_key)
        elif plot_type == "scatter":
            tikz_code = generator.generate_scatter_plot(data, title, xlabel, ylabel, x_key, y_key)
        elif plot_type == "bar":
            tikz_code = generator.generate_bar_chart(data, title, xlabel, ylabel, x_key, y_key)
        
        # Output
        if args.full_latex:
            content = generator.create_full_latex_document(tikz_code, title)
        else:
            content = tikz_code
        
        if args.output:
            with open(args.output, 'w') as f:
                f.write(content)
            print(f"Generated: {args.output}")
        else:
            print(content)
    
    else:
        print("Error: --data must be a CSV file or directory")
        sys.exit(1)

if __name__ == "__main__":
    main()