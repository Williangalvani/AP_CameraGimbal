#pragma once
/* One-push (underwater) white balance as the camera's factory firmware runs
 * it: manual gains are stepped one frame at a time until the centre of the
 * picture meets the water's target, then saved through the 4K Cam Manager's
 * 0–255 slider scale. No SDK types, so host tests can drive it against a
 * simulated sensor. Gains are in 1/256 (256 = 1x). */
#include <cstddef>
#include <cstdint>

namespace ca_hisi_wb {

struct Gains { uint32_t red, green, blue; };
/* mean 8-bit R, G and B of a frame's centre */
struct Means { uint32_t red, green, blue; };
/* the 4K Cam Manager's awb_red, awb_green and awb_blue */
struct Levels { unsigned red, green, blue; };

enum class Water { green, blue };

/* the wait after setting gains, before the frame is read */
constexpr unsigned settle_ms = 20;
/* The first one-push since start-up begins from the auto white balance's
 * gains, given this long to settle, and may take more steps. */
constexpr unsigned first_auto_ms = 500;
constexpr unsigned first_step_limit = 300;
constexpr unsigned step_limit = 100;

inline unsigned slider(uint64_t value) { return value > 255U ? 255U : (unsigned)value; }

/* Manual white balance from the sliders, as the 4K Cam Manager sets it. */
inline Gains gains(const Levels &levels)
{
    return {levels.red * 15U + 256U, (levels.green + 32U) * 8U, (levels.blue + 32U) * 8U};
}

/* The sliders a one-push saves, rounding down and keeping red 5% short. The
 * factory firmware stores them in a byte unchecked, so a gain outside the
 * sliders' range wraps there; here it stops at the end of the range. */
inline Levels levels(const Gains &gains)
{
    const uint32_t red = gains.red > 256U ? (gains.red - 256U) / 15U : 0U;
    return {slider((uint64_t)((double)red * 0.95)),
            slider(gains.green > 256U ? (gains.green - 256U) >> 3 : 0U),
            slider(gains.blue > 256U ? (gains.blue - 256U) >> 3 : 0U)};
}

/* The gains a step can reach: 1x up to the sliders' maximum, which the ISP
 * also accepts (it takes 0 to 0xFFF). */
constexpr uint32_t min_gain = 256U;
constexpr uint32_t max_red = 255U * 15U + 256U;
constexpr uint32_t max_green_blue = (255U + 32U) * 8U;
/* below this on every channel's centre mean there is nothing to balance */
constexpr uint32_t dark_level = 4U;

/* Signed, unlike the factory firmware, which compares unsigned: there a level
 * below the tolerance never counts as near, even when equal to the target. */
inline bool near(uint32_t level, uint32_t target, uint32_t tolerance)
{
    const int64_t difference = (int64_t)level - (int64_t)target;
    return (difference < 0 ? -difference : difference) <= (int64_t)tolerance;
}

/* for a level not near the target */
inline int32_t step(uint32_t level, uint32_t target, uint32_t fine, uint32_t coarse)
{
    if (level > target) return -(int32_t)fine;
    return target - level > 30U ? (int32_t)coarse : (int32_t)fine;
}

enum class Stop { running, converged, limit, pinned, too_dark };

/* Green water makes the centre grey: green stays at the auto gain, red is
 * stepped to the green level, then blue. Blue water keeps green and blue at
 * 1x, steps red to the blue level and then raises it by a fifth.
 *
 * Unlike the factory firmware, which wraps a gain stepped below 0 (and then
 * sets gains the ISP refuses until the step limit), each step stops at
 * min_gain and the channel's maximum. When a step would move a gain already
 * there further out, no later step can help, so it stops pinned(). It also
 * stops too_dark() on a frame whose centre is below dark_level on every
 * channel, keeping the gains it has. */
class Stepper {
public:
    Stepper(Water water, const Gains &current, bool first)
        : _water(water), _limit(first ? first_step_limit : step_limit),
          _gains(water == Water::blue ? Gains{current.red, 256U, 256U} : current) {}

    /* green is set on both Gr and Gb */
    const Gains &gains() const { return _gains; }
    unsigned frames() const { return _frames; }
    unsigned steps() const { return _steps; }
    unsigned limit() const { return _limit; }
    Stop stop() const { return _stop; }
    bool converged() const { return _stop == Stop::converged; }
    bool pinned() const { return _stop == Stop::pinned; }
    bool too_dark() const { return _stop == Stop::too_dark; }
    bool done() const { return _stop != Stop::running; }

