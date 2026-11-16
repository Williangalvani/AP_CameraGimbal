#!/usr/bin/env python3
"""Play one MCM stream over WebRTC in headless Chrome and report receiver stats.

Usage: webrtc_stats.py [--camera IP] [--stream NAME] [--seconds N]
Needs google-chrome-stable and the websockets module. Drives MCM's own
/webrtc page, so it exercises the same signalling as the frontend.
"""
import argparse
import asyncio
import json
import os
import shutil
import subprocess
import tempfile
import time
import urllib.request

import websockets

SETUP = """
(async () => {
  const sleep = ms => new Promise(r => setTimeout(r, ms));
  for (let i = 0; i < 50 && !document.getElementById('add-consumer'); i++) await sleep(100);
  document.getElementById('add-consumer').click();
  for (let i = 0; i < 100; i++) {
    const s = [...document.querySelectorAll('.stream')]
      .find(e => e.innerText.includes('"%(stream)s"'));
    if (s) { s.querySelector('#add-session').click(); return 'session added'; }
    await sleep(100);
  }
  return 'stream not found: ' + document.body.innerText.slice(0, 400);
})()
"""

STATS = """
(async () => {
  const pcs = window.__mcm_peer_connections || [];
  const pc = pcs[pcs.length - 1];
  if (!pc) return null;
  const out = {state: pc.connectionState};
  (await pc.getStats()).forEach(r => {
    if (r.type === 'inbound-rtp' && r.kind === 'video') Object.assign(out, {
      frameWidth: r.frameWidth, frameHeight: r.frameHeight,
      framesDecoded: r.framesDecoded, framesDropped: r.framesDropped,
      keyFramesDecoded: r.keyFramesDecoded,
      freezeCount: r.freezeCount, totalFreezesDuration: r.totalFreezesDuration,
      packetsReceived: r.packetsReceived, packetsLost: r.packetsLost,
      retransmittedPacketsReceived: r.retransmittedPacketsReceived,
      nackCount: r.nackCount, pliCount: r.pliCount, firCount: r.firCount,
      bytesReceived: r.bytesReceived,
      jitterBufferDelay: r.jitterBufferDelay,
      jitterBufferEmittedCount: r.jitterBufferEmittedCount,
      totalDecodeTime: r.totalDecodeTime, decoder: r.decoderImplementation,
    });
  });
  return out;
})()
"""


class Cdp:
    def __init__(self, ws):
        self.ws = ws
        self.next_id = 0

    async def call(self, method, **params):
        self.next_id += 1
        mine = self.next_id
        await self.ws.send(json.dumps({"id": mine, "method": method, "params": params}))
        while True:
            message = json.loads(await self.ws.recv())
            if message.get("id") == mine:
                if "error" in message:
                    raise RuntimeError(message["error"])
                return message["result"]

    async def evaluate(self, expression):
        result = await self.call("Runtime.evaluate", expression=expression,
                                 awaitPromise=True, returnByValue=True)
        return result["result"].get("value")


def delta(a, b, key):
    return (b.get(key) or 0) - (a.get(key) or 0)


async def run(args, port):
    for _ in range(50):
        try:
            with urllib.request.urlopen(f"http://127.0.0.1:{port}/json") as r:
                pages = [p for p in json.load(r) if p["type"] == "page"]
            if pages:
                break
        except OSError:
            pass
        time.sleep(0.2)
    async with websockets.connect(pages[0]["webSocketDebuggerUrl"], max_size=None) as ws:
        cdp = Cdp(ws)
        await cdp.call("Page.enable")
        await cdp.call("Page.navigate", url=f"http://{args.camera}:6020/webrtc")
        await asyncio.sleep(2)
        print(await cdp.evaluate(SETUP % {"stream": args.stream}), flush=True)
        added = time.monotonic()

        first = None
        state = None
        for _ in range(int(args.warmup * 20)):
            first = await cdp.evaluate(STATS)
            if first and first.get("state") != state:
                state = first.get("state")
                print(f"{time.monotonic() - added:.2f} s: connection {state}", flush=True)
            if first and first.get("framesDecoded"):
                break
            await asyncio.sleep(0.05)
        if not first or not first.get("framesDecoded"):
            print("no frames decoded:", first)
            return 1
        print(f"first frame {time.monotonic() - added:.2f} s after the session was added", flush=True)
        start = time.monotonic()
        previous = first
        for _ in range(args.seconds):
            await asyncio.sleep(1)
            now = await cdp.evaluate(STATS)
            print(f"t={time.monotonic() - start:4.1f} fps={delta(previous, now, 'framesDecoded'):3d} "
                  f"freezes={now.get('freezeCount')} lost={now.get('packetsLost')} "
                  f"nack={now.get('nackCount')} pli={now.get('pliCount')} "
                  f"kbit/s={delta(previous, now, 'bytesReceived') * 8 // 1000}", flush=True)
            previous = now
        elapsed = time.monotonic() - start
        last = previous
        emitted = delta(first, last, "jitterBufferEmittedCount")
        decoded = delta(first, last, "framesDecoded")
        summary = {
            "resolution": f"{last.get('frameWidth')}x{last.get('frameHeight')}",
            "fps": round(decoded / elapsed, 2),
            "freezes": delta(first, last, "freezeCount"),
            "freeze_seconds": round(delta(first, last, "totalFreezesDuration"), 3),
            "packets_lost": delta(first, last, "packetsLost"),
            "retransmitted": delta(first, last, "retransmittedPacketsReceived"),
            "nack": delta(first, last, "nackCount"),
            "pli": delta(first, last, "pliCount"),
            "keyframes": delta(first, last, "keyFramesDecoded"),
            "frames_dropped": delta(first, last, "framesDropped"),
            "mbit_s": round(delta(first, last, "bytesReceived") * 8 / elapsed / 1e6, 2),
            "jitter_buffer_ms": round(1000 * delta(first, last, "jitterBufferDelay") / emitted, 1)
            if emitted else None,
            "decode_ms": round(1000 * delta(first, last, "totalDecodeTime") / decoded, 2)
            if decoded else None,
            "decoder": last.get("decoder"),
        }
        print("summary", json.dumps(summary))
        return 0


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--camera", default="192.168.2.10")
    parser.add_argument("--stream", default="cam_main")
    parser.add_argument("--seconds", type=int, default=30)
    parser.add_argument("--warmup", type=float, default=15)
    parser.add_argument("--port", type=int, default=9333)
    args = parser.parse_args()
    profile = tempfile.mkdtemp(prefix="webrtc-stats-")
    chrome = subprocess.Popen(
        [shutil.which("google-chrome-stable") or "google-chrome", "--headless=new",
         f"--remote-debugging-port={args.port}", f"--user-data-dir={profile}",
         "--autoplay-policy=no-user-gesture-required", "--no-first-run", "about:blank"],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        return asyncio.run(run(args, args.port))
    finally:
        chrome.terminate()
        chrome.wait()
        shutil.rmtree(profile, ignore_errors=True)


if __name__ == "__main__":
    raise SystemExit(main())
