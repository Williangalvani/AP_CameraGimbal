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
