#include "../APC_ONVIF.h"
#include <cassert>
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
}

int main()
{
    test_sha1_base64();
    test_xml();
    test_username_token();
    test_probe();
    puts("test_onvif: ok");
    return 0;
}
