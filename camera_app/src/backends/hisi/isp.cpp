#include "isp.h"
#include "pipeline.h"
#include "image_tuning.h"
#include "iris.h"

#include "camera_app/log.h"

#include "ss_mpi_ae.h"
#include "ss_mpi_awb.h"
#include "ss_mpi_isp.h"

#include <errno.h>
#include <string.h>
#include <unistd.h>

static_assert(APCAM_HAVE_4KCAM_COMPAT, "the ISP applies the 4K Cam Manager's image settings");
static_assert(OT_ISP_AUTO_ISO_NUM == ca_hisi_tuning::iso_bins &&
              OT_ISP_SHARPEN_LUMA_NUM == ca_hisi_tuning::luma_bins &&
              OT_ISP_GAMMA_NODE_NUM == ca_hisi_tuning::gamma_nodes &&
              OT_ISP_CCM_MATRIX_SIZE == 9 && OT_ISP_CCM_MATRIX_NUM >= 4 &&
              OT_ISP_BAYER_CHN_NUM == 4 && OT_ISP_AWB_CURVE_PARA_NUM == 6,
              "SDK table sizes");

static td_s32 set_csc(ot_vi_pipe pipe, const struct ca_config *config)
{
    ot_isp_csc_attr attr;
    td_s32 result = ss_mpi_isp_get_csc_attr(pipe, &attr);
    if (result != TD_SUCCESS) return result;
    attr.luma = (td_u8)config->brightness;
    attr.satu = (td_u8)config->saturation;
    attr.contr = (td_u8)config->contrast;
    attr.hue = ca_hisi_tuning::csc_hue(config->hue);
    return ss_mpi_isp_set_csc_attr(pipe, &attr);
}

static td_u16 ev_bias(int tenths)
{
    if (tenths < 0) return (td_u16)((10240 + tenths * 896) / 10);
    return (td_u16)((10240 + tenths * 3072) / 10);
}

/* A range whose maximum is below its minimum is refused, and a refused
 * exposure stops the video at start-up. */
static td_u32 range_max(td_u32 wanted, td_u32 minimum)
{
    return wanted < minimum ? minimum : wanted;
}

/* Set by media start-up before its first apply. */
static bool dc_iris_available;
/* camera.ini asks for iris priority but start-up could not apply it: later
 * applies keep the normal exposure rather than refusing every change. */
static bool iris_priority_ignored;

void ca_hisi_set_dc_iris(bool available)
{
    dc_iris_available = available;
}

struct ae_ranges {
    ot_isp_ae_range time, again, dgain, ispd_gain, sys_gain;
};
/* The ranges as they were before iris priority pinned them, put back when it
 * ends: the other modes only set the maxima. */
static bool ranges_pinned;
static struct ae_ranges unpinned_ranges;

static struct ae_ranges get_ae_ranges(const ot_isp_ae_attr &ae)
{
    return {ae.exp_time_range, ae.a_gain_range, ae.d_gain_range, ae.ispd_gain_range, ae.sys_gain_range};
}

static void set_ae_ranges(ot_isp_ae_attr &ae, const struct ae_ranges &ranges)
{
    ae.exp_time_range = ranges.time;
    ae.a_gain_range = ranges.again;
    ae.d_gain_range = ranges.dgain;
    ae.ispd_gain_range = ranges.ispd_gain;
    ae.sys_gain_range = ranges.sys_gain;
}

static ot_isp_ae_range pinned_range(td_u32 value)
{
    ot_isp_ae_range range;
    range.min = range.max = value;
    return range;
}

/* The sensor's frame rate, which bounds the exposure; the encoders drop
 * frames from it to reach the configured stream rates. 0 when unknown. */
static unsigned sensor_frame_rate(ot_vi_pipe pipe)
{
    ot_isp_pub_attr pub;
    if (ss_mpi_isp_get_pub_attr(pipe, &pub) != TD_SUCCESS || !(pub.frame_rate >= 1.0f)) return 0U;
    unsigned fps = (unsigned)pub.frame_rate;
    return (float)fps < pub.frame_rate ? fps + 1U : fps;
}

/* What the last apply left, for the web's status file. */
static apcam_hisi_iris::status iris_status;

