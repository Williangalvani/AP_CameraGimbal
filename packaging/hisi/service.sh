#!/bin/sh
# Hi3516DV500 supervisor, started by /appfs/sh/start.sh instead of xipc.
# Loads the MPP drivers and mounts the microSD card as xipc did, then runs
# hisi-web and camera-app with the web's restart protocol: the web bumps
# camera-app.request and waits for a ready file from a new camera-app.
set -u
umask 077
APP_ROOT=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
CONFIG_ROOT=/date/ap_cameragimbal
MEDIA=/mnt
KOMOD=/komod
CMDLINE=/proc/cmdline
MODULES=/proc/modules
REQUEST=/run/camera-app.request
STARTED=/run/camera-app.started
READY=/run/camera-app.ready
APP_LOG=/run/camera_app.log
WEB_LOG=/run/hisi-web.log
WEB_PORT=80
app_pid=
web_pid=
log_pid=

# The lock is a kernel flock, so stale files do not prevent recovery.
exec 9>/run/hisi-service.lock
flock -n 9 || { echo 'hisi service already running' >&2; exit 1; }
echo $$ >/run/hisi-service.pid

process_alive()
{
    [ -n "$1" ] && [ -r "/proc/$1/status" ] || return 1
    # A zombie thread-group leader can still have live SDK worker threads.
    awk '/^State:/ {zombie=($2 == "Z")} /^Threads:/ {threads=$2}
         END {exit zombie && threads <= 1}' "/proc/$1/status"
}
stop_child()
{
    [ -n "$app_pid" ] || return 0
    kill -TERM "$app_pid" 2>/dev/null || true
    for _ in $(seq 1 15); do
        process_alive "$app_pid" || break
        sleep 1
    done
    kill -KILL "$app_pid" 2>/dev/null || true
    # BusyBox ash can stall in wait after SIGCHLD already reaped a job.
    app_pid=
    rm -f "$READY"
}
wait_for_restart()
{
    echo 'Camera stopped; web UI remains available for diagnosis and restart' >&2
    while [ "$(cat "$REQUEST" 2>/dev/null || echo 0)" = "$generation" ]; do
        check_web
        sleep 1
    done
}
stop()
{
    trap '' INT TERM
    stop_child
    for child in "$web_pid" "$log_pid"; do
        [ -n "$child" ] && kill "$child" 2>/dev/null || true
    done
    rm -f /run/hisi-service.pid "$STARTED"
    trap - INT TERM
    exit 0
}
trap stop INT TERM

# xipc mounted the card's first FAT partition. Output directories are only
# created on the card, never on the rootfs mount point.
mount_media()
{
    if ! mountpoint -q "$MEDIA"; then
        for device in /dev/mmcblk0p1 /dev/mmcblk1p1 /dev/mmcblk0 /dev/mmcblk1; do
            [ -b "$device" ] || continue
            mount -o noatime "$device" "$MEDIA" 2>/dev/null && break
        done
        mountpoint -q "$MEDIA" || return 0
    fi
    mkdir -p "$MEDIA/DCIM/record" "$MEDIA/DCIM/capture" "$MEDIA/logs"
}

load_module()
{
    file=$1
    shift
    name=${file##*/}
    awk -v name="${name%.ko}" '$1 == name {found=1} END {exit !found}' "$MODULES" &&
        return 0
    insmod "$file" "$@"
}
# Same modules, order and parameters as xipc. ot_wdt is left out: nothing
# here feeds a hardware watchdog yet.
load_mpp()
{
    sensor=${MPP_SENSOR:-imx515}
    board=${MPP_BOARD:-dmeb}
    # xipc assumes 1 GiB DDR, keeps 2 MiB above the kernel's mem= and gives
    # the rest to MMZ: the stock mem=384M is 0x58200000, 638M.
    os_mb=
    for arg in $(cat "$CMDLINE"); do
        case $arg in
        mem=*M) os_mb=${arg#mem=}; os_mb=${os_mb%M} ;;
        mem=*K) os_mb=${arg#mem=}; os_mb=${os_mb%K}
                case $os_mb in ''|*[!0-9]*) os_mb= ;; *) os_mb=$((os_mb / 1024)) ;; esac ;;
        esac
    done
    case $os_mb in
    ''|*[!0-9]*) echo "No mem= in kernel command line" >&2; return 1 ;;
    esac
    ddr_mb=${MPP_DDR_MB:-1024}
    mmz_start=${MPP_MMZ_START:-$(printf '0x%x' $(((os_mb + 1026) << 20)))}
    mmz_size=${MPP_MMZ_SIZE:-$((ddr_mb - os_mb - 2))}
    [ "$mmz_size" -gt 0 ] || { echo "No memory left for MMZ" >&2; return 1; }
    load_module "$KOMOD/sys_config.ko" \
        "sensors=sns0=$sensor,sns1=null,sns2=null,sns3=null" "board=$board" \
        vo_intf=bt1120 ir_auto=1 || return 1
    load_module "$KOMOD/ot_osal.ko" || return 1
    load_module "$KOMOD/ot_mmz.ko" anony=1 mmz_allocator=ot \
        "mmz=anonymous,0,$mmz_start,${mmz_size}M" mem_process_isolation=0 || return 1
    for module in ot_base ot_vb ot_vca ot_sys ot_aio ot_ai ot_ao ot_aenc \
        ot_adec ot_acodec ot_tde ot_rgn ot_gdc ot_vpp ot_vgs ot_dis ot_vi \
        ot_isp ot_vpss ot_mcf; do
        load_module "$KOMOD/$module.ko"
    done
    load_module "$KOMOD/gfbg.ko" video="gfbg:vram0_size:16200"
    for module in ot_vo_dev ot_vo ot_chnl ot_vfmw ot_rc ot_venc ot_venc_svr \
        ot_h264e ot_h265e ot_jpege ot_jpegd ot_vdec; do
        load_module "$KOMOD/$module.ko"
    done
    load_module "$KOMOD/ot_ive.ko" save_power=1
    for module in ot_dpu_rect ot_dpu_match ot_svp_npu extdrv/ot_pwm \
        extdrv/ot_piris extdrv/ot_sensor_i2c ot_mipi_rx ot_user ot_aiisp \
        ot_pm ot_cipher ot_km; do
        load_module "$KOMOD/$module.ko"
    done
    # xipc ignores failures after ot_mmz; camera-app cannot run without these.
    for module in ot_sys ot_vi ot_isp ot_vpss ot_venc ot_mipi_rx ot_sensor_i2c; do
        awk -v name="$module" '$1 == name {found=1} END {exit !found}' "$MODULES" ||
            { echo "MPP module $module did not load" >&2; return 1; }
    done
}

