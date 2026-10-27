#include "../APC_ONVIF.h"
#include <cassert>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>

static std::string fixture(const char *name)
{
    std::string path = std::string("tests/fixtures/onvif/") + name;
    FILE *file = fopen(path.c_str(), "rb");
    assert(file != NULL);
    std::string text;
    char buffer[4096];
    size_t got;
    while ((got = fread(buffer, 1, sizeof(buffer), file)) > 0) text.append(buffer, got);
    fclose(file);
    return text;
}

static std::string sha1_hex(const char *text)
{
    unsigned char digest[20];
    char hex[41];
    apc_onvif::sha1(text, strlen(text), digest);
    for (int i = 0; i < 20; i++) snprintf(hex + 2 * i, 3, "%02x", digest[i]);
    return hex;
}

static void test_sha1_base64()
{
    assert(sha1_hex("") == "da39a3ee5e6b4b0d3255bfef95601890afd80709");
    assert(sha1_hex("abc") == "a9993e364706816aba3e25717850c26c9cd0d89d");
    // 56 bytes: the length no longer fits the first padding block
    assert(sha1_hex("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq") ==
           "84983e441c3bd26ebaae4aa1f95129e5e54670f1");
    // 112 bytes: one whole block before the padding
    assert(sha1_hex("abcdefghbcdefghicdefghijdefghijkefghijklfghijklmghijklmnhijklmnoijklmnopjklmnopqklmnopqrlmnopqrsmnopqrstnopqrstu") ==
           "a49b2446a02c645bf419f995b67091253a04a259");

    char text[16];
    assert(apc_onvif::base64_encode((const unsigned char *)"Man", 3, text, sizeof(text)) == 4 && !strcmp(text, "TWFu"));
    assert(apc_onvif::base64_encode((const unsigned char *)"Ma", 2, text, sizeof(text)) == 4 && !strcmp(text, "TWE="));
    assert(apc_onvif::base64_encode((const unsigned char *)"M", 1, text, sizeof(text)) == 4 && !strcmp(text, "TQ=="));
    assert(apc_onvif::base64_encode((const unsigned char *)"Man", 3, text, 4) == 0);

    unsigned char bytes[8];
    size_t length = 0;
    assert(apc_onvif::base64_decode("TWE=", bytes, sizeof(bytes), &length) && length == 2 && !memcmp(bytes, "Ma", 2));
    assert(apc_onvif::base64_decode("TW\nFu", bytes, sizeof(bytes), &length) && length == 3 && !memcmp(bytes, "Man", 3));
    assert(!apc_onvif::base64_decode("TW*u", bytes, sizeof(bytes), &length));
    assert(!apc_onvif::base64_decode("TWFuTWFuTWFu", bytes, sizeof(bytes), &length));
}

static void test_xml()
{
    char text[64];
    const char *xml = "<a:Root><a:UsernameToken><a:Username> admin </a:Username>"
                      "<a:Password Type=\"x#PasswordDigest\">abc=</a:Password><Empty/></a:UsernameToken></a:Root>";
    assert(apc_onvif::element_text(xml, "Username", text, sizeof(text)) && !strcmp(text, "admin"));
    assert(apc_onvif::element_text(xml, "Password", text, sizeof(text)) && !strcmp(text, "abc="));
    assert(apc_onvif::element_text(xml, "Empty", text, sizeof(text)) && !strcmp(text, ""));
    assert(!apc_onvif::element_text(xml, "Nonce", text, sizeof(text)));
    assert(!apc_onvif::element_text(xml, "Username", text, 3));

    assert(apc_onvif::body_operation(fixture("GetDeviceInformation.xml").c_str(), text, sizeof(text)) &&
           !strcmp(text, "GetDeviceInformation"));
    assert(apc_onvif::body_operation(fixture("GetStreamUri.xml").c_str(), text, sizeof(text)) &&
           !strcmp(text, "GetStreamUri"));
    assert(apc_onvif::body_operation("<s:Envelope><s:Body>\n <tds:GetServices><x/></tds:GetServices></s:Body></s:Envelope>",
                                     text, sizeof(text)) && !strcmp(text, "GetServices"));
    assert(!apc_onvif::body_operation("<s:Envelope><s:Body/></s:Envelope>", text, sizeof(text)));
    assert(!apc_onvif::body_operation("<s:Envelope><s:Body></s:Body></s:Envelope>", text, sizeof(text)));
}

