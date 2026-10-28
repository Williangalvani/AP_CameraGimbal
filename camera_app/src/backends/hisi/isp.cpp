#include "isp.h"
#include "pipeline.h"
#include "image_tuning.h"

#include "camera_app/log.h"

#include "ss_mpi_ae.h"
#include "ss_mpi_awb.h"
#include "ss_mpi_isp.h"

#include <errno.h>
#include <math.h>
#include <string.h>
#include <time.h>

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

static td_s32 set_exposure(ot_vi_pipe pipe, const struct ca_config *config)
{
    using namespace ca_hisi_tuning;
    static const td_u32 exposure_us[] = {
        0U, 33333U, 20000U, 10000U, 4000U, 2000U, 1333U, 1000U, 500U,
    };
    ot_isp_exposure_attr attr;
    td_s32 result = ss_mpi_isp_get_exposure_attr(pipe, &attr);
    if (result != TD_SUCCESS) return result;

    // a fixed ISO fixes every gain, whatever the gain mode
    const bool manual_gains = config->iso == CA_ISO_AUTO && config->gain_mode == CA_GAIN_MANUAL;
    attr.op_type = config->iso != CA_ISO_AUTO || config->shutter != CA_SHUTTER_AUTO || manual_gains
        ? OT_OP_MODE_MANUAL : OT_OP_MODE_AUTO;
    attr.auto_attr.ev_bias = ev_bias(config->exposure_compensation);
    attr.auto_attr.compensation = (td_u8)config->backlight;
    attr.auto_attr.sys_gain_range.max = sys_gain_max(config->max_gain);
    attr.auto_attr.a_gain_range.max = again_max(config->max_again);
    attr.auto_attr.ispd_gain_range.max = ispd_gain_max(config->max_dgain);
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
        td_u32 gain = 1024U << ((unsigned)config->iso - 1U);
        attr.manual_attr.a_gain_op_type = OT_OP_MODE_MANUAL;
        attr.manual_attr.d_gain_op_type = OT_OP_MODE_MANUAL;
        attr.manual_attr.ispd_gain_op_type = OT_OP_MODE_MANUAL;
        attr.manual_attr.a_gain = gain;
        attr.manual_attr.d_gain = 1024U;
        attr.manual_attr.isp_d_gain = 1024U;
    }
    if (config->shutter == CA_SHUTTER_AUTO) {
        attr.manual_attr.exp_time_op_type = OT_OP_MODE_AUTO;
        attr.auto_attr.exp_time_range.max = exposure_max_us(config->max_exposure);
    } else {
        attr.manual_attr.exp_time_op_type = OT_OP_MODE_MANUAL;
        attr.manual_attr.exp_time = exposure_us[config->shutter];
    }
    return ss_mpi_isp_set_exposure_attr(pipe, &attr);
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
        stats.ae_cfg.weight[OT_ISP_AE_ZONE_ROW / 2U]
                           [OT_ISP_AE_ZONE_COLUMN / 2U] = 15U;
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

static unsigned clamp_gain(double gain)
{
    if (gain < 128.0) return 128U;
    if (gain > 4095.0) return 4095U;
    return (unsigned)lround(gain);
}

