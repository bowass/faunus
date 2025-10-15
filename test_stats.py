#!/usr/bin/env python3
"""Quick test script to verify enhanced statistics functionality."""

import json
import subprocess
import sys
from pathlib import Path

def run_test():
    """Run a quick test and validate the new statistics features."""
    print("🧪 Running statistics enhancement test...")
    
    # Create a minimal test config
    test_config = """
num_cs: 1
threads_per_cs: 2
num_ms: 1
mem_per_ms: 268435456
cas_delay_us: 1
base_rtt_us: 0.5
initial_slabs_per_size: 1024
maintenance_cs: 0
threads_per_maintenance_cs: 0
log_level: ERROR
ops_per_client: 1000
warmup_inserts: 100
distribution:
  type: uniform
  key_space: 10000
operation_mix:
  insert: 0.7
  read: 0.3
  update: 0.0
  delete: 0.0
"""
    
    # Write test config
    config_path = Path("test_config.yaml")
    with config_path.open("w") as f:
        f.write(test_config)
    
    try:
        # Run the test
        print("📊 Running kv_test with enhanced statistics...")
        result = subprocess.run(
            ["./kv_test", str(config_path)],
            capture_output=True,
            text=True,
            timeout=30
        )
        
        if result.returncode != 0:
            print(f"❌ Test failed with return code {result.returncode}")
            print("STDOUT:", result.stdout)
            print("STDERR:", result.stderr)
            return False
        
        # Check if statistics files were created
        stats_dir = Path("thread_stats")
        if not stats_dir.exists():
            print("❌ Statistics directory not created")
            return False
        
        summary_file = stats_dir / "summary.json"
        if not summary_file.exists():
            print("❌ Summary file not created")
            return False
        
        # Validate summary contains new percentile fields
        with summary_file.open() as f:
            summary = json.load(f)
        
        per_op_latency = summary.get("per_operation_latency", {})
        if not per_op_latency:
            print("❌ No per-operation latency data found")
            return False
        
        # Check for percentile fields
        for op_name, op_data in per_op_latency.items():
            required_fields = ["p50_latency_us", "p95_latency_us", "p99_latency_us", "sample_count"]
            missing_fields = [field for field in required_fields if field not in op_data]
            if missing_fields:
                print(f"❌ Missing percentile fields for {op_name}: {missing_fields}")
                return False
        
        # Check thread-level files
        thread_files = list(stats_dir.glob("cs_*_thread_*.json"))
        if not thread_files:
            print("❌ No thread-level statistics files found")
            return False
        
        # Validate thread files contain percentiles
        for thread_file in thread_files:
            with thread_file.open() as f:
                thread_data = json.load(f)
            
            operations = thread_data.get("operations", {})
            for op_name, op_data in operations.items():
                required_fields = ["p50_latency_us", "p95_latency_us", "p99_latency_us", "sample_count"]
                missing_fields = [field for field in required_fields if field not in op_data]
                if missing_fields:
                    print(f"❌ Missing percentile fields in {thread_file} for {op_name}: {missing_fields}")
                    return False
        
        print("✅ All statistics validation checks passed!")
        
        # Show a sample of the enhanced data
        print("\n📈 Sample enhanced statistics:")
        for op_name, op_data in per_op_latency.items():
            if op_data.get("sample_count", 0) > 0:
                print(f"  {op_name}:")
                print(f"    P50: {op_data.get('p50_latency_us', 'N/A')} µs")
                print(f"    P95: {op_data.get('p95_latency_us', 'N/A')} µs") 
                print(f"    P99: {op_data.get('p99_latency_us', 'N/A')} µs")
                print(f"    Samples: {op_data.get('sample_count', 0)}")
        
        # Test the visualization script
        print("\n🎨 Testing visualization script...")
        viz_result = subprocess.run(
            ["python3", "scripts/visualize_stats.py", "thread_stats"],
            capture_output=True,
            text=True
        )
        
        if viz_result.returncode == 0:
            print("✅ Visualization script ran successfully!")
            print("Generated plots:")
            plot_files = list(Path("thread_stats").glob("faunus_*.png"))
            for plot_file in plot_files:
                print(f"  - {plot_file}")
        else:
            print("⚠️  Visualization script had issues:")
            print(viz_result.stderr)
        
        return True
        
    except subprocess.TimeoutExpired:
        print("❌ Test timed out")
        return False
    except Exception as e:
        print(f"❌ Test failed with exception: {e}")
        return False
    finally:
        # Cleanup
        if config_path.exists():
            config_path.unlink()

if __name__ == "__main__":
    success = run_test()
    sys.exit(0 if success else 1)