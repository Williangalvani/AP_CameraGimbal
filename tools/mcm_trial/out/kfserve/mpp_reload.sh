#!/bin/sh
# Reload the MPP stack above ot_mmz so no xipc state survives. ot_osal, ot_mmz,
# sys_config and ot_wdt stay loaded (ot_wdt rmmod hangs the board).
K=/komod
LOG=/run/mpp_reload.log
UP="ot_base ot_vb ot_vca ot_sys ot_aio ot_ai ot_ao ot_aenc ot_adec ot_acodec ot_tde ot_rgn ot_gdc ot_vpp ot_vgs ot_dis ot_vi ot_isp ot_vpss ot_mcf gfbg ot_vo_dev ot_vo ot_chnl ot_vfmw ot_rc ot_venc ot_venc_svr ot_h264e ot_h265e ot_jpege ot_jpegd ot_vdec ot_ive ot_dpu_rect ot_dpu_match ot_svp_npu extdrv/ot_pwm extdrv/ot_piris extdrv/ot_sensor_i2c ot_mipi_rx ot_user ot_aiisp ot_pm ot_cipher ot_km"
DOWN=
for m in $UP; do [ $m = ot_chnl ] || DOWN="$m $DOWN"; done
DOWN=$(echo "$DOWN" | sed "s/ot_jpegd /ot_jpegd ot_chnl /")
loaded() { awk -v n="$1" '$1 == n {f=1} END {exit !f}' /proc/modules; }
for m in $DOWN; do
    n=${m##*/}
    loaded $n || continue
    echo "rmmod $n" >> $LOG
    rmmod $n >> $LOG 2>&1 || echo "rmmod $n failed" >> $LOG
done
for m in $UP; do
    n=${m##*/}
    loaded $n && continue
    case $n in
    gfbg) args='video=gfbg:vram0_size:16200' ;;
    ot_ive) args='save_power=1' ;;
    *) args= ;;
    esac
    echo "insmod $n" >> $LOG
    insmod $K/$m.ko $args >> $LOG 2>&1 || echo "insmod $n failed" >> $LOG
done
echo reload done >> $LOG
