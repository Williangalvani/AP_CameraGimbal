#include "camera_app/rtp_pacer.h"

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <vector>

// RTP over TCP: 1420 payload + RTP header + interleave header
static const uint32_t kPacket = 1436;

// Sends packets back to back as the event loop does: a packet that is not due
// waits for a 1 ms timer tick at least. Returns each packet's send time in us.
static std::vector<int64_t> drain(ca::RtpPacer &pacer, int64_t &now_us, size_t packets)
{
    std::vector<int64_t> sent;
    while (sent.size() < packets) {
        const int64_t wait = pacer.Take(kPacket, now_us);
        if (wait == 0) {
            sent.push_back(now_us);
            continue;
        }
        now_us += (wait + 999) / 1000 * 1000;
    }
    return sent;
}

static size_t sent_before(const std::vector<int64_t> &sent, int64_t until_us)
{
    size_t count = 0;
    for (int64_t at : sent) count += at < until_us;
    return count;
}

static int64_t bucket_bytes(uint64_t rate)
{
    const int64_t by_time = int64_t(rate / 8) * ca::RtpPacer::kBucketUs / 1000000;
    return by_time > ca::RtpPacer::kBurstBytes ? by_time : ca::RtpPacer::kBurstBytes;
}

struct Frame {
    int64_t at_us;
    size_t packets;
};

// The camera's kernel has 10 ms jiffies and no high-resolution timers, so a
// paced wait of any length wakes the drain on the next tick of tick_us. Each
// wakeup sends every packet the tokens allow, as the drain loop does. A frame
// wakes an idle drain at once; one queued during a wait goes at the next tick.
// Returns each packet's send time in us; *max_queued is the largest backlog.
static std::vector<int64_t> stream(ca::RtpPacer &pacer, const std::vector<Frame> &frames, int64_t tick_us,
                                   size_t *max_queued = nullptr)
{
    std::vector<int64_t> sent;
    size_t next = 0, queued = 0;
    int64_t now = frames.front().at_us;
    if (max_queued) *max_queued = 0;
    while (next < frames.size() || queued > 0) {
        while (next < frames.size() && frames[next].at_us <= now) queued += frames[next++].packets;
        if (max_queued && queued > *max_queued) *max_queued = queued;
        if (queued == 0) {
            pacer.Idle(now);
            now = frames[next].at_us;
            continue;
        }
        const int64_t wait = pacer.Take(kPacket, now);
        if (wait == 0) {
            sent.push_back(now);
            queued--;
            continue;
        }
        now = (now + wait + tick_us - 1) / tick_us * tick_us;
    }
    return sent;
}

static size_t packets_of(size_t bytes)
{
    return bytes / 1420 + 1;
}

// The largest number of bytes sent in any window_us.
static int64_t max_window_bytes(const std::vector<int64_t> &sent, int64_t window_us)
{
    int64_t most = 0;
    size_t first = 0;
    for (size_t last = 0; last < sent.size(); last++) {
        while (sent[last] - sent[first] >= window_us) first++;
        const int64_t bytes = int64_t(last - first + 1) * kPacket;
        if (bytes > most) most = bytes;
    }
    return most;
}

