#include "rate_control.h"
#include "vui.h"

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <initializer_list>

/* The B040 ot_common_venc.h layouts, field types included */
struct BitstreamRestriction { uint8_t bitstream_restriction_flag; };
struct H264TimeInfo {
    uint8_t timing_info_present_flag;
    uint8_t fixed_frame_rate_flag;
    uint32_t num_units_in_tick;
    uint32_t time_scale;
};
struct H264Vui {
    H264TimeInfo vui_time_info;
    BitstreamRestriction vui_bitstream_restric;
};
struct H265TimeInfo {
    uint32_t timing_info_present_flag;
    uint32_t num_units_in_tick;
    uint32_t time_scale;
    uint32_t num_ticks_poc_diff_one_minus1;
};
struct H265Vui {
    H265TimeInfo vui_time_info;
    BitstreamRestriction vui_bitstream_restric;
};
/* the start of ot_venc_h265_cbr_param */
struct CbrParam {
    uint32_t max_i_proportion;
    uint32_t min_i_proportion;
    int32_t max_reencode_times;
};

static void check_rate_control()
{
    using ca_hisi_rc::IProportion;
    const IProportion encoder = {100U, 1U};
    // 0 leaves (and restores) what the encoder chose
    IProportion limits = ca_hisi_rc::i_proportion(0U, encoder);
    assert(limits.max == 100U && limits.min == 1U);
    limits = ca_hisi_rc::i_proportion(0U, IProportion{7U, 3U});
    assert(limits.max == 7U && limits.min == 3U);
    limits = ca_hisi_rc::i_proportion(4U, encoder);
    assert(limits.max == 4U && limits.min == 1U);
    limits = ca_hisi_rc::i_proportion(101U, encoder);
    assert(limits.max == 100U && limits.min == 1U);
    // the minimum stays unless it would pass the maximum
    limits = ca_hisi_rc::i_proportion(4U, IProportion{100U, 2U});
    assert(limits.max == 4U && limits.min == 2U);
    limits = ca_hisi_rc::i_proportion(4U, IProportion{100U, 5U});
    assert(limits.max == 4U && limits.min == 1U);
    limits = ca_hisi_rc::i_proportion(1U, IProportion{100U, 0U});
    assert(limits.max == 1U && limits.min == 1U);

    CbrParam param = {100U, 1U, 3};
    limits = ca_hisi_rc::read(param);
    assert(limits.max == 100U && limits.min == 1U);
    ca_hisi_rc::write(param, ca_hisi_rc::i_proportion(6U, limits));
    assert(param.max_i_proportion == 6U && param.min_i_proportion == 1U && param.max_reencode_times == 3);
}

int main()
{
    check_rate_control();
    for (unsigned fps : {5U, 15U, 25U, 30U}) {
        H264Vui h264 = {};
        ca_hisi_vui::fill_h264(h264, fps);
        assert(h264.vui_time_info.timing_info_present_flag == 1);
        assert(h264.vui_time_info.fixed_frame_rate_flag == 1);
        assert(h264.vui_time_info.num_units_in_tick == 1000U);
        assert(h264.vui_time_info.time_scale == 2U * fps * 1000U);
        // h264parse: framerate = time_scale / (2 * num_units_in_tick)
        assert(h264.vui_time_info.time_scale / (2U * h264.vui_time_info.num_units_in_tick) == fps);
        assert(h264.vui_bitstream_restric.bitstream_restriction_flag == 1);

        H265Vui h265 = {};
        h265.vui_time_info.num_ticks_poc_diff_one_minus1 = 7U;
        ca_hisi_vui::fill_h265(h265, fps);
        assert(h265.vui_time_info.timing_info_present_flag == 1U);
        assert(h265.vui_time_info.num_units_in_tick == 1000U);
        assert(h265.vui_time_info.time_scale == fps * 1000U);
        // h265parse: framerate = time_scale / num_units_in_tick
        assert(h265.vui_time_info.time_scale / h265.vui_time_info.num_units_in_tick == fps);
        assert(h265.vui_time_info.num_ticks_poc_diff_one_minus1 == 0U);
        assert(h265.vui_bitstream_restric.bitstream_restriction_flag == 1);
    }
    puts("PASS hisi VUI timing and bitstream restriction, key-frame size cap");
    return 0;
}