/* The AE keeps running, and with it the ISP's DC iris loop, which starts
 * closing at the route's lowest exposure: with every range pinned to one
 * value, that is the only thing left for it to move. */
static td_s32 set_iris_priority(ot_vi_pipe pipe, ot_isp_exposure_attr &attr, const struct ca_config *config)
{
    unsigned fps = sensor_frame_rate(pipe);
    if (fps == 0U) fps = APCAM_FRAME_RATE;
    const ca_hisi_tuning::PinnedExposure fixed = ca_hisi_tuning::iris_priority_exposure(*config, fps);
    const struct ae_ranges before = ranges_pinned ? unpinned_ranges : get_ae_ranges(attr.auto_attr);
    attr.op_type = OT_OP_MODE_AUTO;
    set_ae_ranges(attr.auto_attr, {pinned_range(fixed.exp_time_us), pinned_range(fixed.again),
                                   pinned_range(fixed.dgain), pinned_range(fixed.ispdgain),
                                   pinned_range(fixed.sys_gain)});
    attr.auto_attr.ae_mode = OT_ISP_AE_MODE_FIX_FRAME_RATE;
    attr.manual_attr.exp_time_op_type = OT_OP_MODE_AUTO;
    attr.manual_attr.a_gain_op_type = OT_OP_MODE_AUTO;
    attr.manual_attr.d_gain_op_type = OT_OP_MODE_AUTO;
    attr.manual_attr.ispd_gain_op_type = OT_OP_MODE_AUTO;
    const td_s32 result = ss_mpi_isp_set_exposure_attr(pipe, &attr);
    if (result == TD_SUCCESS) {
        unpinned_ranges = before;
        ranges_pinned = true;
        if (fixed.clamped) {
            ca_log("iris priority: shutter 1/%u s needs more than one frame at %u fps, using 1/%u s",
                   apcam_hisi_iris::shutter_denominator(fixed.requested_us), fps,
                   apcam_hisi_iris::shutter_denominator(fixed.exp_time_us));
        }
        ca_log("ISP iris priority: exposure %u us, gains (1/1024) analog %u digital %u ISP %u system %u",
               fixed.exp_time_us, fixed.again, fixed.dgain, fixed.ispdgain, fixed.sys_gain);
        iris_status.shutter_requested_us = fixed.requested_us;
        iris_status.shutter_applied_us = fixed.exp_time_us;
    }
    return result;
}

