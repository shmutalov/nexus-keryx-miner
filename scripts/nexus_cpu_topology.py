#!/usr/bin/env python3
"""
Adaptive Host CPU Topology & Partitioning for Nexus Miners.
Supports any core count from 2/4 cores to 32+ cores.
"""
import os
import sys
import argparse
from pathlib import Path

def get_cpu_topology():
    total_threads = os.cpu_count() or 4
    cores = {}
    
    # Try reading sysfs core_id
    sys_cpu = Path("/sys/devices/system/cpu")
    if sys_cpu.exists():
        for cpu_dir in sorted(sys_cpu.glob("cpu[0-9]*")):
            cpu_name = cpu_dir.name
            try:
                cpu_id = int(cpu_name[3:])
                core_file = cpu_dir / "topology" / "core_id"
                if core_file.exists():
                    core_id = int(core_file.read_text().strip())
                    cores.setdefault(core_id, []).append(cpu_id)
            except Exception:
                pass
                
    if not cores:
        # Fallback grouping
        for cpu in range(total_threads):
            core = cpu // 2 if total_threads >= 4 else cpu
            cores.setdefault(core, []).append(cpu)
            
    return total_threads, list(cores.values())

def get_partition(role: str):
    # Check env disable
    if os.environ.get("NEXUS_CPU_AFFINITY_DISABLED") in ("1", "true"):
        return "all"
        
    # Check env override
    if role == "primary" and os.environ.get("NEXUS_CPU_AFFINITY_PRIMARY"):
        return os.environ.get("NEXUS_CPU_AFFINITY_PRIMARY")
    if role == "secondary" and os.environ.get("NEXUS_CPU_AFFINITY_SECONDARY"):
        return os.environ.get("NEXUS_CPU_AFFINITY_SECONDARY")

    total, core_list = get_cpu_topology()
    num_cores = len(core_list)
    
    # Low core count
    if total <= 4 or num_cores <= 2:
        return "all"
        
    def flatten(start, count):
        res = []
        for c in core_list[start:start+count]:
            res.extend(c)
        return ",".join(str(x) for x in res)

    # Medium core count (5-8 threads / 4-6 cores)
    if total <= 8 or num_cores <= 4:
        if role == "solo":
            return flatten(0, max(1, num_cores - 1))
        elif role == "primary":
            return flatten(0, max(1, num_cores // 2))
        elif role == "secondary":
            return flatten(max(1, num_cores // 2), 1)
        else: # node
            return flatten(num_cores - 1, 1)

    # High core count (9-16 threads)
    if total <= 16 or num_cores <= 8:
        prim_count = num_cores // 2
        sec_count = max(1, (num_cores - prim_count) // 2)
        node_start = prim_count + sec_count
        if role in ("solo", "primary"):
            return flatten(0, prim_count)
        elif role == "secondary":
            return flatten(prim_count, sec_count)
        else: # node
            return flatten(node_start, num_cores - node_start)

    # Extreme core count (> 16 threads)
    sys_cores = min(4, max(2, num_cores // 8))
    avail = num_cores - sys_cores
    prim_cores = (avail * 6) // 10
    sec_cores = avail - prim_cores
    if role == "solo":
        return flatten(0, avail)
    elif role == "primary":
        return flatten(0, prim_cores)
    elif role == "secondary":
        return flatten(prim_cores, sec_cores)
    else: # node
        return flatten(prim_cores + sec_cores, sys_cores)

def main():
    parser = argparse.ArgumentParser(description="Nexus Adaptive CPU Topology")
    parser.add_argument("--role", choices=["primary", "secondary", "node", "solo"], default="primary")
    args = parser.parse_args()
    print(get_partition(args.role))

if __name__ == "__main__":
    main()
