#!/bin/sh
# microSD speed check for the Hi3516DV500 camera.
#   sdbench.sh [SIZE_MIB]   write SIZE_MIB (1-4096, default 256) to the card,
#                           read it back and print both speeds in MB/s (MiB/s)
# Do not run it while recording: it refuses while any process has a file open
# on the card, and holds sdcard.sh's lock so no mount or format runs meanwhile.
# It only touches one temporary file, SDCARD_MEDIA/.sdbench.tmp, removed when
# it exits. The card's name, size, bus timing and clock come first, from sysfs,
# debugfs and dmesg where they are available; never its serial number.
set -u
MEDIA=${SDCARD_MEDIA:-/mnt}
STATE=${SDCARD_STATE:-/run/sdcard.state}
LOCK=${SDCARD_LOCK:-/run/sdcard.lock}
PROC=${SDBENCH_PROC:-/proc}
SYSFS=${SDBENCH_SYSFS:-/sys/class/mmc_host}
DEBUGFS=${SDBENCH_DEBUGFS:-/sys/kernel/debug}
UPTIME=${SDBENCH_UPTIME:-/proc/uptime}
DROP_CACHES=${SDBENCH_DROP_CACHES:-/proc/sys/vm/drop_caches}
FILE=$MEDIA/.sdbench.tmp
SIZE=${1:-256}
# Room left on the card beyond the test file.
SPARE_MIB=64

fail()
{
    echo "sdbench: $*" >&2
    exit 1
}
# Uptime in centiseconds; BusyBox date has no %N.
ticks()
{
    read -r up _ <"$UPTIME" || return 1
    echo $((${up%.*} * 100 + 1${up#*.} - 100))
}
# Whether a process other than this shell has a file open under MEDIA.
busy()
{
    for fd in "$PROC"/[0-9]*/fd; do
        pid=${fd%/fd}
        [ "${pid##*/}" = "$$" ] && continue
        # shellcheck disable=SC2010 # the fd links' targets; BusyBox has no find -lname
        ls -l "$fd" 2>/dev/null | grep -qF -- " -> $MEDIA/" && return 0
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
state=$(cat "$STATE" 2>/dev/null)
[ "${state%% *}" = ready ] || fail "card not ready (state: ${state:-unknown})"
exec 7>"$LOCK"
flock -n 7 || fail "card busy (sdcard.sh is formatting or mounting it)"
# Only once the lock is ours: another sdbench may be using the file.
trap 'rm -f "$FILE"' EXIT
trap 'exit 1' INT TERM
busy && fail "recording in progress; stop it first"
free=$(df -k "$MEDIA" 2>/dev/null | awk 'NR > 1 && NF >= 4 { free = $(NF - 2) } END { print free }')
case $free in ''|*[!0-9]*) fail "cannot read the free space on $MEDIA" ;; esac
[ "$free" -ge $(((SIZE + SPARE_MIB) * 1024)) ] ||
    fail "not enough space on $MEDIA: $((free / 1024)) MiB free, $((SIZE + SPARE_MIB)) MiB needed"

card_info
start=$(ticks) || fail "cannot read $UPTIME"
out=$(dd if=/dev/zero of="$FILE" bs=1M count="$SIZE" conv=fsync 2>&1) || fail "write failed: $out"
end=$(ticks)
report write $((end - start))
sync
{ echo 3 >"$DROP_CACHES"; } 2>/dev/null ||
    echo "sdbench: cannot drop the page cache; the read speed may be cached" >&2
start=$(ticks)
out=$(dd if="$FILE" of=/dev/null bs=1M 2>&1) || fail "read failed: $out"
end=$(ticks)
report read $((end - start))
echo ok