int main()
{
    // Off: every packet goes at once.
    {
        ca::RtpPacer pacer;
        for (int i = 0; i < 10000; i++) assert(pacer.Take(kPacket, 1000) == 0);
        pacer.SetRate(0, 2000);
        assert(pacer.Take(kPacket, 2000) == 0 && pacer.Rate() == 0);
    }

    // A 685 KB key frame at 60 Mbit/s: a burst of 16 packets, then the rest
    // spread at the rate, about 90 ms in all instead of one line-rate burst.
    {
        ca::RtpPacer pacer;
        int64_t now = 5000000;
        pacer.SetRate(60000000U, now);
        const size_t packets = 685000 / 1420 + 1;
        const std::vector<int64_t> sent = drain(pacer, now, packets);
        assert(sent_before(sent, 5000000 + 1) == 16);
        const int64_t span = sent.back() - sent.front();
        const int64_t ideal = (int64_t(packets) * kPacket - ca::RtpPacer::kBurstBytes) * 8 / 60;
        printf("key frame: %zu packets over %lld us (ideal %lld us)\n", packets, (long long)span,
               (long long)ideal);
        assert(span >= ideal - 1000 && span <= ideal + 2000);
        // No 10 ms window carries more than the rate plus one burst.
        for (int64_t start = sent.front(); start < sent.back(); start += 500) {
            size_t bytes = 0;
            for (int64_t at : sent) bytes += at >= start && at < start + 10000 ? kPacket : 0;
            assert(bytes <= 60000000U / 8 / 100 + ca::RtpPacer::kBurstBytes);
        }
        // Idle refills the burst, and no more.
        pacer.Idle(now);
        now += 1000000;
        assert(drain(pacer, now, 16).back() == now);
        assert(pacer.Take(kPacket, now) > 0);
    }

    // A late wakeup with packets waiting keeps 30 ms at the rate, or 16
    // packets where that is more.
    for (uint64_t rate : {400000000U, 2000000U}) {
        ca::RtpPacer pacer;
        pacer.SetRate(rate, 0);
        while (pacer.Take(kPacket, 0) == 0) {}
        size_t kept = 0;
        while (pacer.Take(kPacket, 1000000) == 0) kept++;
        assert(kept == size_t(bucket_bytes(rate) / kPacket));
    }

    // Wakeups every 10 or 20 ms with packets always waiting still get the
    // rate over a second; no 10 ms carries more than the rate plus the bucket.
    for (int64_t tick : {10000, 20000}) {
        for (uint64_t rate : {10000000U, 28000000U, 60000000U, 100000000U}) {
            ca::RtpPacer pacer;
            const int64_t start = 3000000 + 3000;
            pacer.SetRate(rate, start);
            const std::vector<int64_t> sent = stream(pacer, {{start, packets_of(rate / 8 * 2)}}, tick);
            const double delivered = double(sent_before(sent, start + 1000000)) * kPacket * 8;
            printf("%lld ms wakeups: %.1f of %.1f Mbit/s in 1 s\n", (long long)tick / 1000, delivered / 1e6,
                   double(rate) / 1e6);
            assert(delivered >= rate * 0.95 && delivered <= rate * 1.05);
            assert(max_window_bytes(sent, 10000) <= int64_t(rate / 8 / 100) + bucket_bytes(rate));
        }
    }

    // A 600 KB frame after a gap at 60 Mbit/s leaves over about 80 ms, less
    // what the last frame left in the bucket, plus up to one 10 or 20 ms
    // wakeup, whatever its phase against the wakeups.
    for (int64_t tick : {10000, 20000}) {
        int64_t fastest = INT64_MAX, slowest = 0;
        for (int64_t phase = 0; phase < tick; phase += 500) {
            ca::RtpPacer pacer;
            pacer.SetRate(60000000U, 0);
            const int64_t at = 1000000 + phase;
            const std::vector<int64_t> sent =
                stream(pacer, {{0, packets_of(117000)}, {at, packets_of(600000)}}, tick);
            const int64_t span = sent.back() - at;
            if (span < fastest) fastest = span;
            if (span > slowest) slowest = span;
            assert(sent[sent.size() - packets_of(600000)] == at);
        }
        printf("600 KB frame, %lld ms wakeups: %lld-%lld us\n", (long long)tick / 1000, (long long)fastest,
               (long long)slowest);
        assert(fastest >= 70000 && slowest <= 100000);
    }

    // The 4K main stream (685 KB key frames, 117 KB P frames, 30 fps) paced
    // at 60 Mbit/s with 10 or 20 ms wakeups keeps up: key frames leave in
    // about 90 ms and the backlog stays far below the 2048-packet queue.
    for (int64_t tick : {10000, 20000}) {
        std::vector<Frame> frames;
        for (int frame = 0; frame < 150; frame++)
            frames.push_back({1000000 + frame * 1000000LL / 30, packets_of(frame % 30 == 0 ? 685000 : 117000)});
        ca::RtpPacer pacer;
        pacer.SetRate(60000000U, 0);
        size_t max_queued = 0;
        const std::vector<int64_t> sent = stream(pacer, frames, tick, &max_queued);
        printf("4K stream, %lld ms wakeups: largest backlog %zu packets, key frames over", (long long)tick / 1000,
               max_queued);
        size_t last = 0;
        for (size_t frame = 0; frame < frames.size(); frame++) {
            last += frames[frame].packets;
            const int64_t done = sent[last - 1] - frames[frame].at_us;
            if (frame % 30 == 0) {
                printf(" %lld", (long long)done / 1000);
                assert(done >= 75000 && done <= 110000);
            }
        }
        printf(" ms\n");
        assert(max_queued < 1024);
    }

    // A rate change applies to the next packet; slowing keeps no more than
    // the new bucket, and turning pacing off releases the rest at once.
    {
        ca::RtpPacer pacer;
        int64_t now = 0;
        pacer.SetRate(80000000U, now);
        std::vector<int64_t> sent = drain(pacer, now, 300);
        const int64_t fast = sent.back() - sent.front();
        const int64_t fast_ideal = (300LL * kPacket - ca::RtpPacer::kBurstBytes) * 8 / 80;
        pacer.SetRate(20000000U, now);
        const int64_t slow_start = now;
        sent = drain(pacer, now, 300);
        const int64_t slow = sent.back() - slow_start;
        const int64_t slow_ideal = 300LL * kPacket * 8 / 20;
        printf("rate change: 300 packets in %lld us at 80 Mbit/s, then %lld us at 20 Mbit/s\n",
               (long long)fast, (long long)slow);
        assert(fast >= fast_ideal - 1000 && fast <= fast_ideal + 2000);
        assert(slow >= slow_ideal - 1000 && slow <= slow_ideal + 2000);
        pacer.SetRate(0, now);
        for (int i = 0; i < 1000; i++) assert(pacer.Take(kPacket, now) == 0);
        // Back on, it starts with a fresh burst.
        pacer.SetRate(20000000U, now);
        assert(drain(pacer, now, 16).back() == now);
        assert(pacer.Take(kPacket, now) > 0);
    }

    // The wait is exact to the microsecond, and long gaps cannot overflow.
    {
        ca::RtpPacer pacer;
        pacer.SetRate(8000000U, 0); // one byte per microsecond
        while (pacer.Take(kPacket, 0) == 0) {}
        assert(pacer.Take(kPacket, 0) == int64_t(kPacket) - (ca::RtpPacer::kBurstBytes % kPacket));
        pacer.SetRate(1000000000U, 1);
        assert(pacer.Take(kPacket, INT64_C(1) << 60) == 0);
    }

    // [stream.*] rtp_pacing_mbps: 0 off, never below twice the bitrate.
    assert(ca::RtpPacer::StreamRate(0, 28000) == 0);
    assert(ca::RtpPacer::StreamRate(60, 28000) == 60000000U);
    assert(ca::RtpPacer::StreamRate(60, 12000) == 60000000U);
    assert(ca::RtpPacer::StreamRate(60, 40000) == 80000000U);
    assert(ca::RtpPacer::StreamRate(10, 2048) == 10000000U);

    puts("PASS RTP pacer: off, burst, spread, coarse wakeups, runtime rate change, stream rate");
    return 0;
}
