#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#define _FILE_OFFSET_BITS 64
#include "camera_app/mp4.h"
#include "camera_app/video_metadata.h"

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <glob.h>
#include <limits.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/vfs.h>
#include <time.h>
#include <unistd.h>
#include <string>
#include <vector>

/* H.265 recording: an hvc1 sample entry whose hvcC carries the VPS, SPS and
 * PPS and the SPS's profile, tier and level; samples with only VCL and SEI;
 * the telemetry as an HEVC prefix SEI; continuations that keep the
 * parameter sets; drops that resume on any IRAP; and a full card. The
 * access units are synthetic: real parameter sets around slice data no
 * decoder needs to read. Given an Annex-B H.265 file and an output path, it
 * muxes that file instead, for a decoder to check. */

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t changed = PTHREAD_COND_INITIALIZER;
static bool hold_writes, fat, card_full, card_filled;
static unsigned writes_entered;

extern "C" ssize_t __real_pwrite64(int fd, const void *data, size_t size, off64_t offset);
extern "C" ssize_t __wrap_pwrite64(int fd, const void *data, size_t size, off64_t offset)
{
    pthread_mutex_lock(&lock);
    writes_entered++;
    pthread_cond_broadcast(&changed);
    while (hold_writes) pthread_cond_wait(&changed, &lock);
    int fail = 0;
    if (card_full) {
        struct stat st;
        assert(fstat(fd, &st) == 0);
        if ((uint64_t)offset + size > (uint64_t)st.st_size) {
            if (card_filled || size < 2) fail = ENOSPC;
            else size /= 2;
            card_filled = true;
        }
    }
    pthread_mutex_unlock(&lock);
    if (fail) { errno = fail; return -1; }
    return __real_pwrite64(fd, data, size, offset);
}

extern "C" int __real_fallocate64(int fd, int mode, off64_t offset, off64_t length);
extern "C" int __wrap_fallocate64(int fd, int mode, off64_t offset, off64_t length)
{
    pthread_mutex_lock(&lock);
    bool full = card_full;
    pthread_mutex_unlock(&lock);
    if (full) { errno = ENOSPC; return -1; }
    return __real_fallocate64(fd, mode, offset, length);
}

extern "C" int __real_fstatfs64(int fd, struct statfs64 *fs);
extern "C" int __wrap_fstatfs64(int fd, struct statfs64 *fs)
{
    int result = __real_fstatfs64(fd, fs);
    if (result == 0 && fat) fs->f_type = 0x4d44;
    return result;
}

/* x265's for 320x240 Main 4:2:0 8-bit, level 2. The SPS's profile_tier_level
 * holds emulation prevention bytes, which the hvcC must not. */
static const uint8_t VPS[] = {
    0x40, 0x01, 0x0c, 0x01, 0xff, 0xff, 0x01, 0x60, 0x00, 0x00, 0x03, 0x00, 0x90, 0x00,
    0x00, 0x03, 0x00, 0x00, 0x03, 0x00, 0x3c, 0x95, 0x98, 0x09,
};
static const uint8_t SPS[] = {
    0x42, 0x01, 0x01, 0x01, 0x60, 0x00, 0x00, 0x03, 0x00, 0x90, 0x00, 0x00, 0x03, 0x00,
    0x00, 0x03, 0x00, 0x3c, 0xa0, 0x0a, 0x08, 0x0f, 0x16, 0x59, 0x59, 0xa4, 0x93, 0x2b,
    0xc0, 0x5a, 0x02, 0x00, 0x00, 0x03, 0x00, 0x02, 0x00, 0x00, 0x03, 0x00, 0x14, 0x10,
};
static const uint8_t PPS[] = {0x44, 0x01, 0xc1, 0x72, 0xb4, 0x62, 0x40};
/* pps_pic_parameter_set_id 1; only the muxer reads it */
static const uint8_t PPS1[] = {0x44, 0x01, 0x58, 0x72, 0xb4, 0x62, 0x40};
static const uint8_t PTL[12] = {0x01, 0x60, 0, 0, 0, 0x90, 0, 0, 0, 0, 0, 0x3c};

