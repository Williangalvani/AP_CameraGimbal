#!/bin/sh
# microSD card for the Hi3516DV500 camera.
#   sdcard.sh mount    mount a new card, or detach one that was removed
#   sdcard.sh format   one FAT32 partition over the whole card, then mount
# service.sh runs "mount" every second; camera-app runs "format" on
# MAV_CMD_STORAGE_FORMAT. Both publish the card's state in SDCARD_STATE for
# camera-app and hisi-web, as its first word:
#   absent       no card
#   unformatted  a card with no filesystem that mounts read-write
#   error        a card that cannot be read
#   formatting   "format" is running
#   ready        mounted on SDCARD_MEDIA, with the output directories made
# "format" also leaves its result in SDCARD_STATE.format, written once it has
# the lock and has finished: "ok UPTIME", or "failed REASON UPTIME" with
# REASON one of absent, unmount, size, small, write, table, partition, mkfs
# or mount, and UPTIME the first field of /proc/uptime then.
# A card that does not mount is not retried every second (the kernel logs
# every attempt), only once it has been removed and inserted again.
# This board's cards can come up unreadable (command timeouts, error -110);
# binding SDCARD_HOST to its driver again initialises them, so that is tried
# up to RESETS_MAX times until a card mounts.
# Its cards also stay at the 400 kHz identification clock: the switch to a
# UHS bus speed fails ("Problem setting bus speed mode!") and the kernel then
# never raises the clock. Once a card is up, the SDIO clock select in the
# CRG register SDCARD_CLOCK_REG is moved from 400 kHz to 25 MHz, the cards'
# default speed, and moved back when the card goes: the kernel's clock
# framework caches the rate it set, so it would identify the next card at
# 25 MHz otherwise.
set -u
MEDIA=${SDCARD_MEDIA:-/mnt}
STATE=${SDCARD_STATE:-/run/sdcard.state}
LOCK=${SDCARD_LOCK:-/run/sdcard.lock}
DISKS=${SDCARD_DISKS:-/dev/mmcblk0 /dev/mmcblk1}
MOUNTS=${SDCARD_MOUNTS:-/proc/mounts}
UPTIME=${SDCARD_UPTIME:-/proc/uptime}
HOST=${SDCARD_HOST-}
HOST_DRIVER=${SDCARD_HOST_DRIVER:-/sys/bus/platform/drivers/sdhci_nebula}
CLOCK_REG=${SDCARD_CLOCK_REG-}
# Bits 24-26 of the register: 0 is 400 kHz, 1 is 25 MHz.
CLOCK_SHIFT=24
RESETS_MAX=2
# The first partition starts 4 MiB in, the erase-block alignment cards use.
PART_START=8192

