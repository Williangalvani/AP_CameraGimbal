// Stress test for the xop RTSP server behind rtsp_xop.cpp. A fast H.265/H.264
// producer runs throughout while clients misbehave: UDP playback to ports
// nobody reads, an RTP-over-TCP reader that stops reading, abrupt closes, more
// idle connections than the server admits, a full house of players and a
// player that goes silent until its session times out. A prober meanwhile
// requires every fresh OPTIONS to be answered within 1 s, and at the end the
// server must be back to the file descriptors it started with.
//
// Plain loopback never refuses a UDP send. tests/paced_loopback.sh reruns
// this with RTP over UDP limited to 40 Mbit/s, the condition that used to
// wedge the server.
//
// RTP pacing: first a healthy RTP-over-TCP player of a realistic 4K stream
// must get every packet with key frames spread out, and its own OPTIONS
// answered while a key frame is still being paced. The main stream then stays
// paced while the clients above misbehave.
#include "camera_app/rtsp.h"

#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <signal.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

static unsigned port;
static std::atomic<bool> stop_threads{false};
static std::atomic<bool> probe_paused{false};
static std::atomic<long> probe_worst_ms{0};
static std::atomic<unsigned> probes{0};
static std::atomic<unsigned> probe_failures{0};
static std::atomic<long> push_worst_us{0};
static std::atomic<unsigned long> frames_pushed{0};
static std::atomic<unsigned> watchdog_fired{0};

#define CHECK(condition) do { if (!(condition)) { \
    fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #condition); \
    exit(1); } } while (0)

