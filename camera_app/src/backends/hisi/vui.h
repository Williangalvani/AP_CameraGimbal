#pragma once
/* The VUI every VENC channel sets. Templates over the SDK's ot_venc_h264_vui
 * and ot_venc_h265_vui, so host tests can fill look-alikes without SDK types. */

namespace ca_hisi_vui {

/* Without bitstream_restriction a decoder must assume frames are reordered
 * and holds the level's whole DPB (5 frames at 4K) before showing one. These
 * streams have no B-frames, so declare zero.
 * Without timing info the SPS has no frame rate, and GStreamer's h264parse
 * and h265parse report none: MAVLink Camera Manager's RTSP probe then fails. */

// H.264 ticks are fields: fps = time_scale / (2 * num_units_in_tick)
template <typename Vui> void fill_h264(Vui &vui, unsigned fps)
{
    vui.vui_time_info.timing_info_present_flag = 1;
    vui.vui_time_info.fixed_frame_rate_flag = 1;
    vui.vui_time_info.num_units_in_tick = 1000U;
    vui.vui_time_info.time_scale = 2U * fps * 1000U;
    vui.vui_bitstream_restric.bitstream_restriction_flag = 1;
}

// fps = time_scale / num_units_in_tick
template <typename Vui> void fill_h265(Vui &vui, unsigned fps)
{
    vui.vui_time_info.timing_info_present_flag = 1;
    vui.vui_time_info.num_units_in_tick = 1000U;
    vui.vui_time_info.time_scale = fps * 1000U;
    vui.vui_time_info.num_ticks_poc_diff_one_minus1 = 0U;
    vui.vui_bitstream_restric.bitstream_restriction_flag = 1;
}

}  // namespace ca_hisi_vui
