#!/bin/sh
# microSD speed check for the Hi3516DV500 camera.
#   sdbench.sh [SIZE_MIB]   write SIZE_MIB (1-4096, default 256) to the card,
#                           read it back and print both speeds in MB/s (MiB/s)
# Do not run it while recording: it refuses while any process has a file open
# on the card. It holds SDBENCH_LOCK while it runs, which camera-app and
# hisi-web check before they write to the card or format it, and sdcard.sh's
# lock, so no mount or format runs meanwhile. With SDBENCH_LOCK_INHERITED=1
# the caller already holds SDBENCH_LOCK for it (hisi-web does).
# It only touches one temporary file, SDCARD_MEDIA/.sdbench.tmp, removed when
# it exits. The card's name, size, bus timing and clock come first, from sysfs,
# debugfs and dmesg where they are available; never its serial number.
# The write and the read are each stopped after SDBENCH_TIMEOUT seconds
# (default 180): a failing card can block in I/O for minutes.
# Exit status: 0 measured, 1 failed, 2 usage, 3 card not ready, 4 card busy
# (sdcard.sh, or another benchmark), 5 a file on the card is open, 6 not
# enough space. Refusals and failures print "sdbench: REASON" on stderr.
set -u
MEDIA=${SDCARD_MEDIA:-/mnt}
STATE=${SDCARD_STATE:-/run/sdcard.state}
LOCK=${SDCARD_LOCK:-/run/sdcard.lock}
BENCH_LOCK=${SDBENCH_LOCK:-/run/sdbench.lock}
TIMEOUT=${SDBENCH_TIMEOUT:-180}
PROC=${SDBENCH_PROC:-/proc}
SYSFS=${SDBENCH_SYSFS:-/sys/class/mmc_host}
DEBUGFS=${SDBENCH_DEBUGFS:-/sys/kernel/debug}
UPTIME=${SDBENCH_UPTIME:-/proc/uptime}
DROP_CACHES=${SDBENCH_DROP_CACHES:-/proc/sys/vm/drop_caches}
FILE=$MEDIA/.sdbench.tmp
SIZE=${1:-256}
# Room left on the card beyond the test file.
SPARE_MIB=64

