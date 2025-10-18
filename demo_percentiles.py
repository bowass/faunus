#!/usr/bin/env python3
"""Demonstrate the new percentile statistics functionality."""

import json
from pathlib import Path

def demonstrate_percentiles():
    """Show the enhanced statistics with percentiles."""
    print("=" * 50)
    
    # Load summary statistics
    summary_path = Path("thread_stats/summary.json")
    if not summary_path.exists():
        print("No statistics found. Run kv_test first.")
        return
    
    with summary_path.open() as f:
        summary = json.load(f)
    
    # Display global summary
    print(f"Global Summary:")
    print(f"   Total operations: {summary.get('total_attempted', 0):,}")
    print(f"   Success rate: {summary.get('total_succeeded', 0)/summary.get('total_attempted', 1)*100:.1f}%")
    print(f"   Duration: {summary.get('elapsed_sec', 0):.2f}s")
    print()
    
    # Display enhanced per-operation latency statistics
    per_op_latency = summary.get("per_operation_latency", {})

    print("Per-Operation Latency Statistics (microseconds):")
    print("-" * 80)
    print(f"{'Operation':<10} {'Count':<8} {'Avg':<8} {'P50':<8} {'P95':<8} {'P99':<8} {'Samples':<8}")
    print("-" * 80)
    
    for op_name, stats in per_op_latency.items():
        if stats.get("successes", 0) > 0:
            count = stats.get("successes", 0)
            avg = stats.get("avg_latency_us", 0)
            p50 = stats.get("p50_latency_us", 0)
            p95 = stats.get("p95_latency_us", 0)
            p99 = stats.get("p99_latency_us", 0)
            samples = stats.get("sample_count", 0)
            
            print(f"{op_name:<10} {count:<8,} {avg:<8.1f} {p50:<8.1f} {p95:<8.1f} {p99:<8.1f} {samples:<8,}")
    
    print("-" * 80)
    print()
    
    # Display per-thread breakdown for the first few threads
    print("Per-Thread Percentile Analysis:")
    print("-" * 60)
    
    thread_files = sorted(Path("thread_stats").glob("cs_*_thread_*.json"))[:4]  # Show first 4 threads
    
    for thread_file in thread_files:
        with thread_file.open() as f:
            thread_data = json.load(f)
        
        cs_id = thread_data.get("compute_server", 0)
        thread_id = thread_data.get("thread", 0)
        operations = thread_data.get("operations", {})
        
        print(f"CS {cs_id} Thread {thread_id}:")
        
        for op_name, op_stats in operations.items():
            if op_stats.get("successes", 0) > 0:
                p50 = op_stats.get("p50_latency_us")
                p95 = op_stats.get("p95_latency_us") 
                p99 = op_stats.get("p99_latency_us")
                samples = op_stats.get("sample_count", 0)
                
                print(f"  {op_name:<8}: P50={p50:<6.1f} P95={p95:<6.1f} P99={p99:<6.1f} ({samples} samples)")
        print()
    
    # Show visualization files generated
    print("Generated Visualization Files:")
    viz_files = list(Path("thread_stats").glob("*.png"))
    for viz_file in sorted(viz_files):
        print(f"   {viz_file.name}")

    print(f"\nEnhanced statistics successfully demonstrated!")
    print(f"   • Efficient per-thread latency sampling (max 10,000 samples per operation)")
    print(f"   • Accurate P50, P95, P99 percentile calculation across all clients") 
    print(f"   • Enhanced JSON output with percentile data")
    print(f"   • Updated visualization scripts with percentile plots")

if __name__ == "__main__":
    demonstrate_percentiles()
