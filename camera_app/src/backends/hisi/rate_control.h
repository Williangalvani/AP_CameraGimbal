#pragma once
/* The key-frame size cap. max_i_proportion limits a key frame to that many
 * times the bits of a P-frame, in [min_i_proportion, 100]; min_i_proportion
 * is in [1, 100]. Templates over the SDK's CBR, VBR and AVBR parameters, so
 * host tests can use look-alikes without SDK types. */

namespace ca_hisi_rc {

struct IProportion {
    unsigned max;
    unsigned min;
};

// ratio 0 keeps the encoder's own limits
inline IProportion i_proportion(unsigned ratio, IProportion defaults)
{
    if (ratio == 0U) return defaults;
    IProportion limits;
    limits.max = ratio > 100U ? 100U : ratio;
    limits.min = defaults.min < 1U ? 1U : defaults.min;
    if (limits.min > limits.max) limits.min = 1U;
    return limits;
}

template <typename Param> IProportion read(const Param &param)
{
    IProportion limits;
    limits.max = param.max_i_proportion;
    limits.min = param.min_i_proportion;
    return limits;
}

template <typename Param> void write(Param &param, IProportion limits)
{
    param.max_i_proportion = limits.max;
    param.min_i_proportion = limits.min;
}

/* The super frame strategy re-encodes a frame above its threshold at a higher
 * QP. Vendor V18 keeps P-frames at half the key-frame threshold. */
struct SuperFrame {
    unsigned i_bits;
    unsigned p_bits;
};

// i_kbits 0 turns the strategy off, which the caller maps to the encoder's own
inline SuperFrame super_frame(unsigned i_kbits)
{
    SuperFrame thresholds;
    if (i_kbits > 1000000U) i_kbits = 1000000U;
    thresholds.i_bits = i_kbits * 1000U;
    thresholds.p_bits = thresholds.i_bits / 2U;
    return thresholds;
}

}  // namespace ca_hisi_rc