enum {
    TRAIL_R = 1, BLA_W_LP = 16, BLA_W_RADL = 17, BLA_N_LP = 18, IDR_W_RADL = 19,
    IDR_N_LP = 20, CRA = 21, VPS_NUT = 32, SPS_NUT = 33, PPS_NUT = 34, AUD = 35,
    PREFIX_SEI = 39, SUFFIX_SEI = 40,
};
static const long LAT_BASE = -353632610;
static char directory[] = "/tmp/mp4-hevc-test-XXXXXX";

typedef std::vector<uint8_t> bytes;

static void put_nal(bytes &au, const uint8_t *nal, size_t length)
{
    static const uint8_t start[] = {0, 0, 0, 1};
    au.insert(au.end(), start, start + 4);
    au.insert(au.end(), nal, nal + length);
}

static void put_parameter_sets(bytes &au)
{
    put_nal(au, VPS, sizeof(VPS));
    put_nal(au, SPS, sizeof(SPS));
    put_nal(au, PPS, sizeof(PPS));
}

/* Slice data is never zero, so it holds no start code. */
static void put_slice(bytes &au, unsigned type, unsigned frame, size_t length, bool first)
{
    bytes nal = {(uint8_t)(type << 1), 1, (uint8_t)(first ? 0x80 : 0x40)};
    for (size_t i = 0; i < length; i++) nal.push_back((uint8_t)(((frame + i) % 251) + 1));
    put_nal(au, nal.data(), nal.size());
}

/* An encoder's access unit: AUD, the parameter sets before an IRAP when
 * asked, an encoder SEI, two slices and a suffix SEI. irap 0 is TRAIL_R. */
static bytes access_unit(unsigned frame, unsigned irap, bool headers, size_t slice_bytes)
{
    static const uint8_t aud[] = {AUD << 1, 1, 0x50};
    static const uint8_t prefix[] = {PREFIX_SEI << 1, 1, 0x81, 0x01, 0x11, 0x80};
    static const uint8_t suffix[] = {SUFFIX_SEI << 1, 1, 0x84, 0x01, 0x22, 0x80};
    bytes au;
    put_nal(au, aud, sizeof(aud));
    if (headers) put_parameter_sets(au);
    put_nal(au, prefix, sizeof(prefix));
    const unsigned type = irap ? irap : (unsigned)TRAIL_R;
    put_slice(au, type, frame, slice_bytes / 2, true);
    put_slice(au, type, frame, slice_bytes - slice_bytes / 2, false);
    put_nal(au, suffix, sizeof(suffix));
    return au;
}

static int write_au(struct ca_mp4 *writer, unsigned frame, const bytes &au)
{
    ca_metadata_set_position((int32_t)(LAT_BASE + frame), 1491652300, 620, 30, 0);
    return ca_mp4_write_video(writer, au.data(), au.size(), frame * 9000U, false, 54.7f);
}

static uint64_t now_us(void)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (uint64_t)now.tv_sec * 1000000U + (uint64_t)now.tv_nsec / 1000U;
}

static void drain(struct ca_mp4 *writer)
{
    struct ca_mp4_stats stats;
    uint64_t deadline = now_us() + 10000000U;
    for (;;) {
        ca_mp4_get_stats(writer, &stats);
        if (stats.buffer_bytes == 0) return;
        assert(now_us() < deadline);
        usleep(1000);
    }
}

static bytes read_file(const char *path)
{
    struct stat st;
    assert(stat(path, &st) == 0);
    bytes data((size_t)st.st_size);
    int fd = open(path, O_RDONLY);
    assert(fd >= 0 && read(fd, data.data(), data.size()) == (ssize_t)data.size());
    close(fd);
    return data;
}

static uint32_t get32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}
static unsigned get16(const uint8_t *p) { return ((unsigned)p[0] << 8) | p[1]; }

/* The first child box of the given type within [at, end), or 0. */
static size_t child(const bytes &data, size_t at, size_t end, const char *type)
{
    while (at + 8 <= end) {
        uint32_t size = get32(&data[at]);
        assert(size >= 8 && size <= end - at);
        if (!memcmp(&data[at + 4], type, 4)) return at;
        at += size;
    }
    return 0;
}

static size_t box_end(const bytes &data, size_t at) { return at + get32(&data[at]); }

struct hvcc {
    unsigned width, height;
    uint8_t ptl[12];
    uint8_t chroma, luma_depth, chroma_depth, layers;
    std::vector<std::pair<unsigned, std::vector<bytes>>> arrays;
};

