#!/bin/bash
# Hardware parameter sampler for RPI-ICU benchmarking.
# Reads CPU (per-core), RAM, and temperature directly from kernel interfaces.
# Zero subprocess spawns per sample — all bash builtins and file reads.
#
# Output: CSV to stdout. All values are raw integers from the kernel.
#   - cN_busy/cN_total: jiffie deltas per core. Plotter computes: busy/total * 100 = CPU%.
#   - mem_avail_kb: raw from /proc/meminfo. Plotter computes: (total - avail) / 1024 = used MB.
#   - temp_raw: millidegrees Celsius. Plotter divides by 1000.
#
# Usage:
#   ./raw_hardware_sampler.sh [interval_seconds]
#   ./raw_hardware_sampler.sh 0.5 > hardware_benchmark.csv

INTERVAL=${1:-1}
THERMAL=/sys/class/thermal/thermal_zone0/temp

echo "timestamp,c0_busy,c0_total,c1_busy,c1_total,c2_busy,c2_total,c3_busy,c3_total,mem_avail_kb,temp_raw"

# Read all 4 cores from /proc/stat in one pass — no grep, no subprocess
read_cores() {
    while IFS=' ' read -r label user nice system idle iowait irq softirq steal _; do
        case "$label" in
            cpu0) TOTAL_0=$((user+nice+system+idle+iowait+irq+softirq+steal)); IDLE_0=$((idle+iowait)) ;;
            cpu1) TOTAL_1=$((user+nice+system+idle+iowait+irq+softirq+steal)); IDLE_1=$((idle+iowait)) ;;
            cpu2) TOTAL_2=$((user+nice+system+idle+iowait+irq+softirq+steal)); IDLE_2=$((idle+iowait)) ;;
            cpu3) TOTAL_3=$((user+nice+system+idle+iowait+irq+softirq+steal)); IDLE_3=$((idle+iowait)); break ;;
        esac
    done < /proc/stat
}

# Initial snapshot
read_cores
PREV_TOTAL_0=$TOTAL_0; PREV_IDLE_0=$IDLE_0
PREV_TOTAL_1=$TOTAL_1; PREV_IDLE_1=$IDLE_1
PREV_TOTAL_2=$TOTAL_2; PREV_IDLE_2=$IDLE_2
PREV_TOTAL_3=$TOTAL_3; PREV_IDLE_3=$IDLE_3
sleep "$INTERVAL"

while true; do
    read_cores

    # Deltas
    dt0=$((TOTAL_0-PREV_TOTAL_0)); db0=$((dt0-(IDLE_0-PREV_IDLE_0)))
    dt1=$((TOTAL_1-PREV_TOTAL_1)); db1=$((dt1-(IDLE_1-PREV_IDLE_1)))
    dt2=$((TOTAL_2-PREV_TOTAL_2)); db2=$((dt2-(IDLE_2-PREV_IDLE_2)))
    dt3=$((TOTAL_3-PREV_TOTAL_3)); db3=$((dt3-(IDLE_3-PREV_IDLE_3)))

    PREV_TOTAL_0=$TOTAL_0; PREV_IDLE_0=$IDLE_0
    PREV_TOTAL_1=$TOTAL_1; PREV_IDLE_1=$IDLE_1
    PREV_TOTAL_2=$TOTAL_2; PREV_IDLE_2=$IDLE_2
    PREV_TOTAL_3=$TOTAL_3; PREV_IDLE_3=$IDLE_3

    # RAM — parse MemAvailable without awk
    while IFS=' ' read -r key val _; do
        [[ "$key" == "MemAvailable:" ]] && break
    done < /proc/meminfo

    # Temp — read without cat
    read -r temp_raw < "$THERMAL" 2>/dev/null || temp_raw=-1

    # Timestamp — bash builtin, no date subprocess
    printf '%(%Y-%m-%d %H:%M:%S)T,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d\n' \
        -1 $db0 $dt0 $db1 $dt1 $db2 $dt2 $db3 $dt3 $val $temp_raw

    sleep "$INTERVAL"
done