int ca_hisi_measure_white_balance(enum ca_wb_water water, struct ca_wb_gains *gains)
{
    static ot_isp_ae_stats stats;
    const unsigned rows = sizeof(stats.be_zone_avg) / sizeof(stats.be_zone_avg[0]);
    const unsigned columns = sizeof(stats.be_zone_avg[0]) / sizeof(stats.be_zone_avg[0][0]);
    double sum[OT_ISP_BAYER_CHN_NUM] = {};
    unsigned zones = 0;

    if ((water != CA_WB_WATER_GREEN && water != CA_WB_WATER_BLUE) || gains == NULL) {
        errno = EINVAL;
        return -1;
    }
    for (unsigned frame = 0; frame < 4U; frame++) {
        const struct timespec frame_time = {.tv_sec = 0, .tv_nsec = 40000000L};
        ot_isp_wb_info info;
        if (frame != 0U) (void)nanosleep(&frame_time, NULL);
        td_s32 result = ss_mpi_isp_query_wb_info(CA_HISI_PIPE, &info);
        if (result == TD_SUCCESS) result = ss_mpi_isp_get_ae_stats(CA_HISI_PIPE, &stats);
        if (result != TD_SUCCESS) {
            ca_log("white balance: ISP statistics failed: 0x%x", (unsigned)result);
            errno = EIO;
            return -1;
        }
        // the statistics include the current WB gains, which are divided out
        const td_u16 applied[OT_ISP_BAYER_CHN_NUM] = {info.r_gain, info.gr_gain, info.gb_gain, info.b_gain};
        double frame_sum[OT_ISP_BAYER_CHN_NUM] = {};
        unsigned frame_zones = 0;
        // the centre half of the picture in each direction
        for (unsigned row = rows / 4U; row < rows - rows / 4U; row++) {
            for (unsigned column = columns / 4U; column < columns - columns / 4U; column++) {
                for (unsigned channel = 0; channel < OT_ISP_BAYER_CHN_NUM; channel++) {
                    frame_sum[channel] += stats.be_zone_avg[row][column][channel];
                }
                frame_zones++;
            }
        }
        for (unsigned channel = 0; channel < OT_ISP_BAYER_CHN_NUM; channel++) {
            if (applied[channel] == 0U) {
                errno = EIO;
                return -1;
            }
            sum[channel] += frame_sum[channel] * 256.0 / applied[channel];
        }
        zones += frame_zones;
    }
    const double red = sum[0] / zones;
    const double green = (sum[1] + sum[2]) / (2.0 * zones);
    const double blue = sum[3] / zones;
    ca_log("white balance: centre averages R %.0f G %.0f B %.0f", red, green, blue);
    if (red < 64.0 || green < 256.0 || blue < 64.0) {
        errno = ERANGE;
        return -1;
    }
    if (water == CA_WB_WATER_GREEN) {
        // neutral, with red held 5% short as the vendor does
        gains->red = clamp_gain(256.0 + 0.95 * (256.0 * green / red - 256.0));
        gains->green = 256U;
        gains->blue = clamp_gain(256.0 * green / blue);
    } else {
        gains->red = clamp_gain(1.2 * 256.0 * blue / red);
        gains->green = 256U;
        gains->blue = 256U;
    }
    ca_log("white balance: one-push %s water gains R %u G %u B %u",
           water == CA_WB_WATER_GREEN ? "green" : "blue", gains->red, gains->green, gains->blue);
    return 0;
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

td_s32 ca_hisi_apply_isp_config(const struct ca_config *config)
{
    const ot_vi_pipe pipe = CA_HISI_PIPE;
    td_s32 result;

    if (config == NULL) return TD_FAILURE;
    if ((result = set_csc(pipe, config)) != TD_SUCCESS ||
        (result = set_exposure(pipe, config)) != TD_SUCCESS ||
        (result = set_metering(pipe, config->metering)) != TD_SUCCESS ||
        (result = set_white_balance(pipe, config)) != TD_SUCCESS) {
        ca_log("ISP configuration failed for pipe %d: 0x%x", pipe, (unsigned)result);
        return result;
    }
    set_tuning(pipe, config);
    ca_log("ISP configured brightness=%d saturation=%d contrast=%d ev=%d "
           "iso=%d shutter=%d metering=%d white_balance=%d (R %u G %u B %u, %u K)",
           config->brightness, config->saturation, config->contrast,
           config->exposure_compensation, config->iso, config->shutter,
           config->metering, config->white_balance, config->wb_red, config->wb_green,
           config->wb_blue, config->wb_temp);
    ca_log("ISP tuning hue=%u sharpness=%u gamma=%u backlight=%u style=%u/%u/%u water=%s "
           "max_gain=%u gain_mode=%s max_again=%u max_dgain=%u again=%s:%u dgain=%s:%u "
           "ae=%s max_exposure=1/%u slow_shutter=%s",
           config->hue, config->sharpness, config->gamma, config->backlight, config->wb_style_red,
           config->wb_style_green, config->wb_style_blue, config->wb_water == CA_WATER_BLUE ? "blue" : "green",
           config->max_gain, config->gain_mode == CA_GAIN_MANUAL ? "manual" : "auto", config->max_again,
           config->max_dgain, config->manual_again ? "on" : "off", config->again,
           config->manual_dgain ? "on" : "off", config->dgain,
           config->ae_strategy == CA_AE_PRIOR_LOWLIGHT ? "lowlight" : "highlight", config->max_exposure,
           config->slow_shutter ? "on" : "off");
    return TD_SUCCESS;
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