struct layout {
    bool tiled, moov;
    unsigned sidx, sidx_refs, moofs;
    uint32_t sidx_sap;
    uint32_t timescale, duration; /* from the mvhd */
    hvcc config;
};

static layout parse_file(const char *path)
{
    bytes data = read_file(path);
    layout file = {};
    size_t at = 0;
    assert(data.size() >= 8 && !memcmp(&data[4], "ftyp", 4));
    while (at < data.size()) {
        if (data.size() - at < 8) return file;
        uint32_t size = get32(&data[at]);
        if (size < 8 || size > data.size() - at) return file;
        if (!memcmp(&data[at + 4], "moov", 4)) {
            file.moov = true;
            size_t end = at + size, mvhd = child(data, at + 8, end, "mvhd");
            assert(mvhd && data[mvhd + 8] == 0);
            file.timescale = get32(&data[mvhd + 20]);
            file.duration = get32(&data[mvhd + 24]);
            size_t trak = child(data, at + 8, end, "trak");
            size_t mdia = child(data, trak + 8, box_end(data, trak), "mdia");
            size_t minf = child(data, mdia + 8, box_end(data, mdia), "minf");
            size_t stbl = child(data, minf + 8, box_end(data, minf), "stbl");
            size_t stsd = child(data, stbl + 8, box_end(data, stbl), "stsd");
            assert(trak && mdia && minf && stbl && stsd && get32(&data[stsd + 12]) == 1);
            size_t entry = stsd + 16;
            assert(!memcmp(&data[entry + 4], "hvc1", 4));
            hvcc &c = file.config;
            c.width = get16(&data[entry + 32]);
            c.height = get16(&data[entry + 34]);
            size_t box = child(data, entry + 86, box_end(data, entry), "hvcC");
            assert(box);
            const uint8_t *p = &data[box + 8];
            assert(p[0] == 1);
            memcpy(c.ptl, p + 1, sizeof(c.ptl));
            c.chroma = p[16];
            c.luma_depth = p[17];
            c.chroma_depth = p[18];
            c.layers = p[21];
            size_t q = 23, limit = get32(&data[box]) - 8;
            for (unsigned i = 0; i < p[22]; i++) {
                std::pair<unsigned, std::vector<bytes>> array;
                array.first = p[q];
                unsigned count = get16(p + q + 1);
                q += 3;
                for (unsigned j = 0; j < count; j++) {
                    unsigned length = get16(p + q);
                    assert(q + 2 + length <= limit);
                    array.second.push_back(bytes(p + q + 2, p + q + 2 + length));
                    q += 2 + length;
                }
                c.arrays.push_back(array);
            }
            assert(q == limit);
        }
        if (!memcmp(&data[at + 4], "sidx", 4)) {
            file.sidx++;
            file.sidx_refs = get16(&data[at + 38]);
            file.sidx_sap = get32(&data[at + 40 + 8]);
        }
        if (!memcmp(&data[at + 4], "moof", 4)) file.moofs++;
        at += size;
    }
    file.tiled = true;
    return file;
}

/* The decoder configuration must be the fixture's: each parameter set once,
 * and the SPS's profile_tier_level without its emulation prevention. With
 * second_pps, the PPS array also holds PPS1. */
static void check_config(const hvcc &c, bool second_pps = false)
{
    assert(c.width == 320 && c.height == 240);
    assert(!memcmp(c.ptl, PTL, sizeof(PTL)));
    assert(c.chroma == 0xfd && c.luma_depth == 0xf8 && c.chroma_depth == 0xf8);
    /* one temporal layer, temporal ID nested, four-byte lengths */
    assert(c.layers == 0x0f);
    assert(c.arrays.size() == 3);
    const unsigned types[] = {VPS_NUT, SPS_NUT, PPS_NUT};
    const bytes sets[] = {bytes(VPS, VPS + sizeof(VPS)), bytes(SPS, SPS + sizeof(SPS)),
                          bytes(PPS, PPS + sizeof(PPS))};
    for (unsigned i = 0; i < 3; i++) {
        const size_t count = i == 2 && second_pps ? 2 : 1;
        assert(c.arrays[i].first == (0x80U | types[i]));
        assert(c.arrays[i].second.size() == count && c.arrays[i].second[0] == sets[i]);
    }
    if (second_pps) assert(c.arrays[2].second[1] == bytes(PPS1, PPS1 + sizeof(PPS1)));
}

