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
        now += 1000000;
        assert(drain(pacer, now, 16).back() == now);
        assert(pacer.Take(kPacket, now) > 0);
    }

    // The burst is 2 ms at the rate where that exceeds 16 packets.
    {
        ca::RtpPacer pacer;
        pacer.SetRate(400000000U, 0);
        size_t burst = 0;
        while (pacer.Take(kPacket, 0) == 0) burst++;
        assert(burst == 400000000U / 8 * 2 / 1000 / kPacket);
    }

    // A rate change applies to the next packet; slowing keeps no more than
    // the new burst, and turning pacing off releases the rest at once.
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

    puts("PASS RTP pacer: off, burst, spread, runtime rate change, stream rate");
    return 0;
}
