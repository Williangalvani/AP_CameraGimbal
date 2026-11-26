#ifndef CAMERA_APP_RTP_PACER_H
#define CAMERA_APP_RTP_PACER_H

#include <cstdint>

namespace ca {

/* Token bucket over one RTSP client's RTP packets, so a key frame leaves over
 * a few frame intervals instead of in one burst at line rate. Owned by the
 * RTSP event loop; not thread safe. */
class RtpPacer {
public:
    // The burst sent back to back: about 16 full RTP packets, or 2 ms at the
    // rate if that is more.
    static const int64_t kBurstBytes = 16 * 1500;
    static const int64_t kBurstUs = 2000;

    // [stream.*] rtp_pacing_mbps over the stream's bitrate: 0 is off, and a
    // stream is never paced below twice its bitrate.
    static uint64_t StreamRate(unsigned mbps, unsigned bitrate_kbps)
    {
        if (mbps == 0) return 0;
        const uint64_t configured = uint64_t(mbps) * 1000000U;
        const uint64_t floor = uint64_t(bitrate_kbps) * 2000U;
        return configured > floor ? configured : floor;
    }

    // Bits per second; 0 sends without delay. Tokens earned at the old rate
    // are kept, up to the new burst; turning pacing on starts with a burst.
    void SetRate(uint64_t bits_per_second, int64_t now_us)
    {
        if (bits_per_second == rate_) return;
        Refill(now_us);
        const bool was_off = rate_ == 0;
        rate_ = bits_per_second;
        if (was_off || tokens_ > Burst()) tokens_ = Burst();
    }

    uint64_t Rate() const { return rate_; }

    // 0 if a packet of this many bytes may be sent now, which spends its
    // tokens; otherwise the microseconds until it may.
    int64_t Take(uint32_t bytes, int64_t now_us)
    {
        if (rate_ == 0) return 0;
        Refill(now_us);
        const int64_t cost = int64_t(bytes) * kUnitsPerByte;
        if (tokens_ >= cost) {
            tokens_ -= cost;
            return 0;
        }
        const uint64_t missing = uint64_t(cost - tokens_);
        return int64_t((missing + rate_ - 1) / rate_);
    }

private:
    // Tokens count bits x 10^6, so that rate (bit/s) x elapsed (us) adds
    // them exactly.
    static const int64_t kUnitsPerByte = 8 * 1000000;

    int64_t Burst() const
    {
        const int64_t by_time = int64_t(rate_) * kBurstUs;
        const int64_t by_packets = kBurstBytes * kUnitsPerByte;
        return by_time > by_packets ? by_time : by_packets;
    }

    void Refill(int64_t now_us)
    {
        if (rate_ != 0 && now_us > refilled_us_) {
            const int64_t room = Burst() - tokens_;
            const int64_t elapsed = now_us - refilled_us_;
            // Compare before multiplying: a long idle gap would overflow.
            if (room <= 0 || elapsed >= room / int64_t(rate_) + 1) tokens_ = Burst();
            else tokens_ += elapsed * int64_t(rate_);
        }
        refilled_us_ = now_us;
    }

    uint64_t rate_ = 0;
    int64_t tokens_ = 0;
    int64_t refilled_us_ = 0;
};

}  // namespace ca

#endif
