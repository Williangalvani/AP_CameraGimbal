#ifndef CAMERA_APP_HISI_PIPELINE_H
#define CAMERA_APP_HISI_PIPELINE_H

#include "sample_comm.h"
#include "ss_mpi_venc.h"
#include "camera_app/config.h"
#include "apcam/target.h"

#define CA_HISI_SENSOR_WIDTH APCAM_LENS1_WIDTH
#define CA_HISI_SENSOR_HEIGHT APCAM_LENS1_HEIGHT
#define CA_HISI_PIPE 0
#define CA_HISI_GROUP 0

#define CA_HISI_MAIN_CHN 0
#define CA_HISI_SUB_CHN 1
#define CA_HISI_RECORD_CHN 2

struct ca_hisi_output_sizes {
    td_u32 width[3];
    td_u32 height[3];
    // the record channel exists only when recording cannot reuse the main VENC
    bool record_channel;
};

/* Bring-up overrides: CA_HISI_SENSOR_BUS=<n> selects the sensor I2C bus and
 * CA_HISI_SENSOR_2LANE=1 the 2-lane IMX678 mode. */
sample_sns_type ca_hisi_sensor_type(void);
td_s32 ca_hisi_system_init(const struct ca_hisi_output_sizes *sizes);
void ca_hisi_configure_vi(sample_vi_cfg *vi_cfg);
td_s32 ca_hisi_vpss_start(const struct ca_hisi_output_sizes *sizes);
void ca_hisi_vpss_stop(const struct ca_hisi_output_sizes *sizes);
// frame_rate is the encoded rate; the encoder drops sensor frames to reach it
void ca_hisi_video_attributes(ot_venc_chn_attr *attr,
                              enum ca_video_codec codec, td_u32 width,
                              td_u32 height, td_u32 frame_rate,
                              td_u32 bit_rate_kbps);
td_s32 ca_hisi_set_bitrate(ot_venc_chn channel, td_u32 bit_rate_kbps);
td_s32 ca_hisi_set_inverted(td_bool inverted, bool record_enabled,
                            const struct ca_hisi_output_sizes *sizes);

#endif
