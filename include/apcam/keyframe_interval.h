#ifndef APCAM_KEYFRAME_INTERVAL_H
#define APCAM_KEYFRAME_INTERVAL_H

/* [stream.main] / [stream.sub] keyframe_interval_ms: the time between
 * keyframes, in milliseconds so that it means the same at any frame rate.
 * Shared by camera-app and the web, so no SDK types. */
#define APCAM_KEYINT_MIN_MS 30
#define APCAM_KEYINT_MAX_MS 10000
/* gop = frame rate, one keyframe a second, the encoder's fixed value before
 * the setting existed */
#define APCAM_DEFAULT_KEYINT_MS 1000
/* the HiSilicon rate control's gop range is [1, 65536] (ot_common_rc.h) */
#define APCAM_GOP_MAX_FRAMES 65536U

/* Frames per GOP at fps frames a second: the interval rounded to whole frames
 * (halves up), at least 1, so an interval shorter than a frame makes every
 * frame a keyframe. */
static inline unsigned apcam_gop_frames(unsigned interval_ms, unsigned fps)
{
    const unsigned long long frames = ((unsigned long long)interval_ms * fps + 500U) / 1000U;
    if (frames < 1U) return 1U;
    return frames > APCAM_GOP_MAX_FRAMES ? APCAM_GOP_MAX_FRAMES : (unsigned)frames;
}

/* The interval a GOP of frames lasts at fps, rounded to whole milliseconds and
 * clamped to the setting's bounds. Within them apcam_gop_frames() gives the
 * frames back. fps 0 counts as 1. */
static inline unsigned apcam_keyint_ms(unsigned frames, unsigned fps)
{
    if (fps == 0U) fps = 1U;
    const unsigned long long ms = ((unsigned long long)frames * 1000U + fps / 2U) / fps;
    if (ms < (unsigned long long)APCAM_KEYINT_MIN_MS) return APCAM_KEYINT_MIN_MS;
    return ms > (unsigned long long)APCAM_KEYINT_MAX_MS ? APCAM_KEYINT_MAX_MS : (unsigned)ms;
}

#endif