static td_s32 set_exposure(ot_vi_pipe pipe, const struct ca_config *config, bool iris_priority)
{
    using namespace ca_hisi_tuning;
    ot_isp_exposure_attr attr;
    td_s32 result = ss_mpi_isp_get_exposure_attr(pipe, &attr);
    if (result != TD_SUCCESS) return result;

    attr.auto_attr.ev_bias = ev_bias(config->exposure_compensation);
    attr.auto_attr.compensation = (td_u8)config->backlight;
    if (iris_priority) return set_iris_priority(pipe, attr, config);
    if (ranges_pinned) set_ae_ranges(attr.auto_attr, unpinned_ranges);

    // a fixed ISO fixes every gain, whatever the gain mode
    const bool manual_gains = config->iso == CA_ISO_AUTO && config->gain_mode == CA_GAIN_MANUAL;
    attr.op_type = config->iso != CA_ISO_AUTO || config->shutter != CA_SHUTTER_AUTO || manual_gains
        ? OT_OP_MODE_MANUAL : OT_OP_MODE_AUTO;
    attr.auto_attr.sys_gain_range.max =
        range_max(sys_gain_max(config->max_gain), attr.auto_attr.sys_gain_range.min);
    attr.auto_attr.a_gain_range.max =
        range_max(again_max(config->max_again), attr.auto_attr.a_gain_range.min);
    attr.auto_attr.ispd_gain_range.max =
        range_max(ispd_gain_max(config->max_dgain), attr.auto_attr.ispd_gain_range.min);
    attr.auto_attr.ae_strategy_mode = config->ae_strategy == CA_AE_PRIOR_LOWLIGHT
        ? OT_ISP_AE_EXP_LOWLIGHT_PRIOR : OT_ISP_AE_EXP_HIGHLIGHT_PRIOR;
    attr.auto_attr.ae_mode = config->slow_shutter ? OT_ISP_AE_MODE_SLOW_SHUTTER : OT_ISP_AE_MODE_FIX_FRAME_RATE;
    if (config->iso == CA_ISO_AUTO) {
        const bool again = manual_gains && config->manual_again;
        const bool dgain = manual_gains && config->manual_dgain;
        attr.manual_attr.a_gain_op_type = again ? OT_OP_MODE_MANUAL : OT_OP_MODE_AUTO;
        attr.manual_attr.d_gain_op_type = dgain ? OT_OP_MODE_MANUAL : OT_OP_MODE_AUTO;
        attr.manual_attr.ispd_gain_op_type = dgain ? OT_OP_MODE_MANUAL : OT_OP_MODE_AUTO;
        attr.manual_attr.a_gain = manual_gain(config->again);
        attr.manual_attr.d_gain = 1024U;
        attr.manual_attr.isp_d_gain = manual_gain(config->dgain);
    } else {
        attr.manual_attr.a_gain_op_type = OT_OP_MODE_MANUAL;
        attr.manual_attr.d_gain_op_type = OT_OP_MODE_MANUAL;
        attr.manual_attr.ispd_gain_op_type = OT_OP_MODE_MANUAL;
        attr.manual_attr.a_gain = iso_gain((unsigned)config->iso);
        attr.manual_attr.d_gain = 1024U;
        attr.manual_attr.isp_d_gain = 1024U;
    }
    ot_isp_ae_attr &limits = attr.auto_attr;
    const ot_isp_me_attr &fixed = attr.manual_attr;
    if (fixed.a_gain_op_type == OT_OP_MODE_MANUAL)
        limits.a_gain_range.max = with_manual(limits.a_gain_range.max, fixed.a_gain);
    if (fixed.d_gain_op_type == OT_OP_MODE_MANUAL)
        limits.d_gain_range.max = with_manual(limits.d_gain_range.max, fixed.d_gain);
    if (fixed.ispd_gain_op_type == OT_OP_MODE_MANUAL)
        limits.ispd_gain_range.max = with_manual(limits.ispd_gain_range.max, fixed.isp_d_gain);
    if (attr.op_type == OT_OP_MODE_MANUAL)
        limits.sys_gain_range.max = with_manual(limits.sys_gain_range.max,
            sys_gain(fixed.a_gain_op_type == OT_OP_MODE_MANUAL ? fixed.a_gain : 1024U,
                     fixed.d_gain_op_type == OT_OP_MODE_MANUAL ? fixed.d_gain : 1024U,
                     fixed.ispd_gain_op_type == OT_OP_MODE_MANUAL ? fixed.isp_d_gain : 1024U));
    if (config->shutter == CA_SHUTTER_AUTO) {
        attr.manual_attr.exp_time_op_type = OT_OP_MODE_AUTO;
        attr.auto_attr.exp_time_range.max =
            range_max(exposure_max_us(config->max_exposure), attr.auto_attr.exp_time_range.min);
    } else {
        attr.manual_attr.exp_time_op_type = OT_OP_MODE_MANUAL;
        attr.manual_attr.exp_time = shutter_us(config->shutter);
    }
    result = ss_mpi_isp_set_exposure_attr(pipe, &attr);
    if (result == TD_SUCCESS) ranges_pinned = false;
    return result;
}