    /* a frame taken with gains() applied */
    void measured(const Means &means)
    {
        _frames++;
        if (means.red < dark_level && means.green < dark_level && means.blue < dark_level) {
            _stop = Stop::too_dark;
            return;
        }
        bool moved;
        if (_water == Water::green) {
            if (!near(means.red, means.green, 8U)) {
                moved = move(_gains.red, max_red, step(means.red, means.green, 8U, 60U));
            } else if (!near(means.blue, means.green, 8U)) {
                moved = move(_gains.blue, max_green_blue, step(means.blue, means.green, 8U, 60U));
            } else {
                _stop = Stop::converged;
                return;
            }
        } else if (!near(means.red, means.blue, 3U)) {
            moved = move(_gains.red, max_red, step(means.red, means.blue, 4U, 16U));
        } else {
            _stop = Stop::converged;
            return;
        }
        if (!moved) {
            _stop = Stop::pinned;
            return;
        }
        if (++_steps >= _limit) _stop = Stop::limit;
    }

    /* What one-push applies and saves. At the step limit that is the last
     * step, which no frame has checked. Too dark, red was never matched, so
     * blue water's fifth is not added. */
    Levels result_levels() const
    {
        Gains result = _gains;
        if (_water == Water::blue && _stop != Stop::too_dark) {
            const double red = result.red * 1.2;
            result.red = red >= 4294967295.0 ? UINT32_MAX : (uint32_t)red;
        }
        return levels(result);
    }
    Gains result() const { return ca_hisi_wb::gains(result_levels()); }

private:
    /* false, leaving the gain, when it is already at the end of its range
     * in the step's direction */
    static bool move(uint32_t &gain, uint32_t max, int32_t delta)
    {
        if (delta < 0 ? gain <= min_gain : gain >= max) return false;
        const int64_t moved = (int64_t)gain + delta;
        gain = moved < (int64_t)min_gain ? min_gain : moved > (int64_t)max ? max : (uint32_t)moved;
        return true;
    }

    Water _water;
    unsigned _limit;
    Gains _gains;
    unsigned _frames = 0, _steps = 0;
    Stop _stop = Stop::running;
};

inline uint32_t rgb8(int value)
{
    if (value <= 0) return 0U;
    value = (value + 512) >> 10;
    return value > 255 ? 255U : (uint32_t)value;
}

/* Mean RGB of the centre (half the width by half the height) of a 4:2:0
 * semi-planar frame. Each pixel is converted from limited-range BT.709 to
 * full-range RGB, as the factory firmware's IVE conversion does, and
 * clipped before averaging; the means round down. vu_order selects NV21's
 * V,U chroma pairs, otherwise U,V. */
inline Means centre_means(const uint8_t *luma, size_t luma_stride, const uint8_t *chroma,
                          size_t chroma_stride, unsigned width, unsigned height, bool vu_order)
{
    const unsigned left = width / 4U, top = height / 4U;
    const unsigned right = left + width / 2U, bottom = top + height / 2U;
    const uint64_t count = (uint64_t)(width / 2U) * (height / 2U);
    uint64_t red = 0, green = 0, blue = 0;
    if (count == 0U) return {0U, 0U, 0U};
    for (unsigned y = top; y < bottom; y++) {
        const uint8_t *row = luma + (size_t)y * luma_stride;
        const uint8_t *pairs = chroma + (size_t)(y / 2U) * chroma_stride;
        for (unsigned x = left; x < right; x++) {
            const uint8_t *pair = pairs + (x & ~1U);
            const int l = 1192 * ((int)row[x] - 16);
            const int u = (int)pair[vu_order ? 1 : 0] - 128;
            const int v = (int)pair[vu_order ? 0 : 1] - 128;
            red += rgb8(l + 1836 * v);
            green += rgb8(l - 218 * u - 546 * v);
            blue += rgb8(l + 2163 * u);
        }
    }
    return {(uint32_t)(red / count), (uint32_t)(green / count), (uint32_t)(blue / count)};
}

}  // namespace ca_hisi_wb
