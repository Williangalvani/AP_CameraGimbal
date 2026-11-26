// VENC slice output: the packs of each ss_mpi_venc_get_stream go to RTSP as
// parts that each hold a slice, parameter sets with the first, the access
// unit's end only on the frame-end pack; the other consumers get whole units.
#include "low_latency.h"

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <initializer_list>
#include <vector>

typedef std::vector<uint8_t> Bytes;

struct Part {
    Bytes data;
    bool first, last;
};
struct Frame {
    Bytes data;
    uint64_t pts;
};

struct Sink {
    std::vector<Part> parts;
    std::vector<Frame> frames;
    // the order of the calls: 'p' for a part, 'f' for a frame
    std::vector<char> calls;
    void part(const uint8_t *data, size_t size, bool first, bool last)
    {
        parts.push_back({Bytes(data, data + size), first, last});
        calls.push_back('p');
    }
    void frame(const uint8_t *data, size_t size, uint64_t pts)
    {
        frames.push_back({Bytes(data, data + size), pts});
        calls.push_back('f');
    }
};

// one H.265 NAL unit as VENC returns it in a pack
static Bytes nal(unsigned type, size_t payload)
{
    Bytes out = {0, 0, 0, 1, (uint8_t)(type << 1), 1};
    for (size_t i = 0; i < payload; i++) out.push_back((uint8_t)(0x80 | (i & 0x7f)));
    return out;
}

static Bytes h264_nal(unsigned type, size_t payload)
{
    Bytes out = {0, 0, 0, 1, (uint8_t)(0x60 | type)};
    for (size_t i = 0; i < payload; i++) out.push_back((uint8_t)(0x80 | (i & 0x7f)));
    return out;
}

static Bytes join(std::initializer_list<Bytes> parts)
{
    Bytes out;
    for (const Bytes &part : parts) out.insert(out.end(), part.begin(), part.end());
    return out;
}

static void add(ca_hisi_ll::Assembler &slices, Sink &sink, const Bytes &pack, uint64_t pts, bool end = false)
{
    slices.add(pack.data(), pack.size(), pts, end, sink);
}

static void check_key_frame_slices()
{
    ca_hisi_ll::Assembler slices(true);
    Sink sink;
    const Bytes vps = nal(32, 20), sps = nal(33, 40), pps = nal(34, 8), sei = nal(39, 30);
    const Bytes s1 = nal(19, 3000), s2 = nal(19, 2500), s3 = nal(19, 2800), s4 = nal(19, 2000);
    // the parameter sets come alone: they wait for the first slice
    add(slices, sink, vps, 1000);
    add(slices, sink, sps, 1000);
    add(slices, sink, pps, 1000);
    add(slices, sink, sei, 1000);
    slices.flush(sink);
    assert(sink.parts.empty() && sink.frames.empty());
    add(slices, sink, s1, 1000);
    slices.flush(sink);
    assert(sink.parts.size() == 1 && sink.frames.empty());
    assert(sink.parts[0].data == join({vps, sps, pps, sei, s1}));
    assert(sink.parts[0].first && !sink.parts[0].last);
    // two slices in one get_stream go together, sent before the frame ends
    add(slices, sink, s2, 1000);
    add(slices, sink, s3, 1000);
    slices.flush(sink);
    assert(sink.parts.size() == 2 && sink.frames.empty());
    assert(sink.parts[1].data == join({s2, s3}) && !sink.parts[1].first && !sink.parts[1].last);
    add(slices, sink, s4, 1000, true);
    assert(sink.parts.size() == 3 && sink.parts[2].data == s4);
    assert(!sink.parts[2].first && sink.parts[2].last);
    // the frame's consumers get the whole access unit, after its last part
    assert(sink.frames.size() == 1 && sink.frames[0].pts == 1000U);
    assert(sink.frames[0].data == join({vps, sps, pps, sei, s1, s2, s3, s4}));
    assert((sink.calls == std::vector<char>{'p', 'p', 'p', 'f'}));
    // nothing left over
    slices.flush(sink);
    assert(sink.parts.size() == 3 && slices.unterminated() == 0U);

    // a frame in one get_stream is one part, first and last
    const Bytes p1 = nal(1, 900), p2 = nal(1, 700);
    add(slices, sink, p1, 1033);
    add(slices, sink, p2, 1033, true);
    slices.flush(sink);
    assert(sink.parts.size() == 4 && sink.parts[3].data == join({p1, p2}));
    assert(sink.parts[3].first && sink.parts[3].last);
    assert(sink.frames.size() == 2 && sink.frames[1].data == join({p1, p2}) && sink.frames[1].pts == 1033U);
    puts("slices: parameter sets with the first slice, parts as encoded, end only on the frame end");
}

