#ifndef APCAM_TARGET_HISI_H
#define APCAM_TARGET_HISI_H

/* Target properties shared by firmware, web UI and generated SITL data.
 *
 * HiSilicon Hi3516DV500 (aarch64, Linux 5.10) IP camera with a Sony IMX678
 * (4K) CSI sensor. Video uses the HiSilicon "ot" MPP pipeline
 * (VI -> ISP -> VPSS -> VENC), the same HiSilicon ot-MPP SDK (ss_mpi) used by
 * src/backends/mt11 (ss928v100 = Hi3519DV500, our chip's sibling) — this
 * backend derives from mt11, trimmed to a single fixed RGB sensor (no thermal,
 * no dual sensor, no optical zoom/focus, no gimbal). Fixed
 * camera: the gimbal is a separate backend and is not provided here.
 */
#define APCAM_NAME "hisi"
#define APCAM_PRODUCT_NAME "HiSi 4K IPC"
#define APCAM_MODEL_NAME "Hi3516DV500 IMX678"
#define APCAM_MANUFACTURER "HiSilicon"
#define APCAM_NUM_LENSES 1
/* Two encoded streams: main (up to 4K) served as /video1 + recording, and a
 * lower-rate sub stream as /video2. VPSS produces both from one capture. */
#define APCAM_NUM_STREAMS 2
#define APCAM_STREAM1_LENS_MASK 1
#define APCAM_STREAM2_LENS_MASK 1
#define APCAM_HAVE_PHOTO 1
/* JPEG snapshot from a dedicated VENC channel; no separate photo sensor mode. */
#define APCAM_HAVE_PHOTO_RESOLUTION 0
#define APCAM_HAVE_ZOOM 0
#define APCAM_NUM_RECORDING_CHANNELS 1
/* MAVLink only; no vendor UDP/TCP control service on this port. */
#define APCAM_VENDOR_PROTOCOL APCAM_PROTOCOL_NONE
#define APCAM_HAVE_THERMAL 0
#define APCAM_HAVE_LIDAR 0
#define APCAM_ZOOM_NATIVE_RATE 0
#define APCAM_ZOOM_CONTROL_MAX APCAM_ZOOM_MAX
#define APCAM_WEB_CONTROL_MAVLINK 1
#define APCAM_HAVE_OVERLAY_RECORDING_SELECT 0
#define APCAM_HAVE_GIMBAL_RATES 0
/* Fixed camera: no gimbal advertised. */
#define APCAM_GIMBAL_OPTIONAL 1
/* No external UART is exposed, so no gimbal can be attached either. */
#define APCAM_HAVE_GIMBAL 0
#define APCAM_SUPPRESS_DUPLICATE_ANGLES 0
#define APCAM_WEB_CENTER_COMMAND 3
#define APCAM_HAVE_OPTICAL_ZOOM 0
#define APCAM_HAVE_FOCUS 0
#define APCAM_HAVE_IMAGE_CONTROLS 1
#define APCAM_HAVE_WB_TUNING 1
#define APCAM_HAVE_EXTERNAL_UART 0
#define APCAM_HAVE_SSH_KEYS 0
#define APCAM_HAVE_SOC_TEMPERATURE 1
/* No simulated camera / SITL launcher entry yet. */
#define APCAM_HAVE_SITL 0
/* The device's own network service owns the interfaces for now. */
#define APCAM_HAVE_NETWORK_CONFIG 0
/* No thermal zone; ot_pm reports the die temperature in its status table. */
#ifndef SOC_TEMPERATURE_PATH
#define SOC_TEMPERATURE_PATH "/proc/umap/pm"
#define SOC_TEMPERATURE_FIELD "cur_temp:"
#endif
#define APCAM_VENDOR_PORT 0
#define APCAM_WEB_PORT 80
#define APCAM_LENS1_TYPE APCAM_LENS_TYPE_RGB
#define APCAM_LENS1_NAME "RGB"
/* Nominal; measure on the real optics. */
#define APCAM_LENS1_FOV_H 90.0f
#define APCAM_LENS1_FOV_H_TELE 0.0f
#define APCAM_LENS1_FOV_MODEL APCAM_FOV_FOCAL_LENGTH
#define APCAM_LENS1_OPTICAL_ZOOM_MAX 1
/* IMX678 full 4K readout. */
#define APCAM_LENS1_WIDTH 3840
#define APCAM_LENS1_HEIGHT 2160
#define APCAM_ZOOM_MAX 1.0f
#define APCAM_HAVE_VIDEO_BITRATE 1
/* 0 = automatic: 12 Mbit/s at 2160p, 4 Mbit/s at 1080p, 2 Mbit/s below. */
#define APCAM_DEFAULT_MAIN_BITRATE_KBPS 0
#define APCAM_MAIN_BITRATE_MIN_KBPS 0
#define APCAM_HAVE_VIDEO_FRAME_RATE 1
/* IMX678 4K@30 (10/12-bit); 60 fps mode exists but 30 is the qualified default. */
#define APCAM_FRAME_RATE 30
#define APCAM_THERMAL_FRAME_RATE 0
#define APCAM_THERMAL_STREAM_WIDTH 0
#define APCAM_THERMAL_STREAM_HEIGHT 0
/* Main stream up to 2160p; sub stream 1080p. H.265 + H.264 both supported. */
#define APCAM_MAIN_RESOLUTIONS (APCAM_RES_MASK_2160P | APCAM_RES_MASK_1080P | APCAM_RES_MASK_720P | \
    APCAM_RES_MASK_540P | APCAM_RES_MASK_480P)
