#ifndef CAMERA_APP_RTP_PACER_H
#define CAMERA_APP_RTP_PACER_H

#include <cstdint>

namespace ca {

/* Token bucket over one RTSP client's RTP packets, so a key frame leaves over
 * a few frame intervals instead of in one burst at line rate. Owned by the
 * RTSP event loop; not thread safe. */
class RtpPacer {
public:
    // Sent back to back after the client had nothing to send: about 16 full
    // RTP packets.
    static const int64_t kBurstBytes = 16 * 1500;
    // While packets wait, the bucket keeps 30 ms at the rate, or the burst if
    // that is more. The camera's kernel has no high-resolution timers: a 1 ms
    // wait wakes 10-20 ms later, and that wakeup must send everything earned
    // meanwhile for the client to get the rate.
    static const int64_t kBucketUs = 30000;

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
    // are kept, up to the new bucket; turning pacing on starts with a burst.
    void SetRate(uint64_t bits_per_second, int64_t now_us)
    {
        if (bits_per_second == rate_) return;
        Refill(now_us);
        const bool was_off = rate_ == 0;
        rate_ = bits_per_second;
        if (was_off) tokens_ = kBurstBytes * kUnitsPerByte;
        else if (tokens_ > Bucket()) tokens_ = Bucket();
    }

    uint64_t Rate() const { return rate_; }

    // 0 if a packet of this many bytes may be sent now, which spends its
    // tokens; otherwise the microseconds until it may.
    int64_t Take(uint32_t bytes, int64_t now_us)
    {
        if (rate_ == 0) return 0;
        Refill(now_us);
        idle_ = false;
        const int64_t cost = int64_t(bytes) * kUnitsPerByte;
        if (tokens_ >= cost) {
            tokens_ -= cost;
            return 0;
        }
        const uint64_t missing = uint64_t(cost - tokens_);
        return int64_t((missing + rate_ - 1) / rate_);
    }

    // The client has nothing left to send: until the next Take the bucket
    // refills only to the burst, so a frame after a gap does not leave 30 ms
    // of tokens at line rate.
    void Idle(int64_t now_us)
    {
        Refill(now_us);
        idle_ = true;
    }

private:
    // Tokens count bits x 10^6, so that rate (bit/s) x elapsed (us) adds
    // them exactly.
    static const int64_t kUnitsPerByte = 8 * 1000000;

    int64_t Bucket() const
    {
        const int64_t by_time = int64_t(rate_) * kBucketUs;
        const int64_t by_packets = kBurstBytes * kUnitsPerByte;
        return by_time > by_packets ? by_time : by_packets;
    }

    void Refill(int64_t now_us)
    {
        if (rate_ != 0 && now_us > refilled_us_) {
            const int64_t full = idle_ ? kBurstBytes * kUnitsPerByte : Bucket();
            const int64_t room = full - tokens_;
            const int64_t elapsed = now_us - refilled_us_;
            // Compare before multiplying: a long idle gap would overflow.
            if (room > 0 && elapsed >= room / int64_t(rate_) + 1) tokens_ = full;
            else if (room > 0) tokens_ += elapsed * int64_t(rate_);
        }
        refilled_us_ = now_us;
    }

    uint64_t rate_ = 0;
    int64_t tokens_ = 0;
    int64_t refilled_us_ = 0;
    bool idle_ = true;
};

}  // namespace ca

#endif
