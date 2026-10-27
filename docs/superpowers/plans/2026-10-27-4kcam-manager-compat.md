# 4K Cam Manager Compatibility Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** The stock 4K Cam Manager BlueOS extension and MAVLink Camera Manager (MCM) find, stream and control the Hi3516DV500 camera running this firmware, as they do with the vendor's `xipc`.

**Architecture:** Everything lives in `hisi-web`. Two header-only modules hold the protocol logic and have host unit tests: `web/APC_ONVIF.h` (WS-Discovery, UsernameToken digest, SOAP replies) and `web/APC_FourKCam.h` (the `/action` JSON protocol and its mapping onto `camera.ini`). `mt11-web.cpp` wires them to two unauthenticated routes, `POST /onvif/device_service` and `POST /action/<name>`, and to a forked WS-Discovery responder. All of it is behind `APCAM_HAVE_4KCAM_COMPAT`, which only `target_hisi.h` sets. Settings change the way the web's parameter form changes them: rewrite `camera.ini`, which camera-app polls every 500 ms.

**Tech Stack:** C++ (hisi-web, header-only modules), host `c++` for unit tests, a small Rust crate pinned to MCM's `onvif-rs` revision for schema checks (built with cargo on willpc), Python 3 stdlib for the on-camera check.

## Global Constraints

- Spec: `docs/4kcam-manager-compat.md`. Where this plan differs, Task 9 updates the spec.
- Only `target_hisi.h` sets `APCAM_HAVE_4KCAM_COMPAT 1`; every other target builds unchanged with 0.
- Identity: manufacturer `RadCam`, model `HDIPC`, hardware ID `IPCamera`. MCM names the source `HDIPC - RadCam (IPCamera)`.
- Discovery scopes: `onvif://www.onvif.org/type/video_encoder`, `onvif://www.onvif.org/hardware/HW0100302`, `onvif://www.onvif.org/name/hd`, `onvif://www.onvif.org/Profile/Streaming`, `onvif://www.onvif.org/location/country`; type `dn:NetworkVideoTransmitter`; XAddrs `http://<address>/onvif/device_service`.
- Namespaces: SOAP `http://www.w3.org/2003/05/soap-envelope`, discovery `http://schemas.xmlsoap.org/ws/2005/04/discovery`, addressing `http://schemas.xmlsoap.org/ws/2004/08/addressing`, `tds` `http://www.onvif.org/ver10/device/wsdl`, `trt` `http://www.onvif.org/ver10/media/wsdl`, `tt` `http://www.onvif.org/ver10/schema`, `ter` `http://www.onvif.org/ver10/error`.
- ONVIF user `admin`; password `[compat] onvif_password`, default `blue`. A missing or bad token gets HTTP 400 with a SOAP fault whose subcode is `ter:NotAuthorized` (onvif-rs then retries with a UsernameToken). `GetSystemDateAndTime` needs no token.
- Settings: `[compat] fourkcam_manager` (web **Allow 4K Cam Manager**, default `true`), `[compat] onvif_password` (**ONVIF password**, default `blue`), `[image] wb_water` (**One-push water**, `green`/`blue`, default `green`). With the setting off, `/onvif/*` and `/action/*` return 404 and discovery doesn't answer.
- `/action` replies: HTTP 200, flat JSON, the fields plus `code` (0, or −1 on failure), `device_mac`, `device_id`, `device_ip`, `log`, `sign_tby`. A field the request sends with the value reads report changes nothing.
- RTSP: `rtsp://<address>:<CAMERA_APP_RTSP_PORT or 8554>/video1` (main) and `/video2` (sub).
- Camera safety: test in RAM only (bind-mount `hisi-web`, never flash), never `rmmod ot_wdt`, never `kill -9 camera-app` (stale ISP state forces a reboot), restart camera-app only through the web's restart path. The serial console is read-only. Another agent owns the main checkout and `hisi-backend`: work only in `.worktrees/hisi-next`, don't edit `HANDOFF.md`, and don't push without the owner's OK.

## Environment

- Worktree: `/Users/will/git/4kcamera_works/tools/AP_CameraGimbal/.worktrees/hisi-next` (branch `hisi-next`). All paths below are relative to it.
- Cross-build hisi-web: `PATH=/opt/homebrew/opt/llvm/bin:$PATH make -C web CAMERA_BACKEND=hisi HISI_CROSS_COMPILE=/Users/will/git/4kcamera_works/tools/hisi-clang/bin/aarch64-linux- hisi-web`
- Camera `192.168.2.10` (telnet through `/Users/will/git/4kcamera_works/tools/camsh.py`); the Mac is `192.168.2.15` on that LAN. `camera.ini` is `/date/ap_cameragimbal/camera.ini`.
- willpc: `will@192.168.15.108`, Linux with `g++`, `make`, `python3`, `rsync`, `cargo`. It is not on the camera's LAN.

**RAM deploy of hisi-web** (used by Tasks 6–8; a reboot undoes it):

```bash
mkdir -p /tmp/4kdeploy && cp web/hisi-web /tmp/4kdeploy/hisi-web
pgrep -f "http.server 8089" >/dev/null || (cd /tmp/4kdeploy && nohup python3 -m http.server 8089 --bind 192.168.2.15 >/tmp/4kdeploy/http.log 2>&1 &)
python3 /Users/will/git/4kcamera_works/tools/camsh.py -t 60 'mkdir -p /run/4kc && umount -l /appfs/ap_cameragimbal/bin/hisi-web 2>/dev/null; wget -q -O /run/4kc/hisi-web http://192.168.2.15:8089/hisi-web && chmod +x /run/4kc/hisi-web && mount --bind /run/4kc/hisi-web /appfs/ap_cameragimbal/bin/hisi-web && kill $(pidof hisi-web); sleep 3; pidof hisi-web'
```

Expected: one or more PIDs printed (service.sh respawned hisi-web from the bind-mounted binary).

---

### Task 1: ONVIF schema checker and MCM request fixtures

**Files:**
- Create: `web/tests/onvif-check/Cargo.toml`
- Create: `web/tests/onvif-check/src/main.rs`
- Create: `web/tests/onvif-check/run.sh`
- Create: `web/tests/onvif-check/samples/probe.xml`
- Create (generated): `web/tests/fixtures/onvif/{GetDeviceInformation,GetServices,GetProfiles,GetStreamUri,Probe}.xml`
- Create (generated): `web/tests/onvif-check/Cargo.lock`
- Modify: `.gitignore`

