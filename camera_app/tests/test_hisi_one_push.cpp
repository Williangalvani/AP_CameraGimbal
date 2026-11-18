#include "one_push_wb.h"

#include <algorithm>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <vector>

using namespace ca_hisi_wb;

/* A grey scene under coloured water: each channel's 8-bit level at 1x gain,
 * scaled by the applied gain and clipped. lag shows the previous step's
 * gains, as a frame already queued when the gains change would. */
struct Sensor {
    uint32_t red, green, blue;
    bool lag = false;
    Gains shown{256U, 256U, 256U};
    static uint32_t level(uint32_t raw, uint32_t gain)
    {
        const uint64_t value = (uint64_t)raw * (gain & 0xffffU) / 256U;
        return value > 255U ? 255U : (uint32_t)value;
    }
    Means frame(const Gains &applied)
    {
        const Gains used = lag ? shown : applied;
        shown = applied;
        return {level(red, used.red), level(green, used.green), level(blue, used.blue)};
    }
};

static Stepper run(Water water, Sensor sensor, const Gains &start, bool first)
{
    Stepper stepper(water, start, first);
    sensor.shown = start;
    while (!stepper.done()) stepper.measured(sensor.frame(stepper.gains()));
    return stepper;
}

static void test_sliders()
{
    for (unsigned level = 0; level < 256U; level++) {
        const Levels saved = levels(gains({level, level, level}));
        assert(saved.red == (unsigned)(level * 0.95) && saved.green == level && saved.blue == level);
    }
    const Gains max = gains({255U, 255U, 255U});
    assert(max.red == 4081U && max.green == 2296U && max.blue == 2296U);
    // rounded down; red kept 5% short
    const Levels saved = levels({1000U, 263U, 700U});
    assert(saved.red == 46U && saved.green == 0U && saved.blue == 55U);
    const Gains applied = gains(saved);
    assert(applied.red == 946U && applied.green == 256U && applied.blue == 696U);
    // the ends of the sliders, where the factory firmware's byte would wrap
    const Levels low = levels({200U, 100U, 0U});
    assert(low.red == 0U && low.green == 0U && low.blue == 0U);
    const Levels high = levels({20000U, 5000U, UINT32_MAX});
    assert(high.red == 255U && high.green == 255U && high.blue == 255U);
}

static void test_green_steps()
{
    Stepper stepper(Water::green, {300U, 264U, 400U}, false);
    assert(stepper.limit() == 100U && stepper.gains().green == 264U);
    stepper.measured({100U, 140U, 140U});   // 40 short of green
    assert(stepper.gains().red == 360U && stepper.steps() == 1U);
    stepper.measured({109U, 140U, 140U});   // 31 short
    assert(stepper.gains().red == 420U);
    stepper.measured({110U, 140U, 140U});   // 30 short
    assert(stepper.gains().red == 428U);
    stepper.measured({149U, 140U, 140U});   // 9 over
    assert(stepper.gains().red == 420U);
    // red within 8 of green, so blue steps
    stepper.measured({148U, 140U, 100U});
    assert(stepper.gains().red == 420U && stepper.gains().blue == 460U);
    stepper.measured({132U, 140U, 120U});
    assert(stepper.gains().blue == 468U);
    stepper.measured({132U, 140U, 150U});
    assert(stepper.gains().blue == 460U && !stepper.done());
    // red is checked first on every frame
    stepper.measured({120U, 140U, 140U});
    assert(stepper.gains().red == 428U && stepper.gains().blue == 460U && stepper.steps() == 8U);
    stepper.measured({132U, 140U, 148U});
    assert(stepper.converged() && stepper.done() && stepper.steps() == 8U && stepper.frames() == 9U);
    assert(stepper.gains().red == 428U && stepper.gains().green == 264U && stepper.gains().blue == 460U);
    const Gains result = stepper.result();
    assert(result.red == 406U && result.green == 264U && result.blue == 456U);

    // a level under the tolerance is near an equal or close one (the factory
    // firmware's unsigned compare would step red down here)
    Stepper dim(Water::green, {300U, 256U, 300U}, false);
    dim.measured({3U, 5U, 5U});
    assert(dim.converged() && dim.steps() == 0U && dim.gains().red == 300U);
    Stepper low(Water::green, {300U, 256U, 300U}, false);
    low.measured({2U, 20U, 20U});
    assert(low.gains().red == 308U && !low.done());
    low.measured({30U, 20U, 20U});
    assert(low.gains().red == 300U);
}

static void test_blue_steps()
{
    Stepper stepper(Water::blue, {500U, 300U, 400U}, false);
    assert(stepper.gains().red == 500U && stepper.gains().green == 256U && stepper.gains().blue == 256U);
    stepper.measured({100U, 0U, 140U});
    assert(stepper.gains().red == 516U);
    stepper.measured({109U, 0U, 140U});
    assert(stepper.gains().red == 532U);
    stepper.measured({110U, 0U, 140U});
    assert(stepper.gains().red == 536U);
    stepper.measured({144U, 0U, 140U});
    assert(stepper.gains().red == 532U);
    stepper.measured({143U, 999U, 140U});   // green is ignored
    assert(stepper.converged() && stepper.steps() == 4U);
    // 532 * 1.2 = 638, saved as slider 23
    const Gains result = stepper.result();
    assert(stepper.result_levels().red == 23U);
    assert(result.red == 601U && result.green == 256U && result.blue == 256U);
}

