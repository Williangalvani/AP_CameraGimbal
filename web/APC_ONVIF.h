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
        const char *end = name + strcspn(name, " \t\r\n/><");
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
 * password)). Created isn't compared with the clock, which may not be set.
 * The fields are read from the token on, so a wsu:Timestamp's Created before
 * it can't stand in for the token's. */
inline Auth check_username_token(const char *xml, const char *user, const char *password)
{
    const char *local;
    size_t length;
    const char *token = next_start_tag(xml, &local, &length);
    while (token != NULL && !local_name_is(local, length, "UsernameToken")) {
        token = next_start_tag(token + 1, &local, &length);
    }
    char username[64], digest[64], nonce[96], created[64];
    if (token == NULL || !element_text(token, "Username", username, sizeof(username))) return Auth::missing;
    if (!element_text(token, "Password", digest, sizeof(digest)) ||
        !element_text(token, "Nonce", nonce, sizeof(nonce)) ||
        !element_text(token, "Created", created, sizeof(created)) ||
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
    const size_t expected_length = base64_encode(hash, sizeof(hash), expected, sizeof(expected));
    if (expected_length == 0U || strlen(digest) != expected_length) return Auth::bad;
    unsigned char difference = 0;
    for (size_t i = 0; i < expected_length; i++) difference |= (unsigned char)(expected[i] ^ digest[i]);
    return difference == 0U ? Auth::ok : Auth::bad;
}

/* The MessageID of a WS-Discovery Probe; false for any other message or an
 * empty MessageID. */
inline bool probe_message_id(const char *xml, char *out, size_t capacity)
{
    static const char suffix[] = "/discovery/Probe";
    char action[160];
    if (!element_text(xml, "Action", action, sizeof(action))) return false;
    const size_t length = strlen(action);
    if (length < sizeof(suffix) - 1U || strcmp(action + length - (sizeof(suffix) - 1U), suffix) != 0) return false;
    return element_text(xml, "MessageID", out, capacity) && out[0] != '\0';
}

/* Whether a Probe's Types ask for this camera: none (MCM sends an empty
 * Types), or a QName whose local part is NetworkVideoTransmitter or Device.
 * The namespaces aren't checked. */
inline bool probe_types_match(const char *xml)
{
    const char *local;
    size_t length;
    const char *p = next_start_tag(xml, &local, &length);
    while (p != NULL && !local_name_is(local, length, "Types")) p = next_start_tag(p + 1, &local, &length);
    if (p == NULL) return true;
    const char *close = strchr(local + length, '>');
    if (close == NULL) return false;
    if (close[-1] == '/') return true;
    const char *stop = strchr(close + 1, '<');
    if (stop == NULL) return false;
    bool empty = true;
    for (const char *q = close + 1; q < stop;) {
        while (q < stop && isspace((unsigned char)*q)) q++;
        if (q == stop) break;
        empty = false;
        const char *name = q;
        for (; q < stop && !isspace((unsigned char)*q); q++)
            if (*q == ':') name = q + 1;
        const size_t name_length = (size_t)(q - name);
        if (local_name_is(name, name_length, "NetworkVideoTransmitter") || local_name_is(name, name_length, "Device"))
            return true;
    }
    return empty;
}

inline void element(APC_StringBuffer &out, const char *tag, const char *text)
{
    out.appendf("<%s>", tag);
    out.append_html(text);
    out.appendf("</%s>", tag);
}

inline void envelope_begin(APC_StringBuffer &out)
{
    out.append("<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
               "<s:Envelope xmlns:s=\"http://www.w3.org/2003/05/soap-envelope\""
               " xmlns:ter=\"http://www.onvif.org/ver10/error\""
               " xmlns:tds=\"http://www.onvif.org/ver10/device/wsdl\""
               " xmlns:trt=\"http://www.onvif.org/ver10/media/wsdl\""
               " xmlns:tt=\"http://www.onvif.org/ver10/schema\"><s:Body>");
}

inline void envelope_end(APC_StringBuffer &out)
{
    out.append("</s:Body></s:Envelope>");
}

inline void fault(APC_StringBuffer &out, const char *code, const char *subcode, const char *reason)
{
    envelope_begin(out);
    out.appendf("<s:Fault><s:Code><s:Value>s:%s</s:Value><s:Subcode><s:Value>ter:%s</s:Value>"
                "</s:Subcode></s:Code><s:Reason><s:Text xml:lang=\"en\">", code, subcode);
    out.append_html(reason);
    out.append("</s:Text></s:Reason></s:Fault>");
    envelope_end(out);
}

struct DeviceInformation {
    const char *manufacturer;
    const char *model;
    const char *firmware_version;
    const char *serial_number;
    const char *hardware_id;
};

inline void device_information(APC_StringBuffer &out, const DeviceInformation &info)
{
    envelope_begin(out);
    out.append("<tds:GetDeviceInformationResponse>");
    element(out, "tds:Manufacturer", info.manufacturer);
    element(out, "tds:Model", info.model);
    element(out, "tds:FirmwareVersion", info.firmware_version);
    element(out, "tds:SerialNumber", info.serial_number);
    element(out, "tds:HardwareId", info.hardware_id);
    out.append("</tds:GetDeviceInformationResponse>");
    envelope_end(out);
}

/* Device management and media share one endpoint: MCM requires every service
 * address to be within the one it discovered. */
inline void services(APC_StringBuffer &out, const char *xaddr)
{
    static const char *const namespaces[] = {
        "http://www.onvif.org/ver10/device/wsdl", "http://www.onvif.org/ver10/media/wsdl"};
    envelope_begin(out);
    out.append("<tds:GetServicesResponse>");
    for (const char *name : namespaces) {
        out.append("<tds:Service>");
        element(out, "tds:Namespace", name);
        element(out, "tds:XAddr", xaddr);
        out.append("<tds:Version><tt:Major>2</tt:Major><tt:Minor>0</tt:Minor></tds:Version></tds:Service>");
    }
    out.append("</tds:GetServicesResponse>");
    envelope_end(out);
}

struct Profile {
    const char *token;  // "main" or "sub", also the names
    unsigned width;
    unsigned height;
    unsigned frame_rate;
    unsigned bitrate_kbps;
    bool h265;
};

/* MCM skips profiles without a video encoder configuration, and takes the
 * codec from the RTSP stream itself. */
inline void profiles(APC_StringBuffer &out, const Profile *items, size_t count)
{
    envelope_begin(out);
    out.append("<trt:GetProfilesResponse>");
    for (size_t i = 0; i < count; i++) {
        const Profile &p = items[i];
        out.appendf("<trt:Profiles token=\"%s\" fixed=\"true\"><tt:Name>%s</tt:Name>"
                    "<tt:VideoEncoderConfiguration token=\"%s_encoder\"><tt:Name>%s</tt:Name>"
                    "<tt:UseCount>1</tt:UseCount><tt:Encoding>%s</tt:Encoding>"
                    "<tt:Resolution><tt:Width>%u</tt:Width><tt:Height>%u</tt:Height></tt:Resolution>"
                    "<tt:Quality>5</tt:Quality><tt:RateControl><tt:FrameRateLimit>%u</tt:FrameRateLimit>"
                    "<tt:EncodingInterval>1</tt:EncodingInterval><tt:BitrateLimit>%u</tt:BitrateLimit>"
                    "</tt:RateControl><tt:Multicast><tt:Address><tt:Type>IPv4</tt:Type>"
                    "<tt:IPv4Address>0.0.0.0</tt:IPv4Address></tt:Address><tt:Port>0</tt:Port>"
                    "<tt:TTL>0</tt:TTL><tt:AutoStart>false</tt:AutoStart></tt:Multicast>"
                    "<tt:SessionTimeout>PT60S</tt:SessionTimeout></tt:VideoEncoderConfiguration></trt:Profiles>",
                    p.token, p.token, p.token, p.token, p.h265 ? "H265" : "H264", p.width, p.height,
                    p.frame_rate, p.bitrate_kbps);
    }
    out.append("</trt:GetProfilesResponse>");
    envelope_end(out);
}

inline void stream_uri(APC_StringBuffer &out, const char *uri)
{
    envelope_begin(out);
    out.append("<trt:GetStreamUriResponse><trt:MediaUri>");
    element(out, "tt:Uri", uri);
    out.append("<tt:InvalidAfterConnect>false</tt:InvalidAfterConnect>"
               "<tt:InvalidAfterReboot>false</tt:InvalidAfterReboot>"
               "<tt:Timeout>PT0S</tt:Timeout></trt:MediaUri></trt:GetStreamUriResponse>");
    envelope_end(out);
}

inline void system_date_and_time(APC_StringBuffer &out, const struct tm &utc)
{
    envelope_begin(out);
    out.appendf("<tds:GetSystemDateAndTimeResponse><tds:SystemDateAndTime>"
                "<tt:DateTimeType>Manual</tt:DateTimeType><tt:DaylightSavings>false</tt:DaylightSavings>"
                "<tt:TimeZone><tt:TZ>UTC0</tt:TZ></tt:TimeZone><tt:UTCDateTime>"
                "<tt:Time><tt:Hour>%d</tt:Hour><tt:Minute>%d</tt:Minute><tt:Second>%d</tt:Second></tt:Time>"
                "<tt:Date><tt:Year>%d</tt:Year><tt:Month>%d</tt:Month><tt:Day>%d</tt:Day></tt:Date>"
                "</tt:UTCDateTime></tds:SystemDateAndTime></tds:GetSystemDateAndTimeResponse>",
                utc.tm_hour, utc.tm_min, utc.tm_sec, utc.tm_year + 1900, utc.tm_mon + 1, utc.tm_mday);
    envelope_end(out);
}

/* The vendor's scopes: 4K Cam Manager keeps devices named "hd" with hardware
 * HW0100302 or HW20200610. */
inline void probe_match(APC_StringBuffer &out, const char *message_id, const char *relates_to,
                        const char *endpoint, const char *xaddr)
{
    out.append("<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
               "<s:Envelope xmlns:s=\"http://www.w3.org/2003/05/soap-envelope\""
               " xmlns:a=\"http://schemas.xmlsoap.org/ws/2004/08/addressing\""
               " xmlns:d=\"http://schemas.xmlsoap.org/ws/2005/04/discovery\""
               " xmlns:dn=\"http://www.onvif.org/ver10/network/wsdl\"><s:Header>");
    element(out, "a:MessageID", message_id);
    element(out, "a:RelatesTo", relates_to);
    out.append("<a:To>http://schemas.xmlsoap.org/ws/2004/08/addressing/role/anonymous</a:To>"
               "<a:Action>http://schemas.xmlsoap.org/ws/2005/04/discovery/ProbeMatches</a:Action>"
               "</s:Header><s:Body><d:ProbeMatches><d:ProbeMatch><a:EndpointReference>");
    element(out, "a:Address", endpoint);
    out.append("</a:EndpointReference><d:Types>dn:NetworkVideoTransmitter</d:Types><d:Scopes>"
               "onvif://www.onvif.org/type/video_encoder onvif://www.onvif.org/hardware/HW0100302 "
               "onvif://www.onvif.org/name/hd onvif://www.onvif.org/Profile/Streaming "
               "onvif://www.onvif.org/location/country</d:Scopes>");
    element(out, "d:XAddrs", xaddr);
    out.append("<d:MetadataVersion>1</d:MetadataVersion></d:ProbeMatch></d:ProbeMatches></s:Body></s:Envelope>");
}

/* The camera's WS-Discovery endpoint, stable across boots: a fixed prefix and
 * the MAC as the node field. */
inline void uuid_from_mac(const unsigned char mac[6], char out[37])
{
    snprintf(out, 37, "41504341-4d00-4000-8000-%02x%02x%02x%02x%02x%02x",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

}  // namespace apc_onvif
