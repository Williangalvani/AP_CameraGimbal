#ifndef CAMERA_APP_HISI_PIPELINE_H
#define CAMERA_APP_HISI_PIPELINE_H

#include "sample_comm.h"
#include "ss_mpi_venc.h"
#include "camera_app/config.h"
#include "apcam/target.h"
#include "rate_control.h"

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

/* Bring-up override: CA_HISI_SENSOR_BUS=<n> selects the sensor I2C bus. */
sample_sns_type ca_hisi_sensor_type(void);
td_s32 ca_hisi_system_init(const struct ca_hisi_output_sizes *sizes);
void ca_hisi_configure_vi(sample_vi_cfg *vi_cfg);
td_s32 ca_hisi_vpss_start(const struct ca_hisi_output_sizes *sizes);
void ca_hisi_vpss_stop(const struct ca_hisi_output_sizes *sizes);
// frame_rate is the encoded rate; the encoder drops sensor frames to reach it.
// gop_frames is the keyframe interval in encoded frames (apcam_gop_frames()).
void ca_hisi_video_attributes(ot_venc_chn_attr *attr,
                              enum ca_video_codec codec, td_u32 width,
                              td_u32 height, td_u32 frame_rate,
                              td_u32 bit_rate_kbps, td_u32 gop_frames);
// call between create and start, with the attributes the channel was created with
td_s32 ca_hisi_set_low_delay_vui(ot_venc_chn channel, const ot_venc_chn_attr *attr);
td_s32 ca_hisi_set_bitrate(ot_venc_chn channel, td_u32 bit_rate_kbps);
td_s32 ca_hisi_set_gop(ot_venc_chn channel, td_u32 gop_frames);
// the channel's key-frame size limits, max_i_proportion and min_i_proportion
td_s32 ca_hisi_get_i_proportion(ot_venc_chn channel, ca_hisi_rc::IProportion *limits);
td_s32 ca_hisi_set_i_proportion(ot_venc_chn channel, ca_hisi_rc::IProportion limits);
td_s32 ca_hisi_set_inverted(td_bool inverted, bool record_enabled,
                            const struct ca_hisi_output_sizes *sizes);
/* Slices of rows macroblock (H.264) or LCU (H.265) rows, each returned by
 * ss_mpi_venc_get_stream as soon as it is encoded. Call between create and
 * start, on a channel created with venc_attr.is_by_frame false. */
td_s32 ca_hisi_set_slice_output(ot_venc_chn channel, td_u32 rows);
/* The super frame strategy for a key-frame threshold of i_kbits (0 restores
 * defaults): re-encode, or discard where the encoder cannot re-encode. *mode
 * is the one applied. */
td_s32 ca_hisi_set_super_frame(ot_venc_chn channel, unsigned i_kbits,
                               const ot_venc_super_frame_strategy *defaults,
                               ot_venc_super_frame_mode *mode);
/* Low delay: a stage passes a frame on after its first lines, not whole.
 * The VI pipe and its channel, which feed VPSS, or one VPSS channel. */
td_s32 ca_hisi_set_vi_pipe_low_delay(td_u32 lines);
td_s32 ca_hisi_set_vi_chn_low_delay(td_u32 lines);
td_s32 ca_hisi_set_vpss_low_delay(ot_vpss_chn channel, bool enable, td_u32 lines);

#endif
