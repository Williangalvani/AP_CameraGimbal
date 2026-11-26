#define _POSIX_C_SOURCE 200809L
#include "pipeline.h"
#include "vui.h"

#include "camera_app/log.h"

#include "ss_mpi_sys.h"
#include "ss_mpi_vb.h"
#include "ss_mpi_vpss.h"

#include <stdlib.h>
#include <string.h>

/* Pool depths follow the SDK's own 4K H.265 + 1080p H.264 venc sample. */
#define VI_VB_COUNT 6U
#define VPSS_VB_COUNT 8U

/* venc_attr.profile: H.264 0 Baseline, 1 Main, 2 High; H.265 0 Main.
 * High matches the vendor firmware; web/APC_FourKCam.h reports these in getVencConf. */
static const td_u32 ca_hisi_h264_profile_high = 2U;
static const td_u32 ca_hisi_h265_profile_main = 0U;

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
    // photos read frames from the main channel's user queue, one-push white balance the sub's
    attr->depth = channel == CA_HISI_MAIN_CHN ? 2U : channel == CA_HISI_SUB_CHN ? 1U : 0U;
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
                              td_u32 bit_rate_kbps, td_u32 gop_frames)
{
    memset(attr, 0, sizeof(*attr));
    attr->venc_attr.type = codec == CA_VIDEO_H265 ? OT_PT_H265 : OT_PT_H264;
    attr->venc_attr.max_pic_width = width;
    attr->venc_attr.max_pic_height = height;
    attr->venc_attr.pic_width = width;
    attr->venc_attr.pic_height = height;
    attr->venc_attr.buf_size = OT_ALIGN_UP(width * height * 3U / 4U, 64U);
    attr->venc_attr.profile =
        codec == CA_VIDEO_H265 ? ca_hisi_h265_profile_main : ca_hisi_h264_profile_high;
    attr->venc_attr.is_by_frame = TD_TRUE;
    if (codec == CA_VIDEO_H265) {
        attr->venc_attr.h265_attr.rcn_ref_share_buf_en = TD_FALSE;
        attr->venc_attr.h265_attr.frame_buf_ratio = 70;
        attr->rc_attr.rc_mode = OT_VENC_RC_MODE_H265_CBR;
        attr->rc_attr.h265_cbr.gop = gop_frames;
        attr->rc_attr.h265_cbr.stats_time = 1;
        attr->rc_attr.h265_cbr.src_frame_rate = APCAM_FRAME_RATE;
        attr->rc_attr.h265_cbr.dst_frame_rate = frame_rate;
        attr->rc_attr.h265_cbr.bit_rate = bit_rate_kbps;
    } else {
        attr->venc_attr.h264_attr.rcn_ref_share_buf_en = TD_FALSE;
        attr->venc_attr.h264_attr.frame_buf_ratio = 70;
        attr->rc_attr.rc_mode = OT_VENC_RC_MODE_H264_CBR;
        attr->rc_attr.h264_cbr.gop = gop_frames;
        attr->rc_attr.h264_cbr.stats_time = 1;
        attr->rc_attr.h264_cbr.src_frame_rate = APCAM_FRAME_RATE;
        attr->rc_attr.h264_cbr.dst_frame_rate = frame_rate;
        attr->rc_attr.h264_cbr.bit_rate = bit_rate_kbps;
    }
    attr->gop_attr.gop_mode = OT_VENC_GOP_MODE_NORMAL_P;
    attr->gop_attr.normal_p.ip_qp_delta = 2;
}