/* The user_data_unregistered telemetry SEI after a NAL header of
 * header_bytes: the same parse for H.264 (1) and H.265 (2). Returns false
 * for any other SEI. */
static bool telemetry(const uint8_t *nal, size_t length, size_t header_bytes,
                      std::string *json)
{
    bytes rbsp;
    unsigned zeros = 0;
    for (size_t i = header_bytes; i < length; i++) {
        if (zeros >= 2 && nal[i] == 3) { zeros = 0; continue; }
        zeros = nal[i] ? 0 : zeros + 1;
        rbsp.push_back(nal[i]);
    }
    size_t at = 0, size = 0;
    if (rbsp.size() < 2 || rbsp[at++] != 5) return false;
    while (at < rbsp.size() && rbsp[at] == 255) { size += 255; at++; }
    if (at == rbsp.size()) return false;
    size += rbsp[at++];
    if (size < 16 || at + size + 1 != rbsp.size() || rbsp[at + size] != 0x80) return false;
    if (memcmp(&rbsp[at], ca_video_metadata_uuid, 16)) return false;
    json->assign((const char*)&rbsp[at + 16], size - 16);
    return true;
}

static long json_number(const std::string &json, const char *key)
{
    size_t found = json.find(key);
    assert(found != std::string::npos);
    return strtol(json.c_str() + found + strlen(key), NULL, 10);
}

/* The fixture frame in each sample's telemetry and the sample's NAL types. */
struct sample { long frame; unsigned vcl; std::vector<unsigned> types; };

static std::vector<sample> samples(const char *path)
{
    bytes data = read_file(path);
    std::vector<sample> result;
    for (size_t at = 0; at + 8 <= data.size(); at += get32(&data[at])) {
        uint32_t size = get32(&data[at]);
        assert(size >= 8 && size <= data.size() - at);
        if (memcmp(&data[at + 4], "mdat", 4)) continue;
        sample s = {LONG_MIN, 64, {}};
        unsigned telemetry_count = 0;
        for (size_t nal = at + 8; nal < at + size;) {
            assert(nal + 4 <= at + size);
            uint32_t length = get32(&data[nal]);
            assert(length >= 2 && length <= at + size - nal - 4);
            const uint8_t *p = &data[nal + 4];
            unsigned type = (p[0] >> 1) & 63;
            assert((p[0] & 0x81) == 0 && (p[1] >> 3) == 0); /* layer 0 */
            s.types.push_back(type);
            std::string json;
            if (type == PREFIX_SEI && telemetry(p, length, 2, &json)) {
                /* before any slice, and with the HEVC SEI header */
                assert(s.vcl == 64 && p[0] == 0x4e && p[1] == 0x01);
                assert(json.find("\"schema\":\"apcg.telemetry.v1\"") != std::string::npos);
                assert(json_number(json, "\"utc_us\":") > 1600000000000000L);
                s.frame = json_number(json, "\"lat_e7\":") - LAT_BASE;
                telemetry_count++;
            }
            if (type <= 31 && s.vcl == 64) s.vcl = type;
            nal += 4 + length;
        }
        assert(telemetry_count == 1 && s.vcl < 64);
        result.push_back(s);
    }
    return result;
}

/* The sample keeps the encoder's SEI and slices, without the AUD or any
 * parameter set. */
static void check_sample(const sample &s)
{
    const std::vector<unsigned> expected = {PREFIX_SEI, PREFIX_SEI, s.vcl, s.vcl, SUFFIX_SEI};
    assert(s.types == expected);
}

static unsigned irap_type(unsigned frame)
{
    static const unsigned types[] = {IDR_W_RADL, IDR_N_LP, CRA, BLA_W_LP, BLA_W_RADL, BLA_N_LP};
    return frame % 10 ? 0 : types[(frame / 10) % 6];
}

