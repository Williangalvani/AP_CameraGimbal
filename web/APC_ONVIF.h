#pragma once
/* The ONVIF side that MAVLink Camera Manager (MCM) needs to adopt this camera:
 * WS-Discovery, the WS-Security UsernameToken digest, and the SOAP replies MCM
 * reads. Element names and namespaces are onvif-rs's, which MCM parses with;
 * web/tests/onvif-check runs our replies through it. */
#include "APC_StringBuffer.h"
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>

namespace apc_onvif {

inline void sha1_block(uint32_t h[5], const unsigned char *block)
{
    uint32_t w[80];
    for (int i = 0; i < 16; i++) {
        w[i] = (uint32_t)block[4 * i] << 24 | (uint32_t)block[4 * i + 1] << 16 |
               (uint32_t)block[4 * i + 2] << 8 | (uint32_t)block[4 * i + 3];
    }
    for (int i = 16; i < 80; i++) {
        const uint32_t v = w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16];
        w[i] = v << 1 | v >> 31;
    }
    uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
    for (int i = 0; i < 80; i++) {
        uint32_t f, k;
        if (i < 20) {
            f = (b & c) | (~b & d);
            k = 0x5A827999U;
        } else if (i < 40) {
            f = b ^ c ^ d;
            k = 0x6ED9EBA1U;
        } else if (i < 60) {
            f = (b & c) | (b & d) | (c & d);
            k = 0x8F1BBCDCU;
        } else {
            f = b ^ c ^ d;
            k = 0xCA62C1D6U;
        }
        const uint32_t t = (a << 5 | a >> 27) + f + e + k + w[i];
        e = d;
        d = c;
        c = b << 30 | b >> 2;
        b = a;
        a = t;
    }
    h[0] += a;
    h[1] += b;
    h[2] += c;
    h[3] += d;
    h[4] += e;
}

inline void sha1(const void *data, size_t length, unsigned char digest[20])
{
    uint32_t h[5] = {0x67452301U, 0xEFCDAB89U, 0x98BADCFEU, 0x10325476U, 0xC3D2E1F0U};
    const unsigned char *bytes = (const unsigned char *)data;
    const size_t whole = length / 64U * 64U;
    for (size_t i = 0; i < whole; i += 64U) sha1_block(h, bytes + i);
    unsigned char tail[128] = {0};
    const size_t rest = length - whole;
    if (rest > 0U) memcpy(tail, bytes + whole, rest);
    tail[rest] = 0x80U;
    const size_t tail_length = rest + 9U <= 64U ? 64U : 128U;
    const uint64_t bits = (uint64_t)length * 8U;
    for (int i = 0; i < 8; i++) tail[tail_length - 1U - (size_t)i] = (unsigned char)(bits >> (8 * i));
    sha1_block(h, tail);
    if (tail_length == 128U) sha1_block(h, tail + 64);
    for (int i = 0; i < 20; i++) digest[i] = (unsigned char)(h[i / 4] >> (24 - 8 * (i % 4)));
}

inline size_t base64_encode(const unsigned char *data, size_t length, char *out, size_t capacity)
{
    static const char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    if ((length + 2U) / 3U * 4U + 1U > capacity) return 0;
    size_t used = 0;
    for (size_t i = 0; i < length; i += 3U) {
        uint32_t v = (uint32_t)data[i] << 16;
        if (i + 1U < length) v |= (uint32_t)data[i + 1U] << 8;
        if (i + 2U < length) v |= data[i + 2U];
        out[used++] = alphabet[v >> 18 & 63U];
        out[used++] = alphabet[v >> 12 & 63U];
        out[used++] = i + 1U < length ? alphabet[v >> 6 & 63U] : '=';
        out[used++] = i + 2U < length ? alphabet[v & 63U] : '=';
    }
    out[used] = '\0';
    return used;
}

inline bool base64_decode(const char *text, unsigned char *out, size_t capacity, size_t *out_length)
{
    uint32_t v = 0;
    unsigned bits = 0;
    size_t used = 0;
    for (const char *p = text; *p != '\0' && *p != '='; p++) {
        int digit;
        if (*p >= 'A' && *p <= 'Z') digit = *p - 'A';
        else if (*p >= 'a' && *p <= 'z') digit = *p - 'a' + 26;
        else if (*p >= '0' && *p <= '9') digit = *p - '0' + 52;
        else if (*p == '+') digit = 62;
        else if (*p == '/') digit = 63;
        else if (isspace((unsigned char)*p)) continue;
        else return false;
        v = v << 6 | (uint32_t)digit;
        bits += 6U;
        if (bits >= 8U) {
            bits -= 8U;
            if (used == capacity) return false;
            out[used++] = (unsigned char)(v >> bits);
        }
    }
    *out_length = used;
    return true;
}