td_s32 ca_hisi_set_low_delay_vui(ot_venc_chn channel, const ot_venc_chn_attr *attr)
{
    if (attr->rc_attr.rc_mode == OT_VENC_RC_MODE_H265_CBR) {
        ot_venc_h265_vui vui;
        td_s32 result = ss_mpi_venc_get_h265_vui(channel, &vui);
        if (result != TD_SUCCESS) return result;
        ca_hisi_vui::fill_h265(vui, attr->rc_attr.h265_cbr.dst_frame_rate);
        return ss_mpi_venc_set_h265_vui(channel, &vui);
    }
    if (attr->rc_attr.rc_mode != OT_VENC_RC_MODE_H264_CBR) return TD_FAILURE;
    ot_venc_h264_vui vui;
    td_s32 result = ss_mpi_venc_get_h264_vui(channel, &vui);
    if (result != TD_SUCCESS) return result;
    ca_hisi_vui::fill_h264(vui, attr->rc_attr.h264_cbr.dst_frame_rate);
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

td_s32 ca_hisi_set_gop(ot_venc_chn channel, td_u32 gop_frames)
{
    ot_venc_chn_attr attr;
    td_s32 result = ss_mpi_venc_get_chn_attr(channel, &attr);
    if (result != TD_SUCCESS) return result;
    if (attr.rc_attr.rc_mode == OT_VENC_RC_MODE_H265_CBR) {
        attr.rc_attr.h265_cbr.gop = gop_frames;
    } else if (attr.rc_attr.rc_mode == OT_VENC_RC_MODE_H264_CBR) {
        attr.rc_attr.h264_cbr.gop = gop_frames;
    } else {
        return TD_FAILURE;
    }
    return ss_mpi_venc_set_chn_attr(channel, &attr);
}

// applies f to the parameters of the channel's rate control mode
template <typename F> static td_s32 with_rc_param(ot_venc_chn channel, bool write, F f)
{
    ot_venc_chn_attr attr;
    ot_venc_rc_param param;
    td_s32 result = ss_mpi_venc_get_chn_attr(channel, &attr);
    if (result == TD_SUCCESS) result = ss_mpi_venc_get_rc_param(channel, &param);
    if (result != TD_SUCCESS) return result;
    switch (attr.rc_attr.rc_mode) {
    case OT_VENC_RC_MODE_H264_CBR: f(param.h264_cbr_param); break;
    case OT_VENC_RC_MODE_H265_CBR: f(param.h265_cbr_param); break;
    case OT_VENC_RC_MODE_H264_VBR: f(param.h264_vbr_param); break;
    case OT_VENC_RC_MODE_H265_VBR: f(param.h265_vbr_param); break;
    case OT_VENC_RC_MODE_H264_AVBR: f(param.h264_avbr_param); break;
    case OT_VENC_RC_MODE_H265_AVBR: f(param.h265_avbr_param); break;
    default: return TD_FAILURE;
    }
    return write ? ss_mpi_venc_set_rc_param(channel, &param) : TD_SUCCESS;
}

td_s32 ca_hisi_get_i_proportion(ot_venc_chn channel, ca_hisi_rc::IProportion *limits)
{
    return with_rc_param(channel, false, [limits](const auto &p) { *limits = ca_hisi_rc::read(p); });
}

td_s32 ca_hisi_set_i_proportion(ot_venc_chn channel, ca_hisi_rc::IProportion limits)
{
    return with_rc_param(channel, true, [limits](auto &p) { ca_hisi_rc::write(p, limits); });
}

td_s32 ca_hisi_set_slice_output(ot_venc_chn channel, td_u32 rows)
{
    ot_venc_slice_split split;
    td_s32 result = ss_mpi_venc_get_slice_split(channel, &split);
    if (result != TD_SUCCESS) return result;
    split.enable = TD_TRUE;
    // by rows, the only mode the encoder outputs slice by slice
    split.split_mode = 1U;
    split.split_size = rows;
    split.slice_output_en = TD_TRUE;
    return ss_mpi_venc_set_slice_split(channel, &split);
}

td_s32 ca_hisi_set_super_frame(ot_venc_chn channel, unsigned i_kbits,
                               const ot_venc_super_frame_strategy *defaults,
                               ot_venc_super_frame_mode *mode)
{
    ot_venc_super_frame_strategy strategy = *defaults;
    if (i_kbits != 0U) {
        const ca_hisi_rc::SuperFrame thresholds = ca_hisi_rc::super_frame(i_kbits);
        strategy.super_frame_mode = OT_VENC_SUPER_FRAME_REENCODE;
        strategy.i_frame_bits_threshold = thresholds.i_bits;
        strategy.p_frame_bits_threshold = thresholds.p_bits;
        strategy.reencode_priority = OT_VENC_REENCODE_BIT_RATE_FIRST;
    }
    td_s32 result = ss_mpi_venc_set_super_frame_strategy(channel, &strategy);
    if (result != TD_SUCCESS && i_kbits != 0U) {
        strategy.super_frame_mode = OT_VENC_SUPER_FRAME_DISCARD;
        result = ss_mpi_venc_set_super_frame_strategy(channel, &strategy);
    }
    if (result == TD_SUCCESS) *mode = strategy.super_frame_mode;
    return result;
}

static ot_low_delay_info low_delay(bool enable, td_u32 lines)
{
    ot_low_delay_info info = {};
    info.enable = enable ? TD_TRUE : TD_FALSE;
    info.line_cnt = lines;
    info.one_buf_en = TD_FALSE;
    return info;
}

td_s32 ca_hisi_set_vi_pipe_low_delay(td_u32 lines)
{
    const ot_low_delay_info info = low_delay(true, lines);
    return ss_mpi_vi_set_pipe_low_delay(CA_HISI_PIPE, &info);
}

td_s32 ca_hisi_set_vi_chn_low_delay(td_u32 lines)
{
    const ot_low_delay_info info = low_delay(true, lines);
    return ss_mpi_vi_set_chn_low_delay(CA_HISI_PIPE, 0, &info);
}

td_s32 ca_hisi_set_vpss_low_delay(ot_vpss_chn channel, bool enable, td_u32 lines)
{
    const ot_low_delay_info info = low_delay(enable, lines);
    return ss_mpi_vpss_set_chn_low_delay(CA_HISI_GROUP, channel, &info);
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
