#!/bin/sh
# Task 7 driver (host): 30 min 4K WebRTC via apcamsrc, measure.sh every 5 min.
R=$HOME/mcm_trial/results
cd $HOME/mcm_trial
SH="python3 $HOME/git/apcam-mcm/tools/hisi_shell.py"
$SH -t 20 'grep -E "^Udp6?:" /proc/net/snmp; grep Udp6SndbufErrors /proc/net/snmp6' > $R/soak_snmp_start.txt
/tmp/cdpvenv/bin/python3 probe/webrtc_stats.py --stream '4K Cam main (on camera)' --seconds 1830 --every 30 --warmup 20 > $R/soak_webrtc.log 2>&1 &
W=$!
sleep 25
for i in 00 05 10 15 20 25 30; do
  date '+%H:%M:%S' >> $R/soak_measure.txt
  $SH -t 60 "/run/mcm/measure.sh soak-$i 30; grep -E '^Udp6?:' /proc/net/snmp | tail -1" >> $R/soak_measure.txt 2>&1
  [ $i = 30 ] || sleep 268
done
wait $W
$SH -t 20 'grep -E "^Udp6?:" /proc/net/snmp; grep Udp6SndbufErrors /proc/net/snmp6' > $R/soak_snmp_end.txt
echo SOAK_DONE >> $R/soak_measure.txt