static void test_username_token()
{
    // ONVIF Application Programmer's Guide, 6.1.1.3
    const char *guide = "<wsse:UsernameToken><wsse:Username>admin</wsse:Username>"
                        "<wsse:Password Type=\"#PasswordDigest\">tuOSpGlFlIXsozq4HFNeeGeFLEI=</wsse:Password>"
                        "<wsse:Nonce>LKqI6G/AikKCQrN0zqZFlg==</wsse:Nonce>"
                        "<wsu:Created>2010-09-16T07:50:45Z</wsu:Created></wsse:UsernameToken>";
    assert(apc_onvif::check_username_token(guide, "admin", "userpassword") == apc_onvif::Auth::ok);
    assert(apc_onvif::check_username_token(guide, "admin", "userpassworD") == apc_onvif::Auth::bad);
    assert(apc_onvif::check_username_token(guide, "operator", "userpassword") == apc_onvif::Auth::bad);
    const std::string stamped =
        std::string("<wsu:Timestamp><wsu:Created>2010-09-16T07:50:44Z</wsu:Created></wsu:Timestamp>") + guide;
    assert(apc_onvif::check_username_token(stamped.c_str(), "admin", "userpassword") == apc_onvif::Auth::ok);

    const std::string mcm = fixture("GetDeviceInformation.xml");
    assert(apc_onvif::check_username_token(mcm.c_str(), "admin", "blue") == apc_onvif::Auth::ok);
    assert(apc_onvif::check_username_token(mcm.c_str(), "admin", "red") == apc_onvif::Auth::bad);
    assert(apc_onvif::check_username_token("<s:Envelope><s:Body><tds:GetServices/></s:Body></s:Envelope>",
                                           "admin", "blue") == apc_onvif::Auth::missing);
}

static void test_probe()
{
    char id[128];
    assert(apc_onvif::probe_message_id(fixture("Probe.xml").c_str(), id, sizeof(id)) &&
           !strcmp(id, "uuid:00000000-0000-4000-8000-000000000000"));
    assert(!apc_onvif::probe_message_id(
        "<s:Envelope><s:Header><a:MessageID>uuid:1</a:MessageID>"
        "<a:Action>http://schemas.xmlsoap.org/ws/2005/04/discovery/ProbeMatches</a:Action></s:Header></s:Envelope>",
        id, sizeof(id)));
    assert(!apc_onvif::probe_message_id(
        "<s:Envelope><s:Header><a:MessageID> </a:MessageID>"
        "<a:Action>http://schemas.xmlsoap.org/ws/2005/04/discovery/Probe</a:Action></s:Header></s:Envelope>",
        id, sizeof(id)));

    // MCM's probe has empty Types
    assert(apc_onvif::probe_types_match(fixture("Probe.xml").c_str()));
    static const char *const wanted[] = {
        "<s:Body><d:Probe/></s:Body>",
        "<s:Body><d:Probe><d:Types/></d:Probe></s:Body>",
        "<s:Body><d:Probe><d:Types> \n </d:Types></d:Probe></s:Body>",
        "<s:Body><d:Probe><d:Types>dn:NetworkVideoTransmitter</d:Types></d:Probe></s:Body>",
        "<s:Body><d:Probe><d:Types>tds:Device</d:Types></d:Probe></s:Body>",
        "<s:Body><d:Probe><d:Types>wsdp:Printer\tNetworkVideoTransmitter</d:Types></d:Probe></s:Body>",
    };
    for (const char *xml : wanted) assert(apc_onvif::probe_types_match(xml));
    static const char *const unwanted[] = {
        "<s:Body><d:Probe><d:Types>wsdp:Printer</d:Types></d:Probe></s:Body>",
        "<s:Body><d:Probe><d:Types>dn:NetworkVideoTransmitters dn:Devices</d:Types></d:Probe></s:Body>",
        "<s:Body><d:Probe><d:Types>dn:NetworkVideoTransmitter",
    };
    for (const char *xml : unwanted) assert(!apc_onvif::probe_types_match(xml));
}

static void test_hostile()
{
    // Requests are parsed before authentication, so the scan must stay linear.
    const std::string angles(256 * 1024, '<');
    char text[64];
    const auto start = std::chrono::steady_clock::now();
    assert(!apc_onvif::body_operation(angles.c_str(), text, sizeof(text)));
    assert(apc_onvif::check_username_token(angles.c_str(), "admin", "blue") == apc_onvif::Auth::missing);
    assert(apc_onvif::probe_types_match(angles.c_str()));
    assert(std::chrono::steady_clock::now() - start < std::chrono::seconds(2));
}

static bool contains(const APC_StringBuffer &text, const char *part)
{
    return strstr(text.data(), part) != NULL;
}

static const apc_onvif::Profile test_profiles[] = {
    {"main", 3840, 2160, 30, 12000, true},
    {"sub", 1280, 720, 30, 2048, false},
};