static void test_track(void)
{
    char path[256];
    snprintf(path, sizeof(path), "%s/track.mp4", directory);
    struct ca_mp4 *writer = NULL;
    assert(ca_mp4_open_codec(&writer, path, CA_VIDEO_H265, 320, 240, 10) == 0);
    /* nothing before the first IRAP with its parameter sets is recorded */
    assert(write_au(writer, 1000, access_unit(1000, 0, false, 600)) == 0);
    assert(write_au(writer, 1001, access_unit(1001, IDR_W_RADL, false, 600)) == 0);
    /* a second PPS: the hvcC must count each array's own NAL units */
    bytes headers;
    put_parameter_sets(headers);
    put_nal(headers, PPS1, sizeof(PPS1));
    assert(write_au(writer, 0, headers) == 0);
    const unsigned frames = 60;
    for (unsigned n = 0; n < frames; n++) {
        unsigned irap = irap_type(n);
        assert(write_au(writer, n, access_unit(n, irap, irap != 0, 600)) == 0);
    }
    drain(writer);
    struct ca_mp4_stats stats;
    ca_mp4_get_stats(writer, &stats);
    assert(stats.frames_written == frames && stats.frames_dropped == 0);
    assert(ca_mp4_close(writer) == 0);

    layout file = parse_file(path);
    assert(file.tiled && file.moov && file.sidx == 1 && file.moofs == frames);
    check_config(file.config, true);
    /* every IRAP type starts an indexed fragment */
    assert(file.sidx_refs == frames / 10 && file.sidx_sap == 0x90000000U);
    assert(file.timescale == 1000 && file.duration == frames * 100);
    std::vector<sample> written = samples(path);
    assert(written.size() == frames);
    for (unsigned n = 0; n < frames; n++) {
        assert(written[n].frame == (long)n);
        assert(written[n].vcl == (irap_type(n) ? irap_type(n) : (unsigned)TRAIL_R));
        check_sample(written[n]);
    }
    unlink(path);
    puts("PASS H.265 track: hvc1, hvcC with VPS/SPS/PPS and the SPS profile/level, "
         "60 samples of VCL and SEI, telemetry in an HEVC prefix SEI, IRAPs indexed");
}

/* The same telemetry parse reads both codecs' SEI. */
static void test_sei(void)
{
    struct ca_metadata metadata = {};
    metadata.have_position = true;
    metadata.lat_e7 = 7;
    struct timespec utc = {1700000000, 123456000};
    char json[CA_VIDEO_METADATA_JSON_MAX];
    size_t json_length = ca_video_metadata_json(&metadata, &utc, 9000, json, sizeof(json));
    assert(json_length > 0);
    uint8_t h264[CA_VIDEO_METADATA_SEI_MAX], h265[CA_VIDEO_METADATA_SEI_MAX];
    size_t h264_length = ca_video_metadata_sei(CA_VIDEO_H264, json, json_length, h264, sizeof(h264));
    size_t h265_length = ca_video_metadata_sei(CA_VIDEO_H265, json, json_length, h265, sizeof(h265));
    assert(h264_length + 1 == h265_length);
    assert(!memcmp(h265, "\0\0\0\1\x4e\x01", 6) && !memcmp(h264, "\0\0\0\1\x06", 5));
    assert(!memcmp(h264 + 5, h265 + 6, h264_length - 5));
    std::string a, b;
    assert(telemetry(h264 + 4, h264_length - 4, 1, &a));
    assert(telemetry(h265 + 4, h265_length - 4, 2, &b));
    assert(a == b && a == std::string(json, json_length));
    assert(json_number(b, "\"utc_us\":") == 1700000000123456L);
    puts("PASS telemetry SEI: H.265 prefix SEI (4E 01) with the H.264 payload");
}

/* A FAT split continues on an IRAP, and the continuation carries the VPS,
 * SPS and PPS that only the first access unit sent. */
static void test_rollover(void)
{
    char path[256];
    snprintf(path, sizeof(path), "%s/split.mp4", directory);
    fat = true;
    assert(setenv("CAMERA_APP_RECORD_SEGMENT_BYTES", "65536", 1) == 0);
    struct ca_mp4 *writer = NULL;
    assert(ca_mp4_open_codec(&writer, path, CA_VIDEO_H265, 320, 240, 10) == 0);
    const unsigned frames = 100;
    for (unsigned n = 0; n < frames; n++) {
        unsigned irap = irap_type(n);
        assert(write_au(writer, n, access_unit(n, irap, n == 0, 2000)) == 0);
    }
    assert(ca_mp4_close(writer) == 0);
    fat = false;
    unsetenv("CAMERA_APP_RECORD_SEGMENT_BYTES");

    glob_t parts;
    char pattern[256];
    snprintf(pattern, sizeof(pattern), "%s/split*.mp4", directory);
    assert(glob(pattern, 0, NULL, &parts) == 0 && parts.gl_pathc >= 2);
    assert(strstr(parts.gl_pathv[1], "split_part0002.mp4"));
    long next = 0;
    for (size_t i = 0; i < parts.gl_pathc; i++) {
        const char *part = parts.gl_pathv[i];
        layout file = parse_file(part);
        assert(file.tiled && file.moov && file.sidx == 1);
        check_config(file.config);
        std::vector<sample> written = samples(part);
        assert(written.size() == file.moofs && !written.empty());
        assert(irap_type((unsigned)written[0].frame) == written[0].vcl);
        if (i + 1 < parts.gl_pathc) assert(written.size() % 10 == 0);
        for (const sample &s : written) {
            assert(s.frame == next++);
            check_sample(s);
        }
        unlink(part);
    }
    assert(next == frames);
    printf("PASS H.265 split: %zu parts, each from an IRAP with the VPS/SPS/PPS\n",
           parts.gl_pathc);
    globfree(&parts);
}