static td_s32 set_metering(ot_vi_pipe pipe, enum ca_metering_mode mode)
{
    ot_isp_stats_cfg stats;
    td_s32 result = ss_mpi_isp_get_stats_cfg(pipe, &stats);
    if (result != TD_SUCCESS) return result;
    memset(stats.ae_cfg.weight, 1, sizeof(stats.ae_cfg.weight));
    if (mode == CA_METERING_CENTER) {
        for (unsigned row = 5U; row <= 9U; row++) {
            for (unsigned column = 6U; column <= 10U; column++) {
                stats.ae_cfg.weight[row][column] = 8U;
            }
        }
    } else if (mode == CA_METERING_SPOT) {
        /* Only the middle 3x3 zones, about 3.5% of the picture: one zone
         * weighted among 254 others moves the exposure by about 1%. */
        memset(stats.ae_cfg.weight, 0, sizeof(stats.ae_cfg.weight));
        for (unsigned row = OT_ISP_AE_ZONE_ROW / 2U - 1U; row <= OT_ISP_AE_ZONE_ROW / 2U + 1U; row++) {
            for (unsigned column = OT_ISP_AE_ZONE_COLUMN / 2U - 1U;
                 column <= OT_ISP_AE_ZONE_COLUMN / 2U + 1U; column++) {
                stats.ae_cfg.weight[row][column] = 15U;
            }
        }
    }
    return ss_mpi_isp_set_stats_cfg(pipe, &stats);
}

static td_u16 preset_kelvin(enum ca_white_balance mode, unsigned custom)
{
    switch (mode) {
    case CA_WB_DAYLIGHT: return 5500U;
    case CA_WB_CLOUDY: return 6500U;
    case CA_WB_FLUORESCENT: return 4000U;
    case CA_WB_INCANDESCENT: return 2800U;
    case CA_WB_TEMPERATURE: return (td_u16)custom;
    case CA_WB_AUTO:
    case CA_WB_MANUAL:
        break;
    }
    return 0U;
}

static td_s32 set_white_balance(ot_vi_pipe pipe, const struct ca_config *config)
{
    ot_isp_wb_attr attr;
    td_s32 result = ss_mpi_isp_get_wb_attr(pipe, &attr);
    if (result != TD_SUCCESS) return result;
    const ca_hisi_tuning::Water &water = ca_hisi_tuning::waters[config->wb_water == CA_WATER_BLUE ? 1 : 0];
    for (unsigned i = 0; i < OT_ISP_BAYER_CHN_NUM; i++) attr.auto_attr.static_wb[i] = water.static_wb[i];
    for (unsigned i = 0; i < OT_ISP_AWB_CURVE_PARA_NUM; i++) attr.auto_attr.curve_para[i] = water.curve[i];
    attr.op_type = OT_OP_MODE_AUTO;
    attr.auto_attr.rg_strength = 128U;
    attr.auto_attr.bg_strength = 128U;
    if (config->white_balance == CA_WB_MANUAL) {
        attr.op_type = OT_OP_MODE_MANUAL;
        attr.manual_attr = (ot_isp_mwb_attr){(td_u16)config->wb_red, (td_u16)config->wb_green,
                                             (td_u16)config->wb_green, (td_u16)config->wb_blue};
    } else if (config->white_balance != CA_WB_AUTO) {
        td_u16 gains[OT_ISP_BAYER_CHN_NUM];
        const td_u16 kelvin = preset_kelvin(config->white_balance, config->wb_temp);
        // the curve must already be the calibrated one
        result = ss_mpi_isp_cal_gain_by_temp(pipe, &attr, kelvin, 0, gains);
        if (result == TD_SUCCESS) {
            attr.op_type = OT_OP_MODE_MANUAL;
            attr.manual_attr = (ot_isp_mwb_attr){gains[0], gains[1], gains[2], gains[3]};
        } else {
            // a saved temperature the SDK rejects must not stop the pipeline
            ca_log("white balance: no gains for %u K (0x%x); using auto", kelvin,
                   (unsigned)result);
        }
    }
    return ss_mpi_isp_set_wb_attr(pipe, &attr);
}

td_s32 ca_hisi_apply_white_balance(const struct ca_config *config)
{
    return config == NULL ? TD_FAILURE : set_white_balance(CA_HISI_PIPE, config);
}

td_s32 ca_hisi_white_balance_gains(td_u16 *red, td_u16 *green, td_u16 *blue)
{
    ot_isp_wb_info info;
    td_s32 result = ss_mpi_isp_query_wb_info(CA_HISI_PIPE, &info);
    if (result != TD_SUCCESS) return result;
    *red = info.r_gain;
    *green = info.gr_gain;
    *blue = info.b_gain;
    return TD_SUCCESS;
}