# Optional bring-up overrides (MPP_SENSOR, MPP_BOARD, MPP_DDR_MB,
# MPP_MMZ_START, MPP_MMZ_SIZE), kept on /date so no appfs rebuild is needed.
[ ! -f "$CONFIG_ROOT/mpp.conf" ] || . "$CONFIG_ROOT/mpp.conf"
mkdir -p "$CONFIG_ROOT"
[ -e "$CONFIG_ROOT/camera.ini" ] || cp "$APP_ROOT/camera.ini.default" "$CONFIG_ROOT/camera.ini"
[ -e "$CONFIG_ROOT/web.pass" ] || cp "$APP_ROOT/web.pass.default" "$CONFIG_ROOT/web.pass"
export CAMERA_APP_CONFIG=$CONFIG_ROOT/camera.ini
export CAMERA_APP_RECORD_ROOT=$MEDIA/DCIM/record
export CAMERA_APP_CAPTURE_ROOT=$MEDIA/DCIM/capture
export CAMERA_APP_LOG_ROOT=$MEDIA/logs
export CAMERA_APP_READY_PATH=$READY
"$APP_ROOT/bin/hisi-web" -p "$WEB_PORT" >>"$WEB_LOG" 2>&1 9>&- &
web_pid=$!
(
    while sleep 5; do
        for file in "$APP_LOG" "$WEB_LOG" /run/hisi-service.log; do
            [ -f "$file" ] || continue
            if [ "$(wc -c <"$file")" -gt 524288 ]; then
                tail -c 131072 "$file" >"$file.1"
                : >"$file"
            fi
        done
    done
) 9>&- &
log_pid=$!

check_web()
{
    if ! process_alive "$web_pid"; then
        "$APP_ROOT/bin/hisi-web" -p "$WEB_PORT" >>"$WEB_LOG" 2>&1 9>&- &
        web_pid=$!
    fi
}
while true; do
    check_web
    exec 8>/run/camera-app.request.lock
    flock -x 8 || exit 1
    generation=$(cat "$REQUEST" 2>/dev/null || echo 0)
    printf '%s\n' "$generation" >"$STARTED.new"
    mv "$STARTED.new" "$STARTED"
    flock -u 8
    exec 8>&-
    mount_media
    if ! load_mpp; then
        echo 'MPP drivers failed to load; not starting camera-app' >&2
        wait_for_restart
        continue
    fi
    rm -f "$READY"
    "$APP_ROOT/bin/camera-app" --config "$CAMERA_APP_CONFIG" >>"$APP_LOG" 2>&1 9>&- &
    app_pid=$!
    ok=false
    for _ in $(seq 1 45); do
        [ -f "$READY" ] && ok=true && break
        process_alive "$app_pid" || break
        [ "$(cat "$REQUEST" 2>/dev/null || echo 0)" != "$generation" ] && break
        sleep 1
    done
    if [ "$ok" = false ]; then
        stop_child
        if [ "$(cat "$REQUEST" 2>/dev/null || echo 0)" = "$generation" ]; then
            echo 'camera-app startup failed; entering recovery' >&2
            wait_for_restart
        fi
        continue
    fi
    # A web request can race the fork after its PID scan. Observe the request
    # ourselves as well, so a missed old instance cannot block a restart.
    while process_alive "$app_pid"; do
        check_web
        if [ "$(cat "$REQUEST" 2>/dev/null || echo 0)" != "$generation" ]; then
            stop_child
            break
        fi
        sleep 1
    done
    app_pid=
    rm -f "$READY"
    # An intentional web restart increments REQUEST; other exits await recovery.
    if [ "$(cat "$REQUEST" 2>/dev/null || echo 0)" = "$generation" ]; then
        wait_for_restart
    fi
    sleep 1
done