static void set_hold(bool hold)
{
    pthread_mutex_lock(&lock);
    hold_writes = hold;
    if (hold) writes_entered = 0;
    pthread_cond_broadcast(&changed);
    pthread_mutex_unlock(&lock);
}

static void wait_for_write_entry(void)
{
    struct timespec deadline;
    assert(clock_gettime(CLOCK_REALTIME, &deadline) == 0);
    deadline.tv_sec += 5;
    pthread_mutex_lock(&lock);
    while (writes_entered == 0)
        assert(pthread_cond_timedwait(&changed, &lock, &deadline) == 0);
    pthread_mutex_unlock(&lock);
}

static uint64_t dropped(struct ca_mp4 *writer)
{
    struct ca_mp4_stats stats;
    ca_mp4_get_stats(writer, &stats);
    return stats.frames_dropped;
}

/* A full buffer drops pictures until an IRAP; parameter sets sent on their
 * own meanwhile are kept, and a CRA resumes the recording. */
static void test_drops(void)
{
    char path[256];
    snprintf(path, sizeof(path), "%s/drops.mp4", directory);
    assert(setenv("CAMERA_APP_RECORD_BUFFER_BYTES", "65536", 1) == 0);
    struct ca_mp4 *writer = NULL;
    set_hold(true);
    assert(ca_mp4_open_codec(&writer, path, CA_VIDEO_H265, 320, 240, 10) == 0);
    unsetenv("CAMERA_APP_RECORD_BUFFER_BYTES");
    unsigned n = 0, pictures = 0;
    assert(write_au(writer, n++, access_unit(0, IDR_W_RADL, true, 3000)) == 0);
    pictures++;
    wait_for_write_entry();
    /* frames 10, 20 and 30 are IDRs that do not fit */
    for (; n < 40; n++, pictures++)
        assert(write_au(writer, n, access_unit(n, n % 10 ? 0 : IDR_W_RADL, n % 10 == 0, 3000)) == 0);
    assert(dropped(writer) > 0);
    set_hold(false);
    for (; n < 50; n++, pictures++) {
        assert(write_au(writer, n, access_unit(n, 0, false, 3000)) == 0);
        drain(writer);
    }
    /* still dropping: the parameter sets, the VPS alone, are kept */
    uint64_t before = dropped(writer);
    bytes vps, sps_pps;
    put_nal(vps, VPS, sizeof(VPS));
    put_nal(sps_pps, SPS, sizeof(SPS));
    put_nal(sps_pps, PPS, sizeof(PPS));
    assert(write_au(writer, n, vps) == 0 && write_au(writer, n, sps_pps) == 0);
    drain(writer);
    assert(dropped(writer) == before);
    /* an IRAP that is not an IDR resumes */
    for (; n < 80; n++, pictures++) {
        unsigned irap = n == 50 ? CRA : n == 60 ? BLA_W_LP : n == 70 ? IDR_N_LP : 0;
        assert(write_au(writer, n, access_unit(n, irap, false, 3000)) == 0);
        drain(writer);
    }
    struct ca_mp4_stats stats;
    ca_mp4_get_stats(writer, &stats);
    assert(stats.drop_events == 1 && stats.frames_written + stats.frames_dropped == pictures);
    assert(ca_mp4_close(writer) == 0);

    layout file = parse_file(path);
    assert(file.tiled && file.moov && file.sidx == 1 && file.moofs == stats.frames_written);
    check_config(file.config);
    std::vector<sample> written = samples(path);
    assert(written.size() == stats.frames_written);
    size_t gap = 0;
    for (size_t i = 1; i < written.size(); i++) {
        if (written[i].frame == written[i - 1].frame + 1) continue;
        assert(gap == 0);
        gap = i;
    }
    assert(gap != 0 && written[0].frame == 0 && written[0].vcl == IDR_W_RADL);
    assert(written[gap].frame == 50 && written[gap].vcl == CRA);
    assert(written.back().frame == 79);
    for (const sample &s : written) check_sample(s);
    printf("PASS H.265 drops: %llu dropped, parameter sets kept, resumed on a CRA\n",
           (unsigned long long)stats.frames_dropped);
    unlink(path);
}