**Interfaces:**
- Produces: `run.sh HOST requests` (writes MCM's request envelopes to `web/tests/fixtures/onvif/`, with a UsernameToken for `admin`/`blue`), and `run.sh HOST check DIR [KIND...]`, where KIND is one of `probe device-information services profiles stream-uri not-authorized`, read from `DIR/<KIND>.xml`. It exits 1 if any check fails.

- [ ] **Step 1: Write the crate**

`web/tests/onvif-check/Cargo.toml`:

```toml
[package]
name = "onvif-check"
version = "0.1.0"
edition = "2021"
publish = false

[dependencies]
async-trait = "0.1"
onvif = { git = "https://github.com/lumeohq/onvif-rs", rev = "8e2408db" }
onvif-schema = { git = "https://github.com/lumeohq/onvif-rs", rev = "8e2408db", package = "schema", features = ["devicemgmt", "media"] }
tokio = { version = "1", features = ["macros", "rt"] }
yaserde = "0.7.1"
```

`web/tests/onvif-check/src/main.rs`:

```rust
//! Runs this firmware's ONVIF replies through onvif-rs at the revision MAVLink
//! Camera Manager uses, the way MCM reads them, and writes the requests MCM
//! sends (fixtures for web/tests/test_onvif.cpp).

use std::{env, fs, process::exit};

use async_trait::async_trait;
use onvif::soap::{self, auth::username_token::UsernameToken};
use onvif_schema::{
    devicemgmt, media, onvif as tt, transport,
    ws_discovery::{probe, probe_matches},
};

/// Answers every request with one saved reply, as soap::client::Client does
/// for an HTTP 200.
struct SavedReply(String);

#[async_trait]
impl transport::Transport for SavedReply {
    async fn request(&self, _message: &str) -> Result<String, transport::Error> {
        soap::unsoap(&self.0).map_err(|error| transport::Error::Protocol(format!("{error:?}")))
    }
}

fn stream_uri_request(token: &str) -> media::GetStreamUri {
    media::GetStreamUri {
        profile_token: tt::ReferenceToken(token.to_string()),
        stream_setup: tt::StreamSetup {
            stream: tt::StreamType::RtpUnicast,
            transport: tt::Transport {
                protocol: tt::TransportProtocol::Rtsp,
                tunnel: vec![],
            },
        },
    }
}

fn write_requests() {
    let token = Some(UsernameToken::new("admin", "blue", None));
    let requests = [
        ("GetDeviceInformation", yaserde::ser::to_string(&devicemgmt::GetDeviceInformation::default())),
        ("GetServices", yaserde::ser::to_string(&devicemgmt::GetServices::default())),
        ("GetProfiles", yaserde::ser::to_string(&media::GetProfiles::default())),
        ("GetStreamUri", yaserde::ser::to_string(&stream_uri_request("sub"))),
    ];
    for (name, body) in requests {
        // transport::request drops the XML declaration before wrapping
        let body: String = body.expect("serialize request").split("?>").skip(1).collect();
        let envelope = soap::soap(&body, &token).expect("wrap request");
        fs::write(format!("{name}.xml"), envelope).expect("write request");
    }
    let probe = probe::Envelope {
        header: probe::Header {
            message_id: "uuid:00000000-0000-4000-8000-000000000000".into(),
            action: "http://schemas.xmlsoap.org/ws/2005/04/discovery/Probe".into(),
            to: "urn:schemas-xmlsoap-org:ws:2005:04:discovery".into(),
        },
        ..Default::default()
    };
    fs::write("Probe.xml", yaserde::ser::to_string(&probe).expect("serialize probe")).expect("write probe");
}

async fn check(kind: &str, xml: &str) -> Result<String, String> {
    let reply = SavedReply(xml.to_string());
    match kind {
        "probe" => {
            let envelope = yaserde::de::from_str::<probe_matches::Envelope>(xml)?;
            let found = envelope
                .body
                .probe_matches
                .probe_match
                .iter()
                .find(|m| m.find_in_scopes("onvif://www.onvif.org").is_some())
                .ok_or_else(|| "no ProbeMatch with an ONVIF scope".to_string())?;
            let xaddrs: Vec<String> = found.x_addrs().iter().map(|u| u.to_string()).collect();
            Ok(format!(
                "relates_to={} address={} name={:?} hardware={:?} types={:?} xaddrs={:?}",
                envelope.header.relates_to,
                found.endpoint_reference_address(),
                found.name(),
                found.hardware(),
                found.types(),
                xaddrs
            ))
        }
        "device-information" => {
            let i = devicemgmt::get_device_information(&reply, &Default::default())
                .await
                .map_err(|e| e.to_string())?;
            Ok(format!(
                "source={} - {} ({}) firmware={} serial={}",
                i.model, i.manufacturer, i.hardware_id, i.firmware_version, i.serial_number
            ))
        }
        "services" => {
            let s = devicemgmt::get_services(&reply, &Default::default())
                .await
                .map_err(|e| e.to_string())?;
            Ok(s.service
                .iter()
                .map(|s| format!("{} {}", s.namespace, s.x_addr))
                .collect::<Vec<_>>()
                .join("\n"))
        }
        "profiles" => {
            let p = media::get_profiles(&reply, &Default::default())
                .await
                .map_err(|e| e.to_string())?;
            Ok(p.profiles
                .iter()
                .map(|p| match &p.video_encoder_configuration {
                    Some(v) => format!(
                        "{} {}x{} {:?} {:?}",
                        p.token.0,
                        v.resolution.width,
                        v.resolution.height,
                        v.rate_control.as_ref().map(|r| r.frame_rate_limit),
                        v.encoding
                    ),
                    None => format!("{} without a video encoder", p.token.0),
                })
                .collect::<Vec<_>>()
                .join("\n"))
        }
        "stream-uri" => {
            let r = media::get_stream_uri(&reply, &stream_uri_request("main"))
                .await
                .map_err(|e| e.to_string())?;
            Ok(r.media_uri.uri)
        }
        "not-authorized" => match soap::unsoap(xml) {
            Err(soap::Error::Fault(fault)) if fault.is_unauthorized() => Ok("NotAuthorized fault".into()),
            other => Err(format!("expected a NotAuthorized fault, got {other:?}")),
        },
        _ => Err(format!("unknown kind {kind}")),
    }
}

#[tokio::main(flavor = "current_thread")]
async fn main() {
    let args: Vec<String> = env::args().collect();
    let result = match args.as_slice() {
        [_, mode] if mode == "requests" => {
            write_requests();
            return;
        }
        [_, kind, file] => match fs::read_to_string(file) {
            Ok(xml) => check(kind, &xml).await,
            Err(error) => Err(format!("{file}: {error}")),
        },
        _ => Err("usage: onvif-check requests | onvif-check KIND FILE".into()),
    };
    match result {
        Ok(text) => println!("{text}"),
        Err(error) => {
            eprintln!("FAIL: {error}");
            exit(1);
        }
    }
}
```

`web/tests/onvif-check/run.sh` (mode 755):

```sh
#!/bin/sh
# Builds onvif-check on HOST (a Linux machine with cargo) and runs it there.
#   run.sh HOST requests             writes MCM's requests to ../fixtures/onvif/
#   run.sh HOST check DIR [KIND...]  checks the replies saved as DIR/<KIND>.xml
set -eu
here=$(cd "$(dirname "$0")" && pwd)
host=$1
mode=$2
rsync -a --delete --exclude target --exclude replies --exclude requests "$here/" "$host:onvif-check/"
ssh "$host" 'cd onvif-check && cargo build --release -q'
rsync -a "$host:onvif-check/Cargo.lock" "$here/Cargo.lock"
case "$mode" in
requests)
    ssh "$host" 'rm -rf onvif-check/requests && mkdir onvif-check/requests && cd onvif-check/requests && ../target/release/onvif-check requests'
    mkdir -p "$here/../fixtures/onvif"
    rsync -a "$host:onvif-check/requests/" "$here/../fixtures/onvif/"
    ;;
check)
    dir=$3
    shift 3
    kinds=${*:-probe device-information services profiles stream-uri not-authorized}
    rsync -a --delete "$dir/" "$host:onvif-check/replies/"
    ssh "$host" "cd onvif-check && status=0; for kind in $kinds; do echo \"== \$kind\"; ./target/release/onvif-check \$kind replies/\$kind.xml || status=1; done; exit \$status"
    ;;
*)
    echo "usage: $0 HOST requests | $0 HOST check DIR [KIND...]" >&2
    exit 2
    ;;
esac
```

`web/tests/onvif-check/samples/probe.xml` (the ProbeMatch from onvif-rs's own discovery test, to prove the checker works before our code exists):

```xml
<?xml version="1.0" encoding="UTF-8"?>
<SOAP-ENV:Envelope
            xmlns:SOAP-ENV="http://www.w3.org/2003/05/soap-envelope"
            xmlns:wsa="http://schemas.xmlsoap.org/ws/2004/08/addressing"
            xmlns:d="http://schemas.xmlsoap.org/ws/2005/04/discovery"
            xmlns:dn="http://www.onvif.org/ver10/network/wsdl">
    <SOAP-ENV:Header>
        <wsa:RelatesTo>uuid:84ede3de-7dec-11d0-c360-F01234567890</wsa:RelatesTo>
    </SOAP-ENV:Header>
    <SOAP-ENV:Body>
        <d:ProbeMatches>
            <d:ProbeMatch>
                <wsa:EndpointReference>
                    <wsa:Address>an address</wsa:Address>
                </wsa:EndpointReference>
                <d:Scopes>onvif://www.onvif.org/name/MyCamera2000</d:Scopes>
                <d:XAddrs>http://addr_20 http://addr_21</d:XAddrs>
            </d:ProbeMatch>
        </d:ProbeMatches>
    </SOAP-ENV:Body>
</SOAP-ENV:Envelope>
```

Append to `.gitignore` (next to the existing `/web/tests/test_string_buffer` line):

```
/web/tests/test_onvif
/web/tests/test_fourkcam
/web/tests/onvif-check/target/
/web/tests/onvif-check/replies/
```

- [ ] **Step 2: Check the checker with the sample**

Run: `chmod +x web/tests/onvif-check/run.sh && web/tests/onvif-check/run.sh will@192.168.15.108 check web/tests/onvif-check/samples probe`
Expected (first build takes a few minutes):

```
== probe
relates_to=uuid:84ede3de-7dec-11d0-c360-F01234567890 address=an address name=Some("MyCamera2000") hardware=None types=[] xaddrs=["http://addr_20/", "http://addr_21/"]
```

If the build fails on a dependency newer than onvif-rs allows, pin it in `[dependencies]` with `=x.y.z` (as onvif-rs does for `xml-rs`) and rerun.

- [ ] **Step 3: Generate the request fixtures**

Run: `web/tests/onvif-check/run.sh will@192.168.15.108 requests && ls web/tests/fixtures/onvif && grep -c PasswordDigest web/tests/fixtures/onvif/GetDeviceInformation.xml`
Expected: `GetDeviceInformation.xml GetProfiles.xml GetServices.xml GetStreamUri.xml Probe.xml`, then `1`.

- [ ] **Step 4: Commit**

```bash
git add .gitignore web/tests/onvif-check web/tests/fixtures/onvif
git commit -m "web: onvif-check, MCM's onvif-rs as a reply checker, and its requests as fixtures"
```

---

### Task 2: ONVIF parsing and authentication (`APC_ONVIF.h`, part 1)

**Files:**
- Create: `web/APC_ONVIF.h`
- Create: `web/tests/test_onvif.cpp`
- Modify: `web/Makefile` (test rule next to `tests/test_string_buffer`, and the `test:` target)

**Interfaces:**
- Consumes: `web/tests/fixtures/onvif/*.xml` (Task 1).
- Produces, in `namespace apc_onvif`:
  - `void sha1(const void *data, size_t length, unsigned char digest[20])`
  - `size_t base64_encode(const unsigned char *data, size_t length, char *out, size_t capacity)` returns the length, or 0 if `out` is too small
  - `bool base64_decode(const char *text, unsigned char *out, size_t capacity, size_t *out_length)`
  - `bool element_text(const char *xml, const char *local_name, char *out, size_t capacity)`
  - `bool body_operation(const char *xml, char *out, size_t capacity)`
  - `enum class Auth { ok, missing, bad }` and `Auth check_username_token(const char *xml, const char *user, const char *password)`
  - `bool probe_message_id(const char *xml, char *out, size_t capacity)`
  - All XML inputs are NUL-terminated.

- [ ] **Step 1: Write the failing test**

`web/tests/test_onvif.cpp`:

```cpp
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
```

Add to `web/Makefile`, after the `tests/test_string_buffer` rule:

```make
tests/test_onvif: tests/test_onvif.cpp APC_ONVIF.h APC_StringBuffer.h
	$(call progress,Linking $@)
	$(HOST_CXX) $(WEB_CXXFLAGS) -o $@ $<
```

and in the `test:` target add `tests/test_onvif` to the prerequisites and `./tests/test_onvif` after `./tests/test_string_buffer`.

- [ ] **Step 2: Run the test to verify it fails**

Run: `make -C web tests/test_onvif`
Expected: FAIL, `fatal error: '../APC_ONVIF.h' file not found`.

- [ ] **Step 3: Write the implementation**

`web/APC_ONVIF.h`:

```cpp
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
```

- [ ] **Step 4: Run the test to verify it passes**

Run: `make -C web tests/test_onvif && (cd web && ./tests/test_onvif)`
Expected: `test_onvif: ok`

- [ ] **Step 5: Commit**

```bash
git add web/APC_ONVIF.h web/tests/test_onvif.cpp web/Makefile
git commit -m "web: ONVIF request parsing and UsernameToken digest check"
```

---

### Task 3: ONVIF and WS-Discovery replies (`APC_ONVIF.h`, part 2)

**Files:**
- Modify: `web/APC_ONVIF.h` (append before the closing `}  // namespace apc_onvif`)
- Modify: `web/tests/test_onvif.cpp`

**Interfaces:**
- Produces, in `namespace apc_onvif`:
  - `void fault(APC_StringBuffer &out, const char *code, const char *subcode, const char *reason)`, where `code` is `Sender` or `Receiver` and `subcode` is the `ter:` local name.
  - `struct DeviceInformation { const char *manufacturer, *model, *firmware_version, *serial_number, *hardware_id; }` and `void device_information(APC_StringBuffer &, const DeviceInformation &)`.
  - `void services(APC_StringBuffer &, const char *xaddr)`.
  - `struct Profile { const char *token; unsigned width, height, frame_rate, bitrate_kbps; bool h265; }` and `void profiles(APC_StringBuffer &, const Profile *, size_t count)`.
  - `void stream_uri(APC_StringBuffer &, const char *uri)`.
  - `void system_date_and_time(APC_StringBuffer &, const struct tm &utc)`.
  - `void probe_match(APC_StringBuffer &, const char *message_id, const char *relates_to, const char *endpoint, const char *xaddr)`.
  - `void uuid_from_mac(const unsigned char mac[6], char out[37])`.
  - `./tests/test_onvif DIR` also writes the six replies as `DIR/<kind>.xml`, for `onvif-check`.

- [ ] **Step 1: Write the failing test**

In `web/tests/test_onvif.cpp`, add before `main`:

```cpp
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
```

Replace `main` with:

```cpp
int main(int argc, char **argv)
{
    test_sha1_base64();
    test_xml();
    test_username_token();
    test_probe();
    test_replies();
    if (argc == 2) write_replies(argv[1]);
    puts("test_onvif: ok");
    return 0;
}
```

- [ ] **Step 2: Run the test to verify it fails**

Run: `make -C web tests/test_onvif`
Expected: FAIL, `no member named 'fault' in namespace 'apc_onvif'`.

- [ ] **Step 3: Write the implementation**

Append to `web/APC_ONVIF.h`, before `}  // namespace apc_onvif`:

```cpp
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
```

- [ ] **Step 4: Run the tests to verify they pass, then check the replies against onvif-rs**

Run: `make -C web tests/test_onvif && (cd web && mkdir -p tests/onvif-check/replies && ./tests/test_onvif tests/onvif-check/replies) && web/tests/onvif-check/run.sh will@192.168.15.108 check web/tests/onvif-check/replies`
Expected:

```
test_onvif: ok
== probe
relates_to=uuid:00000000-0000-4000-8000-000000000000 address=urn:uuid:41504341-4d00-4000-8000-001122aabbcc name=Some("hd") hardware=Some("HW0100302") types=["NetworkVideoTransmitter"] xaddrs=["http://192.168.2.10/onvif/device_service"]
== device-information
source=HDIPC - RadCam (IPCamera) firmware=abc123 serial=001122AABBCC
== services
http://www.onvif.org/ver10/device/wsdl http://192.168.2.10/onvif/device_service
http://www.onvif.org/ver10/media/wsdl http://192.168.2.10/onvif/device_service
== profiles
main 3840x2160 Some(30) __Unknown__("H265")
sub 1280x720 Some(30) H264
== stream-uri
rtsp://192.168.2.10:8554/video1
== not-authorized
NotAuthorized fault
```

If a kind fails with a deserialization error, fix the element it names in `APC_ONVIF.h` (and the matching assert) and rerun.

- [ ] **Step 5: Commit**

```bash
git add web/APC_ONVIF.h web/tests/test_onvif.cpp
git commit -m "web: ONVIF and WS-Discovery replies, checked against MCM's onvif-rs"
```

---

### Task 4: The `/action` protocol (`APC_FourKCam.h`)

**Files:**
- Create: `web/APC_FourKCam.h`
- Create: `web/tests/test_fourkcam.cpp`
- Modify: `web/Makefile`

**Interfaces:**
- Produces, in `namespace apc_fourkcam`:
  - `struct Field { char key[32]; long value; bool number; }`; `bool parse_object(const char *text, size_t length, Field *fields, size_t capacity, size_t *count)`; `const Field *find(const Field *, size_t, const char *key)`, which returns number fields only.
  - Conversions: `long level_from_percent(long)`, `long percent_from_level(long)`, `unsigned red_gain(long)`, `unsigned green_blue_gain(long)`, `long red_level(unsigned)`, `long green_blue_level(unsigned)`, `unsigned exposure_time(const char *shutter)`, `unsigned shutter_denominator(long)`.
  - `struct Image` (camera.ini image state), `struct Stream` (one stream), `struct Identity { const char *mac, *id, *ip; }`.
  - `struct Changes { Update updates[16]; size_t count; bool one_push; void set(...); void set_number(...); }`, where `Update { const char *section; const char *key; char value[24]; }`.
  - Replies append `"key":value,` items: `image_adjustment(out, Image)`, `image_adjustment_ex(out, Image)`, `venc(out, channel, Stream, resolutions, count)`. `finish(out, code, log, Identity)` appends the identity and the closing `}`. The caller starts with `out.append("{")`.
  - Requests: `image_adjustment_changes(fields, count, Image, Changes&)`, `image_adjustment_ex_changes(fields, count, Image, Changes&)`, `venc_changes(fields, count, Stream, resolutions, resolution_count, section, Changes&)`.

- [ ] **Step 1: Write the failing test**

`web/tests/test_fourkcam.cpp`:

```cpp
#include "../APC_FourKCam.h"
#include <cassert>

using namespace apc_fourkcam;

static bool parse(const char *text, Field *fields, size_t *count)
{
    return parse_object(text, strlen(text), fields, 16, count);
}

static const Update *change(const Changes &changes, const char *key)
{
    for (size_t i = 0; i < changes.count; i++)
        if (!strcmp(changes.updates[i].key, key)) return &changes.updates[i];
    return NULL;
}

static void test_json()
{
    Field fields[16];
    size_t count = 99;
    assert(parse("", fields, &count) && count == 0);
    assert(parse(" \n", fields, &count) && count == 0);
    assert(parse("{}", fields, &count) && count == 0);
    assert(parse("{\"channel\":0,\"pixel_list\":[{\"width\":3840,\"height\":2160}],\"name\":\"a,}\","
                 "\"awb_red\":-3,\"flag\":true, \"rate\" : 25.5 }", fields, &count) && count == 6);
    assert(find(fields, count, "channel") && find(fields, count, "channel")->value == 0);
    assert(find(fields, count, "pixel_list") == NULL);
    assert(find(fields, count, "name") == NULL);
    assert(find(fields, count, "flag") == NULL);
    assert(find(fields, count, "awb_red")->value == -3);
    assert(find(fields, count, "rate")->value == 25);
    assert(!parse("{\"a\":}", fields, &count));
    assert(!parse("{\"a\":1", fields, &count));
    assert(!parse("[1]", fields, &count));
    assert(!parse("{\"a\":1}x", fields, &count));
}

static void test_conversions()
{
    assert(level_from_percent(50) == 128 && level_from_percent(0) == 0 && level_from_percent(100) == 255);
    assert(percent_from_level(128) == 50 && percent_from_level(204) == 80 && percent_from_level(300) == 100);
    assert(red_gain(6) == 346 && red_level(350) == 6 && red_level(500) == 16);
    assert(red_level(100) == 0 && red_level(4095) == 255);
    for (long level = 0; level <= 255; level++) {
        assert(red_level(red_gain(level)) == level);
        assert(green_blue_level(green_blue_gain(level)) == level);
    }
    assert(green_blue_gain(0) == 256 && green_blue_level(504) == 31 && green_blue_level(725) == 59);
    assert(exposure_time("auto") == 60 && exposure_time("1/250") == 200);
    assert(exposure_time("1/750") == 800 && exposure_time("1/30") == 30);
    assert(shutter_denominator(200) == 250 && shutter_denominator(800) == 750);
    assert(shutter_denominator(12) == 30 && shutter_denominator(34464) == 2000);
    static const char *const shutters[] = {"1/30", "1/50", "1/100", "1/250", "1/500", "1/750", "1/1000", "1/2000"};
    for (const char *shutter : shutters) {
        char back[16];
        snprintf(back, sizeof(back), "1/%u", shutter_denominator(exposure_time(shutter)));
        assert(!strcmp(back, shutter));
    }
}

static Image test_image()
{
    Image image;
    image.wb_red = 500;
    image.wb_green = 256;
    image.wb_blue = 504;
    return image;
}

static void test_image_changes()
{
    Field fields[16];
    size_t count;
    Image image = test_image();

    Changes changes;
    assert(parse("{\"auto_awb\":1,\"awb_red\":16,\"awb_blue\":40}", fields, &count));
    image_adjustment_changes(fields, count, image, changes);
    assert(changes.count == 2 && !strcmp(change(changes, "wb_blue")->value, "576"));
    assert(!strcmp(change(changes, "white_balance")->value, "manual"));

    changes = Changes();
    strcpy(image.white_balance, "daylight");
    assert(parse("{\"auto_awb\":1}", fields, &count));
    image_adjustment_changes(fields, count, image, changes);
    assert(changes.count == 0);
    assert(parse("{\"awb_red\":20}", fields, &count));
    image_adjustment_changes(fields, count, image, changes);
    assert(!strcmp(change(changes, "wb_red")->value, "556") && !strcmp(change(changes, "white_balance")->value, "manual"));

    changes = Changes();
    assert(parse("{\"auto_awb\":0,\"awb_red\":20}", fields, &count));
    image_adjustment_changes(fields, count, image, changes);
    assert(!strcmp(change(changes, "white_balance")->value, "auto"));

    image = test_image();
    changes = Changes();
    assert(parse("{\"brightness\":128,\"contrast\":200,\"awb_auto_mode\":1}", fields, &count));
    image_adjustment_changes(fields, count, image, changes);
    assert(changes.count == 2 && !strcmp(change(changes, "contrast")->value, "78"));
    assert(!strcmp(change(changes, "wb_water")->value, "blue"));

    changes = Changes();
    assert(parse("{\"auto_exposureEx\":1,\"exposure_time\":400}", fields, &count));
    image_adjustment_changes(fields, count, image, changes);
    assert(!strcmp(change(changes, "shutter")->value, "1/500"));
    changes = Changes();
    assert(parse("{\"exposure_time\":800}", fields, &count));
    image_adjustment_changes(fields, count, image, changes);
    assert(changes.count == 0);

    strcpy(image.shutter, "1/500");
    changes = Changes();
    assert(parse("{\"exposure_time\":400}", fields, &count));
    image_adjustment_changes(fields, count, image, changes);
    assert(changes.count == 0);
    assert(parse("{\"exposure_time\":800}", fields, &count));
    image_adjustment_changes(fields, count, image, changes);
    assert(!strcmp(change(changes, "shutter")->value, "1/750"));
    changes = Changes();
    assert(parse("{\"auto_exposureEx\":0}", fields, &count));
    image_adjustment_changes(fields, count, image, changes);
    assert(!strcmp(change(changes, "shutter")->value, "auto"));
}

static void test_image_ex_changes()
{
    Field fields[16];
    size_t count;
    Image image = test_image();
    Changes changes;
    assert(parse("{\"onceAWB\":1}", fields, &count));
    image_adjustment_ex_changes(fields, count, image, changes);
    assert(changes.one_push && changes.count == 0);

    changes = Changes();
    assert(parse("{\"mirror\":0,\"flip\":0,\"onceAWB\":0}", fields, &count));
    image_adjustment_ex_changes(fields, count, image, changes);
    assert(!changes.one_push && changes.count == 0);
    assert(parse("{\"mirror\":1}", fields, &count));
    image_adjustment_ex_changes(fields, count, image, changes);
    assert(changes.count == 0);
    assert(parse("{\"mirror\":1,\"flip\":1}", fields, &count));
    image_adjustment_ex_changes(fields, count, image, changes);
    assert(!strcmp(change(changes, "orientation")->value, "inverted"));
    assert(!strcmp(changes.updates[0].section, "mount"));
}

static const char *const main_names[] = {"3840x2160", "1920x1080", "1280x720", "960x540", "854x480"};
static const char *const sub_names[] = {"1920x1080", "1280x720", "854x480"};

static Stream main_stream()
{
    Stream stream;
    stream.width = 3840;
    stream.height = 2160;
    stream.bitrate_kbps = 12000;
    stream.configurable = true;
    stream.min_frame_rate = 5;
    stream.max_bitrate_kbps = 20000;
    return stream;
}

static void test_venc()
{
    Field fields[16];
    size_t count;
    Changes changes;
    assert(parse("{\"channel\":0,\"encode_type\":5,\"pic_width\":1920,\"pic_height\":1080,"
                 "\"bitrate\":12000,\"frame_rate\":25,\"gop\":60}", fields, &count));
    venc_changes(fields, count, main_stream(), main_names, 5, "stream.main", changes);
    assert(changes.count == 3 && !strcmp(change(changes, "codec")->value, "h265"));
    assert(!strcmp(change(changes, "resolution")->value, "1920x1080"));
    assert(!strcmp(change(changes, "frame_rate")->value, "25"));

    changes = Changes();
    assert(parse("{\"bitrate\":30000,\"frame_rate\":60}", fields, &count));
    venc_changes(fields, count, main_stream(), main_names, 5, "stream.main", changes);
    assert(changes.count == 1 && !strcmp(change(changes, "bitrate_kbps")->value, "20000"));

    Stream sub;
    sub.width = 1280;
    sub.height = 720;
    sub.bitrate_kbps = 2048;
    changes = Changes();
    assert(parse("{\"channel\":1,\"pic_width\":640,\"pic_height\":480,\"bitrate\":32,\"frame_rate\":5}", fields, &count));
    venc_changes(fields, count, sub, sub_names, 3, "stream.sub", changes);
    assert(changes.count == 1 && !strcmp(change(changes, "resolution")->value, "854x480"));
    assert(!strcmp(changes.updates[0].section, "stream.sub"));
}

static void test_replies()
{
    APC_StringBuffer out;
    out.append("{");
    image_adjustment(out, Image());
    finish(out, 0, "", {"aa-bb-cc-dd-ee-ff", "AABBCCDDEEFF", "192.168.2.10"});
    assert(strstr(out.data(), "\"brightness\":128,"));
    assert(strstr(out.data(), "\"auto_exposureEx\":0,\"exposure_time\":60,"));
    assert(strstr(out.data(), "\"auto_awb\":0,\"awb_red\":0,\"awb_green\":0,\"awb_blue\":0,\"awb_auto_mode\":0,"));
    assert(strstr(out.data(), "\"code\":0,\"device_mac\":\"aa-bb-cc-dd-ee-ff\",\"device_id\":\"AABBCCDDEEFF\","
                              "\"device_ip\":\"192.168.2.10\",\"log\":\"\",\"sign_tby\":\"\"}"));

    out.reset();
    out.append("{");
    Image inverted;
    inverted.inverted = true;
    image_adjustment_ex(out, inverted);
    venc(out, 0, main_stream(), main_names, 2);
    finish(out, -1, "say \"hi\"\\\n", {"m", "i", "p"});
    assert(strstr(out.data(), "{\"mirror\":1,\"flip\":1,\"onceAWB\":0,"));
    assert(strstr(out.data(), "\"pixel_list\":[{\"width\":3840,\"height\":2160},{\"width\":1920,\"height\":1080}],"));
    assert(strstr(out.data(), "\"pic_width\":3840,\"pic_height\":2160,\"rc_mode\":1,\"bitrate\":12000,"
                              "\"max_framerate\":30,\"frame_rate\":30,\"gop\":60,"));
    assert(strstr(out.data(), "\"code\":-1,") && strstr(out.data(), "\"log\":\"say \\\"hi\\\"\\\\\\u000a\""));
}

int main()
{
    test_json();
    test_conversions();
    test_image_changes();
    test_image_ex_changes();
    test_venc();
    test_replies();
    puts("test_fourkcam: ok");
    return 0;
}
```

Add to `web/Makefile` after the `tests/test_onvif` rule:

```make
tests/test_fourkcam: tests/test_fourkcam.cpp APC_FourKCam.h APC_StringBuffer.h
	$(call progress,Linking $@)
	$(HOST_CXX) $(WEB_CXXFLAGS) -o $@ $<
```

and add `tests/test_fourkcam` to the `test:` prerequisites and `./tests/test_fourkcam` to its commands.

- [ ] **Step 2: Run the test to verify it fails**

Run: `make -C web tests/test_fourkcam`
Expected: FAIL, `fatal error: '../APC_FourKCam.h' file not found`.

- [ ] **Step 3: Write the implementation**

`web/APC_FourKCam.h`:

```cpp
#pragma once
/* The camera protocol of Blue Robotics' 4K Cam Manager, as the vendor's xipc
 * answers it: POST /action/<name> with a flat JSON object. Names, ranges and
 * enums follow the extension's br4kcam_commands types; the settings are this
 * firmware's camera.ini values (docs/4kcam-manager-compat.md). A field sent
 * with the value reads report changes nothing, so a client echoing a whole
 * reply back doesn't move settings that don't map exactly. */
#include "APC_StringBuffer.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace apc_fourkcam {

struct Field {
    char key[32];
    long value;
    bool number;
};

inline const char *skip_space(const char *p, const char *end)
{
    while (p < end && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')) p++;
    return p;
}

/* Past the closing quote of the string starting at p, or NULL. */
inline const char *skip_string(const char *p, const char *end)
{
    for (p++; p < end; p++) {
        if (*p == '\\') {
            p++;
            continue;
        }
        if (*p == '"') return p + 1;
    }
    return NULL;
}

/* Past any JSON value starting at p, nested ones included: scalars end at the
 * next ',' or '}' of the enclosing object. NULL if it doesn't end. */
inline const char *skip_value(const char *p, const char *end)
{
    unsigned depth = 0;
    while (p < end) {
        if (*p == '"') {
            p = skip_string(p, end);
            if (p == NULL) return NULL;
            if (depth == 0U) return p;
            continue;
        }
        if (*p == '{' || *p == '[') {
            depth++;
        } else if (*p == '}' || *p == ']') {
            if (depth == 0U) return p;
            if (--depth == 0U) return p + 1;
        } else if (depth == 0U && *p == ',') {
            return p;
        }
        p++;
    }
    return depth == 0U ? p : NULL;
}

/* One flat JSON object. Numbers are kept (fractions truncated); other values
 * are skipped. An empty body is an empty object: reads have no body. */
inline bool parse_object(const char *text, size_t length, Field *fields, size_t capacity, size_t *count)
{
    const char *end = text + length;
    const char *p = skip_space(text, end);
    *count = 0;
    if (p == end) return true;
    if (*p++ != '{') return false;
    p = skip_space(p, end);
    if (p < end && *p == '}') return skip_space(p + 1, end) == end;
    while (p < end) {
        if (*p != '"') return false;
        const char *key_end = skip_string(p, end);
        if (key_end == NULL) return false;
        const char *key = p + 1;
        const size_t key_length = (size_t)(key_end - key - 1);
        p = skip_space(key_end, end);
        if (p == end || *p++ != ':') return false;
        p = skip_space(p, end);
        const char *value_end = skip_value(p, end);
        if (value_end == NULL || value_end == p) return false;
        if (*count < capacity && key_length < sizeof(fields[0].key)) {
            Field &field = fields[(*count)++];
            memcpy(field.key, key, key_length);
            field.key[key_length] = '\0';
            field.number = *p == '-' || (*p >= '0' && *p <= '9');
            field.value = field.number ? strtol(p, NULL, 10) : 0;
        }
        p = skip_space(value_end, end);
        if (p == end) return false;
        if (*p == '}') return skip_space(p + 1, end) == end;
        if (*p++ != ',') return false;
        p = skip_space(p, end);
    }
    return false;
}

inline const Field *find(const Field *fields, size_t count, const char *key)
{
    for (size_t i = 0; i < count; i++)
        if (fields[i].number && strcmp(fields[i].key, key) == 0) return &fields[i];
    return NULL;
}

inline long clamp(long value, long low, long high)
{
    return value < low ? low : value > high ? high : value;
}

/* image.brightness/contrast/saturation are 0–100; the vendor's are 0–255. */
inline long level_from_percent(long percent) { return (clamp(percent, 0, 100) * 255 + 50) / 100; }
inline long percent_from_level(long level) { return (clamp(level, 0, 255) * 100 + 127) / 255; }

/* The vendor's manual white-balance sliders (0–255) as gains in 1/256. */
inline unsigned red_gain(long level) { return (unsigned)(clamp(level, 0, 255) * 15 + 256); }
inline unsigned green_blue_gain(long level) { return (unsigned)((clamp(level, 0, 255) + 32) * 8); }
inline long red_level(unsigned gain) { return gain <= 256U ? 0 : clamp(((long)gain - 256 + 7) / 15, 0, 255); }
inline long green_blue_level(unsigned gain) { return clamp(((long)gain + 4) / 8 - 32, 0, 255); }

/* exposure_time is x in 1/x s, one of the extension's BaseExposureTimeValue. */
constexpr unsigned vendor_exposures[] = {12, 25, 30, 50, 60, 100, 200, 400, 800, 1000, 2000, 4000, 8000, 10000, 34464};
/* [image] shutter's manual choices, 1/x s. */
constexpr unsigned shutters[] = {30, 50, 100, 250, 500, 750, 1000, 2000};

template <size_t N>
inline unsigned nearest(long value, const unsigned (&choices)[N])
{
    const double wanted = (double)clamp(value, 1, 1000000);
    unsigned best = choices[0];
    double best_distance = INFINITY;
    for (unsigned choice : choices) {
        const double distance = fabs(log(choice / wanted));
        if (distance < best_distance) {
            best = choice;
            best_distance = distance;
        }
    }
    return best;
}

inline unsigned exposure_time(const char *shutter)
{
    unsigned denominator = 0;
    if (sscanf(shutter, "1/%u", &denominator) != 1 || denominator == 0U) return 60;
    return nearest((long)denominator, vendor_exposures);
}

inline unsigned shutter_denominator(long exposure) { return nearest(exposure, shutters); }

struct Image {
    long brightness = 50;
    long contrast = 50;
    long saturation = 50;
    char white_balance[16] = "auto";
    unsigned wb_red = 256;
    unsigned wb_green = 256;
    unsigned wb_blue = 256;
    bool blue_water = false;
    char shutter[16] = "auto";
    bool inverted = false;
};

struct Stream {
    unsigned width = 0;
    unsigned height = 0;
    bool h265 = false;
    unsigned bitrate_kbps = 0;  // in effect, also when camera.ini says automatic
    unsigned frame_rate = 30;
    unsigned max_frame_rate = 30;
    bool configurable = false;  // bitrate and frame rate are settings
    long min_frame_rate = 30;
    long max_bitrate_kbps = 0;
};

struct Identity {
    const char *mac;  // "aa-bb-cc-dd-ee-ff", the vendor's form
    const char *id;
    const char *ip;
};

struct Update {
    const char *section;
    const char *key;
    char value[24];
};

struct Changes {
    Update updates[16];
    size_t count = 0;
    bool one_push = false;

    void set(const char *section, const char *key, const char *value)
    {
        Update *update = NULL;
        for (size_t i = 0; i < count && update == NULL; i++)
            if (!strcmp(updates[i].section, section) && !strcmp(updates[i].key, key)) update = &updates[i];
        if (update == NULL) {
            if (count == sizeof(updates) / sizeof(updates[0])) return;
            update = &updates[count++];
            update->section = section;
            update->key = key;
        }
        snprintf(update->value, sizeof(update->value), "%s", value);
    }

    void set_number(const char *section, const char *key, long value)
    {
        char text[24];
        snprintf(text, sizeof(text), "%ld", value);
        set(section, key, text);
    }
};

inline void append_json_string(APC_StringBuffer &out, const char *text)
{
    out.append("\"");
    for (const unsigned char *p = (const unsigned char *)text; *p != '\0'; p++) {
        if (*p == '"' || *p == '\\') out.appendf("\\%c", *p);
        else if (*p < 0x20U) out.appendf("\\u%04x", *p);
        else out.append_n((const char *)p, 1);
    }
    out.append("\"");
}

inline void image_adjustment(APC_StringBuffer &out, const Image &image)
{
    out.appendf("\"brightness\":%ld,\"contrast\":%ld,\"saturation\":%ld,"
                "\"auto_exposureEx\":%d,\"exposure_time\":%u,"
                "\"auto_awb\":%d,\"awb_red\":%ld,\"awb_green\":%ld,\"awb_blue\":%ld,\"awb_auto_mode\":%d,"
                "\"antiFog\":0,\"frameTurbo_pro\":0,\"AE_strategy_mode\":0,\"rotate\":0,",
                level_from_percent(image.brightness), level_from_percent(image.contrast),
                level_from_percent(image.saturation), strcmp(image.shutter, "auto") != 0,
                exposure_time(image.shutter), strcmp(image.white_balance, "auto") != 0,
                red_level(image.wb_red), green_blue_level(image.wb_green), green_blue_level(image.wb_blue),
                image.blue_water);
}

inline void image_adjustment_ex(APC_StringBuffer &out, const Image &image)
{
    out.appendf("\"mirror\":%d,\"flip\":%d,\"onceAWB\":0,", image.inverted, image.inverted);
}

inline void venc(APC_StringBuffer &out, long channel, const Stream &stream,
                 const char *const *resolutions, size_t count)
{
    out.appendf("\"channel\":%ld,\"encode_type\":%d,\"encode_profile\":2,\"pixel_list\":[",
                channel, stream.h265 ? 5 : 1);
    bool first = true;
    for (size_t i = 0; i < count; i++) {
        unsigned width, height;
        if (sscanf(resolutions[i], "%ux%u", &width, &height) != 2) continue;
        out.appendf("%s{\"width\":%u,\"height\":%u}", first ? "" : ",", width, height);
        first = false;
    }
    out.appendf("],\"pic_width\":%u,\"pic_height\":%u,\"rc_mode\":1,\"bitrate\":%u,"
                "\"max_framerate\":%u,\"frame_rate\":%u,\"gop\":%u,",
                stream.width, stream.height, stream.bitrate_kbps, stream.max_frame_rate,
                stream.frame_rate, stream.frame_rate * 2U);
}

inline void finish(APC_StringBuffer &out, int code, const char *log, const Identity &identity)
{
    out.appendf("\"code\":%d,\"device_mac\":\"%s\",\"device_id\":\"%s\",\"device_ip\":\"%s\",\"log\":",
                code, identity.mac, identity.id, identity.ip);
    append_json_string(out, log);
    out.append(",\"sign_tby\":\"\"}");
}

inline void image_adjustment_changes(const Field *fields, size_t count, const Image &image, Changes &changes)
{
    const struct {
        const char *key;
        long percent;
    } levels[] = {{"brightness", image.brightness}, {"contrast", image.contrast}, {"saturation", image.saturation}};
    for (const auto &level : levels) {
        const Field *field = find(fields, count, level.key);
        if (field != NULL && field->value != level_from_percent(level.percent))
            changes.set_number("image", level.key, percent_from_level(field->value));
    }

    const size_t before_gains = changes.count;
    const Field *red = find(fields, count, "awb_red");
    const Field *green = find(fields, count, "awb_green");
    const Field *blue = find(fields, count, "awb_blue");
    if (red != NULL && red->value != red_level(image.wb_red))
        changes.set_number("image", "wb_red", red_gain(red->value));
    if (green != NULL && green->value != green_blue_level(image.wb_green))
        changes.set_number("image", "wb_green", green_blue_gain(green->value));
    if (blue != NULL && blue->value != green_blue_level(image.wb_blue))
        changes.set_number("image", "wb_blue", green_blue_gain(blue->value));
    const bool gains_changed = changes.count > before_gains;

    // auto_awb 1 means "not auto": a preset or colour temperature stays, unless the gains move
    const Field *awb = find(fields, count, "auto_awb");
    const bool automatic = strcmp(image.white_balance, "auto") == 0;
    if (awb != NULL && awb->value == 0) {
        if (!automatic) changes.set("image", "white_balance", "auto");
    } else if ((awb != NULL && awb->value == 1 && automatic) ||
               (gains_changed && strcmp(image.white_balance, "manual") != 0)) {
        changes.set("image", "white_balance", "manual");
    }

    const Field *water = find(fields, count, "awb_auto_mode");
    if (water != NULL && (water->value == 1) != image.blue_water)
        changes.set("image", "wb_water", water->value == 1 ? "blue" : "green");

    const Field *mode = find(fields, count, "auto_exposureEx");
    const Field *time = find(fields, count, "exposure_time");
    const bool was_manual = strcmp(image.shutter, "auto") != 0;
    const bool manual = mode != NULL ? mode->value == 1 : was_manual;
    if (!manual && was_manual) {
        changes.set("image", "shutter", "auto");
    } else if (manual) {
        const long wanted = time != NULL ? time->value : (long)exposure_time(image.shutter);
        if (!was_manual || wanted != (long)exposure_time(image.shutter)) {
            char shutter[16];
            snprintf(shutter, sizeof(shutter), "1/%u", shutter_denominator(wanted));
            changes.set("image", "shutter", shutter);
        }
    }
}

inline void image_adjustment_ex_changes(const Field *fields, size_t count, const Image &image, Changes &changes)
{
    const Field *once = find(fields, count, "onceAWB");
    if (once != NULL && once->value == 1) changes.one_push = true;
    // mount.orientation only has upright and inverted, so only mirror and flip together map
    const Field *mirror = find(fields, count, "mirror");
    const Field *flip = find(fields, count, "flip");
    const long mirrored = mirror != NULL ? mirror->value : (long)image.inverted;
    const long flipped = flip != NULL ? flip->value : (long)image.inverted;
    if ((mirror != NULL || flip != NULL) && mirrored == flipped && (mirrored == 1) != image.inverted)
        changes.set("mount", "orientation", mirrored == 1 ? "inverted" : "upright");
}

/* A resolution from the target's list: the requested size, else one of the
 * same height (the extension's recommended sub streams are 640x480). */
inline const char *pick_resolution(long width, long height, const char *const *resolutions, size_t count)
{
    for (int pass = 0; pass < 2; pass++) {
        for (size_t i = 0; i < count; i++) {
            unsigned w, h;
            if (sscanf(resolutions[i], "%ux%u", &w, &h) == 2 && (long)h == height &&
                (pass == 1 || (long)w == width)) return resolutions[i];
        }
    }
    return NULL;
}

inline void venc_changes(const Field *fields, size_t count, const Stream &stream,
                         const char *const *resolutions, size_t resolution_count,
                         const char *section, Changes &changes)
{
    const Field *encode = find(fields, count, "encode_type");
    if (encode != NULL && (encode->value == 1 || encode->value == 5) && (encode->value == 5) != stream.h265)
        changes.set(section, "codec", encode->value == 5 ? "h265" : "h264");

    const Field *width = find(fields, count, "pic_width");
    const Field *height = find(fields, count, "pic_height");
    if (width != NULL && height != NULL &&
        (width->value != (long)stream.width || height->value != (long)stream.height)) {
        const char *chosen = pick_resolution(width->value, height->value, resolutions, resolution_count);
        char current[24];
        snprintf(current, sizeof(current), "%ux%u", stream.width, stream.height);
        if (chosen != NULL && strcmp(chosen, current) != 0) changes.set(section, "resolution", chosen);
    }

    if (!stream.configurable) return;
    const Field *bitrate = find(fields, count, "bitrate");
    if (bitrate != NULL && bitrate->value != (long)stream.bitrate_kbps)
        changes.set_number(section, "bitrate_kbps", clamp(bitrate->value, 1, stream.max_bitrate_kbps));
    const Field *rate = find(fields, count, "frame_rate");
    if (rate != NULL) {
        const long value = clamp(rate->value, stream.min_frame_rate, (long)stream.max_frame_rate);
        if (value != (long)stream.frame_rate) changes.set_number(section, "frame_rate", value);
    }
}

}  // namespace apc_fourkcam
```

- [ ] **Step 4: Run the test to verify it passes**

Run: `make -C web tests/test_fourkcam && (cd web && ./tests/test_fourkcam)`
Expected: `test_fourkcam: ok`

- [ ] **Step 5: Commit**

```bash
git add web/APC_FourKCam.h web/tests/test_fourkcam.cpp web/Makefile
git commit -m "web: 4K Cam Manager's /action protocol and its camera.ini mapping"
```

---

### Task 5: Flag, settings and the shared automatic bitrate

**Files:**
- Modify: `include/apcam/target.h` (after the `APCAM_HAVE_WB_TUNING` block, ~line 62)
- Modify: `include/apcam/target_hisi.h` (~lines 49–54 and 81–82)
- Modify: `camera_app/src/backends/hisi/media.cpp:387-392` (`stream_bitrate`) and its callers
- Modify: `web/APC_Translations.h` (enum ~lines 91, 137, 259; rows ~lines 741, 787, 940)
- Modify: `web/mt11-web.cpp` (options ~line 642, parameters ~line 768, defaults ~line 885)

**Interfaces:**
- Produces: `APCAM_HAVE_4KCAM_COMPAT` (0 by default, 1 on hisi); `static inline unsigned apcam_hisi_auto_bitrate_kbps(unsigned width, unsigned height)` in `target_hisi.h`; parameters `wb_water`, `fourkcam_manager` and `onvif_password`, with defaults `green`, `true` and `blue`; string IDs `S_OPT_WATER_GREEN`, `S_OPT_WATER_BLUE`, `S_P_WB_WATER`, `S_H_WB_WATER`, `S_P_FOURKCAM`, `S_H_FOURKCAM`, `S_P_ONVIF_PASSWORD`, `S_H_ONVIF_PASSWORD`.

- [ ] **Step 1: Add the flag and the shared bitrate**

In `include/apcam/target.h`, after the `#endif` of `APCAM_HAVE_WB_TUNING`:

```c
/* Targets that answer Blue Robotics' 4K Cam Manager and MAVLink Camera Manager
 * as the vendor's firmware does: WS-Discovery, a few ONVIF operations and the
 * /action control protocol, all in the web server (docs/4kcam-manager-compat.md). */
#ifndef APCAM_HAVE_4KCAM_COMPAT
#define APCAM_HAVE_4KCAM_COMPAT 0
#endif
```

In `include/apcam/target_hisi.h`, after `#define APCAM_DEFAULT_WB_TEMP 5000`:

```c
#define APCAM_HAVE_4KCAM_COMPAT 1
```

and replace

```c
/* 0 = automatic: 12 Mbit/s at 2160p, 4 Mbit/s at 1080p, 2 Mbit/s below. */
#define APCAM_DEFAULT_MAIN_BITRATE_KBPS 0
```

with

```c
/* 0 = automatic, apcam_hisi_auto_bitrate_kbps() */
#define APCAM_DEFAULT_MAIN_BITRATE_KBPS 0
static inline unsigned apcam_hisi_auto_bitrate_kbps(unsigned width, unsigned height)
{
    if (width >= 3840U || height >= 2160U) return 12000U;
    if (width >= 1920U || height >= 1080U) return 4096U;
    return 2048U;
}
```

In `camera_app/src/backends/hisi/media.cpp`, delete `static unsigned stream_bitrate(unsigned width, unsigned height) { ... }` and replace every call `stream_bitrate(` with `apcam_hisi_auto_bitrate_kbps(`:

Run: `grep -n "stream_bitrate\|apcam_hisi_auto_bitrate_kbps" camera_app/src/backends/hisi/media.cpp`
Expected: only `apcam_hisi_auto_bitrate_kbps` lines.

- [ ] **Step 2: Add the strings**

In `web/APC_Translations.h`, add the IDs to `enum string_id`: `S_OPT_WATER_GREEN, S_OPT_WATER_BLUE,` after `S_OPT_WB_TEMPERATURE,`; `S_P_WB_WATER, S_P_FOURKCAM, S_P_ONVIF_PASSWORD,` after `S_P_WB_TEMP,`; `S_H_WB_WATER, S_H_FOURKCAM, S_H_ONVIF_PASSWORD,` after `S_H_WB_TEMP,`. Add the rows after the matching existing rows:

```cpp
    result[S_OPT_WATER_GREEN] = {"Green water", "绿色水体", "緑色の水"};
    result[S_OPT_WATER_BLUE] = {"Blue water", "蓝色水体", "青色の水"};
```

```cpp
    result[S_P_WB_WATER] = {"One-push water", "一键白平衡水色", "ワンプッシュの水の色"};
    result[S_P_FOURKCAM] = {"Allow 4K Cam Manager", "允许 4K Cam Manager", "4K Cam Manager を許可"};
    result[S_P_ONVIF_PASSWORD] = {"ONVIF password", "ONVIF 密码", "ONVIF パスワード"};
```

```cpp
    result[S_H_WB_WATER] = {"Water that 4K Cam Manager's one-push white balance measures for. The Photos page asks each time.", "4K Cam Manager 一键白平衡所针对的水色。照片页面每次单独选择。", "4K Cam Manager のワンプッシュ ホワイトバランスが対象とする水の色。写真ページでは毎回選びます。"};
    result[S_H_FOURKCAM] = {"Lets Blue Robotics' 4K Cam Manager and MAVLink Camera Manager find and control this camera without the web login, as with the original firmware.", "允许 Blue Robotics 的 4K Cam Manager 和 MAVLink Camera Manager 无需网页登录即可发现并控制本相机，与原厂固件相同。", "Blue Robotics の 4K Cam Manager と MAVLink Camera Manager が、元のファームウェアと同様に Web ログインなしでこのカメラを検出・制御できるようにします。"};
    result[S_H_ONVIF_PASSWORD] = {"Password of the ONVIF user admin. 4K Cam Manager logs in with blue.", "ONVIF 用户 admin 的密码。4K Cam Manager 使用 blue 登录。", "ONVIF ユーザー admin のパスワード。4K Cam Manager は blue でログインします。"};
```

- [ ] **Step 3: Add the parameters**

In `web/mt11-web.cpp`, after `replacement_wb_options[]`:

```cpp
#if APCAM_HAVE_4KCAM_COMPAT
static const struct option wb_water_options[] = {
    {"green", S_OPT_WATER_GREEN}, {"blue", S_OPT_WATER_BLUE},
};
#endif
```

In `replacement_parameters[]`, after the `#endif` closing the `wb_temp` block and before `proxy_enabled`:

```cpp
#if APCAM_HAVE_4KCAM_COMPAT
    {"wb_water", "image", "wb_water", S_P_WB_WATER, S_H_WB_WATER, PARAM_ENUM, 0, 0, 0, wb_water_options, 2},
    {"fourkcam_manager", "compat", "fourkcam_manager", S_P_FOURKCAM, S_H_FOURKCAM,
     PARAM_ENUM, 0, 0, 0, replacement_boolean_options, 2},
    {"onvif_password", "compat", "onvif_password", S_P_ONVIF_PASSWORD, S_H_ONVIF_PASSWORD,
     PARAM_PASSWORD, 1, 63, 1, NULL, 0},
#endif
```

In `replacement_defaults[]`, after the `#endif` closing the WB defaults and before `"false", "", "10001", ...`:

```cpp
#if APCAM_HAVE_4KCAM_COMPAT
    "green", "true", "blue",
#endif
```

- [ ] **Step 4: Build and check**

Run: `PATH=/opt/homebrew/opt/llvm/bin:$PATH make -C web CAMERA_BACKEND=hisi HISI_CROSS_COMPILE=/Users/will/git/4kcamera_works/tools/hisi-clang/bin/aarch64-linux- hisi-web && PATH=/opt/homebrew/opt/llvm/bin:$PATH make -C camera_app CAMERA_BACKEND=hisi CROSS_COMPILE=/Users/will/git/4kcamera_works/tools/hisi-clang/bin/aarch64-linux- 2>&1 | tail -2`
Expected: both link without errors (the `replacement defaults must match parameters` static_assert holds).

- [ ] **Step 5: Commit**

```bash
git add include/apcam/target.h include/apcam/target_hisi.h camera_app/src/backends/hisi/media.cpp web/APC_Translations.h web/mt11-web.cpp
git commit -m "hisi: 4K Cam Manager settings, and the automatic bitrate shared with the web"
```

---

### Task 6: The `/onvif` and `/action` routes

**Files:**
- Modify: `web/mt11-web.cpp`: includes, `request_white_balance` (~3743), a new compat block after `restarted_config_ok()` (~4693), and `handle_request` before `select_language(&request);` (~7537)
- Create: `web/tests/check_4kcam_compat.py`

**Interfaces:**
- Consumes: Tasks 2–5.
- Produces: `static bool start_white_balance(char water, char *message, size_t message_size)`; `static bool fourkcam_enabled(void)`; `static void effective_value(const char *config, const char *section, const char *key, char *value, size_t size)`; `static bool eth0_mac(unsigned char mac[6])`; `static unsigned rtsp_port(void)`. The check script takes `--camera`, `--local`, `--camsh`, `--password`, `--out`, `--ini` and `--skip-discovery`.

- [ ] **Step 1: Write the failing device check**

`web/tests/check_4kcam_compat.py`:

```python
#!/usr/bin/env python3
"""Checks a camera running this firmware the way MAVLink Camera Manager and
4K Cam Manager talk to it, and saves its ONVIF replies for onvif-check.

camera.ini is backed up over telnet (tools/camsh.py) before the first change
and restored at the end, then camera-app is restarted through /action/restart.
"""
import argparse
import base64
import datetime
import hashlib
import json
import os
import re
import socket
import subprocess
import sys
import time
import urllib.error
import urllib.request
import uuid

HERE = os.path.dirname(os.path.abspath(__file__))
BACKUP = '/run/4kcam-check.ini'

U8 = range(256)
U16 = range(65536)
IMAGE_TYPES = {
    'brightness': U8, 'contrast': U8, 'saturation': U8, 'antiFog': {0, 1},
    'frameTurbo_pro': {0, 1, 2}, 'AE_strategy_mode': {0, 1}, 'auto_exposureEx': {0, 1},
    'exposure_time': {12, 25, 30, 50, 60, 100, 200, 400, 800, 1000, 2000, 4000, 8000, 10000, 34464},
    'auto_awb': {0, 1}, 'awb_red': U8, 'awb_green': U8, 'awb_blue': U8, 'awb_auto_mode': {0, 1},
    'rotate': {0, 1, 2, 3},
}
IMAGE_EX_TYPES = {'mirror': {0, 1}, 'flip': {0, 1}, 'onceAWB': U8}
VENC_TYPES = {
    'channel': {0, 1, 2}, 'encode_profile': {0, 1, 2}, 'encode_type': {1, 5}, 'pic_width': U16,
    'pic_height': U16, 'rc_mode': {0, 1}, 'bitrate': U16, 'max_framerate': U16, 'frame_rate': U16, 'gop': U16,
}
IDENTITY = ('code', 'device_mac', 'device_id', 'device_ip', 'log', 'sign_tby')


class Check:
    def __init__(self):
        self.failures = 0

    def __call__(self, ok, what):
        print(('PASS ' if ok else 'FAIL ') + what, flush=True)
        if not ok:
            self.failures += 1
        return ok


def camsh(args, command, timeout=30):
    result = subprocess.run([sys.executable, args.camsh, '-t', str(timeout), command],
                            check=True, capture_output=True, text=True)
    return result.stdout


def post(url, body, content_type):
    request = urllib.request.Request(url, data=body.encode(), method='POST',
                                     headers={'Content-Type': content_type})
    try:
        with urllib.request.urlopen(request, timeout=30) as reply:
            return reply.status, reply.read().decode()
    except urllib.error.HTTPError as error:
        return error.code, error.read().decode()


def action(args, name, fields=None):
    body = json.dumps(fields) if fields is not None else ''
    status, text = post(f'http://{args.camera}/action/{name}', body, 'application/json')
    return status, (json.loads(text) if status == 200 else None)


def bad_types(reply, types):
    return [key for key, allowed in types.items()
            if key in reply and not (isinstance(reply[key], int) and reply[key] in allowed)]


def username_token(password, user='admin'):
    nonce = os.urandom(16)
    created = datetime.datetime.now(datetime.timezone.utc).strftime('%Y-%m-%dT%H:%M:%S.000Z')
    digest = base64.b64encode(hashlib.sha1(nonce + created.encode() + password.encode()).digest()).decode()
    return ('<s:Header><wsse:Security xmlns:wsse="http://docs.oasis-open.org/wss/2004/01/oasis-200401-wss-wssecurity-secext-1.0.xsd" '
            'xmlns:wsu="http://docs.oasis-open.org/wss/2004/01/oasis-200401-wss-wssecurity-utility-1.0.xsd"><wsse:UsernameToken>'
            f'<wsse:Username>{user}</wsse:Username>'
            '<wsse:Password Type="http://docs.oasis-open.org/wss/2004/01/oasis-200401-wss-username-token-profile-1.0#PasswordDigest">'
            f'{digest}</wsse:Password>'
            '<wsse:Nonce EncodingType="http://docs.oasis-open.org/wss/2004/01/oasis-200401-wss-soap-message-security-1.0#Base64Binary">'
            f'{base64.b64encode(nonce).decode()}</wsse:Nonce><wsu:Created>{created}</wsu:Created>'
            '</wsse:UsernameToken></wsse:Security></s:Header>')


def soap(args, operation, password):
    header = username_token(password) if password is not None else ''
    envelope = ('<?xml version="1.0" encoding="UTF-8"?><s:Envelope xmlns:s="http://www.w3.org/2003/05/soap-envelope" '
                'xmlns:tds="http://www.onvif.org/ver10/device/wsdl" xmlns:trt="http://www.onvif.org/ver10/media/wsdl" '
                f'xmlns:tt="http://www.onvif.org/ver10/schema">{header}<s:Body>{operation}</s:Body></s:Envelope>')
    return post(f'http://{args.camera}/onvif/device_service', envelope, 'application/soap+xml; charset=utf-8')


def save(args, kind, text):
    with open(os.path.join(args.out, kind + '.xml'), 'w') as file:
        file.write(text)


def check_onvif(args, check):
    status, text = soap(args, '<tds:GetSystemDateAndTime/>', None)
    check(status == 200 and 'UTCDateTime' in text, 'GetSystemDateAndTime without a login')
    status, text = soap(args, '<tds:GetDeviceInformation/>', None)
    check(status == 400 and 'ter:NotAuthorized' in text, 'GetDeviceInformation without a login is NotAuthorized')
    save(args, 'not-authorized', text)
    status, text = soap(args, '<tds:GetDeviceInformation/>', 'wrong')
    check(status == 400 and 'ter:NotAuthorized' in text, 'GetDeviceInformation with a wrong password is NotAuthorized')
    status, text = soap(args, '<tds:GetDeviceInformation/>', args.password)
    check(status == 200 and '<tds:Manufacturer>RadCam</tds:Manufacturer><tds:Model>HDIPC</tds:Model>' in text
          and '<tds:HardwareId>IPCamera</tds:HardwareId>' in text, 'GetDeviceInformation')
    save(args, 'device-information', text)
    status, text = soap(args, '<tds:GetServices><tds:IncludeCapability>false</tds:IncludeCapability></tds:GetServices>',
                        args.password)
    check(status == 200 and f'<tds:XAddr>http://{args.camera}/onvif/device_service</tds:XAddr>' in text
          and 'ver10/media/wsdl' in text, 'GetServices')
    save(args, 'services', text)
    status, text = soap(args, '<trt:GetProfiles/>', args.password)
    check(status == 200 and 'token="main"' in text and 'token="sub"' in text, 'GetProfiles')
    save(args, 'profiles', text)
    for token, path in (('main', 'video1'), ('sub', 'video2')):
        status, text = soap(args, '<trt:GetStreamUri><trt:StreamSetup><tt:Stream>RTP-Unicast</tt:Stream>'
                                  '<tt:Transport><tt:Protocol>RTSP</tt:Protocol></tt:Transport></trt:StreamSetup>'
                                  f'<trt:ProfileToken>{token}</trt:ProfileToken></trt:GetStreamUri>', args.password)
        check(status == 200 and f'<tt:Uri>rtsp://{args.camera}:8554/{path}</tt:Uri>' in text, f'GetStreamUri {token}')
        if token == 'main':
            save(args, 'stream-uri', text)
    status, text = soap(args, '<tds:GetCapabilities/>', args.password)
    check(status == 400 and 'ter:ActionNotSupported' in text, 'an unsupported operation is ActionNotSupported')


def round_trip(args, check, name, fields, getter, getter_fields, expect):
    status, reply = action(args, name, fields)
    _, after = action(args, getter, getter_fields)
    got = {key: (after or {}).get(key) for key in expect}
    check(status == 200 and reply['code'] == 0 and got == expect, f'{name} {fields} reads back {expect}, got {got}')


def check_actions(args, check):
    status, reply = action(args, 'getSysConfig')
    check(status == 200 and reply['code'] == 0 and all(key in reply for key in IDENTITY)
          and reply['device_ip'] == args.camera, 'getSysConfig identity')
    status, reply = action(args, 'noSuchAction', {})
    check(status == 200 and reply['code'] == -1, 'an unknown action is code -1')
    status, image = action(args, 'getImageAdjustment')
    check(status == 200 and image['code'] == 0 and not bad_types(image, IMAGE_TYPES),
          f'getImageAdjustment types {bad_types(image, IMAGE_TYPES)}')
    status, ex = action(args, 'getImageAdjustmentEx')
    check(status == 200 and ex['code'] == 0 and not bad_types(ex, IMAGE_EX_TYPES), 'getImageAdjustmentEx types')
    for channel in (0, 1, 2):
        status, venc = action(args, 'getVencConf', {'channel': channel})
        check(status == 200 and venc['code'] == 0 and not bad_types(venc, VENC_TYPES) and venc['pixel_list'],
              f'getVencConf channel {channel} types')

    level = 204 if image['brightness'] != 204 else 128
    round_trip(args, check, 'setImageAdjustment', {'brightness': level}, 'getImageAdjustment', None, {'brightness': level})
    round_trip(args, check, 'setImageAdjustment', {'auto_awb': 1, 'awb_red': 40, 'awb_blue': 60},
               'getImageAdjustment', None, {'auto_awb': 1, 'awb_red': 40, 'awb_blue': 60})
    round_trip(args, check, 'setImageAdjustment', {'awb_auto_mode': 1}, 'getImageAdjustment', None, {'awb_auto_mode': 1})
    round_trip(args, check, 'setImageAdjustment', {'awb_auto_mode': 0}, 'getImageAdjustment', None, {'awb_auto_mode': 0})
    round_trip(args, check, 'setImageAdjustment', {'auto_exposureEx': 1, 'exposure_time': 400},
               'getImageAdjustment', None, {'auto_exposureEx': 1, 'exposure_time': 400})
    round_trip(args, check, 'setImageAdjustment', {'auto_exposureEx': 0}, 'getImageAdjustment', None, {'auto_exposureEx': 0})
    round_trip(args, check, 'setImageAdjustmentEx', {'mirror': 1, 'flip': 1}, 'getImageAdjustmentEx', None,
               {'mirror': 1, 'flip': 1})
    round_trip(args, check, 'setImageAdjustmentEx', {'mirror': 0, 'flip': 0}, 'getImageAdjustmentEx', None,
               {'mirror': 0, 'flip': 0})
    round_trip(args, check, 'setVencConf', {'channel': 0, 'bitrate': 8000}, 'getVencConf', {'channel': 0},
               {'bitrate': 8000})
    status, reply = action(args, 'setVencConf', {'channel': 2, 'bitrate': 32})
    check(status == 200 and reply['code'] == 0, 'setVencConf channel 2 is accepted')

    round_trip(args, check, 'setImageAdjustment', {'auto_awb': 0}, 'getImageAdjustment', None, {'auto_awb': 0})
    started = time.time()
    status, reply = action(args, 'setImageAdjustmentEx', {'onceAWB': 1})
    check(status == 200 and reply['code'] == 0 and time.time() - started < 2, 'onceAWB replies at once')
    deadline = time.time() + 10
    image = None
    while time.time() < deadline:
        _, image = action(args, 'getImageAdjustment')
        if image and image['auto_awb'] == 1:
            break
        time.sleep(0.5)
    check(image is not None and image['auto_awb'] == 1, f'onceAWB leaves manual gains within 10 s: {image}')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--camera', default='192.168.2.10')
    parser.add_argument('--local', default='192.168.2.15', help="this machine's address on the camera's LAN")
    parser.add_argument('--camsh', required=True, help='path to tools/camsh.py')
    parser.add_argument('--password', default='blue')
    parser.add_argument('--ini', default='/date/ap_cameragimbal/camera.ini')
    parser.add_argument('--out', default=os.path.join(HERE, 'onvif-check', 'replies'))
    parser.add_argument('--skip-discovery', action='store_true')
    args = parser.parse_args()
    os.makedirs(args.out, exist_ok=True)
    check = Check()
    camsh(args, f'cp {args.ini} {BACKUP}')
    try:
        check_onvif(args, check)
        check_actions(args, check)
    finally:
        camsh(args, f'cp {BACKUP} {args.ini}.restore && mv {args.ini}.restore {args.ini}')
    status, reply = action(args, 'restart', {})
    check(status == 200 and reply['code'] == 0, 'restart restarts camera-app')
    status, reply = action(args, 'getSysConfig')
    check(status == 200 and reply['code'] == 0, 'getSysConfig after restart')
    print(f'{check.failures} failure(s)')
    return 1 if check.failures else 0


if __name__ == '__main__':
    sys.exit(main())
```

- [ ] **Step 2: Run it against the flashed firmware to see it fail**

Run: `python3 web/tests/check_4kcam_compat.py --camsh /Users/will/git/4kcamera_works/tools/camsh.py --skip-discovery 2>&1 | head -5`
Expected: `FAIL GetSystemDateAndTime without a login` (today's firmware sends unauthenticated requests to the login), then a traceback or more FAIL lines. The `finally` restores `camera.ini`.

- [ ] **Step 3: Split the one-push request**

Replace `request_white_balance` in `web/mt11-web.cpp` (from `static bool request_white_balance(char water, char *message, size_t message_size)` up to the end of its `snprintf(message, message_size, T(S_E_WB_REQUEST), strerror(errno)); (void)unlink(temporary); return false; }` block) with:

```cpp
/* Asks camera-app for a one-push measurement ('1' green or '2' blue water)
 * through <config>.wb.command, without waiting for the result. */
static bool start_white_balance(char water, char *message, size_t message_size)
{
    char command[sizeof(REPLACEMENT_CONFIG_PATH) + sizeof(".wb.command")];
    char temporary[sizeof(command) + sizeof(".tmp")];
    char status[sizeof(REPLACEMENT_CONFIG_PATH) + sizeof(".wb.status")];

#ifndef MT11_WEB_TEST
    if (current_camera_kind() == CAMERA_NONE) {
        snprintf(message, message_size, "%s", T(S_E_CAMERA_NOT_RUNNING));
        return false;
    }
#endif
    snprintf(command, sizeof(command), "%s.wb.command", REPLACEMENT_CONFIG_PATH);
    snprintf(temporary, sizeof(temporary), "%s.tmp", command);
    snprintf(status, sizeof(status), "%s.wb.status", REPLACEMENT_CONFIG_PATH);
    (void)unlink(status);
    FILE *output = fopen(temporary, "w");
    bool written = output != NULL && fprintf(output, "%c\n", water) == 2;
    if (output != NULL && fclose(output) != 0) written = false;
    if (!written || rename(temporary, command) != 0) {
        snprintf(message, message_size, T(S_E_WB_REQUEST), strerror(errno));
        (void)unlink(temporary);
        return false;
    }
    return true;
}

static bool request_white_balance(char water, char *message, size_t message_size)
{
    char command[sizeof(REPLACEMENT_CONFIG_PATH) + sizeof(".wb.command")];
    char status[sizeof(REPLACEMENT_CONFIG_PATH) + sizeof(".wb.status")];

    if (!start_white_balance(water, message, message_size)) return false;
    snprintf(command, sizeof(command), "%s.wb.command", REPLACEMENT_CONFIG_PATH);
    snprintf(status, sizeof(status), "%s.wb.status", REPLACEMENT_CONFIG_PATH);
```

The polling loop that follows (`for (unsigned attempt = 0; attempt < 100U; attempt++) {` to the end of the function) stays as it is.

- [ ] **Step 4: Add the includes and the compat block**

In `web/mt11-web.cpp`, after `#include "APC_HTTPRequest.h"`:

```cpp
#if APCAM_HAVE_4KCAM_COMPAT
#include "APC_FourKCam.h"
#include "APC_ONVIF.h"
#include <net/if.h>
#include <sys/prctl.h>
#endif
```

After the closing `}` of `restarted_config_ok()`:

```cpp
#if APCAM_HAVE_4KCAM_COMPAT
/* ---- 4K Cam Manager and MAVLink Camera Manager (docs/4kcam-manager-compat.md) ---- */
static_assert(APCAM_HAVE_VIDEO_BITRATE && APCAM_HAVE_VIDEO_FRAME_RATE && APCAM_HAVE_WB_TUNING,
              "the 4K Cam Manager mapping uses bitrate, frame rate and one-push white balance");

/* A camera.ini value, or the web's default for it when the key is absent. */
static void effective_value(const char *config, const char *section, const char *key,
                            char *value, size_t size)
{
    if (config != NULL && ini_get_value(config, section, key, value, size)) return;
    value[0] = '\0';
    for (size_t i = 0; i < sizeof(replacement_parameters) / sizeof(replacement_parameters[0]); i++) {
        if (!strcmp(replacement_parameters[i].section, section) && !strcmp(replacement_parameters[i].key, key)) {
            snprintf(value, size, "%s", replacement_defaults[i]);
            return;
        }
    }
}

static bool fourkcam_enabled(void)
{
    size_t length;
    char *config = read_file(config_path(CAMERA_REPLACEMENT), MAX_CONFIG, &length);
    char value[16];
    effective_value(config, "compat", "fourkcam_manager", value, sizeof(value));
    free(config);
    return strcmp(value, "false") != 0;
}

static bool eth0_mac(unsigned char mac[6])
{
    size_t length;
    char *text = read_file("/sys/class/net/eth0/address", 64, &length);
    bool ok = text != NULL && sscanf(text, "%hhx:%hhx:%hhx:%hhx:%hhx:%hhx",
                                     &mac[0], &mac[1], &mac[2], &mac[3], &mac[4], &mac[5]) == 6;
    free(text);
    return ok;
}

/* The camera's address on this connection, which the client can reach. */
static void local_address(int fd, char *out, size_t size)
{
    struct sockaddr_in address;
    socklen_t length = sizeof(address);
    if (getsockname(fd, (struct sockaddr *)&address, &length) != 0 ||
        inet_ntop(AF_INET, &address.sin_addr, out, (socklen_t)size) == NULL) snprintf(out, size, "0.0.0.0");
}

static unsigned rtsp_port(void)
{
    const char *text = getenv("CAMERA_APP_RTSP_PORT");
    char *end;
    unsigned long port = text != NULL ? strtoul(text, &end, 10) : 0;
    return text != NULL && *text != '\0' && *end == '\0' && port > 0 && port <= 65535 ? (unsigned)port : 8554U;
}

static void fourkcam_image(const char *config, apc_fourkcam::Image &image)
{
    char value[32];
    effective_value(config, "image", "brightness", value, sizeof(value));
    image.brightness = strtol(value, NULL, 10);
    effective_value(config, "image", "contrast", value, sizeof(value));
    image.contrast = strtol(value, NULL, 10);
    effective_value(config, "image", "saturation", value, sizeof(value));
    image.saturation = strtol(value, NULL, 10);
    effective_value(config, "image", "white_balance", image.white_balance, sizeof(image.white_balance));
    effective_value(config, "image", "wb_red", value, sizeof(value));
    image.wb_red = (unsigned)strtoul(value, NULL, 10);
    effective_value(config, "image", "wb_green", value, sizeof(value));
    image.wb_green = (unsigned)strtoul(value, NULL, 10);
    effective_value(config, "image", "wb_blue", value, sizeof(value));
    image.wb_blue = (unsigned)strtoul(value, NULL, 10);
    effective_value(config, "image", "wb_water", value, sizeof(value));
    image.blue_water = strcmp(value, "blue") == 0;
    effective_value(config, "image", "shutter", image.shutter, sizeof(image.shutter));
    effective_value(config, "mount", "orientation", value, sizeof(value));
    image.inverted = strcmp(value, "inverted") == 0;
}

static void fourkcam_stream(const char *config, bool main_stream, apc_fourkcam::Stream &stream)
{
    const char *section = main_stream ? "stream.main" : "stream.sub";
    char value[32];
    effective_value(config, section, "resolution", value, sizeof(value));
    if (sscanf(value, "%ux%u", &stream.width, &stream.height) != 2) stream.width = stream.height = 0;
    effective_value(config, section, "codec", value, sizeof(value));
    stream.h265 = strcmp(value, "h265") == 0;
    stream.frame_rate = stream.max_frame_rate = APCAM_FRAME_RATE;
    stream.bitrate_kbps = apcam_hisi_auto_bitrate_kbps(stream.width, stream.height);
    if (!main_stream) return;
    stream.configurable = true;
    stream.min_frame_rate = APCAM_MAIN_FPS_MIN;
    stream.max_bitrate_kbps = APCAM_MAIN_BITRATE_MAX_KBPS;
    effective_value(config, section, "bitrate_kbps", value, sizeof(value));
    if (strtoul(value, NULL, 10) != 0) stream.bitrate_kbps = (unsigned)strtoul(value, NULL, 10);
    effective_value(config, section, "frame_rate", value, sizeof(value));
    if (strtoul(value, NULL, 10) != 0) stream.frame_rate = (unsigned)strtoul(value, NULL, 10);
}

/* Saved like the parameter form saves; camera-app applies it within 500 ms. */
static bool fourkcam_save(const char *config, const apc_fourkcam::Changes &changes, char *message, size_t size)
{
    struct ini_update updates[sizeof(changes.updates) / sizeof(changes.updates[0])];
    for (size_t i = 0; i < changes.count; i++) {
        updates[i].section = changes.updates[i].section;
        snprintf(updates[i].key, sizeof(updates[i].key), "%s", changes.updates[i].key);
        snprintf(updates[i].value, sizeof(updates[i].value), "%s", changes.updates[i].value);
        updates[i].found = false;
    }
    size_t length = 0;
    char *updated = ini_apply_updates(config, updates, changes.count, true, &length, message, size);
    bool ok = updated != NULL &&
              save_config(config_path(CAMERA_REPLACEMENT), config_backup_path(CAMERA_REPLACEMENT),
                          updated, length, message, size);
    if (ok && wait_live_config(updated, length, message, size, false)) ok = false;
    free(updated);
    return ok;
}

static void handle_fourkcam_action(int fd, const APC_HTTPRequest *request, const char *peer)
{
    const char *name = request->path + strlen("/action/");
    size_t config_length = 0;
    char *config = read_file(config_path(CAMERA_REPLACEMENT), MAX_CONFIG, &config_length);
    apc_fourkcam::Field fields[64];
    size_t field_count = 0;
    apc_fourkcam::Image image;
    apc_fourkcam::Stream streams[2];
    apc_fourkcam::Changes changes;
    const char *main_names[sizeof(main_resolution_options) / sizeof(main_resolution_options[0])];
    const char *sub_names[sizeof(sub_resolution_options) / sizeof(sub_resolution_options[0])];
    const size_t main_count = sizeof(main_names) / sizeof(main_names[0]);
    const size_t sub_count = sizeof(sub_names) / sizeof(sub_names[0]);
    for (size_t i = 0; i < main_count; i++) main_names[i] = main_resolution_options[i].value;
    for (size_t i = 0; i < sub_count; i++) sub_names[i] = sub_resolution_options[i].value;
    char message[512] = "";
    int code = 0;

    if (config == NULL) {
        code = -1;
        snprintf(message, sizeof(message), T(S_E_CANNOT_READ), config_path(CAMERA_REPLACEMENT), strerror(errno));
    } else if (!apc_fourkcam::parse_object(request->body, request->body_len, fields,
                                           sizeof(fields) / sizeof(fields[0]), &field_count)) {
        code = -1;
        snprintf(message, sizeof(message), "malformed JSON");
    }
    fourkcam_image(config, image);
    fourkcam_stream(config, true, streams[0]);
    fourkcam_stream(config, false, streams[1]);
    const apc_fourkcam::Field *channel_field = apc_fourkcam::find(fields, field_count, "channel");
    const long channel = channel_field != NULL ? channel_field->value : 0;
    const bool main_channel = channel == 0;

    APC_StringBuffer reply;
    reply.append("{");
    if (code != 0) {
    } else if (!strcmp(name, "getSysConfig")) {
    } else if (!strcmp(name, "getImageAdjustment")) {
        apc_fourkcam::image_adjustment(reply, image);
    } else if (!strcmp(name, "setImageAdjustment")) {
        apc_fourkcam::image_adjustment_changes(fields, field_count, image, changes);
    } else if (!strcmp(name, "getImageAdjustmentEx")) {
        apc_fourkcam::image_adjustment_ex(reply, image);
    } else if (!strcmp(name, "setImageAdjustmentEx")) {
        apc_fourkcam::image_adjustment_ex_changes(fields, field_count, image, changes);
    } else if (!strcmp(name, "getVencConf")) {
        // the vendor's third stream reads as the sub stream
        apc_fourkcam::venc(reply, channel, streams[main_channel ? 0 : 1],
                           main_channel ? main_names : sub_names, main_channel ? main_count : sub_count);
    } else if (!strcmp(name, "setVencConf")) {
        if (channel == 0 || channel == 1)
            apc_fourkcam::venc_changes(fields, field_count, streams[channel], channel == 0 ? main_names : sub_names,
                                       channel == 0 ? main_count : sub_count,
                                       channel == 0 ? "stream.main" : "stream.sub", changes);
    } else if (!strcmp(name, "restart")) {
        if (!restart_camera(message, sizeof(message))) code = -1;
    } else {
        code = -1;
        snprintf(message, sizeof(message), "unsupported action %s", name);
    }
    if (code == 0 && changes.count > 0 && !fourkcam_save(config, changes, message, sizeof(message))) code = -1;
    if (code == 0 && changes.one_push &&
        !start_white_balance(image.blue_water ? '2' : '1', message, sizeof(message))) code = -1;
    if (code != 0) log_message("4K Cam Manager %s from %s failed: %s", name, peer, message);
    else if (changes.count > 0 || changes.one_push) log_message("4K Cam Manager %s by %s", name, peer);

    unsigned char mac[6] = {0};
    char mac_text[18], serial[13], address[INET_ADDRSTRLEN];
    (void)eth0_mac(mac);
    snprintf(mac_text, sizeof(mac_text), "%02x-%02x-%02x-%02x-%02x-%02x", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    snprintf(serial, sizeof(serial), "%02X%02X%02X%02X%02X%02X", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    local_address(fd, address, sizeof(address));
    apc_fourkcam::finish(reply, code, code != 0 ? message : "", {mac_text, serial, address});
    send_response(fd, 200, "OK", "application/json", reply.data(), reply.size(), NULL);
    free(config);
}

static void handle_onvif(int fd, const APC_HTTPRequest *request)
{
    char *body = strndup(request->body, request->body_len);
    if (body == NULL) {
        send_text_errorf(fd, 500, "Internal Server Error", S_OUT_OF_MEMORY);
        return;
    }
    size_t config_length = 0;
    char *config = read_file(config_path(CAMERA_REPLACEMENT), MAX_CONFIG, &config_length);
    char operation[64] = "", password[128], address[INET_ADDRSTRLEN], text[160];
    effective_value(config, "compat", "onvif_password", password, sizeof(password));
    local_address(fd, address, sizeof(address));
    APC_StringBuffer reply;
    int status = 200;

    if (!apc_onvif::body_operation(body, operation, sizeof(operation))) {
        status = 400;
        apc_onvif::fault(reply, "Sender", "InvalidArgVal", "No SOAP operation");
    } else if (!strcmp(operation, "GetSystemDateAndTime")) {
        const time_t now = time(NULL);
        struct tm utc;
        gmtime_r(&now, &utc);
        apc_onvif::system_date_and_time(reply, utc);
    } else if (apc_onvif::check_username_token(body, "admin", password) != apc_onvif::Auth::ok) {
        status = 400;
        apc_onvif::fault(reply, "Sender", "NotAuthorized", "Sender not authorized");
    } else if (!strcmp(operation, "GetDeviceInformation")) {
        unsigned char mac[6] = {0};
        char serial[13];
        (void)eth0_mac(mac);
        snprintf(serial, sizeof(serial), "%02X%02X%02X%02X%02X%02X", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
        apc_onvif::device_information(reply, {"RadCam", "HDIPC", CA_FIRMWARE_GIT_HASH, serial, "IPCamera"});
    } else if (!strcmp(operation, "GetServices")) {
        snprintf(text, sizeof(text), "http://%s/onvif/device_service", address);
        apc_onvif::services(reply, text);
    } else if (!strcmp(operation, "GetProfiles")) {
        apc_fourkcam::Stream streams[2];
        fourkcam_stream(config, true, streams[0]);
        fourkcam_stream(config, false, streams[1]);
        const apc_onvif::Profile profiles[] = {
            {"main", streams[0].width, streams[0].height, streams[0].frame_rate, streams[0].bitrate_kbps, streams[0].h265},
            {"sub", streams[1].width, streams[1].height, streams[1].frame_rate, streams[1].bitrate_kbps, streams[1].h265},
        };
        apc_onvif::profiles(reply, profiles, sizeof(profiles) / sizeof(profiles[0]));
    } else if (!strcmp(operation, "GetStreamUri")) {
        char token[16] = "";
        (void)apc_onvif::element_text(body, "ProfileToken", token, sizeof(token));
        snprintf(text, sizeof(text), "rtsp://%s:%u/%s", address, rtsp_port(), strcmp(token, "sub") ? "video1" : "video2");
        apc_onvif::stream_uri(reply, text);
    } else {
        status = 400;
        apc_onvif::fault(reply, "Receiver", "ActionNotSupported", "Operation not supported");
    }
    send_response(fd, status, status == 200 ? "OK" : "Bad Request", "application/soap+xml; charset=utf-8",
                  reply.data(), reply.size(), NULL);
    free(config);
    free(body);
}
#endif
```

In `handle_request`, immediately before `select_language(&request);`:

```cpp
#if APCAM_HAVE_4KCAM_COMPAT
    /* 4K Cam Manager and MAVLink Camera Manager can't log in to the web. ONVIF
     * checks its own UsernameToken; /action is open, as on the vendor's camera. */
    if (strcmp(request.method, "POST") == 0 &&
        (strncmp(request.path, "/action/", strlen("/action/")) == 0 ||
         strcmp(request.path, "/onvif/device_service") == 0)) {
        if (!fourkcam_enabled()) send_text_errorf(fd, 404, "Not Found", S_NOT_FOUND);
        else if (request.path[1] == 'a') handle_fourkcam_action(fd, &request, peer);
        else handle_onvif(fd, &request);
        goto done;
    }
#endif
```

- [ ] **Step 5: Build, deploy in RAM and run the check**

Run the cross-build from **Environment**, then the **RAM deploy** steps, then:
`python3 web/tests/check_4kcam_compat.py --camsh /Users/will/git/4kcamera_works/tools/camsh.py --skip-discovery`
Expected: every line `PASS`, ending `0 failure(s)`. If the `onceAWB` check fails, check the scene isn't dark (one-push needs a lit image) and read `/run/ap_cameragimbal/*.log` for camera-app's one-push message before changing code.

- [ ] **Step 6: Commit**

```bash
git add web/mt11-web.cpp web/tests/check_4kcam_compat.py
git commit -m "hisi-web: answer 4K Cam Manager's /action and MCM's ONVIF requests"
```

---

### Task 7: WS-Discovery responder

**Files:**
- Modify: `web/mt11-web.cpp` (compat block from Task 6, and `main` after `log_message("%s listening on 0.0.0.0:%u", ...)`)
- Modify: `web/tests/check_4kcam_compat.py`

**Interfaces:**
- Consumes: `apc_onvif::probe_message_id`, `probe_match`, `uuid_from_mac`, `fourkcam_enabled()`, `eth0_mac()`.
- Produces: `static void start_discovery(int listen_fd)`, a child that exits with `hisi-web`. The check script gains `check_discovery(args, check, expect_reply)` and `check_disabled(args, check)`.

- [ ] **Step 1: Add the failing discovery checks**

In `web/tests/check_4kcam_compat.py`, add before `main`:

```python
def probe(args, timeout=3.0):
    message_id = f'uuid:{uuid.uuid4()}'
    with open(os.path.join(HERE, 'fixtures', 'onvif', 'Probe.xml')) as file:
        message = file.read().replace('uuid:00000000-0000-4000-8000-000000000000', message_id)
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.bind((args.local, 0))
    sock.setsockopt(socket.IPPROTO_IP, socket.IP_MULTICAST_IF, socket.inet_aton(args.local))
    sock.sendto(message.encode(), ('239.255.255.250', 3702))
    sock.settimeout(0.5)
    replies = []
    deadline = time.time() + timeout
    while time.time() < deadline:
        try:
            data, source = sock.recvfrom(16384)
        except socket.timeout:
            continue
        if source[0] == args.camera:
            replies.append(data.decode())
    sock.close()
    return message_id, replies


def check_discovery(args, check):
    message_id, replies = probe(args)
    reply = replies[0] if len(replies) == 1 else ''
    relates = re.search(r'<a:RelatesTo>([^<]*)</a:RelatesTo>', reply)
    check(len(replies) == 1 and relates is not None and relates.group(1) == message_id,
          f'one ProbeMatch for our Probe ({len(replies)} replies)')
    check('onvif://www.onvif.org/name/hd' in reply and 'onvif://www.onvif.org/hardware/HW0100302' in reply,
          'ProbeMatch scopes name/hd and hardware/HW0100302')
    check(f'<d:XAddrs>http://{args.camera}/onvif/device_service</d:XAddrs>' in reply, 'ProbeMatch XAddrs')
    if reply:
        save(args, 'probe', reply)


def check_disabled(args, check):
    camsh(args, f"if grep -q '^\\[compat\\]' {args.ini}; then sed -i '/^\\[compat\\]/a fourkcam_manager = false' {args.ini};"
                f" else printf '\\n[compat]\\nfourkcam_manager = false\\n' >> {args.ini}; fi")
    status, _ = post(f'http://{args.camera}/action/getSysConfig', '', 'application/json')
    check(status == 404, f'/action is 404 with Allow 4K Cam Manager off ({status})')
    status, _ = soap(args, '<tds:GetSystemDateAndTime/>', None)
    check(status == 404, f'/onvif is 404 with Allow 4K Cam Manager off ({status})')
    _, replies = probe(args)
    check(not replies, 'no ProbeMatch with Allow 4K Cam Manager off')
```

In `main`, change the `try:` body to:

```python
        if not args.skip_discovery:
            check_discovery(args, check)
        check_onvif(args, check)
        check_actions(args, check)
        if not args.skip_discovery:
            check_disabled(args, check)
```

- [ ] **Step 2: Run to verify discovery fails**

Run: `python3 web/tests/check_4kcam_compat.py --camsh /Users/will/git/4kcamera_works/tools/camsh.py 2>&1 | grep -E "ProbeMatch|failure"`
Expected: `FAIL one ProbeMatch for our Probe (0 replies)` and the other ProbeMatch checks failing; `no ProbeMatch with Allow 4K Cam Manager off` passes.

- [ ] **Step 3: Write the responder**

Append inside the compat block in `web/mt11-web.cpp` (before its `#endif`):

```cpp
/* Joins the WS-Discovery group on every IPv4 interface; ones that come up
 * later are joined on the next pass. */
static void join_discovery_group(int fd)
{
    struct ifaddrs *addresses;
    if (getifaddrs(&addresses) != 0) return;
    for (struct ifaddrs *item = addresses; item != NULL; item = item->ifa_next) {
        if (item->ifa_addr == NULL || item->ifa_addr->sa_family != AF_INET ||
            (item->ifa_flags & IFF_LOOPBACK) != 0) continue;
        struct ip_mreq group = {};
        group.imr_multiaddr.s_addr = inet_addr("239.255.255.250");
        group.imr_interface = ((struct sockaddr_in *)item->ifa_addr)->sin_addr;
        (void)setsockopt(fd, IPPROTO_IP, IP_ADD_MEMBERSHIP, &group, sizeof(group));
    }
    freeifaddrs(addresses);
}

/* The camera's address on the route to peer, for the reply's XAddrs. */
static bool address_towards(const struct sockaddr_in *peer, char *out, size_t size)
{
    int fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    struct sockaddr_in local;
    socklen_t length = sizeof(local);
    bool ok = fd >= 0 && connect(fd, (const struct sockaddr *)peer, sizeof(*peer)) == 0 &&
              getsockname(fd, (struct sockaddr *)&local, &length) == 0 &&
              inet_ntop(AF_INET, &local.sin_addr, out, (socklen_t)size) != NULL;
    if (fd >= 0) close(fd);
    return ok;
}

static void random_uuid(char out[37])
{
    unsigned char bytes[16];
    int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    if (fd < 0 || read(fd, bytes, sizeof(bytes)) != (ssize_t)sizeof(bytes)) {
        const unsigned long seed = (unsigned long)time(NULL) ^ (unsigned long)getpid();
        for (size_t i = 0; i < sizeof(bytes); i++) bytes[i] = (unsigned char)(seed >> (i % 8U * 8U)) ^ (unsigned char)i;
    }
    if (fd >= 0) close(fd);
    bytes[6] = (unsigned char)((bytes[6] & 0x0FU) | 0x40U);
    bytes[8] = (unsigned char)((bytes[8] & 0x3FU) | 0x80U);
    snprintf(out, 37, "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
             bytes[0], bytes[1], bytes[2], bytes[3], bytes[4], bytes[5], bytes[6], bytes[7],
             bytes[8], bytes[9], bytes[10], bytes[11], bytes[12], bytes[13], bytes[14], bytes[15]);
}

static void run_discovery(void)
{
    int fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    int on = 1;
    struct sockaddr_in address = {};
    address.sin_family = AF_INET;
    address.sin_port = htons(3702);
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    if (fd < 0 || setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on)) != 0 ||
        bind(fd, (struct sockaddr *)&address, sizeof(address)) != 0) {
        log_message("WS-Discovery unavailable: %s", strerror(errno));
        _exit(1);
    }
    unsigned char mac[6] = {0};
    char uuid[37], endpoint[48];
    (void)eth0_mac(mac);
    apc_onvif::uuid_from_mac(mac, uuid);
    snprintf(endpoint, sizeof(endpoint), "urn:uuid:%s", uuid);
    time_t joined = 0;
    for (;;) {
        if (time(NULL) - joined >= 30) {
            join_discovery_group(fd);
            joined = time(NULL);
        }
        struct pollfd ready = {fd, POLLIN, 0};
        if (poll(&ready, 1, 30000) <= 0) continue;
        char message[8192];
        struct sockaddr_in peer;
        socklen_t peer_length = sizeof(peer);
        ssize_t got = recvfrom(fd, message, sizeof(message) - 1U, 0, (struct sockaddr *)&peer, &peer_length);
        if (got <= 0) continue;
        message[got] = '\0';
        char relates_to[256], local[INET_ADDRSTRLEN], xaddr[96], message_id[48];
        if (!apc_onvif::probe_message_id(message, relates_to, sizeof(relates_to)) || !fourkcam_enabled() ||
            !address_towards(&peer, local, sizeof(local))) continue;
        snprintf(xaddr, sizeof(xaddr), "http://%s/onvif/device_service", local);
        random_uuid(uuid);
        snprintf(message_id, sizeof(message_id), "uuid:%s", uuid);
        APC_StringBuffer reply;
        apc_onvif::probe_match(reply, message_id, relates_to, endpoint, xaddr);
        (void)sendto(fd, reply.data(), reply.size(), 0, (struct sockaddr *)&peer, peer_length);
    }
}

/* Saving the setting doesn't restart the web, so the child always runs and
 * reads the setting for each probe. */
static void start_discovery(int listen_fd)
{
    const pid_t parent = getpid();
    pid_t child = fork();
    if (child < 0) log_message("cannot start WS-Discovery: %s", strerror(errno));
    if (child != 0) return;
    close(listen_fd);
    signal(SIGTERM, SIG_DFL);
    signal(SIGINT, SIG_DFL);
    (void)prctl(PR_SET_PDEATHSIG, SIGTERM);
    if (getppid() != parent) _exit(0);
    run_discovery();
}
```

In `main`, after `log_message("%s listening on 0.0.0.0:%u", SERVER_NAME, port);`:

```cpp
#if APCAM_HAVE_4KCAM_COMPAT
    start_discovery(listen_fd);
#endif
```

- [ ] **Step 4: Build, deploy and run the full check**

Run the cross-build, the **RAM deploy**, then `python3 web/tests/check_4kcam_compat.py --camsh /Users/will/git/4kcamera_works/tools/camsh.py`
Expected: every line `PASS`, `0 failure(s)`. Then check that the child dies with its parent and comes back with the respawned web (3702 is `0E76` in `/proc/net/udp`): `python3 /Users/will/git/4kcamera_works/tools/camsh.py -t 20 'grep -c ":0E76 " /proc/net/udp; kill $(pidof hisi-web); sleep 0.5; grep -c ":0E76 " /proc/net/udp; sleep 3; grep -c ":0E76 " /proc/net/udp'`. Expected: `1`, `0`, `1`. Then rerun the discovery part of the check (`python3 web/tests/check_4kcam_compat.py --camsh ...`) and expect `0 failure(s)`.

- [ ] **Step 5: Commit**

```bash
git add web/mt11-web.cpp web/tests/check_4kcam_compat.py
git commit -m "hisi-web: WS-Discovery, so MCM finds the camera as the vendor's"
```

---

### Task 8: Full verification

**Files:** none changed unless a check fails.

- [ ] **Step 1: Host unit tests on the Mac**

Run: `make -C web tests/test_onvif tests/test_fourkcam tests/test_string_buffer && (cd web && ./tests/test_onvif && ./tests/test_fourkcam && ./tests/test_string_buffer)`
Expected: `test_onvif: ok`, `test_fourkcam: ok`, and no assertion failures.

- [ ] **Step 2: The web test suite on Linux (other targets, flag 0)**

Run: `rsync -a --delete --exclude .git --exclude 'build/' --exclude 'web/tests/onvif-check/target' ./ will@192.168.15.108:apcam-hisi-next/ && ssh will@192.168.15.108 'cd apcam-hisi-next && make -C web test 2>&1 | tail -15'` (without `.git` the build hash is `unknown`, which is fine here)
Expected: the suite ends without errors, including `./tests/test_onvif` and `./tests/test_fourkcam`. If it stops on a step that also fails at `67fa381` (check by rerunning that step on a checkout of `67fa381`), record it and continue.

- [ ] **Step 3: Device check and schema check of the camera's own replies**

Run: `python3 web/tests/check_4kcam_compat.py --camsh /Users/will/git/4kcamera_works/tools/camsh.py && web/tests/onvif-check/run.sh will@192.168.15.108 check web/tests/onvif-check/replies`
Expected: `0 failure(s)`, then all six kinds print values, with `source=HDIPC - RadCam (IPCamera)`, both `xaddrs` and `services` at `http://192.168.2.10/onvif/device_service`, and `stream-uri` `rtsp://192.168.2.10:8554/video1`.

- [ ] **Step 4: The camera is back as it was**

Run: `python3 /Users/will/git/4kcamera_works/tools/camsh.py -t 20 'cmp /run/4kcam-check.ini /date/ap_cameragimbal/camera.ini && echo restored; pidof camera-app'`
Expected: `restored` and a PID. The RAM build of hisi-web stays until a reboot. Tell the other agent's owner (the user) that it's bind-mounted.

---

### Task 9: Documentation

**Files:**
- Modify: `docs/4kcam-manager-compat.md`
- Modify: `web/README.md`

- [ ] **Step 1: Update the spec to what was built**

In `docs/4kcam-manager-compat.md`:
- In **Discovery**, replace the first sentence with: "A child process of `hisi-web` joins `239.255.255.250:3702` on every IPv4 interface and answers `Probe` messages with a `ProbeMatch`, sent back to the prober. It exits with its parent (`PR_SET_PDEATHSIG`), so the supervisor's restart of `hisi-web` restarts it too. It reads **Allow 4K Cam Manager** for each probe, since saving the setting doesn't restart the web."
- In the match's list, replace "`XAddrs` `http://<eth0 address>/onvif/device_service`" with "`XAddrs` `http://<address>/onvif/device_service`, the camera's address on the route to the prober (ONVIF replies use the address the request came in on, since MCM requires service addresses within the one it discovered)".
- In **Settings**, replace "the discovery process doesn't run" with "discovery doesn't answer".
- Replace the **Testing** section with:

```markdown
## Testing

- **Unit tests** (host): `web/tests/test_onvif.cpp` and `web/tests/test_fourkcam.cpp`, in `make -C web test`.
- **Schemas**: `web/tests/onvif-check` builds onvif-rs at MCM's revision on a Linux host and parses replies the
  way MCM does. `run.sh HOST requests` regenerates the request fixtures; `run.sh HOST check DIR` checks saved replies.
- **On the camera** (hisi-web bind-mounted in RAM): `web/tests/check_4kcam_compat.py` probes WS-Discovery, runs
  each ONVIF operation with a correct digest, a wrong one and none, reads and writes each `/action` (checking the
  extension's types), runs `onceAWB` (manual gains within 10 s), restarts camera-app, and turns the setting off
  (404, no probe reply). It backs up and restores `camera.ini`.
- **End to end**: BlueOS on a Raspberry Pi on the camera's LAN, with MCM and 4K Cam Manager installed. Success is
  the camera in the extension's list, its stream in MCM, and the widget's controls and one-push changing the camera.
  (Docker on the Mac was the plan, but Docker isn't installed there.)
```

- [ ] **Step 2: Document it for users**

Append to `web/README.md`:

```markdown
## 4K Cam Manager (Hi3516DV500)

On the Hi3516DV500 camera, the web server also answers Blue Robotics'
[4K Cam Manager](https://github.com/bluerobotics/br4kcam-manager) BlueOS extension and MAVLink Camera Manager as
the original firmware does: WS-Discovery, ONVIF (`POST /onvif/device_service`, user `admin`) and the
`POST /action/<name>` control protocol. Both routes skip the web login, so the extension works unchanged.
**Allow 4K Cam Manager** (`[compat] fourkcam_manager`) turns it all off; **ONVIF password**
(`[compat] onvif_password`, default `blue`, which the extension uses) and **One-push water** (`[image] wb_water`)
are on the parameters page. Design and mapping: `docs/4kcam-manager-compat.md`.
```

- [ ] **Step 3: Commit**

```bash
git add docs/4kcam-manager-compat.md web/README.md
git commit -m "docs: 4K Cam Manager compatibility as built and tested"
```

---

### Task 10: End to end with BlueOS (once the Pi is on the camera's LAN)

**Files:** none, unless it finds a bug (then add a failing unit or device check first, fix, and commit).

- [ ] **Step 1: Find the Pi and check it sees the camera**

The user plugs in the Pi and gives its address (BlueOS's default is `blueos.local`). Run: `ssh pi@<pi> 'ping -c1 192.168.2.10'`.
Expected: one reply. If the Pi isn't on `192.168.2.0/24`, ask the user how it's connected before going further.

- [ ] **Step 2: Install the extension and confirm discovery**

In BlueOS (`http://<pi>`), open Extensions, install **4K Cam Manager**, and open MAVLink Camera Manager's page (`http://<pi>:6020`).
Expected: a video source named `HDIPC - RadCam (IPCamera)`, with streams for `rtsp://192.168.2.10:8554/video1` and `/video2`.

- [ ] **Step 3: Drive the widget**

In the extension's page: the camera is listed. Change brightness, Manual white balance and red/blue sliders, Green/Blue water, then One-push. Change the main stream's bitrate, then **Save and restart camera**. After each, read the camera back: `curl -s -X POST -d '' http://192.168.2.10/action/getImageAdjustment` (from the Pi), and for streams `getVencConf` with `{"channel":0}`.
Expected: each change appears in the reply. One-push leaves `auto_awb` 1 with new gains, and the stream keeps playing in MCM after the restart.

- [ ] **Step 4: Cockpit action**

In Cockpit (BlueOS), add the extension's **4K Cam One-Push White Balance** action to a joystick button and press it.
Expected: `camera-app` logs a one-push, and `getImageAdjustment` shows new manual gains.

- [ ] **Step 5: Restore and report**

Put the camera's settings back the way they were before Step 3, either with the web parameters page or with `cp /run/4kcam-check.ini /date/ap_cameragimbal/camera.ini` if that backup still matches. Then report the results to the user. Ask before any push.