static void test_replies()
{
    APC_StringBuffer out;
    apc_onvif::fault(out, "Sender", "NotAuthorized", "Sender not authorized");
    assert(contains(out, "<s:Value>s:Sender</s:Value><s:Subcode><s:Value>ter:NotAuthorized</s:Value>"));

    out.reset();
    apc_onvif::device_information(out, {"RadCam", "HDIPC", "abc123<", "001122AABBCC", "IPCamera"});
    assert(contains(out, "<tds:Manufacturer>RadCam</tds:Manufacturer><tds:Model>HDIPC</tds:Model>"));
    assert(contains(out, "<tds:FirmwareVersion>abc123&lt;</tds:FirmwareVersion>"));
    assert(contains(out, "<tds:HardwareId>IPCamera</tds:HardwareId>"));

    out.reset();
    apc_onvif::services(out, "http://192.168.2.10/onvif/device_service");
    assert(contains(out, "<tds:Namespace>http://www.onvif.org/ver10/media/wsdl</tds:Namespace>"
                         "<tds:XAddr>http://192.168.2.10/onvif/device_service</tds:XAddr>"));

    out.reset();
    apc_onvif::profiles(out, test_profiles, 2);
    assert(contains(out, "<trt:Profiles token=\"main\" fixed=\"true\">"));
    assert(contains(out, "<tt:Encoding>H265</tt:Encoding><tt:Resolution><tt:Width>3840</tt:Width>"));
    assert(contains(out, "<tt:FrameRateLimit>30</tt:FrameRateLimit>"));
    assert(contains(out, "<trt:Profiles token=\"sub\" fixed=\"true\">"));

    out.reset();
    apc_onvif::stream_uri(out, "rtsp://192.168.2.10:8554/video1");
    assert(contains(out, "<tt:Uri>rtsp://192.168.2.10:8554/video1</tt:Uri>"));

    out.reset();
    struct tm utc = {};
    utc.tm_year = 126;
    utc.tm_mon = 9;
    utc.tm_mday = 27;
    utc.tm_hour = 13;
    apc_onvif::system_date_and_time(out, utc);
    assert(contains(out, "<tt:Year>2026</tt:Year><tt:Month>10</tt:Month><tt:Day>27</tt:Day>"));

    out.reset();
    apc_onvif::probe_match(out, "uuid:reply", "uuid:00000000-0000-4000-8000-000000000000",
                           "urn:uuid:41504341-4d00-4000-8000-001122aabbcc",
                           "http://192.168.2.10/onvif/device_service");
    assert(contains(out, "<a:RelatesTo>uuid:00000000-0000-4000-8000-000000000000</a:RelatesTo>"));
    assert(contains(out, "onvif://www.onvif.org/name/hd"));
    assert(contains(out, "onvif://www.onvif.org/hardware/HW0100302"));

    char uuid[37];
    const unsigned char mac[6] = {0x00, 0x11, 0x22, 0xaa, 0xbb, 0xcc};
    apc_onvif::uuid_from_mac(mac, uuid);
    assert(!strcmp(uuid, "41504341-4d00-4000-8000-001122aabbcc"));
}

static void write_reply(const char *dir, const char *kind, const APC_StringBuffer &text)
{
    std::string path = std::string(dir) + "/" + kind + ".xml";
    FILE *file = fopen(path.c_str(), "wb");
    assert(file != NULL && fwrite(text.data(), 1, text.size(), file) == text.size() && fclose(file) == 0);
}

static void write_replies(const char *dir)
{
    APC_StringBuffer out;
    apc_onvif::probe_match(out, "uuid:reply", "uuid:00000000-0000-4000-8000-000000000000",
                           "urn:uuid:41504341-4d00-4000-8000-001122aabbcc",
                           "http://192.168.2.10/onvif/device_service");
    write_reply(dir, "probe", out);
    out.reset();
    apc_onvif::device_information(out, {"RadCam", "HDIPC", "abc123", "001122AABBCC", "IPCamera"});
    write_reply(dir, "device-information", out);
    out.reset();
    apc_onvif::services(out, "http://192.168.2.10/onvif/device_service");
    write_reply(dir, "services", out);
    out.reset();
    apc_onvif::profiles(out, test_profiles, 2);
    write_reply(dir, "profiles", out);
    out.reset();
    apc_onvif::stream_uri(out, "rtsp://192.168.2.10:8554/video1");
    write_reply(dir, "stream-uri", out);
    out.reset();
    apc_onvif::fault(out, "Sender", "NotAuthorized", "Sender not authorized");
    write_reply(dir, "not-authorized", out);
}

int main(int argc, char **argv)
{
    test_sha1_base64();
    test_xml();
    test_username_token();
    test_probe();
    test_hostile();
    test_replies();
    if (argc == 2) write_replies(argv[1]);
    puts("test_onvif: ok");
    return 0;
}