#define APCAM_SUB_RESOLUTIONS (APCAM_RES_MASK_1080P | APCAM_RES_MASK_720P | APCAM_RES_MASK_480P)
#define APCAM_RECORDING_RESOLUTIONS APCAM_RES_MASK_2160P
#define APCAM_STREAM_CODECS 3
#define APCAM_DEFAULT_MAIN_RESOLUTION APCAM_RES_2160P
#define APCAM_DEFAULT_SUB_RESOLUTION APCAM_RES_720P
#define APCAM_DEFAULT_RECORDING_RESOLUTION APCAM_RES_2160P
#define APCAM_DEFAULT_SYSTEM_ID 0
#define APCAM_DEFAULT_POSITION_TARGETING 0
#define APCAM_DEFAULT_ORIENTATION 0
#define APCAM_DEFAULT_PHOTO_SCOPE 1
#define APCAM_DEFAULT_TIMEZONE "GMT"
/* vendor-compatible RTSP path aliases for /video1 and /video2 */
#define APCAM_DEFAULT_MAIN_ALIAS ""
#define APCAM_DEFAULT_SUB_ALIAS ""
#define APCAM_GIMBAL_PITCH_MIN -90.0f
#define APCAM_GIMBAL_PITCH_MAX 30.0f
#define APCAM_GIMBAL_YAW_CONTINUOUS 0
#define APCAM_GIMBAL_YAW_MIN -180.0f
#define APCAM_GIMBAL_YAW_MAX 180.0f
#define APCAM_GIMBAL_RATE_MAX 60.0f
#define APCAM_VENDOR_YAW_RATE_FULL_SCALE 60.0f
#define APCAM_VENDOR_PITCH_RATE_FULL_SCALE 60.0f
#define APCAM_GIMBAL_FEEDBACK_UPRIGHT_MATRIX {1, 0, 0, 0, 1, 0, 0, 0, 1}
#define APCAM_GIMBAL_FEEDBACK_UPRIGHT_OFFSET {0, 0, 0}
#define APCAM_GIMBAL_FEEDBACK_INVERTED_MATRIX {1, 0, 0, 0, 1, 0, 0, 0, 1}
#define APCAM_GIMBAL_FEEDBACK_INVERTED_OFFSET {0, 0, 0}
#define APCAM_GIMBAL_PRIVATE_FEEDBACK_UPRIGHT_MATRIX {1, 0, 0, 0, 1, 0, 0, 0, 1}
#define APCAM_GIMBAL_PRIVATE_FEEDBACK_UPRIGHT_OFFSET {0, 0, 0}
#define APCAM_GIMBAL_PRIVATE_FEEDBACK_INVERTED_MATRIX {1, 0, 0, 0, 1, 0, 0, 0, 1}
#define APCAM_GIMBAL_PRIVATE_FEEDBACK_INVERTED_OFFSET {0, 0, 0}
#define APCAM_GIMBAL_ANGLE_COMMAND_UPRIGHT_MATRIX {1, 0, 0, 0, 1, 0, 0, 0, 1}
#define APCAM_GIMBAL_ANGLE_COMMAND_UPRIGHT_OFFSET {0, 0, 0}
#define APCAM_GIMBAL_ANGLE_COMMAND_INVERTED_MATRIX {1, 0, 0, 0, 1, 0, 0, 0, 1}
#define APCAM_GIMBAL_ANGLE_COMMAND_INVERTED_OFFSET {0, 0, 0}
#define APCAM_GIMBAL_RATE_COMMAND_UPRIGHT_MATRIX {1, 0, 0, 0, 1, 0, 0, 0, 1}
#define APCAM_GIMBAL_RATE_COMMAND_UPRIGHT_OFFSET {0, 0, 0}
#define APCAM_GIMBAL_RATE_COMMAND_INVERTED_MATRIX {1, 0, 0, 0, 1, 0, 0, 0, 1}
#define APCAM_GIMBAL_RATE_COMMAND_INVERTED_OFFSET {0, 0, 0}
/* /appfs is a read-only squashfs (UBI volume on mtd4). /date (UBIFS, mtd6)
 * keeps settings across appfs updates; recordings and logs go to the microSD
 * card on /mnt, which the launcher mounts in place of xipc. */
