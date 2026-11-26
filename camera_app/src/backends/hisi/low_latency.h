#pragma once
/* VENC slice output and VI/VPSS low delay. With slice output on, each
 * ss_mpi_venc_get_stream returns the packs (NAL units) of the slices encoded
 * so far, and the pack that ends the frame has is_frame_end. The assembler
 * turns those packs into parts for RTSP, sent at once, and whole access units
 * for the consumers that need them (recorder, frame socket, live view).
 * SDK-free so host tests can drive it. */

#include <cstddef>
#include <cstdint>
#include <vector>

namespace ca_hisi_ll {

// The SDK takes [1, the picture's macroblock (H.264) or LCU (H.265) rows]; a
// slice of the whole picture is one slice, so cap at the 16-pixel rows.
inline unsigned slice_rows(unsigned rows, unsigned height)
{
    const unsigned picture_rows = (height + 15U) / 16U;
    return rows < picture_rows ? rows : picture_rows;
}

// The SDK takes a low-delay line count in [16, picture height].
inline unsigned low_delay_lines(unsigned lines, unsigned height)
{
    if (lines > height) lines = height;
    return lines < 16U ? 16U : lines;
}

// a coded slice: H.264 NAL types 1-5, H.265 0-31
inline bool is_slice(bool h265, uint8_t header)
{
    const unsigned type = h265 ? (header >> 1U) & 0x3fU : header & 0x1fU;
    return h265 ? type < 32U : type >= 1U && type <= 5U;
}

inline bool has_slice(bool h265, const uint8_t *data, size_t size)
{
    for (size_t i = 0; i + 3U < size; i++) {
        if (data[i] == 0U && data[i + 1U] == 0U && data[i + 2U] == 1U && is_slice(h265, data[i + 3U]))
            return true;
    }
    return false;
}

/* Sink: part(data, size, first, last) for RTSP and frame(data, size, pts) for
 * the whole access unit, called in that order at its end. Every part holds a
 * slice, the first also the parameter sets and SEI before it, except that the
 * last may be empty when the end was not flagged: it then only closes the
 * access unit. */
class Assembler {
public:
    // an access unit is cut off at limit bytes, more than the VENC stream buffer
    explicit Assembler(bool h265, size_t limit = size_t(16) << 20) : h265_(h265), limit_(limit) {}

    /* One pack. A new pts while an access unit is open means its end was
     * not flagged, and that unit ends first. */
    template <typename Sink> void add(const uint8_t *data, size_t size, uint64_t pts, bool frame_end, Sink &sink)
    {
        if (!unit_.empty() && pts != pts_) {
            unterminated_++;
            finish(sink);
        }
        unit_.insert(unit_.end(), data, data + size);
        pts_ = pts;
        if (!frame_end && unit_.size() >= limit_) unterminated_++;
        if (frame_end || unit_.size() >= limit_) finish(sink);
    }

    // After the packs of one get_stream: sends the slices among them now.
    template <typename Sink> void flush(Sink &sink)
    {
        if (!has_slice(h265_, unit_.data() + sent_, unit_.size() - sent_)) return;
        sink.part(unit_.data() + sent_, unit_.size() - sent_, sent_ == 0U, false);
        sent_ = unit_.size();
    }

    // access units whose end the encoder did not flag
    unsigned long unterminated() const { return unterminated_; }

private:
    template <typename Sink> void finish(Sink &sink)
    {
        sink.part(unit_.data() + sent_, unit_.size() - sent_, sent_ == 0U, true);
        sink.frame(unit_.data(), unit_.size(), pts_);
        unit_.clear();
        sent_ = 0;
    }

    bool h265_;
    size_t limit_;
    std::vector<uint8_t> unit_;
    size_t sent_ = 0;
    uint64_t pts_ = 0;
    unsigned long unterminated_ = 0;
};

}  // namespace ca_hisi_ll