log()
{
    echo "sdcard: $*" >&2
}
# REASON MESSAGE: why the format failed.
format_error()
{
    reason=$1
    shift
    log "$*"
    return 1
}
format_result()
{
    read -r up _ <"$UPTIME" 2>/dev/null || up=0
    printf '%s %s\n' "$*" "$up" >"$STATE.format.new" && mv -f "$STATE.format.new" "$STATE.format"
}
set_state()
{
    [ "$(cat "$STATE" 2>/dev/null)" = "$*" ] && return 0
    printf '%s\n' "$*" >"$STATE.new" && mv -f "$STATE.new" "$STATE"
}
find_disk()
{
    for disk in $DISKS; do
        [ -e "$disk" ] && return 0
    done
    disk=
    return 1
}
partition_of()
{
    case $1 in
    *[0-9]) echo "${1}p1" ;;
    *) echo "${1}1" ;;
    esac
}
# The device and options mounted on MEDIA, if any.
media_mount()
{
    mounted=
    options=
    while read -r device dir _ opts _; do
        [ "$dir" = "$MEDIA" ] || continue
        mounted=$device
        options=$opts
    done <"$MOUNTS"
}
# bspmm ADDRESS prints "ADDRESS: VALUE"; given a VALUE as well, it writes it.
read_clock()
{
    clock=$(bspmm "$CLOCK_REG" </dev/null 2>/dev/null |
        awk -v reg="$CLOCK_REG:" '$1 == reg { print $2 }')
    case $clock in
    0x*|0X*) return 0 ;;
    esac
    return 1
}
# Moves the clock select from FROM to TO, leaving the other bits.
set_clock()
{
    [ -n "$CLOCK_REG" ] && read_clock || return 1
    [ $(((clock >> CLOCK_SHIFT) & 7)) -eq "$1" ] || return 1
    bspmm "$CLOCK_REG" "$(printf '0x%08x' $(((clock & ~(7 << CLOCK_SHIFT)) | ($2 << CLOCK_SHIFT))))" \
        </dev/null >/dev/null 2>&1
}
raise_clock()
{
    [ -e "$STATE.clock" ] && return 0
    set_clock 0 1 || return 0
    touch "$STATE.clock"
    log "SDIO clock raised from 400 kHz to 25 MHz"
}
restore_clock()
{
    [ -e "$STATE.clock" ] || return 0
    rm -f "$STATE.clock"
    set_clock 1 0 && log "SDIO clock back to 400 kHz"
}
make_dirs()
{
    mkdir -p "$MEDIA/DCIM/record" "$MEDIA/DCIM/capture" "$MEDIA/logs"
}
try_mount()
{
    for device in "$(partition_of "$disk")" "$disk"; do
        [ -e "$device" ] || continue
        mount -o noatime "$device" "$MEDIA" 2>/dev/null || continue
        media_mount
        case ,$options, in
        *,rw,*) make_dirs && return 0 ;;
        esac
        log "$device mounted read-only; ignoring it"
        umount "$MEDIA" 2>/dev/null || umount -l "$MEDIA"
    done
    return 1
}
do_mount()
{
    if find_disk; then
        raise_clock
    else
        restore_clock
    fi
    media_mount
    if [ -n "$mounted" ]; then
        if [ -n "$disk" ] && [ -e "$mounted" ]; then
            set_state ready
            return 0
        fi
        # Lazily: camera-app may still hold a recording open on it.
        log "card removed; detaching $mounted"
        umount -l "$MEDIA" 2>/dev/null
    fi
    if [ -z "$disk" ]; then
        set_state absent
        return 0
    fi
    case $(cat "$STATE" 2>/dev/null) in
    "unformatted $disk"|"error $disk") return 0 ;;
    esac
    if try_mount; then
        log "mounted $mounted on $MEDIA"
        rm -f "$STATE.resets"
        set_state ready
    elif dd if="$disk" of=/dev/null bs=512 count=1 2>/dev/null; then
        log "$disk has no mountable filesystem"
        set_state unformatted "$disk"
    elif reset_host; then
        set_state absent
    else
        log "$disk cannot be read"
        set_state error "$disk"
    fi
}
reset_host()
{
    [ -n "$HOST" ] || return 1
    resets=$(cat "$STATE.resets" 2>/dev/null) || resets=0
    case $resets in ''|*[!0-9]*) resets=0 ;; esac
    [ "$resets" -lt "$RESETS_MAX" ] || return 1
    echo $((resets + 1)) >"$STATE.resets"
    log "$disk cannot be read; binding $HOST again"
    restore_clock
    echo "$HOST" >"$HOST_DRIVER/unbind"
    sleep 1
    echo "$HOST" >"$HOST_DRIVER/bind"
}
# Four little-endian bytes, as printf escapes.
le32()
{
    value=$1
    for _ in 1 2 3 4; do
        printf "\\$(printf %03o $((value & 255)))"
        value=$((value >> 8))
    done
}
disk_sectors()
{
    if [ -b "$disk" ]; then
        blockdev --getsz "$disk"
    else
        echo $(($(wc -c <"$disk") / 512))
    fi
}
do_format()
{
    find_disk || { set_state absent; format_error absent 'no card to format'; return; }
    raise_clock
    set_state formatting
    media_mount
    if [ -n "$mounted" ]; then
        umount "$MEDIA" 2>/dev/null || umount -l "$MEDIA" ||
            { format_error unmount "cannot unmount $MEDIA"; return; }
    fi
    sectors=$(disk_sectors) || { format_error size "cannot read the size of $disk"; return; }
    # FAT32 needs at least 65525 clusters; below 64 MiB is not a real card.
    [ "$sectors" -gt $((PART_START + 131072)) ] ||
        { format_error small "$disk is too small ($sectors sectors)"; return; }
    log "formatting $disk ($sectors sectors)"
    # Clears the old partition table and whatever filesystem headers follow it.
    dd if=/dev/zero of="$disk" bs=1M count=1 conv=notrunc 2>/dev/null ||
        { format_error write "cannot write $disk"; return; }
    # One FAT32 LBA (type 0x0c) partition; CHS fields marked unused.
    {
        dd if=/dev/zero bs=446 count=1 2>/dev/null
        printf '\000\376\377\377\014\376\377\377'
        le32 "$PART_START"
        le32 $((sectors - PART_START))
        dd if=/dev/zero bs=48 count=1 2>/dev/null
        printf '\125\252'
    } | dd of="$disk" conv=notrunc 2>/dev/null ||
        { format_error table "cannot write the partition table"; return; }
    sync
    partition=$(partition_of "$disk")
    if [ -b "$disk" ]; then
        blockdev --rereadpt "$disk" 2>/dev/null || partprobe "$disk" 2>/dev/null
        for _ in 1 2 3 4 5 6 7 8 9 10; do
            [ -b "$partition" ] && break
            sleep 1
        done
        [ -b "$partition" ] || { format_error partition "$partition did not appear"; return; }
    fi
    mkfs.vfat -n APCAM "$partition" >/dev/null || { format_error mkfs "mkfs.vfat failed"; return; }
    sync
    try_mount || { format_error mount "the new filesystem does not mount"; return; }
    log "formatted and mounted $partition"
    set_state ready
}

exec 7>"$LOCK"
case ${1:-} in
mount)
    # A running format owns the card.
    flock -n 7 || exit 0
    do_mount
    ;;
format)
    flock 7
    reason=
    if do_format; then
        format_result ok
        exit 0
    fi
    media_mount
    if [ -n "$mounted" ]; then
        set_state ready
    elif find_disk; then
        set_state unformatted "$disk"
    else
        set_state absent
    fi
    format_result failed "${reason:-unknown}"
    exit 1
    ;;
*)
    echo "usage: $0 mount|format" >&2
    exit 2
    ;;
esac