#ifdef APCAM_WEB_BUILD
/* Uploads go to the rootfs recovery updater, which survives any appfs. Names
 * follow apcam_hisi_firmware::valid_image_name(), so the vendor's own .img
 * releases are accepted as well as ours. */
#define WEB_HAVE_FIRMWARE_UPLOAD 1
#define FIRMWARE_PREFIX ""
#define FIRMWARE_ACCEPT ".img,.bin"
#define FIRMWARE_NAME_PATTERNS "*.img / *.bin"
#ifndef APP_INSTALL_DIR
#define APP_INSTALL_DIR "/appfs/ap_cameragimbal"
#endif
#ifndef APP_DIR
#define APP_DIR "/date/ap_cameragimbal"
#endif
#ifndef MEDIA_ROOT
#define MEDIA_ROOT "/mnt"
#endif
#ifndef REPLACEMENT_CAMERA_PATH
#define REPLACEMENT_CAMERA_PATH APP_INSTALL_DIR "/bin/camera-app"
#endif
#ifndef WEB_PATH
#define WEB_PATH APP_INSTALL_DIR "/bin/hisi-web"
#endif
#ifndef WEBROOT_PATH
#define WEBROOT_PATH APP_INSTALL_DIR "/webroot"
#endif
#ifndef APP_REQUEST_PATH
#define APP_REQUEST_PATH "/run/camera-app.request"
#endif
#ifndef APP_REQUEST_LOCK_PATH
#define APP_REQUEST_LOCK_PATH "/run/camera-app.request.lock"
#endif
#ifndef APP_STARTED_PATH
#define APP_STARTED_PATH "/run/camera-app.started"
#endif
#ifndef REPLACEMENT_CONFIG_PATH
#define REPLACEMENT_CONFIG_PATH APP_DIR "/camera.ini"
#endif
#ifndef REPLACEMENT_CONFIG_BACKUP_PATH
#define REPLACEMENT_CONFIG_BACKUP_PATH APP_DIR "/camera.ini.web.bak"
#endif
#ifndef PASSWORD_PATH
#define PASSWORD_PATH APP_DIR "/web.pass"
#endif
#define APP_STORAGE_PATH APP_INSTALL_DIR
#define SETTINGS_STORAGE_PATH APP_DIR
#ifndef REBOOT_COMMAND
#define REBOOT_COMMAND "reboot"
#endif
#endif

#endif
