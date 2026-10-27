#define _POSIX_C_SOURCE 200809L
#include "pipeline.h"

#include "camera_app/log.h"

#include "ss_mpi_sys.h"
#include "ss_mpi_vb.h"
#include "ss_mpi_vpss.h"

#include <stdlib.h>
#include <string.h>

/* Pool depths follow the SDK's own 4K H.265 + 1080p H.264 venc sample. */
#define VI_VB_COUNT 6U
#define VPSS_VB_COUNT 8U

// the B040 IMX678 driver is 4-lane only
sample_sns_type ca_hisi_sensor_type(void)
{
    return SONY_IMX678_MIPI_8M_30FPS_12BIT;
}

static void channel_attributes(ot_vpss_chn_attr *attr, unsigned channel,
                               const struct ca_hisi_output_sizes *sizes)
{
    sample_comm_vpss_get_default_chn_attr(attr);
    attr->width = sizes->width[channel];
    attr->height = sizes->height[channel];
    // photos read frames from the main channel's user queue
    attr->depth = channel == CA_HISI_MAIN_CHN ? 2U : 0U;
    if (channel != CA_HISI_MAIN_CHN) attr->compress_mode = OT_COMPRESS_MODE_NONE;
}

static td_s32 add_vb_pool(ot_vb_cfg *vb_cfg, td_u32 width, td_u32 height,
                          ot_pixel_format pixel_format,
                          ot_compress_mode compress_mode, td_u32 count)
{
    ot_pic_buf_attr attr = {};
    ot_vb_calc_cfg calc = {};
    const td_u32 pool = vb_cfg->max_pool_cnt;

    attr.width = width;
    attr.height = height;
    attr.align = OT_DEFAULT_ALIGN;
    attr.bit_width = OT_DATA_BIT_WIDTH_8;
    attr.pixel_format = pixel_format;
    attr.compress_mode = compress_mode;
    attr.video_format = OT_VIDEO_FORMAT_LINEAR;
    ot_common_get_pic_buf_cfg(&attr, &calc);
    if (calc.vb_size == 0U || pool >= OT_VB_MAX_COMMON_POOLS) return TD_FAILURE;
    vb_cfg->common_pool[pool].blk_size = calc.vb_size;
    vb_cfg->common_pool[pool].blk_cnt = count;
    vb_cfg->common_pool[pool].remap_mode = OT_VB_REMAP_MODE_NOCACHE;
    vb_cfg->max_pool_cnt = pool + 1U;
    ca_log("VB pool %u: %ux%u format=%d compress=%d size=%llu count=%u",
           pool, width, height, pixel_format, compress_mode,
           (unsigned long long)calc.vb_size, count);
    return TD_SUCCESS;
}

td_s32 ca_hisi_system_init(const struct ca_hisi_output_sizes *sizes)
{
    ot_vb_cfg vb_cfg = {};
    td_s32 result;

    /* The VI pool is sized for uncompressed 4:2:2 so it also holds the
     * sensor's line-compressed RAW12 frames. */
    result = add_vb_pool(&vb_cfg, CA_HISI_SENSOR_WIDTH, CA_HISI_SENSOR_HEIGHT,
                         OT_PIXEL_FORMAT_YVU_SEMIPLANAR_422,
                         OT_COMPRESS_MODE_NONE, VI_VB_COUNT);
    for (unsigned channel = 0; result == TD_SUCCESS && channel < 3U; channel++) {
        ot_vpss_chn_attr attr;
        if (channel == CA_HISI_RECORD_CHN && !sizes->record_channel) continue;
        channel_attributes(&attr, channel, sizes);
        result = add_vb_pool(&vb_cfg, attr.width, attr.height, attr.pixel_format,
                             attr.compress_mode, VPSS_VB_COUNT);
    }
    if (result != TD_SUCCESS) return result;
    return sample_comm_sys_init_with_vb_supplement(
        &vb_cfg, OT_VB_SUPPLEMENT_JPEG_MASK | OT_VB_SUPPLEMENT_BNR_MOT_MASK);
}