td_s32 ca_hisi_set_white_balance_gains(bool manual, td_u16 red, td_u16 green, td_u16 blue)
{
    ot_isp_wb_attr attr;
    td_s32 result = ss_mpi_isp_get_wb_attr(CA_HISI_PIPE, &attr);
    if (result != TD_SUCCESS) return result;
    attr.op_type = manual ? OT_OP_MODE_MANUAL : OT_OP_MODE_AUTO;
    if (manual) attr.manual_attr = (ot_isp_mwb_attr){red, green, green, blue};
    return ss_mpi_isp_set_wb_attr(CA_HISI_PIPE, &attr);
}

/* Auto in every exposure mode, as with the vendor firmware; the loop's gains
 * are left alone. */
static td_s32 set_auto_dc_iris(ot_vi_pipe pipe)
{
    ot_isp_iris_attr iris;
    td_s32 result = ss_mpi_isp_get_iris_attr(pipe, &iris);
    if (result != TD_SUCCESS) return result;
    iris.enable = TD_TRUE;
    iris.op_type = OT_OP_MODE_AUTO;
    iris.iris_type = OT_ISP_IRIS_TYPE_DC;
    iris.iris_status = OT_ISP_IRIS_KEEP;
    return ss_mpi_isp_set_iris_attr(pipe, &iris);
}

// the settings the vendor firmware uses on the U35.5 board
td_s32 ca_hisi_enable_dc_iris(void)
{
    td_s32 result = set_auto_dc_iris(CA_HISI_PIPE);
    if (result != TD_SUCCESS) return result;
    ot_isp_dciris_attr dciris;
    result = ss_mpi_isp_get_dciris_attr(CA_HISI_PIPE, &dciris);
    if (result != TD_SUCCESS) return result;
    dciris.min_pwm_duty = 10U;
    dciris.kp = 7000;
    return ss_mpi_isp_set_dciris_attr(CA_HISI_PIPE, &dciris);
}

static td_s32 set_colour_matrices(ot_vi_pipe pipe, enum ca_water colour)
{
    const ca_hisi_tuning::Water &water = ca_hisi_tuning::waters[colour == CA_WATER_BLUE ? 1 : 0];
    ot_isp_color_matrix_attr attr;
    td_s32 result = ss_mpi_isp_get_ccm_attr(pipe, &attr);
    if (result != TD_SUCCESS) return result;
    attr.op_type = OT_OP_MODE_AUTO;
    attr.auto_attr.ccm_tab_num = 4U;
    for (unsigned t = 0; t < 4U; t++) {
        attr.auto_attr.ccm_tab[t].color_temp = water.temps[t];
        for (unsigned i = 0; i < OT_ISP_CCM_MATRIX_SIZE; i++) attr.auto_attr.ccm_tab[t].ccm[i] = water.ccm[t][i];
    }
    for (unsigned i = 0; i < OT_ISP_CCM_MATRIX_SIZE; i++) attr.manual_attr.ccm[i] = water.ccm[1][i];
    return ss_mpi_isp_set_ccm_attr(pipe, &attr);
}

static td_s32 set_colour_tone(ot_vi_pipe pipe, const struct ca_config *config)
{
    const ot_isp_color_tone_attr attr = {ca_hisi_tuning::cast_gain(config->wb_style_red),
                                         ca_hisi_tuning::cast_gain(config->wb_style_green),
                                         ca_hisi_tuning::cast_gain(config->wb_style_blue)};
    return ss_mpi_isp_set_color_tone_attr(pipe, &attr);
}

static td_s32 set_gamma(ot_vi_pipe pipe, unsigned gamma)
{
    const int curve = ca_hisi_tuning::gamma_curve(gamma);
    if (curve < 0) return TD_SUCCESS;
    static ot_isp_gamma_attr attr;  // 2 KB; ISP applies are serialised
    td_s32 result = ss_mpi_isp_get_gamma_attr(pipe, &attr);
    if (result != TD_SUCCESS) return result;
    attr.enable = TD_TRUE;
    attr.curve_type = OT_ISP_GAMMA_CURVE_USER_DEFINE;
    ca_hisi_tuning::gamma_table(curve, attr.table);
    return ss_mpi_isp_set_gamma_attr(pipe, &attr);
}

