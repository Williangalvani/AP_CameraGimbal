#!/bin/sh
# On the camera: one sample of memory and CPU for camera-app and MCM.
# Usage: measure.sh LABEL [SECONDS]  (CPU is averaged over SECONDS, default 10)
LABEL=${1:-sample}
SECONDS_=${2:-10}
pid_of()
{
    for p in /proc/[0-9]*; do
        case $(cat "$p/comm" 2>/dev/null) in "$1"*) echo "${p#/proc/}"; return ;; esac
    done
}
ticks()
{
    if [ -n "$1" ] && [ -r "/proc/$1/stat" ]; then
        awk '{print $14 + $15}' "/proc/$1/stat"
    else
        echo 0
    fi
}
total()
{
    awk '/^cpu / {print $2 + $3 + $4 + $5 + $6 + $7 + $8}' /proc/stat
}
app=$(pid_of camera-app)
mcm=$(pid_of mavlink-camera)
a0=$(ticks "$app"); m0=$(ticks "$mcm"); t0=$(total)
sleep "$SECONDS_"
a1=$(ticks "$app"); m1=$(ticks "$mcm"); t1=$(total)
cores=$(grep -c ^processor /proc/cpuinfo)
echo "== $LABEL $(cut -d' ' -f1 /proc/uptime)"
grep -E 'MemTotal|MemAvailable|Shmem:' /proc/meminfo
awk -v a=$((a1 - a0)) -v m=$((m1 - m0)) -v t=$((t1 - t0)) -v c="$cores" \
    'BEGIN {printf "cpu over interval (100%% = one core): camera-app %.1f%%  mcm %.1f%%\n", 100*c*a/t, 100*c*m/t}'
for pid in "$app" "$mcm"; do
    [ -n "$pid" ] || continue
    printf '%s pid %s: ' "$(cat /proc/$pid/comm)" "$pid"
    grep -E '^(VmRSS|VmHWM|Threads)' /proc/$pid/status | tr -s ' \t' ' ' | tr '\n' ' '
    grep -E '^Pss:' /proc/$pid/smaps_rollup 2>/dev/null | tr -s ' '
    echo
done
if [ -n "$mcm" ]; then
    # libraries MCM actually mapped, for pruning the lean bundle
    awk '$6 ~ /\.so/ {print $6}' /proc/$mcm/maps | sort -u >/run/mcm/state/maps-$LABEL.txt
    echo "mcm maps $(wc -l </run/mcm/state/maps-$LABEL.txt) libraries -> /run/mcm/state/maps-$LABEL.txt"
fi