void ca_hisi_configure_vi(sample_vi_cfg *vi_cfg)
{
    const sample_sns_type sensor = ca_hisi_sensor_type();
    const char *bus = getenv("CA_HISI_SENSOR_BUS");

    sample_comm_vi_get_default_vi_cfg(sensor, vi_cfg);
    if (bus != NULL && *bus != '\0') {
        char *end = NULL;
        long value = strtol(bus, &end, 10);
        if (*end == '\0' && value >= 0 && value <= 7) {
            vi_cfg->sns_info.bus_id = (td_u32)value;
        } else {
            ca_log("ignoring invalid CA_HISI_SENSOR_BUS=%s", bus);
        }
    }
    ca_log("IMX678 4-lane mode on I2C bus %u", vi_cfg->sns_info.bus_id);
}

td_s32 ca_hisi_vpss_start(const struct ca_hisi_output_sizes *sizes)
{
    ot_vpss_grp_attr group_attr;
    sample_vpss_chn_attr channels;

    memset(&channels, 0, sizeof(channels));
    sample_comm_vpss_get_default_grp_attr(&group_attr);
    group_attr.max_width = CA_HISI_SENSOR_WIDTH;
    group_attr.max_height = CA_HISI_SENSOR_HEIGHT;
    for (unsigned channel = 0; channel < 3U; channel++) {
        if (channel == CA_HISI_RECORD_CHN && !sizes->record_channel) continue;
        channel_attributes(&channels.chn_attr[channel], channel, sizes);
        channels.chn_enable[channel] = TD_TRUE;
    }
    channels.chn_array_size = OT_VPSS_MAX_PHYS_CHN_NUM;
    return sample_common_vpss_start(CA_HISI_GROUP, &group_attr, &channels);
}

void ca_hisi_vpss_stop(const struct ca_hisi_output_sizes *sizes)
{
    td_bool enabled[OT_VPSS_MAX_PHYS_CHN_NUM] = {
        TD_TRUE, TD_TRUE, sizes->record_channel ? TD_TRUE : TD_FALSE
    };
    (void)sample_common_vpss_stop(CA_HISI_GROUP, enabled, OT_VPSS_MAX_PHYS_CHN_NUM);
}

void ca_hisi_video_attributes(ot_venc_chn_attr *attr,
                              enum ca_video_codec codec, td_u32 width,
                              td_u32 height, td_u32 frame_rate,
                              td_u32 bit_rate_kbps)
{
    memset(attr, 0, sizeof(*attr));
    attr->venc_attr.type = codec == CA_VIDEO_H265 ? OT_PT_H265 : OT_PT_H264;
    attr->venc_attr.max_pic_width = width;
    attr->venc_attr.max_pic_height = height;
    attr->venc_attr.pic_width = width;
    attr->venc_attr.pic_height = height;
    attr->venc_attr.buf_size = OT_ALIGN_UP(width * height * 3U / 4U, 64U);
    attr->venc_attr.profile = 0;
    attr->venc_attr.is_by_frame = TD_TRUE;
    if (codec == CA_VIDEO_H265) {
        attr->venc_attr.h265_attr.rcn_ref_share_buf_en = TD_FALSE;
        attr->venc_attr.h265_attr.frame_buf_ratio = 70;
        attr->rc_attr.rc_mode = OT_VENC_RC_MODE_H265_CBR;
        attr->rc_attr.h265_cbr.gop = frame_rate;
        attr->rc_attr.h265_cbr.stats_time = 1;
        attr->rc_attr.h265_cbr.src_frame_rate = APCAM_FRAME_RATE;
        attr->rc_attr.h265_cbr.dst_frame_rate = frame_rate;
        attr->rc_attr.h265_cbr.bit_rate = bit_rate_kbps;
    } else {
        attr->venc_attr.h264_attr.rcn_ref_share_buf_en = TD_FALSE;
        attr->venc_attr.h264_attr.frame_buf_ratio = 70;
        attr->rc_attr.rc_mode = OT_VENC_RC_MODE_H264_CBR;
        attr->rc_attr.h264_cbr.gop = frame_rate;
        attr->rc_attr.h264_cbr.stats_time = 1;
        attr->rc_attr.h264_cbr.src_frame_rate = APCAM_FRAME_RATE;
        attr->rc_attr.h264_cbr.dst_frame_rate = frame_rate;
        attr->rc_attr.h264_cbr.bit_rate = bit_rate_kbps;
    }
    attr->gop_attr.gop_mode = OT_VENC_GOP_MODE_NORMAL_P;
    attr->gop_attr.normal_p.ip_qp_delta = 2;
}