/* The next start tag at or after p: returns its '<' and sets its local name
 * (without the namespace prefix), or returns NULL. */
inline const char *next_start_tag(const char *p, const char **local, size_t *local_length)
{
    for (p = strchr(p, '<'); p != NULL; p = strchr(p + 1, '<')) {
        const char *name = p + 1;
        if (*name == '/' || *name == '?' || *name == '!') continue;
        const char *end = name + strcspn(name, " \t\r\n/>");
        const char *colon = (const char *)memchr(name, ':', (size_t)(end - name));
        *local = colon != NULL ? colon + 1 : name;
        *local_length = (size_t)(end - *local);
        return p;
    }
    return NULL;
}

inline bool local_name_is(const char *local, size_t length, const char *name)
{
    return strlen(name) == length && memcmp(local, name, length) == 0;
}

/* The trimmed text of the first element with this local name, any prefix, up
 * to the next tag. Entities aren't decoded: the values read here (names,
 * tokens, base64, timestamps) don't have any. */
inline bool element_text(const char *xml, const char *name, char *out, size_t capacity)
{
    const char *local;
    size_t length;
    for (const char *p = next_start_tag(xml, &local, &length); p != NULL;
         p = next_start_tag(p + 1, &local, &length)) {
        if (!local_name_is(local, length, name)) continue;
        const char *close = strchr(local + length, '>');
        if (close == NULL || capacity == 0U) return false;
        const char *text = close + 1;
        const char *stop = close[-1] == '/' ? text : strchr(text, '<');
        if (stop == NULL) return false;
        while (text < stop && isspace((unsigned char)*text)) text++;
        while (stop > text && isspace((unsigned char)stop[-1])) stop--;
        if ((size_t)(stop - text) >= capacity) return false;
        memcpy(out, text, (size_t)(stop - text));
        out[stop - text] = '\0';
        return true;
    }
    return false;
}

/* The operation: the local name of the first element in the SOAP Body. */
inline bool body_operation(const char *xml, char *out, size_t capacity)
{
    const char *local;
    size_t length;
    for (const char *p = next_start_tag(xml, &local, &length); p != NULL;
         p = next_start_tag(p + 1, &local, &length)) {
        if (!local_name_is(local, length, "Body")) continue;
        const char *close = strchr(local + length, '>');
        if (close == NULL || close[-1] == '/') return false;
        const char *next = strchr(close + 1, '<');
        if (next == NULL || next[1] == '/') return false;
        if (next_start_tag(next, &local, &length) != next || length == 0U || length >= capacity) return false;
        memcpy(out, local, length);
        out[length] = '\0';
        return true;
    }
    return false;
}

enum class Auth { ok, missing, bad };

/* WS-Security UsernameToken, PasswordDigest = Base64(SHA-1(nonce + created +
 * password)). Created isn't compared with the clock, which may not be set. */
inline Auth check_username_token(const char *xml, const char *user, const char *password)
{
    char username[64], digest[64], nonce[96], created[64];
    if (!element_text(xml, "Username", username, sizeof(username))) return Auth::missing;
    if (!element_text(xml, "Password", digest, sizeof(digest)) ||
        !element_text(xml, "Nonce", nonce, sizeof(nonce)) ||
        !element_text(xml, "Created", created, sizeof(created)) ||
        strcmp(username, user) != 0) return Auth::bad;
    unsigned char material[64 + sizeof(created) + 128];
    size_t used = 0;
    const size_t created_length = strlen(created);
    const size_t password_length = strlen(password);
    if (!base64_decode(nonce, material, 64, &used) || password_length > 128U) return Auth::bad;
    memcpy(material + used, created, created_length);
    used += created_length;
    memcpy(material + used, password, password_length);
    used += password_length;
    unsigned char hash[20];
    char expected[32];
    sha1(material, used, hash);
    base64_encode(hash, sizeof(hash), expected, sizeof(expected));
    return strcmp(expected, digest) == 0 ? Auth::ok : Auth::bad;
}

/* The MessageID of a WS-Discovery Probe; false for any other message. */
inline bool probe_message_id(const char *xml, char *out, size_t capacity)
{
    static const char suffix[] = "/discovery/Probe";
    char action[160];
    if (!element_text(xml, "Action", action, sizeof(action))) return false;
    const size_t length = strlen(action);
    if (length < sizeof(suffix) - 1U || strcmp(action + length - (sizeof(suffix) - 1U), suffix) != 0) return false;
    return element_text(xml, "MessageID", out, capacity);
}

}  // namespace apc_onvif