static long now_ms()
{
    return (long)std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

static long now_us()
{
    return (long)std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

static void sleep_ms(long ms)
{
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

static void record_max(std::atomic<long> &worst, long value)
{
    long seen = worst.load();
    while (value > seen && !worst.compare_exchange_weak(seen, value)) {}
}

static int count_fds()
{
    int count = 0;
    DIR *directory = opendir("/proc/self/fd");
    if (directory == nullptr) return -1;
    while (struct dirent *entry = readdir(directory)) {
        if (entry->d_name[0] != '.') count++;
    }
    closedir(directory);
    return count - 1; // the directory's own descriptor
}

static int connect_server(int receive_buffer = 0)
{
    int fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    CHECK(fd >= 0);
    if (receive_buffer > 0) {
        CHECK(setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &receive_buffer, sizeof(receive_buffer)) == 0);
    }
    const timeval timeout{2, 0};
    (void)setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    (void)setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons((uint16_t)port);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (connect(fd, (const sockaddr *)&address, sizeof(address)) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

static bool send_text(int fd, const std::string &text)
{
    return send(fd, text.data(), text.size(), MSG_NOSIGNAL) == (ssize_t)text.size();
}

// One RTSP response (headers plus any Content-Length body), or "" on timeout.
static std::string read_response(int fd, long timeout_ms)
{
    std::string data;
    const long deadline = now_ms() + timeout_ms;
    for (;;) {
        const size_t end = data.find("\r\n\r\n");
        if (end != std::string::npos) {
            size_t body = 0;
            const size_t length = data.find("Content-Length:");
            if (length != std::string::npos && length < end) body = strtoul(data.c_str() + length + 15, nullptr, 10);
            if (data.size() >= end + 4 + body) return data.substr(0, end + 4 + body);
        }
        const long left = deadline - now_ms();
        if (left <= 0) return "";
        pollfd wait{fd, POLLIN, 0};
        if (poll(&wait, 1, (int)left) <= 0) return "";
        char buffer[4096];
        const ssize_t got = recv(fd, buffer, sizeof(buffer), 0);
        if (got <= 0) return "";
        data.append(buffer, (size_t)got);
    }
}

static std::string url(const char *path)
{
    return "rtsp://127.0.0.1:" + std::to_string(port) + "/" + path;
}

static std::string exchange(int fd, const std::string &method, const std::string &target,
                            int cseq, const std::string &headers = "")
{
    if (!send_text(fd, method + " " + target + " RTSP/1.0\r\nCSeq: " + std::to_string(cseq) +
                           "\r\nUser-Agent: test_rtsp_robustness\r\n" + headers + "\r\n")) return "";
    return read_response(fd, 2000);
}

static std::string session_of(const std::string &reply)
{
    const size_t at = reply.find("Session: ");
    if (at == std::string::npos) return "";
    return reply.substr(at + 9, reply.find_first_of(";\r", at + 9) - at - 9);
}

static unsigned unused_udp_port()
{
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    socklen_t length = sizeof(address);
    CHECK(bind(fd, (const sockaddr *)&address, sizeof(address)) == 0);
    CHECK(getsockname(fd, (sockaddr *)&address, &length) == 0);
    close(fd);
    return ntohs(address.sin_port) & ~1U;
}

static std::string describe(const char *path)
{
    int fd = connect_server();
    CHECK(fd >= 0);
    const std::string reply = exchange(fd, "DESCRIBE", url(path), 1, "Accept: application/sdp\r\n");
    close(fd);
    CHECK(reply.find("200 OK") != std::string::npos);
    return reply;
}

// A playing client whose RTP goes to a UDP port where nobody reads.
static int play_udp(const char *path, bool expect_success = true)
{
    int fd = connect_server();
    CHECK(fd >= 0);
    CHECK(exchange(fd, "DESCRIBE", url(path), 1, "Accept: application/sdp\r\n").find("200 OK") != std::string::npos);
    const unsigned rtp = unused_udp_port();
    const std::string setup = exchange(fd, "SETUP", url(path) + "/track0", 2,
        "Transport: RTP/AVP;unicast;client_port=" + std::to_string(rtp) + "-" + std::to_string(rtp + 1) + "\r\n");
    CHECK(setup.find("200 OK") != std::string::npos);
    CHECK(setup.find("timeout=60") != std::string::npos);
    const std::string play = exchange(fd, "PLAY", url(path), 3, "Session: " + session_of(setup) + "\r\n");
    if (expect_success) CHECK(play.find("200 OK") != std::string::npos);
    return fd;
}

// A playing RTP-over-TCP client that then never reads again.
static int play_tcp_and_stall(const char *path)
{
    int fd = connect_server(4096);
    CHECK(fd >= 0);
    CHECK(exchange(fd, "DESCRIBE", url(path), 1, "Accept: application/sdp\r\n").find("200 OK") != std::string::npos);
    const std::string setup = exchange(fd, "SETUP", url(path) + "/track0", 2,
                                       "Transport: RTP/AVP/TCP;unicast;interleaved=0-1\r\n");
    CHECK(setup.find("200 OK") != std::string::npos);
    CHECK(send_text(fd, "PLAY " + url(path) + " RTSP/1.0\r\nCSeq: 3\r\nSession: " + session_of(setup) + "\r\n\r\n"));
    return fd;
}

// The server's end of a client connection, by socket inode: the server runs
// in this process, so whether it closed the connection shows in /proc/self/fd
// without the client reading (and so unstalling) anything.
static unsigned long server_socket_inode(int client_fd)
{
    sockaddr_in local{};
    socklen_t length = sizeof(local);
    CHECK(getsockname(client_fd, (sockaddr *)&local, &length) == 0);
    FILE *table = fopen("/proc/net/tcp", "r");
    CHECK(table != nullptr);
    char line[512];
    unsigned long found = 0;
    while (fgets(line, sizeof(line), table)) {
        unsigned local_port, remote_port;
        unsigned long inode;
        if (sscanf(line, " %*d: %*x:%x %*x:%x %*x %*x:%*x %*x:%*x %*x %*u %*u %lu",
                   &local_port, &remote_port, &inode) == 3 &&
            local_port == port && remote_port == ntohs(local.sin_port)) found = inode;
    }
    fclose(table);
    CHECK(found != 0);
    return found;
}

static bool socket_open(unsigned long inode)
{
    const std::string name = "socket:[" + std::to_string(inode) + "]";
    bool open = false;
    DIR *directory = opendir("/proc/self/fd");
    CHECK(directory != nullptr);
    while (struct dirent *entry = readdir(directory)) {
        char target[64];
        const std::string path = std::string("/proc/self/fd/") + entry->d_name;
        const ssize_t size = readlink(path.c_str(), target, sizeof(target) - 1);
        if (size > 0 && name == std::string(target, size)) open = true;
    }
    closedir(directory);
    return open;
}

// For connections that receive nothing: true once the server closed fd.
static bool peer_closed(int fd, long timeout_ms)
{
    pollfd wait{fd, POLLIN, 0};
    if (poll(&wait, 1, (int)timeout_ms) <= 0) return false;
    char byte;
    const ssize_t got = recv(fd, &byte, 1, MSG_DONTWAIT);
    return got == 0 || (got < 0 && errno == ECONNRESET);
}

static bool closed_by_server(unsigned long inode, long timeout_ms)
{
    const long deadline = now_ms() + timeout_ms;
    while (now_ms() < deadline) {
        if (!socket_open(inode)) return true;
        sleep_ms(100);
    }
    return false;
}

static void prober()
{
    while (!stop_threads) {
        if (probe_paused) { sleep_ms(20); continue; }
        const long start = now_ms();
        int fd = connect_server();
        std::string reply;
        if (fd >= 0) {
            reply = exchange(fd, "OPTIONS", url("video1"), 1);
            close(fd);
        }
        const long elapsed = now_ms() - start;
        probes++;
        if (reply.find("RTSP/1.0 200 OK") == std::string::npos || elapsed > 1000) {
            probe_failures++;
            fprintf(stderr, "OPTIONS probe failed after %ld ms: %.40s\n", elapsed, reply.c_str());
        }
        record_max(probe_worst_ms, elapsed);
        sleep_ms(100);
    }
}

static void append_nal(std::vector<uint8_t> &out, std::initializer_list<uint8_t> header, size_t payload)
{
    out.insert(out.end(), {0, 0, 0, 1});
    out.insert(out.end(), header);
    for (size_t i = 0; i < payload; i++) out.push_back((uint8_t)(0x80 | ((i * 37 + out.size()) & 0x7f)));
}

static std::vector<uint8_t> access_unit(bool h265, bool key, size_t size)
{
    std::vector<uint8_t> out;
    if (h265) {
        if (key) {
            append_nal(out, {0x40, 0x01}, 20); // VPS
            append_nal(out, {0x42, 0x01}, 30); // SPS
            append_nal(out, {0x44, 0x01}, 8);  // PPS
        }
        append_nal(out, {(uint8_t)(key ? 19 << 1 : 1 << 1), 0x01}, size);
    } else {
        if (key) {
            append_nal(out, {0x67, 0x64, 0x00, 0x33}, 20); // SPS, High@5.1
            append_nal(out, {0x68}, 6);                    // PPS
        }
        append_nal(out, {(uint8_t)(key ? 0x65 : 0x41)}, size);
    }
    return out;
}

// About 0.8 Gbit/s across both streams: far faster than any client drains.
static void producer(struct ca_rtsp *rtsp)
{
    const std::vector<uint8_t> units[2][2] = {
        {access_unit(true, false, 60000), access_unit(true, true, 400000)},
        {access_unit(false, false, 15000), access_unit(false, true, 90000)},
    };
    for (unsigned long frame = 0; !stop_threads; frame++) {
        const bool key = frame % 30 == 0;
        for (unsigned stream = 0; stream < 2; stream++) {
            const std::vector<uint8_t> &unit = units[stream][key];
            const auto start = std::chrono::steady_clock::now();
            (void)ca_rtsp_push_video(rtsp, stream, unit.data(), unit.size(), key, 70.0f);
            record_max(push_worst_us, (long)std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - start).count());
        }
        frames_pushed++;
        std::this_thread::sleep_for(std::chrono::microseconds(500));
    }
}

static void count_watchdog(void *)
{
    watchdog_fired++;
}

// What an RTP-over-TCP player receives: RTP packets on channel 0 and the
// arrival of each RTSP reply, timed on arrival.
struct InterleavedReader {
    struct Packet {
        long at_us;
        uint16_t seq;
        bool marker;
        uint32_t timestamp;
        // the payload's first bytes: the NAL or FU header
        uint8_t head[3];
        size_t size;
    };
    std::mutex mutex;
    std::vector<Packet> packets;
    std::vector<long> replies;
    std::atomic<bool> stop{false};
    bool garbage = false;

    void run(int fd)
    {
        std::string data;
        while (!stop) {
            pollfd wait{fd, POLLIN, 0};
            if (poll(&wait, 1, 50) <= 0) continue;
            char buffer[65536];
            const ssize_t got = recv(fd, buffer, sizeof(buffer), 0);
            if (got <= 0) return;
            const long at = now_us();
            data.append(buffer, (size_t)got);
            std::lock_guard<std::mutex> lock(mutex);
            for (;;) {
                if (data.size() >= 4 && data[0] == '$') {
                    const size_t length = (uint8_t)data[2] << 8 | (uint8_t)data[3];
                    if (data.size() < 4 + length) break;
                    if (data[1] == 0 && length >= 12) {
                        const uint8_t *rtp = (const uint8_t *)data.data() + 4;
                        Packet packet = {at, (uint16_t)(rtp[2] << 8 | rtp[3]), (rtp[1] & 0x80) != 0,
                                         (uint32_t)rtp[4] << 24 | (uint32_t)rtp[5] << 16 | rtp[6] << 8 | rtp[7],
                                         {0, 0, 0}, 0};
                        size_t header = 12U + 4U * (rtp[0] & 0x0fU);
                        if ((rtp[0] & 0x10U) && length >= header + 4U)
                            header += 4U + 4U * (size_t)(rtp[header + 2U] << 8 | rtp[header + 3U]);
                        if (length > header) {
                            packet.size = length - header;
                            memcpy(packet.head, rtp + header, std::min<size_t>(3U, packet.size));
                        }
                        packets.push_back(packet);
                    }
                    data.erase(0, 4 + length);
                } else if (data.compare(0, 8, "RTSP/1.0") == 0 || (data.size() < 8 && data[0] == 'R')) {
                    const size_t end = data.find("\r\n\r\n");
                    if (end == std::string::npos) break;
                    replies.push_back(at);
                    data.erase(0, end + 4);
                } else {
                    if (!data.empty()) garbage = true;
                    break;
                }
            }
        }
    }
};

// A healthy RTP-over-TCP player of a realistic 4K H.265 stream (685 KB key
// frames, 117 KB P frames, 30 fps) paced at the default 60 Mbit/s: no packet
// may be dropped, a key frame takes about 90 ms instead of a line-rate burst,
// and an OPTIONS on the same connection is answered while the key frame is
// still going out. With pacing turned off live, the next key frame bursts.
static int play_tcp(const char *path)
{
    int fd = connect_server();
    CHECK(fd >= 0);
    CHECK(exchange(fd, "DESCRIBE", url(path), 1, "Accept: application/sdp\r\n").find("200 OK") != std::string::npos);
    const std::string setup = exchange(fd, "SETUP", url(path) + "/track0", 2,
                                       "Transport: RTP/AVP/TCP;unicast;interleaved=0-1\r\n");
    CHECK(setup.find("200 OK") != std::string::npos);
    CHECK(exchange(fd, "PLAY", url(path), 3, "Session: " + session_of(setup) + "\r\n").find("200 OK") !=
          std::string::npos);
    return fd;
}

// Frames end at the RTP marker; key frames are the ones of 400+ packets.
static size_t count_frames(const std::vector<InterleavedReader::Packet> &packets,
                           std::vector<long> *key_spread_us = nullptr, std::vector<long> *key_end_us = nullptr)
{
    size_t first = 0, frames = 0;
    for (size_t i = 0; i < packets.size(); i++) {
        if (!packets[i].marker) continue;
        frames++;
        if (i + 1 - first > 400) {
            if (key_spread_us) key_spread_us->push_back(packets[i].at_us - packets[first].at_us);
            if (key_end_us) key_end_us->push_back(packets[i].at_us);
        }
        first = i + 1;
    }
    return frames;
}

static void check_pacing(struct ca_rtsp *rtsp)
{
    const std::vector<uint8_t> key = access_unit(true, true, 685000), delta = access_unit(true, false, 117000);
    CHECK(ca_rtsp_set_pacing(rtsp, 0, 60000000U) == 0);
    int fd = play_tcp("video1");
    InterleavedReader reader;
    std::thread read([&reader, fd] { reader.run(fd); });
    long options_sent = 0;
    for (int frame = 0; frame < 75; frame++) {
        const bool is_key = frame % 30 == 0;
        if (frame == 60) CHECK(ca_rtsp_set_pacing(rtsp, 0, 0) == 0);
        const std::vector<uint8_t> &unit = is_key ? key : delta;
        CHECK(ca_rtsp_push_video(rtsp, 0, unit.data(), unit.size(), is_key, 70.0f) == 0);
        if (frame == 30) {
            sleep_ms(10);
            options_sent = now_us();
            CHECK(send_text(fd, "OPTIONS " + url("video1") + " RTSP/1.0\r\nCSeq: 4\r\n\r\n"));
            sleep_ms(23);
        } else {
            sleep_ms(33);
        }
    }
    sleep_ms(500);
    reader.stop = true;
    read.join();
    close(fd);

    CHECK(!reader.garbage);
    unsigned gaps = 0;
    for (size_t i = 1; i < reader.packets.size(); i++)
        gaps += reader.packets[i].seq != (uint16_t)(reader.packets[i - 1].seq + 1);
    std::vector<long> key_spread_us, key_end_us;
    const size_t frames = count_frames(reader.packets, &key_spread_us, &key_end_us);
    printf("pacing: %zu packets, %zu frames, %u sequence gaps; key frames over",
           reader.packets.size(), frames, gaps);
    for (long spread : key_spread_us) printf(" %ld", spread / 1000);
    printf(" ms; OPTIONS answered in %ld ms\n",
           reader.replies.empty() ? -1L : (reader.replies[0] - options_sent) / 1000);
    CHECK(frames == 75 && gaps == 0);
    CHECK(key_spread_us.size() == 3);
    CHECK(key_spread_us[0] >= 70000 && key_spread_us[1] >= 70000);
    CHECK(key_spread_us[2] < 45000);
    CHECK(reader.replies.size() == 1);
    CHECK(reader.replies[0] - options_sent < 50000 && reader.replies[0] < key_end_us[1]);
    puts("pacing: key frames spread at 60 Mbit/s without drops, OPTIONS not held back, off bursts again");

    // Paced below the stream's bitrate, a client falls behind and is dropped
    // to the next key frame, as an unpaced slow client is.
    CHECK(ca_rtsp_set_pacing(rtsp, 0, 8000000U) == 0);
    fd = play_tcp("video1");
    InterleavedReader slow;
    std::thread read_slow([&slow, fd] { slow.run(fd); });
    for (int frame = 0; frame < 90; frame++) {
        const bool is_key = frame % 30 == 0;
        const std::vector<uint8_t> &unit = is_key ? key : delta;
        CHECK(ca_rtsp_push_video(rtsp, 0, unit.data(), unit.size(), is_key, 70.0f) == 0);
        sleep_ms(33);
    }
    sleep_ms(1000);
    slow.stop = true;
    read_slow.join();
    close(fd);
    std::vector<long> slow_keys;
    const size_t slow_frames = count_frames(slow.packets, &slow_keys);
    printf("paced below the bitrate: %zu of 90 frames, %zu key frames\n", slow_frames, slow_keys.size());
    CHECK(!slow.garbage && slow_frames < 85 && slow_keys.size() >= 2);
    CHECK(ca_rtsp_set_pacing(rtsp, 0, 0) == 0);
    puts("pacing: a client paced below the bitrate is dropped to the next key frame");
}

// H.265 NAL units as received: single NAL packets and reassembled FUs
struct ReceivedNal {
    unsigned type;
    size_t size;
};

static std::vector<ReceivedNal> received_nals(const std::vector<InterleavedReader::Packet> &packets,
                                              size_t begin, size_t end)
{
    std::vector<ReceivedNal> nals;
    bool in_fu = false;
    for (size_t i = begin; i < end; i++) {
        const InterleavedReader::Packet &packet = packets[i];
        CHECK(packet.size >= 3);
        const unsigned type = (packet.head[0] >> 1) & 0x3fU;
        CHECK(type != 48); // no aggregation packets
        if (type != 49) {
            CHECK(!in_fu);
            nals.push_back({type, packet.size});
            continue;
        }
        const bool start = (packet.head[2] & 0x80) != 0, stop = (packet.head[2] & 0x40) != 0;
        CHECK(start == !in_fu);
        if (start) nals.push_back({packet.head[2] & 0x3fU, 2});
        CHECK(nals.back().type == (packet.head[2] & 0x3fU));
        nals.back().size += packet.size - 3;
        in_fu = !stop;
    }
    CHECK(!in_fu); // an FU never spans parts or access units
    return nals;
}

// VENC slice output: each slice is pushed as soon as it is encoded. A player
// sees one RTP timestamp per access unit, the marker on its last packet only,
// every slice its own NAL unit or FU, the parameter sets before the first
// slice of a key frame, and does not start in the middle of an access unit.
static void check_slices(struct ca_rtsp *rtsp)
{
    struct Unit {
        std::vector<std::vector<uint8_t>> parts;
        bool key, empty_end;
    };
    std::vector<Unit> units;
    for (int frame = 0; frame < 12; frame++) {
        Unit unit = {{}, frame % 5 == 0, frame == 7};
        std::vector<uint8_t> part;
        if (unit.key) {
            append_nal(part, {0x40, 0x01}, 20); // VPS
            append_nal(part, {0x42, 0x01}, 30); // SPS
            append_nal(part, {0x44, 0x01}, 8);  // PPS
            append_nal(part, {19 << 1, 0x01}, 3000);
            unit.parts.push_back(part);
            part.clear();
            append_nal(part, {19 << 1, 0x01}, 2500);
            append_nal(part, {19 << 1, 0x01}, 4000);
            unit.parts.push_back(part);
            part.clear();
            append_nal(part, {19 << 1, 0x01}, 1000);
            unit.parts.push_back(part);
        } else {
            append_nal(part, {1 << 1, 0x01}, 800 + 100 * frame);
            unit.parts.push_back(part);
            part.clear();
            append_nal(part, {1 << 1, 0x01}, 1200);
            unit.parts.push_back(part);
        }
        units.push_back(unit);
    }
    // the NAL units a player must get for each unit, start codes removed
    auto expected = [](const Unit &unit) {
        std::vector<ReceivedNal> nals;
        for (const auto &part : unit.parts) {
            size_t at = 4;
            while (at < part.size()) {
                size_t next = at;
                while (next + 4 <= part.size() && !(part[next] == 0 && part[next + 1] == 0 && part[next + 2] == 0 &&
                                                     part[next + 3] == 1))
                    next++;
                if (next + 4 > part.size()) next = part.size();
                nals.push_back({(part[at] >> 1) & 0x3fU, next - at});
                at = next + 4;
            }
        }
        return nals;
    };

    CHECK(ca_rtsp_set_pacing(rtsp, 0, 0) == 0);
    // no unit open: an empty part is refused
    CHECK(ca_rtsp_push_video_part(rtsp, 0, nullptr, 0, false, true, 70.0f) < 0);
    int fd = play_tcp("video1");
    InterleavedReader reader;
    std::thread read([&reader, fd] { reader.run(fd); });
    int joiner = -1;
    InterleavedReader late;
    std::thread read_late;
    for (size_t frame = 0; frame < units.size(); frame++) {
        const Unit &unit = units[frame];
        for (size_t i = 0; i < unit.parts.size(); i++) {
            const bool end = i + 1 == unit.parts.size() && !unit.empty_end;
            const auto &part = unit.parts[i];
            CHECK(ca_rtsp_push_video_part(rtsp, 0, part.data(), part.size(), unit.key, end, 70.0f) == 0);
            // a player joining after the first slice of a key frame
            if (frame == 5 && i == 0) {
                joiner = play_tcp("video1");
                read_late = std::thread([&late, joiner] { late.run(joiner); });
                sleep_ms(20);
            }
            sleep_ms(2);
        }
        if (unit.empty_end) CHECK(ca_rtsp_push_video_part(rtsp, 0, nullptr, 0, unit.key, true, 70.0f) == 0);
        sleep_ms(20);
    }
    sleep_ms(300);
    reader.stop = true;
    late.stop = true;
    read.join();
    read_late.join();
    close(fd);
    close(joiner);

    // split at RTP timestamp changes
    auto split = [](const std::vector<InterleavedReader::Packet> &packets) {
        std::vector<std::pair<size_t, size_t>> groups;
        for (size_t i = 0; i < packets.size(); i++) {
            if (i == 0 || packets[i].timestamp != packets[i - 1].timestamp) groups.push_back({i, i});
            groups.back().second = i + 1;
        }
        return groups;
    };
    auto check_unit = [&](const std::vector<InterleavedReader::Packet> &packets, std::pair<size_t, size_t> group,
                          const Unit &unit) {
        for (size_t i = group.first; i < group.second; i++) {
            const bool last = i + 1 == group.second;
            CHECK(packets[i].marker == (last && !unit.empty_end));
        }
        std::vector<ReceivedNal> got = received_nals(packets, group.first, group.second);
        // the metadata SEI goes before the first slice
        unsigned sei = 0;
        for (size_t i = 0; i < got.size(); i++) {
            if (got[i].type != 39) continue;
            sei++;
            CHECK(i + 1 < got.size() && got[i + 1].type < 32);
            CHECK(i == 0 || got[i - 1].type >= 32);
            got.erase(got.begin() + (long)i--);
        }
        CHECK(sei == 1);
        const std::vector<ReceivedNal> want = expected(unit);
        CHECK(got.size() == want.size());
        for (size_t i = 0; i < want.size(); i++) CHECK(got[i].type == want[i].type && got[i].size == want[i].size);
    };

    CHECK(!reader.garbage && !late.garbage);
    unsigned gaps = 0;
    for (size_t i = 1; i < reader.packets.size(); i++)
        gaps += reader.packets[i].seq != (uint16_t)(reader.packets[i - 1].seq + 1);
    const auto groups = split(reader.packets);
    printf("slices: %zu packets, %zu access units, %u sequence gaps; late player %zu packets, %zu access units\n",
           reader.packets.size(), groups.size(), gaps, late.packets.size(), split(late.packets).size());
    CHECK(gaps == 0 && groups.size() == units.size());
    for (size_t i = 0; i < groups.size(); i++) {
        check_unit(reader.packets, groups[i], units[i]);
        if (i > 0) CHECK(reader.packets[groups[i].first].timestamp -
                         reader.packets[groups[i - 1].first].timestamp == 90000U / 25U);
    }
    // the late player starts at the next key frame's first packet
    const auto late_groups = split(late.packets);
    CHECK(late_groups.size() == 2);
    CHECK(late.packets[0].timestamp == reader.packets[groups[10].first].timestamp);
    CHECK(((late.packets[0].head[0] >> 1) & 0x3fU) == 32);
    for (size_t i = 0; i < late_groups.size(); i++) check_unit(late.packets, late_groups[i], units[10 + i]);
    puts("slices: one timestamp and one marker per access unit, slices as NAL units or FUs, "
         "parameter sets first, a late player starts at a key frame");
}

int main()
{
    signal(SIGPIPE, SIG_IGN);
    alarm(300); // a hung server must fail the test rather than hang it

    port = unused_udp_port() | 1U;
    struct ca_rtsp *rtsp = nullptr;
    CHECK(ca_rtsp_open(&rtsp, port, "video1", CA_VIDEO_H265, 25) == 0);
    unsigned sub = 0;
    CHECK(ca_rtsp_add_video(rtsp, "video2", CA_VIDEO_H264, 30, &sub) == 0 && sub == 1);
    CHECK(ca_rtsp_add_alias(rtsp, 0, "stream_0") == 0);
    ca_rtsp_set_watchdog(rtsp, 1500, count_watchdog, nullptr);
    sleep_ms(300);
    const int baseline = count_fds();
    CHECK(baseline > 0);

    // Each stream's SDP carries its frame rate, before the first key frame too.
    CHECK(describe("video1").find("a=rtpmap:96 H265/90000\r\na=framerate:25\r\n") != std::string::npos);
    CHECK(describe("video2").find("a=rtpmap:96 H264/90000\r\na=framerate:30\r\n") != std::string::npos);

    check_pacing(rtsp);
    check_slices(rtsp);

    std::thread produce(producer, rtsp);
    std::thread probe(prober);
    sleep_ms(500);

    {
        const std::string main_sdp = describe("video1"), alias_sdp = describe("stream_0");
        const std::string sub_sdp = describe("video2");
        CHECK(main_sdp.find("sprop-vps=") != std::string::npos);
        CHECK(main_sdp.find("\r\na=framerate:25\r\na=control:track0\r\n") != std::string::npos);
        CHECK(alias_sdp.find("\r\na=framerate:25\r\n") != std::string::npos);
        CHECK(sub_sdp.find("sprop-parameter-sets=") != std::string::npos);
        CHECK(sub_sdp.find("\r\na=framerate:30\r\na=control:track0\r\n") != std::string::npos);
        CHECK(ca_rtsp_set_frame_rate(rtsp, sub, 15) == 0);
        CHECK(describe("video2").find("\r\na=framerate:15\r\n") != std::string::npos);
        CHECK(ca_rtsp_set_frame_rate(rtsp, sub, 30) == 0);
        puts("SDP: a=framerate per stream, aliases included");
    }

    // The main stream (and its alias) stays paced for the clients below.
    CHECK(ca_rtsp_set_pacing(rtsp, 0, 60000000U) == 0);
    CHECK(ca_rtsp_set_pacing(rtsp, 2, 1) < 0);

    // (a) UDP players on both streams, RTP to ports nobody reads.
    const int udp_main = play_udp("video1");
    const long udp_main_since = now_ms();
    const unsigned long udp_main_inode = server_socket_inode(udp_main);
    const int udp_sub = play_udp("video2");
    puts("UDP players on unread ports: playing");

    // (b) RTP over TCP, then the reader stalls with a tiny receive buffer.
    const int stalled = play_tcp_and_stall("video1");
    const long stalled_since = now_ms();
    const unsigned long stalled_inode = server_socket_inode(stalled);
    puts("RTP-over-TCP reader: stalled");

    // (c) Connections closed abruptly: some mid-request, some with a reset.
    for (int i = 0; i < 50; i++) {
        int fd = connect_server();
        CHECK(fd >= 0);
        if (i % 3 == 0) (void)send_text(fd, "OPTIONS " + url("video1") + " RTSP/1.0\r\nCSeq: 1\r\n");
        if (i % 3 == 1) (void)send_text(fd, "DESCRIBE " + url("video1") + " RTSP/1.0\r\nCSeq: 1\r\n\r\n");
        if (i % 2 == 0) {
            const linger reset{1, 0};
            (void)setsockopt(fd, SOL_SOCKET, SO_LINGER, &reset, sizeof(reset));
        }
        close(fd);
    }
    puts("50 abrupt clients: closed");

    // The server must disconnect the stalled reader once it has accepted
    // nothing for 3 s.
    CHECK(closed_by_server(stalled_inode, 8000));
    printf("stalled reader closed after %ld ms\n", now_ms() - stalled_since);
    close(stalled);
    puts("stalled, paced RTP-over-TCP reader: disconnected by the server");

    // More idle connections than the server admits: the oldest idle ones go.
    std::vector<int> idle;
    for (int i = 0; i < 20; i++) {
        int fd = connect_server();
        CHECK(fd >= 0);
        idle.push_back(fd);
        sleep_ms(20);
    }
    unsigned evicted = 0;
    for (int fd : idle) {
        if (peer_closed(fd, 300)) evicted++;
        close(fd);
    }
    printf("20 idle connections: %u evicted\n", evicted);
    CHECK(evicted >= 6);

    // A full house of players: a new connection is refused with 503.
    probe_paused = true;
    sleep_ms(300);
    std::vector<int> players;
    for (int i = 0; i < 14; i++) players.push_back(play_udp(i % 2 ? "video2" : "video1"));
    {
        int fd = connect_server();
        CHECK(fd >= 0);
        const std::string reply = read_response(fd, 2000);
        CHECK(reply.find("503") != std::string::npos);
        close(fd);
    }
    puts("16 players: new connection refused with 503");
    for (int fd : players) close(fd);
    sleep_ms(300);
    probe_paused = false;

    // A player that sends neither requests nor RTCP times out with its
    // session (60 s advertised).
    CHECK(closed_by_server(udp_main_inode, 80000 - (now_ms() - udp_main_since)));
    const long silent = now_ms() - udp_main_since;
    printf("silent UDP player closed after %ld ms\n", silent);
    CHECK(silent >= 60000);
    close(udp_main);
    close(udp_sub);
    stop_threads = true;
    produce.join();
    probe.join();

    int fds = -1;
    for (long deadline = now_ms() + 10000; now_ms() < deadline; sleep_ms(100)) {
        fds = count_fds();
        if (fds == baseline) break;
    }
    printf("file descriptors: %d at start, %d at end\n", baseline, fds);
    printf("OPTIONS probes: %u, failures %u, worst %ld ms\n", probes.load(), probe_failures.load(),
           probe_worst_ms.load());
    printf("frames pushed: %lu, slowest push %ld us\n", frames_pushed.load(), push_worst_us.load());
    CHECK(fds == baseline);
    CHECK(probes > 50 && probe_failures == 0 && probe_worst_ms < 1000);
    CHECK(push_worst_us < 250000);
    CHECK(watchdog_fired == 0);

    // The watchdog notices a stalled event loop, once.
    ca_rtsp_test_stall_loop(rtsp, 3000);
    sleep_ms(3500);
    CHECK(watchdog_fired == 1);
    puts("watchdog: stalled event loop reported");
    ca_rtsp_close(rtsp);
    puts("PASS RTSP server stays responsive under a fast producer and misbehaving clients");
    return 0;
}