# STATUS REASON
refuse()
{
    code=$1
    shift
    echo "sdbench: $*" >&2
    exit "$code"
}
fail()
{
    refuse 1 "$@"
}
# Uptime in centiseconds; BusyBox date has no %N.
ticks()
{
    read -r up _ <"$UPTIME" || return 1
    echo $((${up%.*} * 100 + 1${up#*.} - 100))
}
# A lock on FD. sdbench_running() in hisi-web and camera-app probe it with a
# shared lock for a moment: one more try outlasts the probe.
take_lock()
{
    flock -n "$1" || { sleep 1; flock -n "$1"; }
}
card_ready()
{
    state=$(cat "$STATE" 2>/dev/null)
    [ "${state%% *}" = ready ] || refuse 3 "card not ready (state: ${state:-unknown})"
}
# Whether a process other than this shell has a file open under MEDIA; holder
# names it.
busy()
{
    for fd in "$PROC"/[0-9]*/fd; do
        pid=${fd%/fd}
        [ "${pid##*/}" = "$$" ] && continue
        # shellcheck disable=SC2010 # the fd links' targets; BusyBox has no find -lname
        if ls -l "$fd" 2>/dev/null | grep -qF -- " -> $MEDIA/"; then
            holder="pid ${pid##*/}"
            name=$(cat "$pid/comm" 2>/dev/null) && [ -n "$name" ] && holder="$name, $holder"
            return 0
        fi
    done
    return 1
}
card_info()
{
    for dev in "$SYSFS"/mmc*/mmc*:*; do
        [ "$(cat "$dev/type" 2>/dev/null)" = SD ] && break
        dev=
    done
    if [ -n "$dev" ]; then
        sectors=$(cat "$dev"/block/*/size 2>/dev/null | head -n 1)
        case $sectors in ''|*[!0-9]*) sectors=0 ;; esac
        info=SD
        [ "$sectors" -gt 4194304 ] && info=SDHC
        [ "$sectors" -gt 67108864 ] && info=SDXC
        gib=$(((sectors * 20 / 2097152 + 1) / 2))
        [ "$sectors" -gt 0 ] && info="$info, $((gib / 10)).$((gib % 10)) GiB"
        for field in manfid oemid; do
            value=$(cat "$dev/$field" 2>/dev/null) && info="$info, $field $value"
        done
        echo "card: $(cat "$dev/name" 2>/dev/null || echo unknown) ($info)"
        host=${dev%/*}
        awk -F: '
            { v = $2; sub(/^[ \t]+/, "", v); w = v; sub(/^[^(]*\(/, "", w); sub(/\)$/, "", w) }
            $1 == "clock" { clock = v + 0 }
            $1 == "actual clock" && v + 0 > 0 { clock = v + 0 }
            $1 == "bus width" { width = w }
            $1 == "timing spec" { timing = w }
            END { if (timing != "") printf "bus: %s, %s, %.1f MHz\n", timing, width, clock / 1e6 }
        ' "$DEBUGFS/${host##*/}/ios" 2>/dev/null
    fi
    dmesg 2>/dev/null | awk '
        /Problem setting bus speed/ { problem = $0 }
        /mmc/ { last = $0 }
        END {
            if (problem != "") print "kernel: " problem
            if (last != "" && last != problem) print "kernel: " last
        }'
}
# How dd is given its time limit: timeout(1) as coreutils and BusyBox 1.30
# spell it, BusyBox's older "timeout -t SECS", or else a watchdog. Coreutils
# needs --foreground to leave dd in this process group, which a signal to
# the benchmark reaches.
find_timeout()
{
    limit=
    command -v timeout >/dev/null 2>&1 || return 0
    if timeout --foreground 5 true >/dev/null 2>&1 7>&- 8>&-; then
        limit="timeout --foreground $TIMEOUT"
    elif timeout 5 true >/dev/null 2>&1 7>&- 8>&-; then
        limit="timeout $TIMEOUT"
    elif timeout -t 5 true >/dev/null 2>&1 7>&- 8>&-; then
        limit="timeout -t $TIMEOUT"
    fi
}
# dd ARGS, stopped with SIGTERM after TIMEOUT seconds. It does not need the
# locks: one that outlives this shell must not hold them.
run_dd()
{
    if [ -n "$limit" ]; then
        # shellcheck disable=SC2086 # the command and its seconds
        $limit dd "$@" 7>&- 8>&-
        return
    fi
    dd "$@" 7>&- 8>&- &
    dd_pid=$!
    (
        waited=0
        while kill -0 "$dd_pid" 2>/dev/null; do
            if [ "$waited" -ge "$TIMEOUT" ]; then
                kill "$dd_pid" 2>/dev/null
                break
            fi
            sleep 1
            waited=$((waited + 1))
        done
    ) 7>&- 8>&- &
    watchdog=$!
    wait "$dd_pid"
    dd_status=$?
    wait "$watchdog"
    return "$dd_status"
}
# NAME STATUS OUTPUT: why dd failed. BusyBox dd prints its error before the
# record counts; timeout(1) exits 124, and a dd it or the watchdog stopped 143.
dd_failed()
{
    case $2 in
    124|143) fail "$1 timed out after $TIMEOUT s" ;;
    esac
    fail "$1 failed: $(printf '%s\n' "$3" | head -n 1)"
}
# NAME CENTISECONDS: the speed and the time, rounded to one decimal.
report()
{
    cs=$2
    [ "$cs" -gt 0 ] || cs=1
    rate=$(((SIZE * 2000 / cs + 1) / 2))
    tenths=$(((cs + 5) / 10))
    echo "$1: $((rate / 10)).$((rate % 10)) MB/s ($SIZE MiB in $((tenths / 10)).$((tenths % 10)) s)"
}

case $SIZE in
''|*[!0-9]*|0*|?????*) SIZE=0 ;;
esac
if [ "$SIZE" -lt 1 ] || [ "$SIZE" -gt 4096 ]; then
    echo "usage: $0 [SIZE_MIB]   (1-4096, default 256)" >&2
    exit 2
fi
case $TIMEOUT in
''|*[!0-9]*|0*|??????*) TIMEOUT=180 ;;
esac
card_ready
if [ "${SDBENCH_LOCK_INHERITED:-}" != 1 ]; then
    exec 8>"$BENCH_LOCK"
    take_lock 8 || refuse 4 "card busy (another benchmark is running)"
fi
exec 7>"$LOCK"
take_lock 7 || refuse 4 "card busy (sdcard.sh is formatting or mounting it)"
# Only once the locks are ours: another sdbench may be using the file. One
# left by a run that was killed, or lost power, counts against the space.
trap 'rm -f "$FILE"' EXIT
trap 'exit 1' INT TERM
rm -f "$FILE"
card_ready
busy && refuse 5 "a file on the card is open (recording, logging or a download): $holder"
free=$(df -k "$MEDIA" 2>/dev/null | awk 'NR > 1 && NF >= 4 { free = $(NF - 2) } END { print free }')
case $free in ''|*[!0-9]*) fail "cannot read the free space on $MEDIA" ;; esac
[ "$free" -ge $(((SIZE + SPARE_MIB) * 1024)) ] ||
    refuse 6 "not enough space on $MEDIA: $((free / 1024)) MiB free, $((SIZE + SPARE_MIB)) MiB needed"

card_info
find_timeout
start=$(ticks) || fail "cannot read $UPTIME"
out=$(run_dd if=/dev/zero of="$FILE" bs=1M count="$SIZE" conv=fsync 2>&1) || dd_failed write $? "$out"
end=$(ticks)
report write $((end - start))
sync
# The page cache only; dentries and inodes stay.
{ echo 1 >"$DROP_CACHES"; } 2>/dev/null ||
    echo "sdbench: cannot drop the page cache; the read speed may be cached" >&2
start=$(ticks)
out=$(run_dd if="$FILE" of=/dev/null bs=1M 2>&1) || dd_failed read $? "$out"
end=$(ticks)
report read $((end - start))
echo ok