td_s32 ca_hisi_set_low_delay_vui(ot_venc_chn channel, enum ca_video_codec codec)
{
    // Without bitstream_restriction a decoder must assume frames are
    // reordered and holds the level's whole DPB (5 frames at 4K) before
    // showing one. These streams have no B-frames, so declare zero.
    if (codec == CA_VIDEO_H265) {
        ot_venc_h265_vui vui;
        td_s32 result = ss_mpi_venc_get_h265_vui(channel, &vui);
        if (result != TD_SUCCESS) return result;
        vui.vui_bitstream_restric.bitstream_restriction_flag = 1;
        return ss_mpi_venc_set_h265_vui(channel, &vui);
    }
    ot_venc_h264_vui vui;
    td_s32 result = ss_mpi_venc_get_h264_vui(channel, &vui);
    if (result != TD_SUCCESS) return result;
    vui.vui_bitstream_restric.bitstream_restriction_flag = 1;
    return ss_mpi_venc_set_h264_vui(channel, &vui);
}

td_s32 ca_hisi_set_bitrate(ot_venc_chn channel, td_u32 bit_rate_kbps)
{
    ot_venc_chn_attr attr;
    td_s32 result = ss_mpi_venc_get_chn_attr(channel, &attr);
    if (result != TD_SUCCESS) return result;
    if (attr.rc_attr.rc_mode == OT_VENC_RC_MODE_H265_CBR) {
        attr.rc_attr.h265_cbr.bit_rate = bit_rate_kbps;
    } else if (attr.rc_attr.rc_mode == OT_VENC_RC_MODE_H264_CBR) {
        attr.rc_attr.h264_cbr.bit_rate = bit_rate_kbps;
    } else {
        return TD_FAILURE;
    }
    return ss_mpi_venc_set_chn_attr(channel, &attr);
}

td_s32 ca_hisi_set_inverted(td_bool inverted, bool record_enabled,
                            const struct ca_hisi_output_sizes *sizes)
{
    for (ot_vpss_chn channel = 0; channel < 3; channel++) {
        ot_vpss_chn_attr attr;
        if (channel == CA_HISI_RECORD_CHN && !sizes->record_channel) continue;
        td_s32 result = ss_mpi_vpss_disable_chn(CA_HISI_GROUP, channel);
        if (result != TD_SUCCESS) return result;
        result = ss_mpi_vpss_get_chn_attr(CA_HISI_GROUP, channel, &attr);
        if (result == TD_SUCCESS) {
            attr.mirror_en = inverted;
            attr.flip_en = inverted;
            result = ss_mpi_vpss_set_chn_attr(CA_HISI_GROUP, channel, &attr);
        }
        // the record channel stays disabled unless recording
        td_s32 enable_result = channel == CA_HISI_RECORD_CHN && !record_enabled
                                   ? TD_SUCCESS
                                   : ss_mpi_vpss_enable_chn(CA_HISI_GROUP, channel);
        if (result != TD_SUCCESS) return result;
        if (enable_result != TD_SUCCESS) return enable_result;
    }
    return TD_SUCCESS;
}
