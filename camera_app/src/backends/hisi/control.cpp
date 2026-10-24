/* Fixed HiSilicon IP camera: no integral gimbal. Reuse the optional-gimbal
 * CADDX control so the camera advertises no gimbal unless a CADDX/XFRobot-style
 * gimbal is wired over UART (APCAM_GIMBAL_OPTIONAL). Camera functions are
 * always available; ca_backend_has_gimbal() is false without a configured link.
 */
#include "../caddx/control.cpp"
