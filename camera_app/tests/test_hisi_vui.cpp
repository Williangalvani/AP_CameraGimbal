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

int main()
{
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
    puts("PASS hisi VUI timing and bitstream restriction");
    return 0;
}