static void test_limits()
{
    // red at 1/256 of green's level climbs by 8 a step, too slowly to get
    // near green in 300 steps
    Stepper first = run(Water::green, {1U, 25U, 25U}, {256U, 256U, 256U}, true);
    assert(first.stop() == Stop::limit && first.done() && !first.converged());
    assert(first.steps() == 300U && first.frames() == 300U && first.limit() == 300U);
    assert(first.gains().red == 256U + 300U * 8U && first.gains().blue == 256U);
    const Gains capped = first.result();
    assert(capped.red == 2536U && capped.green == 256U && capped.blue == 256U);

    Stepper later = run(Water::blue, {1U, 120U, 25U}, {256U, 256U, 256U}, false);
    assert(later.stop() == Stop::limit && later.steps() == 100U && later.gains().red == 256U + 100U * 4U);
}

static void test_dark()
{
    for (const Means dark : {Means{0U, 0U, 0U}, Means{3U, 3U, 3U}, Means{3U, 0U, 1U}}) {
        for (Water water : {Water::green, Water::blue}) {
            for (bool first : {true, false}) {
                const Gains start{700U, 300U, 500U};
                Stepper stepper(water, start, first);
                const Gains before = stepper.gains();
                stepper.measured(dark);
                assert(stepper.too_dark() && stepper.done() && !stepper.converged() && !stepper.pinned());
                assert(stepper.steps() == 0U && stepper.frames() == 1U);
                assert(stepper.gains().red == before.red && stepper.gains().green == before.green &&
                       stepper.gains().blue == before.blue);
                // saved as they were, without blue water's fifth
                const Levels saved = stepper.result_levels();
                assert(saved.red == (unsigned)((700U - 256U) / 15U * 0.95));
            }
        }
    }
    // a black scene stops on the first frame and never wraps a gain
    Stepper black = run(Water::green, {0U, 0U, 0U}, {256U, 256U, 256U}, true);
    assert(black.too_dark() && black.frames() == 1U && black.gains().red == 256U);
    // one channel at the floor is not too dark
    Stepper one(Water::green, {256U, 256U, 256U}, false);
    one.measured({0U, 4U, 0U});
    assert(one.converged());
}

static void test_pinned()
{
    // red at 1/20 of green reaches 159 at most, so red climbs by 60 to its
    // maximum and then has nowhere to go
    Stepper red = run(Water::green, {10U, 200U, 200U}, {256U, 256U, 256U}, true);
    assert(red.pinned() && red.done() && !red.converged());
    assert(red.gains().red == max_red && red.gains().blue == 256U);
    assert(red.steps() == 64U && red.frames() == 65U);
    // saved 5% short
    assert(red.result().red == 3886U);

    // red above green even at 1x
    Stepper high = run(Water::green, {200U, 100U, 100U}, {264U, 256U, 256U}, false);
    assert(high.pinned() && high.gains().red == min_gain && high.steps() == 1U);

    // blue clipped high: red stepped to its maximum in blue water
    Stepper blue = run(Water::blue, {5U, 0U, 250U}, {256U, 256U, 256U}, true);
    assert(blue.pinned() && blue.gains().red == max_red && blue.steps() < 300U);
    assert(blue.result().red == 4081U);

    // blue in green water, after red has matched
    Stepper gb = run(Water::green, {100U, 100U, 2U}, {256U, 256U, 256U}, true);
    assert(gb.pinned() && gb.gains().blue == max_green_blue && gb.gains().red == 256U);
    assert(gb.steps() < 300U);

    // a start already past the maximum, stepping further out
    Stepper past(Water::green, {4090U, 256U, 256U}, false);
    past.measured({10U, 100U, 100U});
    assert(past.pinned() && past.gains().red == 4090U && past.steps() == 0U);
}

/* xorshift, so the scenes are the same on every run */
static uint32_t next(uint32_t &state)
{
    state ^= state << 13;
    state ^= state >> 17;
    state ^= state << 5;
    return state;
}