static td_s32 set_sharpen(ot_vi_pipe pipe, unsigned sharpness)
{
    using namespace ca_hisi_tuning;
    static ot_isp_sharpen_attr attr;  // about 17 KB; ISP applies are serialised
    td_s32 result = ss_mpi_isp_get_sharpen_attr(pipe, &attr);
    if (result != TD_SUCCESS) return result;
    attr.enable = TD_TRUE;
    for (unsigned bin = 0; bin < iso_bins; bin++) {
        attr.auto_attr.texture_freq[bin] = sharpen_texture_freq[bin];
        attr.auto_attr.edge_freq[bin] = sharpen_edge_freq[bin];
        attr.auto_attr.over_shoot[bin] = sharpen_over_shoot[bin];
        attr.auto_attr.under_shoot[bin] = sharpen_under_shoot[bin];
        attr.auto_attr.shoot_sup_strength[bin] = 0U;
        attr.auto_attr.shoot_sup_adj[bin] = sharpen_shoot_sup_adj[bin];
        attr.auto_attr.max_sharp_gain[bin] = sharpen_max_gain[bin];
        attr.auto_attr.detail_ctrl[bin] = sharpen_detail(sharpness, bin);
        for (unsigned luma = 0; luma < luma_bins; luma++)
            attr.auto_attr.luma_wgt[luma][bin] = sharpen_luma_weight[luma];
    }
    return ss_mpi_isp_set_sharpen_attr(pipe, &attr);
}

/* The picture's look: a value the SDK refuses keeps the previous one and is
 * logged, rather than stopping the video. */
static void set_tuning(ot_vi_pipe pipe, const struct ca_config *config)
{
    const struct {
        const char *name;
        td_s32 result;
    } steps[] = {
        {"colour matrices", set_colour_matrices(pipe, config->wb_water)},
        {"colour tone", set_colour_tone(pipe, config)},
        {"gamma", set_gamma(pipe, config->gamma)},
        {"sharpening", set_sharpen(pipe, config->sharpness)},
    };
    for (const auto &step : steps)
        if (step.result != TD_SUCCESS) ca_log("ISP %s failed: 0x%x", step.name, (unsigned)step.result);
}

static td_s32 apply_isp_config(const struct ca_config *config, bool startup)
{
    const ot_vi_pipe pipe = CA_HISI_PIPE;
    td_s32 result;

    if (config == NULL) return TD_FAILURE;
    // back to auto on every apply, whatever else last set the iris
    bool iris = dc_iris_available;
    if (iris && (result = set_auto_dc_iris(pipe)) != TD_SUCCESS) {
        ca_log("ISP DC iris failed: 0x%x", (unsigned)result);
        iris = false;
    }
    iris_status.dc_iris = iris;
    const bool wanted = config->iris_mode == CA_IRIS_PRIORITY;
    const bool fall_back = startup || iris_priority_ignored;
    bool iris_priority = wanted;
    if (wanted && !iris) {
        if (!fall_back) {
            ca_log("iris priority needs the DC iris, which this camera does not have or could not start; "
                   "not applied");
            return TD_FAILURE;
        }
        ca_log("iris priority needs the DC iris, which this camera does not have or could not start; "
               "using the normal exposure");
        iris_priority = false;
    }
    if ((result = set_csc(pipe, config)) == TD_SUCCESS) {
        result = set_exposure(pipe, config, iris_priority);
        if (result != TD_SUCCESS && iris_priority && fall_back) {
            ca_log("ISP refused the iris priority exposure (0x%x); using the normal exposure",
                   (unsigned)result);
            iris_priority = false;
            result = set_exposure(pipe, config, false);
        }
        if (result == TD_SUCCESS) {
            iris_status.priority_active = iris_priority;
            if (!iris_priority) iris_status.shutter_requested_us = iris_status.shutter_applied_us = 0U;
        }
    }
    if (result != TD_SUCCESS ||
        (result = set_metering(pipe, config->metering)) != TD_SUCCESS ||
        (result = set_white_balance(pipe, config)) != TD_SUCCESS) {
        ca_log("ISP configuration failed for pipe %d: 0x%x", pipe, (unsigned)result);
        return result;
    }
    iris_priority_ignored = wanted && !iris_priority;
    set_tuning(pipe, config);
    ca_log("ISP configured brightness=%d saturation=%d contrast=%d ev=%d "
           "iso=%d shutter=%d metering=%d white_balance=%d (R %u G %u B %u, %u K)",
           config->brightness, config->saturation, config->contrast,
           config->exposure_compensation, config->iso, config->shutter,
           config->metering, config->white_balance, config->wb_red, config->wb_green,
           config->wb_blue, config->wb_temp);
    ca_log("ISP tuning hue=%u sharpness=%u gamma=%u backlight=%u style=%u/%u/%u water=%s "
           "max_gain=%u gain_mode=%s max_again=%u max_dgain=%u again=%s:%u dgain=%s:%u "
           "ae=%s max_exposure=1/%u slow_shutter=%s iris=%s",
           config->hue, config->sharpness, config->gamma, config->backlight, config->wb_style_red,
           config->wb_style_green, config->wb_style_blue, config->wb_water == CA_WATER_BLUE ? "blue" : "green",
           config->max_gain, config->gain_mode == CA_GAIN_MANUAL ? "manual" : "auto", config->max_again,
           config->max_dgain, config->manual_again ? "on" : "off", config->again,
           config->manual_dgain ? "on" : "off", config->dgain,
           config->ae_strategy == CA_AE_PRIOR_LOWLIGHT ? "lowlight" : "highlight", config->max_exposure,
           config->slow_shutter ? "on" : "off",
           iris_priority ? "priority" : wanted ? "priority (not applied)" : "auto");
    return TD_SUCCESS;
}