static void check_unterminated()
{
    ca_hisi_ll::Assembler slices(true);
    Sink sink;
    const Bytes a1 = nal(1, 500), a2 = nal(1, 400), b1 = nal(1, 600);
    add(slices, sink, a1, 1);
    slices.flush(sink);
    add(slices, sink, a2, 1);
    slices.flush(sink);
    assert(sink.parts.size() == 2 && sink.frames.empty());
    // the next frame's pts closes the open one with an empty last part
    add(slices, sink, b1, 2);
    assert(sink.parts.size() == 3 && sink.parts[2].data.empty() && sink.parts[2].last && !sink.parts[2].first);
    assert(sink.frames.size() == 1 && sink.frames[0].data == join({a1, a2}) && sink.frames[0].pts == 1U);
    assert(slices.unterminated() == 1U);
    slices.flush(sink);
    assert(sink.parts.size() == 4 && sink.parts[3].data == b1 && sink.parts[3].first && !sink.parts[3].last);

    // unsent data goes with the closing part
    ca_hisi_ll::Assembler held(true);
    Sink held_sink;
    const Bytes vps = nal(32, 10), c1 = nal(19, 300);
    add(held, held_sink, c1, 5);
    held.flush(held_sink);
    add(held, held_sink, vps, 5);
    add(held, held_sink, nal(1, 10), 6);
    assert(held_sink.parts.size() == 2 && held_sink.parts[1].data == vps && held_sink.parts[1].last);
    assert(held_sink.frames.size() == 1 && held_sink.frames[0].data == join({c1, vps}));

    // a frame end inside a get_stream: the rest begins the next unit
    ca_hisi_ll::Assembler mixed(true);
    Sink mixed_sink;
    const Bytes end = nal(1, 200), next_vps = nal(32, 10), next_slice = nal(19, 400);
    add(mixed, mixed_sink, end, 7, true);
    add(mixed, mixed_sink, next_vps, 8);
    mixed.flush(mixed_sink);
    assert(mixed_sink.parts.size() == 1 && mixed_sink.frames.size() == 1);
    add(mixed, mixed_sink, next_slice, 8, true);
    assert(mixed_sink.parts.size() == 2 && mixed_sink.parts[1].data == join({next_vps, next_slice}));
    assert(mixed_sink.parts[1].first && mixed_sink.parts[1].last && mixed.unterminated() == 0U);

    // an access unit past the limit is cut off
    ca_hisi_ll::Assembler limited(true, 1000);
    Sink limited_sink;
    add(limited, limited_sink, nal(19, 600), 9);
    add(limited, limited_sink, nal(19, 600), 9);
    assert(limited_sink.frames.size() == 1 && limited_sink.parts.back().last && limited.unterminated() == 1U);
    puts("slices: a frame end the encoder did not flag closes at the next pts, logged");
}

static void check_h264_and_limits()
{
    assert(!ca_hisi_ll::is_slice(false, 0x67) && !ca_hisi_ll::is_slice(false, 0x68));
    assert(!ca_hisi_ll::is_slice(false, 0x06) && ca_hisi_ll::is_slice(false, 0x65));
    assert(ca_hisi_ll::is_slice(false, 0x41) && ca_hisi_ll::is_slice(false, 0x01));
    for (unsigned type : {0U, 1U, 19U, 20U, 21U, 31U}) assert(ca_hisi_ll::is_slice(true, (uint8_t)(type << 1)));
    for (unsigned type : {32U, 33U, 34U, 35U, 39U, 40U}) assert(!ca_hisi_ll::is_slice(true, (uint8_t)(type << 1)));

    ca_hisi_ll::Assembler slices(false);
    Sink sink;
    const Bytes sps = h264_nal(7, 12), pps = h264_nal(8, 4), s1 = h264_nal(5, 900), s2 = h264_nal(5, 800);
    add(slices, sink, sps, 3);
    add(slices, sink, pps, 3);
    slices.flush(sink);
    assert(sink.parts.empty());
    add(slices, sink, s1, 3);
    slices.flush(sink);
    add(slices, sink, s2, 3, true);
    assert(sink.parts.size() == 2 && sink.parts[0].data == join({sps, pps, s1}) && sink.parts[1].data == s2);
    assert(sink.parts[0].first && !sink.parts[0].last && !sink.parts[1].first && sink.parts[1].last);
    assert(sink.frames.size() == 1 && sink.frames[0].data == join({sps, pps, s1, s2}));

    // slice heights capped at the picture's 16-pixel rows; 0 stays off
    assert(ca_hisi_ll::slice_rows(0U, 2160U) == 0U);
    assert(ca_hisi_ll::slice_rows(17U, 2160U) == 17U);
    assert(ca_hisi_ll::slice_rows(135U, 2160U) == 135U);
    assert(ca_hisi_ll::slice_rows(135U, 720U) == 45U);
    assert(ca_hisi_ll::slice_rows(100U, 1080U) == 68U);
    // low-delay lines within [16, height]
    assert(ca_hisi_ll::low_delay_lines(300U, 2160U) == 300U);
    assert(ca_hisi_ll::low_delay_lines(200U, 180U) == 180U);
    assert(ca_hisi_ll::low_delay_lines(0U, 720U) == 16U);
    puts("slices: H.264 slices, slice height and low-delay line limits");
}

int main()
{
    check_key_frame_slices();
    check_unterminated();
    check_h264_and_limits();
    puts("PASS hisi slice output");
    return 0;
}