static void test_any_scene()
{
    uint32_t state = 0x1234567U;
    const auto in = [&](uint32_t low, uint32_t high) { return low + next(state) % (high - low + 1U); };
    unsigned stops[5] = {};
    for (unsigned scene = 0; scene < 20000U; scene++) {
        // every eighth scene dark
        const uint32_t top = scene % 8U == 0U ? 3U : 255U;
        Sensor sensor{in(0U, top), in(0U, top), in(0U, top)};
        sensor.lag = (scene & 1U) != 0U;
        const Water water = (scene & 2U) != 0U ? Water::blue : Water::green;
        const bool first = (scene & 4U) != 0U;
        const Gains start{in(min_gain, max_red), in(min_gain, max_green_blue), in(min_gain, max_green_blue)};
        Stepper stepper(water, start, first);
        sensor.shown = start;
        unsigned frames = 0;
        while (!stepper.done()) {
            const Gains &applied = stepper.gains();
            assert(applied.red >= min_gain && applied.red <= max_red);
            assert(applied.green >= min_gain && applied.green <= max_green_blue);
            assert(applied.blue >= min_gain && applied.blue <= max_green_blue);
            stepper.measured(sensor.frame(applied));
            assert(++frames <= stepper.limit());
        }
        assert(stepper.steps() <= stepper.limit() && stepper.frames() == frames);
        assert(stepper.stop() != Stop::running);
        stops[(int)stepper.stop()]++;
        const Gains result = stepper.result();
        assert(result.red >= min_gain && result.red <= max_red);
        assert(result.green >= min_gain && result.green <= max_green_blue);
        assert(result.blue >= min_gain && result.blue <= max_green_blue);
    }
    // every way of stopping is reached
    assert(stops[(int)Stop::converged] && stops[(int)Stop::limit] && stops[(int)Stop::pinned] &&
           stops[(int)Stop::too_dark]);
}

static void test_convergence()
{
    const Sensor scenes[] = {{40U, 120U, 100U}, {25U, 140U, 90U}, {90U, 100U, 110U}, {60U, 60U, 200U}};
    for (bool lag : {false, true}) {
        for (Sensor scene : scenes) {
            scene.lag = lag;
            Stepper green = run(Water::green, scene, {256U, 256U, 256U}, false);
            Sensor check = scene;
            check.lag = false;
            const Means grey = check.frame(green.gains());
            // blue brighter than green at 1x stops at 1x, the least the
            // sliders save, with red matched
            if (scene.blue > scene.green + 8U) {
                assert(green.pinned() && green.gains().blue == 256U && green.steps() < 100U);
                assert(near(grey.red, grey.green, 8U));
            } else {
                assert(green.converged() && green.steps() < 100U);
                assert(near(grey.red, grey.green, 8U) && near(grey.blue, grey.green, 8U));
            }

            Stepper blue = run(Water::blue, scene, {256U, 256U, 256U}, false);
            assert(blue.converged() && blue.steps() < 100U);
            const Means warm = check.frame(blue.gains());
            assert(near(warm.red, warm.blue, 3U) && blue.gains().green == 256U && blue.gains().blue == 256U);
        }
    }
    // starting from the previous result takes no steps
    Sensor scene{40U, 120U, 100U};
    Stepper again = run(Water::green, scene, run(Water::green, scene, {256U, 256U, 256U}, true).gains(), false);
    assert(again.converged() && again.steps() == 0U);
}

static void test_centre_means()
{
    const unsigned width = 16U, height = 8U, stride = 20U;
    std::vector<uint8_t> luma(stride * height, 16U), chroma(stride * height / 2U, 128U);
    const auto mean = [&](bool vu) {
        return centre_means(luma.data(), stride, chroma.data(), stride, width, height, vu);
    };
    // white in the centre half, columns 4-11 and rows 2-5, black around it
    for (unsigned y = 2; y < 6; y++) memset(&luma[y * stride + 4], 235, 8);
    Means m = mean(true);
    assert(m.red == 255U && m.green == 255U && m.blue == 255U);
    luma[1 * stride + 4] = luma[2 * stride + 3] = luma[2 * stride + 12] = luma[6 * stride + 11] = 235;
    m = mean(true);
    assert(m.red == 255U && m.green == 255U && m.blue == 255U);
    luma[5 * stride + 11] = 16;   // the last pixel inside
    m = mean(true);
    assert(m.red == 247U && m.green == 247U && m.blue == 247U);

    // BT.709 red, Y 63 Cb 102 Cr 240, in the chroma rows 1-2 of the centre
    for (unsigned y = 2; y < 6; y++) memset(&luma[y * stride + 4], 63, 8);
    for (unsigned y = 1; y < 3; y++) {
        for (unsigned x = 4; x < 12; x += 2) {
            chroma[y * stride + x] = 240;
            chroma[y * stride + x + 1] = 102;
        }
    }
    m = mean(true);
    assert(m.red == 255U && m.green == 1U && m.blue == 0U);
    // the same bytes read as U,V
    m = mean(false);
    assert(m.red == 8U && m.blue == 255U);

    // mid grey
    std::fill(luma.begin(), luma.end(), 126);
    std::fill(chroma.begin(), chroma.end(), 128);
    m = mean(true);
    assert(m.red == 128U && m.green == 128U && m.blue == 128U);
    assert(centre_means(luma.data(), stride, chroma.data(), stride, 1U, 1U, true).red == 0U);
}

int main()
{
    test_sliders();
    test_green_steps();
    test_blue_steps();
    test_limits();
    test_dark();
    test_pinned();
    test_any_scene();
    test_convergence();
    test_centre_means();
    puts("test_hisi_one_push: ok");
    return 0;
}
