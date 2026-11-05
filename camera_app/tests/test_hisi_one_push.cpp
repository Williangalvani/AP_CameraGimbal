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

    // a level under the tolerance never counts as near
    Stepper dark(Water::green, {300U, 256U, 300U}, false);
    dark.measured({3U, 5U, 5U});
    assert(dark.gains().red == 292U && !dark.done());
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
    // no red at all: red climbs by 60 every step and never meets green
    Stepper first = run(Water::green, {0U, 120U, 120U}, {256U, 256U, 256U}, true);
    assert(!first.converged() && first.steps() == 300U && first.frames() == 300U && first.limit() == 300U);
    assert(first.gains().red == 256U + 300U * 60U && first.gains().blue == 256U);
    const Gains capped = first.result();
    assert(capped.red == 4081U && capped.green == 256U && capped.blue == 256U);

    Stepper later = run(Water::blue, {0U, 120U, 120U}, {256U, 256U, 256U}, false);
    assert(!later.converged() && later.steps() == 100U && later.gains().red == 256U + 100U * 16U);
}

static void test_convergence()
{
    const Sensor scenes[] = {{40U, 120U, 100U}, {25U, 140U, 90U}, {90U, 100U, 110U}, {60U, 60U, 200U}};
    for (bool lag : {false, true}) {
        for (Sensor scene : scenes) {
            scene.lag = lag;
            Stepper green = run(Water::green, scene, {256U, 256U, 256U}, false);
            assert(green.converged() && green.steps() < 100U);
            Sensor check = scene;
            check.lag = false;
            const Means grey = check.frame(green.gains());
            assert(near(grey.red, grey.green, 8U) && near(grey.blue, grey.green, 8U));

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
    test_convergence();
    test_centre_means();
    puts("test_hisi_one_push: ok");
    return 0;
}
