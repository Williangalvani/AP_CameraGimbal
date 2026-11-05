#pragma once
#include "APC_StringBuffer.h"
#include <sys/socket.h>
#include <cerrno>
#include <cstdio>
#define APC_CORS_COMMON "Vary: Origin\r\n" \
    "Access-Control-Expose-Headers: Content-Length, Content-Range, Content-Disposition, " \
    "Accept-Ranges, ETag, Server, WWW-Authenticate\r\n"
// Response framing and browser security policy share one owner/boundary.
// The connection remains owned by the server or its streaming child.
class APC_HTTPResponse {
public:
    APC_HTTPResponse(int fd, const char *server_name) : _fd(fd), _server_name(server_name) {}
    void send(int status, const char *reason, const char *content_type,
              const char *body, size_t body_len, const char *extra_headers) const;
    // CORS preflight: 204 for any path, before authentication.
    void send_preflight(const char *request_headers, size_t length) const;
    // Each worker serves one request, so its CORS headers are process state.
    // Set them before any reply; every status line must carry cors_headers().
    static void set_cors_origin(const char *origin, size_t length);
    static const char *cors_headers() { return _cors; }
private:
    static constexpr size_t MAX_ORIGIN = 255;
    static constexpr size_t MAX_REQUEST_HEADERS = 512;
    // Reflected values must be printable ASCII, so they cannot split headers.
    static bool reflectable(const char *value, size_t length, size_t limit, bool spaces)
    {
        if (value == nullptr || length == 0 || length > limit) return false;
        for (size_t i = 0; i < length; i++) {
            const unsigned char c = (unsigned char)value[i];
            if (c < 0x20 || c > 0x7e || (c == ' ' && !spaces)) return false;
        }
        return true;
    }
    static bool send_all(int fd, const void *buffer, size_t length)
    {
        const char *cursor = static_cast<const char *>(buffer);
        while (length) {
            const ssize_t sent = ::send(fd, cursor, length, MSG_NOSIGNAL);
            if (sent < 0 && errno == EINTR) continue;
            if (sent <= 0) return false;
            cursor += sent;
            length -= size_t(sent);
        }
        return true;
    }
    static inline char _cors[MAX_ORIGIN + 256] =
        "Access-Control-Allow-Origin: *\r\n" APC_CORS_COMMON;
    int _fd;
    const char *_server_name;
};
inline void APC_HTTPResponse::set_cors_origin(const char *origin, size_t length)
{
    // Credentials are only ever allowed for a named origin, never for *.
    if (reflectable(origin, length, MAX_ORIGIN, false)) {
        snprintf(_cors, sizeof(_cors), "Access-Control-Allow-Origin: %.*s\r\n"
                 "Access-Control-Allow-Credentials: true\r\n" APC_CORS_COMMON,
                 (int)length, origin);
    } else {
        snprintf(_cors, sizeof(_cors), "Access-Control-Allow-Origin: *\r\n" APC_CORS_COMMON);
    }
}
inline void APC_HTTPResponse::send_preflight(const char *request_headers, size_t length) const
{
    char headers[MAX_REQUEST_HEADERS + 160];

    if (!reflectable(request_headers, length, MAX_REQUEST_HEADERS, true)) {
        request_headers = "Authorization, Content-Type, Range, X-Requested-With";
        length = strlen(request_headers);
    }
    snprintf(headers, sizeof(headers),
             "Access-Control-Allow-Methods: GET, POST, PUT, DELETE, OPTIONS\r\n"
             "Access-Control-Allow-Headers: %.*s\r\n"
             "Access-Control-Max-Age: 86400\r\n", (int)length, request_headers);
    send(204, "No Content", "text/plain; charset=utf-8", "", 0, headers);
}
inline void APC_HTTPResponse::send(int status, const char *reason,
                          const char *content_type, const char *body,
                          size_t body_len, const char *extra_headers) const
{
    APC_StringBuffer header;

    header.reset();
    if (header.appendf("HTTP/1.1 %d %s\r\n"
        "Server: %s\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %zu\r\n"
        "Connection: close\r\n"
        "Cache-Control: no-store\r\n"
        "X-Content-Type-Options: nosniff\r\n"
        "X-Frame-Options: DENY\r\n"
        "Content-Security-Policy: default-src 'none'; style-src 'self' 'unsafe-inline'; "
        "img-src 'self' https://firmware.ardupilot.org; media-src 'self' blob:; "
        "script-src 'self'; connect-src 'self'; form-action 'self'; "
        "frame-ancestors 'none'\r\n"
        "%s%s\r\n",
        status, reason, _server_name, content_type, body_len,
        _cors, extra_headers ? extra_headers : "") &&
        send_all(_fd, header.data(), header.size()) && body_len > 0) {
        (void)send_all(_fd, body, body_len);
    }
    header.reset();
}