td_s32 ca_hisi_apply_isp_config(const struct ca_config *config, bool startup)
{
    const td_s32 result = apply_isp_config(config, startup);
    if (!startup) ca_hisi_publish_iris_status();
    return result;
}

void ca_hisi_publish_iris_status(void)
{
    iris_status.pid = (long)getpid();
    if (iris_write_status(iris_status) < 0) ca_log("iris status not written: %s", strerror(errno));
}

int ca_hisi_exposure(unsigned lens, struct ca_exposure *s)
{
    if (lens != 0U) return -ENOTSUP;
    ot_isp_exp_info info = {};
    td_s32 result = ss_mpi_isp_query_exposure_info(CA_HISI_PIPE, &info);
    if (result != TD_SUCCESS) return result;
    s->shutter_us = info.exp_time;
    s->analog_gain = info.a_gain / 1024.0f;
    s->digital_gain = info.d_gain / 1024.0f;
    s->isp_gain = info.isp_d_gain / 1024.0f;
    s->luma = info.ave_lum;
    /* SDK histogram error is logged raw; it is not a luminance target. */
    s->error = info.hist_error;
    s->state = info.exposure_is_max ? CA_AE_STATE_LIMIT : 0;
    s->valid = CA_AE_SHUTTER | CA_AE_AGAIN | CA_AE_DGAIN | CA_AE_IGAIN |
               CA_AE_LUMA | CA_AE_ERROR | CA_AE_LIMIT;
    ot_isp_exposure_attr attr = {(td_bool)(0)};
    result = ss_mpi_isp_get_exposure_attr(CA_HISI_PIPE, &attr);
    if (result != TD_SUCCESS) return result;
    if (attr.op_type == OT_OP_MODE_AUTO) {
        s->mode = CA_AE_AUTO;
    } else {
        bool auto_gain = attr.manual_attr.a_gain_op_type == OT_OP_MODE_AUTO ||
                         attr.manual_attr.d_gain_op_type == OT_OP_MODE_AUTO ||
                         attr.manual_attr.ispd_gain_op_type == OT_OP_MODE_AUTO;
        bool auto_time = attr.manual_attr.exp_time_op_type == OT_OP_MODE_AUTO;
        s->mode = auto_gain ? (auto_time ? CA_AE_AUTO : CA_AE_SHUTTER_PRIORITY)
                            : (auto_time ? CA_AE_GAIN_PRIORITY : CA_AE_MANUAL);
    }
    s->valid |= CA_AE_MODE;
    /* This SDK does not expose the current target or convergence flag. */
    return 0;
}
