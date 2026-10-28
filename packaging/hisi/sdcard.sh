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
# A card that does not mount is not retried every second (the kernel logs
# every attempt), only once it has been removed and inserted again.
# This board's cards can come up unreadable (command timeouts, error -110);
# binding SDCARD_HOST to its driver again initialises them, so that is tried
# up to RESETS_MAX times until a card mounts.
set -u
MEDIA=${SDCARD_MEDIA:-/mnt}
STATE=${SDCARD_STATE:-/run/sdcard.state}
LOCK=${SDCARD_LOCK:-/run/sdcard.lock}
DISKS=${SDCARD_DISKS:-/dev/mmcblk0 /dev/mmcblk1}
MOUNTS=${SDCARD_MOUNTS:-/proc/mounts}
HOST=${SDCARD_HOST-}
HOST_DRIVER=${SDCARD_HOST_DRIVER:-/sys/bus/platform/drivers/sdhci_nebula}
RESETS_MAX=2
# The first partition starts 4 MiB in, the erase-block alignment cards use.
PART_START=8192

log()
{
    echo "sdcard: $*" >&2
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
    find_disk
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
    find_disk || { log 'no card to format'; set_state absent; return 1; }
    set_state formatting
    media_mount
    if [ -n "$mounted" ]; then
        umount "$MEDIA" 2>/dev/null || umount -l "$MEDIA" ||
            { log "cannot unmount $MEDIA"; return 1; }
    fi
    sectors=$(disk_sectors) || return 1
    # FAT32 needs at least 65525 clusters; below 64 MiB is not a real card.
    [ "$sectors" -gt $((PART_START + 131072)) ] ||
        { log "$disk is too small ($sectors sectors)"; return 1; }
    log "formatting $disk ($sectors sectors)"
    # Clears the old partition table and whatever filesystem headers follow it.
    dd if=/dev/zero of="$disk" bs=1M count=1 conv=notrunc 2>/dev/null ||
        { log "cannot write $disk"; return 1; }
    # One FAT32 LBA (type 0x0c) partition; CHS fields marked unused.
    {
        dd if=/dev/zero bs=446 count=1 2>/dev/null
        printf '\000\376\377\377\014\376\377\377'
        le32 "$PART_START"
        le32 $((sectors - PART_START))
        dd if=/dev/zero bs=48 count=1 2>/dev/null
        printf '\125\252'
    } | dd of="$disk" conv=notrunc 2>/dev/null ||
        { log "cannot write the partition table"; return 1; }
    sync
    partition=$(partition_of "$disk")
    if [ -b "$disk" ]; then
        blockdev --rereadpt "$disk" 2>/dev/null || partprobe "$disk" 2>/dev/null
        for _ in 1 2 3 4 5 6 7 8 9 10; do
            [ -b "$partition" ] && break
            sleep 1
        done
        [ -b "$partition" ] || { log "$partition did not appear"; return 1; }
    fi
    mkfs.vfat -n APCAM "$partition" >/dev/null || { log "mkfs.vfat failed"; return 1; }
    sync
    try_mount || { log "the new filesystem does not mount"; return 1; }
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
    if do_format; then
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
    exit 1
    ;;
*)
    echo "usage: $0 mount|format" >&2
    exit 2
    ;;
esac