/* The card fills: the file is cut back to whole fragments, with its index,
 * duration and decoder configuration. */
static void test_card_full(void)
{
    char path[256];
    snprintf(path, sizeof(path), "%s/full.mp4", directory);
    struct ca_mp4 *writer = NULL;
    assert(ca_mp4_open_codec(&writer, path, CA_VIDEO_H265, 320, 240, 10) == 0);
    unsigned n = 0;
    /* past the first write and a timed one */
    for (; n < 15; n++) {
        assert(write_au(writer, n, access_unit(n, irap_type(n), irap_type(n) != 0, 3000)) == 0);
        drain(writer);
        usleep(100000);
    }
    pthread_mutex_lock(&lock);
    card_full = true;
    card_filled = false;
    pthread_mutex_unlock(&lock);
    int result = 0;
    uint64_t deadline = now_us() + 10000000U;
    for (; result == 0; n++) {
        result = write_au(writer, n, access_unit(n, irap_type(n), irap_type(n) != 0, 3000));
        assert(now_us() < deadline);
        usleep(20000);
    }
    assert(result < 0 && errno == ENOSPC);
    errno = 0;
    assert(ca_mp4_close(writer) < 0 && errno == ENOSPC);
    pthread_mutex_lock(&lock);
    assert(card_filled);
    card_full = false;
    pthread_mutex_unlock(&lock);

    layout file = parse_file(path);
    assert(file.tiled && file.moov && file.sidx == 1);
    assert(file.moofs >= 1 && file.moofs < n);
    assert((uint64_t)file.duration * 10 == (uint64_t)file.moofs * file.timescale);
    check_config(file.config);
    std::vector<sample> written = samples(path);
    assert(written.size() == file.moofs);
    for (size_t i = 0; i < written.size(); i++) {
        assert(written[i].frame == (long)i);
        check_sample(written[i]);
    }
    printf("PASS H.265 card full: %u of %u frames kept on whole fragments\n", file.moofs, n);
    unlink(path);
}

/* An encoder's Annex-B H.265, split on its AUDs, for a decoder to check. */
static int mux_stream(const char *input, const char *output)
{
    bytes data = read_file(input);
    unlink(output);
    struct ca_mp4 *writer = NULL;
    assert(ca_mp4_open_codec(&writer, output, CA_VIDEO_H265, 320, 240, 10) == 0);
    size_t start = 0, scan = 0, nal, n;
    unsigned frames = 0;
    while (start < data.size()) {
        size_t end = data.size();
        while (ca_annexb_next(data.data(), data.size(), &scan, &nal, &n)) {
            if (n && ((data[nal] >> 1) & 63) == AUD && nal > start + 4) {
                end = nal - (data[nal - 4] == 0 ? 4 : 3);
                break;
            }
        }
        ca_metadata_set_position((int32_t)(LAT_BASE + frames), 1491652300, 620, 30, 0);
        assert(ca_mp4_write_video(writer, &data[start], end - start, frames * 9000U,
                                  false, 54.7f) == 0);
        frames++;
        start = end;
    }
    assert(ca_mp4_close(writer) == 0);
    std::vector<sample> written = samples(output);
    assert(written.size() == frames && frames > 1);
    for (unsigned i = 0; i < frames; i++) assert(written[i].frame == (long)i);
    printf("frames=%u\n", frames);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc == 3) return mux_stream(argv[1], argv[2]);
    assert(argc == 1);
    assert(mkdtemp(directory) != NULL);
    test_sei();
    test_track();
    test_rollover();
    test_drops();
    test_card_full();
    assert(rmdir(directory) == 0);
    return 0;
}
